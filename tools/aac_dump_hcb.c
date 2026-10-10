#include <stdint.h>
#include <stdio.h>
#include "codebook/hcb.h"

static void emit(const char* name, uint32_t code, int len, int a, int b, int c, int d) {
    printf("%s %d %u %d %d %d %d\n", name, len, code, a, b, c, d);
}
static int two_pair(const char* name, const hcb* t1, int n1, int bits1, const hcb_2_pair* t2) {
    for (int cw = 0; cw < n1; cw++) {
        int off = t1[cw].offset, eb = t1[cw].extra_bits;
        for (int e = 0; e < (1 << eb); e++) {
            int len = t2[off + e].bits, tot = bits1 + eb;
            uint32_t full = ((uint32_t)cw << eb) | (uint32_t)e;
            if (len > tot || len < 1) { fprintf(stderr, "%s bad len\n", name); return 1; }
            if (full & ((1u << (tot - len)) - 1)) continue;
            emit(name, full >> (tot - len), len, t2[off + e].x, t2[off + e].y, 0, 0);
        }
    }
    return 0;
}
static int two_quad(const char* name, const hcb* t1, int n1, int bits1, const hcb_2_quad* t2) {
    for (int cw = 0; cw < n1; cw++) {
        int off = t1[cw].offset, eb = t1[cw].extra_bits;
        for (int e = 0; e < (1 << eb); e++) {
            int len = t2[off + e].bits, tot = bits1 + eb;
            uint32_t full = ((uint32_t)cw << eb) | (uint32_t)e;
            if (len > tot || len < 1) { fprintf(stderr, "%s bad len\n", name); return 1; }
            if (full & ((1u << (tot - len)) - 1)) continue;
            emit(name, full >> (tot - len), len, t2[off + e].x, t2[off + e].y, t2[off + e].v, t2[off + e].w);
        }
    }
    return 0;
}
struct st { int node, len; uint32_t code; };
static void bin_quad(const char* name, const hcb_bin_quad* T) {
    struct st s[512]; int sp = 0; s[sp++] = (struct st){0, 0, 0};
    while (sp) { struct st c = s[--sp];
        if (T[c.node].is_leaf) emit(name, c.code, c.len, T[c.node].data[0], T[c.node].data[1], T[c.node].data[2], T[c.node].data[3]);
        else for (int b = 1; b >= 0; b--) s[sp++] = (struct st){c.node + T[c.node].data[b], c.len + 1, (c.code << 1) | b}; }
}
static void bin_pair(const char* name, const hcb_bin_pair* T) {
    struct st s[512]; int sp = 0; s[sp++] = (struct st){0, 0, 0};
    while (sp) { struct st c = s[--sp];
        if (T[c.node].is_leaf) emit(name, c.code, c.len, T[c.node].data[0], T[c.node].data[1], 0, 0);
        else for (int b = 1; b >= 0; b--) s[sp++] = (struct st){c.node + T[c.node].data[b], c.len + 1, (c.code << 1) | b}; }
}
int main(void) {
    int bad = 0;
    bad |= two_quad("cb1", hcb1_1, 32, 5, hcb1_2);
    bad |= two_quad("cb2", hcb2_1, 32, 5, hcb2_2);
    bin_quad("cb3", hcb3);
    bad |= two_quad("cb4", hcb4_1, 32, 5, hcb4_2);
    bin_pair("cb5", hcb5);
    bad |= two_pair("cb6", hcb6_1, 32, 5, hcb6_2);
    bin_pair("cb7", hcb7);
    bad |= two_pair("cb8", hcb8_1, 32, 5, hcb8_2);
    bin_pair("cb9", hcb9);
    bad |= two_pair("cb10", hcb10_1, 64, 6, hcb10_2);
    bad |= two_pair("cb11", hcb11_1, 32, 5, hcb11_2);
    struct st s[512]; int sp = 0; s[sp++] = (struct st){0, 0, 0};
    while (sp) { struct st c = s[--sp];
        if (hcb_sf[c.node][1] == 0) emit("sf", c.code, c.len, hcb_sf[c.node][0], 0, 0, 0);
        else for (int b = 1; b >= 0; b--) s[sp++] = (struct st){c.node + hcb_sf[c.node][b], c.len + 1, (c.code << 1) | b}; }
    return bad;
}
