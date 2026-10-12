#ifndef GS_RENDERER_H
#define GS_RENDERER_H

#include <tamtypes.h>

#define SCREEN_W 640
#define SCREEN_H 448

typedef struct {
    int vram;
    int tbw;
    int psm;
    int tw, th;
    int pw, ph;
    int w, h;
} GsTexture;

typedef struct {
    float x, y;
    float w, h;
    float u, v;
    float uw, vh;
    u32 color;
    float angle;
    float scale_x, scale_y;
    float alpha;
    int visible;
} GsSprite;

typedef struct {
    float r, g, b, a;
} GsColor;

void gs_init(void);
void gs_shutdown(void);

void gs_frame_begin(void);
void gs_frame_end(void);

void gs_clear(GsColor color);
void gs_clear_depth(void);

GsTexture *gs_texture_create(int w, int h, const u32 *rgba);
GsTexture *gs_texture_create_empty(int w, int h, int psm);
void gs_texture_upload(GsTexture *tex, const u32 *rgba);
void gs_texture_free(GsTexture *tex);
void gs_texture_bind(const GsTexture *tex);

void gs_draw_sprite(const GsSprite *spr);
void gs_draw_sprite_ex(float x, float y, float w, float h,
                       const GsTexture *tex,
                       float u, float v, float uw, float vh,
                       GsColor color, float angle, float alpha);

void gs_draw_rect(float x, float y, float w, float h, GsColor color);
void gs_draw_rounded_rect(float x, float y, float w, float h, float r, GsColor color);
void gs_draw_rect_gradient(float x, float y, float w, float h,
                           GsColor c1, GsColor c2, int vertical);

void gs_draw_line(float x1, float y1, float x2, float y2, GsColor color, float thickness);

void gs_set_scissor(int x, int y, int w, int h);
void gs_reset_scissor(void);

void gs_enable_blend(int enable);
void gs_set_blend_mode(int src, int dst);

int gs_get_vram_used(void);
int gs_get_vram_free(void);

void gs_log(const char *fmt, ...);

#define GS_BLEND_NORMAL 0
#define GS_BLEND_ADD 1
#define GS_BLEND_MUL 2
#define GS_BLEND_ALPHA 3

#endif