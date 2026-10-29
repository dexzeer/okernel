// check: the multibyte CJK decoders (GBK / Shift_JIS / Big5 / gb18030)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web/wdom.h"
#include "web/wcommon.h"

static int check(const char* name, const char* bytes, int n, const char* label, const char* want) {
    int out_len = 0;
    char used[32];
    char* s = wcharset_decode(bytes, n, label, &out_len, used, sizeof used);
    int ok = (int)strlen(want) == out_len && !memcmp(s, want, out_len);
    printf("%s %s (used %s)%s\n", ok ? "PASS" : "FAIL", name, used, ok ? "" : "");
    if (!ok) {
        printf("  want %d bytes: ", (int)strlen(want));
        for (const char* p = want; *p; p++) printf("%02x ", (unsigned char)*p);
        printf("\n  got  %d bytes: ", out_len);
        for (int i = 0; i < out_len; i++) printf("%02x ", (unsigned char)s[i]);
        printf("\n");
    }
    free(s);
    return ok;
}

int main(void) {
    int ok = 1;
    struct wbuf b = {0, 0, 0};
    // GBK: 你好 (C4 E3 BA C3), 全角逗号 (A3 AC)
    const char* want_gbk = "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c";
    ok &= check("gbk 你好，世界", "\xc4\xe3\xba\xc3\xa3\xac\xca\xc0\xbd\xe7", 10, "gbk", want_gbk);
    // Shift_JIS: 日本語 (93 FA 96 7B 8C EA), halfwidth ｱ (B1 = U+FF71)
    const char* want_sjis = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xef\xbd\xb1";
    ok &= check("sjis 日本語ｱ", "\x93\xfa\x96\x7b\x8c\xea\xb1", 7, "shift_jis", want_sjis);
    // Big5: 繁體中文 (C1 63 C5 E9 A4 A4 A4 E5)
    const char* want_big5 = "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87";
    ok &= check("big5 繁體中文", "\xc1\x63\xc5\xe9\xa4\xa4\xa4\xe5", 8, "big5", want_big5);
    // gb18030 four-byte: 𠮷 (95 34 B2 35), euro (80)
    const char* want_gb4 = "\xf0\xa0\xae\xb7\xe2\x82\xac";
    ok &= check("gb18030 4byte+euro", "\x95\x34\xb2\x35\x80", 5, "gb18030", want_gb4);
    // invalid trail -> FFFD (C4 E3 = 你)
    const char* want_bad = "\xe4\xbd\xa0\xef\xbf\xbd";
    ok &= check("gbk invalid trail", "\xc4\xe3\xff", 3, "gbk", want_bad);
    (void)b;
    printf("%s\n", ok ? "ALLPASS" : "FAILED");
    return ok ? 0 : 1;
}
