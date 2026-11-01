// UAX #9 (Unicode Bidirectional Algorithm), standard-shaped: X rules over the
// whole paragraph (explicit marks and the synthetic element controls the
// caller injects), then per isolated run W/N/I rules, then L2 reordering.

#include <stdint.h>
#include "bdata.h"
#include "bidi.h"

int bdi_class(uint32_t cp) {
    int lo = 0, hi = BDI_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < bdi_tab[mid].a) hi = mid - 1;
        else if (cp > bdi_tab[mid].b) lo = mid + 1;
        else return bdi_tab[mid].c;
    }
    return BCL_L;
}

// the common mirrored pairs (ASCII brackets + guillemets + math)
static const uint16_t mirr_tab[] = {
    0x0028, 0x0029, 0x0029, 0x0028, 0x003C, 0x003E, 0x003E, 0x003C,
    0x005B, 0x005D, 0x005D, 0x005B, 0x007B, 0x007D, 0x007D, 0x007B,
    0x00AB, 0x00BB, 0x00BB, 0x00AB, 0x2039, 0x203A, 0x203A, 0x2039,
    0x2045, 0x2046, 0x2046, 0x2045, 0x207D, 0x207E, 0x207E, 0x207D,
    0x2308, 0x2309, 0x2309, 0x2308, 0x230A, 0x230B, 0x230B, 0x230A,
    0x2768, 0x2769, 0x2769, 0x2768, 0x3008, 0x3009, 0x3009, 0x3008,
    0x300A, 0x300B, 0x300B, 0x300A, 0x300C, 0x300D, 0x300D, 0x300C,
    0x3010, 0x3011, 0x3011, 0x3010, 0xFF08, 0xFF09, 0xFF09, 0xFF08,
    0xFF1C, 0xFF1E, 0xFF1E, 0xFF1C, 0xFF3B, 0xFF3D, 0xFF3D, 0xFF3B,
    0xFF5B, 0xFF5D, 0xFF5D, 0xFF5B,
};

uint32_t bdi_mirror(uint32_t cp) {
    for (unsigned i = 0; i < sizeof mirr_tab / sizeof mirr_tab[0]; i += 2)
        if (mirr_tab[i] == cp) return mirr_tab[i + 1];
    return 0;
}

// opening brackets for N0 (matched with their counterparts)
static int bdi_is_open(uint32_t cp) {
    for (unsigned i = 0; i < sizeof mirr_tab / sizeof mirr_tab[0]; i += 2)
        if (mirr_tab[i] == cp) return (int)i;
    return -1;
}

int bdi_first_strong(const uint32_t* cps, int n, int def) {
    int depth = 0;
    for (int i = 0; i < n; i++) {
        int c = bdi_class(cps[i]);
        if (c == BCL_LRE || c == BCL_RLE || c == BCL_LRO || c == BCL_RLO || c == BCL_PDF) continue;
        if (c == BCL_LRI || c == BCL_RLI || c == BCL_FSI) { depth++; continue; }
        if (c == BCL_PDI) { if (depth > 0) depth--; continue; }
        if (c == BCL_L) return 0;
        if (c == BCL_R || c == BCL_AL) return 1;
    }
    return def;
}

#define MAXLEV 125

void bidi_levels(const uint32_t* cps, int n, int base, uint8_t* lvl, uint8_t* ctrl) {
    if (n <= 0) return;
    if (n > 8192) n = 8192;               // static scratch size
    // ---- X1-X9: embedding levels ------------------------------------------------
    struct { uint8_t level; uint8_t override; uint8_t isolate; } stk[64];
    int sp = 0;
    int cur = base & 1, ovr = 0;          // override: 0 none, 1 L, 2 R
    int pend_bound = 0;
    for (int i = 0; i < n; i++) {
        int c = bdi_class(cps[i]);
        ctrl[i] = 0;
        switch (c) {
        case BCL_LRE: case BCL_RLE: case BCL_LRO: case BCL_RLO: case BCL_PDF:
        case BCL_LRI: case BCL_RLI: case BCL_FSI: case BCL_PDI:
            ctrl[i] = 1;
            break;
        }
        switch (c) {
        case BCL_RLE: case BCL_LRE:
            if (sp < 64) { stk[sp].level = (uint8_t)cur; stk[sp].override = (uint8_t)ovr; stk[sp].isolate = 0; sp++; }
            cur = c == BCL_RLE ? ((cur + 1) | 1) : ((cur + 2) & ~1);
            if (cur > MAXLEV) cur = MAXLEV & ~0;  // ignore: keep level
            ovr = 0;
            break;
        case BCL_RLO: case BCL_LRO:
            if (sp < 64) { stk[sp].level = (uint8_t)cur; stk[sp].override = (uint8_t)ovr; stk[sp].isolate = 0; sp++; }
            cur = c == BCL_RLO ? ((cur + 1) | 1) : ((cur + 2) & ~1);
            ovr = c == BCL_RLO ? 2 : 1;
            break;
        case BCL_PDF:
            if (sp > 0) { sp--; cur = stk[sp].level; ovr = stk[sp].override; }
            break;
        case BCL_RLI: case BCL_LRI: case BCL_FSI: {
            int d = c == BCL_RLI ? 1 : 0;
            if (c == BCL_FSI) {
                // first strong inside the matching isolate
                int depth2 = 0;
                d = base & 1;
                for (int j = i + 1; j < n; j++) {
                    int c2 = bdi_class(cps[j]);
                    if (c2 == BCL_LRI || c2 == BCL_RLI || c2 == BCL_FSI) depth2++;
                    else if (c2 == BCL_PDI) { if (depth2 == 0) break; depth2--; }
                    else if (c2 == BCL_L) { d = 0; break; }
                    else if (c2 == BCL_R || c2 == BCL_AL) { d = 1; break; }
                }
            }
            if (sp < 64) { stk[sp].level = (uint8_t)cur; stk[sp].override = (uint8_t)ovr; stk[sp].isolate = 1; sp++; }
            cur = d ? ((cur + 1) | 1) : ((cur + 2) & ~1);
            ovr = 0;
            pend_bound = 1;                 // an isolate boundary before the next char
            break;
        }
        case BCL_PDI:
            while (sp > 0) {
                sp--;
                int was = stk[sp].isolate;
                cur = stk[sp].level; ovr = stk[sp].override;
                if (was) break;
            }
            pend_bound = 1;
            break;
        default:
            lvl[i] = (uint8_t)(ovr == 2 ? (cur | 1) : ovr == 1 ? (cur & ~1) : cur);
            ctrl[i] = 0;
            if (ovr) ctrl[i] |= 4;          // overridden: the level is forced, no I-rule adjustments
            // the isolate boundary applies between the previous content char and this one
            if (i > 0 && pend_bound) ctrl[i] |= 2;   // flag: run boundary before i
            pend_bound = 0;
            break;
        }
    }
    // ---- isolated runs ----------------------------------------------------------
    // types after X (controls keep their class but are skipped inside runs)
    static uint8_t ty[8192];
    for (int i = 0; i < n; i++) ty[i] = (uint8_t)bdi_class(cps[i]);
    int i = 0;
    while (i < n) {
        int start = i;
        int runlvl = lvl[i];
        i++;
        while (i < n && !(ctrl[i] & 2) && !(ctrl[i] & 1) && lvl[i] == runlvl) i++;
        // [start, i) is one isolated run of content chars (ctrl&1 entries may
        // appear inside; they are dropped)
        int end = i;
        // sos/eos: level parity of the neighbouring content char (skipping
        // formatting characters), or the paragraph level
        int sos = base & 1, eos = base & 1;
        for (int q = start - 1; q >= 0; q--)
            if (!(ctrl[q] & 1)) { sos = lvl[q] & 1; break; }
        for (int q = end; q < n; q++)
            if (!(ctrl[q] & 1)) { eos = lvl[q] & 1; break; }
        // ---- W rules -----------------------------------------------------------
        int last_strong = sos;                        // 0 L, 1 R (tracks AL for W2)
        int last_al = 0;
        for (int k = start; k < end; k++) {
            int t = ty[k];
            if (ctrl[k] & 1) continue;
            if (t == BCL_NSM) t = (k > start && !(ctrl[k - 1] & 1)) ? ty[k - 1] : (sos ? BCL_R : BCL_L);
            else if (t == BCL_EN && last_al) t = BCL_AN;
            else if (t == BCL_AL) t = BCL_R;
            if (t == BCL_L || t == BCL_R) { last_strong = t == BCL_R; last_al = 0; }
            ty[k] = (uint8_t)t;
        }
        // W4/W5/W6/W7 (a second pass for the contextual weak rules)
        for (int k = start; k < end; k++) {
            if (ctrl[k] & 1) continue;
            if (ty[k] == BCL_ES || ty[k] == BCL_CS) {
                int prev = -1, next = -1;
                if (k > start && !(ctrl[k - 1] & 1)) prev = ty[k - 1];
                if (k + 1 < end && !(ctrl[k + 1] & 1)) next = ty[k + 1];
                if (ty[k] == BCL_ES && prev == BCL_EN && next == BCL_EN) { ty[k] = BCL_EN; continue; }
                if (ty[k] == BCL_CS && prev == BCL_EN && next == BCL_EN) { ty[k] = BCL_EN; continue; }
                if (ty[k] == BCL_CS && prev == BCL_AN && next == BCL_AN) { ty[k] = BCL_AN; continue; }
                ty[k] = BCL_ON;
            } else if (ty[k] == BCL_ET) {
                int prev = (k > start && !(ctrl[k - 1] & 1)) ? ty[k - 1] : BCL_ON;
                int next = (k + 1 < end && !(ctrl[k + 1] & 1)) ? ty[k + 1] : BCL_ON;
                if (prev == BCL_EN || next == BCL_EN) ty[k] = BCL_EN;
                else ty[k] = BCL_ON;
            }
        }
        // W5 second half: an ET run before an EN (leading ETs); W7
        for (int k = start; k < end; k++) {
            if ((ctrl[k] & 1) || ty[k] != BCL_ON) continue;
            // leading ETs before an EN (W5)
            int k2 = k;
            while (k2 < end && !(ctrl[k2] & 1) && ty[k2] == BCL_ON) k2++;
            if (k2 < end && !(ctrl[k2] & 1) && ty[k2] == BCL_EN) {
                int all_et = 1;
                for (int q = k; q < k2; q++) {
                    int o = bdi_class(cps[q]);
                    if (o != BCL_ET) all_et = 0;
                }
                if (all_et) for (int q = k; q < k2; q++) ty[q] = BCL_EN;
            }
        }
        last_strong = sos; last_al = 0;
        for (int k = start; k < end; k++) {
            if (ctrl[k] & 1) continue;
            if (ty[k] == BCL_L || ty[k] == BCL_R) { last_strong = ty[k] == BCL_R; last_al = 0; }
            else if (ty[k] == BCL_EN && !last_strong) ty[k] = BCL_L;   // W7
        }
        // ---- N rules -----------------------------------------------------------
        // N0 brackets
        {
            struct { int idx; int ch; } bst[64];
            int bsp = 0;
            for (int k = start; k < end; k++) {
                if (ctrl[k] & 1) continue;
                uint32_t cp = cps[k];
                int oi = bdi_is_open(cp);
                int is_open = oi >= 0 && (oi & 2) == 0;   // even slots are openers
                if (is_open) {
                    if (bsp < 64) { bst[bsp].idx = k; bst[bsp].ch = (int)cp; bsp++; }
                    continue;
                }
                // a closer: match the innermost opener
                uint32_t mc = bdi_mirror(cp);
                int found = -1;
                for (int q = bsp - 1; q >= 0; q--)
                    if ((uint32_t)bst[q].ch == mc) { found = q; break; }
                if (found < 0) continue;
                int a = bst[found].idx, b2 = k;
                bsp = found;
                // inside strong?
                int dir_in = -1;                          // 0 L, 1 R
                for (int q = a + 1; q < b2; q++) {
                    if (ctrl[q] & 1) continue;
                    if (ty[q] == BCL_R) { dir_in = 1; break; }
                    if (ty[q] == BCL_L) { if (dir_in != 1) dir_in = 0; }
                }
                if (dir_in < 0) continue;
                int emb = runlvl & 1;
                if (dir_in == emb) {
                    ty[a] = ty[b2] = (uint8_t)(emb ? BCL_R : BCL_L);
                } else {
                    // strong of the embedding direction before the pair?
                    int before = -1;
                    for (int q = start; q < a; q++) {
                        if (ctrl[q] & 1) continue;
                        if (ty[q] == BCL_R) { before = 1; break; }
                        if (ty[q] == BCL_L) { before = 0; break; }
                    }
                    if (before == emb) ty[a] = ty[b2] = (uint8_t)(dir_in ? BCL_R : BCL_L);
                    else ty[a] = ty[b2] = (uint8_t)(emb ? BCL_R : BCL_L);
                }
            }
        }
        // N1/N2: neutral runs
        {
            int k = start;
            while (k < end) {
                if ((ctrl[k] & 1) || (ty[k] != BCL_ON && ty[k] != BCL_WS && ty[k] != BCL_S && ty[k] != BCL_B)) { k++; continue; }
                int a = k;
                while (k < end && !(ctrl[k] & 1) && (ty[k] == BCL_ON || ty[k] == BCL_WS || ty[k] == BCL_S || ty[k] == BCL_B)) k++;
                int b2 = k;                               // [a, b2) neutrals
                int left = a > start ? ty[a - 1] : (sos ? BCL_R : BCL_L);
                int right = b2 < end ? ty[b2] : (eos ? BCL_R : BCL_L);
                int l_is_l = left == BCL_L;
                int r_is_l = right == BCL_L;
                int set;
                if (l_is_l && r_is_l) set = BCL_L;
                else if (!l_is_l && !r_is_l && left != BCL_ON && right != BCL_ON &&
                         ((left == BCL_R || left == BCL_AL || left == BCL_AN || left == BCL_EN) &&
                          (right == BCL_R || right == BCL_AL || right == BCL_AN || right == BCL_EN))) set = BCL_R;
                else set = (runlvl & 1) ? BCL_R : BCL_L;  // N2
                for (int q = a; q < b2; q++) ty[q] = (uint8_t)set;
            }
        }
        // ---- I rules -----------------------------------------------------------
        for (int k = start; k < end; k++) {
            if (ctrl[k] & 1) continue;
            if (ctrl[k] & 4) continue;            // overridden: level is forced
            if (runlvl & 1) {
                if (ty[k] == BCL_L) lvl[k] = (uint8_t)(runlvl + 1);
            } else {
                if (ty[k] == BCL_R || ty[k] == BCL_AN) lvl[k] = (uint8_t)(runlvl + 1);
                else if (ty[k] == BCL_EN) lvl[k] = (uint8_t)(runlvl + 2);
            }
        }
    }
}

int bidi_reorder(const uint8_t* lvl, const uint8_t* ctrl, int n, int* order) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (ctrl && (ctrl[i] & 1)) continue;
        order[m++] = i;
    }
    // L2: from the highest level down to the lowest odd one, reverse runs
    int maxl = 0, minodd = 1 << 30;
    for (int i = 0; i < m; i++) {
        if (lvl[order[i]] > maxl) maxl = lvl[order[i]];
        if ((lvl[order[i]] & 1) && lvl[order[i]] < minodd) minodd = lvl[order[i]];
    }
    for (int l = maxl; l >= minodd && l > 0; l--) {
        int a = 0;
        while (a < m) {
            if (lvl[order[a]] < l) { a++; continue; }
            int b2 = a;
            while (b2 < m && lvl[order[b2]] >= l) b2++;
            for (int x = a, y = b2 - 1; x < y; x++, y--) { int t = order[x]; order[x] = order[y]; order[y] = t; }
            a = b2;
        }
    }
    return m;
}
