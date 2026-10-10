#ifndef MEDIA_MP4_H
#define MEDIA_MP4_H

#include <stddef.h>
#include <stdint.h>

// ISO base media file format (MP4 / MOV) demuxer, incremental like the WebM one: feed it bytes as
// they arrive, in pieces of any size. It reports the tracks once the movie header (moov) is known and
// then every sample in file order. Plain files (sample tables in moov) and fragmented files (moof/trun,
// the MediaSource shape: an init segment followed by media segments) both work. If mdat comes
// before moov the media data is held until the sample tables arrive (bounded by MP4_MAX_HOLD).
//
// Written from ISO/IEC 14496-12 (box structure) and 14496-14/-15 (esds, avcC).

#define MP4_MAX_TRACKS 8
#define MP4_TRACK_VIDEO 1
#define MP4_TRACK_AUDIO 2
#define MP4_TRACK_OTHER 3

#define MP4_MAX_HOLD (96u << 20)         // largest amount of media data kept while waiting for moov
#define MP4_MAX_BOX (32u << 20)          // largest moov/moof accepted

struct mp4_track {
    int id;                              // track_ID (what samples refer to)
    int type;                            // MP4_TRACK_*
    char codec[5];                       // sample entry fourcc: "avc1", "mp4a", "vp09", "Opus", ...
    uint32_t timescale;
    int width, height;                   // video (from the sample entry)
    int channels, sample_rate, sample_size;   // audio
    int object_type;                     // mp4a: objectTypeIndication from esds (0x40 = AAC, 0x6B = MP3)
    uint8_t* config;                     // avcC / vpcC / dOps payload, or the AudioSpecificConfig of an esds; owned by the demuxer
    int config_len;
    int64_t start_offset_ns;             // edit list: how much the first sample is shifted (pts = dts + ctts - this)
    int nal_length_size;                 // avc1: from avcC
};

struct mp4_info {
    int ntracks;
    struct mp4_track tracks[MP4_MAX_TRACKS];
    uint32_t timescale;                  // movie timescale
    int64_t duration_ns;                 // 0 when unknown (fragmented)
    int fragmented;
};

struct mp4_sample {
    int track;                           // mp4_track.id
    int64_t dts_ns, pts_ns, duration_ns;
    int key;
    const uint8_t* data;                 // valid during the callback
    size_t len;
};

struct mp4_cb {
    void (*tracks)(void* user, const struct mp4_info* info);
    void (*sample)(void* user, const struct mp4_sample* s);
    void* user;
};

struct mp4_demux;

struct mp4_demux* mp4_new(const struct mp4_cb* cb);
void mp4_free(struct mp4_demux* d);
// Start over (a new init segment follows). Keeps the callbacks.
void mp4_reset(struct mp4_demux* d);
// Returns 0, or a negative value for a stream that is not valid MP4 (the demuxer stays failed until reset).
int mp4_feed(struct mp4_demux* d, const uint8_t* data, size_t len);
const struct mp4_info* mp4_get_info(const struct mp4_demux* d);

#endif
