// Predefined CSS counter styles (see cstyles.c / cstyles_data.h)
#ifndef CSTYLES_H
#define CSTYLES_H

int cstyle_find(const char* s, int len);
int cstyle_format(int idx, int value, char* out);

#endif
