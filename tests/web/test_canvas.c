// Draws a test scene with the software canvas and writes it as a PNG (look at it, or compare it with a
// browser's rendering of tests/web/js/canvas_scene.html):  t_canvas out.png
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../src/web/wcanvas.h"
#include "../../src/web/wcommon.h"

static void rect(double* p, double x, double y, double w, double h) {
    double r[] = { 0, x, y, 1, x + w, y, 1, x + w, y + h, 1, x, y + h, 4 };
    memcpy(p, r, sizeof r);
}
#define SOLID(c) { .kind = WCV_SOLID, .color = (c), .inv = {1, 0, 0, 1, 0, 0} }

int main(int argc, char** argv) {
    struct wcv* c = wcv_new(400, 300);
    struct wcv_comp over = { WCV_SOURCE_OVER, 1.0, { 0, 0, 0, 0 } };
    struct wcv_paint white = SOLID(0xFFFFFFFFu);
    double p[64];
    rect(p, 0, 0, 400, 300);
    wcv_fill(c, p, 13, 0, &white, &over, 0);
    // red rect, blue translucent rect over it
    struct wcv_paint red = SOLID(0xFFCC2222u), blue = SOLID(0x800000FFu);
    rect(p, 20, 20, 100, 60);
    wcv_fill(c, p, 13, 0, &red, &over, 0);
    rect(p, 70, 40, 100, 60);
    wcv_fill(c, p, 13, 0, &blue, &over, 0);
    // circle via cubics, stroked and filled
    double k = 0.5522847498, cx = 250, cy = 70, r = 40;
    double circ[] = { 0, cx + r, cy, 3, cx + r, cy + r * k, cx + r * k, cy + r, cx, cy + r, 3, cx - r * k, cy + r, cx - r, cy + r * k, cx - r, cy,
                      3, cx - r, cy - r * k, cx - r * k, cy - r, cx, cy - r, 3, cx + r * k, cy - r, cx + r, cy - r * k, cx + r, cy, 4 };
    struct wcv_paint green = SOLID(0xFF22AA44u), black = SOLID(0xFF000000u);
    wcv_fill(c, circ, (int)(sizeof circ / sizeof *circ), 0, &green, &over, 0);
    struct wcv_stroke st = { 6, 0, 0, 10, 0, 0, 0 };
    wcv_stroke(c, circ, (int)(sizeof circ / sizeof *circ), &st, &black, &over);
    // polyline with different joins and caps
    double zig[] = { 0, 30, 150, 1, 70, 120, 1, 110, 170, 1, 150, 130 };
    struct wcv_stroke s1 = { 14, 0, 0, 10, 0, 0, 0 };   // butt, miter
    wcv_stroke(c, zig, 12, &s1, &red, &over);
    double zig2[] = { 0, 30, 200, 1, 70, 170, 1, 110, 220, 1, 150, 180 };
    struct wcv_stroke s2 = { 14, 1, 1, 10, 0, 0, 0 };   // round, round
    wcv_stroke(c, zig2, 12, &s2, &blue, &over);
    double zig3[] = { 0, 30, 250, 1, 70, 220, 1, 110, 270, 1, 150, 230 };
    struct wcv_stroke s3 = { 14, 2, 2, 10, 0, 0, 0 };   // square, bevel
    wcv_stroke(c, zig3, 12, &s3, &green, &over);
    // dashed line
    double dl[] = { 0, 180, 150, 1, 380, 150 };
    double dashes[] = { 12, 6 };
    struct wcv_stroke s4 = { 4, 0, 0, 10, dashes, 2, 0 };
    wcv_stroke(c, dl, 6, &s4, &black, &over);
    // linear gradient rect
    struct wcv_stop stops[] = { { 0, 0xFFFF0000u }, { 0.5, 0xFFFFFF00u }, { 1, 0xFF0000FFu } };
    struct wcv_paint lg = { .kind = WCV_LINEAR, .g = {180, 0, 380, 0}, .stops = stops, .nstops = 3, .inv = {1, 0, 0, 1, 0, 0} };
    rect(p, 180, 170, 200, 40);
    wcv_fill(c, p, 13, 0, &lg, &over, 0);
    struct wcv_paint rg = { .kind = WCV_RADIAL, .g = {280, 250, 0, 280, 250, 40}, .stops = stops, .nstops = 3, .inv = {1, 0, 0, 1, 0, 0} };
    rect(p, 180, 220, 200, 70);
    wcv_fill(c, p, 13, 0, &rg, &over, 0);
    // text
    struct wcv_font f = { 0, 0, 0, 20 };
    int mw, mh, ox, oy;
    uint8_t* mask = wcv_text_mask("Hello canvas", 12, &f, 0, 0, &mw, &mh, &ox, &oy);
    if (mask) {
        struct wcv_mask m = { mask, mw, mh, {1, 0, 0, 1, (double)(ox - 190), (double)(oy - 130)}, 0 };
        rect(p, 190 - ox, 130 - oy, mw, mh);
        wcv_fill(c, p, 13, 0, &black, &over, &m);
        w_free(mask);
    }
    uint32_t len;
    uint8_t* png = wcv_png(c, &len);
    FILE* fo = fopen(argc > 1 ? argv[1] : "canvas.png", "wb");
    fwrite(png, 1, len, fo);
    fclose(fo);
    printf("wrote %u bytes\n", len);
    return 0;
}
