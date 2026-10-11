// 802.11 station logic; see wsta.h.
#include "wsta.h"
#include "wcrypto.h"
#include "../string.h"

#define MAXBSS 48
#define LOG(...) do { if (O && O->log) O->log(__VA_ARGS__); } while (0)

static const struct wsta_ops* O;
static uint8_t MAC[6];
static struct wsta_bss L[MAXBSS];

static struct {
    int state;
    char ssid[33]; uint8_t ssid_len;
    uint8_t psk[32]; int open;
    struct wsta_bss target;
    uint8_t bssid[6];
    int aid;
    uint32_t t_start, t_last;          // state entry, last transmission
    int tries;
    uint8_t snonce[32], anonce[32];
    uint8_t ptk[48];
    int have_ptk, ptk_installed, have_gtk;
    uint8_t gtk[16]; int gtk_id;
    uint64_t last_replay; int have_replay;
    uint64_t tx_pn;
    uint64_t rx_pn[9];                 // unicast TIDs 0-7, [8] = group
    uint16_t seq;
    uint8_t rsn[24]; int rsn_len;     // our RSN information element (as sent in the association request)
    uint32_t tx_frames, rx_frames, bad;
    uint32_t last_tx_ms;
} S;

// ---- helpers ----------------------------------------------------------------------------------------------------------------------
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t now(void) { return O->now_ms(); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

const char* wsta_state_name(void) {
    switch (S.state) { case WSTA_IDLE: return "idle"; case WSTA_AUTH: return "authenticating"; case WSTA_ASSOC: return "associating";
    case WSTA_HANDSHAKE: return "key handshake"; case WSTA_UP: return "connected"; default: return "failed"; }
}
int wsta_state(void) { return S.state; }
const uint8_t* wsta_bssid(void) { return S.bssid; }
const char* wsta_ssid(void) { return S.ssid; }
void wsta_stats(uint32_t* tx, uint32_t* rx, uint32_t* bad) { *tx = S.tx_frames; *rx = S.rx_frames; *bad = S.bad; }

void wsta_init(const uint8_t mac[6], const struct wsta_ops* ops) {
    memset(&S, 0, sizeof S);
    memset(L, 0, sizeof L);
    memcpy(MAC, mac, 6);
    O = ops;
}

// ---- scan list ----------------------------------------------------------------------------------------------------------------------
void wsta_reset_scan(void) { memset(L, 0, sizeof L); }
int wsta_bss_count(void) { int n = 0; for (int i = 0; i < MAXBSS; i++) n += L[i].used; return n; }
const struct wsta_bss* wsta_bss_at(int idx) {
    for (int i = 0; i < MAXBSS; i++) if (L[i].used && idx-- == 0) return &L[i];
    return 0;
}

static struct wsta_bss* bss_find(const uint8_t* bssid) {
    int free_i = -1, oldest = 0;
    for (int i = 0; i < MAXBSS; i++) {
        if (L[i].used && !memcmp(L[i].bssid, bssid, 6)) return &L[i];
        if (!L[i].used && free_i < 0) free_i = i;
        if (L[i].used && L[i].seen < L[oldest].seen) oldest = i;
    }
    int i = free_i >= 0 ? free_i : oldest;
    memset(&L[i], 0, sizeof L[i]);
    L[i].used = 1; memcpy(L[i].bssid, bssid, 6);
    return &L[i];
}

// Parse the information elements of a beacon / probe response into the BSS record.
static void parse_ies(struct wsta_bss* b, const uint8_t* ie, int len, uint16_t cap) {
    int sec = (cap & 0x10) ? 1 : 0;
    int rsn_found = 0, usable = 1, akm_psk = 0, akm_other = 0, pairwise_ccmp = 0, group_ccmp = 1;
    for (int p = 0; p + 2 <= len;) {
        int id = ie[p], l = ie[p + 1];
        if (p + 2 + l > len) break;
        const uint8_t* d = ie + p + 2;
        if (id == 0) {
            int n = l > 32 ? 32 : l;
            memcpy(b->ssid, d, (uint32_t)n); b->ssid[n] = 0; b->ssid_len = (uint8_t)n;
        } else if (id == 3 && l >= 1) b->ch = d[0];
        else if (id == 48 && l >= 8) {                                          // RSN: version, group, pairwise list, AKM list, capabilities
            rsn_found = 1;
            int q = 2;
            if (d[q] == 0 && d[q + 1] == 0x0f && d[q + 2] == 0xac) group_ccmp = d[q + 3] == 4; else usable = 0;
            q += 4;
            int np = rd16(d + q); q += 2;
            for (int i = 0; i < np && q + 4 <= l; i++, q += 4) if (d[q] == 0 && d[q + 1] == 0x0f && d[q + 2] == 0xac && d[q + 3] == 4) pairwise_ccmp = 1;
            if (q + 2 <= l) {
                int na = rd16(d + q); q += 2;
                for (int i = 0; i < na && q + 4 <= l; i++, q += 4) {
                    if (d[q] == 0 && d[q + 1] == 0x0f && d[q + 2] == 0xac) { if (d[q + 3] == 2) akm_psk = 1; else akm_other = 1; }
                }
            }
            if (q + 2 <= l && (rd16(d + q) & 0x0040)) usable = 0;               // management frame protection required: not supported
        } else if (id == 221 && l >= 4 && d[0] == 0 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 1 && sec < 2) sec = 2;
        p += 2 + l;
    }
    if (rsn_found) sec = (usable && akm_psk && pairwise_ccmp && group_ccmp) ? 3 : 4;
    (void)akm_other;
    b->sec = (uint8_t)sec;
}

// ---- frame building ---------------------------------------------------------------------------------------------------------------
static int mgmt_header(uint8_t* f, uint8_t fc0, const uint8_t* da, const uint8_t* bssid) {
    memset(f, 0, 24);
    f[0] = fc0; f[1] = 0;
    memcpy(f + 4, da, 6); memcpy(f + 10, MAC, 6); memcpy(f + 16, bssid, 6);
    return 24;
}

static void send_auth(void) {
    uint8_t f[64]; int n = mgmt_header(f, 0xB0, S.bssid, S.bssid);
    put16(f + n, 0); put16(f + n + 2, 1); put16(f + n + 4, 0); n += 6;     // open system, sequence 1, status 0
    O->tx(f, n, 0);
}

static int build_rsn(uint8_t* p) {
    static const uint8_t rsn[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    memcpy(p, rsn, sizeof rsn);
    return (int)sizeof rsn;
}

static void send_assoc(void) {
    uint8_t f[300]; int n = mgmt_header(f, 0x00, S.bssid, S.bssid);
    put16(f + n, S.open ? 0x0421 : 0x0431); n += 2;                         // ESS, short preamble, short slot (+ privacy)
    put16(f + n, 10); n += 2;                                               // listen interval
    f[n++] = 0; f[n++] = S.ssid_len; memcpy(f + n, S.ssid, S.ssid_len); n += S.ssid_len;
    if (S.target.ch <= 14) {
        static const uint8_t r1[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 }, r2[] = { 50, 4, 0x30, 0x48, 0x60, 0x6c };
        memcpy(f + n, r1, sizeof r1); n += (int)sizeof r1; memcpy(f + n, r2, sizeof r2); n += (int)sizeof r2;
    } else {
        static const uint8_t r1[] = { 1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
        memcpy(f + n, r1, sizeof r1); n += (int)sizeof r1;
    }
    if (!S.open) { S.rsn_len = build_rsn(S.rsn); memcpy(f + n, S.rsn, (uint32_t)S.rsn_len); n += S.rsn_len; }
    O->tx(f, n, 0);
}

static void send_deauth(void) {
    uint8_t f[40]; int n = mgmt_header(f, 0xC0, S.bssid, S.bssid);
    put16(f + n, 3); n += 2;                                                // leaving
    O->tx(f, n, 0);
}

// A data frame to the AP: `dst` is the destination address, `type` the Ethernet type, payload follows the LLC/SNAP header.
static int send_data(const uint8_t* dst, uint16_t type, const uint8_t* payload, int plen, int encrypt) {
    static uint8_t f[2400];
    if (plen < 0 || plen > 2200) return -1;
    int n = 24;
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = (uint8_t)(0x01 | (encrypt ? 0x40 : 0));              // data, to DS, protected
    memcpy(f + 4, S.bssid, 6); memcpy(f + 10, MAC, 6); memcpy(f + 16, dst, 6);
    S.seq = (uint16_t)((S.seq + 1) & 0xfff);
    put16(f + 22, (uint16_t)(S.seq << 4));
    uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, (uint8_t)(type >> 8), (uint8_t)type };
    if (!encrypt) {
        memcpy(f + n, llc, 8); n += 8;
        memcpy(f + n, payload, (uint32_t)plen); n += plen;
    } else {
        if (!S.ptk_installed) return -2;
        static uint8_t clear[2300];
        memcpy(clear, llc, 8); memcpy(clear + 8, payload, (uint32_t)plen);
        uint64_t pn = ++S.tx_pn;
        f[n + 0] = (uint8_t)pn; f[n + 1] = (uint8_t)(pn >> 8); f[n + 2] = 0; f[n + 3] = 0x20;
        f[n + 4] = (uint8_t)(pn >> 16); f[n + 5] = (uint8_t)(pn >> 24); f[n + 6] = (uint8_t)(pn >> 32); f[n + 7] = (uint8_t)(pn >> 40);
        uint8_t aad[22], nonce[13];
        aad[0] = (uint8_t)(f[0] & 0x8f); aad[1] = (uint8_t)((f[1] & 0xc7) | 0x40);
        memcpy(aad + 2, f + 4, 18);
        aad[20] = (uint8_t)(f[22] & 0x0f); aad[21] = 0;
        nonce[0] = 0; memcpy(nonce + 1, f + 10, 6);
        nonce[7] = (uint8_t)(pn >> 40); nonce[8] = (uint8_t)(pn >> 32); nonce[9] = (uint8_t)(pn >> 24); nonce[10] = (uint8_t)(pn >> 16); nonce[11] = (uint8_t)(pn >> 8); nonce[12] = (uint8_t)pn;
        uint8_t mic[8];
        wc_ccm_encrypt(S.ptk + 32, nonce, aad, 22, clear, (uint32_t)(plen + 8), f + n + 8, mic);
        n += 8 + plen + 8;
        memcpy(f + n, mic, 8); n += 8;
    }
    S.tx_frames++;
    S.last_tx_ms = now();
    return O->tx(f, n, 1);
}

int wsta_send_eth(const uint8_t* eth, int len) {
    if (S.state != WSTA_UP || len < 14) return -1;
    return send_data(eth, (uint16_t)((eth[12] << 8) | eth[13]), eth + 14, len - 14, 1) ? -1 : 0;
}

// ---- EAPOL-Key ----------------------------------------------------------------------------------------------------------------------
// Offsets from the start of the EAPOL header.
#define K_INFO 5
#define K_LEN 7
#define K_REPLAY 9
#define K_NONCE 17
#define K_RSC 65
#define K_MIC 81
#define K_DLEN 97
#define K_DATA 99

static void eapol_mic(const uint8_t* kck, uint8_t* frame, int total, uint8_t out[16]) {
    uint8_t save[16], h[20];
    memcpy(save, frame + K_MIC, 16);
    memset(frame + K_MIC, 0, 16);
    wc_hmac_sha1(kck, 16, frame, (uint32_t)total, h);
    memcpy(frame + K_MIC, save, 16);
    memcpy(out, h, 16);
}

static void send_eapol_key(uint16_t info, const uint8_t* replay, const uint8_t* nonce, const uint8_t* data, int dlen) {
    uint8_t f[256]; int total = 99 + dlen;
    memset(f, 0, sizeof f);
    f[0] = 2; f[1] = 3; f[2] = (uint8_t)((total - 4) >> 8); f[3] = (uint8_t)(total - 4);
    f[4] = 2;
    f[K_INFO] = (uint8_t)(info >> 8); f[K_INFO + 1] = (uint8_t)info;
    memcpy(f + K_REPLAY, replay, 8);
    if (nonce) memcpy(f + K_NONCE, nonce, 32);
    f[K_DLEN] = (uint8_t)(dlen >> 8); f[K_DLEN + 1] = (uint8_t)dlen;
    if (dlen) memcpy(f + K_DATA, data, (uint32_t)dlen);
    if (info & 0x100) { uint8_t mic[16]; eapol_mic(S.ptk, f, total, mic); memcpy(f + K_MIC, mic, 16); }
    send_data(S.bssid, 0x888E, f, total, 0);
}

static void install_keys_done(void);

static void handle_eapol(const uint8_t* e, int len) {
    if (len < 99 || e[1] != 3 || e[4] != 2) return;
    int blen = (e[2] << 8) | e[3];
    if (blen + 4 > len) return;
    int total = 4 + blen;
    uint16_t info = (uint16_t)((e[K_INFO] << 8) | e[K_INFO + 1]);
    int dlen = (e[K_DLEN] << 8) | e[K_DLEN + 1];
    if (total < 99 + dlen) return;
    if ((info & 7) != 2) return;                                              // HMAC-SHA1 MIC and AES key wrap only
    uint64_t rc = 0;
    for (int i = 0; i < 8; i++) rc = (rc << 8) | e[K_REPLAY + i];

    if ((info & 0x08) && (info & 0x80) && !(info & 0x100)) {                   // message 1 of 4: ANonce
        if (S.state != WSTA_HANDSHAKE) return;
        memcpy(S.anonce, e + K_NONCE, 32);
        O->random(S.snonce, 32);
        wc_ptk(S.psk, S.bssid, MAC, S.anonce, S.snonce, S.ptk);
        S.have_ptk = 1;
        S.last_replay = rc; S.have_replay = 1;
        send_eapol_key(0x010A, e + K_REPLAY, S.snonce, S.rsn, S.rsn_len);       // message 2 of 4: SNonce, MIC, our RSN element
        return;
    }
    if (!S.have_ptk) return;
    // everything else carries a MIC
    uint8_t copy[256], mic[16];
    if (total > (int)sizeof copy) return;
    memcpy(copy, e, (uint32_t)total);
    eapol_mic(S.ptk, copy, total, mic);
    if (memcmp(mic, e + K_MIC, 16)) { S.bad++; LOG("wifi: handshake MIC mismatch (wrong pass phrase?)"); return; }
    if (S.have_replay && rc <= S.last_replay) return;                           // replayed
    S.last_replay = rc; S.have_replay = 1;

    uint8_t kd[200];
    int kdl = dlen;
    if (info & 0x1000) {                                                       // key data is wrapped with the KEK
        if (dlen < 24 || (dlen & 7) || dlen > (int)sizeof kd + 8) return;
        if (wc_aes_unwrap(S.ptk + 16, e + K_DATA, (uint32_t)(dlen / 8 - 1), kd)) { S.bad++; return; }
        kdl = dlen - 8;
    } else {
        if (dlen > (int)sizeof kd) return;
        memcpy(kd, e + K_DATA, (uint32_t)dlen);
    }
    // look for the group key in the key data (KDE 00-0f-ac:1)
    for (int p = 0; p + 2 <= kdl;) {
        int id = kd[p], l = kd[p + 1];
        if (id == 0 && l == 0) break;                                          // padding
        if (p + 2 + l > kdl) break;
        if (id == 0xdd && l >= 6 + 16 && kd[p + 2] == 0 && kd[p + 3] == 0x0f && kd[p + 4] == 0xac && kd[p + 5] == 1) {
            S.gtk_id = kd[p + 6] & 3;
            memcpy(S.gtk, kd + p + 8, 16);
            S.have_gtk = 1;
            memset(S.rx_pn + 8, 0, 8);
        }
        p += 2 + l;
    }
    if ((info & 0x08)) {                                                       // pairwise: message 3 of 4
        if (!(info & 0x80) || !(info & 0x40)) return;                          // must ask for installation
        if (memcmp(e + K_NONCE, S.anonce, 32)) { S.bad++; return; }            // the ANonce must not have changed
        send_eapol_key(0x030A, e + K_REPLAY, 0, 0, 0);                         // message 4 of 4 (sent before the key is in use)
        if (!S.ptk_installed) { S.ptk_installed = 1; S.tx_pn = 0; memset(S.rx_pn, 0, sizeof S.rx_pn); }
        if (S.have_gtk && S.state == WSTA_HANDSHAKE) install_keys_done();
    } else {                                                                   // group key handshake, message 1 of 2
        send_eapol_key(0x0302, e + K_REPLAY, 0, 0, 0);
        LOG("wifi: group key renewed");
    }
}

static void install_keys_done(void) {
    S.state = WSTA_UP;
    S.t_start = now();
    LOG("wifi: connected to %s, aid %d", S.ssid, S.aid);
}

// ---- state machine ------------------------------------------------------------------------------------------------------------------
static void fail(const char* why) {
    LOG("wifi: %s", why);
    if (S.state >= WSTA_AUTH && S.state != WSTA_FAILED) O->link(0, S.bssid, 0);
    S.state = WSTA_FAILED;
    S.ptk_installed = 0; S.have_ptk = 0; S.have_gtk = 0;
}

int wsta_connect(const char* ssid, const char* pass) {
    int best = -1;
    uint32_t sl = (uint32_t)strlen(ssid);
    for (int i = 0; i < MAXBSS; i++) {
        if (!L[i].used || L[i].ssid_len != sl || memcmp(L[i].ssid, ssid, sl)) continue;
        if (best < 0 || L[i].rssi > L[best].rssi) best = i;
    }
    if (best < 0) { LOG("wifi: %s was not heard in the last scan", ssid); return -1; }
    struct wsta_bss* b = &L[best];
    int open = !pass || !pass[0];
    if (open && b->sec != 0) { LOG("wifi: %s needs a pass phrase", ssid); return -2; }
    if (!open && b->sec != 3) { LOG("wifi: %s is not WPA2-Personal with AES (security class %d)", ssid, b->sec); return -3; }
    if (S.state != WSTA_IDLE && S.state != WSTA_FAILED) wsta_disconnect();
    memset(&S, 0, sizeof S);
    memcpy(S.ssid, ssid, sl); S.ssid_len = (uint8_t)sl;
    S.open = open;
    if (!open) wc_wpa_psk(pass, (const uint8_t*)ssid, sl, S.psk);
    S.target = *b;
    memcpy(S.bssid, b->bssid, 6);
    LOG("wifi: joining %s on channel %d (%s)", ssid, b->ch, open ? "open" : "WPA2");
    O->set_channel(b->ch);
    S.state = WSTA_AUTH; S.t_start = now(); S.tries = 1;
    send_auth();
    S.t_last = now();
    return 0;
}

void wsta_disconnect(void) {
    if (S.state >= WSTA_ASSOC && S.state <= WSTA_UP) send_deauth();
    if (S.state >= WSTA_AUTH && S.state != WSTA_FAILED) O->link(0, S.bssid, 0);
    S.state = WSTA_IDLE;
    S.ptk_installed = 0; S.have_ptk = 0; S.have_gtk = 0;
}

void wsta_poll(void) {
    uint32_t t = now();
    switch (S.state) {
    case WSTA_AUTH:
        if (t - S.t_last >= 300) { if (++S.tries > 4) { fail("no answer to the authentication request"); break; } send_auth(); S.t_last = t; }
        break;
    case WSTA_ASSOC:
        if (t - S.t_last >= 300) { if (++S.tries > 4) { fail("no answer to the association request"); break; } send_assoc(); S.t_last = t; }
        break;
    case WSTA_HANDSHAKE:
        if (t - S.t_start > 5000) fail("the key handshake did not finish (wrong pass phrase?)");
        break;
    case WSTA_UP:
        if (t - S.last_tx_ms > 20000) {                                         // keep-alive: a null data frame
            uint8_t f[24];
            memset(f, 0, 24);
            f[0] = 0x48; f[1] = 0x01;
            memcpy(f + 4, S.bssid, 6); memcpy(f + 10, MAC, 6); memcpy(f + 16, S.bssid, 6);
            O->tx(f, 24, 1);
            S.last_tx_ms = t;
        }
        break;
    default: break;
    }
}

// ---- receive ----------------------------------------------------------------------------------------------------------------------------
static int ccmp_rx(const uint8_t* f, int hl, int len, int qos, int tid, uint8_t* out) {
    // f: whole frame, hl: header length; payload after: CCMP header (8) + data + MIC (8)
    if (len < hl + 8 + 8) return -1;
    const uint8_t* ch = f + hl;
    if (!(ch[3] & 0x20)) return -1;                                           // ExtIV must be set
    int keyid = ch[3] >> 6;
    uint64_t pn = (uint64_t)ch[0] | ((uint64_t)ch[1] << 8) | ((uint64_t)ch[4] << 16) | ((uint64_t)ch[5] << 24) | ((uint64_t)ch[6] << 32) | ((uint64_t)ch[7] << 40);
    int group = f[4] & 1;
    const uint8_t* key;
    uint64_t* last;
    if (group) { if (!S.have_gtk || keyid != S.gtk_id) return -2; key = S.gtk; last = &S.rx_pn[8]; }
    else { if (!S.ptk_installed) return -2; key = S.ptk + 32; last = &S.rx_pn[tid & 7]; }
    if (pn <= *last) return -3;                                                // replay
    uint8_t aad[24], nonce[13];
    aad[0] = (uint8_t)(f[0] & 0x8f); aad[1] = (uint8_t)((f[1] & 0xc7) | 0x40);
    memcpy(aad + 2, f + 4, 18);
    aad[20] = (uint8_t)(f[22] & 0x0f); aad[21] = 0;
    int aadlen = 22;
    if (qos) { aad[1] &= 0x7f; aad[22] = (uint8_t)(f[24] & 0x0f); aad[23] = 0; aadlen = 24; }      // QoS: the order bit is masked, the TID joins the AAD
    nonce[0] = qos ? (uint8_t)(f[24] & 0x0f) : 0; memcpy(nonce + 1, f + 10, 6);
    nonce[7] = ch[7]; nonce[8] = ch[6]; nonce[9] = ch[5]; nonce[10] = ch[4]; nonce[11] = ch[1]; nonce[12] = ch[0];
    int dl = len - hl - 8 - 8;
    if (wc_ccm_decrypt(key, nonce, aad, (uint32_t)aadlen, f + hl + 8, (uint32_t)dl, f + len - 8, out)) return -4;
    *last = pn;
    return dl;
}

static void rx_data(const uint8_t* f, int len) {
    int sub = f[0] >> 4, qos = sub & 8;
    if (sub == 4 || sub == 12) return;                                         // null functions
    int from_ds = f[1] & 2, to_ds = f[1] & 1;
    if (!from_ds || to_ds) return;
    if (memcmp(f + 10, S.bssid, 6)) return;                                    // not from our AP
    if (!memcmp(f + 16, MAC, 6)) return;                                       // our own broadcast, reflected by the AP
    int hl = 24 + (qos ? 2 : 0);
    if ((f[1] & 0x80) && qos) hl += 4;                                         // HT control field
    if (len < hl + 8) return;
    static uint8_t clear[2400];
    const uint8_t* body;
    int blen;
    int tid = qos ? (f[24] & 7) : 0;
    if (f[1] & 0x40) {
        int r = ccmp_rx(f, hl, len, qos, tid, clear);
        if (r < 0) { S.bad++; return; }
        body = clear; blen = r;
    } else {
        if (S.ptk_installed && !((f[hl + 6] == 0x88 && f[hl + 7] == 0x8e))) { S.bad++; return; }      // after the handshake only the key exchange may be in the clear
        body = f + hl; blen = len - hl;
    }
    if (blen < 8 || body[0] != 0xaa || body[1] != 0xaa || body[2] != 3) return;
    uint16_t type = (uint16_t)((body[6] << 8) | body[7]);
    S.rx_frames++;
    if (type == 0x888E) { handle_eapol(body + 8, blen - 8); return; }
    if (S.state != WSTA_UP) return;
    static uint8_t eth[2400];
    memcpy(eth, f + 4, 6);                                                     // DA = receiver address
    memcpy(eth + 6, f + 16, 6);                                                // SA
    eth[12] = body[6]; eth[13] = body[7];
    memcpy(eth + 14, body + 8, (uint32_t)(blen - 8));
    O->eth_rx(eth, 14 + blen - 8);
}

void wsta_rx(const uint8_t* f, int len, int rssi, int channel) {
    if (len < 24) return;
    int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type == 0) {
        if (sub == 8 || sub == 5) {                                            // beacon / probe response
            if (len < 36) return;
            struct wsta_bss* b = bss_find(f + 16);
            b->seen = now();
            b->rssi = (int8_t)rssi;
            b->ch = (uint8_t)channel;
            parse_ies(b, f + 36, len - 36, rd16(f + 34));
            return;
        }
        if (S.state < WSTA_AUTH || S.state == WSTA_FAILED || memcmp(f + 16, S.bssid, 6)) return;
        if (sub == 11 && S.state == WSTA_AUTH && len >= 30) {                  // authentication response
            if (rd16(f + 24) != 0 || rd16(f + 26) != 2) return;
            if (rd16(f + 28) != 0) { LOG("wifi: authentication refused (status %d)", rd16(f + 28)); fail("authentication refused"); return; }
            S.state = WSTA_ASSOC; S.tries = 1; S.t_start = now();
            send_assoc(); S.t_last = now();
        } else if ((sub == 1 || sub == 3) && S.state == WSTA_ASSOC && len >= 30) {   // association / reassociation response
            if (rd16(f + 26) != 0) { LOG("wifi: association refused (status %d)", rd16(f + 26)); fail("association refused"); return; }
            S.aid = rd16(f + 28) & 0x3fff;
            O->link(1, S.bssid, S.aid);
            if (S.open) { S.state = WSTA_UP; LOG("wifi: connected to %s (open), aid %d", S.ssid, S.aid); return; }
            S.state = WSTA_HANDSHAKE; S.t_start = now();
        } else if (sub == 12 || sub == 10) {                                   // deauthentication / disassociation
            LOG("wifi: the access point ended the connection (reason %d)", len >= 26 ? rd16(f + 24) : 0);
            O->link(0, S.bssid, 0);
            S.state = WSTA_FAILED; S.ptk_installed = 0; S.have_ptk = 0; S.have_gtk = 0;
        }
        return;
    }
    if (type == 2 && (S.state == WSTA_HANDSHAKE || S.state == WSTA_UP)) rx_data(f, len);
}
