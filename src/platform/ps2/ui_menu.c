// ui_menu.c - grid animado con transiciones, covers, fondos y lanzamiento de ELFs
// Los juegos se autodetectan escaneando ELFS/: el cover/logo se llama igual que el ELF.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include "ui_menu.h"
#include "gs_renderer.h"
#include "asset_manager.h"
#include "font8x8.h"

extern void LoadExecPS2(const char *filename, int num_args, char *args[]);

#define PAD_LEFT 0x0080
#define PAD_RIGHT 0x0020
#define PAD_UP 0x0010
#define PAD_DOWN 0x0040
#define PAD_CROSS 0x4000
#define PAD_R1 0x0800
#define PAD_L1 0x0400
#define PAD_START 0x0008

static GameList list;
static UIState ui;

/* ---------- easing ---------- */
static float ease_out_cubic(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    float it = 1.0f - t;
    return 1.0f - it * it * it;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

/* ---------- config persistente ---------- */
#define CONFIG_FILE "portmenu.cfg"

static char cfg_base[256] = "";
static char media_prefix[16] = "host:"; /* medio desde donde se cargo games.cfg */

void ui_load_config(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", media_prefix, "portmenu.cfg");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    if (fgets(line, sizeof(line), f)) {
        int v = atoi(line);
        if (v >= 0 && v < list.count) list.selected = v;
    }
    fclose(f);
}

void ui_save_config(void)
{
    if (list.count <= 0) return;
    char path[512];
    snprintf(path, sizeof(path), "%s%s", media_prefix, "portmenu.cfg");
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", list.selected);
        fclose(f);
    }
}

/* ---------- parser games.cfg ---------- */
/* formato: titulo|elf|cover|fondo|logo|icono|genero|anio|descripcion
   todo opcional despues de titulo|elf */

static void field(char *dst, int dstsz, char **p)
{
    char *q = strchr(*p, '|');
    if (!q) q = *p + strlen(*p);
    int n = (int)(q - *p);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, *p, n);
    dst[n] = '\0';
    /* trim */
    char *e = dst + strlen(dst) - 1;
    while (e >= dst && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n')) *e-- = '\0';
    char *s = dst;
    while (*s == ' ' || *s == '\t') s++;
    if (s != dst) memmove(dst, s, strlen(s) + 1);
    *p = q + 1;
}

static void fix_asset_path(char *dst, int sz, const char *val)
{
    if (!*val) { dst[0] = '\0'; return; }
    if (strchr(val, ':')) { strncpy(dst, val, sz - 1); dst[sz-1]=0; return; }
    snprintf(dst, sz, "ART/%s", val);
}

/* ---------- auto-scan de ELFS/ (cover = mismo nombre que el ELF) ---------- */

static void norm_path(char *out, int sz, const char *in)
{
    const char *c = strchr(in, ':');
    if (c) in = c + 1;
    while (*in == '/' || *in == '\\') in++;
    int n = 0;
    for (; *in && n < sz - 1; in++) {
        char ch = *in;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        out[n++] = (ch == '\\') ? '/' : ch;
    }
    out[n] = '\0';
}

static int pref_exists(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", media_prefix, rel);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* cover/logo por convencion: <sub>/<base>.<ext> — el mismo nombre que el ELF.
   Matching: 1) exacto  2) prefijo comun >= 3 chars (tolera nombres 8.3 en ISO) */

typedef struct {
    char base[64];  /* nombre sin extension, uppercase */
    char file[192]; /* ruta relativa completa (sub/archivo.ext) */
} ArtFile;

static ArtFile g_covs[128];
static int g_ncovs = 0;
static ArtFile g_logos[128];
static int g_nlogos = 0;

static void collect_art(const char *sub, ArtFile *out, int max, int *count)
{
    *count = 0;
    char dirp[256];
    snprintf(dirp, sizeof(dirp), "%s%s", media_prefix, sub);
    DIR *d = opendir(dirp);
    if (!d) {
        snprintf(dirp, sizeof(dirp), "%s/%s", media_prefix, sub);
        d = opendir(dirp);
    }
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < max) {
        if (e->d_name[0] == '.') continue;
        /* separar stem/ext en uppercase */
        char up[96];
        int n = 0;
        for (const char *p = e->d_name; *p && n < 95; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            up[n++] = c;
        }
        up[n] = '\0';
        char *dot = strrchr(up, '.');
        if (!dot || dot == up) continue;
        const char *ext = dot + 1;
        if (strcmp(ext, "JPG") != 0 && strcmp(ext, "JPEG") != 0 &&
            strcmp(ext, "PNG") != 0 && strcmp(ext, "BMP") != 0) continue;
        *dot = '\0';
        if (!*up) continue;
        strncpy(out[*count].base, up, sizeof(out[*count].base) - 1);
        out[*count].base[sizeof(out[*count].base) - 1] = '\0';
        snprintf(out[*count].file, sizeof(out[*count].file), "%s/%s", sub, e->d_name);
        (*count)++;
    }
    closedir(d);
}

static void match_art(char *dst, int sz, const ArtFile *arts, int count, const char *elfbase)
{
    dst[0] = '\0';
    int best = -1, bestlen = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(arts[i].base, elfbase) == 0) { best = i; bestlen = 999; break; }
        int l1 = (int)strlen(arts[i].base);
        int l2 = (int)strlen(elfbase);
        int m = l1 < l2 ? l1 : l2;
        if (m >= 3 && strncmp(arts[i].base, elfbase, m) == 0 && m > bestlen) {
            best = i;
            bestlen = m;
        }
    }
    if (best >= 0) {
        strncpy(dst, arts[best].file, sz - 1);
        dst[sz - 1] = '\0';
    }
}

/* base de un ELF: ultimo componente sin extension, uppercase */
static void base_of_path(char *out, int sz, const char *path)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') last = p + 1;
    char up[64];
    int n = 0;
    for (const char *p = last; *p && n < 63; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        up[n++] = c;
    }
    up[n] = '\0';
    char *semi = strchr(up, ';');
    if (semi) *semi = '\0';
    int len = (int)strlen(up);
    if (len > 4 && strcmp(up + len - 4, ".ELF") == 0) up[len - 4] = '\0';
    strncpy(out, up, sz - 1);
    out[sz - 1] = '\0';
}

/* match difuso de bases: uno es prefijo del otro, >= 3 chars (tolera 8.3 en ISO) */
static int base_match(const char *a, const char *b)
{
    if (strcmp(a, b) == 0) return 1;
    int l1 = (int)strlen(a), l2 = (int)strlen(b);
    int m = l1 < l2 ? l1 : l2;
    return m >= 3 && strncmp(a, b, m) == 0;
}

static void add_game_full(const char *title, const char *elf, const char *cover, const char *logo,
                          const char *genre, const char *year, const char *desc)
{
    char norm[256];
    norm_path(norm, sizeof(norm), elf);

    /* 1) match exacto por ruta */
    for (int i = 0; i < list.count; i++) {
        char tmp[256];
        norm_path(tmp, sizeof(tmp), list.games[i].elf_path);
        if (strcmp(tmp, norm) != 0) continue;
        GameEntry *g = &list.games[i];
        if (title && *title) { strncpy(g->title, title, sizeof(g->title) - 1); g->title[sizeof(g->title) - 1] = 0; }
        if (cover && *cover) { strncpy(g->cover, cover, sizeof(g->cover) - 1); g->cover[sizeof(g->cover) - 1] = 0; }
        if (logo && *logo) { strncpy(g->logo, logo, sizeof(g->logo) - 1); g->logo[sizeof(g->logo) - 1] = 0; }
        if (genre && *genre) { strncpy(g->genre, genre, sizeof(g->genre) - 1); g->genre[sizeof(g->genre) - 1] = 0; }
        if (year && *year) { strncpy(g->year, year, sizeof(g->year) - 1); g->year[sizeof(g->year) - 1] = 0; }
        if (desc && *desc) { strncpy(g->description, desc, sizeof(g->description) - 1); g->description[sizeof(g->description) - 1] = 0; }
        return;
    }

    /* 2) match difuso por nombre base (cfg referencia un ELF renombrado a 8.3 en ISO) */
    char want_base[64];
    base_of_path(want_base, sizeof(want_base), elf);
    for (int i = 0; i < list.count; i++) {
        char have_base[64];
        base_of_path(have_base, sizeof(have_base), list.games[i].elf_path);
        if (!base_match(want_base, have_base)) continue;
        GameEntry *g = &list.games[i];
        if (title && *title) { strncpy(g->title, title, sizeof(g->title) - 1); g->title[sizeof(g->title) - 1] = 0; }
        if (cover && *cover) { strncpy(g->cover, cover, sizeof(g->cover) - 1); g->cover[sizeof(g->cover) - 1] = 0; }
        if (logo && *logo) { strncpy(g->logo, logo, sizeof(g->logo) - 1); g->logo[sizeof(g->logo) - 1] = 0; }
        if (genre && *genre) { strncpy(g->genre, genre, sizeof(g->genre) - 1); g->genre[sizeof(g->genre) - 1] = 0; }
        if (year && *year) { strncpy(g->year, year, sizeof(g->year) - 1); g->year[sizeof(g->year) - 1] = 0; }
        if (desc && *desc) { strncpy(g->description, desc, sizeof(g->description) - 1); g->description[sizeof(g->description) - 1] = 0; }
        return;
    }

    /* 3) juego nuevo */
    if (list.count >= MAX_GAMES || !title || !*title || !elf || !*elf) return;
    GameEntry *g = &list.games[list.count];
    memset(g, 0, sizeof(GameEntry));
    strncpy(g->title, title, sizeof(g->title) - 1);
    strncpy(g->elf_path, elf, sizeof(g->elf_path) - 1);
    if (cover) { strncpy(g->cover, cover, sizeof(g->cover) - 1); g->cover[sizeof(g->cover) - 1] = 0; }
    if (logo) { strncpy(g->logo, logo, sizeof(g->logo) - 1); g->logo[sizeof(g->logo) - 1] = 0; }
    if (genre) { strncpy(g->genre, genre, sizeof(g->genre) - 1); g->genre[sizeof(g->genre) - 1] = 0; }
    if (year) { strncpy(g->year, year, sizeof(g->year) - 1); g->year[sizeof(g->year) - 1] = 0; }
    if (desc) { strncpy(g->description, desc, sizeof(g->description) - 1); g->description[sizeof(g->description) - 1] = 0; }
    list.count++;
}

static int tail_is_elf(const char *name)
{
    char up[64];
    int n = 0;
    for (const char *p = name; *p && n < 63; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        up[n++] = c;
    }
    up[n] = '\0';
    char *semi = strchr(up, ';');
    if (semi) *semi = '\0';
    int len = (int)strlen(up);
    return len > 4 && strcmp(up + len - 4, ".ELF") == 0;
}

static void scan_add_elf(const char *full, const char *name)
{
    char up[64];
    int n = 0;
    for (const char *p = name; *p && n < 63; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        up[n++] = c;
    }
    up[n] = '\0';
    char *semi = strchr(up, ';');
    if (semi) *semi = '\0';
    int len = (int)strlen(up);
    if (len <= 4) return;
    up[len - 4] = '\0'; /* quitar .ELF */

    char title[128];
    strncpy(title, up, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    for (char *q = title; *q; q++)
        if (*q == '_') *q = ' ';

    char cov[128], logo[128];
    match_art(cov, sizeof(cov), g_covs, g_ncovs, up);
    match_art(logo, sizeof(logo), g_logos, g_nlogos, up);
    gs_log("scan: %s%s%s", title, *cov ? "" : "  [sin cover]", *logo ? "" : "  [sin logo]");
    add_game_full(title, full, cov, logo, "", "", "");
}

static void scan_games(void)
{
    /* colectar covers y logos disponibles primero */
    collect_art("ART/COV", g_covs, 128, &g_ncovs);
    collect_art("ART/LOGO", g_logos, 128, &g_nlogos);
    gs_log("scan: %d covers, %d logos disponibles", g_ncovs, g_nlogos);

    char dirp[256];
    snprintf(dirp, sizeof(dirp), "%sELFS", media_prefix);
    DIR *d = opendir(dirp);
    if (!d) {
        snprintf(dirp, sizeof(dirp), "%s/ELFS", media_prefix);
        d = opendir(dirp);
    }
    if (!d) {
        gs_log("scan: sin carpeta ELFS en %s", media_prefix);
        return;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dirp, e->d_name);
        DIR *dd = opendir(full);
        if (dd) {
            /* subdirectorio: buscar ELFs dentro (JUEGO/JUEGO.ELF) */
            struct dirent *e2;
            while ((e2 = readdir(dd)) != NULL) {
                if (e2->d_name[0] == '.') continue;
                if (!tail_is_elf(e2->d_name)) continue;
                char full2[640];
                snprintf(full2, sizeof(full2), "%s/%s", full, e2->d_name);
                scan_add_elf(full2, e2->d_name);
                found++;
            }
            closedir(dd);
        } else if (tail_is_elf(e->d_name)) {
            scan_add_elf(full, e->d_name);
            found++;
        }
    }
    closedir(d);
    gs_log("scan: %d ELFs en %s", found, dirp);
}

static void sort_games(void)
{
    for (int i = 0; i < list.count - 1; i++)
        for (int j = 0; j < list.count - 1 - i; j++)
            if (strcmp(list.games[j].title, list.games[j + 1].title) > 0) {
                GameEntry t = list.games[j];
                list.games[j] = list.games[j + 1];
                list.games[j + 1] = t;
            }
}

void ui_load_games(void)
{
    list.count = 0;
    list.selected = 0;
    list.page = 0;
    list.games_per_page = MAX_VISIBLE;

    /* 1) medio: buscar games.cfg. "" = device por defecto (medio de arranque:
       cdrom0: en ISO/PS2 real, host: en desarrollo). Detecta donde exista. */
    static const char *candidates[] = {
        "GAMES.CFG", "games.cfg",
        "cdfs:/GAMES.CFG", "cdfs:/games.cfg",
        "cdrom0:/GAMES.CFG", "cdrom:GAMES.CFG",
        "host:games.cfg", "host:/GAMES.CFG",
        "mass:/games.cfg", "mass:/GAMES.CFG",
        "mc0:games.cfg", "mc0:/GAMES.CFG",
    };
    FILE *f = NULL;
    const char *found = NULL;
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        f = fopen(candidates[i], "r");
        if (f) { found = candidates[i]; break; }
    }
    if (!f) {
        gs_log("games.cfg: no encontrado");
        media_prefix[0] = '\0';
    } else {
        if (strncmp(found, "cdfs:", 5) == 0) strcpy(media_prefix, "cdfs:/");
        else if (strncmp(found, "cdrom", 5) == 0) strcpy(media_prefix, "");
        else if (strncmp(found, "mass:", 5) == 0) strcpy(media_prefix, "mass:/");
        else if (strncmp(found, "mc0:", 4) == 0) strcpy(media_prefix, "mc0:");
        else if (strncmp(found, "host:", 5) == 0) strcpy(media_prefix, "host:");
        else strcpy(media_prefix, ""); /* device por defecto (medio de arranque) */
        gs_log("games.cfg: %s (medio %s)",
               found, media_prefix[0] ? media_prefix : "<arranque>");
    }

    /* 2) auto-scan: ELFS/ con covers del mismo nombre */
    if (!media_prefix[0]) strcpy(media_prefix, "cdfs:/");
    scan_games();
    if (list.count == 0) {
        /* fallback: probar otros medios */
        strcpy(media_prefix, "");
        scan_games();
        if (list.count == 0) {
            strcpy(media_prefix, "host:");
            scan_games();
        }
    }

    /* 3) games.cfg: personaliza titulos/covers o anade juegos externos */
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && list.count < MAX_GAMES) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
            char *nl = p + strlen(p) - 1;
            while (nl >= p && (*nl == '\n' || *nl == '\r')) *nl-- = '\0';

            GameEntry tmp;
            memset(&tmp, 0, sizeof(tmp));
            char raw_cover[128] = "", raw_bg[128] = "", raw_logo[128] = "", raw_icon[128] = "";

            field(tmp.title, sizeof(tmp.title), &p);
            field(tmp.elf_path, sizeof(tmp.elf_path), &p);
            if (!*tmp.title || !*tmp.elf_path) continue;
            field(raw_cover, sizeof(raw_cover), &p);
            field(raw_bg, sizeof(raw_bg), &p);
            field(raw_logo, sizeof(raw_logo), &p);
            field(raw_icon, sizeof(raw_icon), &p);
            field(tmp.genre, sizeof(tmp.genre), &p);
            field(tmp.year, sizeof(tmp.year), &p);
            field(tmp.description, sizeof(tmp.description), &p);

            char cov[128], logo[128];
            fix_asset_path(cov, sizeof(cov), raw_cover);
            fix_asset_path(logo, sizeof(logo), raw_logo);

            /* ruta relativa -> prefijar con el medio actual */
            char elf_full[256];
            if (!strchr(tmp.elf_path, ':'))
                snprintf(elf_full, sizeof(elf_full), "%s%s", media_prefix, tmp.elf_path);
            else
                snprintf(elf_full, sizeof(elf_full), "%s", tmp.elf_path);

            /* para LoadExecPS2 el device por defecto puede no resolver:
               en ISO/PS2 real usar cdrom0: explicito */
            if (!strchr(elf_full, ':')) {
                char cd[280];
                snprintf(cd, sizeof(cd), "cdrom0:%s", elf_full);
                snprintf(elf_full, sizeof(elf_full), "%s", cd);
            }

            add_game_full(tmp.title, elf_full, cov, logo,
                          tmp.genre, tmp.year, tmp.description);
        }
        fclose(f);
    }

    gs_log("games.cfg: %d juegos totales", list.count);

    sort_games();
    list.page = list.selected / list.games_per_page;
}

/* ---------- layout ---------- */
#define CELL_W 128
#define CELL_H 168
#define CARD_R 12  /* radio de esquina */
#define GRID_X 64
#define GRID_Y 48

static float cell_target_x(int idx)
{
    int rel = idx - list.page * list.games_per_page;
    int col = rel % GRID_COLS;
    int row = rel / GRID_COLS;
    return GRID_X + col * CELL_W;
}

static float cell_target_y(int idx)
{
    int rel = idx - list.page * list.games_per_page;
    int row = rel / GRID_COLS;
    return GRID_Y + row * CELL_H;
}

/* ---------- input ---------- */

int ui_handle_input(unsigned short btn, unsigned short pressed)
{
    (void)btn;
    int max_sel = list.count - 1;

    if (pressed & PAD_LEFT) {
        if (list.selected % GRID_COLS > 0) list.selected--;
        else if (list.selected >= GRID_COLS) list.selected -= GRID_COLS;
    }
    if (pressed & PAD_RIGHT) {
        if ((list.selected % GRID_COLS) < GRID_COLS - 1 && list.selected < max_sel) list.selected++;
        else if (list.selected + GRID_COLS <= max_sel) list.selected += GRID_COLS;
    }
    if (pressed & PAD_UP) {
        if (list.selected >= GRID_COLS) list.selected -= GRID_COLS;
    }
    if (pressed & PAD_DOWN) {
        if (list.selected + GRID_COLS <= max_sel) list.selected += GRID_COLS;
    }
    if (pressed & PAD_R1) {
        if ((list.page + 1) * list.games_per_page < list.count) {
            list.page++;
            list.selected = list.page * list.games_per_page;
            if (list.selected > max_sel) list.selected = max_sel;
        }
    }
    if (pressed & PAD_L1) {
        if (list.page > 0) {
            list.page--;
            list.selected = list.page * list.games_per_page;
        }
    }
    if (pressed & PAD_START) {
        list.selected = 0; list.page = 0;
    }

    /* clamp a pagina */
    int start = list.page * list.games_per_page;
    int end = start + list.games_per_page;
    if (end > list.count) end = list.count;
    if (list.selected < start) list.selected = start;
    if (list.selected >= end) list.selected = end - 1;

    /* nueva pagina si el seleccionado se sale */
    int newpage = list.selected / list.games_per_page;
    if (newpage != list.page) list.page = newpage;

    return (pressed & PAD_CROSS) ? 1 : 0;
}

/* ---------- update ---------- */
void ui_update(float dt)
{
    ui.time += dt;
    ui.dt = dt;
    ui.bg_anim += dt * 0.05f;
    ui.particle_time += dt;

    if (ui.error_time > 0.0f) {
        ui.error_time -= dt;
        if (ui.error_time <= 0.0f)
            ui.error_msg[0] = '\0';
    }

    float k = 1.0f - powf(0.0015f, dt); /* suavizado exponencial */

    for (int i = 0; i < list.count; i++) {
        GameEntry *g = &list.games[i];
        int rel = i - list.page * list.games_per_page;
        int on_page = (rel >= 0 && rel < list.games_per_page);
        int is_sel = (i == list.selected);

        float tx = on_page ? cell_target_x(i) : (rel < 0 ? GRID_X - CELL_W * 1.5f : GRID_X + CELL_W * (GRID_COLS + 1));
        float ty = on_page ? cell_target_y(i) : cell_target_y(list.page * list.games_per_page);
        float ts = is_sel && on_page ? 1.12f : (on_page ? 1.0f : 0.6f);
        float ta = on_page ? 1.0f : 0.0f;

        g->target_x = tx; g->target_y = ty;
        g->target_scale = ts; g->target_alpha = ta;

        g->anim_x = lerp(g->anim_x, g->target_x, k);
        g->anim_y = lerp(g->anim_y, g->target_y, k);
        g->anim_scale = lerp(g->anim_scale, g->target_scale, k);
        g->anim_alpha = lerp(g->anim_alpha, g->target_alpha, k);

        /* bobbing sutil en seleccionado */
        if (is_sel && on_page) {
            g->anim_y += sinf(ui.time * 3.0f) * 1.5f;
        }
    }
}

/* ---------- render ---------- */
static void draw_text(int x, int y, const char *s, u32 color)
{
    int cx = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 32 && c < 128) {
            font_draw_glyph(cx, y, c, color);
        }
        cx += 8;
    }
}

static void draw_text_centered(int cx, int y, const char *s, u32 color)
{
    int w = (int)strlen(s) * 8;
    draw_text(cx - w / 2, y, s, color);
}

static void draw_text_big(int x, int y, const char *s, u32 color)
{
    /* escala 2x */
    int cx = x;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 32 && c < 128) {
            font_draw_glyph_scaled(cx, y, c, color, 2);
        }
        cx += 16;
    }
}

static void draw_cell(GameEntry *g, int idx)
{
    float a = g->anim_alpha;
    if (a < 0.02f) return;
    int is_sel = (idx == list.selected);

    float w = CELL_W * g->anim_scale - 8;
    float h = CELL_H * g->anim_scale - 8;
    float x = g->anim_x - (w - (CELL_W - 8)) * 0.5f;
    float y = g->anim_y - (h - (CELL_H - 8)) * 0.5f;

    /* sombra */
    GsColor shadow = {0, 0, 0, 0.55f * a};
    gs_draw_rounded_rect(x + 4, y + 5, w, h, CARD_R, shadow);

    /* panel base (vertical/portrait) */
    GsColor panel = is_sel ? (GsColor){0.22f, 0.30f, 0.58f, 1.0f * a}
                           : (GsColor){0.09f, 0.10f, 0.16f, 0.98f * a};
    gs_draw_rounded_rect(x, y, w, h, CARD_R, panel);

    /* borde del seleccionado: anillo pulsante */
    if (is_sel) {
        float pulse = 0.6f + 0.4f * sinf(ui.time * 5.0f);
        GsColor border = {0.55f + 0.35f * pulse, 0.70f + 0.25f * pulse, 1.0f, 1.0f * a};
        gs_draw_rounded_rect(x - 2, y - 2, w + 4, h + 4, CARD_R + 2, border);
        gs_draw_rounded_rect(x + 1, y + 1, w - 2, h - 2, CARD_R - 1, panel);
    }

    /* cover portrait */
    GsTexture *cover = assets_get(g->cover);
    float avail_w = w - 12;
    float avail_h = h - 30;
    if (cover) {
        float cw = avail_w;
        float ch = cw * cover->h / cover->w;
        if (ch > avail_h) { ch = avail_h; cw = ch * cover->w / cover->h; }
        gs_draw_sprite_ex(x + (w - cw) * 0.5f, y + 6, cw, ch,
                          cover, 0, 0, (float)cover->w, (float)cover->h,
                          (GsColor){1, 1, 1, 1}, 0, a);
    } else {
        /* placeholder con inicial */
        GsColor ph = {0.15f, 0.17f, 0.25f, 1.0f * a};
        gs_draw_rounded_rect(x + 6, y + 6, w - 12, avail_h, CARD_R - 4, ph);
        char init[2] = { g->title[0] ? g->title[0] : '?', 0 };
        u32 col = 0xFFA0C0E0u;
        int ix = (int)(x + w * 0.5f - 8);
        int iy = (int)(y + 6 + (avail_h * 0.5f) - 8);
        draw_text_big(ix, iy, init, col);
    }

    /* titulo (recortado) */
    char title[32];
    int maxch = (int)(w - 8) / 8;
    if (maxch > 31) maxch = 31;
    int n = (int)strlen(g->title);
    if (n > maxch) { memcpy(title, g->title, maxch); title[maxch] = 0; }
    else memcpy(title, g->title, n + 1);
    u32 tcol = is_sel ? 0xFFFFFF00u : 0xC0C8D8FFu;
    draw_text_centered((int)(x + w * 0.5f), (int)(y + h - 16), title, tcol);
}

/* fondo global: ART/BG/BASE con cualquier extension/case, fallback DEFAULT.
   se resuelve UNA sola vez y se pinea (sin fopen por frame) */
static GsTexture *load_base_bg(void)
{
    static int resolved = 0;
    static GsTexture *cached = NULL;
    if (!resolved) {
        resolved = 1;
        static const char *cands[] = {
            "ART/BG/BASE.JPG", "ART/BG/BASE.jpg", "ART/BG/BASE.JPEG", "ART/BG/BASE.jpeg",
            "ART/BG/BASE.PNG", "ART/BG/BASE.png", "ART/BG/BASE.BMP", "ART/BG/BASE.bmp",
            "ART/BG/DEFAULT.JPG", "ART/BG/DEFAULT.jpg",
        };
        for (int i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
            if (pref_exists(cands[i])) {
                cached = assets_pin(cands[i]);
                if (cached) {
                    gs_log("fondo global: %s", cands[i]);
                    break;
                }
            }
        }
    }
    return cached;
}

void ui_render(void)
{
    /* fondo global unico: ART/BG/BASE.JPG (o .PNG/.BMP) */
    GameEntry *sel = &list.games[list.selected];

    GsTexture *base = load_base_bg();
    if (base) {
        gs_clear((GsColor){0.02f, 0.02f, 0.06f, 1});
        gs_draw_sprite_ex(0, 0, SCREEN_W, SCREEN_H, base, 0, 0,
                          (float)base->w, (float)base->h, (GsColor){1, 1, 1, 1}, 0, 1.0f);
    } else {
        /* sin fondo: gradiente animado */
        GsColor bg1 = {0.03f, 0.04f, 0.09f, 1};
        float p = 0.5f + 0.5f * sinf(ui.bg_anim);
        GsColor c2 = {0.08f + 0.04f * p, 0.10f + 0.05f * p, 0.22f + 0.08f * p, 1};
        gs_clear(bg1);
        gs_draw_rect_gradient(0, 0, SCREEN_W, SCREEN_H, bg1, c2, 1);
    }

    /* barra superior */
    GsColor topbar = {0.02f, 0.02f, 0.05f, 0.85f};
    gs_draw_rect(0, 0, SCREEN_W, 30, topbar);
    draw_text(8, 11, "PORTMENU", 0xFF60A0FFu);
    char pageinfo[32];
    snprintf(pageinfo, sizeof(pageinfo), "%d/%d  pag %d/%d",
             list.selected + 1, list.count, list.page + 1,
             (list.count + list.games_per_page - 1) / list.games_per_page);
    draw_text(SCREEN_W - 8 - (int)strlen(pageinfo) * 8, 11, pageinfo, 0xFFA0A0A0u);

    /* celdas */
    for (int i = 0; i < list.count; i++)
        draw_cell(&list.games[i], i);

    /* panel inferior con info del seleccionado */
    GsColor botbar = {0.02f, 0.02f, 0.06f, 0.88f};
    gs_draw_rect_gradient(0, SCREEN_H - 40, SCREEN_W, 40,
                          (GsColor){0.01f, 0.01f, 0.04f, 0.0f}, botbar, 1);
    draw_text(8, SCREEN_H - 30, sel->title, 0xFFFFFF00u);
    draw_text_centered(SCREEN_W / 2, SCREEN_H - 23, "X: jugar   L1/R1: pagina   START: inicio", 0xFF788098u);

    /* logo del juego (esquina) */
    GsTexture *logo = assets_get(sel->logo);
    if (logo) {
        float lw = 150;
        float lh = lw * logo->h / logo->w;
        if (lh > 28) { lh = 28; lw = lh * logo->w / logo->h; }
        gs_draw_sprite_ex(SCREEN_W - lw - 10, SCREEN_H - lh - 6, lw, lh, logo,
                          0, 0, (float)logo->w, (float)logo->h, (GsColor){1,1,1,1}, 0, 0.95f);
    }

    /* mensaje de error (ELF no encontrado) */
    if (ui.error_msg[0]) {
        float t = ui.error_time;
        if (t > 1.0f) t = 1.0f;
        GsColor panel = {0.55f, 0.10f, 0.10f, 0.92f * t};
        int msgw = (int)strlen(ui.error_msg) * 8 + 24;
        gs_draw_rect((SCREEN_W - msgw) / 2, 60, msgw, 22, panel);
        draw_text_centered(SCREEN_W / 2, 67, ui.error_msg, 0xFFFFFFFFu);
    }
}

/* ---------- acceso ---------- */
GameEntry *ui_get_selected(void) { return &list.games[list.selected]; }
int ui_get_selected_idx(void) { return list.selected; }
int ui_get_game_count(void) { return list.count; }

void ui_launch_selected(void)
{
    GameEntry *g = ui_get_selected();

    /* verificar que el ELF exista antes de reiniciar la consola */
    int exists = 0;
    FILE *f = fopen(g->elf_path, "rb");
    if (f) { fclose(f); exists = 1; }

    if (!exists) {
        snprintf(ui.error_msg, sizeof(ui.error_msg),
                 "ELF no encontrado: %s", g->elf_path);
        ui.error_time = 3.0f;
        gs_log("launch: NO EXISTE %s", g->elf_path);
        return;
    }

    gs_log("launch: %s (%s)", g->title, g->elf_path);
    ui_save_config();
    ui_shutdown();
    /* pasar la ruta como argv[0]: los ports derivan la carpeta de sus datos de ahi.
       ruta sin device -> cdrom0: (medio de arranque en ISO/PS2 real) */
    {
        char full[280];
        const char *elf = g->elf_path;
        if (!strchr(elf, ':')) {
            snprintf(full, sizeof(full), "cdrom0:%s", elf);
            elf = full;
        }
        char *args[1];
        args[0] = (char *)elf;
        LoadExecPS2(elf, 1, args);
    }
}

void ui_init(void)
{
    memset(&list, 0, sizeof(list));
    memset(&ui, 0, sizeof(ui));
    for (int i = 0; i < MAX_GAMES; i++) {
        list.games[i].anim_scale = 0.6f;
        list.games[i].anim_alpha = 0.0f;
    }
}

void ui_shutdown(void)
{
    assets_shutdown();
}