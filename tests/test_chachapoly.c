#include <stdio.h>
#include <string.h>
#include "chacha20.h"
#include "poly1305.h"

static void hex(const char* l, const uint8_t* d, int n) {
    printf("%s", l);
    for (int i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

int main(void) {
    // RFC 8439 §2.4.2 ChaCha20 test vector
    {
        uint8_t key[32] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
            0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
        uint8_t nonce[12] = { 0,0,0,0,0,0,0,0x4a,0,0,0,0 };
        const char* pt = "Ladies and Gentlemen of the class of '99: "
                         "If I could offer you only one tip for the future, "
                         "sunscreen would be it.";
        uint8_t ct[114];
        chacha20_encrypt(key, nonce, 1, (const uint8_t*)pt, ct, 114);
        uint8_t expected[114];
        const char* ehex = "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf9"
                           "1b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca"
                           "0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf7"
                           "4a35be6b40b8eedf2785e42874d";
        for (int i = 0; i < 114; i++) {
            uint8_t ev = 0;
            char c = ehex[i*2]; ev = (c>='a'?c-'a'+10:c-'0')<<4;
            c = ehex[i*2+1]; ev |= (c>='a'?c-'a'+10:c-'0');
            expected[i] = ev;
        }
        int ok = 1;
        for (int i = 0; i < 114; i++) if (ct[i] != expected[i]) { ok = 0; break; }
        printf("ChaCha20: %s\n", ok ? "PASS" : "FAIL");
    }

    // Poly1305 test — RFC 8439 §2.5.2 vector (key must be THE RFC key;
    // the original test paired this tag with a different key)
    {
        uint8_t key[32] = {
            0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,
            0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
            0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,
            0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b };
        const char* msg = "Cryptographic Forum Research Group";
        uint8_t tag[16];
        poly1305_auth(key, (const uint8_t*)msg, strlen(msg), tag);
        uint8_t expect[] = { 0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,
                             0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9 };
        int ok = 1;
        for (int i = 0; i < 16; i++) if (tag[i] != expect[i]) { ok = 0; break; }
        printf("Poly1305 (RFC): %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { hex("got:   ", tag, 16); hex("expect:", expect, 16); }
    }

    // Poly1305 second vector — key 0x80..0x9f, true tag computed with an
    // independent bignum oracle (python). Exercises high-limb clamp paths.
    {
        uint8_t key[32];
        for (int i = 0; i < 32; i++) key[i] = 0x80 + i;
        const char* msg = "Cryptographic Forum Research Group";
        uint8_t tag[16];
        poly1305_auth(key, (const uint8_t*)msg, strlen(msg), tag);
        uint8_t expect[] = { 0xcb,0x2d,0xec,0xeb,0xc0,0x1f,0xb7,0x7d,
                             0x7b,0xd5,0xa1,0xaf,0x98,0x98,0xb0,0x0d };
        int ok = 1;
        for (int i = 0; i < 16; i++) if (tag[i] != expect[i]) { ok = 0; break; }
        printf("Poly1305 (0x80-key): %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { hex("got:   ", tag, 16); hex("expect:", expect, 16); }
    }

    // Round-trip: chacha20 is a stream cipher, decrypt == encrypt
    {
        uint8_t key[32]; uint8_t nonce[12];
        for (int i = 0; i < 32; i++) key[i] = i * 7 + 1;
        for (int i = 0; i < 12; i++) nonce[i] = i * 3;
        uint8_t pt[300], ct[300], rt[300];
        for (int i = 0; i < 300; i++) pt[i] = (uint8_t)(i * 11 + 3);
        chacha20_encrypt(key, nonce, 0, pt, ct, 300);
        chacha20_encrypt(key, nonce, 0, ct, rt, 300);
        int ok = 1;
        for (int i = 0; i < 300; i++) if (rt[i] != pt[i]) { ok = 0; break; }
        printf("ChaCha20 round-trip: %s\n", ok ? "PASS" : "FAIL");
    }
    return 0;
}
