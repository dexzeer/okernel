// Opus packet framing: the TOC byte and the four frame-packing codes (RFC 6716, section 3).
#include "opus.h"

static const int frame_ms10_tab[32] = {
    100, 200, 400, 600,  100, 200, 400, 600,  100, 200, 400, 600,      // SILK NB / MB / WB: 10 20 40 60 ms
    100, 200,  100, 200,                                                  // hybrid SWB, FB: 10 20 ms
    25, 50, 100, 200,  25, 50, 100, 200,  25, 50, 100, 200,  25, 50, 100, 200,   // CELT NB WB SWB FB: 2.5 5 10 20 ms
};

// One or two bytes of frame length (3.2.1). Returns the number of bytes used or -1.
static int read_len(const uint8_t* p, int avail, int* len) {
    if (avail < 1) return -1;
    if (p[0] < 252) { *len = p[0]; return 1; }
    if (avail < 2) return -1;
    *len = 4 * p[1] + p[0];
    return 2;
}

int opus_parse_packet(const uint8_t* data, int len, struct opus_packet* o) {
    if (len < 1) return -1;                                       // [R1]
    int toc = data[0];
    o->config = toc >> 3;
    o->stereo = (toc >> 2) & 1;
    o->mode = o->config < 12 ? OPUS_MODE_SILK : o->config < 16 ? OPUS_MODE_HYBRID : OPUS_MODE_CELT;
    if (o->config < 12) o->bandwidth = o->config < 4 ? OPUS_BW_NB : o->config < 8 ? OPUS_BW_MB : OPUS_BW_WB;
    else if (o->config < 16) o->bandwidth = o->config < 14 ? OPUS_BW_SWB : OPUS_BW_FB;
    else {
        int g = (o->config - 16) >> 2;                            // NB, WB, SWB, FB
        o->bandwidth = g == 0 ? OPUS_BW_NB : g == 1 ? OPUS_BW_WB : g == 2 ? OPUS_BW_SWB : OPUS_BW_FB;
    }
    o->frame_ms10 = frame_ms10_tab[o->config];
    const uint8_t* p = data + 1;
    int avail = len - 1;
    for (int i = 0; i < OPUS_MAX_FRAMES; i++) { o->frame[i] = 0; o->size[i] = 0; }

    switch (toc & 3) {
    case 0:                                                       // one frame
        if (avail > 1275) return -1;                              // [R2]
        o->nframes = 1; o->frame[0] = p; o->size[0] = avail;
        return 0;
    case 1:                                                       // two equal frames
        if (avail & 1) return -1;                                 // [R3]
        if (avail / 2 > 1275) return -1;
        o->nframes = 2;
        o->frame[0] = p; o->size[0] = avail / 2;
        o->frame[1] = p + avail / 2; o->size[1] = avail / 2;
        return 0;
    case 2: {                                                     // two frames, first length coded
        int n1;
        int u = read_len(p, avail, &n1);
        if (u < 0 || n1 > avail - u) return -1;                   // [R4]
        o->nframes = 2;
        o->frame[0] = p + u; o->size[0] = n1;
        o->frame[1] = p + u + n1; o->size[1] = avail - u - n1;
        if (o->size[1] > 1275) return -1;
        return 0;
    }
    default: break;
    }
    // code 3: a signalled number of frames
    if (avail < 1) return -1;                                     // [R6][R7]
    int fc = p[0];
    int m = fc & 0x3F, vbr = (fc >> 7) & 1, pad = (fc >> 6) & 1;
    p++; avail--;
    if (m == 0 || m > OPUS_MAX_FRAMES) return -1;                 // [R5]
    if ((long)m * o->frame_ms10 > 1200) return -1;                // at most 120 ms
    int padding = 0;
    if (pad) {
        for (;;) {
            if (avail < 1) return -1;
            int b = *p++; avail--;
            if (b == 255) padding += 254;
            else { padding += b; break; }
        }
    }
    if (padding > avail) return -1;
    avail -= padding;                                             // padding sits at the end of the packet
    o->nframes = m;
    if (!vbr) {                                                   // CBR: equal sizes
        if (avail % m) return -1;
        int each = avail / m;
        if (each > 1275) return -1;
        for (int i = 0; i < m; i++) { o->frame[i] = p + i * each; o->size[i] = each; }
        return 0;
    }
    int sizes[OPUS_MAX_FRAMES], total = 0;
    for (int i = 0; i < m - 1; i++) {                             // M-1 lengths, the last frame takes the rest
        int l, u = read_len(p, avail, &l);
        if (u < 0) return -1;
        p += u; avail -= u;
        sizes[i] = l; total += l;
    }
    if (total > avail) return -1;                                 // [R7]
    sizes[m - 1] = avail - total;
    if (sizes[m - 1] > 1275) return -1;
    for (int i = 0; i < m; i++) { o->frame[i] = p; o->size[i] = sizes[i]; p += sizes[i]; }
    return 0;
}

// Self-delimited framing (RFC 6716, Appendix B): the packets of every stream of a multistream packet except the last one carry
// the length of their final frame explicitly, so the next stream's packet can be found behind it. `*used` = bytes this packet occupies.
int opus_parse_packet_sd(const uint8_t* data, int len, struct opus_packet* o, int* used) {
    if (len < 1) return -1;
    int toc = data[0];
    o->config = toc >> 3;
    o->stereo = (toc >> 2) & 1;
    o->mode = o->config < 12 ? OPUS_MODE_SILK : o->config < 16 ? OPUS_MODE_HYBRID : OPUS_MODE_CELT;
    if (o->config < 12) o->bandwidth = o->config < 4 ? OPUS_BW_NB : o->config < 8 ? OPUS_BW_MB : OPUS_BW_WB;
    else if (o->config < 16) o->bandwidth = o->config < 14 ? OPUS_BW_SWB : OPUS_BW_FB;
    else {
        int g = (o->config - 16) >> 2;
        o->bandwidth = g == 0 ? OPUS_BW_NB : g == 1 ? OPUS_BW_WB : g == 2 ? OPUS_BW_SWB : OPUS_BW_FB;
    }
    o->frame_ms10 = frame_ms10_tab[o->config];
    const uint8_t* p = data + 1;
    int avail = len - 1;
    for (int i = 0; i < OPUS_MAX_FRAMES; i++) { o->frame[i] = 0; o->size[i] = 0; }
    int u, n1, n2;
    switch (toc & 3) {
    case 0:
        u = read_len(p, avail, &n1);
        if (u < 0 || n1 > 1275 || n1 > avail - u) return -1;
        o->nframes = 1; o->frame[0] = p + u; o->size[0] = n1;
        *used = 1 + u + n1;
        return 0;
    case 1:
        u = read_len(p, avail, &n1);
        if (u < 0 || n1 > 1275 || 2 * n1 > avail - u) return -1;
        o->nframes = 2;
        o->frame[0] = p + u; o->size[0] = n1;
        o->frame[1] = p + u + n1; o->size[1] = n1;
        *used = 1 + u + 2 * n1;
        return 0;
    case 2: {
        u = read_len(p, avail, &n1);
        if (u < 0) return -1;
        int u2 = read_len(p + u, avail - u, &n2);
        if (u2 < 0 || n1 > 1275 || n2 > 1275 || n1 + n2 > avail - u - u2) return -1;
        o->nframes = 2;
        o->frame[0] = p + u + u2; o->size[0] = n1;
        o->frame[1] = p + u + u2 + n1; o->size[1] = n2;
        *used = 1 + u + u2 + n1 + n2;
        return 0;
    }
    default: break;
    }
    if (avail < 1) return -1;
    int fc = p[0];
    int m = fc & 0x3F, vbr = (fc >> 7) & 1, pad = (fc >> 6) & 1;
    p++; avail--;
    if (m == 0 || m > OPUS_MAX_FRAMES || (long)m * o->frame_ms10 > 1200) return -1;
    int padding = 0;
    if (pad) {
        for (;;) {
            if (avail < 1) return -1;
            int b = *p++; avail--;
            if (b == 255) padding += 254;
            else { padding += b; break; }
        }
    }
    int sizes[OPUS_MAX_FRAMES], total = 0;
    if (!vbr) {
        u = read_len(p, avail, &n1);
        if (u < 0 || n1 > 1275) return -1;
        p += u; avail -= u;
        for (int i = 0; i < m; i++) { sizes[i] = n1; total += n1; }
    } else {
        for (int i = 0; i < m; i++) {
            u = read_len(p, avail, &n1);
            if (u < 0 || n1 > 1275) return -1;
            p += u; avail -= u;
            sizes[i] = n1; total += n1;
        }
    }
    if (total + padding > avail) return -1;
    o->nframes = m;
    const uint8_t* q = p;
    for (int i = 0; i < m; i++) { o->frame[i] = q; o->size[i] = sizes[i]; q += sizes[i]; }
    *used = (int)(q - data) + padding;
    return 0;
}
