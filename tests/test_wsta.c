// Host test for the 802.11 station (src/rtw/wsta.c) against a simulated WPA2-Personal access point: scan, authentication,
// association, the four-way handshake, encrypted data in both directions, replay protection, a group key renewal, a wrong
// pass phrase, an open network, de-authentication, timeouts and garbage input. The access point is written here from the
// authenticator side of the specification and shares only the primitives (verified separately in test_wcrypto) with the station.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "../src/rtw/wsta.h"
#include "../src/rtw/wcrypto.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- station services -----------------------------------------------------------------------------------------------
static uint32_t clock_ms;
static int chan_set, link_up, link_aid;
static uint8_t link_bssid[6];
static uint8_t eth_got[8][2400]; static int eth_len[8], eth_n;
static uint32_t rnd_ctr;
static void ap_rx(const uint8_t* f, int len);
static int tamper_at = -1;

static void s_log(const char* fmt, ...) {
    if (!getenv("WSTA_LOG")) return;
    va_list ap; va_start(ap, fmt); printf("  [sta] "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static int s_tx(const uint8_t* f, int len, int is_data) { (void)is_data; ap_rx(f, len); return 0; }
static void s_chan(int ch) { chan_set = ch; }
static void s_link(int up, const uint8_t* b, int aid) { link_up = up; link_aid = aid; memcpy(link_bssid, b, 6); }
static void s_eth(const uint8_t* e, int len) { if (eth_n < 8) { memcpy(eth_got[eth_n], e, (size_t)len); eth_len[eth_n++] = len; } }
static uint32_t s_now(void) { return clock_ms; }
static void s_rand(uint8_t* o, int n) { for (int i = 0; i < n; i++) o[i] = (uint8_t)(rnd_ctr++ * 37 + 11); }
static const struct wsta_ops OPS = { s_tx, s_chan, s_link, s_eth, s_now, s_rand, s_log };

static void advance(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) { clock_ms += 10; wsta_poll(); } }

// ---- the access point ---------------------------------------------------------------------------------------------------
static const uint8_t STA[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t AP[6] = { 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 };
static struct {
    char ssid[33], pass[64];
    int open, channel, mute, drop_msg3_mic;
    uint8_t pmk[32], anonce[32], snonce[32], ptk[48], gtk[16];
    int gtk_id, aid_next;
    uint64_t replay;
    int got_assoc, got_msg2, got_msg4, assoc_rsn_len;
    uint8_t assoc_rsn[32];
    uint64_t tx_pn, gtk_pn;
    uint64_t rx_pn;
    uint8_t rx_eth[2400]; int rx_eth_len; int rx_frames_ok, rx_frames_bad;
    uint8_t last_enc[2400]; int last_enc_len;
    uint16_t seq;
} A;

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint8_t RSN[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

static int beacon(uint8_t* f, int ch) {
    int n = 0;
    memset(f, 0, 24);
    f[0] = 0x80; memset(f + 4, 0xff, 6); memcpy(f + 10, AP, 6); memcpy(f + 16, AP, 6);
    n = 24 + 8;                                                  // timestamp
    put16(f + n, 100); put16(f + n + 2, A.open ? 0x0401 : 0x0411); n += 4;
    f[n++] = 0; f[n++] = (uint8_t)strlen(A.ssid); memcpy(f + n, A.ssid, strlen(A.ssid)); n += (int)strlen(A.ssid);
    f[n++] = 1; f[n++] = 2; f[n++] = 0x8c; f[n++] = 0x12;
    f[n++] = 3; f[n++] = 1; f[n++] = (uint8_t)ch;
    if (!A.open) { memcpy(f + n, RSN, sizeof RSN); n += (int)sizeof RSN; }
    return n;
}

static void mgmt_to_sta(uint8_t fc0, const uint8_t* body, int blen) {
    uint8_t f[300];
    memset(f, 0, 24);
    f[0] = fc0; memcpy(f + 4, STA, 6); memcpy(f + 10, AP, 6); memcpy(f + 16, AP, 6);
    memcpy(f + 24, body, (size_t)blen);
    wsta_rx(f, 24 + blen, -45, A.channel);
}

// a data frame from the AP to the station. qos selects the QoS header; unicast uses the PTK, otherwise the GTK.
static void data_to_sta(const uint8_t* da, const uint8_t* sa, uint16_t type, const uint8_t* payload, int plen, int enc, int qos, int unicast, uint64_t force_pn) {
    static uint8_t f[2400], clear[2400];
    int hl = 24 + (qos ? 2 : 0);
    memset(f, 0, 32);
    f[0] = qos ? 0x88 : 0x08; f[1] = (uint8_t)(0x02 | (enc ? 0x40 : 0));
    memcpy(f + 4, da, 6); memcpy(f + 10, AP, 6); memcpy(f + 16, sa, 6);
    A.seq++; put16(f + 22, (uint16_t)(A.seq << 4));
    if (qos) { f[24] = 3; f[25] = 0; }                           // TID 3
    uint8_t llc[8] = { 0xaa, 0xaa, 3, 0, 0, 0, (uint8_t)(type >> 8), (uint8_t)type };
    memcpy(clear, llc, 8); memcpy(clear + 8, payload, (size_t)plen);
    int n = hl;
    if (!enc) { memcpy(f + n, clear, (size_t)plen + 8); n += plen + 8; }
    else {
        uint64_t pn = force_pn ? force_pn : (unicast ? ++A.tx_pn : ++A.gtk_pn);
        f[n] = (uint8_t)pn; f[n + 1] = (uint8_t)(pn >> 8); f[n + 2] = 0; f[n + 3] = (uint8_t)(0x20 | (unicast ? 0 : (A.gtk_id << 6)));
        f[n + 4] = (uint8_t)(pn >> 16); f[n + 5] = (uint8_t)(pn >> 24); f[n + 6] = (uint8_t)(pn >> 32); f[n + 7] = (uint8_t)(pn >> 40);
        uint8_t aad[24], nonce[13];
        aad[0] = f[0] & 0x8f; aad[1] = (uint8_t)(f[1] & 0xc7);
        if (qos) aad[1] &= 0x7f;
        aad[1] |= 0x40;
        memcpy(aad + 2, f + 4, 18);
        aad[20] = f[22] & 0x0f; aad[21] = 0;
        int al = 22;
        if (qos) { aad[22] = f[24] & 0x0f; aad[23] = 0; al = 24; }
        nonce[0] = qos ? (f[24] & 0x0f) : 0; memcpy(nonce + 1, AP, 6);
        nonce[7] = (uint8_t)(pn >> 40); nonce[8] = (uint8_t)(pn >> 32); nonce[9] = (uint8_t)(pn >> 24); nonce[10] = (uint8_t)(pn >> 16); nonce[11] = (uint8_t)(pn >> 8); nonce[12] = (uint8_t)pn;
        uint8_t mic[8];
        wc_ccm_encrypt(unicast ? A.ptk + 32 : A.gtk, nonce, aad, (uint32_t)al, clear, (uint32_t)plen + 8, f + n + 8, mic);
        n += 8 + plen + 8;
        memcpy(f + n, mic, 8); n += 8;
    }
    memcpy(A.last_enc, f, (size_t)n); A.last_enc_len = n;
    if (tamper_at >= 0) f[tamper_at] ^= 1;
    wsta_rx(f, n, -45, A.channel);
}

static void eapol_to_sta(uint16_t info, uint64_t rc, const uint8_t* nonce, const uint8_t* kd, int kdl, int wrap) {
    uint8_t e[300], data[200];
    int dl = kdl;
    if (wrap) {
        uint8_t padded[200];
        memcpy(padded, kd, (size_t)kdl);
        int pl = kdl;
        if (pl % 8 || pl < 16) { padded[pl++] = 0xdd; while (pl % 8 || pl < 16) padded[pl++] = 0; }
        wc_aes_wrap(A.ptk + 16, padded, (uint32_t)(pl / 8), data);
        dl = pl + 8;
    } else if (kdl) memcpy(data, kd, (size_t)kdl);
    int total = 99 + dl;
    memset(e, 0, sizeof e);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)((total - 4) >> 8); e[3] = (uint8_t)(total - 4); e[4] = 2;
    e[5] = (uint8_t)(info >> 8); e[6] = (uint8_t)info; e[8] = 16;
    for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(rc >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, 32);
    e[97] = (uint8_t)(dl >> 8); e[98] = (uint8_t)dl;
    if (dl) memcpy(e + 99, data, (size_t)dl);
    if (info & 0x100) {
        uint8_t h[20];
        wc_hmac_sha1(A.ptk, 16, e, (uint32_t)total, h);
        if (A.drop_msg3_mic) h[0] ^= 0xff;
        memcpy(e + 81, h, 16);
    }
    data_to_sta(STA, AP, 0x888E, e, total, 0, 0, 1, 0);
}

static void send_msg1(void) {
    for (int i = 0; i < 32; i++) A.anonce[i] = (uint8_t)(0x40 + i * 3);
    A.replay++;
    eapol_to_sta(0x008A, A.replay, A.anonce, 0, 0, 0);
}

static void send_msg3(void) {
    uint8_t kd[80]; int n = 0;
    memcpy(kd, RSN, sizeof RSN); n += (int)sizeof RSN;
    kd[n++] = 0xdd; kd[n++] = 22; kd[n++] = 0; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 1; kd[n++] = (uint8_t)(A.gtk_id | 4); kd[n++] = 0;
    memcpy(kd + n, A.gtk, 16); n += 16;
    A.replay++;
    eapol_to_sta(0x13CA, A.replay, A.anonce, kd, n, 1);
}

static void handle_eapol_from_sta(const uint8_t* e, int len) {
    if (len < 99) return;
    uint16_t info = (uint16_t)((e[5] << 8) | e[6]);
    uint8_t mic[20], copy[300];
    memcpy(copy, e, (size_t)len); memset(copy + 81, 0, 16);
    if (!A.got_msg2) {                                           // expect message 2
        memcpy(A.snonce, e + 17, 32);
        wc_ptk(A.pmk, AP, STA, A.anonce, A.snonce, A.ptk);
        wc_hmac_sha1(A.ptk, 16, copy, (uint32_t)len, mic);
        if (memcmp(mic, e + 81, 16)) { A.rx_frames_bad++; return; }          // wrong pass phrase on the station side
        int dl = (e[97] << 8) | e[98];
        A.assoc_rsn_len = dl;
        if (dl != (int)sizeof RSN || memcmp(e + 99, RSN, sizeof RSN) || (info != 0x010A)) { A.rx_frames_bad++; return; }
        A.got_msg2 = 1;
        send_msg3();
    } else {
        wc_hmac_sha1(A.ptk, 16, copy, (uint32_t)len, mic);
        if (memcmp(mic, e + 81, 16)) { A.rx_frames_bad++; return; }
        if (info == 0x030A) A.got_msg4 = 1;                      // message 4 (pairwise, MIC, secure)
        else if (info == 0x0302) A.got_msg4 = 2;                 // group key message 2
    }
}

static void ap_rx(const uint8_t* f, int len) {
    if (A.mute || len < 24) return;
    int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type == 0) {
        if (memcmp(f + 4, AP, 6)) return;
        if (sub == 11) { uint8_t b[6]; put16(b, 0); put16(b + 2, 2); put16(b + 4, 0); mgmt_to_sta(0xB0, b, 6); }
        else if (sub == 0) {
            A.got_assoc = 1;
            uint8_t b[16]; put16(b, 0x0411); put16(b + 2, 0); put16(b + 4, (uint16_t)(0xc000 | (A.aid_next ? A.aid_next : 1))); b[6] = 1; b[7] = 2; b[8] = 0x8c; b[9] = 0x12;
            mgmt_to_sta(0x10, b, 10);
            if (!A.open) send_msg1();
        } else if (sub == 12) A.got_assoc = 0;
        return;
    }
    if (type != 2) return;
    if (memcmp(f + 4, AP, 6) || memcmp(f + 10, STA, 6)) return;
    int sub2 = f[0] >> 4;
    if (sub2 == 4) return;
    if (f[1] & 0x40) {                                           // CCMP from the station
        if (len < 24 + 16) { A.rx_frames_bad++; return; }
        uint64_t pn = (uint64_t)f[24] | ((uint64_t)f[25] << 8) | ((uint64_t)f[28] << 16) | ((uint64_t)f[29] << 24) | ((uint64_t)f[30] << 32) | ((uint64_t)f[31] << 40);
        uint8_t aad[22], nonce[13];
        aad[0] = f[0] & 0x8f; aad[1] = (uint8_t)((f[1] & 0xc7) | 0x40);
        memcpy(aad + 2, f + 4, 18); aad[20] = f[22] & 0x0f; aad[21] = 0;
        nonce[0] = 0; memcpy(nonce + 1, f + 10, 6);
        nonce[7] = f[31]; nonce[8] = f[30]; nonce[9] = f[29]; nonce[10] = f[28]; nonce[11] = f[25]; nonce[12] = f[24];
        static uint8_t clear[2400];
        int dl = len - 24 - 8 - 8;
        if (pn <= A.rx_pn || wc_ccm_decrypt(A.ptk + 32, nonce, aad, 22, f + 32, (uint32_t)dl, f + len - 8, clear)) { A.rx_frames_bad++; return; }
        A.rx_pn = pn;
        if (dl < 8) return;
        A.rx_frames_ok++;
        memcpy(A.rx_eth, f + 16, 6);                             // DA
        memcpy(A.rx_eth + 6, f + 10, 6);                         // SA
        A.rx_eth[12] = clear[6]; A.rx_eth[13] = clear[7];
        memcpy(A.rx_eth + 14, clear + 8, (size_t)dl - 8);
        A.rx_eth_len = 14 + dl - 8;
    } else {
        if (len >= 24 + 8 && f[24 + 6] == 0x88 && f[24 + 7] == 0x8e) handle_eapol_from_sta(f + 32, len - 32);
    }
}

static void ap_config(const char* ssid, const char* pass, int ch) {
    memset(&A, 0, sizeof A);
    strcpy(A.ssid, ssid); if (pass) strcpy(A.pass, pass);
    A.open = !pass; A.channel = ch; A.aid_next = 7; A.gtk_id = 1;
    for (int i = 0; i < 16; i++) A.gtk[i] = (uint8_t)(0xa0 + i);
    if (pass) wc_wpa_psk(pass, (const uint8_t*)ssid, (uint32_t)strlen(ssid), A.pmk);
}

static void announce(void) { uint8_t b[300]; int n = beacon(b, A.channel); wsta_rx(b, n, -45, A.channel); }

static void fresh(void) { eth_n = 0; link_up = 0; chan_set = 0; clock_ms = 1000; wsta_init(STA, &OPS); }

int main(void) {
    // --- a WPA2 network, right pass phrase ---
    ap_config("testnet", "correct horse battery", 6);
    fresh();
    announce();
    CHECK(wsta_bss_count() == 1);
    const struct wsta_bss* b = wsta_bss_at(0);
    CHECK(b && b->sec == 3 && b->ch == 6 && !strcmp(b->ssid, "testnet") && b->rssi == -45);
    CHECK(wsta_connect("nonexistent", "x") < 0);
    CHECK(wsta_connect("testnet", "correct horse battery") == 0);
    CHECK(chan_set == 6);
    CHECK(wsta_state() == WSTA_UP);
    CHECK(link_up == 1 && link_aid == 7 && !memcmp(link_bssid, AP, 6));
    CHECK(A.got_assoc && A.got_msg2 && A.got_msg4 == 1);
    CHECK(!memcmp(A.ptk + 32, A.ptk + 32, 16));

    // station -> AP: encrypted data
    uint8_t eth[200];
    memcpy(eth, AP, 6); memcpy(eth + 6, STA, 6); eth[12] = 0x08; eth[13] = 0x06;
    for (int i = 0; i < 42; i++) eth[14 + i] = (uint8_t)(i * 5);
    CHECK(wsta_send_eth(eth, 14 + 42) == 0);
    CHECK(A.rx_frames_ok == 1 && A.rx_eth_len == 14 + 42 && !memcmp(A.rx_eth + 12, eth + 12, 2 + 42));
    CHECK(wsta_send_eth(eth, 14 + 42) == 0 && A.rx_frames_ok == 2);          // the packet number moves on
    uint8_t big[1400];
    memcpy(big, AP, 6); memcpy(big + 6, STA, 6); big[12] = 0x08; big[13] = 0x00;
    for (int i = 14; i < 1400; i++) big[i] = (uint8_t)(i * 7);
    CHECK(wsta_send_eth(big, 1400) == 0 && A.rx_eth_len == 1400 && !memcmp(A.rx_eth + 14, big + 14, 1386));

    // AP -> station: unicast (non-QoS and QoS), broadcast with the group key
    uint8_t pay[300];
    for (int i = 0; i < 300; i++) pay[i] = (uint8_t)(i ^ 0x5a);
    const uint8_t peer[6] = { 0xde, 0xad, 0xbe, 0xef, 0, 1 };
    data_to_sta(STA, peer, 0x0800, pay, 100, 1, 0, 1, 0);
    CHECK(eth_n == 1 && eth_len[0] == 114 && !memcmp(eth_got[0], STA, 6) && !memcmp(eth_got[0] + 6, peer, 6) && eth_got[0][12] == 8 && !memcmp(eth_got[0] + 14, pay, 100));
    data_to_sta(STA, peer, 0x0800, pay, 300, 1, 1, 1, 0);
    CHECK(eth_n == 2 && eth_len[1] == 314 && !memcmp(eth_got[1] + 14, pay, 300));
    const uint8_t bc[6] = { 255, 255, 255, 255, 255, 255 };
    data_to_sta(bc, peer, 0x0806, pay, 28, 1, 0, 0, 0);
    CHECK(eth_n == 3 && !memcmp(eth_got[2], bc, 6) && eth_len[2] == 42 && !memcmp(eth_got[2] + 14, pay, 28));
    data_to_sta(bc, peer, 0x0806, pay, 28, 1, 1, 0, 0);
    CHECK(eth_n == 4);
    // replays and forgeries
    uint8_t saved[2400]; int slen = A.last_enc_len; memcpy(saved, A.last_enc, (size_t)slen);
    wsta_rx(saved, slen, -45, 6);
    CHECK(eth_n == 4);                                                        // the same encrypted frame again
    data_to_sta(STA, peer, 0x0800, pay, 50, 1, 0, 1, 1);                      // packet number 1 on TID 0 was already used
    CHECK(eth_n == 4);
    data_to_sta(STA, peer, 0x0800, pay, 50, 0, 0, 1, 0);                      // unencrypted data after the handshake
    CHECK(eth_n == 4);
    data_to_sta(STA, peer, 0x0800, pay, 50, 1, 0, 1, 0);                      // a fresh one is accepted
    CHECK(eth_n == 5);
    tamper_at = 40;                                                           // a flipped ciphertext bit must fail the MIC
    data_to_sta(STA, peer, 0x0800, pay, 60, 1, 0, 1, 0);
    CHECK(eth_n == 5);
    tamper_at = 5;                                                            // a changed header address is covered by the MIC too
    data_to_sta(STA, peer, 0x0800, pay, 60, 1, 0, 1, 0);
    CHECK(eth_n == 5);
    tamper_at = -1;
    data_to_sta(STA, peer, 0x0800, pay, 60, 1, 0, 1, 0);
    CHECK(eth_n == 6);
    uint32_t bad0, tx, rx; wsta_stats(&tx, &rx, &bad0);
    CHECK(bad0 >= 3);

    // group key renewal
    uint8_t newgtk[16]; for (int i = 0; i < 16; i++) newgtk[i] = (uint8_t)(0x30 + i);
    uint8_t oldgtk[16]; memcpy(oldgtk, A.gtk, 16);
    {
        uint8_t kd[24]; int n = 0;
        kd[n++] = 0xdd; kd[n++] = 22; kd[n++] = 0; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 1; kd[n++] = 2 | 4; kd[n++] = 0; memcpy(kd + n, newgtk, 16); n += 16;
        A.replay++; A.got_msg4 = 0; A.gtk_id = 2; memcpy(A.gtk, newgtk, 16); A.gtk_pn = 0;
        eapol_to_sta(0x1382, A.replay, 0, kd, n, 1);
    }
    CHECK(A.got_msg4 == 2);                                                   // the station answered with the group message 2
    eth_n = 0;
    data_to_sta(bc, peer, 0x0806, pay, 28, 1, 0, 0, 0);
    CHECK(eth_n == 1);
    { uint8_t save2[16]; memcpy(save2, A.gtk, 16); memcpy(A.gtk, oldgtk, 16); A.gtk_id = 1; data_to_sta(bc, peer, 0x0806, pay, 28, 1, 0, 0, 0); CHECK(eth_n == 1); memcpy(A.gtk, save2, 16); A.gtk_id = 2; }

    // de-authentication
    { uint8_t b2[2]; put16(b2, 7); mgmt_to_sta(0xC0, b2, 2); }
    CHECK(wsta_state() == WSTA_FAILED && link_up == 0);
    CHECK(wsta_send_eth(eth, 56) < 0);

    // --- wrong pass phrase: the AP rejects the station's MIC, the handshake times out ---
    ap_config("testnet", "correct horse battery", 6);
    fresh(); announce();
    CHECK(wsta_connect("testnet", "wrong") == 0);
    CHECK(wsta_state() == WSTA_HANDSHAKE && !A.got_msg2 && A.rx_frames_bad == 1);
    advance(6000);
    CHECK(wsta_state() == WSTA_FAILED);

    // --- the AP's message 3 carries a bad MIC: the station must not complete ---
    ap_config("testnet", "correct horse battery", 6);
    A.drop_msg3_mic = 1;
    fresh(); announce();
    CHECK(wsta_connect("testnet", "correct horse battery") == 0);
    CHECK(wsta_state() == WSTA_HANDSHAKE && !A.got_msg4);
    advance(6000);
    CHECK(wsta_state() == WSTA_FAILED);

    // --- an open network ---
    ap_config("cafe wifi", 0, 11);
    fresh(); announce();
    CHECK(wsta_bss_at(0)->sec == 0);
    CHECK(wsta_connect("cafe wifi", "pw") < 0);                               // does not take a pass phrase
    CHECK(wsta_connect("cafe wifi", "") == 0);
    CHECK(wsta_state() == WSTA_UP && chan_set == 11);

    // --- silent AP: the attempt gives up after its retries ---
    ap_config("testnet", "correct horse battery", 6);
    fresh(); announce();
    A.mute = 1;
    CHECK(wsta_connect("testnet", "correct horse battery") == 0);
    CHECK(wsta_state() == WSTA_AUTH);
    advance(3000);
    CHECK(wsta_state() == WSTA_FAILED);

    // --- WPA3-only / PMF-required networks are marked unusable ---
    ap_config("testnet", "correct horse battery", 6);
    fresh();
    {
        uint8_t bb[300]; int n = beacon(bb, 6);
        bb[n - 2] = 0xc0;                                                      // RSN capabilities: management frame protection capable + required
        wsta_rx(bb, n, -50, 6);
        CHECK(wsta_bss_at(0)->sec == 4);
        CHECK(wsta_connect("testnet", "correct horse battery") < 0);
    }

    // --- hostile input: random frames and truncations of valid ones must never crash (ASan / UBSan) ---
    ap_config("testnet", "correct horse battery", 6);
    fresh(); announce();
    wsta_connect("testnet", "correct horse battery");
    static uint8_t fz[2400];
    uint32_t seed = 12345;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        int len = (int)((seed >> 8) % 400);
        for (int k = 0; k < len; k++) { seed = seed * 1664525u + 1013904223u; fz[k] = (uint8_t)(seed >> 16); }
        if (len > 24 && (seed & 3)) { fz[0] = (uint8_t)((seed >> 24) & 0xfc); memcpy(fz + 10, AP, 6); memcpy(fz + 16, AP, 6); if (seed & 8) fz[1] |= 2; }
        wsta_rx(fz, len, -50, 6);
        if ((i & 255) == 0) { clock_ms += 50; wsta_poll(); }
    }
    for (int cut = 0; cut < 120; cut++) { uint8_t b3[300]; int n = beacon(b3, 6); wsta_rx(b3, cut < n ? cut : n, -50, 6); }

    printf(fails ? "WSTA FAIL (%d)\n" : "WSTA PASS\n", fails);
    return fails != 0;
}
