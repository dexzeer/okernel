// Unicode Bidirectional Algorithm (UAX #9) for the inline layout engine.
// The caller feeds one paragraph's codepoints in logical order — including
// the formatting characters (LRM..PDI) and any synthetic element controls —
// and gets back resolved levels plus a control flag per codepoint.
#ifndef BIDI_H
#define BIDI_H
#include "bdata.h"

int      bdi_class(uint32_t cp);            // BCL_* (bdata.h)
uint32_t bdi_mirror(uint32_t cp);           // mirrored glyph, 0 = none
int      bdi_first_strong(const uint32_t* cps, int n, int def);  // P2/P3: 0 L, 1 R
// levels per codepoint; ctrl[i] = 1 for formatting characters (not displayed)
void     bidi_levels(const uint32_t* cps, int n, int base, uint8_t* lvl, uint8_t* ctrl);
// L2 reordering: order[] = logical index in visual order
int      bidi_reorder(const uint8_t* lvl, const uint8_t* ctrl, int n, int* order);

#endif
