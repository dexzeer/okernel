#ifndef MEDIA_MENG_H
#define MEDIA_MENG_H

#include <stddef.h>
#include <stdint.h>

// Media engine: the playback core behind <video>/<audio>, MediaSource and the `play` command.
//
// Compressed data comes in through source buffers (WebM byte streams, appended in pieces of any
// size: an init segment, then clusters); the engine demuxes it into one Opus and one VP9 packet
// queue, decodes audio ahead into the sound card and video frames when their time comes, keeps a
// media clock (the audio clock while sound is playing, the 100 Hz tick otherwise) and renders
// the current video frame as ARGB at a size the embedder chooses. Everything outside the engine
// (sound card, tick counter) is reached through `struct meng_env`, so the same code runs in the
// kernel and in host tests with a simulated sound card.

struct meng;

struct meng_env {
    int (*audio_ready)(void);
    uint32_t (*audio_queued)(void);                                     // frames queued, not yet played
    uint32_t (*audio_write)(const int16_t* stereo, uint32_t frames);    // returns frames accepted
    uint64_t (*audio_clock)(void);                                      // frames played since start
    void (*audio_flush)(void);
    uint32_t (*ticks)(void);                                            // 100 Hz
};

struct meng_frame {
    const uint32_t* px;         // ARGB, alpha 0xFF
    int w, h;                   // the output size set with meng_set_output_size
    int src_w, src_h;           // size of the decoded video
    uint32_t serial;            // increments with every new frame
};

enum { MENG_NOTHING = 0, MENG_METADATA = 1, MENG_CURRENT = 2, MENG_FUTURE = 3, MENG_ENOUGH = 4 };
enum { MENG_ERR_NONE = 0, MENG_ERR_DECODE = 1, MENG_ERR_FORMAT = 2 };

struct meng* meng_new(const struct meng_env* env);
void meng_free(struct meng* m);

// ---- source buffers ----
// `kinds`: bit 0 = audio (Opus), bit 1 = video (VP9) may arrive through this buffer. Returns
// the buffer id or -1.
int  meng_sb_add(struct meng* m, int kinds);
// Feed bytes. Returns 0, or a negative value if the stream is not valid WebM (the buffer must
// be aborted/re-initialised before more data is accepted).
int  meng_sb_append(struct meng* m, int sb, const uint8_t* data, size_t len);
// Reset the parser (a new init segment follows); buffered packets stay.
void meng_sb_abort(struct meng* m, int sb);
// Drop buffered packets with start <= pts < end (seconds).
void meng_remove(struct meng* m, double start, double end);
// Bytes of compressed data held (for quota decisions).
size_t meng_bytes(const struct meng* m);
void meng_end_of_stream(struct meng* m);
void meng_set_duration(struct meng* m, double seconds);

// ---- playback ----
void meng_play(struct meng* m);
void meng_pause(struct meng* m);
void meng_seek(struct meng* m, double seconds);
void meng_set_volume(struct meng* m, double volume, int muted);
void meng_set_rate(struct meng* m, double rate);        // 0.25 .. 4 (audio keeps its pitch only at 1)
void meng_poll(struct meng* m);                         // call often from the main loop
void meng_set_governor(int on);                         // slow CPU: video gets a share of the CPU (cool-down after a long pass, skip to the newest due key frame when a second behind)

// ---- state ----
double meng_current_time(struct meng* m);
double meng_duration(const struct meng* m);             // NaN until known
int    meng_ready_state(const struct meng* m);
int    meng_paused(const struct meng* m);
int    meng_ended(const struct meng* m);
int    meng_seeking(const struct meng* m);
int    meng_error(const struct meng* m);
int    meng_has_audio(const struct meng* m);
int    meng_has_video(const struct meng* m);
// Buffered time ranges (those with data for every present track), in seconds.
int    meng_buffered(const struct meng* m, int index, double* start, double* end);
// The same for one source buffer: a video-only buffer reports the video ranges, an audio-only one the audio ranges.
int    meng_sb_buffered(const struct meng* m, int sb, int index, double* start, double* end);
// Counters for the embedder to turn into events.
uint32_t meng_frames_decoded(const struct meng* m);
uint32_t meng_frames_shown(const struct meng* m);
void meng_describe(const struct meng* m, char* out, int cap);   // one line of state, for logs
uint32_t meng_frames_dropped(const struct meng* m);
uint32_t meng_underruns(const struct meng* m);   // times the sound card ran dry while playing (the clock was re-based)
double meng_take_decode_ms(struct meng* m);   // average ms per decoded video frame since the last call
uint32_t meng_take_maxgap(struct meng* m);     // longest gap (ticks) between two presented frames since the last call
uint32_t meng_audio_frames(const struct meng* m);       // audio frames handed to the sound card so far

// ---- video output ----
void meng_set_output_size(struct meng* m, int w, int h);        // 0,0 = native size
const struct meng_frame* meng_get_frame(const struct meng* m);  // NULL until the first frame
int  meng_video_size(const struct meng* m, int* w, int* h);     // from the track header; 0 if no video known

#endif
