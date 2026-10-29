// okai on Windows -- the sound card, over waveOut.
//
// audio.h's contract is what the kernel's HDA driver provides: 48 kHz signed
// 16-bit stereo, a queue with a non-blocking write, and audio_clock() =
// frames actually played (the master clock for A/V sync in meng.c).
//
// waveOut is fed 8 x 250 ms buffers. The lifetime rules matter and are the
// reason this file is not a three-liner:
//   * a header is PREPARED ONCE (at init) and unprepared only at shutdown --
//     waveOutUnprepareHeader on a buffer the driver still owns corrupts
//     winmm/ntdll bookkeeping (it showed up as 0xC0000409 in ntdll, which no
//     handler can catch, minutes into a video);
//   * WOM_DONE (or the WHDR_DONE flag) is the only proof a buffer is ours
//     again -- after waveOutReset() the callbacks are asynchronous, so a flush
//     waits for DONE before handing the memory back to the decoder;
//   * the completion callback reads the buffer from WAVEHDR.dwUser.

#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio.h"
#include "win_plat.h"

#define NBUFS      8
#define BUF_FRAMES 12000           // 250 ms
#define MAX_FRAMES (NBUFS * BUF_FRAMES)

struct abuf {
    WAVEHDR hdr;
    int16_t pcm[BUF_FRAMES * 2];
    int frames;
    int busy;
};

static HWAVEOUT hwo;
static struct abuf bufs[NBUFS];
static int ready;
static uint64_t clock_base;
static uint64_t frames_written;
static uint64_t last_clock;
static CRITICAL_SECTION alock;
static int lock_init;

static void CALLBACK wom_done(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
    (void)h; (void)inst; (void)p2;
    if (msg != WOM_DONE) return;
    if (!p1 || !ready) return;        // p1 = WAVEHDR.dwUser
    struct abuf* b = (struct abuf*)p1;
    if (lock_init) EnterCriticalSection(&alock);
    b->busy = 0;
    b->frames = 0;
    if (lock_init) LeaveCriticalSection(&alock);
}

// Wait until every in-flight buffer has been returned by the driver.
static void wait_done(DWORD timeout_ms) {
    DWORD t0 = GetTickCount();
    for (;;) {
        int pending = 0;
        EnterCriticalSection(&alock);
        for (int i = 0; i < NBUFS; i++)
            if (bufs[i].busy && !(bufs[i].hdr.dwFlags & WHDR_DONE)) pending++;
        LeaveCriticalSection(&alock);
        if (!pending) return;
        if (GetTickCount() - t0 >= timeout_ms) return;
        Sleep(1);
    }
}

int audio_init(void) {
    if (ready) return 0;
    if (!lock_init) { InitializeCriticalSection(&alock); lock_init = 1; }
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    MMRESULT mr = waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)wom_done, 0, CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) {
        win_logf("[okai-win] waveOutOpen failed (%u): playing without sound\n", (unsigned)mr);
        return -1;
    }
    ready = 1;
    for (int i = 0; i < NBUFS; i++) {
        memset(&bufs[i], 0, sizeof bufs[i]);
        bufs[i].hdr.lpData = (LPSTR)bufs[i].pcm;
        bufs[i].hdr.dwBufferLength = BUF_FRAMES * 4;    // prepared at full size
        bufs[i].hdr.dwUser = (DWORD_PTR)&bufs[i];
        if (waveOutPrepareHeader(hwo, &bufs[i].hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            win_logf("[okai-win] waveOutPrepareHeader %d failed\n", i);
            ready = 0;
            return -1;
        }
        bufs[i].hdr.dwFlags |= WHDR_DONE;              // ours until written
    }
    waveOutSetVolume(hwo, 0xFFFFFFFF);
    win_logf("[okai-win] audio: waveOut 48000 Hz 16-bit stereo, %d x %d ms buffers\n",
             NBUFS, BUF_FRAMES * 1000 / AUDIO_RATE);
    return 0;
}

int audio_ready(void) { return ready; }

static uint64_t frame_clock(void) {
    if (!hwo) return 0;
    MMTIME t;
    memset(&t, 0, sizeof t);
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(hwo, &t, sizeof t) == MMSYSERR_NOERROR && t.wType == TIME_SAMPLES)
        return clock_base + t.u.sample;
    return last_clock;
}

uint64_t audio_clock(void) {
    uint64_t c = frame_clock();
    if (c < last_clock) c = last_clock;     // never step back
    last_clock = c;
    return c;
}

uint32_t audio_space(void) {
    if (!ready) return 0;
    int free_slots = 0;
    EnterCriticalSection(&alock);
    for (int i = 0; i < NBUFS; i++) if (!bufs[i].busy) free_slots++;
    LeaveCriticalSection(&alock);
    return (uint32_t)free_slots * BUF_FRAMES;
}

uint32_t audio_queued(void) {
    if (!ready) return 0;
    uint32_t q = 0;
    EnterCriticalSection(&alock);
    for (int i = 0; i < NBUFS; i++) if (bufs[i].busy) q += (uint32_t)bufs[i].frames;
    LeaveCriticalSection(&alock);
    return q;
}

uint32_t audio_write(const int16_t* frames, uint32_t nframes) {
    if (!ready || !frames || !nframes) return 0;
    static int first_write = 1;
    if (first_write) {
        first_write = 0;
        win_logf("[okai-win] audio: first write, %u frames queued\n", (unsigned)nframes);
    }
    uint32_t done = 0;
    while (done < nframes) {
        int slot = -1;
        EnterCriticalSection(&alock);
        for (int i = 0; i < NBUFS; i++) {
            // A buffer is ours only when the driver is done with it.
            if (!bufs[i].busy && (bufs[i].hdr.dwFlags & WHDR_DONE)) { slot = i; bufs[i].busy = 1; break; }
        }
        LeaveCriticalSection(&alock);
        if (slot < 0) break;
        struct abuf* b = &bufs[slot];
        uint32_t n = nframes - done;
        if (n > BUF_FRAMES) n = BUF_FRAMES;
        memcpy(b->pcm, frames + (size_t)done * 2, (size_t)n * 4);
        b->frames = (int)n;
        b->hdr.dwBufferLength = n * 4;
        b->hdr.dwFlags &= ~WHDR_DONE;                  // the driver owns it again
        if (waveOutWrite(hwo, &b->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            EnterCriticalSection(&alock);
            b->busy = 0;
            b->frames = 0;
            b->hdr.dwFlags |= WHDR_DONE;
            LeaveCriticalSection(&alock);
            break;
        }
        frames_written += n;
        done += n;
    }
    return done;
}

void audio_flush(void) {
    if (!ready) return;
    uint64_t c = audio_clock();
    waveOutReset(hwo);
    wait_done(500);                     // reset is asynchronous: wait for DONE
    EnterCriticalSection(&alock);
    for (int i = 0; i < NBUFS; i++) {
        bufs[i].busy = 0;
        bufs[i].frames = 0;
        bufs[i].hdr.dwFlags |= WHDR_DONE;
    }
    LeaveCriticalSection(&alock);
    clock_base = c;
    last_clock = c;
}

void audio_tick(void) { (void)audio_clock(); }

void audio_rate_force(uint32_t percent) { (void)percent; }
void audio_trace(int cmd) { (void)cmd; }
uint32_t audio_rate_q16(void) { return 65536; }

void audio_set_volume(int percent) {
    if (!ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t v = (uint32_t)(percent * 65535 / 100);
    waveOutSetVolume(hwo, (v << 16) | v);
}

void audio_describe(void (*out)(const char* line, void* ud), void* ud) {
    out("waveOut, 48000 Hz, 16-bit stereo", ud);
}

void audio_beep(int hz, int ms) {
    if (!ready) return;
    int n = AUDIO_RATE * ms / 1000;
    int16_t* b = (int16_t*)malloc((size_t)n * 4);
    if (!b) return;
    for (int i = 0; i < n; i++) {
        int16_t s = (int16_t)((i * (hz % 1000) < 500) ? 4000 : -4000);
        b[i * 2] = s;
        b[i * 2 + 1] = s;
    }
    audio_write(b, (uint32_t)n);
    free(b);
}

void win_audio_shutdown(void) {
    if (!ready) return;
    audio_flush();
    wait_done(1000);
    ready = 0;                          // callbacks ignore everything from here
    if (lock_init) EnterCriticalSection(&alock);
    for (int i = 0; i < NBUFS; i++) {
        if (bufs[i].hdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(hwo, &bufs[i].hdr, sizeof(WAVEHDR));
        bufs[i].busy = 0;
    }
    if (lock_init) LeaveCriticalSection(&alock);
    waveOutClose(hwo);
    hwo = 0;
}
