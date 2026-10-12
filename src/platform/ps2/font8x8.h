#ifndef FONT8X8_H
#define FONT8X8_H

#include <tamtypes.h>

void font_draw_glyph(int x, int y, unsigned char c, u32 color);
void font_draw_glyph_scaled(int x, int y, unsigned char c, u32 color, int scale);

#endif