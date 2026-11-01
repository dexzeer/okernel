#include "css_int.h"
#include "colortab.h"

// ---- small scanners -----------------------------------------------------------

void cv_trim(const char** s, int* len) {
    while (*len > 0 && w_isspace((unsigned char)**s)) { (*s)++; (*len)--; }
    while (*len > 0 && w_isspace((unsigned char)(*s)[*len - 1])) (*len)--;
}

int cv_ident_eq(const char* s, int len, const char* lit) { return w_ieq(s, len, lit); }

// Next whitespace-separated component, keeping (...) groups and quoted
// strings together. A '/' is its own component. Returns 0 at the end.
int cv_next(const char* s, int len, int* pos, const char** cs, int* clen) {
    int i = *pos;
    while (i < len && w_isspace((unsigned char)s[i])) i++;
    if (i >= len) { *pos = i; return 0; }
    int st = i;
    if (s[i] == '/' || s[i] == ',') { *cs = s + i; *clen = 1; *pos = i + 1; return 1; }
    int depth = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c;
            i++;
            while (i < len && s[i] != q) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (depth == 0 && (w_isspace((unsigned char)c) || c == '/' || c == ',')) break;
        i++;
    }
    if (i > len) i = len;
    *cs = s + st;
    *clen = i - st;
    *pos = i;
    return 1;
}

int cv_next_comma(const char* s, int len, int* pos, const char** cs, int* clen) {
    int i = *pos;
    if (i > len) return 0;
    if (i == len) { *pos = len + 1; if (len == 0) return 0; *cs = s + len; *clen = 0; return 0; }
    int st = i, depth = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c; i++;
            while (i < len && s[i] != q) { if (s[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        else if (c == ',' && depth == 0) break;
        i++;
    }
    if (i > len) i = len;
    *cs = s + st;
    *clen = i - st;
    cv_trim(cs, clen);
    *pos = i + 1;
    return 1;
}

// Parse a CSS number at s (optional sign, digits, fraction, exponent) into
// value*1000. Returns 1 and the bytes consumed.
int cv_number(const char* s, int len, int32_t* milli, int* used) {
    int i = 0, neg = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    int32_t ip = 0;
    int digits = 0;
    while (i < len && w_isdigit((unsigned char)s[i])) {
        if (ip < 2000000) ip = ip * 10 + (s[i] - '0');
        i++; digits++;
    }
    int32_t frac = 0, fscale = 1;
    if (i < len && s[i] == '.' && i + 1 < len && w_isdigit((unsigned char)s[i + 1])) {
        i++;
        while (i < len && w_isdigit((unsigned char)s[i])) {
            if (fscale < 1000000) { frac = frac * 10 + (s[i] - '0'); fscale *= 10; }
            i++; digits++;
        }
    }
    if (!digits) return 0;
    if (ip > 2000000) ip = 2000000;
    int32_t v = ip * 1000 + (frac * 1000 + fscale / 2) / fscale;
    // exponent (only when followed by digits, so "1em" is not an exponent)
    if (i + 1 < len && (s[i] == 'e' || s[i] == 'E') &&
        (w_isdigit((unsigned char)s[i + 1]) ||
         ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < len && w_isdigit((unsigned char)s[i + 2])))) {
        int j = i + 1, eneg = 0, e = 0;
        if (s[j] == '-' || s[j] == '+') { eneg = s[j] == '-'; j++; }
        while (j < len && w_isdigit((unsigned char)s[j])) { if (e < 40) e = e * 10 + (s[j] - '0'); j++; }
        while (e-- > 0) {
            if (eneg) v /= 10;
            else if (v < 200000000) v *= 10;
            else v = 0x7FFFFFFF;
        }
        i = j;
    }
    *milli = neg ? -v : v;
    *used = i;
    return 1;
}

static const char* const UNITS[] = {
    "px", "em", "rem", "vw", "vh", "vmin", "vmax", "ex", "ch", "pt", "pc", "in", "cm",
    "mm", "q", "lh", "deg", "rad", "turn", "grad", "fr", "s", "ms", "dppx", "x", 0
};

int cv_unit(const char* s, int len) {
    for (int i = 0; UNITS[i]; i++) if (w_ieq(s, len, UNITS[i])) return i;
    // dynamic/small/large viewport units behave like vw/vh here
    if (w_ieq(s, len, "dvh") || w_ieq(s, len, "svh") || w_ieq(s, len, "lvh")) return U_VH;
    if (w_ieq(s, len, "dvw") || w_ieq(s, len, "svw") || w_ieq(s, len, "lvw")) return U_VW;
    if (w_ieq(s, len, "cqw") || w_ieq(s, len, "cqi")) return U_VW;
    if (w_ieq(s, len, "cqh") || w_ieq(s, len, "cqb")) return U_VH;
    return -1;
}

// ---- colors ---------------------------------------------------------------------

struct ncolor { const char* n; uint32_t rgb; };
static const struct ncolor NAMED[] = {
    {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},{"aquamarine",0x7FFFD4},
    {"azure",0xF0FFFF},{"beige",0xF5F5DC},{"bisque",0xFFE4C4},{"black",0x000000},
    {"blanchedalmond",0xFFEBCD},{"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},
    {"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},{"chocolate",0xD2691E},
    {"coral",0xFF7F50},{"cornflowerblue",0x6495ED},{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},
    {"cyan",0x00FFFF},{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},
    {"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},{"darkkhaki",0xBDB76B},
    {"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},{"darkorange",0xFF8C00},
    {"darkorchid",0x9932CC},{"darkred",0x8B0000},{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},
    {"darkslateblue",0x483D8B},{"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},
    {"darkturquoise",0x00CED1},{"darkviolet",0x9400D3},{"deeppink",0xFF1493},
    {"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},{"dodgerblue",0x1E90FF},
    {"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},{"fuchsia",0xFF00FF},
    {"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},{"gold",0xFFD700},{"goldenrod",0xDAA520},
    {"gray",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},{"grey",0x808080},
    {"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},{"indianred",0xCD5C5C},{"indigo",0x4B0082},
    {"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},
    {"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},
    {"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},
    {"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},{"lightsalmon",0xFFA07A},
    {"lightseagreen",0x20B2AA},{"lightskyblue",0x87CEFA},{"lightslategray",0x778899},
    {"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},{"lightyellow",0xFFFFE0},
    {"lime",0x00FF00},{"limegreen",0x32CD32},{"linen",0xFAF0E6},{"magenta",0xFF00FF},
    {"maroon",0x800000},{"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},
    {"mediumorchid",0xBA55D3},{"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},
    {"mediumslateblue",0x7B68EE},{"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},
    {"mediumvioletred",0xC71585},{"midnightblue",0x191970},{"mintcream",0xF5FFFA},
    {"mistyrose",0xFFE4E1},{"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},
    {"oldlace",0xFDF5E6},{"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},
    {"orangered",0xFF4500},{"orchid",0xDA70D6},{"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},
    {"paleturquoise",0xAFEEEE},{"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},
    {"peachpuff",0xFFDAB9},{"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},
    {"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xFF0000},
    {"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},{"saddlebrown",0x8B4513},{"salmon",0xFA8072},
    {"sandybrown",0xF4A460},{"seagreen",0x2E8B57},{"seashell",0xFFF5EE},{"sienna",0xA0522D},
    {"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},{"slategray",0x708090},
    {"slategrey",0x708090},{"snow",0xFFFAFA},{"springgreen",0x00FF7F},{"steelblue",0x4682B4},
    {"tan",0xD2B48C},{"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},
    {"turquoise",0x40E0D0},{"violet",0xEE82EE},{"wheat",0xF5DEB3},{"white",0xFFFFFF},
    {"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},{"yellowgreen",0x9ACD32},
    // CSS system colors (light scheme values, as Chrome)
    {"canvas",0xFFFFFF},{"canvastext",0x000000},{"linktext",0x0000EE},{"visitedtext",0x551A8B},
    {"activetext",0xFF0000},{"buttonface",0xEFEFEF},{"buttontext",0x000000},
    {"buttonborder",0x767676},{"field",0xFFFFFF},{"fieldtext",0x000000},{"highlight",0xB5D5FF},
    {"highlighttext",0x000000},{"graytext",0x6D6D6D},{"mark",0xFFFF00},{"marktext",0x000000},
    {"accentcolor",0x0075FF},{"accentcolortext",0xFFFFFF},{"window",0xFFFFFF},
    {"windowtext",0x000000},{"buttonhighlight",0xEFEFEF},{"buttonshadow",0x767676},
    // deprecated system colors: aliases of the ones above (CSS Color 4)
    {"activeborder",0x767676},{"activecaption",0xFFFFFF},{"appworkspace",0xFFFFFF},{"background",0xFFFFFF},
    {"captiontext",0x000000},{"inactiveborder",0x767676},{"inactivecaption",0xFFFFFF},{"inactivecaptiontext",0x6D6D6D},
    {"infobackground",0xFFFFFF},{"infotext",0x000000},{"menu",0xFFFFFF},{"menutext",0x000000},{"scrollbar",0xFFFFFF},
    {"threeddarkshadow",0x767676},{"threedface",0xEFEFEF},{"threedhighlight",0x767676},{"threedlightshadow",0x767676},
    {"threedshadow",0x767676},{"windowframe",0x767676},{"selecteditem",0xB5D5FF},{"selecteditemtext",0x000000},
    {0, 0}
};

static int clamp255(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Parse an rgb() channel: number (0-255) or percentage.
static int chan(const char* s, int len, int32_t* out) {
    int32_t m; int u;
    cv_trim(&s, &len);
    if (w_ieq(s, len, "none")) { *out = 0; return 1; }
    if (!cv_number(s, len, &m, &u)) return 0;
    if (u < len && s[u] == '%') { *out = w_div64((int64_t)m * 255 + 50000, 100000); return 1; }
    if (u != len) return 0;
    *out = (m + 500) / 1000;
    return 1;
}

// alpha: number 0..1 or percentage -> 0..255
static int alpha_of(const char* s, int len, int* a) {
    int32_t m; int u;
    cv_trim(&s, &len);
    if (w_ieq(s, len, "none")) { *a = 0; return 1; }
    if (!cv_number(s, len, &m, &u)) return 0;
    if (u < len && s[u] == '%') m /= 100;
    *a = clamp255((m * 255 + 500) / 1000);
    return 1;
}

// Split function arguments by commas or spaces (+ "/ alpha").
static int fn_args(const char* s, int len, const char** a, int* al, int max, int* slash_at) {
    int n = 0, pos = 0;
    *slash_at = -1;
    int has_comma = 0;
    for (int i = 0; i < len; i++) if (s[i] == ',') has_comma = 1;
    const char* c; int cl;
    if (has_comma) {
        while (n < max && cv_next_comma(s, len, &pos, &c, &cl)) { a[n] = c; al[n] = cl; n++; }
        // "rgb(1, 2, 3 / 0.5)" mixed form: split the last arg on '/'
        return n;
    }
    while (n < max && cv_next(s, len, &pos, &c, &cl)) {
        if (cl == 1 && c[0] == '/') { *slash_at = n; continue; }
        a[n] = c; al[n] = cl; n++;
    }
    return n;
}

static uint32_t hsl_rgb(int32_t h_milli, int32_t s_milli, int32_t l_milli) {
    // h in degrees*1000, s,l in 0..1000
    int32_t h = h_milli % 360000; if (h < 0) h += 360000;
    int32_t dl = 2 * l_milli - 1000; if (dl < 0) dl = -dl;
    int32_t c = (1000 - dl) * s_milli / 1000;
    int32_t hp = h / 60; // 0..5999
    int32_t m2 = hp % 2000 - 1000; if (m2 < 0) m2 = -m2;
    int32_t x = c * (1000 - m2) / 1000;
    int32_t r = 0, g = 0, b = 0;
    switch (hp / 1000) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    int32_t m = l_milli - c / 2;
    return ((uint32_t)clamp255(((r + m) * 255 + 500) / 1000) << 16) |
           ((uint32_t)clamp255(((g + m) * 255 + 500) / 1000) << 8) |
           (uint32_t)clamp255(((b + m) * 255 + 500) / 1000);
}

static int32_t hue_milli(const char* s, int len) {
    int32_t m; int u;
    cv_trim(&s, &len);
    if (!cv_number(s, len, &m, &u)) return 0;
    int unit = u < len ? cv_unit(s + u, len - u) : -1;
    if (unit == U_RAD) m = w_div64((int64_t)m * 57296, 1000);
    else if (unit == U_TURN) m = m * 360;
    else if (unit == U_GRAD) m = m * 9 / 10;
    return m;
}

// OKLab (L 0..1, a/b ~ -0.4..0.4, all *1e6 fixed) -> sRGB
static uint32_t oklab_rgb(int64_t L, int64_t A, int64_t B) {
#define DV(x) ((int64_t)w_div64((x), 1000000))
    int64_t l_ = L + DV(396338 * A + 215804 * B);
    int64_t m_ = L + DV(-105561 * A - 63854 * B);
    int64_t s_ = L + DV(-89484 * A - 1291486 * B);
    int64_t l = DV(DV(l_ * l_) * l_);
    int64_t m = DV(DV(m_ * m_) * m_);
    int64_t s = DV(DV(s_ * s_) * s_);
    int64_t ch[3];
    ch[0] = DV(4076742 * l - 3307712 * m + 230970 * s);
    ch[1] = DV(-1268438 * l + 2609757 * m - 341319 * s);
    ch[2] = DV(-4196 * l - 703419 * m + 1707615 * s);
#undef DV
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        int32_t v = w_div64(ch[i] * 4095, 1000000);
        if (v < 0) v = 0;
        if (v > 4095) v = 4095;
        out = (out << 8) | lin2srgb[v];
    }
    return out;
}

// ---- CSS Color 4: color(), lab(), lch(), oklab(), oklch(), hwb() ---------------------------------------------------------
// Computed in double precision (the engine's other users of doubles are the canvas and wasm; kmath supplies pow/sin/cos).

extern double km_pow(double x, double y);
extern double km_sin(double x);
extern double km_cos(double x);

// a number with the digits kept exactly: integer part and fraction as one integer, divided once
static int dbl_exact(const char* s, int len, double* out) {
    int i = 0, neg = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    uint64_t ip = 0; int nd = 0, fd = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { if (nd < 18) ip = ip * 10 + (uint64_t)(s[i] - '0'); i++; nd++; }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') { if (nd + fd < 18) { ip = ip * 10 + (uint64_t)(s[i] - '0'); fd++; } i++; }
    }
    if (!nd && !fd) return 0;
    double v = (double)ip;
    if (fd) v /= km_pow(10.0, fd);
    if (i < len && (s[i] == 'e' || s[i] == 'E') && i + 1 < len && ((s[i + 1] >= '0' && s[i + 1] <= '9') || ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < len))) {
        int ex = 0, en = 0;
        i++;
        if (s[i] == '+' || s[i] == '-') { en = s[i] == '-'; i++; }
        while (i < len && s[i] >= '0' && s[i] <= '9') { ex = ex * 10 + (s[i] - '0'); i++; }
        v *= km_pow(10.0, en ? -ex : ex);
    }
    *out = neg ? -v : v;
    return i;
}

// one component: number, percentage (100% = pct_base), "none" (0); angles only when hue is set (degrees out)
static int col_comp(const char* s, int len, double pct_base, int hue, double* out) {
    cv_trim(&s, &len);
    if (w_ieq(s, len, "none")) { *out = 0; return 1; }
    double v;
    int n = dbl_exact(s, len, &v);
    if (!n) return 0;
    if (n < len && s[n] == '%' && n + 1 == len) { *out = hue ? v * 3.6 : v * pct_base / 100.0; return 1; }
    if (n == len) { *out = v; return 1; }
    if (!hue) return 0;
    const char* u = s + n; int ul = len - n;
    if (w_ieq(u, ul, "deg")) { *out = v; return 1; }
    if (w_ieq(u, ul, "rad")) { *out = v * 57.29577951308232; return 1; }
    if (w_ieq(u, ul, "grad")) { *out = v * 0.9; return 1; }
    if (w_ieq(u, ul, "turn")) { *out = v * 360.0; return 1; }
    return 0;
}

static double srgb_to_linear(double v) {
    double a = v < 0 ? -v : v;
    double r = a <= 0.04045 ? a / 12.92 : km_pow((a + 0.055) / 1.055, 2.4);
    return v < 0 ? -r : r;
}

static double linear_to_srgb(double l) {
    double a = l < 0 ? -l : l;
    double r = a > 0.0031308 ? 1.055 * km_pow(a, 1.0 / 2.4) - 0.055 : 12.92 * a;
    return l < 0 ? -r : r;
}

static void mat3(const double m[9], const double v[3], double o[3]) {
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static const double M_XYZ65_RGB[9] = { 3.2409699419045226, -1.537383177570094, -0.4986107602930034,
                                       -0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
                                       0.05563007969699366, -0.20397695888897652, 1.0569715142428786 };
static const double M_D50_D65[9] = { 0.9554734527042182, -0.023098536874261423, 0.0632593086610217,
                                     -0.028369706963208136, 1.0099954580058226, 0.021041398966943008,
                                     0.012314001688319899, -0.020507696433477, 1.3303659366080753 };
static const double M_P3_XYZ[9] = { 0.4865709486482162, 0.26566769316909306, 0.1982172852343625,
                                    0.2289745640697488, 0.6917385218365064, 0.079286914093745,
                                    0.0, 0.04511338185890264, 1.043944368900976 };
static const double M_A98_XYZ[9] = { 0.5766690429101305, 0.1855582379065463, 0.1882286462349947,
                                     0.29734497525053605, 0.6273635662554661, 0.0752914584939981,
                                     0.02703136138641234, 0.07068885253582723, 0.9913375368376388 };
static const double M_PRO_XYZ[9] = { 0.7977604896723027, 0.13518583717574031, 0.0313493495815248,
                                     0.2880711282292934, 0.7118432178101014, 0.00008565396060525902,
                                     0.0, 0.0, 0.8251046025104601 };
static const double M_REC2020_XYZ[9] = { 0.6369580483012914, 0.14461690358620832, 0.1688809751641721,
                                         0.2627002120112671, 0.6779980715188708, 0.05930171646986196,
                                         0.0, 0.028072693049087428, 1.0609850577107909 };
static const double M_SRGB_XYZ[9] = { 0.41239079926595934, 0.357584339383878, 0.1804807884018343,
                                      0.21263900587151027, 0.715168678767756, 0.07219231536073371,
                                      0.01933081871559182, 0.11919477979462598, 0.9505321522496607 };

static uint32_t col_pack(double r, double g, double b, int a8) {
    double c[3] = { r, g, b };
    uint32_t o = 0;
    for (int i = 0; i < 3; i++) {
        double v = c[i];
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        uint32_t q = (uint32_t)(v * 255.0 + 0.5);
        o = (o << 8) | q;
    }
    return ((uint32_t)a8 << 24) | o;
}

static uint32_t xyz65_pack(const double xyz[3], int a8) {
    double lin[3];
    mat3(M_XYZ65_RGB, xyz, lin);
    return col_pack(linear_to_srgb(lin[0]), linear_to_srgb(lin[1]), linear_to_srgb(lin[2]), a8);
}

static uint32_t xyz50_pack(const double xyz[3], int a8) {
    double x65[3];
    mat3(M_D50_D65, xyz, x65);
    return xyz65_pack(x65, a8);
}

static uint32_t lab_pack(double L, double a, double b, int a8) {
    const double eps = 216.0 / 24389.0, kap = 24389.0 / 27.0;
    double fy = (L + 16.0) / 116.0, fx = a / 500.0 + fy, fz = fy - b / 200.0;
    double fx3 = fx * fx * fx, fz3 = fz * fz * fz;
    double x = fx3 > eps ? fx3 : (116.0 * fx - 16.0) / kap;
    double y = L > kap * eps ? fy * fy * fy : L / kap;
    double z = fz3 > eps ? fz3 : (116.0 * fz - 16.0) / kap;
    double xyz[3] = { x * 0.3457 / 0.3585, y, z * (1.0 - 0.3457 - 0.3585) / 0.3585 };
    return xyz50_pack(xyz, a8);
}

static uint32_t oklab_pack(double L, double a, double b, int a8) {
    double l_ = L + 0.3963377773761749 * a + 0.2158037573099136 * b;
    double m_ = L - 0.1055613458156586 * a - 0.0638541728258133 * b;
    double s_ = L - 0.0894841775298119 * a - 1.2914855480194092 * b;
    double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    double r = 4.0767416621308 * l - 3.3077115913371 * m + 0.2309699292064 * s;
    double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return col_pack(linear_to_srgb(r), linear_to_srgb(g), linear_to_srgb(bl), a8);
}

// color(<space> a b c [/ alpha])
static int color_fn(const char* args, int alen, uint32_t* argb) {
    const char* a[8]; int al[8]; int slash;
    int n = fn_args(args, alen, a, al, 8, &slash);
    if (n < 4) return 0;
    double v[3];
    const char* sp = a[0]; int spl = al[0];
    cv_trim(&sp, &spl);
    int xyz_like = w_ieq(sp, spl, "xyz") || w_ieq(sp, spl, "xyz-d65") || w_ieq(sp, spl, "xyz-d50");
    for (int i = 0; i < 3; i++) if (!col_comp(a[i + 1], al[i + 1], 1.0, 0, &v[i])) return 0;
    (void)xyz_like;
    int a8 = 255;
    if (n >= 5 && !alpha_of(a[4], al[4], &a8)) return 0;
    double lin[3], xyz[3];
    if (w_ieq(sp, spl, "srgb")) { *argb = col_pack(v[0], v[1], v[2], a8); return 1; }
    if (w_ieq(sp, spl, "srgb-linear")) { *argb = col_pack(linear_to_srgb(v[0]), linear_to_srgb(v[1]), linear_to_srgb(v[2]), a8); return 1; }
    if (w_ieq(sp, spl, "display-p3")) {
        for (int i = 0; i < 3; i++) lin[i] = srgb_to_linear(v[i]);
        mat3(M_P3_XYZ, lin, xyz); *argb = xyz65_pack(xyz, a8); return 1;
    }
    if (w_ieq(sp, spl, "a98-rgb")) {
        for (int i = 0; i < 3; i++) { double t = v[i] < 0 ? -v[i] : v[i]; lin[i] = (v[i] < 0 ? -1.0 : 1.0) * km_pow(t, 563.0 / 256.0); }
        mat3(M_A98_XYZ, lin, xyz); *argb = xyz65_pack(xyz, a8); return 1;
    }
    if (w_ieq(sp, spl, "prophoto-rgb")) {
        for (int i = 0; i < 3; i++) {
            double t = v[i] < 0 ? -v[i] : v[i];
            lin[i] = (v[i] < 0 ? -1.0 : 1.0) * (t <= 16.0 / 512.0 ? t / 16.0 : km_pow(t, 1.8));
        }
        mat3(M_PRO_XYZ, lin, xyz); *argb = xyz50_pack(xyz, a8); return 1;
    }
    if (w_ieq(sp, spl, "rec2020")) {
        const double alpha = 1.09929682680944, beta = 0.018053968510807;
        for (int i = 0; i < 3; i++) {
            double t = v[i] < 0 ? -v[i] : v[i];
            lin[i] = (v[i] < 0 ? -1.0 : 1.0) * (t < beta * 4.5 ? t / 4.5 : km_pow((t + alpha - 1.0) / alpha, 1.0 / 0.45));
        }
        mat3(M_REC2020_XYZ, lin, xyz); *argb = xyz65_pack(xyz, a8); return 1;
    }
    if (w_ieq(sp, spl, "xyz") || w_ieq(sp, spl, "xyz-d65")) { *argb = xyz65_pack(v, a8); return 1; }
    if (w_ieq(sp, spl, "xyz-d50")) { *argb = xyz50_pack(v, a8); return 1; }
    return 0;
}

// lab() lch() oklab() oklch() hwb(); 0 when fn is none of them or the arguments are wrong
static int modern_fn(const char* fn, int fl, const char* args, int alen, uint32_t* argb, int* handled) {
    int is_lab = w_ieq(fn, fl, "lab"), is_lch = w_ieq(fn, fl, "lch"), is_oklab = w_ieq(fn, fl, "oklab"),
        is_oklch = w_ieq(fn, fl, "oklch"), is_hwb = w_ieq(fn, fl, "hwb"), is_color = w_ieq(fn, fl, "color");
    *handled = is_lab || is_lch || is_oklab || is_oklch || is_hwb || is_color;
    if (!*handled) return 0;
    if (is_color) return color_fn(args, alen, argb);
    const char* a[6]; int al[6]; int slash;
    int n = fn_args(args, alen, a, al, 6, &slash);
    if (n < 3) return 0;
    int a8 = 255;
    if (n >= 4 && !alpha_of(a[3], al[3], &a8)) return 0;
    double x, y, z;
    if (is_hwb) {
        double h, w, b;
        if (!col_comp(a[0], al[0], 360.0, 1, &h) || !col_comp(a[1], al[1], 100.0, 0, &w) || !col_comp(a[2], al[2], 100.0, 0, &b)) return 0;
        w /= 100.0; b /= 100.0;
        if (w < 0) w = 0;
        if (b < 0) b = 0;
        if (w + b >= 1.0) { double g = w / (w + b); *argb = col_pack(g, g, g, a8); return 1; }
        double hh = h - 360.0 * (double)(int)(h / 360.0);
        if (hh < 0) hh += 360.0;
        double h6 = hh / 60.0;
        int sector = (int)h6;
        double f = h6 - (double)sector, r0, g0, b0;
        switch (sector) {
        case 0: r0 = 1; g0 = f; b0 = 0; break;
        case 1: r0 = 1 - f; g0 = 1; b0 = 0; break;
        case 2: r0 = 0; g0 = 1; b0 = f; break;
        case 3: r0 = 0; g0 = 1 - f; b0 = 1; break;
        case 4: r0 = f; g0 = 0; b0 = 1; break;
        default: r0 = 1; g0 = 0; b0 = 1 - f; break;
        }
        double sc = 1.0 - w - b;
        *argb = col_pack(r0 * sc + w, g0 * sc + w, b0 * sc + w, a8);
        return 1;
    }
    double lpct = (is_lab || is_lch) ? 100.0 : 1.0;
    double abase = is_lab ? 125.0 : is_oklab ? 0.4 : 0.0;
    double cbase = is_lch ? 150.0 : 0.4;
    if (!col_comp(a[0], al[0], lpct, 0, &x)) return 0;
    if (is_lab || is_oklab) {
        if (!col_comp(a[1], al[1], abase, 0, &y) || !col_comp(a[2], al[2], abase, 0, &z)) return 0;
    } else {
        double c, h;
        if (!col_comp(a[1], al[1], cbase, 0, &c) || !col_comp(a[2], al[2], 360.0, 1, &h)) return 0;
        if (c < 0) c = 0;
        double rad = h * 0.017453292519943295;
        y = c * km_cos(rad);
        z = c * km_sin(rad);
    }
    if (is_lab || is_lch) {
        if (x < 0) x = 0;
        if (x > 100) x = 100;
        *argb = lab_pack(x, y, z, a8);
    } else {
        if (x < 0) x = 0;
        if (x > 1) x = 1;
        *argb = oklab_pack(x, y, z, a8);
    }
    return 1;
}

int cv_color(const char* s, int len, uint32_t* argb, int* current) {
    cv_trim(&s, &len);
    if (current) *current = 0;
    if (len <= 0) return 0;
    if (s[0] == '#') {
        int n = len - 1;
        const char* h = s + 1;
        for (int i = 0; i < n; i++) if (!w_ishex((unsigned char)h[i])) return 0;
        uint32_t r, g, b, a = 255;
        if (n == 3 || n == 4) {
            r = w_hexval(h[0]) * 17; g = w_hexval(h[1]) * 17; b = w_hexval(h[2]) * 17;
            if (n == 4) a = w_hexval(h[3]) * 17;
        } else if (n == 6 || n == 8) {
            r = w_hexval(h[0]) * 16 + w_hexval(h[1]);
            g = w_hexval(h[2]) * 16 + w_hexval(h[3]);
            b = w_hexval(h[4]) * 16 + w_hexval(h[5]);
            if (n == 8) a = w_hexval(h[6]) * 16 + w_hexval(h[7]);
        } else return 0;
        *argb = (a << 24) | (r << 16) | (g << 8) | b;
        return 1;
    }
    // functions
    int p = 0;
    while (p < len && s[p] != '(') p++;
    if (p < len && s[len - 1] == ')') {
        const char* fn = s;
        int fl = p;
        const char* args = s + p + 1;
        int alen = len - p - 2;
        const char* a[6]; int al[6]; int slash;
        if (w_ieq(fn, fl, "rgb") || w_ieq(fn, fl, "rgba")) {
            int n = fn_args(args, alen, a, al, 6, &slash);
            if (n < 3) return 0;
            int32_t r, g, b; int al8 = 255;
            if (!chan(a[0], al[0], &r) || !chan(a[1], al[1], &g) || !chan(a[2], al[2], &b)) return 0;
            if (n >= 4 && !alpha_of(a[3], al[3], &al8)) return 0;
            *argb = ((uint32_t)al8 << 24) | ((uint32_t)clamp255(r) << 16) |
                    ((uint32_t)clamp255(g) << 8) | (uint32_t)clamp255(b);
            return 1;
        }
        if (w_ieq(fn, fl, "hsl") || w_ieq(fn, fl, "hsla")) {
            int n = fn_args(args, alen, a, al, 6, &slash);
            if (n < 3) return 0;
            int32_t h = hue_milli(a[0], al[0]), sm, lm, u;
            const char* x = a[1]; int xl = al[1]; cv_trim(&x, &xl);
            if (!cv_number(x, xl, &sm, &u)) return 0;
            x = a[2]; xl = al[2]; cv_trim(&x, &xl);
            if (!cv_number(x, xl, &lm, &u)) return 0;
            int al8 = 255;
            if (n >= 4 && !alpha_of(a[3], al[3], &al8)) return 0;
            *argb = ((uint32_t)al8 << 24) | hsl_rgb(h, W_CLAMP(sm / 100, 0, 1000), W_CLAMP(lm / 100, 0, 1000));
            return 1;
        }
        {
            int handled = 0;
            int ok = modern_fn(fn, fl, args, alen, argb, &handled);
            if (handled) return ok;
        }
        if (w_ieq(fn, fl, "light-dark")) {
            int pos = 0; const char* c; int cl;
            if (!cv_next_comma(args, alen, &pos, &c, &cl)) return 0;
            return cv_color(c, cl, argb, current);
        }
        if (w_ieq(fn, fl, "color-mix")) {
            // color-mix(in srgb, A p%, B): linear mix in sRGB
            int pos = 0; const char* c; int cl;
            const char* parts[3]; int pl[3]; int n = 0;
            while (n < 3 && cv_next_comma(args, alen, &pos, &c, &cl)) { parts[n] = c; pl[n] = cl; n++; }
            if (n != 3) return 0;
            uint32_t col[2]; int32_t pct[2] = { -1, -1 };
            for (int k = 0; k < 2; k++) {
                const char* q = parts[k + 1]; int ql = pl[k + 1];
                int pp = 0; const char* tk; int tl;
                const char* colstr = 0; int colen = 0;
                while (cv_next(q, ql, &pp, &tk, &tl)) {
                    int32_t m; int u;
                    if (tl > 1 && tk[tl - 1] == '%' && cv_number(tk, tl - 1, &m, &u) && u == tl - 1) pct[k] = m;
                    else { colstr = tk; colen = tl; }
                }
                int cur2;
                if (!colstr || !cv_color(colstr, colen, &col[k], &cur2)) return 0;
            }
            if (pct[0] < 0 && pct[1] < 0) pct[0] = 50000;
            if (pct[0] < 0) pct[0] = 100000 - pct[1];
            int32_t w0 = pct[0] / 100; // 0..1000
            uint32_t r = 0;
            for (int sh = 24; sh >= 0; sh -= 8) {
                int32_t c0 = (col[0] >> sh) & 255, c1 = (col[1] >> sh) & 255;
                r |= (uint32_t)((c0 * w0 + c1 * (1000 - w0) + 500) / 1000) << sh;
            }
            *argb = r;
            return 1;
        }
        if (w_ieq(fn, fl, "var") || w_ieq(fn, fl, "env")) return 0;
        return 0;
    }
    if (w_ieq(s, len, "transparent")) { *argb = 0; return 1; }
    if (w_ieq(s, len, "currentcolor")) { *argb = 0xFF000000; if (current) *current = 1; return 1; }
    for (int i = 0; NAMED[i].n; i++)
        if (w_ieq(s, len, NAMED[i].n)) { *argb = 0xFF000000u | NAMED[i].rgb; return 1; }
    return 0;
}

int css_parse_color(const char* s, int len, uint32_t* argb) {
    int cur;
    return cv_color(s, len, argb, &cur) && !cur;
}
