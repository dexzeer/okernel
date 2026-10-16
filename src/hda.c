// Intel High Definition Audio controller + generic codec driver (output only).
// Written from the Intel HDA specification 1.0a. Polled: no interrupts are used;
// audio_tick() (timer IRQ) tracks the play position.
#include "audio.h"
#include "audio_rate.h"
#include "cpufreq.h"
#include "lapic.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "pcix.h"
#include "serial.h"
#include "string.h"

// ---- controller registers ------------------------------------------------------
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E

// stream descriptor registers (offsets from the descriptor base)
#define SD_CTL   0x00
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define RING_BYTES  (512u * 1024u)       // 2.7 s of 48 kHz stereo: a page script / layout that holds the main loop for a second or two must not run the card dry
#define BDL_ENTRIES 4u
#define STREAM_TAG  1

static struct {
    int ready;
    struct pci_loc loc;
    volatile uint8_t* mmio;
    int iss, oss;                 // input / output stream counts
    uint32_t sd;                  // offset of the output stream descriptor we use
    // CORB/RIRB
    uint32_t* corb; uint64_t* rirb;
    uint16_t corb_n, rirb_n, corb_wp, rirb_rp;
    // codec
    int cad;                      // codec address in use
    int afg;                      // audio function group node id
    int dac, pin;                 // converter and output pin node ids
    uint8_t path[8]; int pathlen; // pin ... dac
    // ring
    uint8_t* ring;                // RING_BYTES, 128-byte aligned
    void* ring_alloc; void* bdl_alloc; void* corb_alloc;
    uint64_t wr_abs;              // bytes handed to the ring since the stream started
    uint64_t clean_abs;           // everything before this (absolute byte position) has been zeroed after playing
    uint32_t last_lpib;
    uint64_t played_base;         // bytes played at the start of the current lap
    uint32_t underruns;
    int volume;
    struct arate rate;            // measured speed of the card against real time (audio_rate.h)
    uint32_t rate_logged;
} H;

static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t*)(H.mmio + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t*)(H.mmio + o); }
static inline uint8_t  rd8(uint32_t o)  { return *(volatile uint8_t*)(H.mmio + o); }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(H.mmio + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(H.mmio + o) = v; }
static inline void wr8(uint32_t o, uint8_t v)   { *(volatile uint8_t*)(H.mmio + o) = v; }

static int wait_bits(uint32_t off, int width, uint32_t mask, uint32_t want, int us) {
    for (int i = 0; i < us; i++) {
        uint32_t v = width == 32 ? rd32(off) : width == 16 ? rd16(off) : rd8(off);
        if ((v & mask) == want) return 1;
        pit_delay_us(1);
    }
    return 0;
}

// ---- CORB / RIRB ------------------------------------------------------------------

static void* aligned(void** raw, uint32_t bytes, uint32_t align) {
    uint8_t* p = (uint8_t*)kcalloc(1, bytes + align);
    *raw = p;
    if (!p) return 0;
    return (void*)(((uint32_t)p + align - 1) & ~(align - 1));
}

static int corb_rirb_init(void) {
    wr8(CORBCTL, 0); wr8(RIRBCTL, 0);
    wait_bits(CORBCTL, 8, 2, 0, 1000); wait_bits(RIRBCTL, 8, 2, 0, 1000);
    // pick the largest size the controller offers
    uint8_t cs = rd8(CORBSIZE), rs = rd8(RIRBSIZE);
    H.corb_n = (cs & 0x40) ? 256 : (cs & 0x20) ? 16 : 2;
    H.rirb_n = (rs & 0x40) ? 256 : (rs & 0x20) ? 16 : 2;
    wr8(CORBSIZE, (cs & 0xFC) | (H.corb_n == 256 ? 2 : H.corb_n == 16 ? 1 : 0));
    wr8(RIRBSIZE, (rs & 0xFC) | (H.rirb_n == 256 ? 2 : H.rirb_n == 16 ? 1 : 0));
    // CORB (256 x 4 = 1KB) then RIRB (256 x 8 = 2KB) in one 128-byte aligned block
    uint8_t* mem = (uint8_t*)aligned(&H.corb_alloc, 3072 + 128, 128);
    if (!mem) return -1;
    H.corb = (uint32_t*)mem;
    H.rirb = (uint64_t*)(mem + 1024);
    wr32(CORBLBASE, V2P((uint32_t)H.corb)); wr32(CORBUBASE, 0);
    wr32(RIRBLBASE, V2P((uint32_t)H.rirb)); wr32(RIRBUBASE, 0);
    // CORB read pointer reset handshake
    wr16(CORBRP, 0x8000);
    if (!wait_bits(CORBRP, 16, 0x8000, 0x8000, 2000)) return -1;
    wr16(CORBRP, 0);
    wait_bits(CORBRP, 16, 0x8000, 0, 2000);
    wr16(CORBWP, 0);
    wr16(RIRBWP, 0x8000);                    // reset the write pointer
    wr16(RINTCNT, 255);                      // response-interrupt threshold; QEMU stalls the CORB at this count until RIRBSTS is cleared
    H.corb_wp = 0; H.rirb_rp = 0;
    wr8(RIRBCTL, 0x02);                      // RIRB DMA run, no interrupts
    wr8(CORBCTL, 0x02);                      // CORB DMA run
    if (!wait_bits(CORBCTL, 8, 2, 2, 2000)) return -1;
    return 0;
}

// Send one verb, wait for its response. Returns 0 on timeout (a valid response
// can be 0 too; callers that care use hda_cmd_ok).
static int hda_cmd(uint32_t cmd, uint32_t* resp) {
    uint16_t wp = (uint16_t)((H.corb_wp + 1) % H.corb_n);
    H.corb[wp] = cmd;
    H.corb_wp = wp;
    wr16(CORBWP, wp);
    for (int i = 0; i < 20000; i++) {
        uint16_t w = rd16(RIRBWP) & 0xFF;
        if (w != H.rirb_rp) {
            H.rirb_rp = (uint16_t)((H.rirb_rp + 1) % H.rirb_n);
            uint64_t e = ((volatile uint64_t*)H.rirb)[H.rirb_rp];
            if (resp) *resp = (uint32_t)e;
            wr8(RIRBSTS, 0x05);
            return 1;
        }
        pit_delay_us(5);
    }
    serial_printf("[hda] command %x timed out (CORB rp %x wp %x, RIRB wp %x)\n", cmd, rd16(CORBRP), rd16(CORBWP), rd16(RIRBWP));
    return 0;
}

static uint32_t verb12(int nid, uint32_t verb, uint32_t payload) {
    return ((uint32_t)H.cad << 28) | ((uint32_t)nid << 20) | (verb << 8) | (payload & 0xFF);
}
static uint32_t verb4(int nid, uint32_t verb, uint32_t payload) {
    return ((uint32_t)H.cad << 28) | ((uint32_t)nid << 20) | (verb << 16) | (payload & 0xFFFF);
}
static uint32_t get_param(int nid, uint32_t param) {
    uint32_t r = 0;
    hda_cmd(verb12(nid, 0xF00, param), &r);
    return r;
}

// ---- codec ---------------------------------------------------------------------------

#define W_OUT 0x0
#define W_MIX 0x2
#define W_SEL 0x3
#define W_PIN 0x4

static int node_type(int nid) { return (int)((get_param(nid, 0x09) >> 20) & 0xF); }

// connection list of a node: fills out[] with node ids, returns the count
static int conn_list(int nid, uint8_t* out, int max) {
    uint32_t len = get_param(nid, 0x0E);
    int n = (int)(len & 0x7F), lng = (len >> 7) & 1, cnt = 0, prev = -1;
    for (int i = 0; i < n && cnt < max; ) {
        uint32_t r = 0;
        hda_cmd(verb12(nid, 0xF02, (uint32_t)i), &r);
        int per = lng ? 2 : 4;
        for (int k = 0; k < per && i < n && cnt < max; k++, i++) {
            uint32_t e = lng ? ((r >> (16 * k)) & 0xFFFF) : ((r >> (8 * k)) & 0xFF);
            int range = lng ? (e & 0x8000) : (e & 0x80);
            int id = (int)(lng ? (e & 0x7FFF) : (e & 0x7F));
            if (range && prev >= 0) { for (int x = prev + 1; x <= id && cnt < max; x++) out[cnt++] = (uint8_t)x; }
            else out[cnt++] = (uint8_t)id;
            prev = id;
        }
    }
    return cnt;
}

// depth-first search from `nid` toward an output converter; fills path[]
static int find_dac(int nid, int depth, uint8_t* path, int* plen) {
    if (depth > 6) return 0;
    path[(*plen)++] = (uint8_t)nid;
    int t = node_type(nid);
    if (t == W_OUT) return 1;
    if (t == W_PIN || t == W_MIX || t == W_SEL) {
        uint8_t c[16];
        int n = conn_list(nid, c, 16);
        for (int i = 0; i < n; i++) {
            int save = *plen;
            if (find_dac(c[i], depth + 1, path, plen)) return 1;
            *plen = save;
        }
    }
    (*plen)--;
    return 0;
}

static void amp_unmute(int nid, int is_pin_or_out, int input_index) {
    uint32_t caps = get_param(nid, 0x12);
    uint32_t cap_w = get_param(nid, 0x09);
    uint32_t gain = caps & 0x7F;                         // offset = the 0 dB step
    if (is_pin_or_out || (cap_w & 4)) {
        if (cap_w & 4) hda_cmd(verb4(nid, 0x3, 0xB000 | gain), 0);   // output amp, both channels, unmuted
    }
    if (input_index >= 0 && (cap_w & 2)) {
        uint32_t icaps = get_param(nid, 0x0D);
        uint32_t ig = icaps & 0x7F;
        hda_cmd(verb4(nid, 0x3, 0x7000 | ((uint32_t)input_index << 8) | ig), 0);   // input amp
    }
}

static int codec_setup(void) {
    uint32_t r = 0;
    // root: vendor, then function groups
    uint32_t vendor = get_param(0, 0x00);
    uint32_t sub = get_param(0, 0x04);
    int first = (int)((sub >> 16) & 0xFF), cnt = (int)(sub & 0xFF);
    serial_printf("[hda] codec %d vendor %x, %d function group(s) from node %d\n", H.cad, vendor, cnt, first);
    H.afg = -1;
    for (int i = 0; i < cnt; i++) {
        uint32_t t = get_param(first + i, 0x05);
        if ((t & 0xFF) == 1) { H.afg = first + i; break; }
    }
    if (H.afg < 0) { serial_puts("[hda] no audio function group\n"); return -1; }
    hda_cmd(verb12(H.afg, 0x705, 0), &r);                // D0
    pit_delay_us(10000);
    uint32_t wsub = get_param(H.afg, 0x04);
    int wfirst = (int)((wsub >> 16) & 0xFF), wcnt = (int)(wsub & 0xFF);

    // choose an output pin: speaker, then line out, then headphones
    static const int pref[3] = { 1, 0, 2 };
    int best = -1, best_rank = 99;
    for (int w = wfirst; w < wfirst + wcnt; w++) {
        if (node_type(w) != W_PIN) continue;
        uint32_t pcaps = get_param(w, 0x0C);
        if (!(pcaps & 0x10)) continue;                    // not output capable
        uint32_t cfg = 0;
        hda_cmd(verb12(w, 0xF1C, 0), &cfg);
        int conn = (int)(cfg >> 30), dev = (int)((cfg >> 20) & 0xF);
        if (conn == 1) continue;                          // no physical connection
        int rank = 99;
        for (int k = 0; k < 3; k++) if (dev == pref[k]) rank = k;
        if (rank < best_rank) { best_rank = rank; best = w; }
    }
    if (best < 0) { serial_puts("[hda] no usable output pin\n"); return -1; }
    H.pin = best;
    H.pathlen = 0;
    uint8_t path[8]; int plen = 0;
    if (!find_dac(best, 0, path, &plen)) { serial_printf("[hda] no path from pin %d to a converter\n", best); return -1; }
    H.pathlen = plen;
    for (int i = 0; i < plen; i++) H.path[i] = path[i];
    H.dac = path[plen - 1];
    serial_printf("[hda] output path: pin %d", H.pin);
    for (int i = 1; i < plen; i++) serial_printf(" -> %d", path[i]);
    serial_puts("\n");

    // program the path: power, connection select, amplifiers
    for (int i = 0; i < plen; i++) {
        int nid = path[i];
        hda_cmd(verb12(nid, 0x705, 0), &r);
        if (i + 1 < plen) {                                // select the connection toward the next node
            uint8_t c[16];
            int n = conn_list(nid, c, 16), idx = 0;
            for (int k = 0; k < n; k++) if (c[k] == path[i + 1]) idx = k;
            if (n > 1) hda_cmd(verb12(nid, 0x701, (uint32_t)idx), &r);
            amp_unmute(nid, 1, idx);
        } else {
            amp_unmute(nid, 1, -1);
        }
    }
    hda_cmd(verb12(H.pin, 0x707, 0x40), &r);               // pin: output enable
    if (get_param(H.pin, 0x0C) & 0x10000) hda_cmd(verb12(H.pin, 0x70C, 0x02), &r);   // EAPD on
    // converter: stream tag/channel 0, format 48 kHz 16 bit stereo
    hda_cmd(verb12(H.dac, 0x706, (STREAM_TAG << 4) | 0), &r);
    hda_cmd(verb4(H.dac, 0x2, 0x0011), &r);
    return 0;
}

// ---- stream ----------------------------------------------------------------------------

static int stream_setup(void) {
    H.sd = 0x80 + 0x20 * (uint32_t)(H.iss + 0);
    uint32_t b = H.sd;
    // reset the descriptor
    wr32(b + SD_CTL, rd32(b + SD_CTL) | 1);
    wait_bits(b + SD_CTL, 32, 1, 1, 2000);
    wr32(b + SD_CTL, rd32(b + SD_CTL) & ~1u);
    wait_bits(b + SD_CTL, 32, 1, 0, 2000);

    H.ring = (uint8_t*)aligned(&H.ring_alloc, RING_BYTES, 128);
    uint8_t* bdl = (uint8_t*)aligned(&H.bdl_alloc, BDL_ENTRIES * 16, 128);
    if (!H.ring || !bdl) return -1;
    for (uint32_t i = 0; i < BDL_ENTRIES; i++) {
        uint32_t* e = (uint32_t*)(bdl + i * 16);
        e[0] = V2P((uint32_t)H.ring) + i * (RING_BYTES / BDL_ENTRIES);
        e[1] = 0;
        e[2] = RING_BYTES / BDL_ENTRIES;
        e[3] = 0;                                        // no interrupt on completion
    }
    wr32(b + SD_BDPL, V2P((uint32_t)bdl));
    wr32(b + SD_BDPU, 0);
    wr32(b + SD_CBL, RING_BYTES);
    wr16(b + SD_LVI, (uint16_t)(BDL_ENTRIES - 1));
    wr16(b + SD_FMT, 0x0011);                            // 48 kHz, 16 bit, 2 channels
    // stream number (tag) in CTL bits 23:20, then run
    uint32_t ctl = rd32(b + SD_CTL) & ~0x00F0001Fu;
    wr32(b + SD_CTL, ctl | ((uint32_t)STREAM_TAG << 20));
    wr32(b + SD_CTL, (ctl | ((uint32_t)STREAM_TAG << 20)) | 2);     // RUN
    H.wr_abs = 0; H.clean_abs = 0; H.last_lpib = 0; H.played_base = 0; H.underruns = 0;
    arate_init(&H.rate);
    H.rate_logged = H.rate.rate_q16;
    return 0;
}

// ---- public API -----------------------------------------------------------------------------

int audio_ready(void) { return H.ready; }

int audio_init(void) {
    struct pci_loc loc;
    if (!pcix_find_class(0x04, 0x03, 0, &loc)) { serial_puts("[hda] no HD Audio controller\n"); return -1; }
    H.loc = loc;
    pcix_enable(loc, 1);
    int is_mem, above;
    uint32_t bar = pcix_bar(loc, 0, &is_mem, &above);
    if (!bar || !is_mem) { serial_puts("[hda] controller BAR0 unusable\n"); return -1; }
    H.mmio = (volatile uint8_t*)paging_map_mmio(bar, 16384);
    if (!H.mmio) { serial_puts("[hda] out of MMIO windows\n"); return -1; }
    uint32_t id = pcix_read32(loc, 0);
    uint16_t gcap = rd16(GCAP);
    H.oss = (gcap >> 12) & 0xF; H.iss = (gcap >> 8) & 0xF;
    serial_printf("[hda] controller %x:%x at %d:%d.%d, %d output / %d input streams, v%d.%d\n",
                  id & 0xFFFF, id >> 16, loc.bus, loc.dev, loc.fn, H.oss, H.iss, rd8(0x03), rd8(0x02));
    if (H.oss < 1) { serial_puts("[hda] no output stream\n"); return -1; }

    // controller reset
    wr32(GCTL, rd32(GCTL) & ~1u);
    if (!wait_bits(GCTL, 32, 1, 0, 5000)) { serial_puts("[hda] reset stuck\n"); return -1; }
    pit_delay_us(200);
    wr32(GCTL, rd32(GCTL) | 1u);
    if (!wait_bits(GCTL, 32, 1, 1, 5000)) { serial_puts("[hda] controller did not leave reset\n"); return -1; }
    pit_delay_us(2000);                                  // codecs announce themselves (>= 521 us)
    uint16_t st = rd16(STATESTS);
    if (!st) { serial_puts("[hda] no codec present\n"); return -1; }
    wr16(INTCTL, 0);                                     // polled
    if (corb_rirb_init() != 0) { serial_puts("[hda] command ring setup failed\n"); return -1; }

    for (int c = 0; c < 15; c++) {
        if (!(st & (1 << c))) continue;
        H.cad = c;
        if (codec_setup() == 0) break;
        H.dac = 0;
    }
    if (!H.dac) return -1;
    if (stream_setup() != 0) { serial_puts("[hda] stream setup failed\n"); return -1; }
    H.volume = 100;
    H.ready = 1;
    serial_puts("[hda] audio output running (48 kHz, 16 bit, stereo)\n");
    return 0;
}

// Hardware read position within the ring, with a sanity clamp.
static uint32_t lpib(void) { uint32_t p = rd32(H.sd + SD_LPIB); return p >= RING_BYTES ? 0 : p; }

// Called from the timer IRQ: lap counting (lpib wraps) and wiping what has been
// played, so an underrun plays silence instead of looping stale audio. Only
// positions the writer cannot have reused are wiped: an absolute position `a`
// shares its ring slot with `a + RING_BYTES`, which holds queued data whenever
// the writer is that far ahead.
void audio_tick(void) {
    if (!H.ready) return;
    uint32_t p = lpib();
    if (p < H.last_lpib) H.played_base += RING_BYTES;    // wrapped
    H.last_lpib = p;
    uint64_t pl = H.played_base + p;
    // Wipe the absolute range [lo, pl). A position `a` shares its ring slot with
    // a + RING_BYTES; when the writer has reached that far (wr_abs > a + RING) the
    // slot holds queued audio, so wiping starts above wr_abs - RING_BYTES.
    uint64_t lo = H.clean_abs;
    if (H.wr_abs > RING_BYTES && H.wr_abs - RING_BYTES > lo) lo = H.wr_abs - RING_BYTES;
    if (pl > lo && pl - lo > RING_BYTES) lo = pl - RING_BYTES;          // the tick was starved for a lap
    while (lo < pl) {
        uint32_t off = (uint32_t)lo & (RING_BYTES - 1);
        uint64_t left = pl - lo;
        uint32_t chunk = RING_BYTES - off;
        if (chunk > left) chunk = (uint32_t)left;
        memset(H.ring + off, 0, chunk);
        lo += chunk;
    }
    if (pl > H.clean_abs) H.clean_abs = pl;
    // the card's speed against real time (see audio_rate.h): the TSC is the reference where it can be trusted
    {
        static uint32_t tsc_khz;
        static int tsc_ok = -1;
        if (tsc_ok < 0) { tsc_khz = cpufreq_tsc_mhz() * 1000u; tsc_ok = tsc_khz && cpufreq_tsc_reliable(); }
        if (tsc_ok) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            arate_update(&H.rate, pl >> 2, (((uint64_t)hi << 32) | lo) / tsc_khz);
            if (H.rate.rate_q16 != H.rate_logged) {
                H.rate_logged = H.rate.rate_q16;
                serial_printf("[hda] the card runs at %u.%u%u%u x real time%s\n", H.rate.rate_q16 >> 16, (H.rate.rate_q16 & 0xFFFF) * 10 >> 16,
                              (H.rate.rate_q16 & 0xFFFF) * 100 / 65536 % 10, (H.rate.rate_q16 & 0xFFFF) * 1000 / 65536 % 10, arate_active(&H.rate) ? " (sound is resampled to match)" : "");
            }
        }
    }
}

// wr_abs is 64-bit: the timer tick must not see it half updated.
static void set_wr(uint64_t v) {
    uint32_t fl;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl) :: "memory");
    H.wr_abs = v;
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

// Bytes the hardware has consumed since the stream started. Correct as long as a
// tick (or any call here) happens at least once per ring lap (683 ms).
static uint64_t played_abs(void) {
    uint32_t fl;                                         // the timer tick updates the lap state
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl) :: "memory");
    uint32_t p = lpib();
    uint64_t base = H.played_base;
    if (p < H.last_lpib) base += RING_BYTES;             // wrapped since the last tick
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    return base + p;
}

// Positions and queue lengths are reported in NOMINAL 48 kHz frames: a card that consumes 1.18x too fast is resampled by 1.18 on the way in
// and its position divided by 1.18 on the way out, so its users (the media engine) see an exact 48 kHz card.
uint64_t audio_clock(void) {
    if (!H.ready) return 0;
    uint32_t fl;                                         // audio_tick (timer IRQ) updates the rate state
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl) :: "memory");
    uint64_t dev = played_abs() >> 2;
    uint64_t nom = arate_nominal(&H.rate, dev);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    return nom;
}

// Distance the writer keeps ahead of the DMA read pointer after an underrun
// (10 ms): the hardware must never be reading the spot we are about to fill.
#define RESYNC_MARGIN 1920u

// Bytes queued and not yet played. An underrun (the reader overtook the writer)
// resynchronises the writer just ahead of the read pointer.
static uint32_t queued_bytes(void) {
    uint64_t pl = played_abs();
    if (H.wr_abs < pl + 256) {                           // empty, or about to be
        if (H.wr_abs < pl) H.underruns++;
        if (H.wr_abs < pl + 256) set_wr(pl + RESYNC_MARGIN);
        return RESYNC_MARGIN;                            // the margin is silence we "owe"
    }
    uint64_t q = H.wr_abs - pl;
    return q > RING_BYTES ? RING_BYTES : (uint32_t)q;
}

uint32_t audio_queued(void) {
    if (!H.ready) return 0;
    uint32_t q = queued_bytes();
    return q > RESYNC_MARGIN ? arate_to_nominal(&H.rate, (q - RESYNC_MARGIN) / 4) : 0;   // real audio only
}

// Drop everything queued (seek, pause): the writer restarts just ahead of the read pointer and
// the audio that was queued behind it is silenced before the hardware reaches it.
void audio_flush(void) {
    if (!H.ready) return;
    uint64_t pl = played_abs();
    uint64_t wr = H.wr_abs;
    set_wr(pl + RESYNC_MARGIN);
    arate_reset_stream(&H.rate);
    uint64_t lo = pl;
    if (wr > pl + RING_BYTES) wr = pl + RING_BYTES;
    while (lo < wr) {
        uint32_t off = (uint32_t)lo & (RING_BYTES - 1);
        uint32_t chunk = RING_BYTES - off;
        if (chunk > wr - lo) chunk = (uint32_t)(wr - lo);
        memset(H.ring + off, 0, chunk);
        lo += chunk;
    }
}

uint32_t audio_space(void) {
    if (!H.ready) return 0;
    uint32_t q = queued_bytes();
    return q + 64 >= RING_BYTES ? 0 : arate_fit(&H.rate, (RING_BYTES - 64 - q) / 4);
}

uint32_t audio_write(const int16_t* frames, uint32_t nframes) {
    if (!H.ready || !frames) return 0;
    uint32_t space = audio_space();
    if (nframes > space) nframes = space;
    if (arate_active(&H.rate)) {                         // the card is not 48 kHz: stretch what is written (audio_rate.h)
        static int16_t tmp[2 * 2048];
        uint32_t in_done = 0;
        while (in_done < nframes) {
            uint32_t c = nframes - in_done > 800 ? 800 : nframes - in_done;
            uint32_t m = arate_resample(&H.rate, frames + (size_t)in_done * 2, c, tmp, 2048);
            uint32_t b = m * 4, d = 0;
            while (d < b) {
                uint32_t off = (uint32_t)H.wr_abs & (RING_BYTES - 1);
                uint32_t chunk = RING_BYTES - off;
                if (chunk > b - d) chunk = b - d;
                memcpy(H.ring + off, (const uint8_t*)tmp + d, chunk);
                set_wr(H.wr_abs + chunk);
                d += chunk;
            }
            in_done += c;
        }
        return nframes;
    }
    uint32_t bytes = nframes * 4, done = 0;
    while (done < bytes) {
        uint32_t off = (uint32_t)H.wr_abs & (RING_BYTES - 1);
        uint32_t chunk = RING_BYTES - off;
        if (chunk > bytes - done) chunk = bytes - done;
        memcpy(H.ring + off, (const uint8_t*)frames + done, chunk);
        set_wr(H.wr_abs + chunk);
        done += chunk;
    }
    return nframes;
}

void audio_rate_force(uint32_t percent) { arate_force(&H.rate, percent ? (uint32_t)(((uint64_t)percent << 16) / 100) : 0); }
uint32_t audio_rate_q16(void) { return H.rate.rate_q16; }

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    H.volume = percent;
    if (!H.ready) return;
    uint32_t r;
    for (int i = 0; i < H.pathlen; i++) {
        int nid = H.path[i];
        uint32_t cw = get_param(nid, 0x09);
        if (!(cw & 4)) continue;
        uint32_t caps = get_param(nid, 0x12);
        uint32_t steps = (caps >> 8) & 0x7F, off = caps & 0x7F;
        if (!steps) continue;
        uint32_t g = (steps * (uint32_t)percent) / 100;
        if (g > steps) g = steps;
        uint32_t mute = percent == 0 ? 0x80 : 0;
        hda_cmd(verb4(nid, 0x3, 0xB000 | mute | (g > 0x7F ? 0x7F : g)), &r);
        (void)off;
    }
}

// ---- test tone and description ---------------------------------------------------------------------

// Integer sine, Q15, phase 0..65535 (Bhaskara's approximation: max error ~0.2%).
static int sine15(uint32_t phase) {
    int neg = phase & 0x8000;
    uint32_t x = phase & 0x7FFF;                         // 0..32767 over half a period
    uint32_t q = (x * (32768u - x)) >> 15;               // u(1-u) in Q15, max 8192
    uint32_t num = 16u * q;
    uint32_t den = 5u * 32768u - 4u * q;
    int v = (int)(((num << 14) / den) << 1);             // Q15
    return neg ? -v : v;
}

void audio_beep(int hz, int ms) {
    if (!H.ready) return;
    uint32_t total = (uint32_t)AUDIO_RATE * (uint32_t)ms / 1000u;
    uint32_t phase = 0, step = ((uint32_t)hz << 16) / AUDIO_RATE;
    int16_t buf[512];
    while (total) {
        uint32_t room = audio_space();
        if (!room) { pit_delay_us(2000); continue; }
        uint32_t n = total < 256 ? total : 256;
        if (n > room) n = room;
        for (uint32_t i = 0; i < n; i++) {
            int s = sine15(phase & 0xFFFF) / 4;           // -12 dB: comfortable
            buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
            phase += step;
        }
        uint32_t acc = audio_write(buf, n);
        total -= acc;
    }
}

void audio_describe(void (*out)(const char*, void*), void* ud) {
    char b[120];
    if (!H.mmio) { out("no HD Audio controller probed", ud); return; }
    int n = 0;
    const char* s = H.ready ? "running" : "not running";
    for (const char* p = "HDA controller: "; *p; p++) b[n++] = *p;
    for (const char* p = s; *p; p++) b[n++] = *p;
    b[n] = 0;
    out(b, ud);
    if (!H.ready) return;
    // codec tree
    uint32_t wsub = get_param(H.afg, 0x04);
    int wfirst = (int)((wsub >> 16) & 0xFF), wcnt = (int)(wsub & 0xFF);
    static const char* tn[16] = { "output", "input", "mixer", "selector", "pin", "power", "volume knob", "beep", "?", "?", "?", "?", "?", "?", "?", "vendor" };
    for (int w = wfirst; w < wfirst + wcnt; w++) {
        int t = node_type(w);
        uint32_t cfg = 0;
        n = 0;
        b[n++] = 'n'; b[n++] = 'o'; b[n++] = 'd'; b[n++] = 'e'; b[n++] = ' ';
        { char t2[4]; int k = 0, v = w; if (!v) t2[k++] = '0'; while (v) { t2[k++] = (char)('0' + v % 10); v /= 10; } while (k) b[n++] = t2[--k]; }
        b[n++] = ' ';
        for (const char* p = tn[t]; *p; p++) b[n++] = *p;
        if (t == W_PIN) {
            hda_cmd(verb12(w, 0xF1C, 0), &cfg);
            const char* dn[16] = { "line out", "speaker", "headphone", "cd", "spdif out", "digital out", "modem line", "modem handset", "line in", "aux", "mic", "telephony", "spdif in", "digital in", "?", "other" };
            b[n++] = ' '; b[n++] = '(';
            for (const char* p = dn[(cfg >> 20) & 0xF]; *p; p++) b[n++] = *p;
            b[n++] = ')';
            if ((cfg >> 30) == 1) { for (const char* p = " no-connection"; *p; p++) b[n++] = *p; }
        }
        if (w == H.pin) { for (const char* p = " <- used"; *p; p++) b[n++] = *p; }
        if (w == H.dac) { for (const char* p = " <- used"; *p; p++) b[n++] = *p; }
        b[n] = 0;
        out(b, ud);
    }
}
