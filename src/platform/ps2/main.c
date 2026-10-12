// PORTMENU main.c - menu launcher PS2 con grid animado, covers y fondos

#include <tamtypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <loadfile.h>
#include <libpad.h>

#include "gs_renderer.h"
#include "asset_manager.h"
#include "ui_menu.h"

/* newlib port internals */
extern void sceSifInitRpc(int unk);
extern int fioInit(void);
extern void __fioOpsInitializeImpl(void);

#define PAD_BUF_SIZE 256
static u8 pad_buf[PAD_BUF_SIZE] __attribute__((aligned(64)));
static int pad_port = 0, pad_slot = 0;
static int pad_available = 0;

static void pad_init(void)
{
    int ret;

    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (ret < 0) {
        gs_log("pad: SIO2MAN fallo (%d)", ret);
        return;
    }
    ret = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (ret < 0) {
        gs_log("pad: PADMAN fallo (%d)", ret);
        return;
    }

    if (padInit(0) != 1) {
        gs_log("pad: padInit fallo");
        return;
    }
    if (padPortOpen(pad_port, pad_slot, pad_buf) == 0) {
        gs_log("pad: padPortOpen fallo");
        return;
    }
    pad_available = 1;
    gs_log("pad: listo");
}

static int pad_read(unsigned short *btn)
{
    struct padButtonStatus status;
    *btn = 0;
    if (!pad_available)
        return 0;
    if (padRead(pad_port, pad_slot, &status) == 0)
        return 0; /* sin datos nuevos este frame */
    *btn = ~status.btns; /* botones activos = bits en 0 */
    return 1;
}

static float frame_delta(void)
{
    return 1.0f / 60.0f; /* NTSC fijo, suficiente para el menu */
}

/* reutiliza la fuente sin depender de ui_menu */
extern void font_draw_glyph_scaled(int x, int y, unsigned char c, u32 color, int scale);
static void ui_diag_text(int x, int y, const char *s, int scale, u32 color)
{
    int cx = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 32 && c < 128)
            font_draw_glyph_scaled(cx, y, c, color, scale);
        cx += 8 * scale;
    }
}

/* boot splash: ART/BG/BOOT (JPG/PNG/BMP); fallback: negro con texto */
static void boot_splash(void)
{
    static const char *cands[] = {
        "ART/BG/BOOT.JPG", "ART/BG/BOOT.jpg", "ART/BG/BOOT.JPEG", "ART/BG/BOOT.jpeg",
        "ART/BG/BOOT.PNG", "ART/BG/BOOT.png", "ART/BG/BOOT.BMP", "ART/BG/BOOT.bmp",
    };
    GsTexture *boot = NULL;
    for (int i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])) && !boot; i++) {
        FILE *f = fopen(cands[i], "rb");
        if (f) {
            fclose(f);
            boot = assets_get(cands[i]);
        }
    }
    if (boot)
        gs_log("boot splash: imagen cargada");

    for (int fr = 0; fr < 150; fr++) { /* ~2.5s */
        gs_frame_begin();
        gs_clear((GsColor){0, 0, 0, 1});
        if (boot) {
            /* aspect-fit centrado sobre negro */
            float w = (float)SCREEN_W;
            float h = w * boot->h / boot->w;
            float y = (SCREEN_H - h) * 0.5f;
            if (h > (float)SCREEN_H) {
                h = (float)SCREEN_H;
                w = h * boot->w / boot->h;
                y = 0;
            }
            gs_draw_sprite_ex((SCREEN_W - w) * 0.5f, y, w, h,
                              boot, 0, 0, (float)boot->w, (float)boot->h,
                              (GsColor){1, 1, 1, 1}, 0, 1.0f);
        } else {
            ui_diag_text(SCREEN_W / 2 - 4 * 16, SCREEN_H / 2 - 16,
                         "PORTsMENU", 2, 0xFF60A0FFu);
            ui_diag_text(SCREEN_W / 2 - 4 * 12, SCREEN_H / 2 + 8,
                         "CARGANDO", 1, 0xFFC0C0C0u);
        }
        gs_frame_end();
    }
    /* la textura queda en cache (se gestiona sola), no se libera aqui */
}

int main(void)
{
    gs_init();

    sceSifInitRpc(0);
    fioInit();
    __fioOpsInitializeImpl();
    gs_log("fio: listo");

    ui_init();
    assets_init();

    /* boot splash (~2.5s) mientras el sistema arranca */
    boot_splash();

    ui_load_games();
    ui_load_config();
    gs_log("menu: %d juegos", ui_get_game_count());

    pad_init();

    gs_log("boot: entrando al menu");

    if (ui_get_game_count() == 0) {
        gs_log("sin juegos; pantalla de espera");
        for (;;) {
            gs_frame_begin();
            gs_clear((GsColor){0.02f, 0.02f, 0.05f, 1});
            gs_draw_rect_gradient(0, 0, SCREEN_W, SCREEN_H,
                                  (GsColor){0.10f, 0.12f, 0.30f, 1},
                                  (GsColor){0.02f, 0.02f, 0.06f, 1}, 1);
            gs_frame_end();
        }
    }

    unsigned short prev_btn = 0;
    int launch = 0;

    for (;;) {
        unsigned short btn = 0;
        pad_read(&btn);
        unsigned short pressed = btn & ~prev_btn;
        prev_btn = btn;

        launch = ui_handle_input(btn, pressed);
        ui_update(frame_delta());

        gs_frame_begin();
        ui_render();
        gs_frame_end();

        if (launch) {
            ui_launch_selected(); /* no retorna */
        }
    }

    return 0;
}