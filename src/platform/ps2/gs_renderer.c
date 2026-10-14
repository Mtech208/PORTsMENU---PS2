#include <kernel.h>
#include <tamtypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

#include <dma.h>
#include <gs_privileged.h>
#include <gif_tags.h>
#include <gs_psm.h>

#include "gs_renderer.h"

#define GS_FB_ADDR 0
#define GS_ZB_ADDR 0x200000
#define GS_TEX_START 0x120000

/* GS register packers (los headers del SDK no los traen todos) */
#define GS_SET_FRAME(FBP, FBW, PSM, MASK)                                     \
    ((u64)((FBP)&0x1FF) | ((u64)((FBW)&0x3F) << 16) |                        \
     ((u64)((PSM)&0x3F) << 24) | ((u64)((MASK)) << 32))
#define GS_SET_XYOFFSET(OFX, OFY) ((u64)((OFX)&0xFFFF) | ((u64)((OFY)&0xFFFF) << 32))
#define GS_SET_SCISSOR(X0, X1, Y0, Y1)                                        \
    ((u64)((X0)&0x7FF) | ((u64)((X1)&0x7FF) << 16) |                          \
     ((u64)((Y0)&0x7FF) << 32) | ((u64)((Y1)&0x7FF) << 48))
#define GS_SET_BITBLTBUF(SBP, SBW, SPSM, DBP, DBW, DPSM)                       \
    ((u64)((SBP)&0x3FFF) | ((u64)((SBW)&0x3F) << 16) |                        \
     ((u64)((SPSM)&0x3F) << 24) | ((u64)((DBP)&0x3FFF) << 32) |               \
     ((u64)((DBW)&0x3F) << 48) | ((u64)((DPSM)&0x3F) << 56))
#define GS_SET_TRXPOS(SSAX, SSAY, DSAX, DSAY, DIR)                            \
    ((u64)((SSAX)&0x7FF) | ((u64)((SSAY)&0x7FF) << 16) |                      \
     ((u64)((DSAX)&0x7FF) << 32) | ((u64)((DSAY)&0x7FF) << 48) |              \
     ((u64)((DIR)&0x03) << 59))
#define GS_SET_TRXREG(RRW, RRH) ((u64)((RRW)&0xFFF) | ((u64)((RRH)&0xFFF) << 32))
#define GS_SET_TRXDIR(XDIR) ((u64)((XDIR)&0x03))

#define PKT_MAX 65536
static u64 pkt[PKT_MAX] __attribute__((aligned(64)));
static int p_n = 0;

static int gs_tex_next = GS_TEX_START;
static int gs_vram_used = 0;

static GsTexture *bound_tex = NULL;

static u32 frame_count = 0;

void gs_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    /* stdout siempre seguro (consola PCSX2 y UART) - sin host: que cuelga PS2 real */
    vprintf(fmt, args);
    printf("\n");

    va_end(args);
}

static inline void pkt_reg(u64 reg, u64 data)
{
    if (p_n + 2 >= PKT_MAX) return;
    pkt[p_n++] = data;
    pkt[p_n++] = reg;
}

static void send_and_wait(const u64 *buf, int qwords)
{
    FlushCache(0);
    dma_channel_send_normal(DMA_CHANNEL_GIF, (void *)buf, qwords, 0, 0);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
}

static void wait_vsync(void)
{
    *GS_REG_CSR |= *GS_REG_CSR & 8;
    while (!(*GS_REG_CSR & 8));
}

static int log2ceil(int v)
{
    int r = 0;
    int p = 1;
    while (p < v) { p <<= 1; r++; }
    return r;
}

/* ---------- VRAM allocator con free-list (first-fit) ---------- */
#define VRAM_SIZE (4 * 1024 * 1024)

typedef struct VramBlock {
    int offset;
    int size;
    int used;
    struct VramBlock *next;
} VramBlock;

static VramBlock *vram_blocks = NULL;

static int vram_alloc(int bytes)
{
    bytes = (bytes + 255) & ~255;
    for (VramBlock *b = vram_blocks; b; b = b->next)
        if (!b->used && b->size >= bytes) {
            b->used = 1;
            return b->offset;
        }
    int end = GS_TEX_START;
    for (VramBlock *b = vram_blocks; b; b = b->next)
        if (b->offset + b->size > end) end = b->offset + b->size;
    if (end + bytes > VRAM_SIZE)
        return -1;
    VramBlock *nb = malloc(sizeof(VramBlock));
    if (!nb) return -1;
    nb->offset = end;
    nb->size = bytes;
    nb->used = 1;
    nb->next = vram_blocks;
    vram_blocks = nb;
    return end;
}

static void vram_free(int offset)
{
    for (VramBlock *b = vram_blocks; b; b = b->next)
        if (b->offset == offset) {
            b->used = 0;
            return;
        }
}

static u64 make_tex0(const GsTexture *t)
{
    return GIF_SET_TEX0(t->vram / 256, t->tbw, t->psm, t->tw, t->th,
                        0, 0, 0, 0, 0, 0, 0);
}

static u64 make_prim(int prim, int iip, int tme, int fge, int abe, int aa1, int fst, int ctx, int fix)
{
    return GIF_SET_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctx, fix);
}

static u64 make_rgbaq(u32 r, u32 g, u32 b, u32 a, u32 q)
{
    return GIF_SET_RGBAQ(r, g, b, a, q);
}

static u64 make_uv(float u, float v)
{
    return GIF_SET_UV((int)(u * 16.0f), (int)(v * 16.0f));
}

static u64 make_xyz(float x, float y, float z)
{
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > 2047.0f) x = 2047.0f;
    if (y > 2047.0f) y = 2047.0f;
    return GIF_SET_XYZ((int)(x * 16.0f), (int)(y * 16.0f), (int)(z * 4096.0f));
}

static u64 make_xyoffset(int ofx, int ofy)
{
    return GS_SET_XYOFFSET(ofx, ofy);
}

static u64 make_frame(int fbp, int fbw, int psm, int mask)
{
    return GS_SET_FRAME(fbp, fbw, psm, mask);
}

static u64 make_scissor(int x0, int x1, int y0, int y1)
{
    return GS_SET_SCISSOR(x0, x1, y0, y1);
}

static u64 make_bitbltbuf(int sbp, int sbw, int spsm, int dbp, int dbw, int dpsm)
{
    return GS_SET_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm);
}

static u64 make_trxpos(int ssax, int ssay, int dsax, int dsay, int dir)
{
    return GS_SET_TRXPOS(ssax, ssay, dsax, dsay, dir);
}

static u64 make_trxreg(int rrw, int rrh)
{
    return GS_SET_TRXREG(rrw, rrh);
}

static u64 make_trxdir(int xdir)
{
    return GS_SET_TRXDIR(xdir);
}

void gs_init(void)
{
    dma_reset();
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    *GS_REG_CSR = (u64)1 << 9;
    *GS_REG_CSR = GS_SET_CSR(0,0,0,0,0,0,0,0,0,0,0,0);
    *GS_REG_IMR = 0x00007700;

    SetGsCrt(1, 2, 0);

    *GS_REG_PMODE = GS_SET_PMODE(1, 0, 1, 1, 0, 0xFF); /* RC1 on, RC2 off (gsKit) */
    *GS_REG_DISPFB1 = GS_SET_DISPFB(GS_FB_ADDR >> 11, SCREEN_W >> 6, GS_PSM_32, 0, 0);
    *GS_REG_DISPLAY1 = GS_SET_DISPLAY(656, 44, 3, 0, SCREEN_W * 4 - 1, SCREEN_H - 1);
    *GS_REG_BGCOLOR = GS_SET_BGCOLOR(0, 0, 0);

    p_n = 2;
    pkt_reg(0x4C, make_frame(GS_FB_ADDR >> 11, SCREEN_W >> 6, GS_PSM_32, 0));
    pkt_reg(0x18, make_xyoffset(0, 0));
    pkt_reg(0x40, make_scissor(0, SCREEN_W - 1, 0, SCREEN_H - 1));
    pkt_reg(0x47, 0);            /* TEST_1: todo desactivado */
    pkt_reg(0x42, 0x44);         /* ALPHA_1: blending estandar (como gsKit) */
    pkt_reg(0x08, 0x5);          /* CLAMP_1: clamp UV (evita repeats en bordes) */
    pkt_reg(0x14, 0);            /* TEX1_1 */
    pkt_reg(0x1A, 1);            /* PRMODECONT */
    pkt_reg(0x3B, 0);            /* TEXA */
    pkt_reg(0x3F, 0);            /* TEXFLUSH */

    int items = (p_n - 2) / 2;
    pkt[0] = GIF_SET_TAG(items, 1, 0, 0, GIF_FLG_PACKED, 1);
    pkt[1] = GIF_REG_AD;
    send_and_wait(pkt, p_n / 2);

    gs_log("gs: init ok, VRAM start=%x", GS_TEX_START);
}

void gs_shutdown(void)
{
    gs_log("gs: shutdown, VRAM used=%d KB", gs_vram_used / 1024);
    /* Dejar la pantalla en negro limpio y esperar VSync para mantener la senal
     * analógica activa en TVs CRT (evita que la tele pierda sincronismo y muestre VIDEO 1).
     * NO resetear el GS (*GS_REG_CSR = 1 << 9) porque apaga el generador de reloj de video. */
    GsColor black = {0.0f, 0.0f, 0.0f, 1.0f};
    gs_frame_begin();
    gs_clear(black);
    gs_frame_end();
    wait_vsync();

    /* Esperar que cualquier DMA de GIF termine para dejar el canal listo al juego */
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
}

void gs_frame_begin(void)
{
    p_n = 2;
    frame_count++;
    bound_tex = NULL;
}

void gs_frame_end(void)
{
    int items = (p_n - 2) / 2;
    pkt[0] = GIF_SET_TAG(items, 1, 0, 0, GIF_FLG_PACKED, 1);
    pkt[1] = GIF_REG_AD;
    send_and_wait(pkt, p_n / 2);
    wait_vsync();
}

void gs_clear(GsColor color)
{
    pkt_reg(0x00, make_prim(GIF_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
    pkt_reg(0x01, make_rgbaq((int)(color.r * 255), (int)(color.g * 255),
                             (int)(color.b * 255), (int)(color.a * 128), 0));
    pkt_reg(0x05, make_xyz(0, 0, 0));
    pkt_reg(0x05, make_xyz(SCREEN_W, SCREEN_H, 0));
}

void gs_clear_depth(void)
{
}

GsTexture *gs_texture_create(int w, int h, const u32 *rgba)
{
    GsTexture *t = calloc(1, sizeof(GsTexture));
    if (!t) return NULL;

    t->w = w;
    t->h = h;
    t->pw = (w + 63) & ~63;
    t->ph = (h + 31) & ~31;
    t->psm = GS_PSM_32;
    t->tbw = t->pw / 64;
    t->tw = log2ceil(t->pw);
    t->th = log2ceil(t->ph);

    int npix = t->pw * t->ph;
    int bytes = npix * 4;
    t->vram = vram_alloc(bytes);
    if (t->vram < 0) {
        gs_log("gs: VRAM llena (%d KB usados), textura %dx%d rechazada", gs_vram_used / 1024, w, h);
        free(t);
        return NULL;
    }
    gs_vram_used += bytes;

    u32 *pad = memalign(64, bytes);
    if (!pad) {
        gs_log("gs: HEAP agotado para pad %dx%d (%d KB)", w, h, bytes / 1024);
        vram_free(t->vram);
        gs_vram_used -= bytes;
        free(t);
        return NULL;
    }
    memset(pad, 0, bytes);
    for (int y = 0; y < h; y++)
        memcpy(&pad[y * t->pw], &rgba[y * w], w * 4);

    /* subir por bandas: cada transferencia DMA <= 16000 qwords (QWC es 16 bits) */
    {
        int rows_max = (16000 * 16) / (t->pw * 4);
        if (rows_max < 1) rows_max = 1;
        int y = 0;
        while (y < t->ph) {
            int rows = t->ph - y;
            if (rows > rows_max) rows = rows_max;
            int qwc_data = (t->pw * rows * 4 + 15) / 16;
            int total = 10 + 2 + qwc_data * 2 + 4;
            u64 *b = memalign(64, total * 8);
            if (!b) {
                gs_log("gs: HEAP agotado para banda de subida (%d KB)", total * 8 / 1024);
                break;
            }
            int i = 0;
            b[i++] = GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1);
            b[i++] = GIF_REG_AD;
            b[i++] = make_bitbltbuf(0, 0, 0, t->vram / 256, t->tbw, t->psm);
            b[i++] = 0x50;
            b[i++] = make_trxpos(0, 0, 0, y, 0);
            b[i++] = 0x51;
            b[i++] = make_trxreg(t->pw, rows);
            b[i++] = 0x52;
            b[i++] = make_trxdir(0);
            b[i++] = 0x53;
            b[i++] = GIF_SET_TAG(qwc_data, 0, 0, 0, GIF_FLG_IMAGE, 0);
            b[i++] = 0;
            memcpy(&b[i], &pad[(size_t)y * t->pw], t->pw * rows * 4);
            i += qwc_data * 2;
            b[i++] = GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1);
            b[i++] = GIF_REG_AD;
            b[i++] = 0;
            b[i++] = 0x3F; /* TEXFLUSH */
            send_and_wait(b, i / 2);
            free(b);
            y += rows;
        }
    }
    free(pad);

    gs_log("gs: texture %dx%d -> VRAM %x (%d KB)", w, h, t->vram, bytes / 1024);
    return t;
}

GsTexture *gs_texture_create_empty(int w, int h, int psm)
{
    GsTexture *t = calloc(1, sizeof(GsTexture));
    if (!t) return NULL;

    t->w = w;
    t->h = h;
    t->pw = (w + 63) & ~63;
    t->ph = (h + 31) & ~31;
    t->psm = psm;
    t->tbw = t->pw / 64;
    t->tw = log2ceil(t->pw);
    t->th = log2ceil(t->ph);

    int npix = t->pw * t->ph;
    int bpp = (psm == GS_PSM_32) ? 4 : 2;
    int bytes = npix * bpp;
    t->vram = (gs_tex_next + 0xFFF) & ~0xFFF;
    gs_tex_next = t->vram + bytes;
    gs_vram_used += bytes;

    gs_log("gs: empty texture %dx%d PSM=%d -> VRAM %x (%d KB)", w, h, psm, t->vram, bytes / 1024);
    return t;
}

void gs_texture_upload(GsTexture *t, const u32 *rgba)
{
    if (!t || !rgba) return;

    int npix = t->pw * t->ph;
    int qwc_data = (npix * 4 + 15) / 16;
    int total = 10 + 2 + qwc_data * 2 + 4;
    u64 *b = memalign(64, total * 8);
    if (!b) return;

    u32 *pad = memalign(64, npix * 4);
    memset(pad, 0, npix * 4);
    for (int y = 0; y < t->h; y++)
        memcpy(&pad[y * t->pw], &rgba[y * t->w], t->w * 4);

    int i = 0;
    b[i++] = GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1);
    b[i++] = GIF_REG_AD;
    b[i++] = make_bitbltbuf(0, 0, 0, t->vram / 256, t->tbw, t->psm);
    b[i++] = 0x50;
    b[i++] = make_trxpos(0, 0, 0, 0, 0);
    b[i++] = 0x51;
    b[i++] = make_trxreg(t->pw, t->ph);
    b[i++] = 0x52;
    b[i++] = make_trxdir(0);
    b[i++] = 0x53;

    b[i++] = GIF_SET_TAG(qwc_data, 0, 0, 0, GIF_FLG_IMAGE, 0);
    b[i++] = 0;
    memcpy(&b[i], pad, npix * 4);
    i += qwc_data * 2;

    b[i++] = GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1);
    b[i++] = GIF_REG_AD;
    b[i++] = 0;
    b[i++] = 0x3F;

    send_and_wait(b, i / 2);
    free(b);
    free(pad);
}

void gs_texture_free(GsTexture *t)
{
    if (!t) return;
    vram_free(t->vram);
    gs_vram_used -= t->pw * t->ph * 4;
    free(t);
}

void gs_texture_bind(const GsTexture *tex)
{
    if (bound_tex == tex) return;
    bound_tex = (GsTexture *)tex;
    if (tex) {
        pkt_reg(0x06, make_tex0(tex));
        pkt_reg(0x14, 0); /* TEX1_1 defaults */
    }
}

void gs_draw_sprite(const GsSprite *spr)
{
    if (!spr || !spr->visible) return;
    if (!spr->w || !spr->h) return;

    GsTexture *tex = bound_tex;
    int has_tex = (tex != NULL);

    float sx = spr->x;
    float sy = spr->y;
    float sw = spr->w * spr->scale_x;
    float sh = spr->h * spr->scale_y;

    if (sw <= 0.0f || sh <= 0.0f) return;

    /* Clipeo seguro para sprites alineados (angle == 0) contra los limites
       del framebuffer de PS2 [0, SCREEN_W] x [0, SCREEN_H].
       Evita que coords negativas desborden el registro de 16 bits del GS y
       generen bandas estiradas hacia la derecha al salir de pantalla. */
    if (spr->angle == 0.0f) {
        if (sx + sw <= 0.0f || sx >= (float)SCREEN_W || sy + sh <= 0.0f || sy >= (float)SCREEN_H)
            return;

        float su = spr->u;
        float sv = spr->v;
        float suw = spr->uw;
        float svh = spr->vh;
        float orig_sw = sw;
        float orig_sh = sh;

        if (sx < 0.0f) {
            float clip = -sx;
            float ratio = clip / orig_sw;
            sx = 0.0f;
            sw -= clip;
            su += ratio * suw;
            suw -= ratio * suw;
        }
        if (sx + sw > (float)SCREEN_W) {
            float clip = (sx + sw) - (float)SCREEN_W;
            float ratio = clip / orig_sw;
            sw = (float)SCREEN_W - sx;
            suw -= ratio * suw;
        }
        if (sy < 0.0f) {
            float clip = -sy;
            float ratio = clip / orig_sh;
            sy = 0.0f;
            sh -= clip;
            sv += ratio * svh;
            svh -= ratio * svh;
        }
        if (sy + sh > (float)SCREEN_H) {
            float clip = (sy + sh) - (float)SCREEN_H;
            float ratio = clip / orig_sh;
            sh = (float)SCREEN_H - sy;
            svh -= ratio * svh;
        }

        if (sw <= 0.0f || sh <= 0.0f) return;

        u32 r = (spr->color >> 16) & 0xFF;
        u32 g = (spr->color >> 8) & 0xFF;
        u32 b = spr->color & 0xFF;
        float af = ((spr->color >> 24) & 0xFF) / 255.0f * spr->alpha;
        u32 a = (u32)(af * 128.0f);

        if (has_tex) {
            pkt_reg(0x00, make_prim(4, 0, 1, 0, 1, 0, 1, 0, 0));
            pkt_reg(0x01, make_rgbaq(128, 128, 128, a, 0));
            pkt_reg(0x03, make_uv(su, sv));
            pkt_reg(0x05, make_xyz(sx, sy, 0));
            pkt_reg(0x03, make_uv(su + suw, sv));
            pkt_reg(0x05, make_xyz(sx + sw, sy, 0));
            pkt_reg(0x03, make_uv(su, sv + svh));
            pkt_reg(0x05, make_xyz(sx, sy + sh, 0));
            pkt_reg(0x03, make_uv(su + suw, sv + svh));
            pkt_reg(0x05, make_xyz(sx + sw, sy + sh, 0));
        } else {
            pkt_reg(0x00, make_prim(GIF_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
            pkt_reg(0x01, make_rgbaq(r, g, b, a, 0));
            pkt_reg(0x05, make_xyz(sx, sy, 0));
            pkt_reg(0x05, make_xyz(sx + sw, sy + sh, 0));
        }
        return;
    }

    float cx = spr->x + spr->w * 0.5f;
    float cy = spr->y + spr->h * 0.5f;

    float hw = spr->w * 0.5f * spr->scale_x;
    float hh = spr->h * 0.5f * spr->scale_y;

    float cos_a = cosf(spr->angle);
    float sin_a = sinf(spr->angle);

    float x1 = -hw, y1 = -hh;   /* TL */
    float x2 =  hw, y2 = -hh;   /* TR */
    float x3 = -hw, y3 =  hh;   /* BL */
    float x4 =  hw, y4 =  hh;   /* BR */

    float rx1 = x1 * cos_a - y1 * sin_a + cx;
    float ry1 = x1 * sin_a + y1 * cos_a + cy;
    float rx2 = x2 * cos_a - y2 * sin_a + cx;
    float ry2 = x2 * sin_a + y2 * cos_a + cy;
    float rx3 = x3 * cos_a - y3 * sin_a + cx;
    float ry3 = x3 * sin_a + y3 * cos_a + cy;
    float rx4 = x4 * cos_a - y4 * sin_a + cx;
    float ry4 = x4 * sin_a + y4 * cos_a + cy;

    /* Descarte si completamente fuera */
    if ((rx1 < 0 && rx2 < 0 && rx3 < 0 && rx4 < 0) ||
        (rx1 > SCREEN_W && rx2 > SCREEN_W && rx3 > SCREEN_W && rx4 > SCREEN_W) ||
        (ry1 < 0 && ry2 < 0 && ry3 < 0 && ry4 < 0) ||
        (ry1 > SCREEN_H && ry2 > SCREEN_H && ry3 > SCREEN_H && ry4 > SCREEN_H))
        return;

    u32 r = (spr->color >> 16) & 0xFF;
    u32 g = (spr->color >> 8) & 0xFF;
    u32 b = spr->color & 0xFF;
    /* alfa GS: 0x80 = 1.0 (no 255) */
    float af = ((spr->color >> 24) & 0xFF) / 255.0f * spr->alpha;
    u32 a = (u32)(af * 128.0f);

    if (has_tex) {
        /* TRIANGLESTRIP con UV por vertice (TL, TR, BL, BR) */
        pkt_reg(0x00, make_prim(4, 0, 1, 0, 1, 0, 1, 0, 0));
        pkt_reg(0x01, make_rgbaq(128, 128, 128, a, 0));
        pkt_reg(0x03, make_uv(spr->u, spr->v));
        pkt_reg(0x05, make_xyz(rx1, ry1, 0));
        pkt_reg(0x03, make_uv(spr->u + spr->uw, spr->v));
        pkt_reg(0x05, make_xyz(rx2, ry2, 0));
        pkt_reg(0x03, make_uv(spr->u, spr->v + spr->vh));
        pkt_reg(0x05, make_xyz(rx3, ry3, 0));
        pkt_reg(0x03, make_uv(spr->u + spr->uw, spr->v + spr->vh));
        pkt_reg(0x05, make_xyz(rx4, ry4, 0));
    } else {
        /* SPRITE: 2 vertices en diagonal (rect alineado) */
        pkt_reg(0x00, make_prim(GIF_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
        pkt_reg(0x01, make_rgbaq(r, g, b, a, 0));
        pkt_reg(0x05, make_xyz(spr->x, spr->y, 0));
        pkt_reg(0x05, make_xyz(spr->x + spr->w, spr->y + spr->h, 0));
    }
}

void gs_draw_sprite_ex(float x, float y, float w, float h,
                       const GsTexture *tex,
                       float u, float v, float uw, float vh,
                       GsColor color, float angle, float alpha)
{
    GsSprite spr = {0};
    spr.x = x;
    spr.y = y;
    spr.w = w;
    spr.h = h;
    spr.u = u;
    spr.v = v;
    spr.uw = uw;
    spr.vh = vh;
    spr.color = (int)(color.r * 255) << 16 | (int)(color.g * 255) << 8 | (int)(color.b * 255) | (int)(color.a * 255) << 24;
    spr.angle = angle;
    spr.scale_x = 1.0f;
    spr.scale_y = 1.0f;
    spr.alpha = alpha;
    spr.visible = 1;

    if (tex) gs_texture_bind(tex);
    gs_draw_sprite(&spr);
}

void gs_draw_rect(float x, float y, float w, float h, GsColor color)
{
    if (x + w <= 0.0f || x >= (float)SCREEN_W || y + h <= 0.0f || y >= (float)SCREEN_H) return;
    if (x < 0.0f) { w += x; x = 0.0f; }
    if (y < 0.0f) { h += y; y = 0.0f; }
    if (x + w > (float)SCREEN_W) w = (float)SCREEN_W - x;
    if (y + h > (float)SCREEN_H) h = (float)SCREEN_H - y;
    if (w <= 0.0f || h <= 0.0f) return;

    /* activar blending solo si es semitransparente */
    int abe = (color.a < 0.999f) ? 1 : 0;
    pkt_reg(0x00, make_prim(GIF_PRIM_SPRITE, 0, 0, 0, abe, 0, 0, 0, 0));
    pkt_reg(0x01, make_rgbaq((int)(color.r * 255), (int)(color.g * 255),
                             (int)(color.b * 255), (int)(color.a * 128), 0));
    pkt_reg(0x05, make_xyz(x, y, 0));
    pkt_reg(0x05, make_xyz(x + w, y + h, 0));
}

/* rect redondeado via TRIANGLEFAN: 4 arcos de esquina, poligono CERRADO */
void gs_draw_rounded_rect(float x, float y, float w, float h, float r, GsColor color)
{
    if (x + w <= 0.0f || x >= (float)SCREEN_W || y + h <= 0.0f || y >= (float)SCREEN_H) return;
    if (x < 0.0f || y < 0.0f || x + w > (float)SCREEN_W || y + h > (float)SCREEN_H) {
        gs_draw_rect(x, y, w, h, color);
        return;
    }
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 2.0f) {
        gs_draw_rect(x, y, w, h, color);
        return;
    }

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    int abe = (color.a < 0.999f) ? 1 : 0;
    const float PI = 3.14159265f;

    pkt_reg(0x00, make_prim(5, 0, 0, 0, abe, 0, 0, 0, 0)); /* TRIANGLEFAN */
    pkt_reg(0x01, make_rgbaq((int)(color.r * 255), (int)(color.g * 255),
                             (int)(color.b * 255), (int)(color.a * 128), 0));
    pkt_reg(0x05, make_xyz(cx, cy, 0));

    const int seg = 5;
    float first_x = 0, first_y = 0;
    for (int c = 0; c < 4; c++) {
        float acx = (c == 0 || c == 3) ? (x + r) : (x + w - r);
        float acy = (c < 2) ? (y + r) : (y + h - r);
        float start = PI + c * (PI / 2.0f);
        for (int s = 0; s <= seg; s++) {
            float ang = start + (PI / 2.0f) * s / seg;
            float px = acx + r * cosf(ang);
            float py = acy + r * sinf(ang);
            if (c == 0 && s == 0) { first_x = px; first_y = py; }
            pkt_reg(0x05, make_xyz(px, py, 0));
        }
    }
    /* cerrar el poligono: ultimo punto = primer punto del perimetro */
    pkt_reg(0x05, make_xyz(first_x, first_y, 0));
}

void gs_draw_rect_gradient(float x, float y, float w, float h,
                           GsColor c1, GsColor c2, int vertical)
{
    (void)vertical;
    if (x + w <= 0.0f || x >= (float)SCREEN_W || y + h <= 0.0f || y >= (float)SCREEN_H) return;
    if (x < 0.0f) { w += x; x = 0.0f; }
    if (y < 0.0f) { h += y; y = 0.0f; }
    if (x + w > (float)SCREEN_W) w = (float)SCREEN_W - x;
    if (y + h > (float)SCREEN_H) h = (float)SCREEN_H - y;
    if (w <= 0.0f || h <= 0.0f) return;

    int abe = (c1.a < 0.999f || c2.a < 0.999f) ? 1 : 0;
    pkt_reg(0x00, make_prim(GIF_PRIM_SPRITE, 1, 0, 0, abe, 0, 0, 0, 0));
    pkt_reg(0x01, make_rgbaq((int)(c1.r * 255), (int)(c1.g * 255),
                             (int)(c1.b * 255), (int)(c1.a * 128), 0));
    pkt_reg(0x05, make_xyz(x, y, 0));
    pkt_reg(0x01, make_rgbaq((int)(c2.r * 255), (int)(c2.g * 255),
                             (int)(c2.b * 255), (int)(c2.a * 128), 0));
    pkt_reg(0x05, make_xyz(x + w, y + h, 0));
}

void gs_draw_line(float x1, float y1, float x2, float y2, GsColor color, float thickness)
{
    (void)thickness;
    pkt_reg(0x00, make_prim(GIF_PRIM_LINE, 0, 0, 0, 0, 0, 0, 0, 0));
    pkt_reg(0x01, make_rgbaq((int)(color.r * 255), (int)(color.g * 255), (int)(color.b * 255), (int)(color.a * 255), 0));
    pkt_reg(0x05, make_xyz(x1, y1, 0));
    pkt_reg(0x05, make_xyz(x2, y2, 0));
}

void gs_set_scissor(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    int y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    pkt_reg(0x40, make_scissor(x0, x1 - 1, y0, y1 - 1));
}

void gs_reset_scissor(void)
{
    pkt_reg(0x40, make_scissor(0, SCREEN_W - 1, 0, SCREEN_H - 1));
}

void gs_enable_blend(int enable)
{
    if (enable) {
        pkt_reg(0x42, 0x44);
    } else {
        pkt_reg(0x42, 0x10); /* C=0/D=1: opaco */
    }
}

void gs_set_blend_mode(int src, int dst)
{
    (void)dst;
    switch (src) {
        case GS_BLEND_NORMAL:
        case GS_BLEND_ALPHA:
            pkt_reg(0x42, 0x44);
            break;
        case GS_BLEND_ADD:    /* C=A: suma */
            pkt_reg(0x42, 0x40);
            break;
        case GS_BLEND_MUL:    /* C=B: multiplica */
            pkt_reg(0x42, 0x04);
            break;
    }
}

int gs_get_vram_used(void) { return gs_vram_used; }
int gs_get_vram_free(void) { return 4 * 1024 * 1024 - gs_vram_used; }