// Writes terminal escape sequences with a known result, then asks for the cell grid on the serial log (OSC 7777).
// tests/headless/test_lx_term.py reads it.
#include "lxsys.h"

__attribute__((force_align_arg_pointer)) void _start(void) {
    puts_("\x1b[2J\x1b[H");
    puts_("abc\x1b[2;5Hdef\x1b[1;2Hz");                          // row 0 "azc", row 1 "    def"
    puts_("\x1b[3;1Hhello\x1b[3;3H\x1b[K");                      // row 2 "he"
    puts_("\x1b[4;1Habcdef\x1b[4;2H\x1b[2P");                    // row 3 "adef"
    puts_("\x1b[5;1Habcdef\x1b[5;2H\x1b[2@XY");                  // row 4 "aXYbcdef"
    puts_("\x1b[6;1HAAA\x1b[7;1HBBB\x1b[8;1HCCC\x1b[9;1Hkeep");  // a region of rows 5-7, row 8 below it
    puts_("\x1b[6;8r\x1b[8;1H\n\x1b[r");                         // one scroll inside: BBB, CCC, blank; keep stays
    puts_("\x1b[10;1Hmain\x1b[?1049h\x1b[2J\x1b[Halt screen\x1b[?1049l");   // row 9 "main", the cursor back after it
    puts_("!");                                                  // row 9 "main!"
    puts_("\x1b[11;999Hxy");                                     // x in the last column, y wraps to row 11
    puts_("\x1b[13;1H\x1b[31;1mred\x1b[0m \x1b[7minv\x1b[27m \x1b[38;5;82m256\x1b[0m"); // row 12 "red inv 256"
    puts_("\x1b[14;1Habc\x1b[1D\x1b[1A\x1b[1B\x1b[2C!");         // row 13 "ab" + "c" + one blank + "!": "abc !"
    puts_("\x1b[15;1Hgone\r\x1b[2Kback");                        // row 14 "back"
    puts_("\xe2\x94\x80\xe2\x94\x82\x1b(B\x1b=\x1b>");           // row 14 ends "-|": line drawing, charset and keypad modes
    puts_("\x1b]7777\x07");
    sys_exit(0);
}
