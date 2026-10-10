#ifndef WEB_WCANVAS_H
#define WEB_WCANVAS_H

#include <stdint.h>

// Software 2D canvas (the engine behind CanvasRenderingContext2D): a straight-alpha ARGB bitmap
// with anti-aliased path fill and stroke (caps, joins, miter limit, dashes), linear / radial /
// conic gradients, patterns and images through the affine paint matrix, a clip mask, the Porter-Duff
// and blend-mode composite operators, shadows, text, ImageData access and a PNG encoder.
//
// Geometry is in device pixels as doubles (the script side applies the current transform when it
// builds a path). A path is an array of doubles:
//     0 x y                      moveTo
//     1 x y                      lineTo
//     2 cx cy x y                quadraticCurveTo
//     3 c1x c1y c2x c2y x y      bezierCurveTo
//     4                          closePath
// Matrices are (a b c d e f): x' = a x + c y + e, y' = b x + d y + f.
//
// Unlike the layout engine this uses floating point: it is only called from script natives (the
// realm has the FPU set up) and from host tests.

struct wcv;

enum { WCV_SOLID = 0, WCV_LINEAR, WCV_RADIAL, WCV_CONIC, WCV_IMAGE };

struct wcv_stop { double t; uint32_t argb; };           // straight ARGB

struct wcv_paint {
    int kind;
    uint32_t color;                                      // WCV_SOLID: straight ARGB
    double g[6];                                         // linear: x0 y0 x1 y1; radial: x0 y0 r0 x1 y1 r1; conic: angle cx cy
    const struct wcv_stop* stops;
    int nstops;
    double inv[6];                                       // device -> paint space (gradient / image) affine
    // WCV_IMAGE: pixels (straight ARGB), the source rectangle, how it extends past it
    const uint32_t* img;
    int stride;                                          // pixels per row of img
    int sx, sy, sw, sh;
    int repeat_x, repeat_y;                              // pattern repetition; neither = outside is transparent
    int clamp;                                           // drawImage: outside the rect samples the edge
    int smooth;                                          // bilinear (else nearest)
};

// Optional 8-bit coverage multiplied into a fill (text): mask pixel (u,v) = inv * device point
struct wcv_mask {
    const uint8_t* m;
    int w, h;
    double inv[6];
    int smooth;
};

enum {
    WCV_SOURCE_OVER = 0, WCV_SOURCE_IN, WCV_SOURCE_OUT, WCV_SOURCE_ATOP,
    WCV_DEST_OVER, WCV_DEST_IN, WCV_DEST_OUT, WCV_DEST_ATOP,
    WCV_LIGHTER, WCV_COPY, WCV_XOR,
    WCV_MULTIPLY, WCV_SCREEN, WCV_OVERLAY, WCV_DARKEN, WCV_LIGHTEN, WCV_COLOR_DODGE, WCV_COLOR_BURN,
    WCV_HARD_LIGHT, WCV_SOFT_LIGHT, WCV_DIFFERENCE, WCV_EXCLUSION,
    WCV_HUE, WCV_SATURATION, WCV_COLOR, WCV_LUMINOSITY,
    WCV_CLEAR,                                           // internal: clearRect
    WCV_NOPS
};

struct wcv_stroke {
    double width;                                        // device pixels
    int cap;                                             // 0 butt, 1 round, 2 square
    int join;                                            // 0 miter, 1 round, 2 bevel
    double miter_limit;
    const double* dash;                                  // device pixels
    int ndash;                                           // even count (the script repeats an odd pattern)
    double dash_offset;
};

struct wcv_shadow {
    uint32_t color;                                      // straight ARGB (alpha 0 = no shadow)
    double blur;                                         // shadowBlur (sigma = blur / 2)
    double dx, dy;                                       // device pixels
};

struct wcv_comp {
    int op;
    double alpha;                                        // globalAlpha
    struct wcv_shadow shadow;
};

#define WCV_MAX_PIXELS (8 * 1024 * 1024)
#define WCV_MAX_DIM 16384

struct wcv* wcv_new(int w, int h);                       // transparent black; NULL if too large / no memory
void wcv_free(struct wcv* c);
int  wcv_resize(struct wcv* c, int w, int h);            // clears; also resets the clip stack
int  wcv_width(const struct wcv* c);
int  wcv_height(const struct wcv* c);
uint32_t* wcv_pixels(struct wcv* c);                     // straight ARGB, width x height
unsigned wcv_serial(const struct wcv* c);                // changes with every modification
void wcv_touch(struct wcv* c);

void wcv_fill(struct wcv* c, const double* path, int n, int evenodd, const struct wcv_paint* p,
              const struct wcv_comp* comp, const struct wcv_mask* mask);
void wcv_stroke(struct wcv* c, const double* path, int n, const struct wcv_stroke* st, const struct wcv_paint* p,
                const struct wcv_comp* comp);
// Intersect the clip with a path.
void wcv_clip(struct wcv* c, const double* path, int n, int evenodd);
void wcv_save(struct wcv* c);                            // clip stack
void wcv_restore(struct wcv* c);
void wcv_reset_clip(struct wcv* c);

// Raw pixel access (straight RGBA bytes, no compositing, no clip)
void wcv_get_image(const struct wcv* c, int x, int y, int w, int h, uint8_t* rgba);
void wcv_put_image(struct wcv* c, int x, int y, int w, int h, const uint8_t* rgba, int dx, int dy, int dw, int dh);

int  wcv_point_in_path(const double* path, int n, double x, double y, int evenodd);
int  wcv_point_in_stroke(const double* path, int n, double x, double y, const struct wcv_stroke* st);

// PNG file of the canvas (malloc'd with the engine allocator; free with w_free). NULL on failure.
uint8_t* wcv_png(const struct wcv* c, uint32_t* len);

// Text: a coverage mask of `str` drawn with the engine's fonts. The origin of the text (start of
// the baseline) is mask pixel (ox, oy) plus the fraction (fx, fy) in [0, 1). Returns the mask (w_free) or NULL.
struct wcv_font { int family; int bold; int italic; double px; };
uint8_t* wcv_text_mask(const char* str, int len, const struct wcv_font* f, double fx, double fy, int* w, int* h, int* ox, int* oy);
double wcv_text_width(const char* str, int len, const struct wcv_font* f);
void wcv_font_metrics(const struct wcv_font* f, double* ascent, double* descent);

#endif
