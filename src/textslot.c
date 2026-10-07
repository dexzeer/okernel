// Unicode codepoint -> one-byte render slot for the 8x16 bitmap terminal /
// editor fonts (src/font_data.c + font8x16_ext). Split out of the retired
// cell-grid HTML renderer; the web engine (src/web) renders real TrueType.
#include "textslot.h"

// Map a Unicode codepoint to a one-byte render slot. ASCII passes through;
// \n and \t survive (preformatted blocks need them; every flowed consumer
// collapses or spaces them, and URL-bearing attributes strip them after
// decoding). Other controls collapse to space. Cyrillic А-я live at slots
// 0x80-0xBF; common symbols at 0xC0+ (matching font8x16_ext in graphics.c).
// Latin-1: lowercase à-ÿ at 0xD0-0xEE, capitals À Á Â Ã Ä Ç È É Ê Í Ñ Ó Ô Õ
// Ö Ú Ü at 0xEF-0xFF. Latin letters WITHOUT a slot transliterate to their
// ASCII base (readable, never '?'); symbols map to a lookalike or '?'.
static char html_map_cp(int cp) {    if (cp == '\n' || cp == '\t') return (char)cp;
    if (cp < 0x20) return ' '; // whitespace/controls collapse to space
    if (cp < 0x7F) return (char)cp;
    if (cp >= 0x0410 && cp < 0x0450) return (char)(cp - 0x0410 + 0x80);
    // Latin-1 lowercase à(U+00E0)-ÿ(U+00FF), skipping ÷(U+00F7 → '/').
    if (cp >= 0xE0 && cp < 0x100) {
        if (cp == 0xF7) return '/';
        int o = cp - 0xE0;
        if (cp > 0xF7) o--;
        return (char)(0xD0 + o);
    }
    // Case folds for scripts with contiguous capitals: Georgian Asomtavruli
    // (U+10A0-) folds to Mkhedruli (+0x30); Armenian capitals (U+0531-)
    // fold to lowercase (+0x30). Handled before the switch so the tables
    // below only list one case.
    if (cp >= 0x10A0 && cp <= 0x10C5) cp += 0x30;
    if (cp >= 0x531 && cp <= 0x556) cp += 0x30;
    // Arabic-Indic / Eastern Arabic-Indic digits → ASCII digits.
    if (cp >= 0x660 && cp <= 0x669) return (char)('0' + cp - 0x660);
    if (cp >= 0x6F0 && cp <= 0x6F9) return (char)('0' + cp - 0x6F0);
    // Devanagari digits → ASCII digits.
    if (cp >= 0x966 && cp <= 0x96F) return (char)('0' + cp - 0x966);
    // Mathematical alphanumerics fold to ASCII (𝐀→A — featured articles
    // use them for vectors/matrices). Contiguous bold/italic runs only;
    // script/fraktur have reserved gaps (left as astral-drop below).
    if (cp >= 0x1D400 && cp <= 0x1D419) return (char)('A' + cp - 0x1D400);
    if (cp >= 0x1D41A && cp <= 0x1D433) return (char)('a' + cp - 0x1D41A);
    if (cp >= 0x1D434 && cp <= 0x1D44D) return (char)('A' + cp - 0x1D434);
    if (cp >= 0x1D44E && cp <= 0x1D467) return (char)('a' + cp - 0x1D44E);
    if (cp >= 0x1D468 && cp <= 0x1D481) return (char)('A' + cp - 0x1D468);
    if (cp >= 0x1D482 && cp <= 0x1D49B) return (char)('a' + cp - 0x1D482);
    if (cp >= 0x1D7CE && cp <= 0x1D7D7) return (char)('0' + cp - 0x1D7CE);
    // Combining marks with no consonantal value: Hebrew niqqud/cantillation
    // (U+0591-05CF) and Arabic harakat (U+064B-065F, U+0670). Return 0 =
    // emit nothing (callers skip the slot); unpointed text reads fine.
    if ((cp >= 0x591 && cp <= 0x5CF) ||
        (cp >= 0x64B && cp <= 0x65F) || cp == 0x670 ||
        (cp >= 0xE48 && cp <= 0xE4C)) return 0;
    switch (cp) {
    case 0xC0: return (char)0xEF;  // À
    case 0xC1: return (char)0xF0;  // Á
    case 0xC2: return (char)0xF1;  // Â
    case 0xC3: return (char)0xF2;  // Ã
    case 0xC4: return (char)0xF3;  // Ä
    case 0xC7: return (char)0xF4;  // Ç
    case 0xC8: return (char)0xF5;  // È
    case 0xC9: return (char)0xF6;  // É
    case 0xCA: return (char)0xF7;  // Ê
    case 0xCD: return (char)0xF8;  // Í
    case 0xD1: return (char)0xF9;  // Ñ
    case 0xD3: return (char)0xFA;  // Ó
    case 0xD4: return (char)0xFB;  // Ô
    case 0xD5: return (char)0xFC;  // Õ
    case 0xD6: return (char)0xFD;  // Ö
    case 0xDA: return (char)0xFE;  // Ú
    case 0xDC: return (char)0xFF;  // Ü
    case 0x401: return (char)0xC0;  // Ё
    case 0x451: return (char)0xC1;  // ё
    case 0x2014: return (char)0xC2; // —
    case 0x2013: return (char)0xC3; // –
    case 0x00AB: return (char)0xC4; // «
    case 0x00BB: return (char)0xC5; // »
    case 0x2022: return (char)0xC6; // •
    case 0x2116: return (char)0xC7; // №
    case 0x2026: return (char)0xC8; // …
    case 0x00B0: return (char)0xC9; // °
    case 0x20AC: return (char)0xCA; // €
    case 0x00A9: return (char)0xCB; // ©
    case 0x00AE: return (char)0xCC; // ®
    case 0x2122: return (char)0xCD; // ™
    case 0x2500: return (char)0xCE; // ─
    case 0x2588: return (char)0x01; // █ FULL BLOCK (KAnarchy logo blocks)
    case 0x2592: return (char)0x02; // ▒ MEDIUM SHADE (slots: graphics.c)
    case 0x3000: return ' '; // ideographic space
    case 0x30FB: return (char)0xC6; // ・ nakaguro → bullet
    case 0x30FC: return (char)0xC2; // ー prolonged sound mark → em dash
    // Latin-1 capitals without slots → ASCII base (French caps are
    // conventionally unaccented anyway; Nordic/Icelandic stay readable).
    case 0xC5: case 0xC6: return 'A'; // Å Æ
    case 0xCC: case 0xCE: case 0xCF: return 'I'; // Ì Î Ï
    case 0xD0: return 'D'; // Ð
    case 0xD2: case 0xD8: return 'O'; // Ò Ø
    case 0xD9: case 0xDB: return 'U'; // Ù Û
    case 0xDD: return 'Y'; // Ý
    case 0xDE: return 'P'; // Þ
    case 0xCB: return 'E'; // Ë
    // Latin-1 symbols → lookalike or space.
    case 0xA0: return ' ';  // nbsp
    case 0x202F: return ' '; // narrow nbsp (French thousands separators)
    case 0xA1: return '!';  // ¡
    case 0xA2: return 'c';  // ¢
    case 0xA3: return 'L';  // £
    case 0xA5: return 'Y';  // ¥
    case 0xA6: return '|';  // ¦
    case 0xA7: return 'S';  // §
    case 0xA8: return '"';  // ¨
    case 0xAA: return 'a';  // ª
    case 0xAC: return '-';  // ¬
    case 0xAD: return '-';  // soft hyphen
    case 0xAF: return '-';  // ¯
    case 0xB4: return '\''; // ´
    case 0xB5: return 'u';  // µ
    case 0xB6: return 'P';  // ¶
    case 0xB7: return '.';  // ·
    case 0xB8: return ',';  // ¸
    case 0xB9: return '1';  // ¹
    case 0xBA: return 'o';  // º
    case 0xBF: return '?';  // ¿
    case 0xD7: return 'x';  // ×
    case 0xDF: return 's';  // ß (no slot: single-char "ss" impossible)
    // Smart quotes → ASCII quotes (extremely common on real pages).
    case 0x2018: case 0x2019: case 0x201A: return '\''; // ' ' ‚
    case 0x201C: case 0x201D: case 0x201E: return '"';  // " " „
    case 0x2039: return '<'; // ‹
    case 0x203A: return '>'; // ›
    case 0x02C6: return '^'; // ˆ
    case 0x02DC: return '~'; // ˜
    // Latin Extended-A/B strays → ASCII base.
    case 0x152: case 0x153: return (cp == 0x152) ? 'O' : 'o'; // Œ œ
    case 0x160: case 0x161: return (cp == 0x160) ? 'S' : 's'; // Š š
    case 0x178: return 'Y'; // Ÿ
    case 0x17D: case 0x17E: return (cp == 0x17D) ? 'Z' : 'z'; // Ž ž
    case 0x141: case 0x142: return (cp == 0x141) ? 'L' : 'l'; // Ł ł
    case 0x11E: case 0x11F: return (cp == 0x11E) ? 'G' : 'g'; // Ğ ğ
    case 0x131: return 'i'; // ı
    case 0x218: case 0x219: return (cp == 0x218) ? 'S' : 's'; // Ș ș
    case 0x21A: case 0x21B: return (cp == 0x21A) ? 'T' : 't'; // Ț ț
    // Extended Latin A/B coverage for language names (Czech/Maltese/
    // Romanian/Turkish/Vietnamese-adjacent/Azeri): ASCII base letters.
    // Slots are full, so transliterate (readable, never '?').
    case 0x100: case 0x101: return (cp == 0x100) ? 'A' : 'a'; // Ā ā
    case 0x102: case 0x103: return (cp == 0x102) ? 'A' : 'a'; // Ă ă
    case 0x104: case 0x105: return (cp == 0x104) ? 'A' : 'a'; // Ą ą
    case 0x106: case 0x107: return (cp == 0x106) ? 'C' : 'c'; // Ć ć
    case 0x10C: case 0x10D: return (cp == 0x10C) ? 'C' : 'c'; // Č č
    case 0x10E: case 0x10F: return (cp == 0x10E) ? 'D' : 'd'; // Ď ď
    case 0x110: case 0x111: return (cp == 0x110) ? 'D' : 'd'; // Đ đ
    case 0x112: case 0x113: return (cp == 0x112) ? 'E' : 'e'; // Ē ē
    case 0x116: case 0x117: return (cp == 0x116) ? 'E' : 'e'; // Ė ė
    case 0x118: case 0x119: return (cp == 0x118) ? 'E' : 'e'; // Ę ę
    case 0x11A: case 0x11B: return (cp == 0x11A) ? 'E' : 'e'; // Ě ě
    case 0x122: case 0x123: return (cp == 0x122) ? 'G' : 'g'; // Ģ ģ
    case 0x12A: case 0x12B: return (cp == 0x12A) ? 'I' : 'i'; // Ī ī
    case 0x12E: case 0x12F: return (cp == 0x12E) ? 'I' : 'i'; // Į į
    case 0x130: return 'I'; // İ
    case 0x136: case 0x137: return (cp == 0x136) ? 'K' : 'k'; // Ķ ķ
    case 0x13B: case 0x13C: return (cp == 0x13B) ? 'L' : 'l'; // Ļ ļ
    case 0x143: case 0x144: return (cp == 0x143) ? 'N' : 'n'; // Ń ń
    case 0x145: case 0x146: return (cp == 0x145) ? 'N' : 'n'; // Ņ ņ
    case 0x147: case 0x148: return (cp == 0x147) ? 'N' : 'n'; // Ň ň
    case 0x150: case 0x151: return (cp == 0x150) ? 'O' : 'o'; // Ő ő
    case 0x154: case 0x155: return (cp == 0x154) ? 'R' : 'r'; // Ŕ ŕ
    case 0x156: case 0x157: return (cp == 0x156) ? 'R' : 'r'; // Ŗ ŗ
    case 0x158: case 0x159: return (cp == 0x158) ? 'R' : 'r'; // Ř ř
    case 0x15E: case 0x15F: return (cp == 0x15E) ? 'S' : 's'; // Ş ş
    case 0x162: case 0x163: return (cp == 0x162) ? 'T' : 't'; // Ţ ţ
    case 0x164: case 0x165: return (cp == 0x164) ? 'T' : 't'; // Ť ť
    case 0x168: case 0x169: return (cp == 0x168) ? 'U' : 'u'; // Ũ ũ
    case 0x16A: case 0x16B: return (cp == 0x16A) ? 'U' : 'u'; // Ū ū
    case 0x16E: case 0x16F: return (cp == 0x16E) ? 'U' : 'u'; // Ů ů
    case 0x170: case 0x171: return (cp == 0x170) ? 'U' : 'u'; // Ű ű
    case 0x172: case 0x173: return (cp == 0x172) ? 'U' : 'u'; // Ų ų
    case 0x174: case 0x175: return (cp == 0x174) ? 'W' : 'w'; // Ŵ ŵ
    case 0x176: case 0x177: return (cp == 0x176) ? 'Y' : 'y'; // Ŷ ŷ
    case 0x2BB: case 0x2BC: return '\''; // ʻ ʻokina (Oʻzbekcha → O'zbekcha)
    case 0x259: return 'e'; // ə schwa (Azərbaycanca → Azebaycanca)
    // Cyrillic Extended (Ukrainian/Belarusian/Serbian/Macedonian/Tatar):
    // map to the nearest existing Cyrillic slot (slots are full).
    case 0x400: case 0x454: return (char)0xA5; // Ѐ Є → е
    case 0x402: return (char)0xA4; // Ђ → д
    case 0x403: return (char)0x83; // Ѓ → г
    case 0x405: return 'S'; // Ѕ → S
    case 0x406: return 'I'; // І → I
    case 0x407: return (char)0xA8; // Ї → и
    case 0x408: return 'J'; // Ј → J
    case 0x409: return (char)0xAB; // Љ → л
    case 0x40A: return (char)0xAD; // Њ → н
    case 0x40B: return 'h'; // Ћ → h
    case 0x40C: return (char)0xAA; // Ќ → к
    case 0x40D: return (char)0xA9; // Ѝ → й
    case 0x40E: return (char)0xB3; // Ў → у
    case 0x40F: return 'u'; // Џ → u
    case 0x450: return (char)0xA5; // ё → е
    case 0x452: return (char)0xA4; // ђ → д
    case 0x453: return (char)0xA3; // ѓ → г
    case 0x455: return 's'; // ѕ → s
    case 0x456: return 'i'; // і → i
    case 0x457: return (char)0xA8; // ї → и
    case 0x458: return 'j'; // ј → j
    case 0x459: return (char)0xAB; // љ → л
    case 0x45A: return (char)0xAD; // њ → н
    case 0x45B: return 'h'; // ћ → h
    case 0x45C: return (char)0xAA; // ќ → к
    case 0x45D: return (char)0xA9; // ѝ → й
    case 0x45E: return (char)0xB3; // ў → у
    case 0x45F: return 'u'; // џ → u
    case 0x490: return (char)0x83; // Ґ → г
    case 0x491: return (char)0xA3; // ґ → г
    // Greek → Latin transliteration (slots are full; readable, never '?').
    // Modern pronunciation + lookalikes: Β→V, Η→I, Θ→T, Χ→X, Ψ→P.
    case 0x391: case 0x3B1: return (cp == 0x391) ? 'A' : 'a'; // Α α
    case 0x392: case 0x3B2: return (cp == 0x392) ? 'V' : 'v'; // Β β
    case 0x393: case 0x3B3: return (cp == 0x393) ? 'G' : 'g'; // Γ γ
    case 0x394: case 0x3B4: return (cp == 0x394) ? 'D' : 'd'; // Δ δ
    case 0x395: case 0x3B5: return (cp == 0x395) ? 'E' : 'e'; // Ε ε
    case 0x396: case 0x3B6: return (cp == 0x396) ? 'Z' : 'z'; // Ζ ζ
    case 0x397: case 0x3B7: return (cp == 0x397) ? 'I' : 'i'; // Η η
    case 0x398: case 0x3B8: return (cp == 0x398) ? 'T' : 't'; // Θ θ
    case 0x399: case 0x3B9: return (cp == 0x399) ? 'I' : 'i'; // Ι ι
    case 0x39A: case 0x3BA: return (cp == 0x39A) ? 'K' : 'k'; // Κ κ
    case 0x39B: case 0x3BB: return (cp == 0x39B) ? 'L' : 'l'; // Λ λ
    case 0x39C: case 0x3BC: return (cp == 0x39C) ? 'M' : 'm'; // Μ μ
    case 0x39D: case 0x3BD: return (cp == 0x39D) ? 'N' : 'n'; // Ν ν
    case 0x39E: case 0x3BE: return (cp == 0x39E) ? 'X' : 'x'; // Ξ ξ
    case 0x39F: case 0x3BF: return (cp == 0x39F) ? 'O' : 'o'; // Ο ο
    case 0x3A0: case 0x3C0: return (cp == 0x3A0) ? 'P' : 'p'; // Π π
    case 0x3A1: case 0x3C1: return (cp == 0x3A1) ? 'R' : 'r'; // Ρ ρ
    case 0x3A3: case 0x3C3: case 0x3C2: return (cp == 0x3A3) ? 'S' : 's'; // Σ σ ς
    case 0x3A4: case 0x3C4: return (cp == 0x3A4) ? 'T' : 't'; // Τ τ
    case 0x3A5: case 0x3C5: return (cp == 0x3A5) ? 'Y' : 'y'; // Υ υ
    case 0x3A6: case 0x3C6: return (cp == 0x3A6) ? 'F' : 'f'; // Φ φ
    case 0x3A7: case 0x3C7: return (cp == 0x3A7) ? 'X' : 'x'; // Χ χ
    case 0x3A8: case 0x3C8: return (cp == 0x3A8) ? 'P' : 'p'; // Ψ ψ
    case 0x3A9: case 0x3C9: return (cp == 0x3A9) ? 'O' : 'o'; // Ω ω
    case 0x386: case 0x3AC: return (cp == 0x386) ? 'A' : 'a'; // Ά ά
    case 0x388: case 0x3AD: return (cp == 0x388) ? 'E' : 'e'; // Έ έ
    case 0x389: case 0x3AE: return (cp == 0x389) ? 'I' : 'i'; // Ή ή
    case 0x38A: case 0x3AF: return (cp == 0x38A) ? 'I' : 'i'; // Ί ί
    case 0x38C: case 0x3CC: return (cp == 0x38C) ? 'O' : 'o'; // Ό ό
    case 0x38E: case 0x3CD: return (cp == 0x38E) ? 'Y' : 'y'; // Ύ ύ
    case 0x38F: case 0x3CE: return (cp == 0x38F) ? 'O' : 'o'; // Ώ ώ
    case 0x3AA: case 0x3CA: return (cp == 0x3AA) ? 'I' : 'i'; // Ϊ ϊ
    case 0x3AB: case 0x3CB: return (cp == 0x3AB) ? 'Y' : 'y'; // Ϋ ϋ
    case 0x3B0: return 'y'; // ΰ
    case 0x3D0: case 0x3D1: case 0x3D5: case 0x3D6: // ϐ ϑ ϕ ϖ
        return (cp == 0x3D0) ? 'v' : (cp == 0x3D1) ? 't' : (cp == 0x3D5) ? 'f' : 'p';
    case 0x3D8: case 0x3F1: case 0x3F2: case 0x3F5: // ϰ ϱ ϲ ϵ
        return (cp == 0x3D8) ? 'k' : (cp == 0x3F1) ? 'r' : (cp == 0x3F2) ? 's' : 'e';
    case 0x3F9: return 's'; // Ϲ lunate sigma
    // Georgian Mkhedruli → Latin (Asomtavruli folded above).
    case 0x10D0: return 'a'; // ა
    case 0x10D1: return 'b'; // ბ
    case 0x10D2: return 'g'; // გ
    case 0x10D3: return 'd'; // დ
    case 0x10D4: return 'e'; // ე
    case 0x10D5: return 'v'; // ვ
    case 0x10D6: return 'z'; // ზ
    case 0x10D7: return 't'; // თ
    case 0x10D8: return 'i'; // ი
    case 0x10D9: return 'k'; // კ
    case 0x10DA: return 'l'; // ლ
    case 0x10DB: return 'm'; // მ
    case 0x10DC: return 'n'; // ნ
    case 0x10DD: return 'o'; // ო
    case 0x10DE: return 'p'; // პ
    case 0x10DF: return 'z'; // ჟ (zh)
    case 0x10E0: return 'r'; // რ
    case 0x10E1: return 's'; // ს
    case 0x10E2: return 't'; // ტ
    case 0x10E3: return 'u'; // უ
    case 0x10E4: return 'p'; // ფ (p')
    case 0x10E5: return 'k'; // ქ (k')
    case 0x10E6: return 'g'; // ღ (gh)
    case 0x10E7: return 'q'; // ყ
    case 0x10E8: return 's'; // შ (sh)
    case 0x10E9: return 'c'; // ჩ (ch)
    case 0x10EA: return 'c'; // ც (ts)
    case 0x10EB: return 'd'; // ძ (dz)
    case 0x10EC: return 'c'; // წ (ts')
    case 0x10ED: return 'c'; // ჭ (ch')
    case 0x10EE: return 'x'; // ხ (kh)
    case 0x10EF: return 'j'; // ჯ
    case 0x10F0: return 'h'; // ჰ
    case 0x10F1: return 'e'; // ჱ (archaic he)
    case 0x10F2: return 'y'; // ჲ (archaic)
    case 0x10F3: return 'w'; // ჳ (archaic)
    case 0x10F4: return 'q'; // ჴ (archaic)
    case 0x10F5: return 'o'; // ჵ (archaic)
    case 0x10F6: return 'f'; // ჶ (archaic)
    // Armenian → Latin (capitals folded above).
    case 0x561: return 'a'; // ա
    case 0x562: return 'b'; // բ
    case 0x563: return 'g'; // գ
    case 0x564: return 'd'; // դ
    case 0x565: return 'e'; // ե
    case 0x566: return 'z'; // զ
    case 0x567: return 'e'; // է
    case 0x568: return 'e'; // ը (schwa-ish)
    case 0x569: return 't'; // թ
    case 0x56A: return 'z'; // ժ (zh)
    case 0x56B: return 'i'; // ի
    case 0x56C: return 'l'; // լ
    case 0x56D: return 'x'; // խ (kh)
    case 0x56E: return 'c'; // ծ (ts)
    case 0x56F: return 'k'; // կ
    case 0x570: return 'h'; // հ
    case 0x571: return 'd'; // ձ (dz)
    case 0x572: return 'g'; // ղ (gh)
    case 0x573: return 'c'; // ճ (ch)
    case 0x574: return 'm'; // մ
    case 0x575: return 'y'; // յ
    case 0x576: return 'n'; // ն
    case 0x577: return 's'; // շ (sh)
    case 0x578: return 'o'; // ո
    case 0x579: return 'c'; // չ (ch')
    case 0x57A: return 'p'; // պ
    case 0x57B: return 'j'; // ջ (j)
    case 0x57C: return 'r'; // ռ
    case 0x57D: return 's'; // ս
    case 0x57E: return 'v'; // վ
    case 0x57F: return 't'; // տ
    case 0x580: return 'r'; // ր
    case 0x581: return 'c'; // ց (ts')
    case 0x582: return 'w'; // ւ
    case 0x583: return 'p'; // փ (p')
    case 0x584: return 'q'; // ք (k')
    case 0x585: return 'o'; // օ
    case 0x586: return 'f'; // ֆ
    case 0x587: return 'e'; // և ligature (ew)
    // Hebrew letters → Latin. Vowel points / cantillation (U+0591-05CF)
    // carry no consonantal value: dropped (return 0 = emit nothing).
    // Hebrew letters → Latin (U+05D0-; niqqud dropped by the range rule
    // above). Vowel points / cantillation (U+0591-05CF) emit nothing.
    case 0x5D0: return 'a'; // א
    case 0x5D1: return 'b'; // ב
    case 0x5D2: return 'g'; // ג
    case 0x5D3: return 'd'; // ד
    case 0x5D4: return 'h'; // ה
    case 0x5D5: return 'v'; // ו
    case 0x5D6: return 'z'; // ז
    case 0x5D7: return 'h'; // ח (het)
    case 0x5D8: return 't'; // ט (tet)
    case 0x5D9: return 'y'; // י
    case 0x5DA: case 0x5DB: return 'k'; // ך כ
    case 0x5DC: return 'l'; // ל
    case 0x5DD: case 0x5DE: return 'm'; // ם מ
    case 0x5DF: case 0x5E0: return 'n'; // ן נ
    case 0x5E1: return 's'; // ס
    case 0x5E2: return 'e'; // ע
    case 0x5E3: case 0x5E4: return 'p'; // ף פ
    case 0x5E5: case 0x5E6: return 'c'; // ץ צ (ts)
    case 0x5E7: return 'q'; // ק
    case 0x5E8: return 'r'; // ר
    case 0x5E9: return 's'; // ש (sh/sin)
    case 0x5EA: return 't'; // ת
    case 0x5F0: return 'v'; // װ (Yiddish double-vav)
    case 0x5F1: return 'o'; // ױ (Yiddish vav-yod)
    case 0x5F2: return 'y'; // ײ (Yiddish double-yod)
    case 0x5F3: return '\''; // ׳ geresh
    case 0x5F4: return '"'; // ״ gershayim
    // Arabic letters → Latin (simplified; slots are full). Harakat and
    // other combining marks (U+064B-065F, U+0670) return 0 = emit nothing.
    case 0x621: return '\''; // ء hamza
    case 0x622: case 0x623: case 0x627: return 'a'; // آ أ ا
    case 0x624: case 0x648: return 'w'; // ؤ و
    case 0x625: return 'i'; // إ
    case 0x626: case 0x649: case 0x64A: case 0x6CC: case 0x6C3: return 'y'; // ئ ى ي ی ے
    case 0x628: return 'b'; // ب
    case 0x629: case 0x603: return 'h'; // ة ۃ (teh marbuta)
    case 0x62A: case 0x679: return 't'; // ت ٹ
    case 0x62B: return 's'; // ث (th)
    case 0x62C: return 'j'; // ج
    case 0x62D: return 'h'; // ح
    case 0x62E: return 'x'; // خ (kh)
    case 0x62F: return 'd'; // د
    case 0x630: return 'z'; // ذ (dh)
    case 0x631: return 'r'; // ر
    case 0x632: return 'z'; // ز
    case 0x633: return 's'; // س
    case 0x634: return 's'; // ش (sh)
    case 0x635: return 's'; // ص (S)
    case 0x636: return 'd'; // ض (D)
    case 0x637: return 't'; // ط (T)
    case 0x638: return 'z'; // ظ (Z)
    case 0x639: return 'e'; // ع
    case 0x63A: return 'g'; // غ (gh)
    case 0x640: return '-'; // ـ tatweel
    case 0x641: return 'f'; // ف
    case 0x642: return 'q'; // ق
    case 0x643: case 0x6A9: return 'k'; // ك ک
    case 0x644: return 'l'; // ل
    case 0x645: return 'm'; // م
    case 0x646: return 'n'; // ن
    case 0x647: case 0x6BE: case 0x6C1: case 0x6C2: return 'h'; // ه ھ ہ ۂ
    case 0x686: return 'c'; // چ (ch)
    case 0x698: return 'z'; // ژ (zh)
    case 0x671: return 'g'; // گ
    case 0x6BA: return 'n'; // ں (noon ghunna)
    case 0x6D5: return 'e'; // ە
    case 0x688: return 'd'; // ڈ
    case 0x691: return 'r'; // ڑ
    case 0x60C: return ','; // ،
    case 0x61B: return ';'; // ؛
    case 0x61F: return '?'; // ؟
    // Devanagari → Latin. Dependent vowel signs map to their vowel;
    // virama/nukta join or silence (emit nothing).
    case 0x901: case 0x902: return 'n'; // ँ ं (nasalization)
    case 0x903: return 'h'; // ः visarga
    case 0x905: case 0x906: return 'a'; // अ आ
    case 0x907: case 0x908: return 'i'; // इ ई
    case 0x909: case 0x90A: return 'u'; // उ ऊ
    case 0x90B: return 'r'; // ऋ (vocalic r)
    case 0x90F: case 0x910: return 'e'; // ए ऐ
    case 0x911: case 0x912: return (cp == 0x911) ? 'o' : 'l'; // ऑ ऌ (rare)
    case 0x913: case 0x914: return 'o'; // ओ औ
    case 0x915: case 0x916: return 'k'; // क ख
    case 0x917: case 0x918: return 'g'; // ग घ
    case 0x919: return 'n'; // ङ (ng)
    case 0x91A: case 0x91B: return 'c'; // च छ
    case 0x91C: case 0x91D: return 'j'; // ज झ
    case 0x91E: return 'n'; // ञ (ny)
    case 0x91F: case 0x920: return 't'; // ट ठ
    case 0x921: case 0x922: return 'd'; // ड ढ
    case 0x923: return 'n'; // ण
    case 0x924: case 0x925: return 't'; // त थ
    case 0x926: case 0x927: return 'd'; // द ध
    case 0x928: return 'n'; // न
    case 0x929: return 'y'; // य़
    case 0x92A: case 0x92B: return 'p'; // प फ
    case 0x92C: case 0x92D: return 'b'; // ब भ
    case 0x92E: return 'm'; // म
    case 0x92F: return 'y'; // य
    case 0x950: return 'O'; // ॐ om
    case 0x930: return 'r'; // र
    case 0x931: return 'r'; // ड़ RRA
    case 0x932: return 'l'; // ल
    case 0x933: case 0x934: return 'l'; // ळ ळ्ळ (retroflex l)
    case 0x935: return 'v'; // व
    case 0x936: case 0x937: case 0x938: return 's'; // श ष स
    case 0x939: return 'h'; // ह
    case 0x958: return 'q'; // क़
    case 0x959: return 'x'; // ख़ (kh)
    case 0x95A: return 'g'; // ग़ (gh)
    case 0x95B: return 'z'; // ज़
    case 0x95C: case 0x95D: return 'd'; // ड़ ढ़
    case 0x95E: return 'f'; // फ़
    case 0x95F: return 'y'; // य़
    case 0x93E: return 'a'; // ा
    case 0x93F: case 0x940: return 'i'; // ि ी
    case 0x941: case 0x942: return 'u'; // ु ू
    case 0x943: case 0x944: return 'r'; // ृ ॄ
    case 0x945: case 0x946: case 0x947: case 0x948: return 'e'; // ॅ ॆ े ै
    case 0x949: case 0x94A: case 0x94B: return 'o'; // ॉ ो ौ
    case 0x94E: case 0x94F: return 'e'; // ॎ ॏ (rare vowel signs)
    case 0x93A: case 0x93B: return 'o'; // OE/OE-length signs
    case 0x93D: return '\''; // avagraha
    case 0x964: case 0x965: case 0x970: return '.'; // danda/section stops
    // Thai letters/signs → Latin. Tone marks, silencer, repetition and
    // shorten marks emit nothing (return 0, handled with the skips below).
    case 0xE01: case 0xE02: case 0xE03: case 0xE04: case 0xE05: case 0xE06:
        return 'k';
    case 0xE07: return 'n';
    case 0xE08: case 0xE09: case 0xE0A: case 0xE0C: return 'c';
    case 0xE0B: return 's';
    case 0xE0D: return 'y';
    case 0xE0E: return 'd';
    case 0xE0F: case 0xE10: case 0xE11: case 0xE12: return 't';
    case 0xE13: return 'n';
    case 0xE14: return 'd';
    case 0xE15: case 0xE16: case 0xE17: case 0xE18: return 't';
    case 0xE19: return 'n';
    case 0xE1A: return 'b';
    case 0xE1B: case 0xE1C: case 0xE1E: case 0xE20: return 'p';
    case 0xE1D: case 0xE1F: return 'f';
    case 0xE21: return 'm';
    case 0xE22: return 'y';
    case 0xE23: case 0xE24: return 'r';
    case 0xE25: case 0xE26: case 0xE2C: return 'l';
    case 0xE27: return 'w';
    case 0xE28: case 0xE29: case 0xE2A: return 's';
    case 0xE2B: case 0xE2E: return 'h';
    case 0xE2D: return 'o';
    case 0xE30: case 0xE32: case 0xE33: return 'a';
    case 0xE34: case 0xE35: return 'i';
    case 0xE36: case 0xE37: case 0xE38: case 0xE39: return 'u';
    case 0xE40: case 0xE41: return 'e';
    case 0xE42: return 'o';
    case 0xE43: case 0xE44: case 0xE45: return 'a';
    case 0xE4E: return 'n';
    case 0xE4F: return 'O';
    case 0xEAF: return '.';
    // Myanmar letters/signs → Latin (stacking/reordering not modeled;
    // usual L-to-R order still reads). Asat/dot-below emit nothing.
    case 0x1000: case 0x1001: return 'k';
    case 0x1002: case 0x1003: return 'g';
    case 0x1004: return 'n';
    case 0x1005: case 0x1006: return 's';
    case 0x1007: case 0x1008: return 'z';
    case 0x1009: case 0x100A: return 'n';
    case 0x100B: case 0x100C: return 't';
    case 0x100D: case 0x100E: return 'd';
    case 0x100F: return 'n';
    case 0x1010: case 0x1011: return 't';
    case 0x1012: case 0x1013: return 'd';
    case 0x1014: return 'n';
    case 0x1015: case 0x1016: return 'p';
    case 0x1017: case 0x1018: return 'b';
    case 0x1019: return 'm';
    case 0x101A: return 'y';
    case 0x101B: return 'r';
    case 0x101C: case 0x1020: return 'l';
    case 0x101D: return 'w';
    case 0x101E: return 's';
    case 0x101F: return 's'; // GREAT SA (was 'h': copy-paste slip)
    case 0x1021: return 'a';
    case 0x102C: return 'a';
    case 0x102D: case 0x102E: return 'i';
    case 0x102F: case 0x1030: return 'u';
    case 0x1031: case 0x1032: return 'e';
    case 0x1036: return 'n';
    case 0x1038: return 'h';
    case 0x103B: return 'y';
    case 0x103C: return 'r';
    case 0x103D: return 'w';
    case 0x103E: return 'h';
    case 0x104A: case 0x104B: return '.';
    // Tifinagh (Berber) → Latin.
    case 0x2D30: return 'a';
    case 0x2D31: case 0x2D32: return 'b';
    case 0x2D33: case 0x2D34: return 'g';
    case 0x2D35: case 0x2D36: return 'j';
    case 0x2D37: case 0x2D38: case 0x2D39: case 0x2D3A: return 'd';
    case 0x2D3B: return 'e';
    case 0x2D3C: return 'f';
    case 0x2D3D: case 0x2D3E: return 'k';
    case 0x2D3F: return 'x';
    case 0x2D40: case 0x2D41: case 0x2D42: case 0x2D43: return 'h';
    case 0x2D44: return 'a';
    case 0x2D45: case 0x2D46: return 'x';
    case 0x2D47: case 0x2D48: return 'q';
    case 0x2D49: return 'i';
    case 0x2D4A: case 0x2D4B: case 0x2D4C: return 'z';
    case 0x2D4D: return 'l';
    case 0x2D4E: return 'm';
    case 0x2D4F: case 0x2D50: case 0x2D51: return 'n';
    case 0x2D52: return 'p';
    case 0x2D53: return 'u';
    case 0x2D54: case 0x2D55: return 'r';
    case 0x2D56: case 0x2D57: case 0x2D58: return 'g';
    case 0x2D59: case 0x2D5A: return 's';
    case 0x2D5B: return 'c';
    case 0x2D5C: case 0x2D5D: return 't';
    case 0x2D5E: return 'c';
    case 0x2D5F: return 't';
    case 0x2D60: return 'v';
    case 0x2D61: return 'w';
    case 0x2D62: return 'y';
    case 0x2D63: case 0x2D64: case 0x2D65: return 'z';
    // Combining marks with no consonantal value emit NOTHING (return 0;
    // callers skip the slot): Hebrew niqqud/cantillation, Arabic harakat,
    // Devanagari virama/nukta. Unpointed text reads fine without them.
    case 0x93C: case 0x94D: return 0; // nukta, virama
    case 0xE31: case 0xE46: case 0xE47: case 0xE4D: return 0; // Thai marks
    case 0x1039: case 0x103A: case 0x1037: return 0; // Myanmar virama/asat/dot
    // Wikipedia ???? census 2026-10-01: transliteration for scripts with no
    // font slots (readable, never '?'). Drops (0) are combining marks, tones,
    // viramas, format controls — invisible by design.
    case 0x300: return 0; // Combining Grave Accent
    case 0x301: return 0; // Combining Acute Accent
    case 0x304: return 0; // Combining Macron
    case 0x30D: return 0; // Combining Vertical Line Above
    case 0x324: return 0; // Combining Diaeresis Below
    case 0x331: return 0; // Combining Macron Below
    case 0x200E: return 0; // Left-To-Right Mark
    case 0x2C7: return 0; // caron / Mandarin 3rd-tone mark (invisible)
    case 0x2B9: return '\''; // Modifier Letter Prime
    case 0x2BF: return '\''; // Modifier Letter Left Half Ring
    case 0x108: return 'C'; // Latin Capital Letter C With Circumflex
    case 0x10B: return 'c'; // Latin Small Letter C With Dot Above
    case 0x115: return 'e'; // Latin Small Letter E With Breve
    case 0x11D: return 'g'; // Latin Small Letter G With Circumflex
    case 0x121: return 'g'; // Latin Small Letter G With Dot Above
    case 0x129: return 'i'; // Latin Small Letter I With Tilde
    case 0x12D: return 'i'; // Latin Small Letter I With Breve
    case 0x14A: return 'N'; // Latin Capital Letter Eng
    case 0x14B: return 'n'; // Latin Small Letter Eng
    case 0x14C: return 'O'; // Latin Capital Letter O With Macron
    case 0x14D: return 'o'; // Latin Small Letter O With Macron
    case 0x14F: return 'o'; // Latin Small Letter O With Breve
    case 0x15A: return 'S'; // Latin Capital Letter S With Acute
    case 0x16D: return 'u'; // Latin Small Letter U With Breve
    case 0x1CE: return 'a'; // Latin Small Letter A With Caron
    case 0x1D4: return 'u'; // Latin Small Letter U With Caron
    case 0x253: return 'b'; // Latin Small Letter B With Hook
    case 0x254: return 'o'; // Latin Small Letter Open O
    case 0x256: return 'd'; // Latin Small Letter D With Tail
    case 0x25B: return 'e'; // Latin Small Letter Open E
    case 0x263: return 'g'; // Latin Small Letter Gamma
    case 0x269: return 'i'; // Latin Small Letter Iota
    case 0x26F: return 'u'; // Latin Small Letter Turned M
    case 0x282: return 's'; // Latin Small Letter S With Hook
    case 0x28B: return 'v'; // Latin Small Letter V With Hook
    case 0x1E13: return 'd'; // Latin Small Letter D With Circumflex Below
    case 0x1E25: return 'h'; // Latin Small Letter H With Dot Below
    case 0x1E37: return 'l'; // Latin Small Letter L With Dot Below
    case 0x1E3B: return 'l'; // Latin Small Letter L With Line Below
    case 0x1E43: return 'm'; // Latin Small Letter M With Dot Below
    case 0x1E57: return 'p'; // Latin Small Letter P With Dot Above
    case 0x1E5B: return 'r'; // Latin Small Letter R With Dot Below
    case 0x1E63: return 's'; // Latin Small Letter S With Dot Below
    case 0x1E6D: return 't'; // Latin Small Letter T With Dot Below
    case 0x1E73: return 'u'; // Latin Small Letter U With Diaeresis Below
    case 0x1EBD: return 'e'; // Latin Small Letter E With Tilde
    case 0x1EBF: return 'a'; // Latin Small Letter E With Circumflex And Acute
    case 0x1EC7: return 'e'; // Latin Small Letter E With Circumflex And Dot Below
    case 0x1ECD: return 'o'; // Latin Small Letter O With Dot Below
    case 0x1EE5: return 'u'; // Latin Small Letter U With Dot Below
    case 0x463: return 'e'; // Cyrillic Small Letter Yat
    case 0x49A: return 'k'; // Cyrillic Capital Letter Ka With Descender
    case 0x49B: return 'k'; // Cyrillic Small Letter Ka With Descender
    case 0x49F: return 'k'; // Cyrillic Small Letter Ka With Stroke
    case 0x4A1: return 'k'; // Cyrillic Small Letter Bashkir Ka
    case 0x4A5: return 'n'; // Cyrillic Small Ligature En Ghe
    case 0x4AF: return 'u'; // Cyrillic Small Letter Straight U
    case 0x4B3: return 'h'; // Cyrillic Small Letter Ha With Descender
    case 0x4B7: return 'c'; // Cyrillic Small Letter Che With Descender
    case 0x4BB: return 'h'; // Cyrillic Small Letter Shha
    case 0x4C0: return 'l'; // Cyrillic Letter Palochka
    case 0x4D1: return 'a'; // Cyrillic Small Letter A With Breve
    case 0x4D3: return 'a'; // Cyrillic Small Letter A With Diaeresis
    case 0x4D9: return 'e'; // Cyrillic Small Letter Schwa
    case 0x4E3: return 'i'; // Cyrillic Small Letter I With Macron
    case 0x4E7: return 'o'; // Cyrillic Small Letter O With Diaeresis
    case 0x4F1: return 'u'; // Cyrillic Small Letter U With Diaeresis
    case 0x4F9: return 'y'; // Cyrillic Small Letter Yeru With Diaeresis
    case 0x525: return 'p'; // Cyrillic Small Letter Pe With Descender
    case 0x672: return 'a'; // Arabic Letter Alef With Wavy Hamza Above
    case 0x67E: return 'p'; // Arabic Letter Peh
    case 0x68C: return 'd'; // Arabic Letter Dahal
    case 0x693: return 'r'; // Arabic Letter Reh With Ring
    case 0x69A: return 'x'; // Arabic Letter Seen With Dot Below And Dot Above
    case 0x6AF: return 'g'; // Arabic Letter Gaf
    case 0x6C6: return 'o'; // Arabic Letter Oe
    case 0x6C7: return 'u'; // Arabic Letter U
    case 0x6CE: return 'e'; // Arabic Letter Yeh With Small V
    case 0x710: return 'a'; // Syriac Letter Alaph
    case 0x718: return 'w'; // Syriac Letter Waw
    case 0x71D: return 'y'; // Syriac Letter Yudh
    case 0x721: return 'm'; // Syriac Letter Mim
    case 0x726: return 'p'; // Syriac Letter Pe
    case 0x72A: return 'r'; // Syriac Letter Rish
    case 0x72B: return 's'; // Syriac Letter Shin
    case 0x72C: return 't'; // Syriac Letter Taw
    case 0x7CA: return 'a'; // Nko Letter A
    case 0x7CF: return 'o'; // Nko Letter Oo
    case 0x7D2: return 'n'; // Nko Letter N
    case 0x7D3: return 'b'; // Nko Letter Ba
    case 0x7D8: return 'd'; // Nko Letter Da
    case 0x7DE: return 'k'; // Nko Letter Ka
    case 0x7DF: return 'l'; // Nko Letter La
    case 0x7EC: return 0; // Nko Combining Short Low Tone
    case 0x7F2: return 0; // Nko Combining Nasalization Mark
    case 0x780: return 'h'; // Thaana Letter Haa
    case 0x784: return 'b'; // Thaana Letter Baa
    case 0x787: return 'a'; // Thaana Letter Alifu
    case 0x788: return 'v'; // Thaana Letter Vaavu
    case 0x789: return 'm'; // Thaana Letter Meemu
    case 0x78A: return 'f'; // Thaana Letter Faafu
    case 0x78B: return 'd'; // Thaana Letter Dhaalu
    case 0x790: return 's'; // Thaana Letter Seenu
    case 0x799: return 'h'; // Thaana Letter Hhaa
    case 0x79E: return 's'; // Thaana Letter Saadhu
    case 0x7A6: return 'a'; // Thaana Abafili
    case 0x7A7: return 'a'; // Thaana Aabaafili
    case 0x7A8: return 'i'; // Thaana Ibifili
    case 0x7AC: return 'e'; // Thaana Ebefili
    case 0x7B0: return 0; // Thaana Sukun
    case 0x94C: return 'o'; // Devanagari Vowel Sign Au
    case 0x982: return 'n'; // Bengali Sign Anusvara
    case 0x985: return 'o'; // Bengali Letter A
    case 0x99F: return 't'; // Bengali Letter Tta
    case 0x9A3: return 'n'; // Bengali Letter Nna
    case 0x9A4: return 't'; // Bengali Letter Ta
    case 0x9A7: return 'd'; // Bengali Letter Dha
    case 0x9A8: return 'n'; // Bengali Letter Na
    case 0x9AA: return 'p'; // Bengali Letter Pa
    case 0x9AC: return 'b'; // Bengali Letter Ba
    case 0x9AE: return 'm'; // Bengali Letter Ma
    case 0x9AF: return 'y'; // Bengali Letter Ya
    case 0x9B0: return 'r'; // Bengali Letter Ra
    case 0x9B2: return 'l'; // Bengali Letter La
    case 0x9B7: return 's'; // Bengali Letter Ssa
    case 0x9B8: return 's'; // Bengali Letter Sa
    case 0x9BC: return 0; // Bengali Sign Nukta
    case 0x9BE: return 'a'; // Bengali Vowel Sign Aa
    case 0x9BF: return 'i'; // Bengali Vowel Sign I
    case 0x9C0: return 'i'; // Bengali Vowel Sign Ii
    case 0x9C1: return 'u'; // Bengali Vowel Sign U
    case 0x9C7: return 'e'; // Bengali Vowel Sign E
    case 0x9CD: return 0; // Bengali Sign Virama
    case 0xA16: return 'k'; // Gurmukhi Letter Kha
    case 0xA1C: return 'j'; // Gurmukhi Letter Ja
    case 0xA2A: return 'p'; // Gurmukhi Letter Pa
    case 0xA2B: return 'p'; // Gurmukhi Letter Pha
    case 0xA2C: return 'b'; // Gurmukhi Letter Ba
    case 0xA2E: return 'm'; // Gurmukhi Letter Ma
    case 0xA38: return 's'; // Gurmukhi Letter Sa
    case 0xA3C: return 0; // Gurmukhi Sign Nukta
    case 0xA3E: return 'a'; // Gurmukhi Vowel Sign Aa
    case 0xA40: return 'i'; // Gurmukhi Vowel Sign Ii
    case 0xA41: return 'u'; // Gurmukhi Vowel Sign U
    case 0xA70: return 'n'; // Gurmukhi Tippi
    case 0xA71: return 0; // Gurmukhi Addak
    case 0xA96: return 'k'; // Gujarati Letter Kha
    case 0xA97: return 'g'; // Gujarati Letter Ga
    case 0xA9C: return 'j'; // Gujarati Letter Ja
    case 0xAA0: return 't'; // Gujarati Letter Ttha
    case 0xAA4: return 't'; // Gujarati Letter Ta
    case 0xAAA: return 'p'; // Gujarati Letter Pa
    case 0xAAE: return 'm'; // Gujarati Letter Ma
    case 0xAB0: return 'r'; // Gujarati Letter Ra
    case 0xAB7: return 's'; // Gujarati Letter Ssa
    case 0xABE: return 'a'; // Gujarati Vowel Sign Aa
    case 0xAC0: return 'i'; // Gujarati Vowel Sign Ii
    case 0xAC1: return 'u'; // Gujarati Vowel Sign U
    case 0xAC3: return 'r'; // Gujarati Vowel Sign Vocalic R
    case 0xACD: return 0; // Gujarati Sign Virama
    case 0xB06: return 'a'; // Oriya Letter Aa
    case 0xB13: return 'o'; // Oriya Letter O
    case 0xB20: return 't'; // Oriya Letter Ttha
    case 0xB21: return 'd'; // Oriya Letter Dda
    case 0xB27: return 'd'; // Oriya Letter Dha
    case 0xB28: return 'n'; // Oriya Letter Na
    case 0xB2A: return 'p'; // Oriya Letter Pa
    case 0xB30: return 'r'; // Oriya Letter Ra
    case 0xB37: return 's'; // Oriya Letter Ssa
    case 0xB3C: return 0; // Oriya Sign Nukta
    case 0xB3E: return 'a'; // Oriya Vowel Sign Aa
    case 0xB3F: return 'i'; // Oriya Vowel Sign I
    case 0xB43: return 'r'; // Oriya Vowel Sign Vocalic R
    case 0xB4D: return 0; // Oriya Sign Virama
    case 0xB95: return 'k'; // Tamil Letter Ka
    case 0xBA4: return 't'; // Tamil Letter Ta
    case 0xBAA: return 'p'; // Tamil Letter Pa
    case 0xBAE: return 'm'; // Tamil Letter Ma
    case 0xBB1: return 'r'; // Tamil Letter Rra
    case 0xBB4: return 'l'; // Tamil Letter Llla
    case 0xBBF: return 'i'; // Tamil Vowel Sign I
    case 0xBC1: return 'u'; // Tamil Vowel Sign U
    case 0xBCD: return 0; // Tamil Sign Virama
    case 0xC17: return 'g'; // Telugu Letter Ga
    case 0xC1C: return 'j'; // Telugu Letter Ja
    case 0xC1F: return 't'; // Telugu Letter Tta
    case 0xC24: return 't'; // Telugu Letter Ta
    case 0xC26: return 'd'; // Telugu Letter Da
    case 0xC2A: return 'p'; // Telugu Letter Pa
    case 0xC2E: return 'm'; // Telugu Letter Ma
    case 0xC32: return 'l'; // Telugu Letter La
    case 0xC3F: return 'i'; // Telugu Vowel Sign I
    case 0xC40: return 'i'; // Telugu Vowel Sign Ii
    case 0xC41: return 'u'; // Telugu Vowel Sign U
    case 0xC46: return 'e'; // Telugu Vowel Sign E
    case 0xC47: return 'e'; // Telugu Vowel Sign Ee
    case 0xC4A: return 'o'; // Telugu Vowel Sign O
    case 0xC95: return 'k'; // Kannada Letter Ka
    case 0xC96: return 'k'; // Kannada Letter Kha
    case 0xC9F: return 't'; // Kannada Letter Tta
    case 0xCA1: return 'd'; // Kannada Letter Dda
    case 0xCA4: return 't'; // Kannada Letter Ta
    case 0xCA8: return 'n'; // Kannada Letter Na
    case 0xCAA: return 'p'; // Kannada Letter Pa
    case 0xCAE: return 'm'; // Kannada Letter Ma
    case 0xCAF: return 'y'; // Kannada Letter Ya
    case 0xCB3: return 'l'; // Kannada Letter Lla
    case 0xCC1: return 'u'; // Kannada Vowel Sign U
    case 0xCCD: return 0; // Kannada Sign Virama
    case 0xD02: return 'n'; // Malayalam Sign Anusvara
    case 0xD24: return 't'; // Malayalam Letter Ta
    case 0xD27: return 'd'; // Malayalam Letter Dha
    case 0xD28: return 'n'; // Malayalam Letter Na
    case 0xD2A: return 'p'; // Malayalam Letter Pa
    case 0xD2E: return 'm'; // Malayalam Letter Ma
    case 0xD2F: return 'y'; // Malayalam Letter Ya
    case 0xD30: return 'r'; // Malayalam Letter Ra
    case 0xD32: return 'l'; // Malayalam Letter La
    case 0xD33: return 'l'; // Malayalam Letter Lla
    case 0xD3E: return 'a'; // Malayalam Vowel Sign Aa
    case 0xD4D: return 0; // Malayalam Sign Virama
    case 0xD7E: return 'l'; // Malayalam Letter Chillu Ll
    case 0xD82: return 'n'; // Sinhala Sign Anusvaraya
    case 0xDA7: return 't'; // Sinhala Letter Alpapraana Ttayanna
    case 0xDB4: return 'p'; // Sinhala Letter Alpapraana Payanna
    case 0xDB8: return 'm'; // Sinhala Letter Mayanna
    case 0xDBD: return 'l'; // Sinhala Letter Dantaja Layanna
    case 0xDC0: return 'v'; // Sinhala Letter Vayanna
    case 0xDC3: return 's'; // Sinhala Letter Dantaja Sayanna
    case 0xDC4: return 'h'; // Sinhala Letter Hayanna
    case 0xDCA: return 0; // Sinhala Sign Al-Lakuna
    case 0xDD2: return 'i'; // Sinhala Vowel Sign Ketti Is-Pilla
    case 0xDD4: return 'a'; // Sinhala Vowel Sign Ketti Paa-Pilla
    case 0xE81: return 'k'; // Lao Letter Ko
    case 0xE9E: return 'p'; // Lao Letter Pho Tam
    case 0xEA5: return 'l'; // Lao Letter Lo Loot
    case 0xEA7: return 'w'; // Lao Letter Wo
    case 0xEAA: return 's'; // Lao Letter So Sung
    case 0xEAB: return 'h'; // Lao Letter Ho Sung
    case 0xEB1: return 'a'; // Lao Vowel Sign Mai Kan
    case 0xEB2: return 'a'; // Lao Vowel Sign Aa
    case 0xEBC: return 'l'; // Lao Semivowel Sign Lo
    case 0xEC9: return 0; // Lao Tone Mai Tho
    case 0xEDC: return 'h'; // Lao Ho No
    case 0xF0B: return ' '; // Tibetan Mark Intersyllabic Tsheg
    case 0xF0D: return '|'; // Tibetan Mark Shad
    case 0xF41: return 'k'; // Tibetan Letter Kha
    case 0xF42: return 'g'; // Tibetan Letter Ga
    case 0xF44: return 'n'; // Tibetan Letter Nga
    case 0xF47: return 'j'; // Tibetan Letter Ja
    case 0xF51: return 'd'; // Tibetan Letter Da
    case 0xF56: return 'b'; // Tibetan Letter Ba
    case 0xF58: return 'm'; // Tibetan Letter Ma
    case 0xF59: return 't'; // Tibetan Letter Tsa
    case 0xF61: return 'y'; // Tibetan Letter Ya
    case 0xF62: return 'r'; // Tibetan Letter Ra
    case 0xF64: return 's'; // Tibetan Letter Sha
    case 0xF66: return 's'; // Tibetan Letter Sa
    case 0xF72: return 'i'; // Tibetan Vowel Sign I
    case 0xF7C: return 'o'; // Tibetan Vowel Sign O
    case 0xFAB: return 'd'; // Tibetan Subjoined Letter Dza
    case 0x102B: return 'a'; // Myanmar Vowel Sign Tall Aa
    case 0x1075: return 'k'; // Myanmar Letter Shan Ka
    case 0x107C: return 'n'; // Myanmar Letter Shan Na
    case 0x107D: return 'p'; // Myanmar Letter Shan Pha
    case 0x1081: return 'h'; // Myanmar Letter Shan Ha
    case 0x1083: return 'a'; // Myanmar Vowel Sign Shan Aa
    case 0x1085: return 'e'; // Myanmar Vowel Sign Shan E Above
    case 0x1086: return 'y'; // Myanmar Vowel Sign Shan Final Y
    case 0x1087: return 0; // Myanmar Sign Shan Tone-2
    case 0x1088: return 0; // Myanmar Sign Shan Tone-3
    case 0x108F: return 0; // Myanmar Sign Rumai Palaung Tone-5
    case 0x10F7: return 'q'; // Georgian Letter Yn
    case 0x1218: return 'm'; // Ethiopic Syllable Ma
    case 0x121B: return 'm'; // Ethiopic Syllable Maa
    case 0x122C: return 'r'; // Ethiopic Syllable Ree
    case 0x122D: return 'r'; // Ethiopic Syllable Re
    case 0x1232: return 's'; // Ethiopic Syllable Si
    case 0x1260: return 'b'; // Ethiopic Syllable Ba
    case 0x1275: return 't'; // Ethiopic Syllable Te
    case 0x1293: return 'n'; // Ethiopic Syllable Naa
    case 0x129B: return 'n'; // Ethiopic Syllable Nyaa
    case 0x12A0: return 'a'; // Ethiopic Syllable Glottal A
    case 0x12CB: return 'w'; // Ethiopic Syllable Waa
    case 0x12CD: return 'w'; // Ethiopic Syllable We
    case 0x12F3: return 'd'; // Ethiopic Syllable Daa
    case 0x1308: return 'g'; // Ethiopic Syllable Ga
    case 0x130D: return 'g'; // Ethiopic Syllable Ge
    case 0x133D: return 't'; // Ethiopic Syllable Tse
    case 0x13A4: return 'u'; // Cherokee Letter U
    case 0x13A9: return 'g'; // Cherokee Letter Gi
    case 0x13AE: return 'h'; // Cherokee Letter He
    case 0x13B3: return 'l'; // Cherokee Letter La
    case 0x13B5: return 'l'; // Cherokee Letter Li
    case 0x13CD: return 's'; // Cherokee Letter S
    case 0x13D7: return 'd'; // Cherokee Letter Di
    case 0x13E3: return 't'; // Cherokee Letter Tsa
    case 0x1403: return 'i'; // Canadian Syllabics I
    case 0x140A: return 'a'; // Canadian Syllabics A
    case 0x140D: return 'w'; // Canadian Syllabics West-Cree We
    case 0x140F: return 'w'; // Canadian Syllabics West-Cree Wi
    case 0x1423: return '\''; // Canadian Syllabics Final Right Half Ring
    case 0x1426: return '\''; // Canadian Syllabics Final Double Short Vertical Strokes
    case 0x1439: return 'p'; // Canadian Syllabics Paa
    case 0x144C: return 't'; // Canadian Syllabics Te
    case 0x144E: return 't'; // Canadian Syllabics Ti
    case 0x1450: return 't'; // Canadian Syllabics To
    case 0x1455: return 't'; // Canadian Syllabics Ta
    case 0x1466: return 't'; // Canadian Syllabics T
    case 0x1472: return 'k'; // Canadian Syllabics Ka
    case 0x1483: return 'k'; // Canadian Syllabics K
    case 0x148B: return 'c'; // Canadian Syllabics Ci
    case 0x14A5: return 'm'; // Canadian Syllabics Mi
    case 0x14BB: return 'm'; // Canadian Syllabics M
    case 0x14C0: return 'n'; // Canadian Syllabics Ne
    case 0x14C2: return 'n'; // Canadian Syllabics Ni
    case 0x14C3: return 'n'; // Canadian Syllabics Nii
    case 0x14C4: return 'n'; // Canadian Syllabics No
    case 0x14D0: return 'n'; // Canadian Syllabics N
    case 0x1505: return 's'; // Canadian Syllabics S
    case 0x1525: return 's'; // Canadian Syllabics Sh
    case 0x152D: return 'y'; // Canadian Syllabics Ya
    case 0x1585: return 'q'; // Canadian Syllabics Q
    case 0x1781: return 'k'; // Khmer Letter Kha
    case 0x178A: return 'd'; // Khmer Letter Da
    case 0x1791: return 't'; // Khmer Letter To
    case 0x1796: return 'p'; // Khmer Letter Po
    case 0x1797: return 'p'; // Khmer Letter Pho
    case 0x1798: return 'm'; // Khmer Letter Mo
    case 0x179A: return 'r'; // Khmer Letter Ro
    case 0x179F: return 's'; // Khmer Letter Sa
    case 0x17B6: return 'a'; // Khmer Vowel Sign Aa
    case 0x17BE: return 'o'; // Khmer Vowel Sign Oe
    case 0x17C2: return 'a'; // Khmer Vowel Sign Ae
    case 0x17C6: return 'n'; // Khmer Sign Nikahit
    case 0x17D0: return 'n'; // Khmer Sign Samyok Sannya
    case 0x17D2: return 0; // Khmer Sign Coeng
    case 0x1951: return 'x'; // Tai Le Letter Xa
    case 0x1952: return 'n'; // Tai Le Letter Nga
    case 0x1956: return 't'; // Tai Le Letter Ta
    case 0x1958: return 'l'; // Tai Le Letter La
    case 0x195D: return 'v'; // Tai Le Letter Va
    case 0x195E: return 'h'; // Tai Le Letter Ha
    case 0x1963: return 'a'; // Tai Le Letter A
    case 0x1965: return 'e'; // Tai Le Letter Ee
    case 0x1968: return 'o'; // Tai Le Letter Oo
    case 0x196C: return 'a'; // Tai Le Letter Aue
    case 0x196D: return 'a'; // Tai Le Letter Ai
    case 0x1970: return 0; // Tai Le Letter Tone-2
    case 0x1972: return 0; // Tai Le Letter Tone-4
    case 0x1973: return 0; // Tai Le Letter Tone-5
    case 0x1974: return 0; // Tai Le Letter Tone-6
    case 0x1C5B: return 'a'; // Ol Chiki Letter At
    case 0x1C5F: return 'l'; // Ol Chiki Letter Laa
    case 0x1C62: return 'a'; // Ol Chiki Letter Aam
    case 0x1C64: return 'l'; // Ol Chiki Letter Li
    case 0x1C65: return 's'; // Ol Chiki Letter Is
    case 0x1C66: return 'h'; // Ol Chiki Letter Ih
    case 0x1C69: return 'l'; // Ol Chiki Letter Lu
    case 0x1C6C: return 'n'; // Ol Chiki Letter Unn
    case 0x1C71: return 'n'; // Ol Chiki Letter En
    case 0x1C72: return 'r'; // Ol Chiki Letter Err
    case 0x1C74: return 't'; // Ol Chiki Letter Ott
    case 0x2C02: return 'v'; // Glagolitic Capital Letter Vede
    case 0x2C0D: return 'k'; // Glagolitic Capital Letter Kako
    case 0x2C0E: return 'l'; // Glagolitic Capital Letter Ljudije
    case 0x2C10: return 'n'; // Glagolitic Capital Letter Nashi
    case 0x2C11: return 'o'; // Glagolitic Capital Letter Onu
    case 0x2C14: return 's'; // Glagolitic Capital Letter Slovo
    case 0x2C1F: return 'y'; // Glagolitic Capital Letter Yeru
    case 0x2C20: return 'i'; // Glagolitic Capital Letter Yeri
    case 0x2C21: return 'e'; // Glagolitic Capital Letter Yati
    case 0xA80D: return 'c'; // Syloti Nagri Letter Cho
    case 0xA810: return 't'; // Syloti Nagri Letter Tto
    case 0xA814: return 't'; // Syloti Nagri Letter To
    case 0xA81A: return 'p'; // Syloti Nagri Letter Pho
    case 0xA81D: return 'm'; // Syloti Nagri Letter Mo
    case 0xA81F: return 'l'; // Syloti Nagri Letter Lo
    case 0xA823: return 'a'; // Syloti Nagri Vowel Sign A
    case 0xA824: return 'i'; // Syloti Nagri Vowel Sign I
    case 0xA825: return 'u'; // Syloti Nagri Vowel Sign U
    case 0xABC2: return 'l'; // Meetei Mayek Letter Lai
    case 0xABC3: return 'm'; // Meetei Mayek Letter Mit
    case 0xABC7: return 't'; // Meetei Mayek Letter Til
    case 0xABCF: return 'i'; // Meetei Mayek Letter I
    case 0xABD1: return 'a'; // Meetei Mayek Letter Atiya
    case 0xABD4: return 'r'; // Meetei Mayek Letter Rai
    case 0xABD5: return 'b'; // Meetei Mayek Letter Ba
    case 0xABDF: return 'n'; // Meetei Mayek Letter Na Lonsum
    case 0xABE3: return 'o'; // Meetei Mayek Vowel Sign Onap
    case 0xABE4: return 'i'; // Meetei Mayek Vowel Sign Inap
    case 0xABE5: return 'a'; // Meetei Mayek Vowel Sign Anap
    case 0xABE8: return 'u'; // Meetei Mayek Vowel Sign Unap
    case 0xABE9: return 'e'; // Meetei Mayek Vowel Sign Cheinap
    // Gothic (astral — needs the 4-byte decoder; Gutisk interlanguage links).
    case 0x10330: return 'a'; // Gothic Letter Ahsa
    case 0x10332: return 'g'; // Gothic Letter Giba
    case 0x10333: return 'd'; // Gothic Letter Dags
    case 0x10334: return 'e'; // Gothic Letter Aihvus
    case 0x10339: return 'i'; // Gothic Letter Eis
    case 0x1033A: return 'k'; // Gothic Letter Kusma
    case 0x1033B: return 'l'; // Gothic Letter Lagus
    case 0x1033D: return 'n'; // Gothic Letter Nauths
    case 0x1033F: return 'u'; // Gothic Letter Urus
    case 0x10343: return 's'; // Gothic Letter Sauil
    case 0x10344: return 't'; // Gothic Letter Teiws
    case 0x10346: return 'f'; // Gothic Letter Faihu
    case 0x10349: return 'o'; // Gothic Letter Othal
    // Wikipedia i18n push 2026-10-01: full-script transliteration
    // (transliterate, never ?; drops are invisible marks). Generated
    // from Unicode names with per-script review.
    case 0xA4: return '$'; // Currency Sign
    case 0xB2: return '2'; // Superscript Two
    case 0xB3: return '3'; // Superscript Three
    case 0x180: return 'b'; // Latin Small Letter B With Stroke
    case 0x181: return 'B'; // Latin Capital Letter B With Hook
    case 0x182: return 'B'; // Latin Capital Letter B With Topbar
    case 0x183: return 'b'; // Latin Small Letter B With Topbar
    case 0x184: return 0; // Latin Capital Letter Tone Six
    case 0x185: return 0; // Latin Small Letter Tone Six
    case 0x186: return 'O'; // Latin Capital Letter Open O
    case 0x187: return 'C'; // Latin Capital Letter C With Hook
    case 0x188: return 'c'; // Latin Small Letter C With Hook
    case 0x189: return 'A'; // Latin Capital Letter African D
    case 0x18A: return 'D'; // Latin Capital Letter D With Hook
    case 0x18B: return 'D'; // Latin Capital Letter D With Topbar
    case 0x18C: return 'd'; // Latin Small Letter D With Topbar
    case 0x18D: return 'd'; // Latin Small Letter Turned Delta
    case 0x18E: return 'e'; // Latin Capital Letter Reversed E
    case 0x18F: return 'S'; // Latin Capital Letter Schwa
    case 0x190: return 'E'; // Latin Capital Letter Open E
    case 0x191: return 'F'; // Latin Capital Letter F With Hook
    case 0x192: return 'f'; // Latin Small Letter F With Hook
    case 0x193: return 'G'; // Latin Capital Letter G With Hook
    case 0x194: return 'G'; // Latin Capital Letter Gamma
    case 0x195: return 'h'; // Latin Small Letter Hv
    case 0x196: return 'I'; // Latin Capital Letter Iota
    case 0x197: return 'I'; // Latin Capital Letter I With Stroke
    case 0x198: return 'K'; // Latin Capital Letter K With Hook
    case 0x199: return 'k'; // Latin Small Letter K With Hook
    case 0x19A: return 'l'; // Latin Small Letter L With Bar
    case 0x19B: return 'l'; // Latin Small Letter Lambda With Stroke
    case 0x19C: return 'W'; // Latin Capital Letter Turned M
    case 0x19D: return 'n'; // Latin Capital Letter N With Left Hook
    case 0x19E: return 'n'; // Latin Small Letter N With Long Right Leg
    case 0x19F: return 'O'; // Latin Capital Letter O With Middle Tilde
    case 0x1A0: return 'O'; // Latin Capital Letter O With Horn
    case 0x1A1: return 'o'; // Latin Small Letter O With Horn
    case 0x1A2: return 'O'; // Latin Capital Letter Oi
    case 0x1A3: return 'o'; // Latin Small Letter Oi
    case 0x1A4: return 'P'; // Latin Capital Letter P With Hook
    case 0x1A5: return 'p'; // Latin Small Letter P With Hook
    case 0x1A6: return 'y'; // Latin Letter Yr
    case 0x1A7: return 0; // Latin Capital Letter Tone Two
    case 0x1A8: return 0; // Latin Small Letter Tone Two
    case 0x1A9: return 'E'; // Latin Capital Letter Esh
    case 0x1AA: return 'r'; // Latin Letter Reversed Esh Loop
    case 0x1AB: return 't'; // Latin Small Letter T With Palatal Hook
    case 0x1AC: return 'T'; // Latin Capital Letter T With Hook
    case 0x1AD: return 't'; // Latin Small Letter T With Hook
    case 0x1AE: return 'T'; // Latin Capital Letter T With Retroflex Hook
    case 0x1AF: return 'U'; // Latin Capital Letter U With Horn
    case 0x1B0: return 'u'; // Latin Small Letter U With Horn
    case 0x1B1: return 'U'; // Latin Capital Letter Upsilon
    case 0x1B2: return 'V'; // Latin Capital Letter V With Hook
    case 0x1B3: return 'Y'; // Latin Capital Letter Y With Hook
    case 0x1B4: return 'y'; // Latin Small Letter Y With Hook
    case 0x1B5: return 'Z'; // Latin Capital Letter Z With Stroke
    case 0x1B6: return 'z'; // Latin Small Letter Z With Stroke
    case 0x1B7: return 'Z'; // Latin Capital Letter Ezh
    case 0x1B8: return 'Z'; // Latin Capital Letter Ezh Reversed
    case 0x1B9: return 'z'; // Latin Small Letter Ezh Reversed
    case 0x1BA: return 'z'; // Latin Small Letter Ezh With Tail
    case 0x1BB: return 'o'; // Latin Letter Two With Stroke
    case 0x1BC: return 0; // Latin Capital Letter Tone Five
    case 0x1BD: return 0; // Latin Small Letter Tone Five
    case 0x1BE: return '\''; // Latin Letter Inverted Glottal Stop With Stroke
    case 0x1BF: return 'w'; // Latin Letter Wynn
    case 0x1C0: return '|'; // Latin Letter Dental Click
    case 0x1C1: return '|'; // Latin Letter Lateral Click
    case 0x1C2: return '='; // Latin Letter Alveolar Click
    case 0x1C3: return '!'; // Latin Letter Retroflex Click
    case 0x1C4: return 'D'; // Latin Capital Letter Dz With Caron
    case 0x1C5: return 'D'; // Latin Capital Letter D With Small Letter Z With Caron
    case 0x1C6: return 'd'; // Latin Small Letter Dz With Caron
    case 0x1C7: return 'L'; // Latin Capital Letter Lj
    case 0x1C8: return 'L'; // Latin Capital Letter L With Small Letter J
    case 0x1C9: return 'l'; // Latin Small Letter Lj
    case 0x1CA: return 'N'; // Latin Capital Letter Nj
    case 0x1CB: return 'N'; // Latin Capital Letter N With Small Letter J
    case 0x1CC: return 'n'; // Latin Small Letter Nj
    case 0x1CD: return 'A'; // Latin Capital Letter A With Caron
    case 0x1CF: return 'I'; // Latin Capital Letter I With Caron
    case 0x1D0: return 'i'; // Latin Small Letter I With Caron
    case 0x1D1: return 'O'; // Latin Capital Letter O With Caron
    case 0x1D2: return 'o'; // Latin Small Letter O With Caron
    case 0x1D3: return 'U'; // Latin Capital Letter U With Caron
    case 0x1D5: return 'U'; // Latin Capital Letter U With Diaeresis And Macron
    case 0x1D6: return 'u'; // Latin Small Letter U With Diaeresis And Macron
    case 0x1D7: return 'U'; // Latin Capital Letter U With Diaeresis And Acute
    case 0x1D8: return 'u'; // Latin Small Letter U With Diaeresis And Acute
    case 0x1D9: return 'U'; // Latin Capital Letter U With Diaeresis And Caron
    case 0x1DA: return 'u'; // Latin Small Letter U With Diaeresis And Caron
    case 0x1DB: return 'U'; // Latin Capital Letter U With Diaeresis And Grave
    case 0x1DC: return 'u'; // Latin Small Letter U With Diaeresis And Grave
    case 0x1DD: return 'e'; // Latin Small Letter Turned E
    case 0x1DE: return 'A'; // Latin Capital Letter A With Diaeresis And Macron
    case 0x1DF: return 'a'; // Latin Small Letter A With Diaeresis And Macron
    case 0x1E0: return 'A'; // Latin Capital Letter A With Dot Above And Macron
    case 0x1E1: return 'a'; // Latin Small Letter A With Dot Above And Macron
    case 0x1E2: return 'A'; // Latin Capital Letter Ae With Macron
    case 0x1E3: return 'a'; // Latin Small Letter Ae With Macron
    case 0x1E4: return 'G'; // Latin Capital Letter G With Stroke
    case 0x1E5: return 'g'; // Latin Small Letter G With Stroke
    case 0x1E6: return 'G'; // Latin Capital Letter G With Caron
    case 0x1E7: return 'g'; // Latin Small Letter G With Caron
    case 0x1E8: return 'K'; // Latin Capital Letter K With Caron
    case 0x1E9: return 'k'; // Latin Small Letter K With Caron
    case 0x1EA: return 'O'; // Latin Capital Letter O With Ogonek
    case 0x1EB: return 'o'; // Latin Small Letter O With Ogonek
    case 0x1EC: return 'O'; // Latin Capital Letter O With Ogonek And Macron
    case 0x1ED: return 'o'; // Latin Small Letter O With Ogonek And Macron
    case 0x1EE: return 'E'; // Latin Capital Letter Ezh With Caron
    case 0x1EF: return 'e'; // Latin Small Letter Ezh With Caron
    case 0x1F0: return 'j'; // Latin Small Letter J With Caron
    case 0x1F1: return 'D'; // Latin Capital Letter Dz
    case 0x1F2: return 'D'; // Latin Capital Letter D With Small Letter Z
    case 0x1F3: return 'd'; // Latin Small Letter Dz
    case 0x1F4: return 'G'; // Latin Capital Letter G With Acute
    case 0x1F5: return 'g'; // Latin Small Letter G With Acute
    case 0x1F6: return 'H'; // Latin Capital Letter Hwair
    case 0x1F7: return 'W'; // Latin Capital Letter Wynn
    case 0x1F8: return 'N'; // Latin Capital Letter N With Grave
    case 0x1F9: return 'n'; // Latin Small Letter N With Grave
    case 0x1FA: return 'A'; // Latin Capital Letter A With Ring Above And Acute
    case 0x1FB: return 'a'; // Latin Small Letter A With Ring Above And Acute
    case 0x1FC: return 'A'; // Latin Capital Letter Ae With Acute
    case 0x1FD: return 'a'; // Latin Small Letter Ae With Acute
    case 0x1FE: return 'O'; // Latin Capital Letter O With Stroke And Acute
    case 0x1FF: return 'o'; // Latin Small Letter O With Stroke And Acute
    case 0x200: return 'A'; // Latin Capital Letter A With Double Grave
    case 0x201: return 'a'; // Latin Small Letter A With Double Grave
    case 0x202: return 'A'; // Latin Capital Letter A With Inverted Breve
    case 0x203: return 'a'; // Latin Small Letter A With Inverted Breve
    case 0x204: return 'E'; // Latin Capital Letter E With Double Grave
    case 0x205: return 'e'; // Latin Small Letter E With Double Grave
    case 0x206: return 'E'; // Latin Capital Letter E With Inverted Breve
    case 0x207: return 'e'; // Latin Small Letter E With Inverted Breve
    case 0x208: return 'I'; // Latin Capital Letter I With Double Grave
    case 0x209: return 'i'; // Latin Small Letter I With Double Grave
    case 0x20A: return 'I'; // Latin Capital Letter I With Inverted Breve
    case 0x20B: return 'i'; // Latin Small Letter I With Inverted Breve
    case 0x20C: return 'O'; // Latin Capital Letter O With Double Grave
    case 0x20D: return 'o'; // Latin Small Letter O With Double Grave
    case 0x20E: return 'O'; // Latin Capital Letter O With Inverted Breve
    case 0x20F: return 'o'; // Latin Small Letter O With Inverted Breve
    case 0x210: return 'R'; // Latin Capital Letter R With Double Grave
    case 0x211: return 'r'; // Latin Small Letter R With Double Grave
    case 0x212: return 'R'; // Latin Capital Letter R With Inverted Breve
    case 0x213: return 'r'; // Latin Small Letter R With Inverted Breve
    case 0x214: return 'U'; // Latin Capital Letter U With Double Grave
    case 0x215: return 'u'; // Latin Small Letter U With Double Grave
    case 0x216: return 'U'; // Latin Capital Letter U With Inverted Breve
    case 0x217: return 'u'; // Latin Small Letter U With Inverted Breve
    case 0x21C: return 'y'; // Latin Capital Letter Yogh
    case 0x21D: return 'y'; // Latin Small Letter Yogh
    case 0x21E: return 'H'; // Latin Capital Letter H With Caron
    case 0x21F: return 'h'; // Latin Small Letter H With Caron
    case 0x220: return 'N'; // Latin Capital Letter N With Long Right Leg
    case 0x221: return 'd'; // Latin Small Letter D With Curl
    case 0x222: return 'O'; // Latin Capital Letter Ou
    case 0x223: return 'o'; // Latin Small Letter Ou
    case 0x224: return 'Z'; // Latin Capital Letter Z With Hook
    case 0x225: return 'z'; // Latin Small Letter Z With Hook
    case 0x226: return 'A'; // Latin Capital Letter A With Dot Above
    case 0x227: return 'a'; // Latin Small Letter A With Dot Above
    case 0x228: return 'E'; // Latin Capital Letter E With Cedilla
    case 0x229: return 'e'; // Latin Small Letter E With Cedilla
    case 0x22A: return 'O'; // Latin Capital Letter O With Diaeresis And Macron
    case 0x22B: return 'o'; // Latin Small Letter O With Diaeresis And Macron
    case 0x22C: return 'O'; // Latin Capital Letter O With Tilde And Macron
    case 0x22D: return 'o'; // Latin Small Letter O With Tilde And Macron
    case 0x22E: return 'O'; // Latin Capital Letter O With Dot Above
    case 0x22F: return 'o'; // Latin Small Letter O With Dot Above
    case 0x230: return 'O'; // Latin Capital Letter O With Dot Above And Macron
    case 0x231: return 'o'; // Latin Small Letter O With Dot Above And Macron
    case 0x232: return 'Y'; // Latin Capital Letter Y With Macron
    case 0x233: return 'y'; // Latin Small Letter Y With Macron
    case 0x234: return 'l'; // Latin Small Letter L With Curl
    case 0x235: return 'n'; // Latin Small Letter N With Curl
    case 0x236: return 't'; // Latin Small Letter T With Curl
    case 0x237: return 'j'; // Latin Small Letter Dotless J
    case 0x238: return 'd'; // Latin Small Letter Db Digraph
    case 0x239: return 'q'; // Latin Small Letter Qp Digraph
    case 0x23A: return 'A'; // Latin Capital Letter A With Stroke
    case 0x23B: return 'C'; // Latin Capital Letter C With Stroke
    case 0x23C: return 'c'; // Latin Small Letter C With Stroke
    case 0x23D: return 'L'; // Latin Capital Letter L With Bar
    case 0x23E: return 'T'; // Latin Capital Letter T With Diagonal Stroke
    case 0x23F: return 's'; // Latin Small Letter S With Swash Tail
    case 0x240: return 'z'; // Latin Small Letter Z With Swash Tail
    case 0x241: return '\''; // Latin Capital Letter Glottal Stop
    case 0x242: return '\''; // Latin Small Letter Glottal Stop
    case 0x243: return 'B'; // Latin Capital Letter B With Stroke
    case 0x244: return 'U'; // Latin Capital Letter U Bar
    case 0x245: return 'U'; // Latin Capital Letter Turned V
    case 0x246: return 'E'; // Latin Capital Letter E With Stroke
    case 0x247: return 'e'; // Latin Small Letter E With Stroke
    case 0x248: return 'J'; // Latin Capital Letter J With Stroke
    case 0x249: return 'j'; // Latin Small Letter J With Stroke
    case 0x24A: return 'S'; // Latin Capital Letter Small Q With Hook Tail
    case 0x24B: return 'q'; // Latin Small Letter Q With Hook Tail
    case 0x24C: return 'R'; // Latin Capital Letter R With Stroke
    case 0x24D: return 'r'; // Latin Small Letter R With Stroke
    case 0x24E: return 'Y'; // Latin Capital Letter Y With Stroke
    case 0x24F: return 'y'; // Latin Small Letter Y With Stroke
    case 0x2C9: return 0; // Modifier Letter Macron
    case 0x2CA: return 0; // Modifier Letter Acute Accent
    case 0x2CB: return 0; // Modifier Letter Grave Accent
    case 0x2D9: return 0; // Dot Above
    case 0x302: return 0; // Combining Circumflex Accent
    case 0x303: return 0; // Combining Tilde
    case 0x306: return 0; // Combining Breve
    case 0x307: return 0; // Combining Dot Above
    case 0x308: return 0; // Combining Diaeresis
    case 0x30A: return 0; // Combining Ring Above
    case 0x30C: return 0; // Combining Caron
    case 0x313: return '\''; // Combining Comma Above
    case 0x314: return '\''; // Combining Reversed Comma Above
    case 0x323: return 0; // Combining Dot Below
    case 0x327: return 0; // Combining Cedilla
    case 0x328: return 0; // Combining Ogonek
    case 0x32D: return 0; // Combining Circumflex Accent Below
    case 0x32E: return 0; // Combining Breve Below
    case 0x32F: return 0; // Combining Inverted Breve Below
    case 0x330: return 0; // Combining Tilde Below
    case 0x332: return 0; // Combining Low Line
    case 0x333: return 0; // Combining Double Low Line
    case 0x334: return 0; // Combining Tilde Overlay
    case 0x335: return 0; // Combining Short Stroke Overlay
    case 0x336: return 0; // Combining Long Stroke Overlay
    case 0x337: return 0; // Combining Short Solidus Overlay
    case 0x338: return 0; // Combining Long Solidus Overlay
    case 0x339: return 0; // Combining Right Half Ring Below
    case 0x33A: return 0; // Combining Inverted Bridge Below
    case 0x33B: return 0; // Combining Square Below
    case 0x33C: return 0; // Combining Seagull Below
    case 0x33D: return 0; // Combining X Above
    case 0x33E: return 0; // Combining Vertical Tilde
    case 0x33F: return 0; // Combining Double Overline
    case 0x340: return 0; // Combining Grave Tone Mark
    case 0x341: return 0; // Combining Acute Tone Mark
    case 0x343: return '\''; // Combining Greek Koronis
    case 0x344: return '"'; // Combining Greek Dialytika Tonos
    case 0x345: return 'i'; // Combining Greek Ypogegrammeni
    case 0x350: return 0; // Combining Right Arrowhead Above
    case 0x351: return 0; // Combining Left Half Ring Above
    case 0x352: return 0; // Combining Fermata
    case 0x353: return 0; // Combining X Below
    case 0x354: return 0; // Combining Left Arrowhead Below
    case 0x355: return 0; // Combining Right Arrowhead Below
    case 0x356: return 0; // Combining Right Arrowhead And Up Arrowhead Below
    case 0x357: return 0; // Combining Right Half Ring Above
    case 0x358: return 0; // Combining Dot Above Right
    case 0x359: return 0; // Combining Asterisk Below
    case 0x35A: return 0; // Combining Double Ring Below
    case 0x35B: return 0; // Combining Zigzag Above
    case 0x35C: return 0; // Combining Double Breve Below
    case 0x35D: return 0; // Combining Double Breve
    case 0x35E: return 0; // Combining Double Macron
    case 0x35F: return 0; // Combining Double Macron Below
    case 0x360: return 0; // Combining Double Tilde
    case 0x361: return 0; // Combining Double Inverted Breve
    case 0x362: return 0; // Combining Double Rightwards Arrow Below
    case 0x363: return 0; // Combining Latin Small Letter A
    case 0x364: return 0; // Combining Latin Small Letter E
    case 0x365: return 0; // Combining Latin Small Letter I
    case 0x366: return 0; // Combining Latin Small Letter O
    case 0x367: return 0; // Combining Latin Small Letter U
    case 0x368: return 0; // Combining Latin Small Letter C
    case 0x369: return 0; // Combining Latin Small Letter D
    case 0x36A: return 0; // Combining Latin Small Letter H
    case 0x36B: return 0; // Combining Latin Small Letter M
    case 0x36C: return 0; // Combining Latin Small Letter R
    case 0x36D: return 0; // Combining Latin Small Letter T
    case 0x36E: return 0; // Combining Latin Small Letter V
    case 0x36F: return 0; // Combining Latin Small Letter X
    case 0x370: return 'h'; // Greek Capital Letter Heta
    case 0x371: return 'h'; // Greek Small Letter Heta
    case 0x372: return 's'; // Greek Capital Letter Archaic Sampi
    case 0x374: return 0; // Greek Numeral Sign
    case 0x375: return 0; // Greek Lower Numeral Sign
    case 0x376: return 'w'; // Greek Capital Letter Pamphylian Digamma
    case 0x377: return 'w'; // Greek Small Letter Pamphylian Digamma
    case 0x37E: return ';'; // Greek Question Mark
    case 0x37F: return 'j'; // Greek Capital Letter Yot
    case 0x3CF: return '&'; // Greek Capital Kai Symbol
    case 0x3D7: return '&'; // Greek Kai Symbol
    case 0x3D9: return 's'; // Greek Small Letter Archaic Koppa
    case 0x3DA: return 's'; // Greek Letter Stigma
    case 0x3DB: return 's'; // Greek Small Letter Stigma
    case 0x3DC: return 'g'; // Greek Letter Digamma
    case 0x3DD: return 'g'; // Greek Small Letter Digamma
    case 0x3DE: return 'k'; // Greek Letter Koppa
    case 0x3DF: return 'k'; // Greek Small Letter Koppa
    case 0x3E0: return 's'; // Greek Letter Sampi
    case 0x3E1: return 's'; // Greek Small Letter Sampi
    case 0x3F3: return 'j'; // Greek Letter Yot
    case 0x3F6: return 's'; // Greek Reversed Lunate Epsilon Symbol
    case 0x3F7: return 's'; // Greek Capital Letter Sho
    case 0x3F8: return 's'; // Greek Small Letter Sho
    case 0x3FA: return 's'; // Greek Capital Letter San
    case 0x3FD: return 's'; // Greek Capital Reversed Lunate Sigma Symbol
    case 0x3FE: return 's'; // Greek Capital Dotted Lunate Sigma Symbol
    case 0x3FF: return 's'; // Greek Capital Reversed Dotted Lunate Sigma Symbol
    case 0x70F: return '.'; // Syriac Abbreviation Mark
    case 0x711: return 's'; // Syriac Letter Superscript Alaph
    case 0x712: return 'b'; // Syriac Letter Beth
    case 0x713: return 'g'; // Syriac Letter Gamal
    case 0x714: return 'g'; // Syriac Letter Gamal Garshuni
    case 0x715: return 'd'; // Syriac Letter Dalath
    case 0x716: return 'd'; // Syriac Letter Dotless Dalath Rish
    case 0x717: return 'h'; // Syriac Letter He
    case 0x719: return 'z'; // Syriac Letter Zain
    case 0x71A: return 'h'; // Syriac Letter Heth
    case 0x71B: return 't'; // Syriac Letter Teth
    case 0x71C: return 't'; // Syriac Letter Teth Garshuni
    case 0x71E: return 'y'; // Syriac Letter Yudh He
    case 0x71F: return 'k'; // Syriac Letter Kaph
    case 0x720: return 'l'; // Syriac Letter Lamadh
    case 0x722: return 'n'; // Syriac Letter Nun
    case 0x723: return 's'; // Syriac Letter Semkath
    case 0x724: return 'f'; // Syriac Letter Final Semkath
    case 0x725: return 'e'; // Syriac Letter E
    case 0x727: return 'r'; // Syriac Letter Reversed Pe
    case 0x728: return 's'; // Syriac Letter Sadhe
    case 0x729: return 'q'; // Syriac Letter Qaph
    case 0x72D: return 'p'; // Syriac Letter Persian Bheth
    case 0x72E: return 'p'; // Syriac Letter Persian Ghamal
    case 0x72F: return 'p'; // Syriac Letter Persian Dhalath
    case 0x74D: return 's'; // Syriac Letter Sogdian Zhain
    case 0x74E: return 's'; // Syriac Letter Sogdian Khaph
    case 0x74F: return 's'; // Syriac Letter Sogdian Fe
    case 0x781: return 's'; // Thaana Letter Shaviyani
    case 0x782: return 'n'; // Thaana Letter Noonu
    case 0x783: return 'r'; // Thaana Letter Raa
    case 0x785: return 'l'; // Thaana Letter Lhaviyani
    case 0x786: return 'k'; // Thaana Letter Kaafu
    case 0x78C: return 't'; // Thaana Letter Thaa
    case 0x78D: return 'l'; // Thaana Letter Laamu
    case 0x78E: return 'g'; // Thaana Letter Gaafu
    case 0x78F: return 'g'; // Thaana Letter Gnaviyani
    case 0x791: return 'd'; // Thaana Letter Daviyani
    case 0x792: return 'z'; // Thaana Letter Zaviyani
    case 0x793: return 't'; // Thaana Letter Taviyani
    case 0x794: return 'y'; // Thaana Letter Yaa
    case 0x795: return 'p'; // Thaana Letter Paviyani
    case 0x796: return 'j'; // Thaana Letter Javiyani
    case 0x797: return 'c'; // Thaana Letter Chaviyani
    case 0x798: return 't'; // Thaana Letter Ttaa
    case 0x79A: return 'k'; // Thaana Letter Khaa
    case 0x79B: return 't'; // Thaana Letter Thaalu
    case 0x79C: return 'z'; // Thaana Letter Zaa
    case 0x79D: return 's'; // Thaana Letter Sheenu
    case 0x79F: return 'd'; // Thaana Letter Daadhu
    case 0x7A0: return 't'; // Thaana Letter To
    case 0x7A1: return 'z'; // Thaana Letter Zo
    case 0x7A2: return 'a'; // Thaana Letter Ainu
    case 0x7A3: return 'g'; // Thaana Letter Ghainu
    case 0x7A4: return 'q'; // Thaana Letter Qaafu
    case 0x7A5: return 'w'; // Thaana Letter Waavu
    case 0x7B1: return 'n'; // Thaana Letter Naa
    case 0x7C0: return '0'; // Nko Digit Zero
    case 0x7C1: return '1'; // Nko Digit One
    case 0x7C2: return '2'; // Nko Digit Two
    case 0x7C3: return '3'; // Nko Digit Three
    case 0x7C4: return '4'; // Nko Digit Four
    case 0x7C5: return '5'; // Nko Digit Five
    case 0x7C6: return '6'; // Nko Digit Six
    case 0x7C7: return '7'; // Nko Digit Seven
    case 0x7C8: return '8'; // Nko Digit Eight
    case 0x7C9: return '9'; // Nko Digit Nine
    case 0x7CB: return 'e'; // Nko Letter Ee
    case 0x7CC: return 'i'; // Nko Letter I
    case 0x7CD: return 'e'; // Nko Letter E
    case 0x7CE: return 'u'; // Nko Letter U
    case 0x7D0: return 'o'; // Nko Letter O
    case 0x7D1: return 'd'; // Nko Letter Dagbasinna
    case 0x7D4: return 'p'; // Nko Letter Pa
    case 0x7D5: return 't'; // Nko Letter Ta
    case 0x7D6: return 'j'; // Nko Letter Ja
    case 0x7D7: return 'c'; // Nko Letter Cha
    case 0x7D9: return 'r'; // Nko Letter Ra
    case 0x7DA: return 'r'; // Nko Letter Rra
    case 0x7DB: return 's'; // Nko Letter Sa
    case 0x7DC: return 'g'; // Nko Letter Gba
    case 0x7DD: return 'f'; // Nko Letter Fa
    case 0x7E0: return 'n'; // Nko Letter Na Woloso
    case 0x7E1: return 'm'; // Nko Letter Ma
    case 0x7E2: return 'n'; // Nko Letter Nya
    case 0x7E3: return 'n'; // Nko Letter Na
    case 0x7E4: return 'h'; // Nko Letter Ha
    case 0x7E5: return 'w'; // Nko Letter Wa
    case 0x7E6: return 'y'; // Nko Letter Ya
    case 0x7E7: return 'n'; // Nko Letter Nya Woloso
    case 0x7E8: return 'j'; // Nko Letter Jona Ja
    case 0x7E9: return 'j'; // Nko Letter Jona Cha
    case 0x7EA: return 'j'; // Nko Letter Jona Ra
    case 0x7EB: return 0; // Nko Combining Short High Tone
    case 0x7ED: return 0; // Nko Combining Short Rising Tone
    case 0x7EE: return 0; // Nko Combining Long Descending Tone
    case 0x7EF: return 0; // Nko Combining Long High Tone
    case 0x7F0: return 0; // Nko Combining Long Low Tone
    case 0x7F1: return 0; // Nko Combining Long Rising Tone
    case 0x7F4: return 0; // Nko High Tone Apostrophe
    case 0x7F5: return 0; // Nko Low Tone Apostrophe
    case 0x904: return 'a'; // Devanagari Letter Short A
    case 0x90D: return 'e'; // Devanagari Letter Candra E
    case 0x90E: return 'e'; // Devanagari Letter Short E
    case 0x955: return 'e'; // Devanagari Vowel Sign Candra Long E
    case 0x956: return 'u'; // Devanagari Vowel Sign Ue
    case 0x957: return 'u'; // Devanagari Vowel Sign Uue
    case 0x960: return 'r'; // Devanagari Letter Vocalic Rr
    case 0x961: return 'l'; // Devanagari Letter Vocalic Ll
    case 0x962: return 'l'; // Devanagari Vowel Sign Vocalic L
    case 0x963: return 'l'; // Devanagari Vowel Sign Vocalic Ll
    case 0x966: return '0'; // Devanagari Digit Zero
    case 0x967: return '1'; // Devanagari Digit One
    case 0x968: return '2'; // Devanagari Digit Two
    case 0x969: return '3'; // Devanagari Digit Three
    case 0x96A: return '4'; // Devanagari Digit Four
    case 0x96B: return '5'; // Devanagari Digit Five
    case 0x96C: return '6'; // Devanagari Digit Six
    case 0x96D: return '7'; // Devanagari Digit Seven
    case 0x96E: return '8'; // Devanagari Digit Eight
    case 0x96F: return '9'; // Devanagari Digit Nine
    case 0x971: return '.'; // Devanagari Sign High Spacing Dot
    case 0x972: return 'a'; // Devanagari Letter Candra A
    case 0x979: return 'g'; // Devanagari Letter Zha
    case 0x97A: return 'j'; // Devanagari Letter Heavy Ya
    case 0x97B: return 'g'; // Devanagari Letter Gga
    case 0x97C: return 'j'; // Devanagari Letter Jja
    case 0x97D: return 'd'; // Devanagari Letter Glottal Stop
    case 0x97E: return 'b'; // Devanagari Letter Ddda
    case 0x980: return 'o'; // Bengali Anji
    case 0x983: return 'r'; // Bengali Sign Visarga
    case 0x984: return 'l'; // ?
    case 0x986: return 'a'; // Bengali Letter Aa
    case 0x987: return 'i'; // Bengali Letter I
    case 0x988: return 'i'; // Bengali Letter Ii
    case 0x989: return 'u'; // Bengali Letter U
    case 0x98A: return 'u'; // Bengali Letter Uu
    case 0x98B: return 'r'; // Bengali Letter Vocalic R
    case 0x98C: return 'l'; // Bengali Letter Vocalic L
    case 0x98F: return 'e'; // Bengali Letter E
    case 0x990: return 'a'; // Bengali Letter Ai
    case 0x991: return 'o'; // ?
    case 0x992: return 'o'; // ?
    case 0x993: return 'k'; // Bengali Letter O
    case 0x994: return 'k'; // Bengali Letter Au
    case 0x995: return 'g'; // Bengali Letter Ka
    case 0x996: return 'g'; // Bengali Letter Kha
    case 0x997: return 'n'; // Bengali Letter Ga
    case 0x998: return 'c'; // Bengali Letter Gha
    case 0x999: return 'c'; // Bengali Letter Nga
    case 0x99A: return 'j'; // Bengali Letter Ca
    case 0x99B: return 'j'; // Bengali Letter Cha
    case 0x99C: return 'n'; // Bengali Letter Ja
    case 0x99D: return 't'; // Bengali Letter Jha
    case 0x99E: return 't'; // Bengali Letter Nya
    case 0x9A0: return 't'; // Bengali Letter Ttha
    case 0x9A1: return 'd'; // Bengali Letter Dda
    case 0x9A2: return 'd'; // Bengali Letter Ddha
    case 0x9A5: return 't'; // Bengali Letter Tha
    case 0x9A6: return 't'; // Bengali Letter Da
    case 0x9A9: return 'd'; // ?
    case 0x9AB: return 'n'; // Bengali Letter Pha
    case 0x9AD: return 't'; // Bengali Letter Bha
    case 0x9B1: return 'r'; // ?
    case 0x9B3: return 'l'; // ?
    case 0x9B4: return 'l'; // ?
    case 0x9B5: return 'l'; // ?
    case 0x9B6: return 's'; // Bengali Letter Sha
    case 0x9B9: return 'h'; // Bengali Letter Ha
    case 0x9C2: return 'u'; // Bengali Vowel Sign Uu
    case 0x9C3: return 'r'; // Bengali Vowel Sign Vocalic R
    case 0x9C4: return 'r'; // Bengali Vowel Sign Vocalic Rr
    case 0x9C5: return 'l'; // ?
    case 0x9C6: return 'l'; // ?
    case 0x9C8: return 'o'; // Bengali Vowel Sign Ai
    case 0x9CA: return 'o'; // ?
    case 0x9CB: return 'o'; // Bengali Vowel Sign O
    case 0x9CC: return 'o'; // Bengali Vowel Sign Au
    case 0x9CE: return 't'; // Bengali Letter Khanda Ta
    case 0x9D7: return 0; // Bengali Au Length Mark
    case 0x9DC: return 'd'; // Bengali Letter Rra
    case 0x9DD: return 'd'; // Bengali Letter Rha
    case 0x9DE: return 'y'; // ?
    case 0x9DF: return 'y'; // Bengali Letter Yya
    case 0x9E0: return 'r'; // Bengali Letter Vocalic Rr
    case 0x9E1: return 'l'; // Bengali Letter Vocalic Ll
    case 0x9E2: return 'l'; // Bengali Vowel Sign Vocalic L
    case 0x9E3: return 'l'; // Bengali Vowel Sign Vocalic Ll
    case 0x9E6: return '0'; // Bengali Digit Zero
    case 0x9E7: return '1'; // Bengali Digit One
    case 0x9E8: return '2'; // Bengali Digit Two
    case 0x9E9: return '3'; // Bengali Digit Three
    case 0x9EA: return '4'; // Bengali Digit Four
    case 0x9EB: return '5'; // Bengali Digit Five
    case 0x9EC: return '6'; // Bengali Digit Six
    case 0x9ED: return '7'; // Bengali Digit Seven
    case 0x9EE: return '8'; // Bengali Digit Eight
    case 0x9EF: return '9'; // Bengali Digit Nine
    case 0x9F0: return 'r'; // Bengali Letter Ra With Middle Diagonal
    case 0x9F1: return 'r'; // Bengali Letter Ra With Lower Diagonal
    case 0x9F2: return 'y'; // Bengali Rupee Mark
    case 0x9F3: return 'y'; // Bengali Rupee Sign
    case 0x9F4: return '.'; // Bengali Currency Numerator One
    case 0x9F5: return '.'; // Bengali Currency Numerator Two
    case 0x9F6: return '.'; // Bengali Currency Numerator Three
    case 0x9F7: return '.'; // Bengali Currency Numerator Four
    case 0x9F8: return 's'; // Bengali Currency Numerator One Less Than The Denominator
    case 0x9F9: return 's'; // Bengali Currency Denominator Sixteen
    case 0x9FA: return 'h'; // Bengali Isshar
    case 0x9FB: return 'R'; // Bengali Ganda Mark
    case 0xA01: return 'a'; // Gurmukhi Sign Adak Bindi
    case 0xA02: return 'n'; // Gurmukhi Sign Bindi
    case 0xA03: return 'h'; // Gurmukhi Sign Visarga
    case 0xA05: return 'a'; // Gurmukhi Letter A
    case 0xA06: return 'a'; // Gurmukhi Letter Aa
    case 0xA07: return 'i'; // Gurmukhi Letter I
    case 0xA08: return 'i'; // Gurmukhi Letter Ii
    case 0xA09: return 'u'; // Gurmukhi Letter U
    case 0xA0A: return 'u'; // Gurmukhi Letter Uu
    case 0xA0F: return 'e'; // Gurmukhi Letter Ee
    case 0xA10: return 'a'; // Gurmukhi Letter Ai
    case 0xA13: return 'o'; // Gurmukhi Letter Oo
    case 0xA14: return 'o'; // Gurmukhi Letter Au
    case 0xA15: return 'k'; // Gurmukhi Letter Ka
    case 0xA17: return 'g'; // Gurmukhi Letter Ga
    case 0xA18: return 'g'; // Gurmukhi Letter Gha
    case 0xA19: return 'n'; // Gurmukhi Letter Nga
    case 0xA1A: return 'c'; // Gurmukhi Letter Ca
    case 0xA1B: return 'c'; // Gurmukhi Letter Cha
    case 0xA1D: return 'j'; // Gurmukhi Letter Jha
    case 0xA1E: return 'n'; // Gurmukhi Letter Nya
    case 0xA1F: return 't'; // Gurmukhi Letter Tta
    case 0xA20: return 't'; // Gurmukhi Letter Ttha
    case 0xA21: return 'd'; // Gurmukhi Letter Dda
    case 0xA22: return 'd'; // Gurmukhi Letter Ddha
    case 0xA23: return 'n'; // Gurmukhi Letter Nna
    case 0xA24: return 't'; // Gurmukhi Letter Ta
    case 0xA25: return 't'; // Gurmukhi Letter Tha
    case 0xA26: return 'd'; // Gurmukhi Letter Da
    case 0xA27: return 'd'; // Gurmukhi Letter Dha
    case 0xA28: return 'n'; // Gurmukhi Letter Na
    case 0xA2D: return 'b'; // Gurmukhi Letter Bha
    case 0xA2F: return 'y'; // Gurmukhi Letter Ya
    case 0xA30: return 'r'; // Gurmukhi Letter Ra
    case 0xA32: return 'l'; // Gurmukhi Letter La
    case 0xA33: return 'l'; // Gurmukhi Letter Lla
    case 0xA35: return 'v'; // Gurmukhi Letter Va
    case 0xA36: return 's'; // Gurmukhi Letter Sha
    case 0xA39: return 'h'; // Gurmukhi Letter Ha
    case 0xA3F: return 'i'; // Gurmukhi Vowel Sign I
    case 0xA42: return 'u'; // Gurmukhi Vowel Sign Uu
    case 0xA47: return 'e'; // Gurmukhi Vowel Sign Ee
    case 0xA48: return 'a'; // Gurmukhi Vowel Sign Ai
    case 0xA4B: return 'o'; // Gurmukhi Vowel Sign Oo
    case 0xA4C: return 'a'; // Gurmukhi Vowel Sign Au
    case 0xA4D: return 0; // Gurmukhi Sign Virama
    case 0xA51: return 0; // Gurmukhi Sign Udaat
    case 0xA59: return 'k'; // Gurmukhi Letter Khha
    case 0xA5A: return 'g'; // Gurmukhi Letter Ghha
    case 0xA5B: return 'z'; // Gurmukhi Letter Za
    case 0xA5C: return 'r'; // Gurmukhi Letter Rra
    case 0xA5E: return 'f'; // Gurmukhi Letter Fa
    case 0xA66: return '0'; // Gurmukhi Digit Zero
    case 0xA67: return '1'; // Gurmukhi Digit One
    case 0xA68: return '2'; // Gurmukhi Digit Two
    case 0xA69: return '3'; // Gurmukhi Digit Three
    case 0xA6A: return '4'; // Gurmukhi Digit Four
    case 0xA6B: return '5'; // Gurmukhi Digit Five
    case 0xA6C: return '6'; // Gurmukhi Digit Six
    case 0xA6D: return '7'; // Gurmukhi Digit Seven
    case 0xA6E: return '8'; // Gurmukhi Digit Eight
    case 0xA6F: return '9'; // Gurmukhi Digit Nine
    case 0xA75: return 'y'; // Gurmukhi Sign Yakash
    case 0xA81: return 'n'; // Gujarati Sign Candrabindu
    case 0xA82: return 'n'; // Gujarati Sign Anusvara
    case 0xA83: return 'h'; // Gujarati Sign Visarga
    case 0xA85: return 'a'; // Gujarati Letter A
    case 0xA86: return 'a'; // Gujarati Letter Aa
    case 0xA87: return 'i'; // Gujarati Letter I
    case 0xA88: return 'i'; // Gujarati Letter Ii
    case 0xA89: return 'u'; // Gujarati Letter U
    case 0xA8A: return 'u'; // Gujarati Letter Uu
    case 0xA8B: return 'r'; // Gujarati Letter Vocalic R
    case 0xA8C: return 'l'; // Gujarati Letter Vocalic L
    case 0xA8D: return 'e'; // Gujarati Vowel Candra E
    case 0xA8F: return 'e'; // Gujarati Letter E
    case 0xA90: return 'a'; // Gujarati Letter Ai
    case 0xA91: return 'o'; // Gujarati Vowel Candra O
    case 0xA93: return 'o'; // Gujarati Letter O
    case 0xA94: return 'o'; // Gujarati Letter Au
    case 0xA95: return 'k'; // Gujarati Letter Ka
    case 0xA98: return 'g'; // Gujarati Letter Gha
    case 0xA99: return 'n'; // Gujarati Letter Nga
    case 0xA9A: return 'c'; // Gujarati Letter Ca
    case 0xA9B: return 'c'; // Gujarati Letter Cha
    case 0xA9D: return 'j'; // Gujarati Letter Jha
    case 0xA9E: return 'n'; // Gujarati Letter Nya
    case 0xA9F: return 't'; // Gujarati Letter Tta
    case 0xAA1: return 'd'; // Gujarati Letter Dda
    case 0xAA2: return 'd'; // Gujarati Letter Ddha
    case 0xAA3: return 'n'; // Gujarati Letter Nna
    case 0xAA5: return 't'; // Gujarati Letter Tha
    case 0xAA6: return 'd'; // Gujarati Letter Da
    case 0xAA7: return 'd'; // Gujarati Letter Dha
    case 0xAA8: return 'n'; // Gujarati Letter Na
    case 0xAAB: return 'p'; // Gujarati Letter Pha
    case 0xAAC: return 'b'; // Gujarati Letter Ba
    case 0xAAD: return 'b'; // Gujarati Letter Bha
    case 0xAAF: return 'y'; // Gujarati Letter Ya
    case 0xAB2: return 'l'; // Gujarati Letter La
    case 0xAB3: return 'l'; // Gujarati Letter Lla
    case 0xAB5: return 'v'; // Gujarati Letter Va
    case 0xAB6: return 's'; // Gujarati Letter Sha
    case 0xAB8: return 's'; // Gujarati Letter Sa
    case 0xAB9: return 'h'; // Gujarati Letter Ha
    case 0xABC: return 0; // Gujarati Sign Nukta
    case 0xABD: return '\''; // Gujarati Sign Avagraha
    case 0xABF: return 'i'; // Gujarati Vowel Sign I
    case 0xAC2: return 'u'; // Gujarati Vowel Sign Uu
    case 0xAC4: return 'r'; // Gujarati Vowel Sign Vocalic Rr
    case 0xAC5: return 'c'; // Gujarati Vowel Sign Candra E
    case 0xAC7: return 'e'; // Gujarati Vowel Sign E
    case 0xAC8: return 'a'; // Gujarati Vowel Sign Ai
    case 0xAC9: return 'c'; // Gujarati Vowel Sign Candra O
    case 0xACB: return 'o'; // Gujarati Vowel Sign O
    case 0xACC: return 'a'; // Gujarati Vowel Sign Au
    case 0xAD0: return 'o'; // Gujarati Om
    case 0xAE0: return 'r'; // Gujarati Letter Vocalic Rr
    case 0xAE1: return 'l'; // Gujarati Letter Vocalic Ll
    case 0xAE2: return 'l'; // Gujarati Vowel Sign Vocalic L
    case 0xAE3: return 'l'; // Gujarati Vowel Sign Vocalic Ll
    case 0xAE6: return '0'; // Gujarati Digit Zero
    case 0xAE7: return '1'; // Gujarati Digit One
    case 0xAE8: return '2'; // Gujarati Digit Two
    case 0xAE9: return '3'; // Gujarati Digit Three
    case 0xAEA: return '4'; // Gujarati Digit Four
    case 0xAEB: return '5'; // Gujarati Digit Five
    case 0xAEC: return '6'; // Gujarati Digit Six
    case 0xAED: return '7'; // Gujarati Digit Seven
    case 0xAEE: return '8'; // Gujarati Digit Eight
    case 0xAEF: return '9'; // Gujarati Digit Nine
    case 0xAF0: return '.'; // Gujarati Abbreviation Sign
    case 0xB01: return 'n'; // Oriya Sign Candrabindu
    case 0xB02: return 'n'; // Oriya Sign Anusvara
    case 0xB03: return 'h'; // Oriya Sign Visarga
    case 0xB05: return 'a'; // Oriya Letter A
    case 0xB07: return 'i'; // Oriya Letter I
    case 0xB08: return 'i'; // Oriya Letter Ii
    case 0xB09: return 'u'; // Oriya Letter U
    case 0xB0A: return 'u'; // Oriya Letter Uu
    case 0xB0B: return 'r'; // Oriya Letter Vocalic R
    case 0xB0C: return 'l'; // Oriya Letter Vocalic L
    case 0xB0F: return 'e'; // Oriya Letter E
    case 0xB10: return 'a'; // Oriya Letter Ai
    case 0xB14: return 'a'; // Oriya Letter Au
    case 0xB15: return 'k'; // Oriya Letter Ka
    case 0xB16: return 'k'; // Oriya Letter Kha
    case 0xB17: return 'g'; // Oriya Letter Ga
    case 0xB18: return 'g'; // Oriya Letter Gha
    case 0xB19: return 'n'; // Oriya Letter Nga
    case 0xB1A: return 'c'; // Oriya Letter Ca
    case 0xB1B: return 'c'; // Oriya Letter Cha
    case 0xB1C: return 'j'; // Oriya Letter Ja
    case 0xB1D: return 'j'; // Oriya Letter Jha
    case 0xB1E: return 'n'; // Oriya Letter Nya
    case 0xB1F: return 't'; // Oriya Letter Tta
    case 0xB22: return 'd'; // Oriya Letter Ddha
    case 0xB23: return 'n'; // Oriya Letter Nna
    case 0xB24: return 't'; // Oriya Letter Ta
    case 0xB25: return 't'; // Oriya Letter Tha
    case 0xB26: return 'd'; // Oriya Letter Da
    case 0xB2B: return 'p'; // Oriya Letter Pha
    case 0xB2C: return 'b'; // Oriya Letter Ba
    case 0xB2D: return 'b'; // Oriya Letter Bha
    case 0xB2E: return 'm'; // Oriya Letter Ma
    case 0xB2F: return 'y'; // Oriya Letter Ya
    case 0xB32: return 'l'; // Oriya Letter La
    case 0xB33: return 'l'; // Oriya Letter Lla
    case 0xB35: return 'v'; // Oriya Letter Va
    case 0xB36: return 's'; // Oriya Letter Sha
    case 0xB38: return 's'; // Oriya Letter Sa
    case 0xB39: return 'h'; // Oriya Letter Ha
    case 0xB3D: return '\''; // Oriya Sign Avagraha
    case 0xB40: return 'i'; // Oriya Vowel Sign Ii
    case 0xB41: return 'u'; // Oriya Vowel Sign U
    case 0xB42: return 'u'; // Oriya Vowel Sign Uu
    case 0xB44: return 'r'; // Oriya Vowel Sign Vocalic Rr
    case 0xB47: return 'e'; // Oriya Vowel Sign E
    case 0xB48: return 'a'; // Oriya Vowel Sign Ai
    case 0xB4B: return 'o'; // Oriya Vowel Sign O
    case 0xB4C: return 'a'; // Oriya Vowel Sign Au
    case 0xB55: return 'o'; // Oriya Sign Overline
    case 0xB5C: return 'r'; // Oriya Letter Rra
    case 0xB5D: return 'r'; // Oriya Letter Rha
    case 0xB5F: return 'y'; // Oriya Letter Yya
    case 0xB60: return 'r'; // Oriya Letter Vocalic Rr
    case 0xB61: return 'l'; // Oriya Letter Vocalic Ll
    case 0xB62: return 'l'; // Oriya Vowel Sign Vocalic L
    case 0xB63: return 'l'; // Oriya Vowel Sign Vocalic Ll
    case 0xB66: return '0'; // Oriya Digit Zero
    case 0xB67: return '1'; // Oriya Digit One
    case 0xB68: return '2'; // Oriya Digit Two
    case 0xB69: return '3'; // Oriya Digit Three
    case 0xB6A: return '4'; // Oriya Digit Four
    case 0xB6B: return '5'; // Oriya Digit Five
    case 0xB6C: return '6'; // Oriya Digit Six
    case 0xB6D: return '7'; // Oriya Digit Seven
    case 0xB6E: return '8'; // Oriya Digit Eight
    case 0xB6F: return '9'; // Oriya Digit Nine
    case 0xB70: return 'R'; // Oriya Isshar
    case 0xB71: return 'w'; // Oriya Letter Wa
    case 0xB82: return 'n'; // Tamil Sign Anusvara
    case 0xB83: return 'h'; // Tamil Sign Visarga
    case 0xB85: return 'a'; // Tamil Letter A
    case 0xB86: return 'a'; // Tamil Letter Aa
    case 0xB87: return 'i'; // Tamil Letter I
    case 0xB88: return 'i'; // Tamil Letter Ii
    case 0xB89: return 'u'; // Tamil Letter U
    case 0xB8A: return 'u'; // Tamil Letter Uu
    case 0xB8E: return 'e'; // Tamil Letter E
    case 0xB8F: return 'e'; // Tamil Letter Ee
    case 0xB90: return 'a'; // Tamil Letter Ai
    case 0xB92: return 'o'; // Tamil Letter O
    case 0xB93: return 'o'; // Tamil Letter Oo
    case 0xB94: return 'a'; // Tamil Letter Au
    case 0xB99: return 'n'; // Tamil Letter Nga
    case 0xB9A: return 'c'; // Tamil Letter Ca
    case 0xB9C: return 'j'; // Tamil Letter Ja
    case 0xB9E: return 'n'; // Tamil Letter Nya
    case 0xB9F: return 't'; // Tamil Letter Tta
    case 0xBA3: return 'n'; // Tamil Letter Nna
    case 0xBA8: return 'n'; // Tamil Letter Na
    case 0xBA9: return 'n'; // Tamil Letter Nnna
    case 0xBAF: return 'y'; // Tamil Letter Ya
    case 0xBB0: return 'r'; // Tamil Letter Ra
    case 0xBB2: return 'l'; // Tamil Letter La
    case 0xBB3: return 'l'; // Tamil Letter Lla
    case 0xBB5: return 'v'; // Tamil Letter Va
    case 0xBB6: return 's'; // Tamil Letter Sha
    case 0xBB7: return 's'; // Tamil Letter Ssa
    case 0xBB8: return 's'; // Tamil Letter Sa
    case 0xBB9: return 'h'; // Tamil Letter Ha
    case 0xBBE: return 'a'; // Tamil Vowel Sign Aa
    case 0xBC0: return 'i'; // Tamil Vowel Sign Ii
    case 0xBC2: return 'u'; // Tamil Vowel Sign Uu
    case 0xBC6: return 'e'; // Tamil Vowel Sign E
    case 0xBC7: return 'e'; // Tamil Vowel Sign Ee
    case 0xBC8: return 'a'; // Tamil Vowel Sign Ai
    case 0xBCA: return 'o'; // Tamil Vowel Sign O
    case 0xBCB: return 'o'; // Tamil Vowel Sign Oo
    case 0xBCC: return 'a'; // Tamil Vowel Sign Au
    case 0xBD0: return 'o'; // Tamil Om
    case 0xBE6: return '0'; // Tamil Digit Zero
    case 0xBE7: return '1'; // Tamil Digit One
    case 0xBE8: return '2'; // Tamil Digit Two
    case 0xBE9: return '3'; // Tamil Digit Three
    case 0xBEA: return '4'; // Tamil Digit Four
    case 0xBEB: return '5'; // Tamil Digit Five
    case 0xBEC: return '6'; // Tamil Digit Six
    case 0xBED: return '7'; // Tamil Digit Seven
    case 0xBEE: return '8'; // Tamil Digit Eight
    case 0xBEF: return '9'; // Tamil Digit Nine
    case 0xC00: return 'n'; // Telugu Sign Combining Candrabindu Above
    case 0xC01: return 'n'; // Telugu Sign Candrabindu
    case 0xC02: return 'n'; // Telugu Sign Anusvara
    case 0xC03: return 'h'; // Telugu Sign Visarga
    case 0xC04: return 'n'; // Telugu Sign Combining Anusvara Above
    case 0xC05: return 'a'; // Telugu Letter A
    case 0xC06: return 'a'; // Telugu Letter Aa
    case 0xC07: return 'i'; // Telugu Letter I
    case 0xC08: return 'i'; // Telugu Letter Ii
    case 0xC09: return 'u'; // Telugu Letter U
    case 0xC0A: return 'u'; // Telugu Letter Uu
    case 0xC0B: return 'r'; // Telugu Letter Vocalic R
    case 0xC0C: return 'l'; // Telugu Letter Vocalic L
    case 0xC0E: return 'e'; // Telugu Letter E
    case 0xC0F: return 'e'; // Telugu Letter Ee
    case 0xC10: return 'a'; // Telugu Letter Ai
    case 0xC12: return 'o'; // Telugu Letter O
    case 0xC13: return 'o'; // Telugu Letter Oo
    case 0xC14: return 'a'; // Telugu Letter Au
    case 0xC15: return 'k'; // Telugu Letter Ka
    case 0xC16: return 'k'; // Telugu Letter Kha
    case 0xC18: return 'g'; // Telugu Letter Gha
    case 0xC19: return 'n'; // Telugu Letter Nga
    case 0xC1A: return 'c'; // Telugu Letter Ca
    case 0xC1B: return 'c'; // Telugu Letter Cha
    case 0xC1D: return 'j'; // Telugu Letter Jha
    case 0xC1E: return 'n'; // Telugu Letter Nya
    case 0xC20: return 't'; // Telugu Letter Ttha
    case 0xC21: return 'd'; // Telugu Letter Dda
    case 0xC22: return 'd'; // Telugu Letter Ddha
    case 0xC23: return 'n'; // Telugu Letter Nna
    case 0xC25: return 't'; // Telugu Letter Tha
    case 0xC27: return 'd'; // Telugu Letter Dha
    case 0xC28: return 'n'; // Telugu Letter Na
    case 0xC2B: return 'p'; // Telugu Letter Pha
    case 0xC2C: return 'b'; // Telugu Letter Ba
    case 0xC2D: return 'b'; // Telugu Letter Bha
    case 0xC2F: return 'y'; // Telugu Letter Ya
    case 0xC30: return 'r'; // Telugu Letter Ra
    case 0xC31: return 'r'; // Telugu Letter Rra
    case 0xC33: return 'l'; // Telugu Letter Lla
    case 0xC34: return 'l'; // Telugu Letter Llla
    case 0xC35: return 'v'; // Telugu Letter Va
    case 0xC36: return 's'; // Telugu Letter Sha
    case 0xC37: return 's'; // Telugu Letter Ssa
    case 0xC38: return 's'; // Telugu Letter Sa
    case 0xC39: return 'h'; // Telugu Letter Ha
    case 0xC3C: return 0; // Telugu Sign Nukta
    case 0xC3D: return '\''; // Telugu Sign Avagraha
    case 0xC3E: return 'a'; // Telugu Vowel Sign Aa
    case 0xC42: return 'u'; // Telugu Vowel Sign Uu
    case 0xC43: return 'r'; // Telugu Vowel Sign Vocalic R
    case 0xC44: return 'r'; // Telugu Vowel Sign Vocalic Rr
    case 0xC48: return 'a'; // Telugu Vowel Sign Ai
    case 0xC4B: return 'o'; // Telugu Vowel Sign Oo
    case 0xC4C: return 'a'; // Telugu Vowel Sign Au
    case 0xC4D: return 0; // Telugu Sign Virama
    case 0xC58: return 't'; // Telugu Letter Tsa
    case 0xC59: return 'd'; // Telugu Letter Dza
    case 0xC5A: return 'r'; // Telugu Letter Rrra
    case 0xC5D: return 'n'; // Telugu Letter Nakaara Pollu
    case 0xC60: return 'r'; // Telugu Letter Vocalic Rr
    case 0xC61: return 'l'; // Telugu Letter Vocalic Ll
    case 0xC62: return 'l'; // Telugu Vowel Sign Vocalic L
    case 0xC63: return 'l'; // Telugu Vowel Sign Vocalic Ll
    case 0xC66: return '0'; // Telugu Digit Zero
    case 0xC67: return '1'; // Telugu Digit One
    case 0xC68: return '2'; // Telugu Digit Two
    case 0xC69: return '3'; // Telugu Digit Three
    case 0xC6A: return '4'; // Telugu Digit Four
    case 0xC6B: return '5'; // Telugu Digit Five
    case 0xC6C: return '6'; // Telugu Digit Six
    case 0xC6D: return '7'; // Telugu Digit Seven
    case 0xC6E: return '8'; // Telugu Digit Eight
    case 0xC6F: return '9'; // Telugu Digit Nine
    case 0xC77: return 's'; // Telugu Sign Siddham
    case 0xC78: return '0'; // Telugu Fraction Digit Zero For Odd Powers Of Four
    case 0xC79: return '1'; // Telugu Fraction Digit One For Odd Powers Of Four
    case 0xC7A: return '2'; // Telugu Fraction Digit Two For Odd Powers Of Four
    case 0xC7B: return '3'; // Telugu Fraction Digit Three For Odd Powers Of Four
    case 0xC7C: return '1'; // Telugu Fraction Digit One For Even Powers Of Four
    case 0xC7D: return '2'; // Telugu Fraction Digit Two For Even Powers Of Four
    case 0xC7E: return '3'; // Telugu Fraction Digit Three For Even Powers Of Four
    case 0xC7F: return 't'; // Telugu Sign Tuumu
    case 0xC80: return 'n'; // Kannada Sign Spacing Candrabindu
    case 0xC81: return 'n'; // Kannada Sign Candrabindu
    case 0xC82: return 'n'; // Kannada Sign Anusvara
    case 0xC83: return 'h'; // Kannada Sign Visarga
    case 0xC84: return 's'; // Kannada Sign Siddham
    case 0xC85: return 'a'; // Kannada Letter A
    case 0xC86: return 'a'; // Kannada Letter Aa
    case 0xC87: return 'i'; // Kannada Letter I
    case 0xC88: return 'i'; // Kannada Letter Ii
    case 0xC89: return 'u'; // Kannada Letter U
    case 0xC8A: return 'u'; // Kannada Letter Uu
    case 0xC8B: return 'r'; // Kannada Letter Vocalic R
    case 0xC8C: return 'l'; // Kannada Letter Vocalic L
    case 0xC8E: return 'e'; // Kannada Letter E
    case 0xC8F: return 'e'; // Kannada Letter Ee
    case 0xC90: return 'a'; // Kannada Letter Ai
    case 0xC92: return 'o'; // Kannada Letter O
    case 0xC93: return 'o'; // Kannada Letter Oo
    case 0xC94: return 'a'; // Kannada Letter Au
    case 0xC97: return 'g'; // Kannada Letter Ga
    case 0xC98: return 'g'; // Kannada Letter Gha
    case 0xC99: return 'n'; // Kannada Letter Nga
    case 0xC9A: return 'c'; // Kannada Letter Ca
    case 0xC9B: return 'c'; // Kannada Letter Cha
    case 0xC9C: return 'j'; // Kannada Letter Ja
    case 0xC9D: return 'j'; // Kannada Letter Jha
    case 0xC9E: return 'n'; // Kannada Letter Nya
    case 0xCA0: return 't'; // Kannada Letter Ttha
    case 0xCA2: return 'd'; // Kannada Letter Ddha
    case 0xCA3: return 'n'; // Kannada Letter Nna
    case 0xCA5: return 't'; // Kannada Letter Tha
    case 0xCA6: return 'd'; // Kannada Letter Da
    case 0xCA7: return 'd'; // Kannada Letter Dha
    case 0xCAB: return 'p'; // Kannada Letter Pha
    case 0xCAC: return 'b'; // Kannada Letter Ba
    case 0xCAD: return 'b'; // Kannada Letter Bha
    case 0xCB0: return 'r'; // Kannada Letter Ra
    case 0xCB1: return 'r'; // Kannada Letter Rra
    case 0xCB2: return 'l'; // Kannada Letter La
    case 0xCB5: return 'v'; // Kannada Letter Va
    case 0xCB6: return 's'; // Kannada Letter Sha
    case 0xCB7: return 's'; // Kannada Letter Ssa
    case 0xCB8: return 's'; // Kannada Letter Sa
    case 0xCB9: return 'h'; // Kannada Letter Ha
    case 0xCBC: return 0; // Kannada Sign Nukta
    case 0xCBD: return '\''; // Kannada Sign Avagraha
    case 0xCBE: return 'a'; // Kannada Vowel Sign Aa
    case 0xCBF: return 'i'; // Kannada Vowel Sign I
    case 0xCC0: return 'i'; // Kannada Vowel Sign Ii
    case 0xCC2: return 'u'; // Kannada Vowel Sign Uu
    case 0xCC3: return 'r'; // Kannada Vowel Sign Vocalic R
    case 0xCC4: return 'r'; // Kannada Vowel Sign Vocalic Rr
    case 0xCC6: return 'e'; // Kannada Vowel Sign E
    case 0xCC7: return 'e'; // Kannada Vowel Sign Ee
    case 0xCC8: return 'a'; // Kannada Vowel Sign Ai
    case 0xCCA: return 'o'; // Kannada Vowel Sign O
    case 0xCCB: return 'o'; // Kannada Vowel Sign Oo
    case 0xCCC: return 'a'; // Kannada Vowel Sign Au
    case 0xCDD: return 'n'; // Kannada Letter Nakaara Pollu
    case 0xCDE: return 'f'; // Kannada Letter Fa
    case 0xCE0: return 'r'; // Kannada Letter Vocalic Rr
    case 0xCE1: return 'l'; // Kannada Letter Vocalic Ll
    case 0xCE2: return 'l'; // Kannada Vowel Sign Vocalic L
    case 0xCE3: return 'l'; // Kannada Vowel Sign Vocalic Ll
    case 0xCE6: return '0'; // Kannada Digit Zero
    case 0xCE7: return '1'; // Kannada Digit One
    case 0xCE8: return '2'; // Kannada Digit Two
    case 0xCE9: return '3'; // Kannada Digit Three
    case 0xCEA: return '4'; // Kannada Digit Four
    case 0xCEB: return '5'; // Kannada Digit Five
    case 0xCEC: return '6'; // Kannada Digit Six
    case 0xCED: return '7'; // Kannada Digit Seven
    case 0xCEE: return '8'; // Kannada Digit Eight
    case 0xCEF: return '9'; // Kannada Digit Nine
    case 0xCF1: return 'j'; // Kannada Sign Jihvamuliya
    case 0xCF2: return 'u'; // Kannada Sign Upadhmaniya
    case 0xCF3: return 'n'; // Kannada Sign Combining Anusvara Above Right
    case 0xD00: return 'n'; // Malayalam Sign Combining Anusvara Above
    case 0xD01: return 'n'; // Malayalam Sign Candrabindu
    case 0xD03: return 'h'; // Malayalam Sign Visarga
    case 0xD04: return 'n'; // Malayalam Letter Vedic Anusvara
    case 0xD05: return 'a'; // Malayalam Letter A
    case 0xD06: return 'a'; // Malayalam Letter Aa
    case 0xD07: return 'i'; // Malayalam Letter I
    case 0xD08: return 'i'; // Malayalam Letter Ii
    case 0xD09: return 'u'; // Malayalam Letter U
    case 0xD0A: return 'u'; // Malayalam Letter Uu
    case 0xD0B: return 'r'; // Malayalam Letter Vocalic R
    case 0xD0C: return 'l'; // Malayalam Letter Vocalic L
    case 0xD0E: return 'e'; // Malayalam Letter E
    case 0xD0F: return 'e'; // Malayalam Letter Ee
    case 0xD10: return 'a'; // Malayalam Letter Ai
    case 0xD12: return 'o'; // Malayalam Letter O
    case 0xD13: return 'o'; // Malayalam Letter Oo
    case 0xD14: return 'a'; // Malayalam Letter Au
    case 0xD15: return 'k'; // Malayalam Letter Ka
    case 0xD16: return 'k'; // Malayalam Letter Kha
    case 0xD17: return 'g'; // Malayalam Letter Ga
    case 0xD18: return 'g'; // Malayalam Letter Gha
    case 0xD19: return 'n'; // Malayalam Letter Nga
    case 0xD1A: return 'c'; // Malayalam Letter Ca
    case 0xD1B: return 'c'; // Malayalam Letter Cha
    case 0xD1C: return 'j'; // Malayalam Letter Ja
    case 0xD1D: return 'j'; // Malayalam Letter Jha
    case 0xD1E: return 'n'; // Malayalam Letter Nya
    case 0xD1F: return 't'; // Malayalam Letter Tta
    case 0xD20: return 't'; // Malayalam Letter Ttha
    case 0xD21: return 'd'; // Malayalam Letter Dda
    case 0xD22: return 'd'; // Malayalam Letter Ddha
    case 0xD23: return 'n'; // Malayalam Letter Nna
    case 0xD25: return 't'; // Malayalam Letter Tha
    case 0xD26: return 'd'; // Malayalam Letter Da
    case 0xD29: return 'n'; // Malayalam Letter Nnna
    case 0xD2B: return 'p'; // Malayalam Letter Pha
    case 0xD2C: return 'b'; // Malayalam Letter Ba
    case 0xD2D: return 'b'; // Malayalam Letter Bha
    case 0xD31: return 'r'; // Malayalam Letter Rra
    case 0xD34: return 'l'; // Malayalam Letter Llla
    case 0xD35: return 'v'; // Malayalam Letter Va
    case 0xD36: return 's'; // Malayalam Letter Sha
    case 0xD37: return 's'; // Malayalam Letter Ssa
    case 0xD38: return 's'; // Malayalam Letter Sa
    case 0xD39: return 'h'; // Malayalam Letter Ha
    case 0xD3A: return 't'; // Malayalam Letter Ttta
    case 0xD3B: return 0; // Malayalam Sign Vertical Bar Virama
    case 0xD3C: return 0; // Malayalam Sign Circular Virama
    case 0xD3D: return '\''; // Malayalam Sign Avagraha
    case 0xD3F: return 'i'; // Malayalam Vowel Sign I
    case 0xD40: return 'i'; // Malayalam Vowel Sign Ii
    case 0xD41: return 'u'; // Malayalam Vowel Sign U
    case 0xD42: return 'u'; // Malayalam Vowel Sign Uu
    case 0xD43: return 'r'; // Malayalam Vowel Sign Vocalic R
    case 0xD44: return 'r'; // Malayalam Vowel Sign Vocalic Rr
    case 0xD46: return 'e'; // Malayalam Vowel Sign E
    case 0xD47: return 'e'; // Malayalam Vowel Sign Ee
    case 0xD48: return 'a'; // Malayalam Vowel Sign Ai
    case 0xD4A: return 'o'; // Malayalam Vowel Sign O
    case 0xD4B: return 'o'; // Malayalam Vowel Sign Oo
    case 0xD4C: return 'a'; // Malayalam Vowel Sign Au
    case 0xD4E: return 'd'; // Malayalam Letter Dot Reph
    case 0xD4F: return 'p'; // Malayalam Sign Para
    case 0xD54: return 'm'; // Malayalam Letter Chillu M
    case 0xD55: return 'y'; // Malayalam Letter Chillu Y
    case 0xD56: return 'l'; // Malayalam Letter Chillu Lll
    case 0xD5F: return 'a'; // Malayalam Letter Archaic Ii
    case 0xD60: return 'r'; // Malayalam Letter Vocalic Rr
    case 0xD61: return 'l'; // Malayalam Letter Vocalic Ll
    case 0xD62: return 'l'; // Malayalam Vowel Sign Vocalic L
    case 0xD63: return 'l'; // Malayalam Vowel Sign Vocalic Ll
    case 0xD66: return '0'; // Malayalam Digit Zero
    case 0xD67: return '1'; // Malayalam Digit One
    case 0xD68: return '2'; // Malayalam Digit Two
    case 0xD69: return '3'; // Malayalam Digit Three
    case 0xD6A: return '4'; // Malayalam Digit Four
    case 0xD6B: return '5'; // Malayalam Digit Five
    case 0xD6C: return '6'; // Malayalam Digit Six
    case 0xD6D: return '7'; // Malayalam Digit Seven
    case 0xD6E: return '8'; // Malayalam Digit Eight
    case 0xD6F: return '9'; // Malayalam Digit Nine
    case 0xD7A: return 'n'; // Malayalam Letter Chillu Nn
    case 0xD7B: return 'n'; // Malayalam Letter Chillu N
    case 0xD7C: return 'r'; // Malayalam Letter Chillu Rr
    case 0xD7D: return 'l'; // Malayalam Letter Chillu L
    case 0xD7F: return 'k'; // Malayalam Letter Chillu K
    case 0xD81: return 'n'; // Sinhala Sign Candrabindu
    case 0xD83: return 'v'; // Sinhala Sign Visargaya
    case 0xD85: return 'a'; // Sinhala Letter Ayanna
    case 0xD86: return 'a'; // Sinhala Letter Aayanna
    case 0xD87: return 'a'; // Sinhala Letter Aeyanna
    case 0xD88: return 'a'; // Sinhala Letter Aeeyanna
    case 0xD89: return 'i'; // Sinhala Letter Iyanna
    case 0xD8A: return 'i'; // Sinhala Letter Iiyanna
    case 0xD8B: return 'u'; // Sinhala Letter Uyanna
    case 0xD8C: return 'u'; // Sinhala Letter Uuyanna
    case 0xD8D: return 'i'; // Sinhala Letter Iruyanna
    case 0xD8E: return 'i'; // Sinhala Letter Iruuyanna
    case 0xD8F: return 'i'; // Sinhala Letter Iluyanna
    case 0xD90: return 'i'; // Sinhala Letter Iluuyanna
    case 0xD91: return 'e'; // Sinhala Letter Eyanna
    case 0xD92: return 'e'; // Sinhala Letter Eeyanna
    case 0xD93: return 'a'; // Sinhala Letter Aiyanna
    case 0xD94: return 'o'; // Sinhala Letter Oyanna
    case 0xD95: return 'o'; // Sinhala Letter Ooyanna
    case 0xD96: return 'a'; // Sinhala Letter Auyanna
    case 0xD9A: return 'k'; // Sinhala Letter Alpapraana Kayanna
    case 0xD9B: return 'm'; // Sinhala Letter Mahaapraana Kayanna
    case 0xD9C: return 'g'; // Sinhala Letter Alpapraana Gayanna
    case 0xD9D: return 'm'; // Sinhala Letter Mahaapraana Gayanna
    case 0xD9E: return 'k'; // Sinhala Letter Kantaja Naasikyaya
    case 0xD9F: return 'g'; // Sinhala Letter Sanyaka Gayanna
    case 0xDA0: return 'c'; // Sinhala Letter Alpapraana Cayanna
    case 0xDA1: return 'm'; // Sinhala Letter Mahaapraana Cayanna
    case 0xDA2: return 'j'; // Sinhala Letter Alpapraana Jayanna
    case 0xDA3: return 'm'; // Sinhala Letter Mahaapraana Jayanna
    case 0xDA4: return 't'; // Sinhala Letter Taaluja Naasikyaya
    case 0xDA5: return 't'; // Sinhala Letter Taaluja Sanyooga Naaksikyaya
    case 0xDA6: return 'j'; // Sinhala Letter Sanyaka Jayanna
    case 0xDA8: return 'm'; // Sinhala Letter Mahaapraana Ttayanna
    case 0xDA9: return 'd'; // Sinhala Letter Alpapraana Ddayanna
    case 0xDAA: return 'm'; // Sinhala Letter Mahaapraana Ddayanna
    case 0xDAB: return 'm'; // Sinhala Letter Muurdhaja Nayanna
    case 0xDAC: return 'd'; // Sinhala Letter Sanyaka Ddayanna
    case 0xDAD: return 't'; // Sinhala Letter Alpapraana Tayanna
    case 0xDAE: return 'm'; // Sinhala Letter Mahaapraana Tayanna
    case 0xDAF: return 'd'; // Sinhala Letter Alpapraana Dayanna
    case 0xDB0: return 'm'; // Sinhala Letter Mahaapraana Dayanna
    case 0xDB1: return 'n'; // Sinhala Letter Dantaja Nayanna
    case 0xDB3: return 'd'; // Sinhala Letter Sanyaka Dayanna
    case 0xDB5: return 'm'; // Sinhala Letter Mahaapraana Payanna
    case 0xDB6: return 'b'; // Sinhala Letter Alpapraana Bayanna
    case 0xDB7: return 'm'; // Sinhala Letter Mahaapraana Bayanna
    case 0xDB9: return 'a'; // Sinhala Letter Amba Bayanna
    case 0xDBA: return 'y'; // Sinhala Letter Yayanna
    case 0xDBB: return 'n'; // Sinhala Letter Rayanna
    case 0xDBE: return 'n'; // ?
    case 0xDBF: return 'n'; // ?
    case 0xDC1: return 't'; // Sinhala Letter Taaluja Sayanna
    case 0xDC2: return 'm'; // Sinhala Letter Muurdhaja Sayanna
    case 0xDC5: return 'm'; // Sinhala Letter Muurdhaja Layanna
    case 0xDC6: return 'f'; // Sinhala Letter Fayanna
    case 0xDCF: return 'a'; // Sinhala Vowel Sign Aela-Pilla
    case 0xDD0: return 'a'; // Sinhala Vowel Sign Ketti Aeda-Pilla
    case 0xDD1: return 'a'; // Sinhala Vowel Sign Diga Aeda-Pilla
    case 0xDD3: return 'i'; // Sinhala Vowel Sign Diga Is-Pilla
    case 0xDD6: return 'a'; // Sinhala Vowel Sign Diga Paa-Pilla
    case 0xDD8: return 'o'; // Sinhala Vowel Sign Gaetta-Pilla
    case 0xDD9: return 'o'; // Sinhala Vowel Sign Kombuva
    case 0xDDA: return 'o'; // Sinhala Vowel Sign Diga Kombuva
    case 0xDDB: return 'e'; // Sinhala Vowel Sign Kombu Deka
    case 0xDDC: return 'o'; // Sinhala Vowel Sign Kombuva Haa Aela-Pilla
    case 0xDDD: return 'o'; // Sinhala Vowel Sign Kombuva Haa Diga Aela-Pilla
    case 0xDDE: return 'o'; // Sinhala Vowel Sign Kombuva Haa Gayanukitta
    case 0xDDF: return 'a'; // Sinhala Vowel Sign Gayanukitta
    case 0xDE6: return '0'; // Sinhala Lith Digit Zero
    case 0xDE7: return '1'; // Sinhala Lith Digit One
    case 0xDE8: return '2'; // Sinhala Lith Digit Two
    case 0xDE9: return '3'; // Sinhala Lith Digit Three
    case 0xDEA: return '4'; // Sinhala Lith Digit Four
    case 0xDEB: return '5'; // Sinhala Lith Digit Five
    case 0xDEC: return '6'; // Sinhala Lith Digit Six
    case 0xDED: return '7'; // Sinhala Lith Digit Seven
    case 0xDEE: return '8'; // Sinhala Lith Digit Eight
    case 0xDEF: return '9'; // Sinhala Lith Digit Nine
    case 0xDF2: return 'g'; // Sinhala Vowel Sign Diga Gaetta-Pilla
    case 0xDF3: return 'g'; // Sinhala Vowel Sign Diga Gayanukitta
    case 0xE2F: return '.'; // Thai Character Paiyannoi
    case 0xE3A: return 0; // Thai Character Phinthu
    case 0xE3F: return 'B'; // Thai Currency Symbol Baht
    case 0xE48: return 0; // Thai Character Mai Ek
    case 0xE49: return 0; // Thai Character Mai Tho
    case 0xE4A: return 0; // Thai Character Mai Tri
    case 0xE4B: return 0; // Thai Character Mai Chattawa
    case 0xE4C: return 0; // Thai Character Thanthakhat
    case 0xE50: return '0'; // Thai Digit Zero
    case 0xE51: return '1'; // Thai Digit One
    case 0xE52: return '2'; // Thai Digit Two
    case 0xE53: return '3'; // Thai Digit Three
    case 0xE54: return '4'; // Thai Digit Four
    case 0xE55: return '5'; // Thai Digit Five
    case 0xE56: return '6'; // Thai Digit Six
    case 0xE57: return '7'; // Thai Digit Seven
    case 0xE58: return '8'; // Thai Digit Eight
    case 0xE59: return '9'; // Thai Digit Nine
    case 0xE5A: return '|'; // Thai Character Angkhankhu
    case 0xE5B: return '|'; // Thai Character Khomut
    case 0xE82: return 'k'; // Lao Letter Kho Sung
    case 0xE84: return 'k'; // Lao Letter Kho Tam
    case 0xE86: return 'p'; // Lao Letter Pali Gha
    case 0xE87: return 'n'; // Lao Letter Ngo
    case 0xE88: return 'c'; // Lao Letter Co
    case 0xE89: return 'p'; // Lao Letter Pali Cha
    case 0xE8A: return 's'; // Lao Letter So Tam
    case 0xE8C: return 'p'; // Lao Letter Pali Jha
    case 0xE8D: return 'n'; // Lao Letter Nyo
    case 0xE8E: return 'p'; // Lao Letter Pali Nya
    case 0xE8F: return 'p'; // Lao Letter Pali Tta
    case 0xE90: return 'p'; // Lao Letter Pali Ttha
    case 0xE91: return 'p'; // Lao Letter Pali Dda
    case 0xE92: return 'p'; // Lao Letter Pali Ddha
    case 0xE93: return 'p'; // Lao Letter Pali Nna
    case 0xE94: return 'd'; // Lao Letter Do
    case 0xE95: return 't'; // Lao Letter To
    case 0xE96: return 't'; // Lao Letter Tho Sung
    case 0xE97: return 't'; // Lao Letter Tho Tam
    case 0xE98: return 'p'; // Lao Letter Pali Dha
    case 0xE99: return 'n'; // Lao Letter No
    case 0xE9A: return 'b'; // Lao Letter Bo
    case 0xE9B: return 'p'; // Lao Letter Po
    case 0xE9C: return 'p'; // Lao Letter Pho Sung
    case 0xE9D: return 'f'; // Lao Letter Fo Tam
    case 0xE9F: return 'f'; // Lao Letter Fo Sung
    case 0xEA0: return 'p'; // Lao Letter Pali Bha
    case 0xEA1: return 'm'; // Lao Letter Mo
    case 0xEA2: return 'y'; // Lao Letter Yo
    case 0xEA3: return 'l'; // Lao Letter Lo Ling
    case 0xEA8: return 's'; // Lao Letter Sanskrit Sha
    case 0xEA9: return 's'; // Lao Letter Sanskrit Ssa
    case 0xEAC: return 'p'; // Lao Letter Pali Lla
    case 0xEAD: return 'o'; // Lao Letter O
    case 0xEAE: return 'h'; // Lao Letter Ho Tam
    case 0xEB0: return 'a'; // Lao Vowel Sign A
    case 0xEB3: return 'a'; // Lao Vowel Sign Am
    case 0xEB4: return 'i'; // Lao Vowel Sign I
    case 0xEB5: return 'i'; // Lao Vowel Sign Ii
    case 0xEB6: return 'y'; // Lao Vowel Sign Y
    case 0xEB7: return 'y'; // Lao Vowel Sign Yy
    case 0xEB8: return 'u'; // Lao Vowel Sign U
    case 0xEB9: return 'u'; // Lao Vowel Sign Uu
    case 0xEBA: return 0; // Lao Sign Pali Virama
    case 0xEBB: return 'k'; // Lao Vowel Sign Mai Kon
    case 0xEC0: return 'e'; // Lao Vowel Sign E
    case 0xEC1: return 'e'; // Lao Vowel Sign Ei
    case 0xEC2: return 'o'; // Lao Vowel Sign O
    case 0xEC3: return 'a'; // Lao Vowel Sign Ay
    case 0xEC4: return 'a'; // Lao Vowel Sign Ai
    case 0xEC8: return 0; // Lao Tone Mai Ek
    case 0xECA: return 0; // Lao Tone Mai Ti
    case 0xECB: return 0; // Lao Tone Mai Catawa
    case 0xECC: return 0; // Lao Cancellation Mark
    case 0xECD: return 0; // Lao Niggahita
    case 0xED0: return '0'; // Lao Digit Zero
    case 0xED1: return '1'; // Lao Digit One
    case 0xED2: return '2'; // Lao Digit Two
    case 0xED3: return '3'; // Lao Digit Three
    case 0xED4: return '4'; // Lao Digit Four
    case 0xED5: return '5'; // Lao Digit Five
    case 0xED6: return '6'; // Lao Digit Six
    case 0xED7: return '7'; // Lao Digit Seven
    case 0xED8: return '8'; // Lao Digit Eight
    case 0xED9: return '9'; // Lao Digit Nine
    case 0xEDE: return 'k'; // Lao Letter Khmu Go
    case 0xEDF: return 'k'; // Lao Letter Khmu Nyo
    case 0xF00: return 'o'; // Tibetan Syllable Om
    case 0xF04: return '|'; // Tibetan Mark Initial Yig Mgo Mdun Ma
    case 0xF05: return '|'; // Tibetan Mark Closing Yig Mgo Sgab Ma
    case 0xF06: return 0; // Tibetan Mark Caret Yig Mgo Phur Shad Ma
    case 0xF07: return '|'; // Tibetan Mark Yig Mgo Tsheg Shad Ma
    case 0xF08: return '|'; // Tibetan Mark Sbrul Shad
    case 0xF09: return '|'; // Tibetan Mark Bskur Yig Mgo
    case 0xF0A: return '|'; // Tibetan Mark Bka- Shog Yig Mgo
    case 0xF0C: return ' '; // Tibetan Mark Delimiter Tsheg Bstar
    case 0xF0E: return '|'; // Tibetan Mark Nyis Shad
    case 0xF0F: return '|'; // Tibetan Mark Tsheg Shad
    case 0xF10: return '|'; // Tibetan Mark Nyis Tsheg Shad
    case 0xF11: return '|'; // Tibetan Mark Rin Chen Spungs Shad
    case 0xF12: return '|'; // Tibetan Mark Rgya Gram Shad
    case 0xF13: return 0; // Tibetan Mark Caret -Dzud Rtags Me Long Can
    case 0xF14: return '|'; // Tibetan Mark Gter Tsheg
    case 0xF15: return 'o'; // Tibetan Logotype Sign Chad Rtags
    case 0xF16: return 'o'; // Tibetan Logotype Sign Lhag Rtags
    case 0xF17: return '*'; // Tibetan Astrological Sign Sgra Gcan -Char Rtags
    case 0xF18: return '*'; // Tibetan Astrological Sign -Khyud Pa
    case 0xF19: return '*'; // Tibetan Astrological Sign Sdong Tshugs
    case 0xF1A: return '|'; // Tibetan Sign Rdel Dkar Gcig
    case 0xF1B: return '|'; // Tibetan Sign Rdel Dkar Gnyis
    case 0xF1C: return '|'; // Tibetan Sign Rdel Dkar Gsum
    case 0xF1D: return '|'; // Tibetan Sign Rdel Nag Gcig
    case 0xF1E: return '|'; // Tibetan Sign Rdel Nag Gnyis
    case 0xF1F: return '|'; // Tibetan Sign Rdel Dkar Rdel Nag
    case 0xF20: return '0'; // Tibetan Digit Zero
    case 0xF21: return '1'; // Tibetan Digit One
    case 0xF22: return '2'; // Tibetan Digit Two
    case 0xF23: return '3'; // Tibetan Digit Three
    case 0xF24: return '4'; // Tibetan Digit Four
    case 0xF25: return '5'; // Tibetan Digit Five
    case 0xF26: return '6'; // Tibetan Digit Six
    case 0xF27: return '7'; // Tibetan Digit Seven
    case 0xF28: return '8'; // Tibetan Digit Eight
    case 0xF29: return '9'; // Tibetan Digit Nine
    case 0xF2A: return '1'; // Tibetan Digit Half One
    case 0xF2B: return '2'; // Tibetan Digit Half Two
    case 0xF2C: return '3'; // Tibetan Digit Half Three
    case 0xF2D: return '4'; // Tibetan Digit Half Four
    case 0xF2E: return '5'; // Tibetan Digit Half Five
    case 0xF2F: return '6'; // Tibetan Digit Half Six
    case 0xF30: return '7'; // Tibetan Digit Half Seven
    case 0xF31: return '8'; // Tibetan Digit Half Eight
    case 0xF32: return '9'; // Tibetan Digit Half Nine
    case 0xF33: return '0'; // Tibetan Digit Half Zero
    case 0xF34: return '|'; // Tibetan Mark Bsdus Rtags
    case 0xF35: return 0; // Tibetan Mark Ngas Bzung Nyi Zla
    case 0xF36: return 0; // Tibetan Mark Caret -Dzud Rtags Bzhi Mig Can
    case 0xF37: return 0; // Tibetan Mark Ngas Bzung Sgor Rtags
    case 0xF38: return 0; // Tibetan Mark Che Mgo
    case 0xF39: return 0; // Tibetan Mark Tsa -Phru
    case 0xF3A: return '('; // Tibetan Mark Gug Rtags Gyon
    case 0xF3B: return ')'; // Tibetan Mark Gug Rtags Gyas
    case 0xF3C: return '('; // Tibetan Mark Ang Khang Gyon
    case 0xF3D: return ')'; // Tibetan Mark Ang Khang Gyas
    case 0xF3E: return ','; // Tibetan Sign Yar Tshes
    case 0xF3F: return '.'; // Tibetan Sign Mar Tshes
    case 0xF40: return 'k'; // Tibetan Letter Ka
    case 0xF43: return 'g'; // Tibetan Letter Gha
    case 0xF45: return 'c'; // Tibetan Letter Ca
    case 0xF46: return 'c'; // Tibetan Letter Cha
    case 0xF49: return 'n'; // Tibetan Letter Nya
    case 0xF4A: return 't'; // Tibetan Letter Tta
    case 0xF4B: return 't'; // Tibetan Letter Ttha
    case 0xF4C: return 'd'; // Tibetan Letter Dda
    case 0xF4D: return 'd'; // Tibetan Letter Ddha
    case 0xF4E: return 'n'; // Tibetan Letter Nna
    case 0xF4F: return 't'; // Tibetan Letter Ta
    case 0xF50: return 't'; // Tibetan Letter Tha
    case 0xF52: return 'd'; // Tibetan Letter Dha
    case 0xF53: return 'n'; // Tibetan Letter Na
    case 0xF54: return 'p'; // Tibetan Letter Pa
    case 0xF55: return 'p'; // Tibetan Letter Pha
    case 0xF57: return 'b'; // Tibetan Letter Bha
    case 0xF5A: return 't'; // Tibetan Letter Tsha
    case 0xF5B: return 'd'; // Tibetan Letter Dza
    case 0xF5C: return 'd'; // Tibetan Letter Dzha
    case 0xF5D: return 'w'; // Tibetan Letter Wa
    case 0xF5E: return 'z'; // Tibetan Letter Zha
    case 0xF5F: return 'z'; // Tibetan Letter Za
    case 0xF63: return 'l'; // Tibetan Letter La
    case 0xF65: return 's'; // Tibetan Letter Ssa
    case 0xF67: return 'h'; // Tibetan Letter Ha
    case 0xF68: return 'a'; // Tibetan Letter A
    case 0xF69: return 'k'; // Tibetan Letter Kssa
    case 0xF6A: return 'r'; // Tibetan Letter Fixed-Form Ra
    case 0xF6B: return 'k'; // Tibetan Letter Kka
    case 0xF6C: return 'r'; // Tibetan Letter Rra
    case 0xF71: return 'a'; // Tibetan Vowel Sign Aa
    case 0xF73: return 'i'; // Tibetan Vowel Sign Ii
    case 0xF74: return 'u'; // Tibetan Vowel Sign U
    case 0xF75: return 'u'; // Tibetan Vowel Sign Uu
    case 0xF76: return 'r'; // Tibetan Vowel Sign Vocalic R
    case 0xF77: return 'r'; // Tibetan Vowel Sign Vocalic Rr
    case 0xF78: return 'l'; // Tibetan Vowel Sign Vocalic L
    case 0xF79: return 'l'; // Tibetan Vowel Sign Vocalic Ll
    case 0xF7A: return 'e'; // Tibetan Vowel Sign E
    case 0xF7B: return 'e'; // Tibetan Vowel Sign Ee
    case 0xF7D: return 'o'; // Tibetan Vowel Sign Oo
    case 0xF80: return 'r'; // Tibetan Vowel Sign Reversed I
    case 0xF81: return 'r'; // Tibetan Vowel Sign Reversed Ii
    case 0xF90: return 'k'; // Tibetan Subjoined Letter Ka
    case 0xF91: return 'k'; // Tibetan Subjoined Letter Kha
    case 0xF92: return 'g'; // Tibetan Subjoined Letter Ga
    case 0xF93: return 'g'; // Tibetan Subjoined Letter Gha
    case 0xF94: return 'n'; // Tibetan Subjoined Letter Nga
    case 0xF95: return 'c'; // Tibetan Subjoined Letter Ca
    case 0xF96: return 'c'; // Tibetan Subjoined Letter Cha
    case 0xF97: return 'j'; // Tibetan Subjoined Letter Ja
    case 0xF99: return 'n'; // Tibetan Subjoined Letter Nya
    case 0xF9A: return 't'; // Tibetan Subjoined Letter Tta
    case 0xF9B: return 't'; // Tibetan Subjoined Letter Ttha
    case 0xF9C: return 'd'; // Tibetan Subjoined Letter Dda
    case 0xF9D: return 'd'; // Tibetan Subjoined Letter Ddha
    case 0xF9E: return 'n'; // Tibetan Subjoined Letter Nna
    case 0xF9F: return 't'; // Tibetan Subjoined Letter Ta
    case 0xFA0: return 't'; // Tibetan Subjoined Letter Tha
    case 0xFA1: return 'd'; // Tibetan Subjoined Letter Da
    case 0xFA2: return 'd'; // Tibetan Subjoined Letter Dha
    case 0xFA3: return 'n'; // Tibetan Subjoined Letter Na
    case 0xFA4: return 'p'; // Tibetan Subjoined Letter Pa
    case 0xFA5: return 'p'; // Tibetan Subjoined Letter Pha
    case 0xFA6: return 'b'; // Tibetan Subjoined Letter Ba
    case 0xFA7: return 'b'; // Tibetan Subjoined Letter Bha
    case 0xFA8: return 'm'; // Tibetan Subjoined Letter Ma
    case 0xFA9: return 't'; // Tibetan Subjoined Letter Tsa
    case 0xFAA: return 't'; // Tibetan Subjoined Letter Tsha
    case 0xFAC: return 'd'; // Tibetan Subjoined Letter Dzha
    case 0xFAD: return 'w'; // Tibetan Subjoined Letter Wa
    case 0xFAE: return 'z'; // Tibetan Subjoined Letter Zha
    case 0xFAF: return 'z'; // Tibetan Subjoined Letter Za
    case 0xFB1: return 'y'; // Tibetan Subjoined Letter Ya
    case 0xFB2: return 'r'; // Tibetan Subjoined Letter Ra
    case 0xFB3: return 'l'; // Tibetan Subjoined Letter La
    case 0xFB4: return 's'; // Tibetan Subjoined Letter Sha
    case 0xFB5: return 's'; // Tibetan Subjoined Letter Ssa
    case 0xFB6: return 's'; // Tibetan Subjoined Letter Sa
    case 0xFB7: return 'h'; // Tibetan Subjoined Letter Ha
    case 0xFB8: return 'a'; // Tibetan Subjoined Letter A
    case 0xFB9: return 'k'; // Tibetan Subjoined Letter Kssa
    case 0xFBA: return 'w'; // Tibetan Subjoined Letter Fixed-Form Wa
    case 0xFBB: return 'y'; // Tibetan Subjoined Letter Fixed-Form Ya
    case 0xFBC: return 'r'; // Tibetan Subjoined Letter Fixed-Form Ra
    case 0x1022: return 's'; // Myanmar Letter Shan A
    case 0x1023: return 'i'; // Myanmar Letter I
    case 0x1024: return 'i'; // Myanmar Letter Ii
    case 0x1025: return 'u'; // Myanmar Letter U
    case 0x1026: return 'u'; // Myanmar Letter Uu
    case 0x1027: return 'e'; // Myanmar Letter E
    case 0x1028: return 'm'; // Myanmar Letter Mon E
    case 0x1029: return 'o'; // Myanmar Letter O
    case 0x102A: return 'a'; // Myanmar Letter Au
    case 0x1033: return 'm'; // Myanmar Vowel Sign Mon Ii
    case 0x1034: return 'm'; // Myanmar Vowel Sign Mon O
    case 0x1035: return 'e'; // Myanmar Vowel Sign E Above
    case 0x103F: return 's'; // Myanmar Letter Great Sa
    case 0x1040: return '0'; // Myanmar Digit Zero
    case 0x1041: return '1'; // Myanmar Digit One
    case 0x1042: return '2'; // Myanmar Digit Two
    case 0x1043: return '3'; // Myanmar Digit Three
    case 0x1044: return '4'; // Myanmar Digit Four
    case 0x1045: return '5'; // Myanmar Digit Five
    case 0x1046: return '6'; // Myanmar Digit Six
    case 0x1047: return '7'; // Myanmar Digit Seven
    case 0x1048: return '8'; // Myanmar Digit Eight
    case 0x1049: return '9'; // Myanmar Digit Nine
    case 0x104C: return '.'; // Myanmar Symbol Locative
    case 0x104D: return '.'; // Myanmar Symbol Completed
    case 0x104E: return 'n'; // Myanmar Symbol Aforementioned
    case 0x104F: return '.'; // Myanmar Symbol Genitive
    case 0x1050: return 's'; // Myanmar Letter Sha
    case 0x1051: return 's'; // Myanmar Letter Ssa
    case 0x1052: return 'r'; // Myanmar Letter Vocalic R
    case 0x1053: return 'r'; // Myanmar Letter Vocalic Rr
    case 0x1054: return 'l'; // Myanmar Letter Vocalic L
    case 0x1055: return 'l'; // Myanmar Letter Vocalic Ll
    case 0x1056: return 'r'; // Myanmar Vowel Sign Vocalic R
    case 0x1057: return 'r'; // Myanmar Vowel Sign Vocalic Rr
    case 0x1058: return 'l'; // Myanmar Vowel Sign Vocalic L
    case 0x1059: return 'l'; // Myanmar Vowel Sign Vocalic Ll
    case 0x105A: return 'm'; // Myanmar Letter Mon Nga
    case 0x105B: return 'm'; // Myanmar Letter Mon Jha
    case 0x105C: return 'm'; // Myanmar Letter Mon Bba
    case 0x105D: return 'm'; // Myanmar Letter Mon Bbe
    case 0x1061: return 's'; // Myanmar Letter Sgaw Karen Sha
    case 0x1062: return 's'; // Myanmar Vowel Sign Sgaw Karen Eu
    case 0x1063: return 0; // Myanmar Tone Mark Sgaw Karen Hathi
    case 0x1064: return 0; // Myanmar Tone Mark Sgaw Karen Ke Pho
    case 0x1065: return 'w'; // Myanmar Letter Western Pwo Karen Tha
    case 0x1066: return 'w'; // Myanmar Letter Western Pwo Karen Pwa
    case 0x1067: return 'w'; // Myanmar Vowel Sign Western Pwo Karen Eu
    case 0x1068: return 'w'; // Myanmar Vowel Sign Western Pwo Karen Ue
    case 0x106E: return 'e'; // Myanmar Letter Eastern Pwo Karen Nna
    case 0x106F: return 'e'; // Myanmar Letter Eastern Pwo Karen Ywa
    case 0x1070: return 'e'; // Myanmar Letter Eastern Pwo Karen Ghwa
    case 0x1071: return 'g'; // Myanmar Vowel Sign Geba Karen I
    case 0x1072: return 'k'; // Myanmar Vowel Sign Kayah Oe
    case 0x1073: return 'k'; // Myanmar Vowel Sign Kayah U
    case 0x1074: return 'k'; // Myanmar Vowel Sign Kayah Ee
    case 0x1076: return 's'; // Myanmar Letter Shan Kha
    case 0x1077: return 's'; // Myanmar Letter Shan Ga
    case 0x1078: return 's'; // Myanmar Letter Shan Ca
    case 0x1079: return 's'; // Myanmar Letter Shan Za
    case 0x107A: return 's'; // Myanmar Letter Shan Nya
    case 0x107B: return 's'; // Myanmar Letter Shan Da
    case 0x107E: return 's'; // Myanmar Letter Shan Fa
    case 0x107F: return 's'; // Myanmar Letter Shan Ba
    case 0x1080: return 's'; // Myanmar Letter Shan Tha
    case 0x1084: return 's'; // Myanmar Vowel Sign Shan E
    case 0x108D: return 0; // Myanmar Sign Shan Council Emphatic Tone
    case 0x108E: return 'r'; // Myanmar Letter Rumai Palaung Fa
    case 0x1090: return '0'; // Myanmar Shan Digit Zero
    case 0x1091: return '1'; // Myanmar Shan Digit One
    case 0x1092: return '2'; // Myanmar Shan Digit Two
    case 0x1093: return '3'; // Myanmar Shan Digit Three
    case 0x1094: return '4'; // Myanmar Shan Digit Four
    case 0x1095: return '5'; // Myanmar Shan Digit Five
    case 0x1096: return '6'; // Myanmar Shan Digit Six
    case 0x1097: return '7'; // Myanmar Shan Digit Seven
    case 0x1098: return '8'; // Myanmar Shan Digit Eight
    case 0x1099: return '9'; // Myanmar Shan Digit Nine
    case 0x109C: return 'a'; // Myanmar Vowel Sign Aiton A
    case 0x109D: return 'a'; // Myanmar Vowel Sign Aiton Ai
    case 0x109E: return '1'; // Myanmar Symbol Shan One
    case 0x109F: return '!'; // Myanmar Symbol Shan Exclamation
    case 0x1200: return 'h'; // Ethiopic Syllable Ha
    case 0x1201: return 'h'; // Ethiopic Syllable Hu
    case 0x1202: return 'h'; // Ethiopic Syllable Hi
    case 0x1203: return 'h'; // Ethiopic Syllable Haa
    case 0x1204: return 'h'; // Ethiopic Syllable Hee
    case 0x1205: return 'h'; // Ethiopic Syllable He
    case 0x1206: return 'h'; // Ethiopic Syllable Ho
    case 0x1207: return 'h'; // Ethiopic Syllable Hoa
    case 0x1208: return 'l'; // Ethiopic Syllable La
    case 0x1209: return 'l'; // Ethiopic Syllable Lu
    case 0x120A: return 'l'; // Ethiopic Syllable Li
    case 0x120B: return 'l'; // Ethiopic Syllable Laa
    case 0x120C: return 'l'; // Ethiopic Syllable Lee
    case 0x120D: return 'l'; // Ethiopic Syllable Le
    case 0x120E: return 'l'; // Ethiopic Syllable Lo
    case 0x120F: return 'l'; // Ethiopic Syllable Lwa
    case 0x1210: return 'h'; // Ethiopic Syllable Hha
    case 0x1211: return 'h'; // Ethiopic Syllable Hhu
    case 0x1212: return 'h'; // Ethiopic Syllable Hhi
    case 0x1213: return 'h'; // Ethiopic Syllable Hhaa
    case 0x1214: return 'h'; // Ethiopic Syllable Hhee
    case 0x1215: return 'h'; // Ethiopic Syllable Hhe
    case 0x1216: return 'h'; // Ethiopic Syllable Hho
    case 0x1217: return 'h'; // Ethiopic Syllable Hhwa
    case 0x1219: return 'm'; // Ethiopic Syllable Mu
    case 0x121A: return 'm'; // Ethiopic Syllable Mi
    case 0x121C: return 'm'; // Ethiopic Syllable Mee
    case 0x121D: return 'm'; // Ethiopic Syllable Me
    case 0x121E: return 'm'; // Ethiopic Syllable Mo
    case 0x121F: return 'm'; // Ethiopic Syllable Mwa
    case 0x1220: return 's'; // Ethiopic Syllable Sza
    case 0x1221: return 's'; // Ethiopic Syllable Szu
    case 0x1222: return 's'; // Ethiopic Syllable Szi
    case 0x1223: return 's'; // Ethiopic Syllable Szaa
    case 0x1224: return 's'; // Ethiopic Syllable Szee
    case 0x1225: return 's'; // Ethiopic Syllable Sze
    case 0x1226: return 's'; // Ethiopic Syllable Szo
    case 0x1227: return 's'; // Ethiopic Syllable Szwa
    case 0x1228: return 'r'; // Ethiopic Syllable Ra
    case 0x1229: return 'r'; // Ethiopic Syllable Ru
    case 0x122A: return 'r'; // Ethiopic Syllable Ri
    case 0x122B: return 'r'; // Ethiopic Syllable Raa
    case 0x122E: return 'r'; // Ethiopic Syllable Ro
    case 0x122F: return 'r'; // Ethiopic Syllable Rwa
    case 0x1230: return 's'; // Ethiopic Syllable Sa
    case 0x1231: return 's'; // Ethiopic Syllable Su
    case 0x1233: return 's'; // Ethiopic Syllable Saa
    case 0x1234: return 's'; // Ethiopic Syllable See
    case 0x1235: return 's'; // Ethiopic Syllable Se
    case 0x1236: return 's'; // Ethiopic Syllable So
    case 0x1237: return 's'; // Ethiopic Syllable Swa
    case 0x1238: return 's'; // Ethiopic Syllable Sha
    case 0x1239: return 's'; // Ethiopic Syllable Shu
    case 0x123A: return 's'; // Ethiopic Syllable Shi
    case 0x123B: return 's'; // Ethiopic Syllable Shaa
    case 0x123C: return 's'; // Ethiopic Syllable Shee
    case 0x123D: return 's'; // Ethiopic Syllable She
    case 0x123E: return 's'; // Ethiopic Syllable Sho
    case 0x123F: return 's'; // Ethiopic Syllable Shwa
    case 0x1240: return 'q'; // Ethiopic Syllable Qa
    case 0x1241: return 'q'; // Ethiopic Syllable Qu
    case 0x1242: return 'q'; // Ethiopic Syllable Qi
    case 0x1243: return 'q'; // Ethiopic Syllable Qaa
    case 0x1244: return 'q'; // Ethiopic Syllable Qee
    case 0x1245: return 'q'; // Ethiopic Syllable Qe
    case 0x1246: return 'q'; // Ethiopic Syllable Qo
    case 0x1247: return 'q'; // Ethiopic Syllable Qoa
    case 0x1248: return 'q'; // Ethiopic Syllable Qwa
    case 0x124A: return 'q'; // Ethiopic Syllable Qwi
    case 0x124B: return 'q'; // Ethiopic Syllable Qwaa
    case 0x124C: return 'q'; // Ethiopic Syllable Qwee
    case 0x124D: return 'q'; // Ethiopic Syllable Qwe
    case 0x1250: return 'q'; // Ethiopic Syllable Qha
    case 0x1251: return 'q'; // Ethiopic Syllable Qhu
    case 0x1252: return 'q'; // Ethiopic Syllable Qhi
    case 0x1253: return 'q'; // Ethiopic Syllable Qhaa
    case 0x1254: return 'q'; // Ethiopic Syllable Qhee
    case 0x1255: return 'q'; // Ethiopic Syllable Qhe
    case 0x1256: return 'q'; // Ethiopic Syllable Qho
    case 0x1258: return 'q'; // Ethiopic Syllable Qhwa
    case 0x125A: return 'q'; // Ethiopic Syllable Qhwi
    case 0x125B: return 'q'; // Ethiopic Syllable Qhwaa
    case 0x125C: return 'q'; // Ethiopic Syllable Qhwee
    case 0x125D: return 'q'; // Ethiopic Syllable Qhwe
    case 0x1261: return 'b'; // Ethiopic Syllable Bu
    case 0x1262: return 'b'; // Ethiopic Syllable Bi
    case 0x1263: return 'b'; // Ethiopic Syllable Baa
    case 0x1264: return 'b'; // Ethiopic Syllable Bee
    case 0x1265: return 'b'; // Ethiopic Syllable Be
    case 0x1266: return 'b'; // Ethiopic Syllable Bo
    case 0x1267: return 'b'; // Ethiopic Syllable Bwa
    case 0x1268: return 'v'; // Ethiopic Syllable Va
    case 0x1269: return 'v'; // Ethiopic Syllable Vu
    case 0x126A: return 'v'; // Ethiopic Syllable Vi
    case 0x126B: return 'v'; // Ethiopic Syllable Vaa
    case 0x126C: return 'v'; // Ethiopic Syllable Vee
    case 0x126D: return 'v'; // Ethiopic Syllable Ve
    case 0x126E: return 'v'; // Ethiopic Syllable Vo
    case 0x126F: return 'v'; // Ethiopic Syllable Vwa
    case 0x1270: return 't'; // Ethiopic Syllable Ta
    case 0x1271: return 't'; // Ethiopic Syllable Tu
    case 0x1272: return 't'; // Ethiopic Syllable Ti
    case 0x1273: return 't'; // Ethiopic Syllable Taa
    case 0x1274: return 't'; // Ethiopic Syllable Tee
    case 0x1276: return 't'; // Ethiopic Syllable To
    case 0x1277: return 't'; // Ethiopic Syllable Twa
    case 0x1278: return 'c'; // Ethiopic Syllable Ca
    case 0x1279: return 'c'; // Ethiopic Syllable Cu
    case 0x127A: return 'c'; // Ethiopic Syllable Ci
    case 0x127B: return 'c'; // Ethiopic Syllable Caa
    case 0x127C: return 'c'; // Ethiopic Syllable Cee
    case 0x127D: return 'c'; // Ethiopic Syllable Ce
    case 0x127E: return 'c'; // Ethiopic Syllable Co
    case 0x127F: return 'c'; // Ethiopic Syllable Cwa
    case 0x1280: return 'x'; // Ethiopic Syllable Xa
    case 0x1281: return 'x'; // Ethiopic Syllable Xu
    case 0x1282: return 'x'; // Ethiopic Syllable Xi
    case 0x1283: return 'x'; // Ethiopic Syllable Xaa
    case 0x1284: return 'x'; // Ethiopic Syllable Xee
    case 0x1285: return 'x'; // Ethiopic Syllable Xe
    case 0x1286: return 'x'; // Ethiopic Syllable Xo
    case 0x1287: return 'x'; // Ethiopic Syllable Xoa
    case 0x1288: return 'x'; // Ethiopic Syllable Xwa
    case 0x128A: return 'x'; // Ethiopic Syllable Xwi
    case 0x128B: return 'x'; // Ethiopic Syllable Xwaa
    case 0x128C: return 'x'; // Ethiopic Syllable Xwee
    case 0x128D: return 'x'; // Ethiopic Syllable Xwe
    case 0x1290: return 'n'; // Ethiopic Syllable Na
    case 0x1291: return 'n'; // Ethiopic Syllable Nu
    case 0x1292: return 'n'; // Ethiopic Syllable Ni
    case 0x1294: return 'n'; // Ethiopic Syllable Nee
    case 0x1295: return 'n'; // Ethiopic Syllable Ne
    case 0x1296: return 'n'; // Ethiopic Syllable No
    case 0x1297: return 'n'; // Ethiopic Syllable Nwa
    case 0x1298: return 'n'; // Ethiopic Syllable Nya
    case 0x1299: return 'n'; // Ethiopic Syllable Nyu
    case 0x129A: return 'n'; // Ethiopic Syllable Nyi
    case 0x129C: return 'n'; // Ethiopic Syllable Nyee
    case 0x129D: return 'n'; // Ethiopic Syllable Nye
    case 0x129E: return 'n'; // Ethiopic Syllable Nyo
    case 0x129F: return 'n'; // Ethiopic Syllable Nywa
    case 0x12A1: return 'u'; // Ethiopic Syllable Glottal U
    case 0x12A2: return 'i'; // Ethiopic Syllable Glottal I
    case 0x12A3: return 'a'; // Ethiopic Syllable Glottal Aa
    case 0x12A4: return 'e'; // Ethiopic Syllable Glottal Ee
    case 0x12A5: return 'e'; // Ethiopic Syllable Glottal E
    case 0x12A6: return 'o'; // Ethiopic Syllable Glottal O
    case 0x12A7: return 'a'; // Ethiopic Syllable Glottal Wa
    case 0x12A8: return 'k'; // Ethiopic Syllable Ka
    case 0x12A9: return 'k'; // Ethiopic Syllable Ku
    case 0x12AA: return 'k'; // Ethiopic Syllable Ki
    case 0x12AB: return 'k'; // Ethiopic Syllable Kaa
    case 0x12AC: return 'k'; // Ethiopic Syllable Kee
    case 0x12AD: return 'k'; // Ethiopic Syllable Ke
    case 0x12AE: return 'k'; // Ethiopic Syllable Ko
    case 0x12AF: return 'k'; // Ethiopic Syllable Koa
    case 0x12B0: return 'k'; // Ethiopic Syllable Kwa
    case 0x12B2: return 'k'; // Ethiopic Syllable Kwi
    case 0x12B3: return 'k'; // Ethiopic Syllable Kwaa
    case 0x12B4: return 'k'; // Ethiopic Syllable Kwee
    case 0x12B5: return 'k'; // Ethiopic Syllable Kwe
    case 0x12B8: return 'k'; // Ethiopic Syllable Kxa
    case 0x12B9: return 'k'; // Ethiopic Syllable Kxu
    case 0x12BA: return 'k'; // Ethiopic Syllable Kxi
    case 0x12BB: return 'k'; // Ethiopic Syllable Kxaa
    case 0x12BC: return 'k'; // Ethiopic Syllable Kxee
    case 0x12BD: return 'k'; // Ethiopic Syllable Kxe
    case 0x12BE: return 'k'; // Ethiopic Syllable Kxo
    case 0x12C0: return 'k'; // Ethiopic Syllable Kxwa
    case 0x12C2: return 'k'; // Ethiopic Syllable Kxwi
    case 0x12C3: return 'k'; // Ethiopic Syllable Kxwaa
    case 0x12C4: return 'k'; // Ethiopic Syllable Kxwee
    case 0x12C5: return 'k'; // Ethiopic Syllable Kxwe
    case 0x12C8: return 'w'; // Ethiopic Syllable Wa
    case 0x12C9: return 'w'; // Ethiopic Syllable Wu
    case 0x12CA: return 'w'; // Ethiopic Syllable Wi
    case 0x12CC: return 'w'; // Ethiopic Syllable Wee
    case 0x12CE: return 'w'; // Ethiopic Syllable Wo
    case 0x12CF: return 'w'; // Ethiopic Syllable Woa
    case 0x12D0: return 'a'; // Ethiopic Syllable Pharyngeal A
    case 0x12D1: return 'u'; // Ethiopic Syllable Pharyngeal U
    case 0x12D2: return 'i'; // Ethiopic Syllable Pharyngeal I
    case 0x12D3: return 'a'; // Ethiopic Syllable Pharyngeal Aa
    case 0x12D4: return 'e'; // Ethiopic Syllable Pharyngeal Ee
    case 0x12D5: return 'e'; // Ethiopic Syllable Pharyngeal E
    case 0x12D6: return 'o'; // Ethiopic Syllable Pharyngeal O
    case 0x12D8: return 'z'; // Ethiopic Syllable Za
    case 0x12D9: return 'z'; // Ethiopic Syllable Zu
    case 0x12DA: return 'z'; // Ethiopic Syllable Zi
    case 0x12DB: return 'z'; // Ethiopic Syllable Zaa
    case 0x12DC: return 'z'; // Ethiopic Syllable Zee
    case 0x12DD: return 'z'; // Ethiopic Syllable Ze
    case 0x12DE: return 'z'; // Ethiopic Syllable Zo
    case 0x12DF: return 'z'; // Ethiopic Syllable Zwa
    case 0x12E0: return 'z'; // Ethiopic Syllable Zha
    case 0x12E1: return 'z'; // Ethiopic Syllable Zhu
    case 0x12E2: return 'z'; // Ethiopic Syllable Zhi
    case 0x12E3: return 'z'; // Ethiopic Syllable Zhaa
    case 0x12E4: return 'z'; // Ethiopic Syllable Zhee
    case 0x12E5: return 'z'; // Ethiopic Syllable Zhe
    case 0x12E6: return 'z'; // Ethiopic Syllable Zho
    case 0x12E7: return 'z'; // Ethiopic Syllable Zhwa
    case 0x12E8: return 'y'; // Ethiopic Syllable Ya
    case 0x12E9: return 'y'; // Ethiopic Syllable Yu
    case 0x12EA: return 'y'; // Ethiopic Syllable Yi
    case 0x12EB: return 'y'; // Ethiopic Syllable Yaa
    case 0x12EC: return 'y'; // Ethiopic Syllable Yee
    case 0x12ED: return 'y'; // Ethiopic Syllable Ye
    case 0x12EE: return 'y'; // Ethiopic Syllable Yo
    case 0x12EF: return 'y'; // Ethiopic Syllable Yoa
    case 0x12F0: return 'd'; // Ethiopic Syllable Da
    case 0x12F1: return 'd'; // Ethiopic Syllable Du
    case 0x12F2: return 'd'; // Ethiopic Syllable Di
    case 0x12F4: return 'd'; // Ethiopic Syllable Dee
    case 0x12F5: return 'd'; // Ethiopic Syllable De
    case 0x12F6: return 'd'; // Ethiopic Syllable Do
    case 0x12F7: return 'd'; // Ethiopic Syllable Dwa
    case 0x12F8: return 'd'; // Ethiopic Syllable Dda
    case 0x12F9: return 'd'; // Ethiopic Syllable Ddu
    case 0x12FA: return 'd'; // Ethiopic Syllable Ddi
    case 0x12FB: return 'd'; // Ethiopic Syllable Ddaa
    case 0x12FC: return 'd'; // Ethiopic Syllable Ddee
    case 0x12FD: return 'd'; // Ethiopic Syllable Dde
    case 0x12FE: return 'd'; // Ethiopic Syllable Ddo
    case 0x12FF: return 'd'; // Ethiopic Syllable Ddwa
    case 0x1300: return 'j'; // Ethiopic Syllable Ja
    case 0x1301: return 'j'; // Ethiopic Syllable Ju
    case 0x1302: return 'j'; // Ethiopic Syllable Ji
    case 0x1303: return 'j'; // Ethiopic Syllable Jaa
    case 0x1304: return 'j'; // Ethiopic Syllable Jee
    case 0x1305: return 'j'; // Ethiopic Syllable Je
    case 0x1306: return 'j'; // Ethiopic Syllable Jo
    case 0x1307: return 'j'; // Ethiopic Syllable Jwa
    case 0x1309: return 'g'; // Ethiopic Syllable Gu
    case 0x130A: return 'g'; // Ethiopic Syllable Gi
    case 0x130B: return 'g'; // Ethiopic Syllable Gaa
    case 0x130C: return 'g'; // Ethiopic Syllable Gee
    case 0x130E: return 'g'; // Ethiopic Syllable Go
    case 0x130F: return 'g'; // Ethiopic Syllable Goa
    case 0x1310: return 'g'; // Ethiopic Syllable Gwa
    case 0x1312: return 'g'; // Ethiopic Syllable Gwi
    case 0x1313: return 'g'; // Ethiopic Syllable Gwaa
    case 0x1314: return 'g'; // Ethiopic Syllable Gwee
    case 0x1315: return 'g'; // Ethiopic Syllable Gwe
    case 0x1318: return 'g'; // Ethiopic Syllable Gga
    case 0x1319: return 'g'; // Ethiopic Syllable Ggu
    case 0x131A: return 'g'; // Ethiopic Syllable Ggi
    case 0x131B: return 'g'; // Ethiopic Syllable Ggaa
    case 0x131C: return 'g'; // Ethiopic Syllable Ggee
    case 0x131D: return 'g'; // Ethiopic Syllable Gge
    case 0x131E: return 'g'; // Ethiopic Syllable Ggo
    case 0x131F: return 'g'; // Ethiopic Syllable Ggwaa
    case 0x1320: return 't'; // Ethiopic Syllable Tha
    case 0x1321: return 't'; // Ethiopic Syllable Thu
    case 0x1322: return 't'; // Ethiopic Syllable Thi
    case 0x1323: return 't'; // Ethiopic Syllable Thaa
    case 0x1324: return 't'; // Ethiopic Syllable Thee
    case 0x1325: return 't'; // Ethiopic Syllable The
    case 0x1326: return 't'; // Ethiopic Syllable Tho
    case 0x1327: return 't'; // Ethiopic Syllable Thwa
    case 0x1328: return 'c'; // Ethiopic Syllable Cha
    case 0x1329: return 'c'; // Ethiopic Syllable Chu
    case 0x132A: return 'c'; // Ethiopic Syllable Chi
    case 0x132B: return 'c'; // Ethiopic Syllable Chaa
    case 0x132C: return 'c'; // Ethiopic Syllable Chee
    case 0x132D: return 'c'; // Ethiopic Syllable Che
    case 0x132E: return 'c'; // Ethiopic Syllable Cho
    case 0x132F: return 'c'; // Ethiopic Syllable Chwa
    case 0x1330: return 'p'; // Ethiopic Syllable Pha
    case 0x1331: return 'p'; // Ethiopic Syllable Phu
    case 0x1332: return 'p'; // Ethiopic Syllable Phi
    case 0x1333: return 'p'; // Ethiopic Syllable Phaa
    case 0x1334: return 'p'; // Ethiopic Syllable Phee
    case 0x1335: return 'p'; // Ethiopic Syllable Phe
    case 0x1336: return 'p'; // Ethiopic Syllable Pho
    case 0x1337: return 'p'; // Ethiopic Syllable Phwa
    case 0x1338: return 't'; // Ethiopic Syllable Tsa
    case 0x1339: return 't'; // Ethiopic Syllable Tsu
    case 0x133A: return 't'; // Ethiopic Syllable Tsi
    case 0x133B: return 't'; // Ethiopic Syllable Tsaa
    case 0x133C: return 't'; // Ethiopic Syllable Tsee
    case 0x133E: return 't'; // Ethiopic Syllable Tso
    case 0x133F: return 't'; // Ethiopic Syllable Tswa
    case 0x1340: return 't'; // Ethiopic Syllable Tza
    case 0x1341: return 't'; // Ethiopic Syllable Tzu
    case 0x1342: return 't'; // Ethiopic Syllable Tzi
    case 0x1343: return 't'; // Ethiopic Syllable Tzaa
    case 0x1344: return 't'; // Ethiopic Syllable Tzee
    case 0x1345: return 't'; // Ethiopic Syllable Tze
    case 0x1346: return 't'; // Ethiopic Syllable Tzo
    case 0x1347: return 't'; // Ethiopic Syllable Tzoa
    case 0x1348: return 'f'; // Ethiopic Syllable Fa
    case 0x1349: return 'f'; // Ethiopic Syllable Fu
    case 0x134A: return 'f'; // Ethiopic Syllable Fi
    case 0x134B: return 'f'; // Ethiopic Syllable Faa
    case 0x134C: return 'f'; // Ethiopic Syllable Fee
    case 0x134D: return 'f'; // Ethiopic Syllable Fe
    case 0x134E: return 'f'; // Ethiopic Syllable Fo
    case 0x134F: return 'f'; // Ethiopic Syllable Fwa
    case 0x1350: return 'p'; // Ethiopic Syllable Pa
    case 0x1351: return 'p'; // Ethiopic Syllable Pu
    case 0x1352: return 'p'; // Ethiopic Syllable Pi
    case 0x1353: return 'p'; // Ethiopic Syllable Paa
    case 0x1354: return 'p'; // Ethiopic Syllable Pee
    case 0x1355: return 'p'; // Ethiopic Syllable Pe
    case 0x1356: return 'p'; // Ethiopic Syllable Po
    case 0x1357: return 'p'; // Ethiopic Syllable Pwa
    case 0x1358: return 'r'; // Ethiopic Syllable Rya
    case 0x1359: return 'm'; // Ethiopic Syllable Mya
    case 0x135A: return 'f'; // Ethiopic Syllable Fya
    case 0x135D: return 0; // Ethiopic Combining Gemination And Vowel Length Mark
    case 0x135F: return 0; // Ethiopic Combining Gemination Mark
    case 0x1360: return '|'; // Ethiopic Section Mark
    case 0x1368: return '|'; // Ethiopic Paragraph Separator
    case 0x1369: return '1'; // Ethiopic Digit One
    case 0x136A: return '2'; // Ethiopic Digit Two
    case 0x136B: return '3'; // Ethiopic Digit Three
    case 0x136C: return '4'; // Ethiopic Digit Four
    case 0x136D: return '5'; // Ethiopic Digit Five
    case 0x136E: return '6'; // Ethiopic Digit Six
    case 0x136F: return '7'; // Ethiopic Digit Seven
    case 0x1370: return '8'; // Ethiopic Digit Eight
    case 0x1371: return '9'; // Ethiopic Digit Nine
    case 0x1372: return '1'; // Ethiopic Number Ten
    case 0x1373: return '2'; // Ethiopic Number Twenty
    case 0x1374: return '3'; // Ethiopic Number Thirty
    case 0x137B: return '1'; // Ethiopic Number Hundred
    case 0x137C: return '1'; // Ethiopic Number Ten Thousand
    case 0x1380: return 'm'; // Ethiopic Syllable Sebatbeit Mwa
    case 0x1381: return 'm'; // Ethiopic Syllable Mwi
    case 0x1382: return 'm'; // Ethiopic Syllable Mwee
    case 0x1383: return 'm'; // Ethiopic Syllable Mwe
    case 0x1384: return 'b'; // Ethiopic Syllable Sebatbeit Bwa
    case 0x1385: return 'b'; // Ethiopic Syllable Bwi
    case 0x1386: return 'b'; // Ethiopic Syllable Bwee
    case 0x1387: return 'b'; // Ethiopic Syllable Bwe
    case 0x1388: return 'f'; // Ethiopic Syllable Sebatbeit Fwa
    case 0x1389: return 'f'; // Ethiopic Syllable Fwi
    case 0x138A: return 'f'; // Ethiopic Syllable Fwee
    case 0x138B: return 'f'; // Ethiopic Syllable Fwe
    case 0x138C: return 'p'; // Ethiopic Syllable Sebatbeit Pwa
    case 0x138D: return 'p'; // Ethiopic Syllable Pwi
    case 0x138E: return 'p'; // Ethiopic Syllable Pwee
    case 0x138F: return 'p'; // Ethiopic Syllable Pwe
    case 0x1390: return 0; // Ethiopic Tonal Mark Yizet
    case 0x1391: return 0; // Ethiopic Tonal Mark Deret
    case 0x1392: return 0; // Ethiopic Tonal Mark Rikrik
    case 0x1393: return 0; // Ethiopic Tonal Mark Short Rikrik
    case 0x1394: return 0; // Ethiopic Tonal Mark Difat
    case 0x1395: return 0; // Ethiopic Tonal Mark Kenat
    case 0x1396: return 0; // Ethiopic Tonal Mark Chiret
    case 0x1397: return 0; // Ethiopic Tonal Mark Hidet
    case 0x1398: return 0; // Ethiopic Tonal Mark Deret-Hidet
    case 0x1399: return 0; // Ethiopic Tonal Mark Kurt
    case 0x13A0: return 'a'; // Cherokee Letter A
    case 0x13A1: return 'e'; // Cherokee Letter E
    case 0x13A2: return 'i'; // Cherokee Letter I
    case 0x13A3: return 'o'; // Cherokee Letter O
    case 0x13A5: return 'v'; // Cherokee Letter V
    case 0x13A6: return 'g'; // Cherokee Letter Ga
    case 0x13A7: return 'k'; // Cherokee Letter Ka
    case 0x13A8: return 'g'; // Cherokee Letter Ge
    case 0x13AA: return 'g'; // Cherokee Letter Go
    case 0x13AB: return 'g'; // Cherokee Letter Gu
    case 0x13AC: return 'g'; // Cherokee Letter Gv
    case 0x13AD: return 'h'; // Cherokee Letter Ha
    case 0x13AF: return 'h'; // Cherokee Letter Hi
    case 0x13B0: return 'h'; // Cherokee Letter Ho
    case 0x13B1: return 'h'; // Cherokee Letter Hu
    case 0x13B2: return 'h'; // Cherokee Letter Hv
    case 0x13B4: return 'l'; // Cherokee Letter Le
    case 0x13B6: return 'l'; // Cherokee Letter Lo
    case 0x13B7: return 'l'; // Cherokee Letter Lu
    case 0x13B8: return 'l'; // Cherokee Letter Lv
    case 0x13B9: return 'm'; // Cherokee Letter Ma
    case 0x13BA: return 'm'; // Cherokee Letter Me
    case 0x13BB: return 'm'; // Cherokee Letter Mi
    case 0x13BC: return 'm'; // Cherokee Letter Mo
    case 0x13BD: return 'm'; // Cherokee Letter Mu
    case 0x13BE: return 'n'; // Cherokee Letter Na
    case 0x13BF: return 'h'; // Cherokee Letter Hna
    case 0x13C0: return 'n'; // Cherokee Letter Nah
    case 0x13C1: return 'n'; // Cherokee Letter Ne
    case 0x13C2: return 'n'; // Cherokee Letter Ni
    case 0x13C3: return 'n'; // Cherokee Letter No
    case 0x13C4: return 'n'; // Cherokee Letter Nu
    case 0x13C5: return 'n'; // Cherokee Letter Nv
    case 0x13C6: return 'q'; // Cherokee Letter Qua
    case 0x13C7: return 'q'; // Cherokee Letter Que
    case 0x13C8: return 'q'; // Cherokee Letter Qui
    case 0x13C9: return 'q'; // Cherokee Letter Quo
    case 0x13CA: return 'q'; // Cherokee Letter Quu
    case 0x13CB: return 'q'; // Cherokee Letter Quv
    case 0x13CC: return 's'; // Cherokee Letter Sa
    case 0x13CE: return 's'; // Cherokee Letter Se
    case 0x13CF: return 's'; // Cherokee Letter Si
    case 0x13D0: return 's'; // Cherokee Letter So
    case 0x13D1: return 's'; // Cherokee Letter Su
    case 0x13D2: return 's'; // Cherokee Letter Sv
    case 0x13D3: return 'd'; // Cherokee Letter Da
    case 0x13D4: return 't'; // Cherokee Letter Ta
    case 0x13D5: return 'd'; // Cherokee Letter De
    case 0x13D6: return 't'; // Cherokee Letter Te
    case 0x13D8: return 't'; // Cherokee Letter Ti
    case 0x13D9: return 'd'; // Cherokee Letter Do
    case 0x13DA: return 'd'; // Cherokee Letter Du
    case 0x13DB: return 'd'; // Cherokee Letter Dv
    case 0x13DC: return 'd'; // Cherokee Letter Dla
    case 0x13DD: return 't'; // Cherokee Letter Tla
    case 0x13DE: return 't'; // Cherokee Letter Tle
    case 0x13DF: return 't'; // Cherokee Letter Tli
    case 0x13E0: return 't'; // Cherokee Letter Tlo
    case 0x13E1: return 't'; // Cherokee Letter Tlu
    case 0x13E2: return 't'; // Cherokee Letter Tlv
    case 0x13E4: return 't'; // Cherokee Letter Tse
    case 0x13E5: return 't'; // Cherokee Letter Tsi
    case 0x13E6: return 't'; // Cherokee Letter Tso
    case 0x13E7: return 't'; // Cherokee Letter Tsu
    case 0x13E8: return 't'; // Cherokee Letter Tsv
    case 0x13E9: return 'w'; // Cherokee Letter Wa
    case 0x13EA: return 'w'; // Cherokee Letter We
    case 0x13EB: return 'w'; // Cherokee Letter Wi
    case 0x13EC: return 'w'; // Cherokee Letter Wo
    case 0x13ED: return 'w'; // Cherokee Letter Wu
    case 0x13EE: return 'w'; // Cherokee Letter Wv
    case 0x13EF: return 'y'; // Cherokee Letter Ya
    case 0x13F0: return 'y'; // Cherokee Letter Ye
    case 0x13F1: return 'y'; // Cherokee Letter Yi
    case 0x13F2: return 'y'; // Cherokee Letter Yo
    case 0x13F3: return 'y'; // Cherokee Letter Yu
    case 0x13F4: return 'y'; // Cherokee Letter Yv
    case 0x13F5: return 'm'; // Cherokee Letter Mv
    case 0x13F8: return 'y'; // Cherokee Small Letter Ye
    case 0x13F9: return 'y'; // Cherokee Small Letter Yi
    case 0x13FA: return 'y'; // Cherokee Small Letter Yo
    case 0x13FB: return 'y'; // Cherokee Small Letter Yu
    case 0x13FC: return 'y'; // Cherokee Small Letter Yv
    case 0x13FD: return 'm'; // Cherokee Small Letter Mv
    case 0x1400: return 'h'; // Canadian Syllabics Hyphen
    case 0x1401: return 'e'; // Canadian Syllabics E
    case 0x1402: return 'a'; // Canadian Syllabics Aai
    case 0x1404: return 'i'; // Canadian Syllabics Ii
    case 0x1405: return 'o'; // Canadian Syllabics O
    case 0x1406: return 'o'; // Canadian Syllabics Oo
    case 0x1407: return 'y'; // Canadian Syllabics Y-Cree Oo
    case 0x1408: return 'c'; // Canadian Syllabics Carrier Ee
    case 0x1409: return 'c'; // Canadian Syllabics Carrier I
    case 0x140B: return 'a'; // Canadian Syllabics Aa
    case 0x140C: return 'w'; // Canadian Syllabics We
    case 0x140E: return 'w'; // Canadian Syllabics Wi
    case 0x1410: return 'w'; // Canadian Syllabics Wii
    case 0x1411: return 'w'; // Canadian Syllabics West-Cree Wii
    case 0x1412: return 'w'; // Canadian Syllabics Wo
    case 0x1413: return 'w'; // Canadian Syllabics West-Cree Wo
    case 0x1414: return 'w'; // Canadian Syllabics Woo
    case 0x1415: return 'w'; // Canadian Syllabics West-Cree Woo
    case 0x1416: return 'n'; // Canadian Syllabics Naskapi Woo
    case 0x1417: return 'w'; // Canadian Syllabics Wa
    case 0x1418: return 'w'; // Canadian Syllabics West-Cree Wa
    case 0x1419: return 'w'; // Canadian Syllabics Waa
    case 0x141A: return 'w'; // Canadian Syllabics West-Cree Waa
    case 0x141B: return 'n'; // Canadian Syllabics Naskapi Waa
    case 0x141C: return 'a'; // Canadian Syllabics Ai
    case 0x141D: return 'y'; // Canadian Syllabics Y-Cree W
    case 0x141E: return 'g'; // Canadian Syllabics Glottal Stop
    case 0x141F: return 0; // Canadian Syllabics Final Acute
    case 0x1420: return 0; // Canadian Syllabics Final Grave
    case 0x1421: return 0; // Canadian Syllabics Final Bottom Half Ring
    case 0x1422: return 0; // Canadian Syllabics Final Top Half Ring
    case 0x1424: return 0; // Canadian Syllabics Final Ring
    case 0x1425: return 0; // Canadian Syllabics Final Double Acute
    case 0x1427: return 0; // Canadian Syllabics Final Middle Dot
    case 0x1428: return 0; // Canadian Syllabics Final Short Horizontal Stroke
    case 0x1429: return 'f'; // Canadian Syllabics Final Plus
    case 0x142A: return 'f'; // Canadian Syllabics Final Down Tack
    case 0x142B: return 'e'; // Canadian Syllabics En
    case 0x142C: return 'i'; // Canadian Syllabics In
    case 0x142D: return 'o'; // Canadian Syllabics On
    case 0x142E: return 'a'; // Canadian Syllabics An
    case 0x142F: return 'p'; // Canadian Syllabics Pe
    case 0x1430: return 'p'; // Canadian Syllabics Paai
    case 0x1431: return 'p'; // Canadian Syllabics Pi
    case 0x1432: return 'p'; // Canadian Syllabics Pii
    case 0x1433: return 'p'; // Canadian Syllabics Po
    case 0x1434: return 'p'; // Canadian Syllabics Poo
    case 0x1435: return 'y'; // Canadian Syllabics Y-Cree Poo
    case 0x1436: return 'c'; // Canadian Syllabics Carrier Hee
    case 0x1437: return 'c'; // Canadian Syllabics Carrier Hi
    case 0x1438: return 'p'; // Canadian Syllabics Pa
    case 0x143A: return 'p'; // Canadian Syllabics Pwe
    case 0x143B: return 'w'; // Canadian Syllabics West-Cree Pwe
    case 0x143C: return 'p'; // Canadian Syllabics Pwi
    case 0x143D: return 'w'; // Canadian Syllabics West-Cree Pwi
    case 0x143E: return 'p'; // Canadian Syllabics Pwii
    case 0x143F: return 'w'; // Canadian Syllabics West-Cree Pwii
    case 0x1440: return 'p'; // Canadian Syllabics Pwo
    case 0x1441: return 'w'; // Canadian Syllabics West-Cree Pwo
    case 0x1442: return 'p'; // Canadian Syllabics Pwoo
    case 0x1443: return 'w'; // Canadian Syllabics West-Cree Pwoo
    case 0x1444: return 'p'; // Canadian Syllabics Pwa
    case 0x1445: return 'w'; // Canadian Syllabics West-Cree Pwa
    case 0x1446: return 'p'; // Canadian Syllabics Pwaa
    case 0x1447: return 'w'; // Canadian Syllabics West-Cree Pwaa
    case 0x1448: return 'y'; // Canadian Syllabics Y-Cree Pwaa
    case 0x1449: return 'p'; // Canadian Syllabics P
    case 0x144A: return 'w'; // Canadian Syllabics West-Cree P
    case 0x144B: return 'c'; // Canadian Syllabics Carrier H
    case 0x144D: return 't'; // Canadian Syllabics Taai
    case 0x144F: return 't'; // Canadian Syllabics Tii
    case 0x1451: return 't'; // Canadian Syllabics Too
    case 0x1452: return 'y'; // Canadian Syllabics Y-Cree Too
    case 0x1453: return 'c'; // Canadian Syllabics Carrier Dee
    case 0x1454: return 'c'; // Canadian Syllabics Carrier Di
    case 0x1456: return 't'; // Canadian Syllabics Taa
    case 0x1457: return 't'; // Canadian Syllabics Twe
    case 0x1458: return 'w'; // Canadian Syllabics West-Cree Twe
    case 0x1459: return 't'; // Canadian Syllabics Twi
    case 0x145A: return 'w'; // Canadian Syllabics West-Cree Twi
    case 0x145B: return 't'; // Canadian Syllabics Twii
    case 0x145C: return 'w'; // Canadian Syllabics West-Cree Twii
    case 0x145D: return 't'; // Canadian Syllabics Two
    case 0x145E: return 'w'; // Canadian Syllabics West-Cree Two
    case 0x145F: return 't'; // Canadian Syllabics Twoo
    case 0x1460: return 'w'; // Canadian Syllabics West-Cree Twoo
    case 0x1461: return 't'; // Canadian Syllabics Twa
    case 0x1462: return 'w'; // Canadian Syllabics West-Cree Twa
    case 0x1463: return 't'; // Canadian Syllabics Twaa
    case 0x1464: return 'w'; // Canadian Syllabics West-Cree Twaa
    case 0x1465: return 'n'; // Canadian Syllabics Naskapi Twaa
    case 0x1467: return 't'; // Canadian Syllabics Tte
    case 0x1468: return 't'; // Canadian Syllabics Tti
    case 0x1469: return 't'; // Canadian Syllabics Tto
    case 0x146A: return 't'; // Canadian Syllabics Tta
    case 0x146B: return 'k'; // Canadian Syllabics Ke
    case 0x146C: return 'k'; // Canadian Syllabics Kaai
    case 0x146D: return 'k'; // Canadian Syllabics Ki
    case 0x146E: return 'k'; // Canadian Syllabics Kii
    case 0x146F: return 'k'; // Canadian Syllabics Ko
    case 0x1470: return 'k'; // Canadian Syllabics Koo
    case 0x1471: return 'y'; // Canadian Syllabics Y-Cree Koo
    case 0x1473: return 'k'; // Canadian Syllabics Kaa
    case 0x1474: return 'k'; // Canadian Syllabics Kwe
    case 0x1475: return 'w'; // Canadian Syllabics West-Cree Kwe
    case 0x1476: return 'k'; // Canadian Syllabics Kwi
    case 0x1477: return 'w'; // Canadian Syllabics West-Cree Kwi
    case 0x1478: return 'k'; // Canadian Syllabics Kwii
    case 0x1479: return 'w'; // Canadian Syllabics West-Cree Kwii
    case 0x147A: return 'k'; // Canadian Syllabics Kwo
    case 0x147B: return 'w'; // Canadian Syllabics West-Cree Kwo
    case 0x147C: return 'k'; // Canadian Syllabics Kwoo
    case 0x147D: return 'w'; // Canadian Syllabics West-Cree Kwoo
    case 0x147E: return 'k'; // Canadian Syllabics Kwa
    case 0x147F: return 'w'; // Canadian Syllabics West-Cree Kwa
    case 0x1480: return 'k'; // Canadian Syllabics Kwaa
    case 0x1481: return 'w'; // Canadian Syllabics West-Cree Kwaa
    case 0x1482: return 'n'; // Canadian Syllabics Naskapi Kwaa
    case 0x1484: return 'k'; // Canadian Syllabics Kw
    case 0x1485: return 's'; // Canadian Syllabics South-Slavey Keh
    case 0x1486: return 's'; // Canadian Syllabics South-Slavey Kih
    case 0x1487: return 's'; // Canadian Syllabics South-Slavey Koh
    case 0x1488: return 's'; // Canadian Syllabics South-Slavey Kah
    case 0x1489: return 'c'; // Canadian Syllabics Ce
    case 0x148A: return 'c'; // Canadian Syllabics Caai
    case 0x148C: return 'c'; // Canadian Syllabics Cii
    case 0x148D: return 'c'; // Canadian Syllabics Co
    case 0x148E: return 'c'; // Canadian Syllabics Coo
    case 0x148F: return 'y'; // Canadian Syllabics Y-Cree Coo
    case 0x1490: return 'c'; // Canadian Syllabics Ca
    case 0x1491: return 'c'; // Canadian Syllabics Caa
    case 0x1492: return 'c'; // Canadian Syllabics Cwe
    case 0x1493: return 'w'; // Canadian Syllabics West-Cree Cwe
    case 0x1494: return 'c'; // Canadian Syllabics Cwi
    case 0x1495: return 'w'; // Canadian Syllabics West-Cree Cwi
    case 0x1496: return 'c'; // Canadian Syllabics Cwii
    case 0x1497: return 'w'; // Canadian Syllabics West-Cree Cwii
    case 0x1498: return 'c'; // Canadian Syllabics Cwo
    case 0x1499: return 'w'; // Canadian Syllabics West-Cree Cwo
    case 0x149A: return 'c'; // Canadian Syllabics Cwoo
    case 0x149B: return 'w'; // Canadian Syllabics West-Cree Cwoo
    case 0x149C: return 'c'; // Canadian Syllabics Cwa
    case 0x149D: return 'w'; // Canadian Syllabics West-Cree Cwa
    case 0x149E: return 'c'; // Canadian Syllabics Cwaa
    case 0x149F: return 'w'; // Canadian Syllabics West-Cree Cwaa
    case 0x14A0: return 'n'; // Canadian Syllabics Naskapi Cwaa
    case 0x14A1: return 'c'; // Canadian Syllabics C
    case 0x14A2: return 's'; // Canadian Syllabics Sayisi Th
    case 0x14A3: return 'm'; // Canadian Syllabics Me
    case 0x14A4: return 'm'; // Canadian Syllabics Maai
    case 0x14A6: return 'm'; // Canadian Syllabics Mii
    case 0x14A7: return 'm'; // Canadian Syllabics Mo
    case 0x14A8: return 'm'; // Canadian Syllabics Moo
    case 0x14A9: return 'y'; // Canadian Syllabics Y-Cree Moo
    case 0x14AA: return 'm'; // Canadian Syllabics Ma
    case 0x14AB: return 'm'; // Canadian Syllabics Maa
    case 0x14AC: return 'm'; // Canadian Syllabics Mwe
    case 0x14AD: return 'w'; // Canadian Syllabics West-Cree Mwe
    case 0x14AE: return 'm'; // Canadian Syllabics Mwi
    case 0x14AF: return 'w'; // Canadian Syllabics West-Cree Mwi
    case 0x14B0: return 'm'; // Canadian Syllabics Mwii
    case 0x14B1: return 'w'; // Canadian Syllabics West-Cree Mwii
    case 0x14B2: return 'm'; // Canadian Syllabics Mwo
    case 0x14B3: return 'w'; // Canadian Syllabics West-Cree Mwo
    case 0x14B4: return 'm'; // Canadian Syllabics Mwoo
    case 0x14B5: return 'w'; // Canadian Syllabics West-Cree Mwoo
    case 0x14B6: return 'm'; // Canadian Syllabics Mwa
    case 0x14B7: return 'w'; // Canadian Syllabics West-Cree Mwa
    case 0x14B8: return 'm'; // Canadian Syllabics Mwaa
    case 0x14B9: return 'w'; // Canadian Syllabics West-Cree Mwaa
    case 0x14BA: return 'n'; // Canadian Syllabics Naskapi Mwaa
    case 0x14BC: return 'w'; // Canadian Syllabics West-Cree M
    case 0x14BD: return 'm'; // Canadian Syllabics Mh
    case 0x14BE: return 'a'; // Canadian Syllabics Athapascan M
    case 0x14BF: return 's'; // Canadian Syllabics Sayisi M
    case 0x14C1: return 'n'; // Canadian Syllabics Naai
    case 0x14C5: return 'n'; // Canadian Syllabics Noo
    case 0x14C6: return 'y'; // Canadian Syllabics Y-Cree Noo
    case 0x14C7: return 'n'; // Canadian Syllabics Na
    case 0x14C8: return 'n'; // Canadian Syllabics Naa
    case 0x14C9: return 'n'; // Canadian Syllabics Nwe
    case 0x14CA: return 'w'; // Canadian Syllabics West-Cree Nwe
    case 0x14CB: return 'n'; // Canadian Syllabics Nwa
    case 0x14CC: return 'w'; // Canadian Syllabics West-Cree Nwa
    case 0x14CD: return 'n'; // Canadian Syllabics Nwaa
    case 0x14CE: return 'w'; // Canadian Syllabics West-Cree Nwaa
    case 0x14CF: return 'n'; // Canadian Syllabics Naskapi Nwaa
    case 0x14D1: return 'c'; // Canadian Syllabics Carrier Ng
    case 0x14D2: return 'n'; // Canadian Syllabics Nh
    case 0x14D3: return 'l'; // Canadian Syllabics Le
    case 0x14D4: return 'l'; // Canadian Syllabics Laai
    case 0x14D5: return 'l'; // Canadian Syllabics Li
    case 0x14D6: return 'l'; // Canadian Syllabics Lii
    case 0x14D7: return 'l'; // Canadian Syllabics Lo
    case 0x14D8: return 'l'; // Canadian Syllabics Loo
    case 0x14D9: return 'y'; // Canadian Syllabics Y-Cree Loo
    case 0x14DA: return 'l'; // Canadian Syllabics La
    case 0x14DB: return 'l'; // Canadian Syllabics Laa
    case 0x14DC: return 'l'; // Canadian Syllabics Lwe
    case 0x14DD: return 'w'; // Canadian Syllabics West-Cree Lwe
    case 0x14DE: return 'l'; // Canadian Syllabics Lwi
    case 0x14DF: return 'w'; // Canadian Syllabics West-Cree Lwi
    case 0x14E0: return 'l'; // Canadian Syllabics Lwii
    case 0x14E1: return 'w'; // Canadian Syllabics West-Cree Lwii
    case 0x14E2: return 'l'; // Canadian Syllabics Lwo
    case 0x14E3: return 'w'; // Canadian Syllabics West-Cree Lwo
    case 0x14E4: return 'l'; // Canadian Syllabics Lwoo
    case 0x14E5: return 'w'; // Canadian Syllabics West-Cree Lwoo
    case 0x14E6: return 'l'; // Canadian Syllabics Lwa
    case 0x14E7: return 'w'; // Canadian Syllabics West-Cree Lwa
    case 0x14E8: return 'l'; // Canadian Syllabics Lwaa
    case 0x14E9: return 'w'; // Canadian Syllabics West-Cree Lwaa
    case 0x14EA: return 'l'; // Canadian Syllabics L
    case 0x14EB: return 'w'; // Canadian Syllabics West-Cree L
    case 0x14EC: return 'm'; // Canadian Syllabics Medial L
    case 0x14ED: return 's'; // Canadian Syllabics Se
    case 0x14EE: return 's'; // Canadian Syllabics Saai
    case 0x14EF: return 's'; // Canadian Syllabics Si
    case 0x14F0: return 's'; // Canadian Syllabics Sii
    case 0x14F1: return 's'; // Canadian Syllabics So
    case 0x14F2: return 's'; // Canadian Syllabics Soo
    case 0x14F3: return 'y'; // Canadian Syllabics Y-Cree Soo
    case 0x14F4: return 's'; // Canadian Syllabics Sa
    case 0x14F5: return 's'; // Canadian Syllabics Saa
    case 0x14F6: return 's'; // Canadian Syllabics Swe
    case 0x14F7: return 'w'; // Canadian Syllabics West-Cree Swe
    case 0x14F8: return 's'; // Canadian Syllabics Swi
    case 0x14F9: return 'w'; // Canadian Syllabics West-Cree Swi
    case 0x14FA: return 's'; // Canadian Syllabics Swii
    case 0x14FB: return 'w'; // Canadian Syllabics West-Cree Swii
    case 0x14FC: return 's'; // Canadian Syllabics Swo
    case 0x14FD: return 'w'; // Canadian Syllabics West-Cree Swo
    case 0x14FE: return 's'; // Canadian Syllabics Swoo
    case 0x14FF: return 'w'; // Canadian Syllabics West-Cree Swoo
    case 0x1500: return 's'; // Canadian Syllabics Swa
    case 0x1501: return 'w'; // Canadian Syllabics West-Cree Swa
    case 0x1502: return 's'; // Canadian Syllabics Swaa
    case 0x1503: return 'w'; // Canadian Syllabics West-Cree Swaa
    case 0x1504: return 'n'; // Canadian Syllabics Naskapi Swaa
    case 0x1506: return 'a'; // Canadian Syllabics Athapascan S
    case 0x1507: return 's'; // Canadian Syllabics Sw
    case 0x1508: return 'b'; // Canadian Syllabics Blackfoot S
    case 0x1509: return 'm'; // Canadian Syllabics Moose-Cree Sk
    case 0x150A: return 'n'; // Canadian Syllabics Naskapi Skw
    case 0x150B: return 'n'; // Canadian Syllabics Naskapi S-W
    case 0x150C: return 'n'; // Canadian Syllabics Naskapi Spwa
    case 0x150D: return 'n'; // Canadian Syllabics Naskapi Stwa
    case 0x150E: return 'n'; // Canadian Syllabics Naskapi Skwa
    case 0x150F: return 'n'; // Canadian Syllabics Naskapi Scwa
    case 0x1510: return 's'; // Canadian Syllabics She
    case 0x1511: return 's'; // Canadian Syllabics Shi
    case 0x1512: return 's'; // Canadian Syllabics Shii
    case 0x1513: return 's'; // Canadian Syllabics Sho
    case 0x1514: return 's'; // Canadian Syllabics Shoo
    case 0x1515: return 's'; // Canadian Syllabics Sha
    case 0x1516: return 's'; // Canadian Syllabics Shaa
    case 0x1517: return 's'; // Canadian Syllabics Shwe
    case 0x1518: return 'w'; // Canadian Syllabics West-Cree Shwe
    case 0x1519: return 's'; // Canadian Syllabics Shwi
    case 0x151A: return 'w'; // Canadian Syllabics West-Cree Shwi
    case 0x151B: return 's'; // Canadian Syllabics Shwii
    case 0x151C: return 'w'; // Canadian Syllabics West-Cree Shwii
    case 0x151D: return 's'; // Canadian Syllabics Shwo
    case 0x151E: return 'w'; // Canadian Syllabics West-Cree Shwo
    case 0x151F: return 's'; // Canadian Syllabics Shwoo
    case 0x1520: return 'w'; // Canadian Syllabics West-Cree Shwoo
    case 0x1521: return 's'; // Canadian Syllabics Shwa
    case 0x1522: return 'w'; // Canadian Syllabics West-Cree Shwa
    case 0x1523: return 's'; // Canadian Syllabics Shwaa
    case 0x1524: return 'w'; // Canadian Syllabics West-Cree Shwaa
    case 0x1526: return 'y'; // Canadian Syllabics Ye
    case 0x1527: return 'y'; // Canadian Syllabics Yaai
    case 0x1528: return 'y'; // Canadian Syllabics Yi
    case 0x1529: return 'y'; // Canadian Syllabics Yii
    case 0x152A: return 'y'; // Canadian Syllabics Yo
    case 0x152B: return 'y'; // Canadian Syllabics Yoo
    case 0x152C: return 'y'; // Canadian Syllabics Y-Cree Yoo
    case 0x152E: return 'y'; // Canadian Syllabics Yaa
    case 0x152F: return 'y'; // Canadian Syllabics Ywe
    case 0x1530: return 'w'; // Canadian Syllabics West-Cree Ywe
    case 0x1531: return 'y'; // Canadian Syllabics Ywi
    case 0x1532: return 'w'; // Canadian Syllabics West-Cree Ywi
    case 0x1533: return 'y'; // Canadian Syllabics Ywii
    case 0x1534: return 'w'; // Canadian Syllabics West-Cree Ywii
    case 0x1535: return 'y'; // Canadian Syllabics Ywo
    case 0x1536: return 'w'; // Canadian Syllabics West-Cree Ywo
    case 0x1537: return 'y'; // Canadian Syllabics Ywoo
    case 0x1538: return 'w'; // Canadian Syllabics West-Cree Ywoo
    case 0x1539: return 'y'; // Canadian Syllabics Ywa
    case 0x153A: return 'w'; // Canadian Syllabics West-Cree Ywa
    case 0x153B: return 'y'; // Canadian Syllabics Ywaa
    case 0x153C: return 'w'; // Canadian Syllabics West-Cree Ywaa
    case 0x153D: return 'n'; // Canadian Syllabics Naskapi Ywaa
    case 0x153E: return 'y'; // Canadian Syllabics Y
    case 0x153F: return 'b'; // Canadian Syllabics Bible-Cree Y
    case 0x1540: return 'w'; // Canadian Syllabics West-Cree Y
    case 0x1541: return 's'; // Canadian Syllabics Sayisi Yi
    case 0x1542: return 'r'; // Canadian Syllabics Re
    case 0x1543: return 'r'; // Canadian Syllabics R-Cree Re
    case 0x1544: return 'w'; // Canadian Syllabics West-Cree Le
    case 0x1545: return 'r'; // Canadian Syllabics Raai
    case 0x1546: return 'r'; // Canadian Syllabics Ri
    case 0x1547: return 'r'; // Canadian Syllabics Rii
    case 0x1548: return 'r'; // Canadian Syllabics Ro
    case 0x1549: return 'r'; // Canadian Syllabics Roo
    case 0x154A: return 'w'; // Canadian Syllabics West-Cree Lo
    case 0x154B: return 'r'; // Canadian Syllabics Ra
    case 0x154C: return 'r'; // Canadian Syllabics Raa
    case 0x154D: return 'w'; // Canadian Syllabics West-Cree La
    case 0x154E: return 'r'; // Canadian Syllabics Rwaa
    case 0x154F: return 'w'; // Canadian Syllabics West-Cree Rwaa
    case 0x1550: return 'r'; // Canadian Syllabics R
    case 0x1551: return 'w'; // Canadian Syllabics West-Cree R
    case 0x1552: return 'm'; // Canadian Syllabics Medial R
    case 0x1553: return 'f'; // Canadian Syllabics Fe
    case 0x1554: return 'f'; // Canadian Syllabics Faai
    case 0x1555: return 'f'; // Canadian Syllabics Fi
    case 0x1556: return 'f'; // Canadian Syllabics Fii
    case 0x1557: return 'f'; // Canadian Syllabics Fo
    case 0x1558: return 'f'; // Canadian Syllabics Foo
    case 0x1559: return 'f'; // Canadian Syllabics Fa
    case 0x155A: return 'f'; // Canadian Syllabics Faa
    case 0x155B: return 'f'; // Canadian Syllabics Fwaa
    case 0x155C: return 'w'; // Canadian Syllabics West-Cree Fwaa
    case 0x155D: return 'f'; // Canadian Syllabics F
    case 0x155E: return 't'; // Canadian Syllabics The
    case 0x155F: return 'n'; // Canadian Syllabics N-Cree The
    case 0x1560: return 't'; // Canadian Syllabics Thi
    case 0x1561: return 'n'; // Canadian Syllabics N-Cree Thi
    case 0x1562: return 't'; // Canadian Syllabics Thii
    case 0x1563: return 'n'; // Canadian Syllabics N-Cree Thii
    case 0x1564: return 't'; // Canadian Syllabics Tho
    case 0x1565: return 't'; // Canadian Syllabics Thoo
    case 0x1566: return 't'; // Canadian Syllabics Tha
    case 0x1567: return 't'; // Canadian Syllabics Thaa
    case 0x1568: return 't'; // Canadian Syllabics Thwaa
    case 0x1569: return 'w'; // Canadian Syllabics West-Cree Thwaa
    case 0x156A: return 't'; // Canadian Syllabics Th
    case 0x156B: return 't'; // Canadian Syllabics Tthe
    case 0x156C: return 't'; // Canadian Syllabics Tthi
    case 0x156D: return 't'; // Canadian Syllabics Ttho
    case 0x156E: return 't'; // Canadian Syllabics Ttha
    case 0x156F: return 't'; // Canadian Syllabics Tth
    case 0x1570: return 't'; // Canadian Syllabics Tye
    case 0x1571: return 't'; // Canadian Syllabics Tyi
    case 0x1572: return 't'; // Canadian Syllabics Tyo
    case 0x1573: return 't'; // Canadian Syllabics Tya
    case 0x1574: return 'n'; // Canadian Syllabics Nunavik He
    case 0x1575: return 'n'; // Canadian Syllabics Nunavik Hi
    case 0x1576: return 'n'; // Canadian Syllabics Nunavik Hii
    case 0x1577: return 'n'; // Canadian Syllabics Nunavik Ho
    case 0x1578: return 'n'; // Canadian Syllabics Nunavik Hoo
    case 0x1579: return 'n'; // Canadian Syllabics Nunavik Ha
    case 0x157A: return 'n'; // Canadian Syllabics Nunavik Haa
    case 0x157B: return 'n'; // Canadian Syllabics Nunavik H
    case 0x157C: return 'n'; // Canadian Syllabics Nunavut H
    case 0x157D: return 'h'; // Canadian Syllabics Hk
    case 0x157E: return 'q'; // Canadian Syllabics Qaai
    case 0x157F: return 'q'; // Canadian Syllabics Qi
    case 0x1580: return 'q'; // Canadian Syllabics Qii
    case 0x1581: return 'q'; // Canadian Syllabics Qo
    case 0x1582: return 'q'; // Canadian Syllabics Qoo
    case 0x1583: return 'q'; // Canadian Syllabics Qa
    case 0x1584: return 'q'; // Canadian Syllabics Qaa
    case 0x1586: return 't'; // Canadian Syllabics Tlhe
    case 0x1587: return 't'; // Canadian Syllabics Tlhi
    case 0x1588: return 't'; // Canadian Syllabics Tlho
    case 0x1589: return 't'; // Canadian Syllabics Tlha
    case 0x158A: return 'w'; // Canadian Syllabics West-Cree Re
    case 0x158B: return 'w'; // Canadian Syllabics West-Cree Ri
    case 0x158C: return 'w'; // Canadian Syllabics West-Cree Ro
    case 0x158D: return 'w'; // Canadian Syllabics West-Cree Ra
    case 0x158E: return 'n'; // Canadian Syllabics Ngaai
    case 0x158F: return 'n'; // Canadian Syllabics Ngi
    case 0x1590: return 'n'; // Canadian Syllabics Ngii
    case 0x1591: return 'n'; // Canadian Syllabics Ngo
    case 0x1592: return 'n'; // Canadian Syllabics Ngoo
    case 0x1593: return 'n'; // Canadian Syllabics Nga
    case 0x1594: return 'n'; // Canadian Syllabics Ngaa
    case 0x1595: return 'n'; // Canadian Syllabics Ng
    case 0x1596: return 'n'; // Canadian Syllabics Nng
    case 0x1597: return 's'; // Canadian Syllabics Sayisi She
    case 0x1598: return 's'; // Canadian Syllabics Sayisi Shi
    case 0x1599: return 's'; // Canadian Syllabics Sayisi Sho
    case 0x159A: return 's'; // Canadian Syllabics Sayisi Sha
    case 0x159B: return 'w'; // Canadian Syllabics Woods-Cree The
    case 0x159C: return 'w'; // Canadian Syllabics Woods-Cree Thi
    case 0x159D: return 'w'; // Canadian Syllabics Woods-Cree Tho
    case 0x159E: return 'w'; // Canadian Syllabics Woods-Cree Tha
    case 0x159F: return 'w'; // Canadian Syllabics Woods-Cree Th
    case 0x15A0: return 'l'; // Canadian Syllabics Lhi
    case 0x15A1: return 'l'; // Canadian Syllabics Lhii
    case 0x15A2: return 'l'; // Canadian Syllabics Lho
    case 0x15A3: return 'l'; // Canadian Syllabics Lhoo
    case 0x15A4: return 'l'; // Canadian Syllabics Lha
    case 0x15A5: return 'l'; // Canadian Syllabics Lhaa
    case 0x15A6: return 'l'; // Canadian Syllabics Lh
    case 0x15A7: return 't'; // Canadian Syllabics Th-Cree The
    case 0x15A8: return 't'; // Canadian Syllabics Th-Cree Thi
    case 0x15A9: return 't'; // Canadian Syllabics Th-Cree Thii
    case 0x15AA: return 't'; // Canadian Syllabics Th-Cree Tho
    case 0x15AB: return 't'; // Canadian Syllabics Th-Cree Thoo
    case 0x15AC: return 't'; // Canadian Syllabics Th-Cree Tha
    case 0x15AD: return 't'; // Canadian Syllabics Th-Cree Thaa
    case 0x15AE: return 't'; // Canadian Syllabics Th-Cree Th
    case 0x15AF: return 'a'; // Canadian Syllabics Aivilik B
    case 0x15B0: return 'b'; // Canadian Syllabics Blackfoot E
    case 0x15B1: return 'b'; // Canadian Syllabics Blackfoot I
    case 0x15B2: return 'b'; // Canadian Syllabics Blackfoot O
    case 0x15B3: return 'b'; // Canadian Syllabics Blackfoot A
    case 0x15B4: return 'b'; // Canadian Syllabics Blackfoot We
    case 0x15B5: return 'b'; // Canadian Syllabics Blackfoot Wi
    case 0x15B6: return 'b'; // Canadian Syllabics Blackfoot Wo
    case 0x15B7: return 'b'; // Canadian Syllabics Blackfoot Wa
    case 0x15B8: return 'b'; // Canadian Syllabics Blackfoot Ne
    case 0x15B9: return 'b'; // Canadian Syllabics Blackfoot Ni
    case 0x15BA: return 'b'; // Canadian Syllabics Blackfoot No
    case 0x15BB: return 'b'; // Canadian Syllabics Blackfoot Na
    case 0x15BC: return 'b'; // Canadian Syllabics Blackfoot Ke
    case 0x15BD: return 'b'; // Canadian Syllabics Blackfoot Ki
    case 0x15BE: return 'b'; // Canadian Syllabics Blackfoot Ko
    case 0x15BF: return 'b'; // Canadian Syllabics Blackfoot Ka
    case 0x15C0: return 's'; // Canadian Syllabics Sayisi He
    case 0x15C1: return 's'; // Canadian Syllabics Sayisi Hi
    case 0x15C2: return 's'; // Canadian Syllabics Sayisi Ho
    case 0x15C3: return 's'; // Canadian Syllabics Sayisi Ha
    case 0x15C4: return 'c'; // Canadian Syllabics Carrier Ghu
    case 0x15C5: return 'c'; // Canadian Syllabics Carrier Gho
    case 0x15C6: return 'c'; // Canadian Syllabics Carrier Ghe
    case 0x15C7: return 'c'; // Canadian Syllabics Carrier Ghee
    case 0x15C8: return 'c'; // Canadian Syllabics Carrier Ghi
    case 0x15C9: return 'c'; // Canadian Syllabics Carrier Gha
    case 0x15CA: return 'c'; // Canadian Syllabics Carrier Ru
    case 0x15CB: return 'c'; // Canadian Syllabics Carrier Ro
    case 0x15CC: return 'c'; // Canadian Syllabics Carrier Re
    case 0x15CD: return 'c'; // Canadian Syllabics Carrier Ree
    case 0x15CE: return 'c'; // Canadian Syllabics Carrier Ri
    case 0x15CF: return 'c'; // Canadian Syllabics Carrier Ra
    case 0x15D0: return 'c'; // Canadian Syllabics Carrier Wu
    case 0x15D1: return 'c'; // Canadian Syllabics Carrier Wo
    case 0x15D2: return 'c'; // Canadian Syllabics Carrier We
    case 0x15D3: return 'c'; // Canadian Syllabics Carrier Wee
    case 0x15D4: return 'c'; // Canadian Syllabics Carrier Wi
    case 0x15D5: return 'c'; // Canadian Syllabics Carrier Wa
    case 0x15D6: return 'c'; // Canadian Syllabics Carrier Hwu
    case 0x15D7: return 'c'; // Canadian Syllabics Carrier Hwo
    case 0x15D8: return 'c'; // Canadian Syllabics Carrier Hwe
    case 0x15D9: return 'c'; // Canadian Syllabics Carrier Hwee
    case 0x15DA: return 'c'; // Canadian Syllabics Carrier Hwi
    case 0x15DB: return 'c'; // Canadian Syllabics Carrier Hwa
    case 0x15DC: return 'c'; // Canadian Syllabics Carrier Thu
    case 0x15DD: return 'c'; // Canadian Syllabics Carrier Tho
    case 0x15DE: return 'c'; // Canadian Syllabics Carrier The
    case 0x15DF: return 'c'; // Canadian Syllabics Carrier Thee
    case 0x15E0: return 'c'; // Canadian Syllabics Carrier Thi
    case 0x15E1: return 'c'; // Canadian Syllabics Carrier Tha
    case 0x15E2: return 'c'; // Canadian Syllabics Carrier Ttu
    case 0x15E3: return 'c'; // Canadian Syllabics Carrier Tto
    case 0x15E4: return 'c'; // Canadian Syllabics Carrier Tte
    case 0x15E5: return 'c'; // Canadian Syllabics Carrier Ttee
    case 0x15E6: return 'c'; // Canadian Syllabics Carrier Tti
    case 0x15E7: return 'c'; // Canadian Syllabics Carrier Tta
    case 0x15E8: return 'c'; // Canadian Syllabics Carrier Pu
    case 0x15E9: return 'c'; // Canadian Syllabics Carrier Po
    case 0x15EA: return 'c'; // Canadian Syllabics Carrier Pe
    case 0x15EB: return 'c'; // Canadian Syllabics Carrier Pee
    case 0x15EC: return 'c'; // Canadian Syllabics Carrier Pi
    case 0x15ED: return 'c'; // Canadian Syllabics Carrier Pa
    case 0x15EE: return 'c'; // Canadian Syllabics Carrier P
    case 0x15EF: return 'c'; // Canadian Syllabics Carrier Gu
    case 0x15F0: return 'c'; // Canadian Syllabics Carrier Go
    case 0x15F1: return 'c'; // Canadian Syllabics Carrier Ge
    case 0x15F2: return 'c'; // Canadian Syllabics Carrier Gee
    case 0x15F3: return 'c'; // Canadian Syllabics Carrier Gi
    case 0x15F4: return 'c'; // Canadian Syllabics Carrier Ga
    case 0x15F5: return 'c'; // Canadian Syllabics Carrier Khu
    case 0x15F6: return 'c'; // Canadian Syllabics Carrier Kho
    case 0x15F7: return 'c'; // Canadian Syllabics Carrier Khe
    case 0x15F8: return 'c'; // Canadian Syllabics Carrier Khee
    case 0x15F9: return 'c'; // Canadian Syllabics Carrier Khi
    case 0x15FA: return 'c'; // Canadian Syllabics Carrier Kha
    case 0x15FB: return 'c'; // Canadian Syllabics Carrier Kku
    case 0x15FC: return 'c'; // Canadian Syllabics Carrier Kko
    case 0x15FD: return 'c'; // Canadian Syllabics Carrier Kke
    case 0x15FE: return 'c'; // Canadian Syllabics Carrier Kkee
    case 0x15FF: return 'c'; // Canadian Syllabics Carrier Kki
    case 0x1600: return 'c'; // Canadian Syllabics Carrier Kka
    case 0x1601: return 'c'; // Canadian Syllabics Carrier Kk
    case 0x1602: return 'c'; // Canadian Syllabics Carrier Nu
    case 0x1603: return 'c'; // Canadian Syllabics Carrier No
    case 0x1604: return 'c'; // Canadian Syllabics Carrier Ne
    case 0x1605: return 'c'; // Canadian Syllabics Carrier Nee
    case 0x1606: return 'c'; // Canadian Syllabics Carrier Ni
    case 0x1607: return 'c'; // Canadian Syllabics Carrier Na
    case 0x1608: return 'c'; // Canadian Syllabics Carrier Mu
    case 0x1609: return 'c'; // Canadian Syllabics Carrier Mo
    case 0x160A: return 'c'; // Canadian Syllabics Carrier Me
    case 0x160B: return 'c'; // Canadian Syllabics Carrier Mee
    case 0x160C: return 'c'; // Canadian Syllabics Carrier Mi
    case 0x160D: return 'c'; // Canadian Syllabics Carrier Ma
    case 0x160E: return 'c'; // Canadian Syllabics Carrier Yu
    case 0x160F: return 'c'; // Canadian Syllabics Carrier Yo
    case 0x1610: return 'c'; // Canadian Syllabics Carrier Ye
    case 0x1611: return 'c'; // Canadian Syllabics Carrier Yee
    case 0x1612: return 'c'; // Canadian Syllabics Carrier Yi
    case 0x1613: return 'c'; // Canadian Syllabics Carrier Ya
    case 0x1614: return 'c'; // Canadian Syllabics Carrier Ju
    case 0x1615: return 's'; // Canadian Syllabics Sayisi Ju
    case 0x1616: return 'c'; // Canadian Syllabics Carrier Jo
    case 0x1617: return 'c'; // Canadian Syllabics Carrier Je
    case 0x1618: return 'c'; // Canadian Syllabics Carrier Jee
    case 0x1619: return 'c'; // Canadian Syllabics Carrier Ji
    case 0x161A: return 's'; // Canadian Syllabics Sayisi Ji
    case 0x161B: return 'c'; // Canadian Syllabics Carrier Ja
    case 0x161C: return 'c'; // Canadian Syllabics Carrier Jju
    case 0x161D: return 'c'; // Canadian Syllabics Carrier Jjo
    case 0x161E: return 'c'; // Canadian Syllabics Carrier Jje
    case 0x161F: return 'c'; // Canadian Syllabics Carrier Jjee
    case 0x1620: return 'c'; // Canadian Syllabics Carrier Jji
    case 0x1621: return 'c'; // Canadian Syllabics Carrier Jja
    case 0x1622: return 'c'; // Canadian Syllabics Carrier Lu
    case 0x1623: return 'c'; // Canadian Syllabics Carrier Lo
    case 0x1624: return 'c'; // Canadian Syllabics Carrier Le
    case 0x1625: return 'c'; // Canadian Syllabics Carrier Lee
    case 0x1626: return 'c'; // Canadian Syllabics Carrier Li
    case 0x1627: return 'c'; // Canadian Syllabics Carrier La
    case 0x1628: return 'c'; // Canadian Syllabics Carrier Dlu
    case 0x1629: return 'c'; // Canadian Syllabics Carrier Dlo
    case 0x162A: return 'c'; // Canadian Syllabics Carrier Dle
    case 0x162B: return 'c'; // Canadian Syllabics Carrier Dlee
    case 0x162C: return 'c'; // Canadian Syllabics Carrier Dli
    case 0x162D: return 'c'; // Canadian Syllabics Carrier Dla
    case 0x162E: return 'c'; // Canadian Syllabics Carrier Lhu
    case 0x162F: return 'c'; // Canadian Syllabics Carrier Lho
    case 0x1630: return 'c'; // Canadian Syllabics Carrier Lhe
    case 0x1631: return 'c'; // Canadian Syllabics Carrier Lhee
    case 0x1632: return 'c'; // Canadian Syllabics Carrier Lhi
    case 0x1633: return 'c'; // Canadian Syllabics Carrier Lha
    case 0x1634: return 'c'; // Canadian Syllabics Carrier Tlhu
    case 0x1635: return 'c'; // Canadian Syllabics Carrier Tlho
    case 0x1636: return 'c'; // Canadian Syllabics Carrier Tlhe
    case 0x1637: return 'c'; // Canadian Syllabics Carrier Tlhee
    case 0x1638: return 'c'; // Canadian Syllabics Carrier Tlhi
    case 0x1639: return 'c'; // Canadian Syllabics Carrier Tlha
    case 0x163A: return 'c'; // Canadian Syllabics Carrier Tlu
    case 0x163B: return 'c'; // Canadian Syllabics Carrier Tlo
    case 0x163C: return 'c'; // Canadian Syllabics Carrier Tle
    case 0x163D: return 'c'; // Canadian Syllabics Carrier Tlee
    case 0x163E: return 'c'; // Canadian Syllabics Carrier Tli
    case 0x163F: return 'c'; // Canadian Syllabics Carrier Tla
    case 0x1640: return 'c'; // Canadian Syllabics Carrier Zu
    case 0x1641: return 'c'; // Canadian Syllabics Carrier Zo
    case 0x1642: return 'c'; // Canadian Syllabics Carrier Ze
    case 0x1643: return 'c'; // Canadian Syllabics Carrier Zee
    case 0x1644: return 'c'; // Canadian Syllabics Carrier Zi
    case 0x1645: return 'c'; // Canadian Syllabics Carrier Za
    case 0x1646: return 'c'; // Canadian Syllabics Carrier Z
    case 0x1647: return 'c'; // Canadian Syllabics Carrier Initial Z
    case 0x1648: return 'c'; // Canadian Syllabics Carrier Dzu
    case 0x1649: return 'c'; // Canadian Syllabics Carrier Dzo
    case 0x164A: return 'c'; // Canadian Syllabics Carrier Dze
    case 0x164B: return 'c'; // Canadian Syllabics Carrier Dzee
    case 0x164C: return 'c'; // Canadian Syllabics Carrier Dzi
    case 0x164D: return 'c'; // Canadian Syllabics Carrier Dza
    case 0x164E: return 'c'; // Canadian Syllabics Carrier Su
    case 0x164F: return 'c'; // Canadian Syllabics Carrier So
    case 0x1650: return 'c'; // Canadian Syllabics Carrier Se
    case 0x1651: return 'c'; // Canadian Syllabics Carrier See
    case 0x1652: return 'c'; // Canadian Syllabics Carrier Si
    case 0x1653: return 'c'; // Canadian Syllabics Carrier Sa
    case 0x1654: return 'c'; // Canadian Syllabics Carrier Shu
    case 0x1655: return 'c'; // Canadian Syllabics Carrier Sho
    case 0x1656: return 'c'; // Canadian Syllabics Carrier She
    case 0x1657: return 'c'; // Canadian Syllabics Carrier Shee
    case 0x1658: return 'c'; // Canadian Syllabics Carrier Shi
    case 0x1659: return 'c'; // Canadian Syllabics Carrier Sha
    case 0x165A: return 'c'; // Canadian Syllabics Carrier Sh
    case 0x165B: return 'c'; // Canadian Syllabics Carrier Tsu
    case 0x165C: return 'c'; // Canadian Syllabics Carrier Tso
    case 0x165D: return 'c'; // Canadian Syllabics Carrier Tse
    case 0x165E: return 'c'; // Canadian Syllabics Carrier Tsee
    case 0x165F: return 'c'; // Canadian Syllabics Carrier Tsi
    case 0x1660: return 'c'; // Canadian Syllabics Carrier Tsa
    case 0x1661: return 'c'; // Canadian Syllabics Carrier Chu
    case 0x1662: return 'c'; // Canadian Syllabics Carrier Cho
    case 0x1663: return 'c'; // Canadian Syllabics Carrier Che
    case 0x1664: return 'c'; // Canadian Syllabics Carrier Chee
    case 0x1665: return 'c'; // Canadian Syllabics Carrier Chi
    case 0x1666: return 'c'; // Canadian Syllabics Carrier Cha
    case 0x1667: return 'c'; // Canadian Syllabics Carrier Ttsu
    case 0x1668: return 'c'; // Canadian Syllabics Carrier Ttso
    case 0x1669: return 'c'; // Canadian Syllabics Carrier Ttse
    case 0x166A: return 'c'; // Canadian Syllabics Carrier Ttsee
    case 0x166B: return 'c'; // Canadian Syllabics Carrier Ttsi
    case 0x166C: return 'c'; // Canadian Syllabics Carrier Ttsa
    case 0x166D: return 'c'; // Canadian Syllabics Chi Sign
    case 0x166E: return 'f'; // Canadian Syllabics Full Stop
    case 0x166F: return 'q'; // Canadian Syllabics Qai
    case 0x1670: return 'n'; // Canadian Syllabics Ngai
    case 0x1671: return 'n'; // Canadian Syllabics Nngi
    case 0x1672: return 'n'; // Canadian Syllabics Nngii
    case 0x1673: return 'n'; // Canadian Syllabics Nngo
    case 0x1674: return 'n'; // Canadian Syllabics Nngoo
    case 0x1675: return 'n'; // Canadian Syllabics Nnga
    case 0x1676: return 'n'; // Canadian Syllabics Nngaa
    case 0x1677: return 'w'; // Canadian Syllabics Woods-Cree Thwee
    case 0x1678: return 'w'; // Canadian Syllabics Woods-Cree Thwi
    case 0x1679: return 'w'; // Canadian Syllabics Woods-Cree Thwii
    case 0x167A: return 'w'; // Canadian Syllabics Woods-Cree Thwo
    case 0x167B: return 'w'; // Canadian Syllabics Woods-Cree Thwoo
    case 0x167C: return 'w'; // Canadian Syllabics Woods-Cree Thwa
    case 0x167D: return 'w'; // Canadian Syllabics Woods-Cree Thwaa
    case 0x167E: return 'w'; // Canadian Syllabics Woods-Cree Final Th
    case 0x167F: return 'b'; // Canadian Syllabics Blackfoot W
    case 0x1780: return 'k'; // Khmer Letter Ka
    case 0x1782: return 'k'; // Khmer Letter Ko
    case 0x1783: return 'k'; // Khmer Letter Kho
    case 0x1784: return 'n'; // Khmer Letter Ngo
    case 0x1785: return 'c'; // Khmer Letter Ca
    case 0x1786: return 'c'; // Khmer Letter Cha
    case 0x1787: return 'c'; // Khmer Letter Co
    case 0x1788: return 'c'; // Khmer Letter Cho
    case 0x1789: return 'n'; // Khmer Letter Nyo
    case 0x178B: return 't'; // Khmer Letter Ttha
    case 0x178C: return 'd'; // Khmer Letter Do
    case 0x178D: return 't'; // Khmer Letter Ttho
    case 0x178E: return 'n'; // Khmer Letter Nno
    case 0x178F: return 't'; // Khmer Letter Ta
    case 0x1790: return 't'; // Khmer Letter Tha
    case 0x1792: return 't'; // Khmer Letter Tho
    case 0x1793: return 'n'; // Khmer Letter No
    case 0x1794: return 'b'; // Khmer Letter Ba
    case 0x1795: return 'p'; // Khmer Letter Pha
    case 0x1799: return 'y'; // Khmer Letter Yo
    case 0x179B: return 'l'; // Khmer Letter Lo
    case 0x179C: return 'v'; // Khmer Letter Vo
    case 0x179D: return 's'; // Khmer Letter Sha
    case 0x179E: return 's'; // Khmer Letter Sso
    case 0x17A0: return 'h'; // Khmer Letter Ha
    case 0x17A1: return 'l'; // Khmer Letter La
    case 0x17A2: return 'q'; // Khmer Letter Qa
    case 0x17A3: return 'q'; // Khmer Independent Vowel Qaq
    case 0x17A4: return 'q'; // Khmer Independent Vowel Qaa
    case 0x17A5: return 'q'; // Khmer Independent Vowel Qi
    case 0x17A6: return 'q'; // Khmer Independent Vowel Qii
    case 0x17A7: return 'q'; // Khmer Independent Vowel Qu
    case 0x17A8: return 'q'; // Khmer Independent Vowel Quk
    case 0x17A9: return 'q'; // Khmer Independent Vowel Quu
    case 0x17AA: return 'q'; // Khmer Independent Vowel Quuv
    case 0x17AB: return 'r'; // Khmer Independent Vowel Ry
    case 0x17AC: return 'r'; // Khmer Independent Vowel Ryy
    case 0x17AD: return 'l'; // Khmer Independent Vowel Ly
    case 0x17AE: return 'l'; // Khmer Independent Vowel Lyy
    case 0x17AF: return 'q'; // Khmer Independent Vowel Qe
    case 0x17B0: return 'q'; // Khmer Independent Vowel Qai
    case 0x17B1: return 'q'; // Khmer Independent Vowel Qoo Type One
    case 0x17B2: return 'q'; // Khmer Independent Vowel Qoo Type Two
    case 0x17B3: return 'q'; // Khmer Independent Vowel Qau
    case 0x17B4: return 'i'; // Khmer Vowel Inherent Aq
    case 0x17B5: return 'i'; // Khmer Vowel Inherent Aa
    case 0x17B7: return 'i'; // Khmer Vowel Sign I
    case 0x17B8: return 'i'; // Khmer Vowel Sign Ii
    case 0x17B9: return 'y'; // Khmer Vowel Sign Y
    case 0x17BA: return 'y'; // Khmer Vowel Sign Yy
    case 0x17BB: return 'u'; // Khmer Vowel Sign U
    case 0x17BC: return 'u'; // Khmer Vowel Sign Uu
    case 0x17BD: return 'u'; // Khmer Vowel Sign Ua
    case 0x17BF: return 'y'; // Khmer Vowel Sign Ya
    case 0x17C0: return 'i'; // Khmer Vowel Sign Ie
    case 0x17C1: return 'e'; // Khmer Vowel Sign E
    case 0x17C3: return 'a'; // Khmer Vowel Sign Ai
    case 0x17C4: return 'o'; // Khmer Vowel Sign Oo
    case 0x17C5: return 'a'; // Khmer Vowel Sign Au
    case 0x17D1: return 0; // Khmer Sign Viriam
    case 0x17D3: return ':'; // Khmer Sign Bathamasat
    case 0x17D4: return 0; // Khmer Sign Khan
    case 0x17D5: return 0; // Khmer Sign Bariyoosan
    case 0x17D6: return 0; // Khmer Sign Camnuc Pii Kuuh
    case 0x17D7: return '.'; // Khmer Sign Lek Too
    case 0x17D8: return '|'; // Khmer Sign Beyyal
    case 0x17D9: return '|'; // Khmer Sign Phnaek Muan
    case 0x17DB: return 'R'; // Khmer Currency Symbol Riel
    case 0x17E0: return '0'; // Khmer Digit Zero
    case 0x17E1: return '1'; // Khmer Digit One
    case 0x17E2: return '2'; // Khmer Digit Two
    case 0x17E3: return '3'; // Khmer Digit Three
    case 0x17E4: return '4'; // Khmer Digit Four
    case 0x17E5: return '5'; // Khmer Digit Five
    case 0x17E6: return '6'; // Khmer Digit Six
    case 0x17E7: return '7'; // Khmer Digit Seven
    case 0x17E8: return '8'; // Khmer Digit Eight
    case 0x17E9: return '9'; // Khmer Digit Nine
    case 0x17F0: return '0'; // Khmer Symbol Lek Attak Son
    case 0x17F1: return '1'; // Khmer Symbol Lek Attak Muoy
    case 0x17F2: return '2'; // Khmer Symbol Lek Attak Pii
    case 0x17F3: return '3'; // Khmer Symbol Lek Attak Bei
    case 0x17F4: return '4'; // Khmer Symbol Lek Attak Buon
    case 0x17F5: return '5'; // Khmer Symbol Lek Attak Pram
    case 0x17F6: return '6'; // Khmer Symbol Lek Attak Pram-Muoy
    case 0x17F7: return '7'; // Khmer Symbol Lek Attak Pram-Pii
    case 0x17F8: return '8'; // Khmer Symbol Lek Attak Pram-Bei
    case 0x17F9: return '9'; // Khmer Symbol Lek Attak Pram-Buon
    case 0x1800: return '|'; // Mongolian Birga
    case 0x1801: return '|'; // Mongolian Ellipsis
    case 0x1802: return '|'; // Mongolian Comma
    case 0x1803: return '|'; // Mongolian Full Stop
    case 0x1804: return '|'; // Mongolian Colon
    case 0x1805: return '|'; // Mongolian Four Dots
    case 0x183F: return 'z'; // Mongolian Letter Zra
    case 0x1840: return 'l'; // Mongolian Letter Lha
    case 0x1841: return 'z'; // Mongolian Letter Zhi
    case 0x1842: return 'c'; // Mongolian Letter Chi
    case 0x1843: return 0; // Mongolian Letter Todo Long Vowel Sign
    case 0x1844: return 'e'; // Mongolian Letter Todo E
    case 0x1845: return 'i'; // Mongolian Letter Todo I
    case 0x1846: return 'o'; // Mongolian Letter Todo O
    case 0x1847: return 'u'; // Mongolian Letter Todo U
    case 0x1848: return 'o'; // Mongolian Letter Todo Oe
    case 0x1849: return 'u'; // Mongolian Letter Todo Ue
    case 0x184A: return 'a'; // Mongolian Letter Todo Ang
    case 0x184B: return 'b'; // Mongolian Letter Todo Ba
    case 0x184C: return 'p'; // Mongolian Letter Todo Pa
    case 0x184D: return 'q'; // Mongolian Letter Todo Qa
    case 0x184E: return 'g'; // Mongolian Letter Todo Ga
    case 0x184F: return 'm'; // Mongolian Letter Todo Ma
    case 0x1850: return 't'; // Mongolian Letter Todo Ta
    case 0x1851: return 'd'; // Mongolian Letter Todo Da
    case 0x1852: return 'c'; // Mongolian Letter Todo Cha
    case 0x1853: return 'j'; // Mongolian Letter Todo Ja
    case 0x1854: return 't'; // Mongolian Letter Todo Tsa
    case 0x1855: return 'y'; // Mongolian Letter Todo Ya
    case 0x1856: return 'w'; // Mongolian Letter Todo Wa
    case 0x1857: return 'k'; // Mongolian Letter Todo Ka
    case 0x1858: return 'g'; // Mongolian Letter Todo Gaa
    case 0x1859: return 'h'; // Mongolian Letter Todo Haa
    case 0x185A: return 'j'; // Mongolian Letter Todo Jia
    case 0x185B: return 'n'; // Mongolian Letter Todo Nia
    case 0x185C: return 'd'; // Mongolian Letter Todo Dza
    case 0x185D: return 'e'; // Mongolian Letter Sibe E
    case 0x185E: return 'i'; // Mongolian Letter Sibe I
    case 0x185F: return 'i'; // Mongolian Letter Sibe Iy
    case 0x1860: return 'u'; // Mongolian Letter Sibe Ue
    case 0x1861: return 'u'; // Mongolian Letter Sibe U
    case 0x1862: return 'a'; // Mongolian Letter Sibe Ang
    case 0x1863: return 'k'; // Mongolian Letter Sibe Ka
    case 0x1864: return 'g'; // Mongolian Letter Sibe Ga
    case 0x1865: return 'h'; // Mongolian Letter Sibe Ha
    case 0x1866: return 'p'; // Mongolian Letter Sibe Pa
    case 0x1867: return 's'; // Mongolian Letter Sibe Sha
    case 0x1868: return 't'; // Mongolian Letter Sibe Ta
    case 0x1869: return 'd'; // Mongolian Letter Sibe Da
    case 0x186A: return 'j'; // Mongolian Letter Sibe Ja
    case 0x186B: return 'f'; // Mongolian Letter Sibe Fa
    case 0x186C: return 'g'; // Mongolian Letter Sibe Gaa
    case 0x186D: return 'h'; // Mongolian Letter Sibe Haa
    case 0x186E: return 't'; // Mongolian Letter Sibe Tsa
    case 0x186F: return 'z'; // Mongolian Letter Sibe Za
    case 0x1870: return 'r'; // Mongolian Letter Sibe Raa
    case 0x18B0: return 'o'; // Canadian Syllabics Oy
    case 0x18B1: return 'a'; // Canadian Syllabics Ay
    case 0x18B2: return 'a'; // Canadian Syllabics Aay
    case 0x18B3: return 'w'; // Canadian Syllabics Way
    case 0x18B4: return 'p'; // Canadian Syllabics Poy
    case 0x18B5: return 'p'; // Canadian Syllabics Pay
    case 0x18B6: return 'p'; // Canadian Syllabics Pwoy
    case 0x18B7: return 't'; // Canadian Syllabics Tay
    case 0x18B8: return 'k'; // Canadian Syllabics Kay
    case 0x18B9: return 'k'; // Canadian Syllabics Kway
    case 0x18BA: return 'm'; // Canadian Syllabics May
    case 0x18BB: return 'n'; // Canadian Syllabics Noy
    case 0x18BC: return 'n'; // Canadian Syllabics Nay
    case 0x18BD: return 'l'; // Canadian Syllabics Lay
    case 0x18BE: return 's'; // Canadian Syllabics Soy
    case 0x18BF: return 's'; // Canadian Syllabics Say
    case 0x18C0: return 's'; // Canadian Syllabics Shoy
    case 0x18C1: return 's'; // Canadian Syllabics Shay
    case 0x18C2: return 's'; // Canadian Syllabics Shwoy
    case 0x18C3: return 'y'; // Canadian Syllabics Yoy
    case 0x18C4: return 'y'; // Canadian Syllabics Yay
    case 0x18C5: return 'r'; // Canadian Syllabics Ray
    case 0x18C6: return 'n'; // Canadian Syllabics Nwi
    case 0x18C7: return 'o'; // Canadian Syllabics Ojibway Nwi
    case 0x18C8: return 'n'; // Canadian Syllabics Nwii
    case 0x18C9: return 'o'; // Canadian Syllabics Ojibway Nwii
    case 0x18CA: return 'n'; // Canadian Syllabics Nwo
    case 0x18CB: return 'o'; // Canadian Syllabics Ojibway Nwo
    case 0x18CC: return 'n'; // Canadian Syllabics Nwoo
    case 0x18CD: return 'o'; // Canadian Syllabics Ojibway Nwoo
    case 0x18CE: return 'r'; // Canadian Syllabics Rwee
    case 0x18CF: return 'r'; // Canadian Syllabics Rwi
    case 0x18D0: return 'r'; // Canadian Syllabics Rwii
    case 0x18D1: return 'r'; // Canadian Syllabics Rwo
    case 0x18D2: return 'r'; // Canadian Syllabics Rwoo
    case 0x18D3: return 'r'; // Canadian Syllabics Rwa
    case 0x18D4: return 'o'; // Canadian Syllabics Ojibway P
    case 0x18D5: return 'o'; // Canadian Syllabics Ojibway T
    case 0x18D6: return 'o'; // Canadian Syllabics Ojibway K
    case 0x18D7: return 'o'; // Canadian Syllabics Ojibway C
    case 0x18D8: return 'o'; // Canadian Syllabics Ojibway M
    case 0x18D9: return 'o'; // Canadian Syllabics Ojibway N
    case 0x18DA: return 'o'; // Canadian Syllabics Ojibway S
    case 0x18DB: return 'o'; // Canadian Syllabics Ojibway Sh
    case 0x18DC: return 'e'; // Canadian Syllabics Eastern W
    case 0x18DD: return 'w'; // Canadian Syllabics Western W
    case 0x18DE: return 0; // Canadian Syllabics Final Small Ring
    case 0x18DF: return 0; // Canadian Syllabics Final Raised Dot
    case 0x18E0: return 'r'; // Canadian Syllabics R-Cree Rwe
    case 0x18E1: return 'w'; // Canadian Syllabics West-Cree Loo
    case 0x18E2: return 'w'; // Canadian Syllabics West-Cree Laa
    case 0x18E3: return 't'; // Canadian Syllabics Thwe
    case 0x18E4: return 't'; // Canadian Syllabics Thwa
    case 0x18E5: return 't'; // Canadian Syllabics Tthwe
    case 0x18E6: return 't'; // Canadian Syllabics Tthoo
    case 0x18E7: return 't'; // Canadian Syllabics Tthaa
    case 0x18E8: return 't'; // Canadian Syllabics Tlhwe
    case 0x18E9: return 't'; // Canadian Syllabics Tlhoo
    case 0x18EA: return 's'; // Canadian Syllabics Sayisi Shwe
    case 0x18EB: return 's'; // Canadian Syllabics Sayisi Shoo
    case 0x18EC: return 's'; // Canadian Syllabics Sayisi Hoo
    case 0x18ED: return 'c'; // Canadian Syllabics Carrier Gwu
    case 0x18EE: return 'c'; // Canadian Syllabics Carrier Dene Gee
    case 0x18EF: return 'c'; // Canadian Syllabics Carrier Gaa
    case 0x18F0: return 'c'; // Canadian Syllabics Carrier Gwa
    case 0x18F1: return 's'; // Canadian Syllabics Sayisi Juu
    case 0x18F2: return 'c'; // Canadian Syllabics Carrier Jwa
    case 0x18F3: return 'b'; // Canadian Syllabics Beaver Dene L
    case 0x18F4: return 'b'; // Canadian Syllabics Beaver Dene R
    case 0x18F5: return 'c'; // Canadian Syllabics Carrier Dental S
    case 0x1950: return 'k'; // Tai Le Letter Ka
    case 0x1953: return 't'; // Tai Le Letter Tsa
    case 0x1954: return 's'; // Tai Le Letter Sa
    case 0x1955: return 'y'; // Tai Le Letter Ya
    case 0x1957: return 't'; // Tai Le Letter Tha
    case 0x1959: return 'p'; // Tai Le Letter Pa
    case 0x195A: return 'p'; // Tai Le Letter Pha
    case 0x195B: return 'm'; // Tai Le Letter Ma
    case 0x195C: return 'f'; // Tai Le Letter Fa
    case 0x195F: return 'q'; // Tai Le Letter Qa
    case 0x1960: return 'k'; // Tai Le Letter Kha
    case 0x1961: return 't'; // Tai Le Letter Tsha
    case 0x1962: return 'n'; // Tai Le Letter Na
    case 0x1964: return 'i'; // Tai Le Letter I
    case 0x1966: return 'e'; // Tai Le Letter Eh
    case 0x1967: return 'u'; // Tai Le Letter U
    case 0x1969: return 'o'; // Tai Le Letter O
    case 0x196A: return 'u'; // Tai Le Letter Ue
    case 0x196B: return 'e'; // Tai Le Letter E
    case 0x1971: return 't'; // Tai Le Letter Tone-3
    case 0x1C50: return '0'; // Ol Chiki Digit Zero
    case 0x1C51: return '1'; // Ol Chiki Digit One
    case 0x1C52: return '2'; // Ol Chiki Digit Two
    case 0x1C53: return '3'; // Ol Chiki Digit Three
    case 0x1C54: return '4'; // Ol Chiki Digit Four
    case 0x1C55: return '5'; // Ol Chiki Digit Five
    case 0x1C56: return '6'; // Ol Chiki Digit Six
    case 0x1C57: return '7'; // Ol Chiki Digit Seven
    case 0x1C58: return '8'; // Ol Chiki Digit Eight
    case 0x1C59: return '9'; // Ol Chiki Digit Nine
    case 0x1C5A: return 'l'; // Ol Chiki Letter La
    case 0x1C5C: return 'a'; // Ol Chiki Letter Ag
    case 0x1C5D: return 'a'; // Ol Chiki Letter Ang
    case 0x1C5E: return 'a'; // Ol Chiki Letter Al
    case 0x1C60: return 'a'; // Ol Chiki Letter Aak
    case 0x1C61: return 'a'; // Ol Chiki Letter Aaj
    case 0x1C63: return 'a'; // Ol Chiki Letter Aaw
    case 0x1C67: return 'i'; // Ol Chiki Letter Iny
    case 0x1C68: return 'i'; // Ol Chiki Letter Ir
    case 0x1C6A: return 'u'; // Ol Chiki Letter Uc
    case 0x1C6B: return 'u'; // Ol Chiki Letter Ud
    case 0x1C6D: return 'u'; // Ol Chiki Letter Uy
    case 0x1C6E: return 'l'; // Ol Chiki Letter Le
    case 0x1C6F: return 'e'; // Ol Chiki Letter Ep
    case 0x1C70: return 'e'; // Ol Chiki Letter Edd
    case 0x1C73: return 'l'; // Ol Chiki Letter Lo
    case 0x1C75: return 'o'; // Ol Chiki Letter Ob
    case 0x1C76: return 'o'; // Ol Chiki Letter Ov
    case 0x1C77: return 'o'; // Ol Chiki Letter Oh
    case 0x1E00: return 'A'; // Latin Capital Letter A With Ring Below
    case 0x1E01: return 'a'; // Latin Small Letter A With Ring Below
    case 0x1E02: return 'B'; // Latin Capital Letter B With Dot Above
    case 0x1E03: return 'b'; // Latin Small Letter B With Dot Above
    case 0x1E04: return 'B'; // Latin Capital Letter B With Dot Below
    case 0x1E05: return 'b'; // Latin Small Letter B With Dot Below
    case 0x1E06: return 'B'; // Latin Capital Letter B With Line Below
    case 0x1E07: return 'b'; // Latin Small Letter B With Line Below
    case 0x1E08: return 'C'; // Latin Capital Letter C With Cedilla And Acute
    case 0x1E09: return 'c'; // Latin Small Letter C With Cedilla And Acute
    case 0x1E0A: return 'D'; // Latin Capital Letter D With Dot Above
    case 0x1E0B: return 'd'; // Latin Small Letter D With Dot Above
    case 0x1E0C: return 'D'; // Latin Capital Letter D With Dot Below
    case 0x1E0D: return 'd'; // Latin Small Letter D With Dot Below
    case 0x1E0E: return 'D'; // Latin Capital Letter D With Line Below
    case 0x1E0F: return 'd'; // Latin Small Letter D With Line Below
    case 0x1E10: return 'D'; // Latin Capital Letter D With Cedilla
    case 0x1E11: return 'd'; // Latin Small Letter D With Cedilla
    case 0x1E12: return 'D'; // Latin Capital Letter D With Circumflex Below
    case 0x1E14: return 'E'; // Latin Capital Letter E With Macron And Grave
    case 0x1E15: return 'e'; // Latin Small Letter E With Macron And Grave
    case 0x1E16: return 'E'; // Latin Capital Letter E With Macron And Acute
    case 0x1E17: return 'e'; // Latin Small Letter E With Macron And Acute
    case 0x1E18: return 'E'; // Latin Capital Letter E With Circumflex Below
    case 0x1E19: return 'e'; // Latin Small Letter E With Circumflex Below
    case 0x1E1A: return 'E'; // Latin Capital Letter E With Tilde Below
    case 0x1E1B: return 'e'; // Latin Small Letter E With Tilde Below
    case 0x1E1C: return 'E'; // Latin Capital Letter E With Cedilla And Breve
    case 0x1E1D: return 'e'; // Latin Small Letter E With Cedilla And Breve
    case 0x1E1E: return 'F'; // Latin Capital Letter F With Dot Above
    case 0x1E1F: return 'f'; // Latin Small Letter F With Dot Above
    case 0x1E20: return 'G'; // Latin Capital Letter G With Macron
    case 0x1E21: return 'g'; // Latin Small Letter G With Macron
    case 0x1E22: return 'H'; // Latin Capital Letter H With Dot Above
    case 0x1E23: return 'h'; // Latin Small Letter H With Dot Above
    case 0x1E24: return 'H'; // Latin Capital Letter H With Dot Below
    case 0x1E26: return 'H'; // Latin Capital Letter H With Diaeresis
    case 0x1E27: return 'h'; // Latin Small Letter H With Diaeresis
    case 0x1E28: return 'H'; // Latin Capital Letter H With Cedilla
    case 0x1E29: return 'h'; // Latin Small Letter H With Cedilla
    case 0x1E2A: return 'H'; // Latin Capital Letter H With Breve Below
    case 0x1E2B: return 'h'; // Latin Small Letter H With Breve Below
    case 0x1E2C: return 'I'; // Latin Capital Letter I With Tilde Below
    case 0x1E2D: return 'i'; // Latin Small Letter I With Tilde Below
    case 0x1E2E: return 'I'; // Latin Capital Letter I With Diaeresis And Acute
    case 0x1E2F: return 'i'; // Latin Small Letter I With Diaeresis And Acute
    case 0x1E30: return 'K'; // Latin Capital Letter K With Acute
    case 0x1E31: return 'k'; // Latin Small Letter K With Acute
    case 0x1E32: return 'K'; // Latin Capital Letter K With Dot Below
    case 0x1E33: return 'k'; // Latin Small Letter K With Dot Below
    case 0x1E34: return 'K'; // Latin Capital Letter K With Line Below
    case 0x1E35: return 'k'; // Latin Small Letter K With Line Below
    case 0x1E36: return 'L'; // Latin Capital Letter L With Dot Below
    case 0x1E38: return 'L'; // Latin Capital Letter L With Dot Below And Macron
    case 0x1E39: return 'l'; // Latin Small Letter L With Dot Below And Macron
    case 0x1E3A: return 'L'; // Latin Capital Letter L With Line Below
    case 0x1E3C: return 'L'; // Latin Capital Letter L With Circumflex Below
    case 0x1E3D: return 'l'; // Latin Small Letter L With Circumflex Below
    case 0x1E3E: return 'M'; // Latin Capital Letter M With Acute
    case 0x1E3F: return 'm'; // Latin Small Letter M With Acute
    case 0x1E40: return 'M'; // Latin Capital Letter M With Dot Above
    case 0x1E41: return 'm'; // Latin Small Letter M With Dot Above
    case 0x1E42: return 'M'; // Latin Capital Letter M With Dot Below
    case 0x1E44: return 'N'; // Latin Capital Letter N With Dot Above
    case 0x1E45: return 'n'; // Latin Small Letter N With Dot Above
    case 0x1E46: return 'N'; // Latin Capital Letter N With Dot Below
    case 0x1E47: return 'n'; // Latin Small Letter N With Dot Below
    case 0x1E48: return 'N'; // Latin Capital Letter N With Line Below
    case 0x1E49: return 'n'; // Latin Small Letter N With Line Below
    case 0x1E4A: return 'N'; // Latin Capital Letter N With Circumflex Below
    case 0x1E4B: return 'n'; // Latin Small Letter N With Circumflex Below
    case 0x1E4C: return 'O'; // Latin Capital Letter O With Tilde And Acute
    case 0x1E4D: return 'o'; // Latin Small Letter O With Tilde And Acute
    case 0x1E4E: return 'O'; // Latin Capital Letter O With Tilde And Diaeresis
    case 0x1E4F: return 'o'; // Latin Small Letter O With Tilde And Diaeresis
    case 0x1E50: return 'O'; // Latin Capital Letter O With Macron And Grave
    case 0x1E51: return 'o'; // Latin Small Letter O With Macron And Grave
    case 0x1E52: return 'O'; // Latin Capital Letter O With Macron And Acute
    case 0x1E53: return 'o'; // Latin Small Letter O With Macron And Acute
    case 0x1E54: return 'P'; // Latin Capital Letter P With Acute
    case 0x1E55: return 'p'; // Latin Small Letter P With Acute
    case 0x1E56: return 'P'; // Latin Capital Letter P With Dot Above
    case 0x1E58: return 'R'; // Latin Capital Letter R With Dot Above
    case 0x1E59: return 'r'; // Latin Small Letter R With Dot Above
    case 0x1E5A: return 'R'; // Latin Capital Letter R With Dot Below
    case 0x1E5C: return 'R'; // Latin Capital Letter R With Dot Below And Macron
    case 0x1E5D: return 'r'; // Latin Small Letter R With Dot Below And Macron
    case 0x1E5E: return 'R'; // Latin Capital Letter R With Line Below
    case 0x1E5F: return 'r'; // Latin Small Letter R With Line Below
    case 0x1E60: return 'S'; // Latin Capital Letter S With Dot Above
    case 0x1E61: return 's'; // Latin Small Letter S With Dot Above
    case 0x1E62: return 'S'; // Latin Capital Letter S With Dot Below
    case 0x1E64: return 'S'; // Latin Capital Letter S With Acute And Dot Above
    case 0x1E65: return 's'; // Latin Small Letter S With Acute And Dot Above
    case 0x1E66: return 'S'; // Latin Capital Letter S With Caron And Dot Above
    case 0x1E67: return 's'; // Latin Small Letter S With Caron And Dot Above
    case 0x1E68: return 'S'; // Latin Capital Letter S With Dot Below And Dot Above
    case 0x1E69: return 's'; // Latin Small Letter S With Dot Below And Dot Above
    case 0x1E6A: return 'T'; // Latin Capital Letter T With Dot Above
    case 0x1E6B: return 't'; // Latin Small Letter T With Dot Above
    case 0x1E6C: return 'T'; // Latin Capital Letter T With Dot Below
    case 0x1E6E: return 'T'; // Latin Capital Letter T With Line Below
    case 0x1E6F: return 't'; // Latin Small Letter T With Line Below
    case 0x1E70: return 'T'; // Latin Capital Letter T With Circumflex Below
    case 0x1E71: return 't'; // Latin Small Letter T With Circumflex Below
    case 0x1E72: return 'U'; // Latin Capital Letter U With Diaeresis Below
    case 0x1E74: return 'U'; // Latin Capital Letter U With Tilde Below
    case 0x1E75: return 'u'; // Latin Small Letter U With Tilde Below
    case 0x1E76: return 'U'; // Latin Capital Letter U With Circumflex Below
    case 0x1E77: return 'u'; // Latin Small Letter U With Circumflex Below
    case 0x1E78: return 'U'; // Latin Capital Letter U With Tilde And Acute
    case 0x1E79: return 'u'; // Latin Small Letter U With Tilde And Acute
    case 0x1E7A: return 'U'; // Latin Capital Letter U With Macron And Diaeresis
    case 0x1E7B: return 'u'; // Latin Small Letter U With Macron And Diaeresis
    case 0x1E7C: return 'V'; // Latin Capital Letter V With Tilde
    case 0x1E7D: return 'v'; // Latin Small Letter V With Tilde
    case 0x1E7E: return 'V'; // Latin Capital Letter V With Dot Below
    case 0x1E7F: return 'v'; // Latin Small Letter V With Dot Below
    case 0x1E80: return 'W'; // Latin Capital Letter W With Grave
    case 0x1E81: return 'w'; // Latin Small Letter W With Grave
    case 0x1E82: return 'W'; // Latin Capital Letter W With Acute
    case 0x1E83: return 'w'; // Latin Small Letter W With Acute
    case 0x1E84: return 'W'; // Latin Capital Letter W With Diaeresis
    case 0x1E85: return 'w'; // Latin Small Letter W With Diaeresis
    case 0x1E86: return 'W'; // Latin Capital Letter W With Dot Above
    case 0x1E87: return 'w'; // Latin Small Letter W With Dot Above
    case 0x1E88: return 'W'; // Latin Capital Letter W With Dot Below
    case 0x1E89: return 'w'; // Latin Small Letter W With Dot Below
    case 0x1E8A: return 'X'; // Latin Capital Letter X With Dot Above
    case 0x1E8B: return 'x'; // Latin Small Letter X With Dot Above
    case 0x1E8C: return 'X'; // Latin Capital Letter X With Diaeresis
    case 0x1E8D: return 'x'; // Latin Small Letter X With Diaeresis
    case 0x1E8E: return 'Y'; // Latin Capital Letter Y With Dot Above
    case 0x1E8F: return 'y'; // Latin Small Letter Y With Dot Above
    case 0x1E90: return 'Z'; // Latin Capital Letter Z With Circumflex
    case 0x1E91: return 'z'; // Latin Small Letter Z With Circumflex
    case 0x1E92: return 'Z'; // Latin Capital Letter Z With Dot Below
    case 0x1E93: return 'z'; // Latin Small Letter Z With Dot Below
    case 0x1E94: return 'Z'; // Latin Capital Letter Z With Line Below
    case 0x1E95: return 'z'; // Latin Small Letter Z With Line Below
    case 0x1E96: return 'h'; // Latin Small Letter H With Line Below
    case 0x1E97: return 't'; // Latin Small Letter T With Diaeresis
    case 0x1E98: return 'w'; // Latin Small Letter W With Ring Above
    case 0x1E99: return 'y'; // Latin Small Letter Y With Ring Above
    case 0x1E9A: return 'a'; // Latin Small Letter A With Right Half Ring
    case 0x1E9B: return 'l'; // Latin Small Letter Long S With Dot Above
    case 0x1E9C: return 'l'; // Latin Small Letter Long S With Diagonal Stroke
    case 0x1E9D: return 'l'; // Latin Small Letter Long S With High Stroke
    case 0x1E9E: return 'S'; // Latin Capital Letter Sharp S
    case 0x1E9F: return 'd'; // Latin Small Letter Delta
    case 0x1EA0: return 'A'; // Latin Capital Letter A With Dot Below
    case 0x1EA1: return 'a'; // Latin Small Letter A With Dot Below
    case 0x1EA2: return 'A'; // Latin Capital Letter A With Hook Above
    case 0x1EA3: return 'a'; // Latin Small Letter A With Hook Above
    case 0x1EA4: return 'A'; // Latin Capital Letter A With Circumflex And Acute
    case 0x1EA5: return 'a'; // Latin Small Letter A With Circumflex And Acute
    case 0x1EA6: return 'A'; // Latin Capital Letter A With Circumflex And Grave
    case 0x1EA7: return 'a'; // Latin Small Letter A With Circumflex And Grave
    case 0x1EA8: return 'A'; // Latin Capital Letter A With Circumflex And Hook Above
    case 0x1EA9: return 'a'; // Latin Small Letter A With Circumflex And Hook Above
    case 0x1EAA: return 'A'; // Latin Capital Letter A With Circumflex And Tilde
    case 0x1EAB: return 'a'; // Latin Small Letter A With Circumflex And Tilde
    case 0x1EAC: return 'A'; // Latin Capital Letter A With Circumflex And Dot Below
    case 0x1EAD: return 'a'; // Latin Small Letter A With Circumflex And Dot Below
    case 0x1EAE: return 'A'; // Latin Capital Letter A With Breve And Acute
    case 0x1EAF: return 'a'; // Latin Small Letter A With Breve And Acute
    case 0x1EB0: return 'A'; // Latin Capital Letter A With Breve And Grave
    case 0x1EB1: return 'a'; // Latin Small Letter A With Breve And Grave
    case 0x1EB2: return 'A'; // Latin Capital Letter A With Breve And Hook Above
    case 0x1EB3: return 'a'; // Latin Small Letter A With Breve And Hook Above
    case 0x1EB4: return 'A'; // Latin Capital Letter A With Breve And Tilde
    case 0x1EB5: return 'a'; // Latin Small Letter A With Breve And Tilde
    case 0x1EB6: return 'A'; // Latin Capital Letter A With Breve And Dot Below
    case 0x1EB7: return 'a'; // Latin Small Letter A With Breve And Dot Below
    case 0x1EB8: return 'E'; // Latin Capital Letter E With Dot Below
    case 0x1EB9: return 'e'; // Latin Small Letter E With Dot Below
    case 0x1EBA: return 'E'; // Latin Capital Letter E With Hook Above
    case 0x1EBB: return 'e'; // Latin Small Letter E With Hook Above
    case 0x1EBC: return 'E'; // Latin Capital Letter E With Tilde
    case 0x1EBE: return 'E'; // Latin Capital Letter E With Circumflex And Acute
    case 0x1EC0: return 'E'; // Latin Capital Letter E With Circumflex And Grave
    case 0x1EC1: return 'e'; // Latin Small Letter E With Circumflex And Grave
    case 0x1EC2: return 'E'; // Latin Capital Letter E With Circumflex And Hook Above
    case 0x1EC3: return 'e'; // Latin Small Letter E With Circumflex And Hook Above
    case 0x1EC4: return 'E'; // Latin Capital Letter E With Circumflex And Tilde
    case 0x1EC5: return 'e'; // Latin Small Letter E With Circumflex And Tilde
    case 0x1EC6: return 'E'; // Latin Capital Letter E With Circumflex And Dot Below
    case 0x1EC8: return 'I'; // Latin Capital Letter I With Hook Above
    case 0x1EC9: return 'i'; // Latin Small Letter I With Hook Above
    case 0x1ECA: return 'I'; // Latin Capital Letter I With Dot Below
    case 0x1ECB: return 'i'; // Latin Small Letter I With Dot Below
    case 0x1ECC: return 'O'; // Latin Capital Letter O With Dot Below
    case 0x1ECE: return 'O'; // Latin Capital Letter O With Hook Above
    case 0x1ECF: return 'o'; // Latin Small Letter O With Hook Above
    case 0x1ED0: return 'O'; // Latin Capital Letter O With Circumflex And Acute
    case 0x1ED1: return 'o'; // Latin Small Letter O With Circumflex And Acute
    case 0x1ED2: return 'O'; // Latin Capital Letter O With Circumflex And Grave
    case 0x1ED3: return 'o'; // Latin Small Letter O With Circumflex And Grave
    case 0x1ED4: return 'O'; // Latin Capital Letter O With Circumflex And Hook Above
    case 0x1ED5: return 'o'; // Latin Small Letter O With Circumflex And Hook Above
    case 0x1ED6: return 'O'; // Latin Capital Letter O With Circumflex And Tilde
    case 0x1ED7: return 'o'; // Latin Small Letter O With Circumflex And Tilde
    case 0x1ED8: return 'O'; // Latin Capital Letter O With Circumflex And Dot Below
    case 0x1ED9: return 'o'; // Latin Small Letter O With Circumflex And Dot Below
    case 0x1EDA: return 'O'; // Latin Capital Letter O With Horn And Acute
    case 0x1EDB: return 'o'; // Latin Small Letter O With Horn And Acute
    case 0x1EDC: return 'O'; // Latin Capital Letter O With Horn And Grave
    case 0x1EDD: return 'o'; // Latin Small Letter O With Horn And Grave
    case 0x1EDE: return 'O'; // Latin Capital Letter O With Horn And Hook Above
    case 0x1EDF: return 'o'; // Latin Small Letter O With Horn And Hook Above
    case 0x1EE0: return 'O'; // Latin Capital Letter O With Horn And Tilde
    case 0x1EE1: return 'o'; // Latin Small Letter O With Horn And Tilde
    case 0x1EE2: return 'O'; // Latin Capital Letter O With Horn And Dot Below
    case 0x1EE3: return 'o'; // Latin Small Letter O With Horn And Dot Below
    case 0x1EE4: return 'U'; // Latin Capital Letter U With Dot Below
    case 0x1EE6: return 'U'; // Latin Capital Letter U With Hook Above
    case 0x1EE7: return 'u'; // Latin Small Letter U With Hook Above
    case 0x1EE8: return 'U'; // Latin Capital Letter U With Horn And Acute
    case 0x1EE9: return 'u'; // Latin Small Letter U With Horn And Acute
    case 0x1EEA: return 'U'; // Latin Capital Letter U With Horn And Grave
    case 0x1EEB: return 'u'; // Latin Small Letter U With Horn And Grave
    case 0x1EEC: return 'U'; // Latin Capital Letter U With Horn And Hook Above
    case 0x1EED: return 'u'; // Latin Small Letter U With Horn And Hook Above
    case 0x1EEE: return 'U'; // Latin Capital Letter U With Horn And Tilde
    case 0x1EEF: return 'u'; // Latin Small Letter U With Horn And Tilde
    case 0x1EF0: return 'U'; // Latin Capital Letter U With Horn And Dot Below
    case 0x1EF1: return 'u'; // Latin Small Letter U With Horn And Dot Below
    case 0x1EF2: return 'Y'; // Latin Capital Letter Y With Grave
    case 0x1EF3: return 'y'; // Latin Small Letter Y With Grave
    case 0x1EF4: return 'Y'; // Latin Capital Letter Y With Dot Below
    case 0x1EF5: return 'y'; // Latin Small Letter Y With Dot Below
    case 0x1EF6: return 'Y'; // Latin Capital Letter Y With Hook Above
    case 0x1EF7: return 'y'; // Latin Small Letter Y With Hook Above
    case 0x1EF8: return 'Y'; // Latin Capital Letter Y With Tilde
    case 0x1EF9: return 'y'; // Latin Small Letter Y With Tilde
    case 0x1EFA: return 'M'; // Latin Capital Letter Middle-Welsh Ll
    case 0x1EFB: return 'm'; // Latin Small Letter Middle-Welsh Ll
    case 0x1EFC: return 'M'; // Latin Capital Letter Middle-Welsh V
    case 0x1EFD: return 'm'; // Latin Small Letter Middle-Welsh V
    case 0x1EFE: return 'Y'; // Latin Capital Letter Y With Loop
    case 0x1EFF: return 'y'; // Latin Small Letter Y With Loop
    case 0x1F00: return 'a'; // Greek Small Letter Alpha With Psili
    case 0x1F01: return 'a'; // Greek Small Letter Alpha With Dasia
    case 0x1F02: return 'a'; // Greek Small Letter Alpha With Psili And Varia
    case 0x1F03: return 'a'; // Greek Small Letter Alpha With Dasia And Varia
    case 0x1F04: return 'a'; // Greek Small Letter Alpha With Psili And Oxia
    case 0x1F05: return 'a'; // Greek Small Letter Alpha With Dasia And Oxia
    case 0x1F06: return 'a'; // Greek Small Letter Alpha With Psili And Perispomeni
    case 0x1F07: return 'a'; // Greek Small Letter Alpha With Dasia And Perispomeni
    case 0x1F08: return 'A'; // Greek Capital Letter Alpha With Psili
    case 0x1F09: return 'A'; // Greek Capital Letter Alpha With Dasia
    case 0x1F0A: return 'A'; // Greek Capital Letter Alpha With Psili And Varia
    case 0x1F0B: return 'A'; // Greek Capital Letter Alpha With Dasia And Varia
    case 0x1F0C: return 'A'; // Greek Capital Letter Alpha With Psili And Oxia
    case 0x1F0D: return 'A'; // Greek Capital Letter Alpha With Dasia And Oxia
    case 0x1F0E: return 'A'; // Greek Capital Letter Alpha With Psili And Perispomeni
    case 0x1F0F: return 'A'; // Greek Capital Letter Alpha With Dasia And Perispomeni
    case 0x1F10: return 'e'; // Greek Small Letter Epsilon With Psili
    case 0x1F11: return 'e'; // Greek Small Letter Epsilon With Dasia
    case 0x1F12: return 'e'; // Greek Small Letter Epsilon With Psili And Varia
    case 0x1F13: return 'e'; // Greek Small Letter Epsilon With Dasia And Varia
    case 0x1F14: return 'e'; // Greek Small Letter Epsilon With Psili And Oxia
    case 0x1F15: return 'e'; // Greek Small Letter Epsilon With Dasia And Oxia
    case 0x1F18: return 'E'; // Greek Capital Letter Epsilon With Psili
    case 0x1F19: return 'E'; // Greek Capital Letter Epsilon With Dasia
    case 0x1F1A: return 'E'; // Greek Capital Letter Epsilon With Psili And Varia
    case 0x1F1B: return 'E'; // Greek Capital Letter Epsilon With Dasia And Varia
    case 0x1F1C: return 'E'; // Greek Capital Letter Epsilon With Psili And Oxia
    case 0x1F1D: return 'E'; // Greek Capital Letter Epsilon With Dasia And Oxia
    case 0x1F20: return 'i'; // Greek Small Letter Eta With Psili
    case 0x1F21: return 'i'; // Greek Small Letter Eta With Dasia
    case 0x1F22: return 'i'; // Greek Small Letter Eta With Psili And Varia
    case 0x1F23: return 'i'; // Greek Small Letter Eta With Dasia And Varia
    case 0x1F24: return 'i'; // Greek Small Letter Eta With Psili And Oxia
    case 0x1F25: return 'i'; // Greek Small Letter Eta With Dasia And Oxia
    case 0x1F26: return 'i'; // Greek Small Letter Eta With Psili And Perispomeni
    case 0x1F27: return 'i'; // Greek Small Letter Eta With Dasia And Perispomeni
    case 0x1F28: return 'I'; // Greek Capital Letter Eta With Psili
    case 0x1F29: return 'I'; // Greek Capital Letter Eta With Dasia
    case 0x1F2A: return 'I'; // Greek Capital Letter Eta With Psili And Varia
    case 0x1F2B: return 'I'; // Greek Capital Letter Eta With Dasia And Varia
    case 0x1F2C: return 'I'; // Greek Capital Letter Eta With Psili And Oxia
    case 0x1F2D: return 'I'; // Greek Capital Letter Eta With Dasia And Oxia
    case 0x1F2E: return 'I'; // Greek Capital Letter Eta With Psili And Perispomeni
    case 0x1F2F: return 'I'; // Greek Capital Letter Eta With Dasia And Perispomeni
    case 0x1F30: return 'i'; // Greek Small Letter Iota With Psili
    case 0x1F31: return 'i'; // Greek Small Letter Iota With Dasia
    case 0x1F32: return 'i'; // Greek Small Letter Iota With Psili And Varia
    case 0x1F33: return 'i'; // Greek Small Letter Iota With Dasia And Varia
    case 0x1F34: return 'i'; // Greek Small Letter Iota With Psili And Oxia
    case 0x1F35: return 'i'; // Greek Small Letter Iota With Dasia And Oxia
    case 0x1F36: return 'i'; // Greek Small Letter Iota With Psili And Perispomeni
    case 0x1F37: return 'i'; // Greek Small Letter Iota With Dasia And Perispomeni
    case 0x1F38: return 'I'; // Greek Capital Letter Iota With Psili
    case 0x1F39: return 'I'; // Greek Capital Letter Iota With Dasia
    case 0x1F3A: return 'I'; // Greek Capital Letter Iota With Psili And Varia
    case 0x1F3B: return 'I'; // Greek Capital Letter Iota With Dasia And Varia
    case 0x1F3C: return 'I'; // Greek Capital Letter Iota With Psili And Oxia
    case 0x1F3D: return 'I'; // Greek Capital Letter Iota With Dasia And Oxia
    case 0x1F3E: return 'I'; // Greek Capital Letter Iota With Psili And Perispomeni
    case 0x1F3F: return 'I'; // Greek Capital Letter Iota With Dasia And Perispomeni
    case 0x1F40: return 'o'; // Greek Small Letter Omicron With Psili
    case 0x1F41: return 'o'; // Greek Small Letter Omicron With Dasia
    case 0x1F42: return 'o'; // Greek Small Letter Omicron With Psili And Varia
    case 0x1F43: return 'o'; // Greek Small Letter Omicron With Dasia And Varia
    case 0x1F44: return 'o'; // Greek Small Letter Omicron With Psili And Oxia
    case 0x1F45: return 'o'; // Greek Small Letter Omicron With Dasia And Oxia
    case 0x1F48: return 'O'; // Greek Capital Letter Omicron With Psili
    case 0x1F49: return 'O'; // Greek Capital Letter Omicron With Dasia
    case 0x1F4A: return 'O'; // Greek Capital Letter Omicron With Psili And Varia
    case 0x1F4B: return 'O'; // Greek Capital Letter Omicron With Dasia And Varia
    case 0x1F4C: return 'O'; // Greek Capital Letter Omicron With Psili And Oxia
    case 0x1F4D: return 'O'; // Greek Capital Letter Omicron With Dasia And Oxia
    case 0x1F50: return 'y'; // Greek Small Letter Upsilon With Psili
    case 0x1F51: return 'y'; // Greek Small Letter Upsilon With Dasia
    case 0x1F52: return 'y'; // Greek Small Letter Upsilon With Psili And Varia
    case 0x1F53: return 'y'; // Greek Small Letter Upsilon With Dasia And Varia
    case 0x1F54: return 'y'; // Greek Small Letter Upsilon With Psili And Oxia
    case 0x1F55: return 'y'; // Greek Small Letter Upsilon With Dasia And Oxia
    case 0x1F56: return 'y'; // Greek Small Letter Upsilon With Psili And Perispomeni
    case 0x1F57: return 'y'; // Greek Small Letter Upsilon With Dasia And Perispomeni
    case 0x1F59: return 'Y'; // Greek Capital Letter Upsilon With Dasia
    case 0x1F5B: return 'Y'; // Greek Capital Letter Upsilon With Dasia And Varia
    case 0x1F5D: return 'Y'; // Greek Capital Letter Upsilon With Dasia And Oxia
    case 0x1F5F: return 'Y'; // Greek Capital Letter Upsilon With Dasia And Perispomeni
    case 0x1F60: return 'o'; // Greek Small Letter Omega With Psili
    case 0x1F61: return 'o'; // Greek Small Letter Omega With Dasia
    case 0x1F62: return 'o'; // Greek Small Letter Omega With Psili And Varia
    case 0x1F63: return 'o'; // Greek Small Letter Omega With Dasia And Varia
    case 0x1F64: return 'o'; // Greek Small Letter Omega With Psili And Oxia
    case 0x1F65: return 'o'; // Greek Small Letter Omega With Dasia And Oxia
    case 0x1F66: return 'o'; // Greek Small Letter Omega With Psili And Perispomeni
    case 0x1F67: return 'o'; // Greek Small Letter Omega With Dasia And Perispomeni
    case 0x1F68: return 'O'; // Greek Capital Letter Omega With Psili
    case 0x1F69: return 'O'; // Greek Capital Letter Omega With Dasia
    case 0x1F6A: return 'O'; // Greek Capital Letter Omega With Psili And Varia
    case 0x1F6B: return 'O'; // Greek Capital Letter Omega With Dasia And Varia
    case 0x1F6C: return 'O'; // Greek Capital Letter Omega With Psili And Oxia
    case 0x1F6D: return 'O'; // Greek Capital Letter Omega With Dasia And Oxia
    case 0x1F6E: return 'O'; // Greek Capital Letter Omega With Psili And Perispomeni
    case 0x1F6F: return 'O'; // Greek Capital Letter Omega With Dasia And Perispomeni
    case 0x1F70: return 'a'; // Greek Small Letter Alpha With Varia
    case 0x1F71: return 'a'; // Greek Small Letter Alpha With Oxia
    case 0x1F72: return 'e'; // Greek Small Letter Epsilon With Varia
    case 0x1F73: return 'e'; // Greek Small Letter Epsilon With Oxia
    case 0x1F74: return 'i'; // Greek Small Letter Eta With Varia
    case 0x1F75: return 'i'; // Greek Small Letter Eta With Oxia
    case 0x1F76: return 'i'; // Greek Small Letter Iota With Varia
    case 0x1F77: return 'i'; // Greek Small Letter Iota With Oxia
    case 0x1F78: return 'o'; // Greek Small Letter Omicron With Varia
    case 0x1F79: return 'o'; // Greek Small Letter Omicron With Oxia
    case 0x1F7A: return 'y'; // Greek Small Letter Upsilon With Varia
    case 0x1F7B: return 'y'; // Greek Small Letter Upsilon With Oxia
    case 0x1F7C: return 'o'; // Greek Small Letter Omega With Varia
    case 0x1F7D: return 'o'; // Greek Small Letter Omega With Oxia
    case 0x1F80: return 'a'; // Greek Small Letter Alpha With Psili And Ypogegrammeni
    case 0x1F81: return 'a'; // Greek Small Letter Alpha With Dasia And Ypogegrammeni
    case 0x1F82: return 'a'; // Greek Small Letter Alpha With Psili And Varia And Ypogegrammeni
    case 0x1F83: return 'a'; // Greek Small Letter Alpha With Dasia And Varia And Ypogegrammeni
    case 0x1F84: return 'a'; // Greek Small Letter Alpha With Psili And Oxia And Ypogegrammeni
    case 0x1F85: return 'a'; // Greek Small Letter Alpha With Dasia And Oxia And Ypogegrammeni
    case 0x1F86: return 'a'; // Greek Small Letter Alpha With Psili And Perispomeni And Ypogegrammeni
    case 0x1F87: return 'a'; // Greek Small Letter Alpha With Dasia And Perispomeni And Ypogegrammeni
    case 0x1F88: return 'A'; // Greek Capital Letter Alpha With Psili And Prosgegrammeni
    case 0x1F89: return 'A'; // Greek Capital Letter Alpha With Dasia And Prosgegrammeni
    case 0x1F8A: return 'A'; // Greek Capital Letter Alpha With Psili And Varia And Prosgegrammeni
    case 0x1F8B: return 'A'; // Greek Capital Letter Alpha With Dasia And Varia And Prosgegrammeni
    case 0x1F8C: return 'A'; // Greek Capital Letter Alpha With Psili And Oxia And Prosgegrammeni
    case 0x1F8D: return 'A'; // Greek Capital Letter Alpha With Dasia And Oxia And Prosgegrammeni
    case 0x1F8E: return 'A'; // Greek Capital Letter Alpha With Psili And Perispomeni And Prosgegrammeni
    case 0x1F8F: return 'A'; // Greek Capital Letter Alpha With Dasia And Perispomeni And Prosgegrammeni
    case 0x1F90: return 'i'; // Greek Small Letter Eta With Psili And Ypogegrammeni
    case 0x1F91: return 'i'; // Greek Small Letter Eta With Dasia And Ypogegrammeni
    case 0x1F92: return 'i'; // Greek Small Letter Eta With Psili And Varia And Ypogegrammeni
    case 0x1F93: return 'i'; // Greek Small Letter Eta With Dasia And Varia And Ypogegrammeni
    case 0x1F94: return 'i'; // Greek Small Letter Eta With Psili And Oxia And Ypogegrammeni
    case 0x1F95: return 'i'; // Greek Small Letter Eta With Dasia And Oxia And Ypogegrammeni
    case 0x1F96: return 'i'; // Greek Small Letter Eta With Psili And Perispomeni And Ypogegrammeni
    case 0x1F97: return 'i'; // Greek Small Letter Eta With Dasia And Perispomeni And Ypogegrammeni
    case 0x1F98: return 'I'; // Greek Capital Letter Eta With Psili And Prosgegrammeni
    case 0x1F99: return 'I'; // Greek Capital Letter Eta With Dasia And Prosgegrammeni
    case 0x1F9A: return 'I'; // Greek Capital Letter Eta With Psili And Varia And Prosgegrammeni
    case 0x1F9B: return 'I'; // Greek Capital Letter Eta With Dasia And Varia And Prosgegrammeni
    case 0x1F9C: return 'I'; // Greek Capital Letter Eta With Psili And Oxia And Prosgegrammeni
    case 0x1F9D: return 'I'; // Greek Capital Letter Eta With Dasia And Oxia And Prosgegrammeni
    case 0x1F9E: return 'I'; // Greek Capital Letter Eta With Psili And Perispomeni And Prosgegrammeni
    case 0x1F9F: return 'I'; // Greek Capital Letter Eta With Dasia And Perispomeni And Prosgegrammeni
    case 0x1FA0: return 'o'; // Greek Small Letter Omega With Psili And Ypogegrammeni
    case 0x1FA1: return 'o'; // Greek Small Letter Omega With Dasia And Ypogegrammeni
    case 0x1FA2: return 'o'; // Greek Small Letter Omega With Psili And Varia And Ypogegrammeni
    case 0x1FA3: return 'o'; // Greek Small Letter Omega With Dasia And Varia And Ypogegrammeni
    case 0x1FA4: return 'o'; // Greek Small Letter Omega With Psili And Oxia And Ypogegrammeni
    case 0x1FA5: return 'o'; // Greek Small Letter Omega With Dasia And Oxia And Ypogegrammeni
    case 0x1FA6: return 'o'; // Greek Small Letter Omega With Psili And Perispomeni And Ypogegrammeni
    case 0x1FA7: return 'o'; // Greek Small Letter Omega With Dasia And Perispomeni And Ypogegrammeni
    case 0x1FA8: return 'O'; // Greek Capital Letter Omega With Psili And Prosgegrammeni
    case 0x1FA9: return 'O'; // Greek Capital Letter Omega With Dasia And Prosgegrammeni
    case 0x1FAA: return 'O'; // Greek Capital Letter Omega With Psili And Varia And Prosgegrammeni
    case 0x1FAB: return 'O'; // Greek Capital Letter Omega With Dasia And Varia And Prosgegrammeni
    case 0x1FAC: return 'O'; // Greek Capital Letter Omega With Psili And Oxia And Prosgegrammeni
    case 0x1FAD: return 'O'; // Greek Capital Letter Omega With Dasia And Oxia And Prosgegrammeni
    case 0x1FAE: return 'O'; // Greek Capital Letter Omega With Psili And Perispomeni And Prosgegrammeni
    case 0x1FAF: return 'O'; // Greek Capital Letter Omega With Dasia And Perispomeni And Prosgegrammeni
    case 0x1FB0: return 'a'; // Greek Small Letter Alpha With Vrachy
    case 0x1FB1: return 'a'; // Greek Small Letter Alpha With Macron
    case 0x1FB2: return 'a'; // Greek Small Letter Alpha With Varia And Ypogegrammeni
    case 0x1FB3: return 'a'; // Greek Small Letter Alpha With Ypogegrammeni
    case 0x1FB4: return 'a'; // Greek Small Letter Alpha With Oxia And Ypogegrammeni
    case 0x1FB6: return 'a'; // Greek Small Letter Alpha With Perispomeni
    case 0x1FB7: return 'a'; // Greek Small Letter Alpha With Perispomeni And Ypogegrammeni
    case 0x1FB8: return 'A'; // Greek Capital Letter Alpha With Vrachy
    case 0x1FB9: return 'A'; // Greek Capital Letter Alpha With Macron
    case 0x1FBA: return 'A'; // Greek Capital Letter Alpha With Varia
    case 0x1FBB: return 'A'; // Greek Capital Letter Alpha With Oxia
    case 0x1FBC: return 'A'; // Greek Capital Letter Alpha With Prosgegrammeni
    case 0x1FBD: return '\''; // Greek Koronis
    case 0x1FBF: return 0; // Greek Psili
    case 0x1FC0: return 0; // Greek Perispomeni
    case 0x1FC1: return 0; // Greek Dialytika And Perispomeni
    case 0x1FC2: return 'i'; // Greek Small Letter Eta With Varia And Ypogegrammeni
    case 0x1FC3: return 'i'; // Greek Small Letter Eta With Ypogegrammeni
    case 0x1FC4: return 'i'; // Greek Small Letter Eta With Oxia And Ypogegrammeni
    case 0x1FC6: return 'i'; // Greek Small Letter Eta With Perispomeni
    case 0x1FC7: return 'i'; // Greek Small Letter Eta With Perispomeni And Ypogegrammeni
    case 0x1FC8: return 'E'; // Greek Capital Letter Epsilon With Varia
    case 0x1FC9: return 'E'; // Greek Capital Letter Epsilon With Oxia
    case 0x1FCA: return 'I'; // Greek Capital Letter Eta With Varia
    case 0x1FCB: return 'I'; // Greek Capital Letter Eta With Oxia
    case 0x1FCC: return 'I'; // Greek Capital Letter Eta With Prosgegrammeni
    case 0x1FCD: return 0; // Greek Psili And Varia
    case 0x1FCE: return 0; // Greek Psili And Oxia
    case 0x1FCF: return 0; // Greek Psili And Perispomeni
    case 0x1FD0: return 'i'; // Greek Small Letter Iota With Vrachy
    case 0x1FD1: return 'i'; // Greek Small Letter Iota With Macron
    case 0x1FD2: return 'i'; // Greek Small Letter Iota With Dialytika And Varia
    case 0x1FD3: return 'i'; // Greek Small Letter Iota With Dialytika And Oxia
    case 0x1FD6: return 'i'; // Greek Small Letter Iota With Perispomeni
    case 0x1FD7: return 'i'; // Greek Small Letter Iota With Dialytika And Perispomeni
    case 0x1FD8: return 'I'; // Greek Capital Letter Iota With Vrachy
    case 0x1FD9: return 'I'; // Greek Capital Letter Iota With Macron
    case 0x1FDA: return 'I'; // Greek Capital Letter Iota With Varia
    case 0x1FDB: return 'I'; // Greek Capital Letter Iota With Oxia
    case 0x1FDD: return 0; // Greek Dasia And Varia
    case 0x1FDE: return 0; // Greek Dasia And Oxia
    case 0x1FDF: return 0; // Greek Dasia And Perispomeni
    case 0x1FE0: return 'y'; // Greek Small Letter Upsilon With Vrachy
    case 0x1FE1: return 'y'; // Greek Small Letter Upsilon With Macron
    case 0x1FE2: return 'y'; // Greek Small Letter Upsilon With Dialytika And Varia
    case 0x1FE3: return 'y'; // Greek Small Letter Upsilon With Dialytika And Oxia
    case 0x1FE4: return 'r'; // Greek Small Letter Rho With Psili
    case 0x1FE5: return 'r'; // Greek Small Letter Rho With Dasia
    case 0x1FE6: return 'y'; // Greek Small Letter Upsilon With Perispomeni
    case 0x1FE7: return 'y'; // Greek Small Letter Upsilon With Dialytika And Perispomeni
    case 0x1FE8: return 'Y'; // Greek Capital Letter Upsilon With Vrachy
    case 0x1FE9: return 'Y'; // Greek Capital Letter Upsilon With Macron
    case 0x1FEA: return 'Y'; // Greek Capital Letter Upsilon With Varia
    case 0x1FEB: return 'Y'; // Greek Capital Letter Upsilon With Oxia
    case 0x1FEC: return 'R'; // Greek Capital Letter Rho With Dasia
    case 0x1FED: return 0; // Greek Dialytika And Varia
    case 0x1FEE: return 0; // Greek Dialytika And Oxia
    case 0x1FEF: return 0; // Greek Varia
    case 0x1FF2: return 'o'; // Greek Small Letter Omega With Varia And Ypogegrammeni
    case 0x1FF3: return 'o'; // Greek Small Letter Omega With Ypogegrammeni
    case 0x1FF4: return 'o'; // Greek Small Letter Omega With Oxia And Ypogegrammeni
    case 0x1FF6: return 'o'; // Greek Small Letter Omega With Perispomeni
    case 0x1FF7: return 'o'; // Greek Small Letter Omega With Perispomeni And Ypogegrammeni
    case 0x1FF8: return 'O'; // Greek Capital Letter Omicron With Varia
    case 0x1FF9: return 'O'; // Greek Capital Letter Omicron With Oxia
    case 0x1FFA: return 'O'; // Greek Capital Letter Omega With Varia
    case 0x1FFB: return 'O'; // Greek Capital Letter Omega With Oxia
    case 0x1FFC: return 'O'; // Greek Capital Letter Omega With Prosgegrammeni
    case 0x1FFD: return 0; // Greek Oxia
    case 0x1FFE: return 0; // Greek Dasia
    case 0x200B: return 0; // Zero Width Space
    case 0x200C: return 0; // Zero Width Non-Joiner
    case 0x200D: return 0; // Zero Width Joiner
    case 0x2020: return '+'; // Dagger
    case 0x2021: return '+'; // Double Dagger
    case 0x2023: return '>'; // Triangular Bullet
    case 0x2030: return '%'; // Per Mille Sign
    case 0x2032: return '\''; // Prime
    case 0x2033: return '"'; // Double Prime
    case 0x2070: return '0'; // Superscript Zero
    case 0x2074: return '4'; // Superscript Four
    case 0x2075: return '5'; // Superscript Five
    case 0x2076: return '6'; // Superscript Six
    case 0x2077: return '7'; // Superscript Seven
    case 0x2078: return '8'; // Superscript Eight
    case 0x2079: return '9'; // Superscript Nine
    case 0x207A: return '+'; // Superscript Plus Sign
    case 0x207B: return '-'; // Superscript Minus
    case 0x207C: return '='; // Superscript Equals Sign
    case 0x207D: return '('; // Superscript Left Parenthesis
    case 0x207E: return ')'; // Superscript Right Parenthesis
    case 0x207F: return 'n'; // Superscript Latin Small Letter N
    case 0x2080: return '0'; // Subscript Zero
    case 0x2081: return '1'; // Subscript One
    case 0x2082: return '2'; // Subscript Two
    case 0x2083: return '3'; // Subscript Three
    case 0x2084: return '4'; // Subscript Four
    case 0x2085: return '5'; // Subscript Five
    case 0x2086: return '6'; // Subscript Six
    case 0x2087: return '7'; // Subscript Seven
    case 0x2088: return '8'; // Subscript Eight
    case 0x2089: return '9'; // Subscript Nine
    case 0x208A: return '+'; // Subscript Plus Sign
    case 0x208B: return '-'; // Subscript Minus
    case 0x208C: return '='; // Subscript Equals Sign
    case 0x208D: return '('; // Subscript Left Parenthesis
    case 0x208E: return ')'; // Subscript Right Parenthesis
    case 0x20A9: return 'W'; // Won Sign
    case 0x20AA: return 's'; // New Sheqel Sign
    case 0x20AB: return 'd'; // Dong Sign
    case 0x20AD: return 'd'; // Kip Sign
    case 0x20AE: return 'd'; // Tugrik Sign
    case 0x20B4: return 'h'; // Hryvnia Sign
    case 0x20B8: return 'd'; // Tenge Sign
    case 0x20B9: return 'R'; // Indian Rupee Sign
    case 0x2190: return '<'; // Leftwards Arrow
    case 0x2191: return '^'; // Upwards Arrow
    case 0x2192: return '>'; // Rightwards Arrow
    case 0x2193: return 'v'; // Downwards Arrow
    case 0x21D2: return '='; // Rightwards Double Arrow
    case 0x21D4: return '='; // Left Right Double Arrow
    case 0x2202: return 'e'; // Partial Differential
    case 0x220F: return 'P'; // N-Ary Product
    case 0x2211: return 'S'; // N-Ary Summation
    case 0x2212: return '-'; // Minus Sign
    case 0x221A: return 'v'; // Square Root
    case 0x221E: return '8'; // Infinity
    case 0x2227: return '^'; // Logical And
    case 0x2228: return 'v'; // Logical Or
    case 0x2229: return 'n'; // Intersection
    case 0x222A: return 'u'; // Union
    case 0x2248: return '='; // Almost Equal To
    case 0x2260: return '='; // Not Equal To
    case 0x2264: return '<'; // Less-Than Or Equal To
    case 0x2265: return '>'; // Greater-Than Or Equal To
    case 0x2282: return 'c'; // Subset Of
    case 0x2283: return 'c'; // Superset Of
    case 0x22C5: return '*'; // Dot Operator
    case 0x2580: return '-'; // Upper Half Block
    case 0x2584: return '_'; // Lower Half Block
    case 0x2591: return '.'; // Light Shade
    case 0x2593: return '#'; // Dark Shade
    case 0x25A0: return '#'; // Black Square
    case 0x25A1: return ' '; // White Square
    case 0x25AA: return '#'; // Black Small Square
    case 0x25AB: return ' '; // White Small Square
    case 0x25B2: return '^'; // Black Up-Pointing Triangle
    case 0x25BC: return 'v'; // Black Down-Pointing Triangle
    case 0x25C6: return '+'; // Black Diamond
    case 0x25CA: return '+'; // Lozenge
    case 0x25CB: return 'o'; // White Circle
    case 0x25CF: return 'o'; // Black Circle
    case 0x25E6: return 'o'; // White Bullet
    case 0x2600: return 'o'; // Black Sun With Rays
    case 0x2605: return '*'; // Black Star
    case 0x2606: return '*'; // White Star
    case 0x2620: return 'x'; // Skull And Crossbones
    case 0x263A: return ':'; // White Smiling Face
    case 0x263B: return ':'; // Black Smiling Face
    case 0x2660: return 's'; // Black Spade Suit
    case 0x2663: return 'c'; // Black Club Suit
    case 0x2665: return 'h'; // Black Heart Suit
    case 0x2666: return 'd'; // Black Diamond Suit
    case 0x266A: return '#'; // Eighth Note
    case 0x266B: return '#'; // Beamed Eighth Notes
    case 0x2713: return 'v'; // Check Mark
    case 0x2714: return 'v'; // Heavy Check Mark
    case 0x2717: return 'x'; // Ballot X
    case 0x2718: return 'x'; // Heavy Ballot X
    case 0x2C00: return 'A'; // Glagolitic Capital Letter Azu
    case 0x2C01: return 'B'; // Glagolitic Capital Letter Buky
    case 0x2C03: return 'G'; // Glagolitic Capital Letter Glagoli
    case 0x2C04: return 'D'; // Glagolitic Capital Letter Dobro
    case 0x2C05: return 'Y'; // Glagolitic Capital Letter Yestu
    case 0x2C06: return 'Z'; // Glagolitic Capital Letter Zhivete
    case 0x2C07: return 'D'; // Glagolitic Capital Letter Dzelo
    case 0x2C08: return 'Z'; // Glagolitic Capital Letter Zemlja
    case 0x2C09: return 'I'; // Glagolitic Capital Letter Izhe
    case 0x2C0A: return 'I'; // Glagolitic Capital Letter Initial Izhe
    case 0x2C0B: return 'I'; // Glagolitic Capital Letter I
    case 0x2C0C: return 'D'; // Glagolitic Capital Letter Djervi
    case 0x2C0F: return 'M'; // Glagolitic Capital Letter Myslite
    case 0x2C12: return 'P'; // Glagolitic Capital Letter Pokoji
    case 0x2C13: return 'R'; // Glagolitic Capital Letter Ritsi
    case 0x2C15: return 'T'; // Glagolitic Capital Letter Tvrido
    case 0x2C16: return 'U'; // Glagolitic Capital Letter Uku
    case 0x2C17: return 'F'; // Glagolitic Capital Letter Fritu
    case 0x2C18: return 'H'; // Glagolitic Capital Letter Heru
    case 0x2C19: return 'O'; // Glagolitic Capital Letter Otu
    case 0x2C1A: return 'P'; // Glagolitic Capital Letter Pe
    case 0x2C1B: return 'S'; // Glagolitic Capital Letter Shta
    case 0x2C1C: return 'T'; // Glagolitic Capital Letter Tsi
    case 0x2C1D: return 'C'; // Glagolitic Capital Letter Chrivi
    case 0x2C1E: return 'S'; // Glagolitic Capital Letter Sha
    case 0x2C22: return 'S'; // Glagolitic Capital Letter Spidery Ha
    case 0x2C23: return 'Y'; // Glagolitic Capital Letter Yu
    case 0x2C24: return 'Y'; // Glagolitic Capital Letter Small Yus
    case 0x2C25: return 'Y'; // Glagolitic Capital Letter Small Yus With Tail
    case 0x2C26: return 'Y'; // Glagolitic Capital Letter Yo
    case 0x2C27: return 'Y'; // Glagolitic Capital Letter Iotated Small Yus
    case 0x2C28: return 'B'; // Glagolitic Capital Letter Big Yus
    case 0x2C29: return 'B'; // Glagolitic Capital Letter Iotated Big Yus
    case 0x2C2A: return 'F'; // Glagolitic Capital Letter Fita
    case 0x2C2B: return 'I'; // Glagolitic Capital Letter Izhitsa
    case 0x2C2C: return 'S'; // Glagolitic Capital Letter Shtapic
    case 0x2C2D: return 'T'; // Glagolitic Capital Letter Trokutasti A
    case 0x2C2E: return 'L'; // Glagolitic Capital Letter Latinate Myslite
    case 0x2C2F: return 'C'; // Glagolitic Capital Letter Caudate Chrivi
    case 0x2C30: return 'a'; // Glagolitic Small Letter Azu
    case 0x2C31: return 'b'; // Glagolitic Small Letter Buky
    case 0x2C32: return 'v'; // Glagolitic Small Letter Vede
    case 0x2C33: return 'g'; // Glagolitic Small Letter Glagoli
    case 0x2C34: return 'd'; // Glagolitic Small Letter Dobro
    case 0x2C35: return 'y'; // Glagolitic Small Letter Yestu
    case 0x2C36: return 'z'; // Glagolitic Small Letter Zhivete
    case 0x2C37: return 'd'; // Glagolitic Small Letter Dzelo
    case 0x2C38: return 'z'; // Glagolitic Small Letter Zemlja
    case 0x2C39: return 'i'; // Glagolitic Small Letter Izhe
    case 0x2C3A: return 'i'; // Glagolitic Small Letter Initial Izhe
    case 0x2C3B: return 'i'; // Glagolitic Small Letter I
    case 0x2C3C: return 'd'; // Glagolitic Small Letter Djervi
    case 0x2C3D: return 'k'; // Glagolitic Small Letter Kako
    case 0x2C3E: return 'l'; // Glagolitic Small Letter Ljudije
    case 0x2C3F: return 'm'; // Glagolitic Small Letter Myslite
    case 0x2C40: return 'n'; // Glagolitic Small Letter Nashi
    case 0x2C41: return 'o'; // Glagolitic Small Letter Onu
    case 0x2C42: return 'p'; // Glagolitic Small Letter Pokoji
    case 0x2C43: return 'r'; // Glagolitic Small Letter Ritsi
    case 0x2C44: return 's'; // Glagolitic Small Letter Slovo
    case 0x2C45: return 't'; // Glagolitic Small Letter Tvrido
    case 0x2C46: return 'u'; // Glagolitic Small Letter Uku
    case 0x2C47: return 'f'; // Glagolitic Small Letter Fritu
    case 0x2C48: return 'h'; // Glagolitic Small Letter Heru
    case 0x2C49: return 'o'; // Glagolitic Small Letter Otu
    case 0x2C4A: return 'p'; // Glagolitic Small Letter Pe
    case 0x2C4B: return 's'; // Glagolitic Small Letter Shta
    case 0x2C4C: return 't'; // Glagolitic Small Letter Tsi
    case 0x2C4D: return 'c'; // Glagolitic Small Letter Chrivi
    case 0x2C4E: return 's'; // Glagolitic Small Letter Sha
    case 0x2C4F: return 'y'; // Glagolitic Small Letter Yeru
    case 0x2C50: return 'y'; // Glagolitic Small Letter Yeri
    case 0x2C51: return 'y'; // Glagolitic Small Letter Yati
    case 0x2C52: return 's'; // Glagolitic Small Letter Spidery Ha
    case 0x2C53: return 'y'; // Glagolitic Small Letter Yu
    case 0x2C54: return 'y'; // Glagolitic Small Letter Small Yus
    case 0x2C55: return 'y'; // Glagolitic Small Letter Small Yus With Tail
    case 0x2C56: return 'y'; // Glagolitic Small Letter Yo
    case 0x2C57: return 'y'; // Glagolitic Small Letter Iotated Small Yus
    case 0x2C58: return 'b'; // Glagolitic Small Letter Big Yus
    case 0x2C59: return 'b'; // Glagolitic Small Letter Iotated Big Yus
    case 0x2C5A: return 'f'; // Glagolitic Small Letter Fita
    case 0x2C5B: return 'i'; // Glagolitic Small Letter Izhitsa
    case 0x2C5C: return 's'; // Glagolitic Small Letter Shtapic
    case 0x2C5D: return 't'; // Glagolitic Small Letter Trokutasti A
    case 0x2C5E: return 'l'; // Glagolitic Small Letter Latinate Myslite
    case 0x2C5F: return 'c'; // Glagolitic Small Letter Caudate Chrivi
    case 0x2D80: return 'l'; // Ethiopic Syllable Loa
    case 0x2D81: return 'm'; // Ethiopic Syllable Moa
    case 0x2D82: return 'r'; // Ethiopic Syllable Roa
    case 0x2D83: return 's'; // Ethiopic Syllable Soa
    case 0x2D84: return 's'; // Ethiopic Syllable Shoa
    case 0x2D85: return 'b'; // Ethiopic Syllable Boa
    case 0x2D86: return 't'; // Ethiopic Syllable Toa
    case 0x2D87: return 'c'; // Ethiopic Syllable Coa
    case 0x2D88: return 'n'; // Ethiopic Syllable Noa
    case 0x2D89: return 'n'; // Ethiopic Syllable Nyoa
    case 0x2D8A: return 'a'; // Ethiopic Syllable Glottal Oa
    case 0x2D8B: return 'z'; // Ethiopic Syllable Zoa
    case 0x2D8C: return 'd'; // Ethiopic Syllable Doa
    case 0x2D8D: return 'd'; // Ethiopic Syllable Ddoa
    case 0x2D8E: return 'j'; // Ethiopic Syllable Joa
    case 0x2D8F: return 't'; // Ethiopic Syllable Thoa
    case 0x2D90: return 'c'; // Ethiopic Syllable Choa
    case 0x2D91: return 'p'; // Ethiopic Syllable Phoa
    case 0x2D92: return 'p'; // Ethiopic Syllable Poa
    case 0x2D93: return 'g'; // Ethiopic Syllable Ggwa
    case 0x2D94: return 'g'; // Ethiopic Syllable Ggwi
    case 0x2D95: return 'g'; // Ethiopic Syllable Ggwee
    case 0x2D96: return 'g'; // Ethiopic Syllable Ggwe
    case 0x2DA0: return 's'; // Ethiopic Syllable Ssa
    case 0x2DA1: return 's'; // Ethiopic Syllable Ssu
    case 0x2DA2: return 's'; // Ethiopic Syllable Ssi
    case 0x2DA3: return 's'; // Ethiopic Syllable Ssaa
    case 0x2DA4: return 's'; // Ethiopic Syllable Ssee
    case 0x2DA5: return 's'; // Ethiopic Syllable Sse
    case 0x2DA6: return 's'; // Ethiopic Syllable Sso
    case 0x2DA8: return 'c'; // Ethiopic Syllable Cca
    case 0x2DA9: return 'c'; // Ethiopic Syllable Ccu
    case 0x2DAA: return 'c'; // Ethiopic Syllable Cci
    case 0x2DAB: return 'c'; // Ethiopic Syllable Ccaa
    case 0x2DAC: return 'c'; // Ethiopic Syllable Ccee
    case 0x2DAD: return 'c'; // Ethiopic Syllable Cce
    case 0x2DAE: return 'c'; // Ethiopic Syllable Cco
    case 0x2DB0: return 'z'; // Ethiopic Syllable Zza
    case 0x2DB1: return 'z'; // Ethiopic Syllable Zzu
    case 0x2DB2: return 'z'; // Ethiopic Syllable Zzi
    case 0x2DB3: return 'z'; // Ethiopic Syllable Zzaa
    case 0x2DB4: return 'z'; // Ethiopic Syllable Zzee
    case 0x2DB5: return 'z'; // Ethiopic Syllable Zze
    case 0x2DB6: return 'z'; // Ethiopic Syllable Zzo
    case 0x2DB8: return 'c'; // Ethiopic Syllable Ccha
    case 0x2DB9: return 'c'; // Ethiopic Syllable Cchu
    case 0x2DBA: return 'c'; // Ethiopic Syllable Cchi
    case 0x2DBB: return 'c'; // Ethiopic Syllable Cchaa
    case 0x2DBC: return 'c'; // Ethiopic Syllable Cchee
    case 0x2DBD: return 'c'; // Ethiopic Syllable Cche
    case 0x2DBE: return 'c'; // Ethiopic Syllable Ccho
    case 0x2DC0: return 'q'; // Ethiopic Syllable Qya
    case 0x2DC1: return 'q'; // Ethiopic Syllable Qyu
    case 0x2DC2: return 'q'; // Ethiopic Syllable Qyi
    case 0x2DC3: return 'q'; // Ethiopic Syllable Qyaa
    case 0x2DC4: return 'q'; // Ethiopic Syllable Qyee
    case 0x2DC5: return 'q'; // Ethiopic Syllable Qye
    case 0x2DC6: return 'q'; // Ethiopic Syllable Qyo
    case 0x2DC8: return 'k'; // Ethiopic Syllable Kya
    case 0x2DC9: return 'k'; // Ethiopic Syllable Kyu
    case 0x2DCA: return 'k'; // Ethiopic Syllable Kyi
    case 0x2DCB: return 'k'; // Ethiopic Syllable Kyaa
    case 0x2DCC: return 'k'; // Ethiopic Syllable Kyee
    case 0x2DCD: return 'k'; // Ethiopic Syllable Kye
    case 0x2DCE: return 'k'; // Ethiopic Syllable Kyo
    case 0x2DD0: return 'x'; // Ethiopic Syllable Xya
    case 0x2DD1: return 'x'; // Ethiopic Syllable Xyu
    case 0x2DD2: return 'x'; // Ethiopic Syllable Xyi
    case 0x2DD3: return 'x'; // Ethiopic Syllable Xyaa
    case 0x2DD4: return 'x'; // Ethiopic Syllable Xyee
    case 0x2DD5: return 'x'; // Ethiopic Syllable Xye
    case 0x2DD6: return 'x'; // Ethiopic Syllable Xyo
    case 0x2DD8: return 'g'; // Ethiopic Syllable Gya
    case 0x2DD9: return 'g'; // Ethiopic Syllable Gyu
    case 0x2DDA: return 'g'; // Ethiopic Syllable Gyi
    case 0x2DDB: return 'g'; // Ethiopic Syllable Gyaa
    case 0x2DDC: return 'g'; // Ethiopic Syllable Gyee
    case 0x2DDD: return 'g'; // Ethiopic Syllable Gye
    case 0x2DDE: return 'g'; // Ethiopic Syllable Gyo
    case 0x2E3A: return '-'; // Two-Em Dash
    case 0x2E3B: return '-'; // Three-Em Dash
    case 0x312A: return 'v'; // Bopomofo Letter V
    case 0x312B: return 'n'; // Bopomofo Letter Ng
    case 0x312C: return 'g'; // Bopomofo Letter Gn
    case 0x312D: return 'i'; // Bopomofo Letter Ih
    case 0x312E: return 'o'; // Bopomofo Letter O With Dot Above
    case 0x312F: return 'n'; // Bopomofo Letter Nn
    case 0x3131: return 'k'; // Hangul Letter Kiyeok
    case 0x3132: return 's'; // Hangul Letter Ssangkiyeok
    case 0x3133: return 'k'; // Hangul Letter Kiyeok-Sios
    case 0x3134: return 'n'; // Hangul Letter Nieun
    case 0x3135: return 'n'; // Hangul Letter Nieun-Cieuc
    case 0x3136: return 'n'; // Hangul Letter Nieun-Hieuh
    case 0x3137: return 't'; // Hangul Letter Tikeut
    case 0x3138: return 's'; // Hangul Letter Ssangtikeut
    case 0x3139: return 'r'; // Hangul Letter Rieul
    case 0x313A: return 'r'; // Hangul Letter Rieul-Kiyeok
    case 0x313B: return 'r'; // Hangul Letter Rieul-Mieum
    case 0x313C: return 'r'; // Hangul Letter Rieul-Pieup
    case 0x313D: return 'r'; // Hangul Letter Rieul-Sios
    case 0x313E: return 'r'; // Hangul Letter Rieul-Thieuth
    case 0x313F: return 'r'; // Hangul Letter Rieul-Phieuph
    case 0x3140: return 'r'; // Hangul Letter Rieul-Hieuh
    case 0x3141: return 'm'; // Hangul Letter Mieum
    case 0x3142: return 'p'; // Hangul Letter Pieup
    case 0x3143: return 's'; // Hangul Letter Ssangpieup
    case 0x3144: return 'p'; // Hangul Letter Pieup-Sios
    case 0x3145: return 's'; // Hangul Letter Sios
    case 0x3146: return 's'; // Hangul Letter Ssangsios
    case 0x3147: return 'i'; // Hangul Letter Ieung
    case 0x3148: return 'c'; // Hangul Letter Cieuc
    case 0x3149: return 's'; // Hangul Letter Ssangcieuc
    case 0x314A: return 'c'; // Hangul Letter Chieuch
    case 0x314B: return 'k'; // Hangul Letter Khieukh
    case 0x314C: return 't'; // Hangul Letter Thieuth
    case 0x314D: return 'p'; // Hangul Letter Phieuph
    case 0x314E: return 'h'; // Hangul Letter Hieuh
    case 0x314F: return 'a'; // Hangul Letter A
    case 0x3150: return 'a'; // Hangul Letter Ae
    case 0x3151: return 'y'; // Hangul Letter Ya
    case 0x3152: return 'y'; // Hangul Letter Yae
    case 0x3153: return 'e'; // Hangul Letter Eo
    case 0x3154: return 'e'; // Hangul Letter E
    case 0x3155: return 'y'; // Hangul Letter Yeo
    case 0x3156: return 'y'; // Hangul Letter Ye
    case 0x3157: return 'o'; // Hangul Letter O
    case 0x3158: return 'w'; // Hangul Letter Wa
    case 0x3159: return 'w'; // Hangul Letter Wae
    case 0x315A: return 'o'; // Hangul Letter Oe
    case 0x315B: return 'y'; // Hangul Letter Yo
    case 0x315C: return 'u'; // Hangul Letter U
    case 0x315D: return 'w'; // Hangul Letter Weo
    case 0x315E: return 'w'; // Hangul Letter We
    case 0x315F: return 'w'; // Hangul Letter Wi
    case 0x3160: return 'y'; // Hangul Letter Yu
    case 0x3161: return 'e'; // Hangul Letter Eu
    case 0x3162: return 'y'; // Hangul Letter Yi
    case 0x3163: return 'i'; // Hangul Letter I
    case 0x3165: return 's'; // Hangul Letter Ssangnieun
    case 0x3166: return 'n'; // Hangul Letter Nieun-Tikeut
    case 0x3167: return 'n'; // Hangul Letter Nieun-Sios
    case 0x3168: return 'n'; // Hangul Letter Nieun-Pansios
    case 0x3169: return 'r'; // Hangul Letter Rieul-Kiyeok-Sios
    case 0x316A: return 'r'; // Hangul Letter Rieul-Tikeut
    case 0x316B: return 'r'; // Hangul Letter Rieul-Pieup-Sios
    case 0x316C: return 'r'; // Hangul Letter Rieul-Pansios
    case 0x316D: return 'r'; // Hangul Letter Rieul-Yeorinhieuh
    case 0x316E: return 'm'; // Hangul Letter Mieum-Pieup
    case 0x316F: return 'm'; // Hangul Letter Mieum-Sios
    case 0x3170: return 'm'; // Hangul Letter Mieum-Pansios
    case 0x3171: return 'k'; // Hangul Letter Kapyeounmieum
    case 0x3172: return 'p'; // Hangul Letter Pieup-Kiyeok
    case 0x3173: return 'p'; // Hangul Letter Pieup-Tikeut
    case 0x3174: return 'p'; // Hangul Letter Pieup-Sios-Kiyeok
    case 0x3175: return 'p'; // Hangul Letter Pieup-Sios-Tikeut
    case 0x3176: return 'p'; // Hangul Letter Pieup-Cieuc
    case 0x3177: return 'p'; // Hangul Letter Pieup-Thieuth
    case 0x3178: return 'k'; // Hangul Letter Kapyeounpieup
    case 0x3179: return 'k'; // Hangul Letter Kapyeounssangpieup
    case 0x317A: return 's'; // Hangul Letter Sios-Kiyeok
    case 0x317B: return 's'; // Hangul Letter Sios-Nieun
    case 0x317C: return 's'; // Hangul Letter Sios-Tikeut
    case 0x317D: return 's'; // Hangul Letter Sios-Pieup
    case 0x317E: return 's'; // Hangul Letter Sios-Cieuc
    case 0x317F: return 'p'; // Hangul Letter Pansios
    case 0x3180: return 's'; // Hangul Letter Ssangieung
    case 0x3181: return 'y'; // Hangul Letter Yesieung
    case 0x3182: return 'y'; // Hangul Letter Yesieung-Sios
    case 0x3183: return 'y'; // Hangul Letter Yesieung-Pansios
    case 0x3184: return 'k'; // Hangul Letter Kapyeounphieuph
    case 0x3185: return 's'; // Hangul Letter Ssanghieuh
    case 0x3186: return 'y'; // Hangul Letter Yeorinhieuh
    case 0x3187: return 'y'; // Hangul Letter Yo-Ya
    case 0x3188: return 'y'; // Hangul Letter Yo-Yae
    case 0x3189: return 'y'; // Hangul Letter Yo-I
    case 0x318A: return 'y'; // Hangul Letter Yu-Yeo
    case 0x318B: return 'y'; // Hangul Letter Yu-Ye
    case 0x318C: return 'y'; // Hangul Letter Yu-I
    case 0x318D: return 'a'; // Hangul Letter Araea
    case 0x318E: return 'a'; // Hangul Letter Araeae
    case 0x31A0: return 'b'; // Bopomofo Letter Bu
    case 0x31A1: return 'z'; // Bopomofo Letter Zi
    case 0x31A2: return 'j'; // Bopomofo Letter Ji
    case 0x31A3: return 'g'; // Bopomofo Letter Gu
    case 0x31A4: return 'e'; // Bopomofo Letter Ee
    case 0x31A5: return 'e'; // Bopomofo Letter Enn
    case 0x31A6: return 'o'; // Bopomofo Letter Oo
    case 0x31A7: return 'o'; // Bopomofo Letter Onn
    case 0x31A8: return 'i'; // Bopomofo Letter Ir
    case 0x31A9: return 'a'; // Bopomofo Letter Ann
    case 0x31AA: return 'i'; // Bopomofo Letter Inn
    case 0x31AB: return 'u'; // Bopomofo Letter Unn
    case 0x31AC: return 'i'; // Bopomofo Letter Im
    case 0x31AD: return 'n'; // Bopomofo Letter Ngg
    case 0x31AE: return 'a'; // Bopomofo Letter Ainn
    case 0x31AF: return 'a'; // Bopomofo Letter Aunn
    case 0x31B0: return 'a'; // Bopomofo Letter Am
    case 0x31B1: return 'o'; // Bopomofo Letter Om
    case 0x31B2: return 'o'; // Bopomofo Letter Ong
    case 0x31B3: return 'i'; // Bopomofo Letter Innn
    case 0x31B4: return 'p'; // Bopomofo Final Letter P
    case 0x31B5: return 't'; // Bopomofo Final Letter T
    case 0x31B6: return 'k'; // Bopomofo Final Letter K
    case 0x31B7: return 'h'; // Bopomofo Final Letter H
    case 0x31B8: return 'g'; // Bopomofo Letter Gh
    case 0x31B9: return 'l'; // Bopomofo Letter Lh
    case 0x31BA: return 'z'; // Bopomofo Letter Zy
    case 0x31BB: return 'g'; // Bopomofo Final Letter G
    case 0x31BC: return 'g'; // Bopomofo Letter Gw
    case 0x31BD: return 'k'; // Bopomofo Letter Kw
    case 0x31BE: return 'o'; // Bopomofo Letter Oe
    case 0x31BF: return 'a'; // Bopomofo Letter Ah
    case 0xA800: return 'a'; // Syloti Nagri Letter A
    case 0xA801: return 'i'; // Syloti Nagri Letter I
    case 0xA803: return 'u'; // Syloti Nagri Letter U
    case 0xA804: return 'e'; // Syloti Nagri Letter E
    case 0xA805: return 'o'; // Syloti Nagri Letter O
    case 0xA807: return 'k'; // Syloti Nagri Letter Ko
    case 0xA808: return 'k'; // Syloti Nagri Letter Kho
    case 0xA809: return 'g'; // Syloti Nagri Letter Go
    case 0xA80A: return 'g'; // Syloti Nagri Letter Gho
    case 0xA80B: return 'n'; // Syloti Nagri Sign Anusvara
    case 0xA80C: return 'c'; // Syloti Nagri Letter Co
    case 0xA80E: return 'j'; // Syloti Nagri Letter Jo
    case 0xA80F: return 'j'; // Syloti Nagri Letter Jho
    case 0xA811: return 't'; // Syloti Nagri Letter Ttho
    case 0xA812: return 'd'; // Syloti Nagri Letter Ddo
    case 0xA813: return 'd'; // Syloti Nagri Letter Ddho
    case 0xA815: return 't'; // Syloti Nagri Letter Tho
    case 0xA816: return 'd'; // Syloti Nagri Letter Do
    case 0xA817: return 'd'; // Syloti Nagri Letter Dho
    case 0xA818: return 'n'; // Syloti Nagri Letter No
    case 0xA819: return 'p'; // Syloti Nagri Letter Po
    case 0xA81B: return 'b'; // Syloti Nagri Letter Bo
    case 0xA81C: return 'b'; // Syloti Nagri Letter Bho
    case 0xA81E: return 'r'; // Syloti Nagri Letter Ro
    case 0xA820: return 'r'; // Syloti Nagri Letter Rro
    case 0xA821: return 's'; // Syloti Nagri Letter So
    case 0xA822: return 'h'; // Syloti Nagri Letter Ho
    case 0xA826: return 'e'; // Syloti Nagri Vowel Sign E
    case 0xA827: return 'o'; // Syloti Nagri Vowel Sign Oo
    case 0xA830: return '0'; // North Indic Fraction One Quarter
    case 0xA831: return '1'; // North Indic Fraction One Half
    case 0xA832: return '2'; // North Indic Fraction Three Quarters
    case 0xA833: return '3'; // North Indic Fraction One Sixteenth
    case 0xA834: return '4'; // North Indic Fraction One Eighth
    case 0xA835: return '5'; // North Indic Fraction Three Sixteenths
    case 0xA836: return '6'; // North Indic Quarter Mark
    case 0xA837: return '7'; // North Indic Placeholder Mark
    case 0xA838: return '8'; // North Indic Rupee Mark
    case 0xA839: return '9'; // North Indic Quantity Mark
    case 0xABC0: return 'k'; // Meetei Mayek Letter Kok
    case 0xABC1: return 's'; // Meetei Mayek Letter Sam
    case 0xABC4: return 'p'; // Meetei Mayek Letter Pa
    case 0xABC5: return 'n'; // Meetei Mayek Letter Na
    case 0xABC6: return 'c'; // Meetei Mayek Letter Chil
    case 0xABC8: return 'k'; // Meetei Mayek Letter Khou
    case 0xABC9: return 'n'; // Meetei Mayek Letter Ngou
    case 0xABCA: return 't'; // Meetei Mayek Letter Thou
    case 0xABCB: return 'w'; // Meetei Mayek Letter Wai
    case 0xABCC: return 'y'; // Meetei Mayek Letter Yang
    case 0xABCD: return 'h'; // Meetei Mayek Letter Huk
    case 0xABCE: return 'u'; // Meetei Mayek Letter Un
    case 0xABD0: return 'p'; // Meetei Mayek Letter Pham
    case 0xABD2: return 'g'; // Meetei Mayek Letter Gok
    case 0xABD3: return 'j'; // Meetei Mayek Letter Jham
    case 0xABD6: return 'j'; // Meetei Mayek Letter Jil
    case 0xABD7: return 'd'; // Meetei Mayek Letter Dil
    case 0xABD8: return 'g'; // Meetei Mayek Letter Ghou
    case 0xABD9: return 'd'; // Meetei Mayek Letter Dhou
    case 0xABDA: return 'b'; // Meetei Mayek Letter Bham
    case 0xABDB: return 'k'; // Meetei Mayek Letter Kok Lonsum
    case 0xABDC: return 'l'; // Meetei Mayek Letter Lai Lonsum
    case 0xABDD: return 'm'; // Meetei Mayek Letter Mit Lonsum
    case 0xABDE: return 'p'; // Meetei Mayek Letter Pa Lonsum
    case 0xABE0: return 't'; // Meetei Mayek Letter Til Lonsum
    case 0xABE1: return 'n'; // Meetei Mayek Letter Ngou Lonsum
    case 0xABE2: return 'i'; // Meetei Mayek Letter I Lonsum
    case 0xABE6: return 'y'; // Meetei Mayek Vowel Sign Yenap
    case 0xABE7: return 's'; // Meetei Mayek Vowel Sign Sounap
    case 0xABEA: return 'n'; // Meetei Mayek Vowel Sign Nung
    case 0xABEB: return '|'; // Meetei Mayek Cheikhei
    case 0xABF0: return '0'; // Meetei Mayek Digit Zero
    case 0xABF1: return '1'; // Meetei Mayek Digit One
    case 0xABF2: return '2'; // Meetei Mayek Digit Two
    case 0xABF3: return '3'; // Meetei Mayek Digit Three
    case 0xABF4: return '4'; // Meetei Mayek Digit Four
    case 0xABF5: return '5'; // Meetei Mayek Digit Five
    case 0xABF6: return '6'; // Meetei Mayek Digit Six
    case 0xABF7: return '7'; // Meetei Mayek Digit Seven
    case 0xABF8: return '8'; // Meetei Mayek Digit Eight
    case 0xABF9: return '9'; // Meetei Mayek Digit Nine
    case 0xFEFF: return 0; // Zero Width No-Break Space
    case 0xFF01: return '!'; // Fullwidth Exclamation Mark
    case 0xFF02: return '"'; // Fullwidth Quotation Mark
    case 0xFF03: return '#'; // Fullwidth Number Sign
    case 0xFF04: return '$'; // Fullwidth Dollar Sign
    case 0xFF05: return '%'; // Fullwidth Percent Sign
    case 0xFF06: return '&'; // Fullwidth Ampersand
    case 0xFF07: return '\''; // Fullwidth Apostrophe
    case 0xFF08: return '('; // Fullwidth Left Parenthesis
    case 0xFF09: return ')'; // Fullwidth Right Parenthesis
    case 0xFF0A: return '*'; // Fullwidth Asterisk
    case 0xFF0B: return '+'; // Fullwidth Plus Sign
    case 0xFF0C: return ','; // Fullwidth Comma
    case 0xFF0D: return '-'; // Fullwidth Hyphen-Minus
    case 0xFF0E: return '.'; // Fullwidth Full Stop
    case 0xFF0F: return '/'; // Fullwidth Solidus
    case 0xFF10: return '0'; // Fullwidth Digit Zero
    case 0xFF11: return '1'; // Fullwidth Digit One
    case 0xFF12: return '2'; // Fullwidth Digit Two
    case 0xFF13: return '3'; // Fullwidth Digit Three
    case 0xFF14: return '4'; // Fullwidth Digit Four
    case 0xFF15: return '5'; // Fullwidth Digit Five
    case 0xFF16: return '6'; // Fullwidth Digit Six
    case 0xFF17: return '7'; // Fullwidth Digit Seven
    case 0xFF18: return '8'; // Fullwidth Digit Eight
    case 0xFF19: return '9'; // Fullwidth Digit Nine
    case 0xFF1A: return ':'; // Fullwidth Colon
    case 0xFF1B: return ';'; // Fullwidth Semicolon
    case 0xFF1C: return '<'; // Fullwidth Less-Than Sign
    case 0xFF1D: return '='; // Fullwidth Equals Sign
    case 0xFF1E: return '>'; // Fullwidth Greater-Than Sign
    case 0xFF1F: return '?'; // Fullwidth Question Mark
    case 0xFF20: return '@'; // Fullwidth Commercial At
    case 0xFF21: return 'A'; // Fullwidth Latin Capital Letter A
    case 0xFF22: return 'B'; // Fullwidth Latin Capital Letter B
    case 0xFF23: return 'C'; // Fullwidth Latin Capital Letter C
    case 0xFF24: return 'D'; // Fullwidth Latin Capital Letter D
    case 0xFF25: return 'E'; // Fullwidth Latin Capital Letter E
    case 0xFF26: return 'F'; // Fullwidth Latin Capital Letter F
    case 0xFF27: return 'G'; // Fullwidth Latin Capital Letter G
    case 0xFF28: return 'H'; // Fullwidth Latin Capital Letter H
    case 0xFF29: return 'I'; // Fullwidth Latin Capital Letter I
    case 0xFF2A: return 'J'; // Fullwidth Latin Capital Letter J
    case 0xFF2B: return 'K'; // Fullwidth Latin Capital Letter K
    case 0xFF2C: return 'L'; // Fullwidth Latin Capital Letter L
    case 0xFF2D: return 'M'; // Fullwidth Latin Capital Letter M
    case 0xFF2E: return 'N'; // Fullwidth Latin Capital Letter N
    case 0xFF2F: return 'O'; // Fullwidth Latin Capital Letter O
    case 0xFF30: return 'P'; // Fullwidth Latin Capital Letter P
    case 0xFF31: return 'Q'; // Fullwidth Latin Capital Letter Q
    case 0xFF32: return 'R'; // Fullwidth Latin Capital Letter R
    case 0xFF33: return 'S'; // Fullwidth Latin Capital Letter S
    case 0xFF34: return 'T'; // Fullwidth Latin Capital Letter T
    case 0xFF35: return 'U'; // Fullwidth Latin Capital Letter U
    case 0xFF36: return 'V'; // Fullwidth Latin Capital Letter V
    case 0xFF37: return 'W'; // Fullwidth Latin Capital Letter W
    case 0xFF38: return 'X'; // Fullwidth Latin Capital Letter X
    case 0xFF39: return 'Y'; // Fullwidth Latin Capital Letter Y
    case 0xFF3A: return 'Z'; // Fullwidth Latin Capital Letter Z
    case 0xFF3B: return '['; // Fullwidth Left Square Bracket
    case 0xFF3C: return '\\'; // Fullwidth Reverse Solidus
    case 0xFF3D: return ']'; // Fullwidth Right Square Bracket
    case 0xFF3E: return '^'; // Fullwidth Circumflex Accent
    case 0xFF3F: return '_'; // Fullwidth Low Line
    case 0xFF40: return '`'; // Fullwidth Grave Accent
    case 0xFF41: return 'a'; // Fullwidth Latin Small Letter A
    case 0xFF42: return 'b'; // Fullwidth Latin Small Letter B
    case 0xFF43: return 'c'; // Fullwidth Latin Small Letter C
    case 0xFF44: return 'd'; // Fullwidth Latin Small Letter D
    case 0xFF45: return 'e'; // Fullwidth Latin Small Letter E
    case 0xFF46: return 'f'; // Fullwidth Latin Small Letter F
    case 0xFF47: return 'g'; // Fullwidth Latin Small Letter G
    case 0xFF48: return 'h'; // Fullwidth Latin Small Letter H
    case 0xFF49: return 'i'; // Fullwidth Latin Small Letter I
    case 0xFF4A: return 'j'; // Fullwidth Latin Small Letter J
    case 0xFF4B: return 'k'; // Fullwidth Latin Small Letter K
    case 0xFF4C: return 'l'; // Fullwidth Latin Small Letter L
    case 0xFF4D: return 'm'; // Fullwidth Latin Small Letter M
    case 0xFF4E: return 'n'; // Fullwidth Latin Small Letter N
    case 0xFF4F: return 'o'; // Fullwidth Latin Small Letter O
    case 0xFF50: return 'p'; // Fullwidth Latin Small Letter P
    case 0xFF51: return 'q'; // Fullwidth Latin Small Letter Q
    case 0xFF52: return 'r'; // Fullwidth Latin Small Letter R
    case 0xFF53: return 's'; // Fullwidth Latin Small Letter S
    case 0xFF54: return 't'; // Fullwidth Latin Small Letter T
    case 0xFF55: return 'u'; // Fullwidth Latin Small Letter U
    case 0xFF56: return 'v'; // Fullwidth Latin Small Letter V
    case 0xFF57: return 'w'; // Fullwidth Latin Small Letter W
    case 0xFF58: return 'x'; // Fullwidth Latin Small Letter X
    case 0xFF59: return 'y'; // Fullwidth Latin Small Letter Y
    case 0xFF5A: return 'z'; // Fullwidth Latin Small Letter Z
    case 0xFF5B: return '{'; // Fullwidth Left Curly Bracket
    case 0xFF5C: return '|'; // Fullwidth Vertical Line
    case 0xFF5D: return '}'; // Fullwidth Right Curly Bracket
    case 0xFF5E: return '~'; // Fullwidth Tilde
    case 0xFFE5: return 'Y'; // Fullwidth Yen Sign
    case 0xFFE6: return 'W'; // Fullwidth Won Sign
    }
    // Remaining astral chars (emoji, untabled historic scripts): DROP, never
    // '?'. An invisible char beats tofu — and the census proves every astral
    // char on real pages is either Gothic (above) or math (folded above).
    if (cp >= 0x10000) return 0;
    return '?';
}

// Display decoding for non-HTML text (editor): Unicode codepoint to the
// one-byte render slot the browser uses, so text files show the same
// glyphs in both. See html_map_cp for the table.
char text_slot_for_codepoint(int cp) {
    int m = html_map_cp(cp);
    return m ? (char)m : ' '; // dropped combining marks show as a space
}
