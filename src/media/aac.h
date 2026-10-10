#ifndef MEDIA_AAC_H
#define MEDIA_AAC_H

#include <stddef.h>
#include <stdint.h>

// AAC-LC decoder (ISO/IEC 14496-3, 1024-sample frames): raw_data_block() in, 16-bit PCM out.
// Single/pair/LFE elements, long and short windows with the sine and KBD windows, section and
// scalefactor data, pulse data, TNS, M/S, intensity stereo and PNS. Not implemented (rejected with
// AAC_ERR_UNSUPPORTED): Main-profile prediction, LTP, SSR gain control, coupling channels,
// error-resilient object types, 960-sample frames. SBR (HE-AAC) is not decoded: the core is played
// at the core sample rate (sbr_seen tells the caller what is missing).
// Written from the standard; the constant tables are generated (tools/gen_aac_tables.py).

#define AAC_FRAME 1024
#define AAC_MAX_CH 8

enum {
    AAC_ERR_BAD_ARG = -1,
    AAC_ERR_INVALID = -2,        // malformed or truncated data
    AAC_ERR_UNSUPPORTED = -3,
};

struct aac_dec;

struct aac_adts {
    int object_type;             // profile + 1 (2 = LC)
    int sf_index;
    int channel_config;
    int frame_len;               // whole frame including this header
    int header_len;              // 7, or 9 with the CRC
    int nblocks;                 // raw data blocks in the frame (1 for nearly everything)
};

// Parse an ADTS header at data[0]. Returns 0 or -1 (no sync / invalid).
int aac_adts_parse(const uint8_t* data, size_t len, struct aac_adts* out);

struct aac_dec* aac_new(void);
void aac_free(struct aac_dec* d);

// Configure from an AudioSpecificConfig (the esds payload of an mp4 track). Returns 0 or a negative AAC_ERR_*.
int aac_set_config(struct aac_dec* d, const uint8_t* asc, int len);
// Configure from an ADTS header (the parameters ride in every frame).
int aac_set_adts(struct aac_dec* d, const struct aac_adts* h);
// Output channel count: 1 or 2. More source channels are mixed down, one channel is duplicated.
// Default: min(source channels, 2). Call after the configuration.
void aac_set_output_channels(struct aac_dec* d, int channels);
void aac_reset(struct aac_dec* d);          // after a seek: drops the overlap

int aac_channels(const struct aac_dec* d);          // source channels (from the configuration)
int aac_out_channels(const struct aac_dec* d);
int aac_sample_rate(const struct aac_dec* d);       // core sample rate
int aac_sbr_seen(const struct aac_dec* d);          // SBR data or an SBR signalling was seen (not decoded)

// Decode one raw_data_block (an mp4 sample / an ADTS payload). pcm receives AAC_FRAME * out_channels
// interleaved samples. Returns samples per channel, or a negative AAC_ERR_*.
int aac_decode(struct aac_dec* d, const uint8_t* data, size_t len, int16_t* pcm);

#endif
