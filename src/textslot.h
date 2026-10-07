#ifndef TEXTSLOT_H
#define TEXTSLOT_H

// Map a Unicode codepoint to a one-byte glyph slot of the bitmap fonts:
// ASCII passes through, Cyrillic / Latin-1 / common symbols map to their
// slots (0x80+), unmapped letters transliterate, the rest become '?' / ' '.
char text_slot_for_codepoint(int cp);

#endif
