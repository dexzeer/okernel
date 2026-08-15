#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>

#define EDITOR_MAX_LINES 256
#define EDITOR_LINE_LEN 128
#define EDITOR_MAX_TEXT (EDITOR_MAX_LINES * EDITOR_LINE_LEN)

struct editor {
    int win_id;
    char filename[32];
    char text[EDITOR_MAX_TEXT];
    int text_len;
    int cursor_pos;
    int scroll_y;
    int dirty;
    int modified;
};

void editor_init(void);
int editor_open(const char* filename);
void editor_close(int ed_id);
void editor_handle_key(int ed_id, char c);
void editor_draw(int ed_id);
int editor_get_count(void);
struct editor* editor_get(int ed_id);
int editor_find_by_win(int win_id);

#endif
