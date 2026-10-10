#ifndef MEDIA_WEBM_H
#define MEDIA_WEBM_H

#include <stddef.h>
#include <stdint.h>

// WebM / Matroska demuxer (EBML). Incremental: feed it bytes as they arrive, in chunks of any
// size, and it reports the track list once and then every frame (video frame, Opus packet) in
// file order. The same parser serves a downloaded file and MediaSource appends (an init segment
// followed by clusters). Frames are copied out of a private buffer only if the caller keeps them:
// the data pointer is valid during the callback.

#define WEBM_MAX_TRACKS 8
#define WEBM_TRACK_VIDEO 1
#define WEBM_TRACK_AUDIO 2

struct webm_track {
    int number;                 // the id blocks refer to
    int type;                   // WEBM_TRACK_*
    char codec[24];             // "V_VP9", "A_OPUS", ...
    int width, height;          // coded size (video)
    int display_width, display_height;
    double sample_rate;         // audio
    int channels;
    int bit_depth;
    uint64_t codec_delay_ns;    // Opus pre-skip
    uint64_t seek_preroll_ns;
    uint64_t default_duration_ns;
    uint8_t* priv;              // CodecPrivate (OpusHead for Opus), owned by the demuxer
    int priv_len;
};

struct webm_cue {
    int64_t time_ns;
    uint64_t cluster_pos;       // byte offset of the cluster from the start of the segment data
    int track;
};

struct webm_info {
    uint64_t timecode_scale;    // ns per tick (default 1000000)
    double duration_ns;         // 0 when unknown (live)
    int ntracks;
    struct webm_track tracks[WEBM_MAX_TRACKS];
    const struct webm_cue* cues;
    int ncues;
    uint64_t segment_pos;       // stream offset where the segment's data starts
};

struct webm_frame {
    int track;                  // webm_track.number
    int64_t pts_ns;
    int64_t duration_ns;        // 0 if the muxer did not say
    int keyframe;
    int invisible;
    int64_t discard_padding_ns; // Opus: samples to cut from the end of this packet
    const uint8_t* data;
    size_t len;
};

struct webm_cb {
    void (*tracks)(void* user, const struct webm_info* info);     // once the Tracks element is complete
    void (*frame)(void* user, const struct webm_frame* f);
    void* user;
};

struct webm_demux;

struct webm_demux* webm_new(const struct webm_cb* cb);
void webm_free(struct webm_demux* d);
// Start over for a new stream (seek / new init segment); keeps the callbacks.
void webm_reset(struct webm_demux* d);
// Returns 0, or a negative value when the stream is not valid WebM (the demuxer stays failed
// until webm_reset).
int webm_feed(struct webm_demux* d, const uint8_t* data, size_t len);
const struct webm_info* webm_get_info(const struct webm_demux* d);
// Bytes consumed so far (stream offset of the next unparsed byte).
uint64_t webm_position(const struct webm_demux* d);

#endif
