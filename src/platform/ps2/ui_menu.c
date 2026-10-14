// ui_menu.c - grid animado con transiciones, covers, fondos y lanzamiento de ELFs
// Los juegos se autodetectan escaneando ELFS/: el cover/logo se llama igual que el ELF.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <dirent.h>

#include "ui_menu.h"
#include "gs_renderer.h"
#include "asset_manager.h"
#include "font8x8.h"

extern void LoadExecPS2(const char *filename, int num_args, char *args[]);

/* LoadExecPS2 internamente resetea el IOP después de leer el ELF */
extern void FlushCache(int mode);  /* 0=writeback, 2=invalidate */

#define PAD_SELECT   0x0001
#define PAD_START    0x0008
#define PAD_UP       0x0010
#define PAD_RIGHT    0x0020
#define PAD_DOWN     0x0040
#define PAD_LEFT     0x0080
#define PAD_L2       0x0100
#define PAD_R2       0x0200
#define PAD_L1       0x0400
#define PAD_R1       0x0800
#define PAD_TRIANGLE 0x1000
#define PAD_CIRCLE   0x2000
#define PAD_CROSS    0x4000
#define PAD_SQUARE   0x8000

#define LANG_ES 0
#define LANG_EN 1
#define LANG_PT 2
#define LANG_COUNT 3

static const char *lang_names[LANG_COUNT] = {
    "ESPANOL", "ENGLISH", "PORTUGUES"
};

static const char *sort_names[LANG_COUNT][3] = {
    { "PERSONALIZADO", "ALFABETICO (A-Z)", "ALFABETICO (Z-A)" },
    { "CUSTOM",        "ALPHABETICAL (A-Z)", "ALPHABETICAL (Z-A)" },
    { "PERSONALIZADO", "ALFABETICO (A-Z)", "ALFABETICO (Z-A)" }
};

static const char *txt_settings_title[LANG_COUNT] = {
    "CONFIGURACION", "SETTINGS", "CONFIGURACOES"
};

static const char *txt_opt_lang[LANG_COUNT] = {
    "IDIOMA", "LANGUAGE", "IDIOMA"
};

static const char *txt_opt_sort[LANG_COUNT] = {
    "ORDEN", "SORT", "ORDEM"
};

static const char *txt_opt_move[LANG_COUNT] = {
    "MOVER JUEGO", "MOVE GAME", "MOVER JOGO"
};

static const char *txt_opt_save[LANG_COUNT] = {
    "GUARDAR Y SALIR", "SAVE AND EXIT", "SALVAR E SAIR"
};

static const char *txt_hint_play[LANG_COUNT] = {
    "X: JUGAR     < >: ELEGIR",
    "X: PLAY      < >: SELECT",
    "X: JOGAR     < >: ESCOLHER"
};

static const char *txt_hint_nav[LANG_COUNT] = {
    "L1/R1: RAPIDO   SELECT: AJUSTES",
    "L1/R1: FAST     SELECT: SETTINGS",
    "L1/R1: RAPIDO   SELECT: CONFIG"
};

static const char *txt_modal_hints[LANG_COUNT] = {
    "^ v: OPCION   < >: CAMBIAR   X: CONFIRMAR   /\\: SALIR",
    "^ v: OPTION   < >: CHANGE    X: CONFIRM     /\\: EXIT",
    "^ v: OPCAO    < >: MUDAR     X: CONFIRMAR   /\\: SAIR"
};

static const char *txt_port_ps2[LANG_COUNT] = {
    "PORT PS2", "PS2 PORT", "PORT PS2"
};

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

/* ---------- ordenacion y swaps ---------- */
static void swap_games(int a, int b)
{
    if (a < 0 || a >= list.count || b < 0 || b >= list.count || a == b) return;
    GameEntry tmp = list.games[a];
    list.games[a] = list.games[b];
    list.games[b] = tmp;
}

static void sort_games_az(void)
{
    for (int i = 0; i < list.count - 1; i++)
        for (int j = 0; j < list.count - 1 - i; j++)
            if (strcmp(list.games[j].title, list.games[j + 1].title) > 0)
                swap_games(j, j + 1);
}

static void sort_games_za(void)
{
    for (int i = 0; i < list.count - 1; i++)
        for (int j = 0; j < list.count - 1 - i; j++)
            if (strcmp(list.games[j].title, list.games[j + 1].title) < 0)
                swap_games(j, j + 1);
}

static void sort_games(void)
{
    sort_games_az();
}

/* ---------- config persistente ---------- */
static char media_prefix[16] = "cdrom0:\\"; /* medio desde donde se arranca (por defecto cdrom0: en PS2 real/OPL) */

void ui_set_boot_path(const char *path)
{
    if (!path || !*path) return;
    gs_log("boot: argv[0] = '%s'", path);
    if (strncmp(path, "cdrom", 5) == 0) {
        strcpy(media_prefix, "cdrom0:\\");
    } else if (strncmp(path, "mass", 4) == 0) {
        strcpy(media_prefix, "mass:/");
    } else if (strncmp(path, "mc", 2) == 0) {
        strcpy(media_prefix, "mc0:");
    }
}

void ui_load_config(void)
{
    const char *paths[] = {
        "mc0:PORTMENU/portmenu.cfg",
        "mc0:portmenu.cfg",
        "mass:portmenu.cfg",
        "portmenu.cfg"
    };
    FILE *f = NULL;
    for (int i = 0; i < (int)(sizeof(paths)/sizeof(paths[0])); i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }
    if (!f) return;

    char line[512];
    int saved_sel = -1;
    char saved_order[MAX_GAMES][256];
    int order_count = 0;

    while (fgets(line, sizeof(line), f)) {
        char *nl = line + strlen(line) - 1;
        while (nl >= line && (*nl == '\n' || *nl == '\r')) *nl-- = '\0';

        if (strncmp(line, "lang=", 5) == 0) {
            ui.language = atoi(line + 5);
            if (ui.language < 0 || ui.language >= LANG_COUNT) ui.language = 0;
        } else if (strncmp(line, "selected=", 9) == 0) {
            saved_sel = atoi(line + 9);
        } else if (strncmp(line, "sort=", 5) == 0) {
            ui.sort_mode = atoi(line + 5);
            if (ui.sort_mode < 0 || ui.sort_mode > 2) ui.sort_mode = 0;
        } else if (strncmp(line, "game", 4) == 0) {
            char *eq = strchr(line, '=');
            if (eq && order_count < MAX_GAMES) {
                strncpy(saved_order[order_count], eq + 1, sizeof(saved_order[order_count]) - 1);
                saved_order[order_count][sizeof(saved_order[order_count]) - 1] = 0;
                order_count++;
            }
        } else if (atoi(line) > 0 && saved_sel < 0) {
            saved_sel = atoi(line);
        }
    }
    fclose(f);

    /* aplicar orden de juegos guardado */
    if (ui.sort_mode == 0 && order_count > 0 && list.count > 0) {
        GameEntry new_list[MAX_GAMES];
        int used[MAX_GAMES] = {0};
        int new_count = 0;

        for (int o = 0; o < order_count; o++) {
            for (int g = 0; g < list.count; g++) {
                if (!used[g] && strcmp(saved_order[o], list.games[g].elf_path) == 0) {
                    new_list[new_count++] = list.games[g];
                    used[g] = 1;
                    break;
                }
            }
        }
        for (int g = 0; g < list.count; g++) {
            if (!used[g]) {
                new_list[new_count++] = list.games[g];
            }
        }
        memcpy(list.games, new_list, sizeof(GameEntry) * list.count);
    } else if (ui.sort_mode == 1) {
        sort_games_az();
    } else if (ui.sort_mode == 2) {
        sort_games_za();
    }

    if (saved_sel >= 0 && saved_sel < list.count)
        list.selected = saved_sel;

    gs_log("config: cargado ok (lang=%d, sel=%d, sort=%d)", ui.language, list.selected, ui.sort_mode);
}

void ui_save_config(void)
{
    if (list.count <= 0) return;
    const char *paths[] = {
        "mc0:PORTMENU/portmenu.cfg",
        "mc0:portmenu.cfg",
        "mass:portmenu.cfg",
        "portmenu.cfg"
    };
    FILE *f = NULL;
    for (int i = 0; i < (int)(sizeof(paths)/sizeof(paths[0])); i++) {
        f = fopen(paths[i], "w");
        if (f) break;
    }
    if (!f) return;

    fprintf(f, "lang=%d\n", ui.language);
    fprintf(f, "selected=%d\n", list.selected);
    fprintf(f, "sort=%d\n", ui.sort_mode);
    fprintf(f, "count=%d\n", list.count);
    for (int i = 0; i < list.count; i++) {
        fprintf(f, "game%d=%s\n", i, list.games[i].elf_path);
    }
    fclose(f);
    gs_log("config: guardado ok (lang=%d, sel=%d, sort=%d)", ui.language, list.selected, ui.sort_mode);
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
    if (strncmp(val, "ART/", 4) == 0 || strncmp(val, "ART\\", 4) == 0) {
        strncpy(dst, val, sz - 1); dst[sz-1]=0; return;
    }
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
    /* Probar con '\' para cdrom */
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    /* Probar con ';1' */
    if (!strchr(p, ';')) {
        strcat(p, ";1");
        f = fopen(p, "rb");
        if (f) { fclose(f); return 1; }
    }
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

/* ---------- diagnostico en pantalla (para PS2 real sin host/log) ---------- */
static char diag_lines[18][100];
static int diag_n = 0;

static void diag_add(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag_lines[diag_n % 18], 100, fmt, ap);
    va_end(ap);
    diag_n++;
}

static void scan_games(void)
{
    /* colectar covers y logos disponibles primero */
    collect_art("ART/COV", g_covs, 128, &g_ncovs);
    collect_art("ART/LOGO", g_logos, 128, &g_nlogos);
    gs_log("scan: %d covers, %d logos disponibles", g_ncovs, g_nlogos);
    diag_add("scan: %d covers, %d logos", g_ncovs, g_nlogos);

    /* variantes de ruta para el device cdrom real (backslash nativo) */
    const char *dirs[6];
    int ndirs = 0;
    char p1[64], p2[64];
    if (media_prefix[0]) {
        snprintf(p1, sizeof(p1), "%sELFS", media_prefix);
        snprintf(p2, sizeof(p2), "%s/ELFS", media_prefix);
        dirs[ndirs++] = p1;
        dirs[ndirs++] = p2;
    } else {
        /* device por defecto (cdrom0: en ISO/PS2 real): probar variantes */
        dirs[ndirs++] = "ELFS";
        dirs[ndirs++] = "/ELFS";
        dirs[ndirs++] = "cdrom0:ELFS";
        dirs[ndirs++] = "cdrom0:\\ELFS";
        dirs[ndirs++] = "cdrom:ELFS";
        dirs[ndirs++] = "cdrom:\\ELFS";
    }

    DIR *d = NULL;
    char dirp[256] = "";
    for (int i = 0; i < ndirs && !d; i++) {
        d = opendir(dirs[i]);
        diag_add("dopen %s -> %s", dirs[i], d ? "OK" : "fail");
        if (d) snprintf(dirp, sizeof(dirp), "%s", dirs[i]);
    }
    if (!d) {
        gs_log("scan: sin carpeta ELFS en %s", media_prefix);
        diag_add("sin ELFS en %s", media_prefix[0] ? media_prefix : "<arranque>");
        return;
    }
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (found == 0) diag_add("readdir 1a: %s", e->d_name);
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
    diag_add("%d ELFs en %s", found, dirp);
}

void ui_load_games(void)
{
    list.count = 0;
    list.selected = 0;
    list.page = 0;
    list.games_per_page = MAX_VISIBLE;

    /* 1) medio: buscar GAMES.CFG */
    static const char *candidates[] = {
        "GAMES.CFG", "games.cfg",
        "cdrom0:\\GAMES.CFG", "cdrom0:\\GAMES.CFG;1",
        "cdrom0:GAMES.CFG", "cdrom0:GAMES.CFG;1",
        "cdrom:\\GAMES.CFG",
        "mass:/games.cfg", "mass:/GAMES.CFG",
        "mc0:games.cfg", "mc0:/GAMES.CFG",
        "cdfs:/GAMES.CFG", "cdfs:/games.cfg",
    };
    FILE *f = NULL;
    const char *found = NULL;
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        f = fopen(candidates[i], "r");
        if (f) { found = candidates[i]; break; }
    }
    if (!f) {
        gs_log("games.cfg: no encontrado");
        strcpy(media_prefix, "cdrom0:\\");
    } else {
        if (strncmp(found, "cdfs:", 5) == 0) strcpy(media_prefix, "cdfs:/");
        else if (strncmp(found, "cdrom", 5) == 0) strcpy(media_prefix, "cdrom0:\\");
        else if (strncmp(found, "mass:", 5) == 0) strcpy(media_prefix, "mass:/");
        else if (strncmp(found, "mc0:", 4) == 0) strcpy(media_prefix, "mc0:");
        else strcpy(media_prefix, "cdrom0:\\"); /* por defecto en disc */
        gs_log("games.cfg: %s (medio %s)", found, media_prefix);
    }

    /* 2) Si se encontro games.cfg, leerlo */
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

            /* Si es cdrom0:, normalizar todos los '/' a '\\' */
            if (strncmp(elf_full, "cdrom", 5) == 0) {
                for (char *c = elf_full; *c; c++) {
                    if (*c == '/') *c = '\\';
                }
            }

            add_game_full(tmp.title, elf_full, cov, logo,
                          tmp.genre, tmp.year, tmp.description);
        }
        fclose(f);
    }

    /* 3) Si games.cfg no tenia juegos, auto-escanear dispositivos compatibles */
    if (list.count == 0) {
        strcpy(media_prefix, "cdrom0:\\"); scan_games();
        if (list.count == 0) { strcpy(media_prefix, "mass:/"); scan_games(); }
        if (list.count == 0) { strcpy(media_prefix, "mc0:"); scan_games(); }
    }

    gs_log("games.cfg: %d juegos totales", list.count);

    sort_games();
    list.page = list.selected / list.games_per_page;
}

/* ---------- layout carrusel Aurora ---------- */
#define CAR_SPACING 140
#define CAR_FLOOR_Y 352

/* ---------- input ---------- */

int ui_handle_input(unsigned short btn, unsigned short pressed)
{
    (void)btn;
    int max_sel = list.count - 1;

    /* si el modal de ajustes esta abierto */
    if (ui.show_settings) {
        if (pressed & (PAD_SELECT | PAD_TRIANGLE | PAD_CIRCLE)) {
            ui.show_settings = 0;
            ui_save_config();
            return 0;
        }
        if (pressed & PAD_UP) {
            ui.settings_sel = (ui.settings_sel + 3) % 4;
        }
        if (pressed & PAD_DOWN) {
            ui.settings_sel = (ui.settings_sel + 1) % 4;
        }
        if (pressed & (PAD_LEFT | PAD_L1)) {
            if (ui.settings_sel == 0) {
                ui.language = (ui.language + LANG_COUNT - 1) % LANG_COUNT;
            } else if (ui.settings_sel == 1) {
                ui.sort_mode = (ui.sort_mode + 2) % 3;
                if (ui.sort_mode == 1) sort_games_az();
                else if (ui.sort_mode == 2) sort_games_za();
            } else if (ui.settings_sel == 2) {
                if (list.selected > 0) {
                    swap_games(list.selected, list.selected - 1);
                    list.selected--;
                    ui.sort_mode = 0;
                }
            }
        }
        if (pressed & (PAD_RIGHT | PAD_R1)) {
            if (ui.settings_sel == 0) {
                ui.language = (ui.language + 1) % LANG_COUNT;
            } else if (ui.settings_sel == 1) {
                ui.sort_mode = (ui.sort_mode + 1) % 3;
                if (ui.sort_mode == 1) sort_games_az();
                else if (ui.sort_mode == 2) sort_games_za();
            } else if (ui.settings_sel == 2) {
                if (list.selected < max_sel) {
                    swap_games(list.selected, list.selected + 1);
                    list.selected++;
                    ui.sort_mode = 0;
                }
            }
        }
        if (pressed & PAD_CROSS) {
            if (ui.settings_sel == 3) {
                ui.show_settings = 0;
                ui_save_config();
            } else if (ui.settings_sel == 0) {
                ui.language = (ui.language + 1) % LANG_COUNT;
            } else if (ui.settings_sel == 1) {
                ui.sort_mode = (ui.sort_mode + 1) % 3;
                if (ui.sort_mode == 1) sort_games_az();
                else if (ui.sort_mode == 2) sort_games_za();
            }
        }
        return 0;
    }

    /* modo normal (carrusel) */
    if (pressed & (PAD_SELECT | PAD_TRIANGLE)) {
        ui.show_settings = 1;
        ui.settings_sel = 0;
        return 0;
    }

    /* atajos para reordenar directamente en carrusel con L2 y R2 */
    if (pressed & PAD_L2) {
        if (list.selected > 0) {
            swap_games(list.selected, list.selected - 1);
            list.selected--;
            ui.sort_mode = 0;
            ui_save_config();
        }
    }
    if (pressed & PAD_R2) {
        if (list.selected < max_sel) {
            swap_games(list.selected, list.selected + 1);
            list.selected++;
            ui.sort_mode = 0;
            ui_save_config();
        }
    }

    /* navegacion carrusel */
    if (pressed & PAD_LEFT) {
        if (list.selected > 0) list.selected--;
    }
    if (pressed & PAD_RIGHT) {
        if (list.selected < max_sel) list.selected++;
    }
    if (pressed & PAD_L1) {
        list.selected -= 5;
        if (list.selected < 0) list.selected = 0;
    }
    if (pressed & PAD_R1) {
        list.selected += 5;
        if (list.selected > max_sel) list.selected = max_sel;
    }
    if (pressed & PAD_START) {
        list.selected = 0;
    }

    if (list.selected < 0) list.selected = 0;
    if (list.selected > max_sel) list.selected = max_sel;
    list.page = list.selected / list.games_per_page;

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

    /* carrusel Aurora: posicion/escala/alpha segun distancia al seleccionado */
    for (int i = 0; i < list.count; i++) {
        GameEntry *g = &list.games[i];
        float d = (float)(i - list.selected);
        float ad = (d < 0) ? -d : d;

        float s_t = (ad < 0.5f) ? 1.00f : (ad < 1.5f) ? 0.74f : (ad < 2.5f) ? 0.54f : 0.40f;
        float a_t = (ad < 3.0f) ? 1.0f : (ad < 4.0f) ? 0.45f : 0.0f;

        g->target_scale = s_t;
        g->target_alpha = a_t;
        g->target_x = SCREEN_W * 0.5f + d * CAR_SPACING;
        g->target_y = CAR_FLOOR_Y;

        g->anim_x = lerp(g->anim_x, g->target_x, k);
        g->anim_y = lerp(g->anim_y, g->target_y, k);
        g->anim_scale = lerp(g->anim_scale, g->target_scale, k);
        g->anim_alpha = lerp(g->anim_alpha, g->target_alpha, k);
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

static void draw_text_big_centered(int cx, int y, const char *s, u32 color)
{
    int w = (int)strlen(s) * 16;
    draw_text_big(cx - w / 2, y, s, color);
}

/* carrusel Aurora: un cover flotando sobre el piso, escala por distancia */
static void draw_carousel_item(GameEntry *g, int idx)
{
    float a = g->anim_alpha;
    if (a < 0.02f) return;
    int is_sel = (idx == list.selected);
    float scale = g->anim_scale;

    /* box maximo escalado por distancia, cover con aspect fit */
    float maxw = 230.0f * scale;
    float maxh = 295.0f * scale;
    float tw = maxw, th = maxh;

    GsTexture *cover = assets_get(g->cover);
    if (cover) {
        float asp = cover->w / (float)cover->h;
        th = maxh;
        tw = th * asp;
        if (tw > maxw) { tw = maxw; th = tw / asp; }
    } else {
        tw = maxw * 0.72f;
        th = maxh;
    }

    float bob = is_sel ? sinf(ui.time * 2.5f) * 4.0f : 0.0f;
    float cx = g->anim_x;
    float floor_y = g->anim_y + bob;
    float left = cx - tw * 0.5f;
    float top = floor_y - th;

    /* Culling estricto: si el cover esta totalmente fuera de la pantalla, no se dibuja */
    if (left + tw <= 0.0f || left >= (float)SCREEN_W || floor_y <= 0.0f || top >= (float)SCREEN_H)
        return;

    /* cover (laterales oscurecidos) */
    if (cover) {
        GsColor tint = is_sel ? (GsColor){1, 1, 1, 1}
                              : (GsColor){0.52f, 0.55f, 0.62f, 1};
        gs_draw_sprite_ex(left, top, tw, th, cover, 0, 0,
                          (float)cover->w, (float)cover->h, tint, 0, a);
    } else {
        GsColor ph = is_sel ? (GsColor){0.16f, 0.19f, 0.30f, 1.0f * a}
                            : (GsColor){0.09f, 0.10f, 0.15f, 0.9f * a};
        gs_draw_rounded_rect(left, top, tw, th, 10, ph);
        char init[2] = { g->title[0] ? g->title[0] : '?', 0 };
        draw_text_big((int)(cx - 8 * scale), (int)(top + th * 0.5f - 8 * scale), init, 0xFFA0C0E0u);
    }

    /* marco del seleccionado */
    if (is_sel) {
        float pulse = 0.6f + 0.4f * sinf(ui.time * 5.0f);
        GsColor border = {0.45f + 0.35f * pulse, 0.65f + 0.25f * pulse, 1.0f, 0.95f * a};
        float bw = 2.5f;
        gs_draw_rect(left - bw, top - bw, tw + bw * 2, bw, border);
        gs_draw_rect(left - bw, top + th, tw + bw * 2, bw, border);
        gs_draw_rect(left - bw, top, bw, th, border);
        gs_draw_rect(left + tw, top, bw, th, border);
    }

    /* reflejo en el piso (fade) */
    GsColor rf1 = {0.6f, 0.7f, 1.0f, 0.10f * a};
    GsColor rf2 = {0.6f, 0.7f, 1.0f, 0.0f};
    gs_draw_rect_gradient(left, floor_y + 2, tw, 18 * scale, rf1, rf2, 1);
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
            cached = assets_pin(cands[i]);
            if (cached) {
                gs_log("fondo global: %s", cands[i]);
                break;
            }
        }
    }
    return cached;
}

static void draw_settings_modal(void)
{
    int lang = ui.language;
    if (lang < 0 || lang >= LANG_COUNT) lang = 0;

    /* overlay oscuro translucido */
    gs_draw_rect(0, 0, SCREEN_W, SCREEN_H, (GsColor){0.0f, 0.0f, 0.02f, 0.75f});

    /* ventana modal centrada */
    float mw = 480.0f;
    float mh = 260.0f;
    float mx = (SCREEN_W - mw) * 0.5f;
    float my = (SCREEN_H - mh) * 0.5f;

    /* sombra */
    gs_draw_rounded_rect(mx + 4, my + 6, mw, mh, 12, (GsColor){0, 0, 0, 0.60f});
    /* panel principal */
    gs_draw_rounded_rect(mx, my, mw, mh, 12, (GsColor){0.06f, 0.08f, 0.16f, 0.98f});

    /* borde brillante */
    float bw = 2.0f;
    GsColor border_col = {0.25f, 0.45f, 0.85f, 0.90f};
    gs_draw_rect(mx - bw, my - bw, mw + bw * 2, bw, border_col);
    gs_draw_rect(mx - bw, my + mh, mw + bw * 2, bw, border_col);
    gs_draw_rect(mx - bw, my, bw, mh, border_col);
    gs_draw_rect(mx + mw, my, bw, mh, border_col);

    /* titulo del modal */
    draw_text_big_centered(SCREEN_W / 2, (int)(my + 16), txt_settings_title[lang], 0xFFFFFFFFu);
    gs_draw_rect(mx + 20, my + 38, mw - 40, 1.5f, (GsColor){0.30f, 0.40f, 0.70f, 0.60f});

    /* lista de 4 opciones */
    float opt_y = my + 52;
    float row_h = 36;

    for (int i = 0; i < 4; i++) {
        int is_sel = (ui.settings_sel == i);
        float ry = opt_y + i * row_h;

        if (is_sel) {
            float pulse = 0.7f + 0.3f * sinf(ui.time * 6.0f);
            GsColor sel_bg = {0.18f * pulse, 0.28f * pulse, 0.55f * pulse, 0.85f};
            gs_draw_rounded_rect(mx + 16, ry, mw - 32, row_h - 4, 6, sel_bg);
        }

        u32 col_title = is_sel ? 0xFFFFFFFFu : 0xFFA0B0C8u;
        u32 col_val = is_sel ? 0xFF60D0FFu : 0xFF7090B0u;

        char left_txt[64] = "";
        char right_txt[64] = "";

        if (i == 0) {
            snprintf(left_txt, sizeof(left_txt), "%s", txt_opt_lang[lang]);
            snprintf(right_txt, sizeof(right_txt), "< %s >", lang_names[lang]);
        } else if (i == 1) {
            snprintf(left_txt, sizeof(left_txt), "%s", txt_opt_sort[lang]);
            snprintf(right_txt, sizeof(right_txt), "< %s >", sort_names[lang][ui.sort_mode]);
        } else if (i == 2) {
            snprintf(left_txt, sizeof(left_txt), "%s", txt_opt_move[lang]);
            char gname[24];
            snprintf(gname, sizeof(gname), "%.14s", list.games[list.selected].title);
            snprintf(right_txt, sizeof(right_txt), "< %s (%d/%d) >", gname, list.selected + 1, list.count);
        } else if (i == 3) {
            snprintf(left_txt, sizeof(left_txt), "[X] %s", txt_opt_save[lang]);
        }

        draw_text((int)(mx + 30), (int)(ry + 8), left_txt, col_title);
        if (right_txt[0]) {
            int rw = (int)strlen(right_txt) * 8;
            draw_text((int)(mx + mw - 30 - rw), (int)(ry + 8), right_txt, col_val);
        }
    }

    /* hints del modal */
    gs_draw_rect(mx + 20, my + mh - 34, mw - 40, 1.0f, (GsColor){0.20f, 0.25f, 0.45f, 0.50f});
    draw_text_centered(SCREEN_W / 2, (int)(my + mh - 22), txt_modal_hints[lang], 0xFF8090B0u);
}

void ui_render(void)
{
    int lang = ui.language;
    if (lang < 0 || lang >= LANG_COUNT) lang = 0;
    GameEntry *sel = &list.games[list.selected];

    /* fondo global oscurecido para que el carrusel resalte */
    GsTexture *base = load_base_bg();
    if (base) {
        gs_clear((GsColor){0.02f, 0.02f, 0.06f, 1});
        gs_draw_sprite_ex(0, 0, SCREEN_W, SCREEN_H, base, 0, 0,
                          (float)base->w, (float)base->h, (GsColor){1, 1, 1, 1}, 0, 1.0f);
        GsColor dim = {0.0f, 0.0f, 0.04f, 0.45f};
        gs_draw_rect(0, 0, SCREEN_W, SCREEN_H, dim);
    } else {
        GsColor bg1 = {0.03f, 0.04f, 0.09f, 1};
        float p = 0.5f + 0.5f * sinf(ui.bg_anim);
        GsColor c2 = {0.08f + 0.04f * p, 0.10f + 0.05f * p, 0.22f + 0.08f * p, 1};
        gs_clear(bg1);
        gs_draw_rect_gradient(0, 0, SCREEN_W, SCREEN_H, bg1, c2, 1);
    }

    /* barra superior fina */
    GsColor topbar = {0.02f, 0.02f, 0.05f, 0.75f};
    gs_draw_rect(0, 0, SCREEN_W, 26, topbar);
    draw_text(8, 9, "PORTMENU", 0xFF60A0FFu);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d / %d", list.selected + 1, list.count);
    draw_text(SCREEN_W - 8 - (int)strlen(cnt) * 8, 9, cnt, 0xFFA0A0A0u);

    /* carrusel: de atras hacia adelante (laterales primero, central al final) */
    int n = list.count;
    for (int off = n; off >= 1; off--) {
        int li = list.selected - off;
        int ri = list.selected + off;
        if (li >= 0 && li < n) draw_carousel_item(&list.games[li], li);
        if (ri >= 0 && ri < n) draw_carousel_item(&list.games[ri], ri);
    }
    draw_carousel_item(&list.games[list.selected], list.selected);

    /* piso: gradiente bajo los covers */
    GsColor f1 = {0.05f, 0.06f, 0.12f, 0.0f};
    GsColor f2 = {0.02f, 0.02f, 0.06f, 0.85f};
    gs_draw_rect_gradient(0, CAR_FLOOR_Y + 2, SCREEN_W, SCREEN_H - CAR_FLOOR_Y - 2, f1, f2, 1);

    /* titulo grande del seleccionado */
    draw_text_big_centered(SCREEN_W / 2, CAR_FLOOR_Y + 12, sel->title, 0xFFFFFFFFu);

    /* meta / plataforma */
    {
        char meta[96];
        if (sel->genre[0] || sel->year[0]) {
            snprintf(meta, sizeof(meta), "%s%s%s",
                     sel->genre, (*sel->genre && *sel->year) ? " - " : "", sel->year);
        } else {
            snprintf(meta, sizeof(meta), "%s", txt_port_ps2[lang]);
        }
        draw_text_centered(SCREEN_W / 2, CAR_FLOOR_Y + 32, meta, 0xFF8C98B6u);
    }

    /* logo del juego si existe */
    GsTexture *logo = assets_get(sel->logo);
    if (logo) {
        float lw = 110;
        float lh = lw * logo->h / logo->w;
        if (lh > 24) { lh = 24; lw = lh * logo->w / logo->h; }
        gs_draw_sprite_ex(SCREEN_W - lw - 14, CAR_FLOOR_Y + 12, lw, lh, logo,
                          0, 0, (float)logo->w, (float)logo->h, (GsColor){1,1,1,1}, 0, 0.95f);
    }

    /* hints de botones mas grandes (escala 2x) traducidos */
    draw_text_big_centered(SCREEN_W / 2, 404, txt_hint_play[lang], 0xFFE0E8FFu);
    draw_text_big_centered(SCREEN_W / 2, 424, txt_hint_nav[lang], 0xFF8C98B2u);

    /* mensaje de error (ELF no encontrado) */
    if (ui.error_msg[0]) {
        float t = ui.error_time;
        if (t > 1.0f) t = 1.0f;
        GsColor panel = {0.55f, 0.10f, 0.10f, 0.92f * t};
        int msgw = (int)strlen(ui.error_msg) * 8 + 24;
        gs_draw_rect((SCREEN_W - msgw) / 2, 40, msgw, 22, panel);
        draw_text_centered(SCREEN_W / 2, 47, ui.error_msg, 0xFFFFFFFFu);
    }

    /* si esta abierto el modal de ajustes, dibujarlo encima */
    if (ui.show_settings) {
        draw_settings_modal();
    }
}

/* ---------- acceso ---------- */
/* precarga progresiva: 1 cover por llamada (durante el boot splash) */
int ui_precache_step(void)
{
    static int idx = 0;
    if (idx >= list.count) return 1;
    if (list.games[idx].cover[0])
        assets_get(list.games[idx].cover);
    idx++;
    return idx >= list.count;
}

int ui_get_diag_count(void) { return diag_n; }
const char *ui_get_diag_line(int i)
{
    return diag_lines[i % 18];
}

GameEntry *ui_get_selected(void) { return &list.games[list.selected]; }
int ui_get_selected_idx(void) { return list.selected; }
int ui_get_game_count(void) { return list.count; }

static int check_file_can_open(const char *path)
{
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static int resolve_elf_path(const char *input, char *resolved, size_t max_len)
{
    char base[280];
    strncpy(base, input, sizeof(base) - 4);
    base[sizeof(base) - 4] = '\0';

    if (!strchr(base, ':')) {
        char tmp[280];
        snprintf(tmp, sizeof(tmp), "%s%s", media_prefix, base);
        strncpy(base, tmp, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    char candidates[16][280];
    int ncand = 0;

    /* 1. base con \ y con / */
    strncpy(candidates[ncand], base, 279);
    candidates[ncand][279] = '\0';
    for (char *p = candidates[ncand]; *p; p++) if (*p == '/') *p = '\\';
    ncand++;

    strncpy(candidates[ncand], base, 279);
    candidates[ncand][279] = '\0';
    for (char *p = candidates[ncand]; *p; p++) if (*p == '\\') *p = '/';
    ncand++;

    /* 2. variantes en MAYUSCULAS */
    for (int i = 0; i < 2; i++) {
        strncpy(candidates[ncand], candidates[i], 279);
        candidates[ncand][279] = '\0';
        char *colon = strchr(candidates[ncand], ':');
        char *start = colon ? colon + 1 : candidates[ncand];
        for (char *p = start; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        }
        ncand++;
    }

    /* 3. variantes 8.3 (recortar stems > 8 caracteres) */
    for (int i = 0; i < 4; i++) {
        char src[280];
        strncpy(src, candidates[i], 279);
        src[279] = '\0';
        char *colon = strchr(src, ':');
        char prefix[32] = "";
        char rest[256] = "";
        if (colon) {
            size_t plen = (size_t)(colon - src + 1);
            if (plen >= sizeof(prefix)) plen = sizeof(prefix) - 1;
            strncpy(prefix, src, plen);
            prefix[plen] = '\0';
            strncpy(rest, colon + 1, sizeof(rest) - 1);
        } else {
            strncpy(rest, src, sizeof(rest) - 1);
        }

        char sep = strchr(rest, '/') ? '/' : '\\';
        char out_rest[256] = "";
        char copy_rest[256];
        strncpy(copy_rest, rest, sizeof(copy_rest) - 1);
        copy_rest[sizeof(copy_rest) - 1] = '\0';

        char *token = strtok(copy_rest, "/\\");
        while (token) {
            char part[32];
            strncpy(part, token, sizeof(part) - 1);
            part[sizeof(part) - 1] = '\0';
            char *dot = strrchr(part, '.');
            if (dot) {
                int stem_len = (int)(dot - part);
                if (stem_len > 8) {
                    char ext[16];
                    strncpy(ext, dot, sizeof(ext) - 1);
                    ext[sizeof(ext) - 1] = '\0';
                    part[8] = '\0';
                    strncat(part, ext, sizeof(part) - strlen(part) - 1);
                }
            } else {
                if (strlen(part) > 8) part[8] = '\0';
            }
            if (out_rest[0]) {
                size_t l = strlen(out_rest);
                if (l < sizeof(out_rest) - 2) {
                    out_rest[l] = sep;
                    out_rest[l + 1] = '\0';
                }
            }
            strncat(out_rest, part, sizeof(out_rest) - strlen(out_rest) - 1);
            token = strtok(NULL, "/\\");
        }
        if (prefix[0] && (sep == '\\' || sep == '/')) {
            snprintf(candidates[ncand], 279, "%s%c%s", prefix, sep, out_rest);
        } else {
            snprintf(candidates[ncand], 279, "%s%s", prefix, out_rest);
        }
        candidates[ncand][279] = '\0';
        ncand++;
    }

    /* Probar cada candidato (primero directo, luego con ;1) */
    for (int i = 0; i < ncand; i++) {
        if (check_file_can_open(candidates[i])) {
            strncpy(resolved, candidates[i], max_len - 1);
            resolved[max_len - 1] = '\0';
            return 1;
        }
        if (strncmp(candidates[i], "cdrom", 5) == 0 && !strchr(candidates[i], ';')) {
            char with_semi[280];
            snprintf(with_semi, sizeof(with_semi), "%s;1", candidates[i]);
            if (check_file_can_open(with_semi)) {
                strncpy(resolved, with_semi, max_len - 1);
                resolved[max_len - 1] = '\0';
                return 1;
            }
        }
    }

    strncpy(resolved, candidates[0], max_len - 1);
    resolved[max_len - 1] = '\0';
    return 0;
}

/* ====================================================================
 * CARGADOR BRAM (0x00084000) - Patron elf-loader de PS2SDK
 * Carga cualquier ELF en memoria de juego (0x00100000) desde BRAM
 * sin sobrescribir el ejecutable actual ni resetear el IOP.
 * reset_iop = 0 es INDISPENSABLE para OPL (mantiene el driver CDVDMAN)
 * y para CD fisico.
 * ==================================================================== */
#define ELF_MAGIC 0x464c457f
#define ELF_PT_LOAD 1

typedef struct {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf_header_t;

typedef struct {
    u32 type;
    u32 offset;
    void *vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} elf_pheader_t;

extern u8 loader_elf[];
extern int size_loader_elf;
extern int ExecPS2(void *entry, void *gp, int argc, char *argv[]);
extern void sceSifExitRpc(void);
extern void sceSifInitRpc(int mode);

static void wipe_bramMem(void)
{
#ifdef _EE
    for (int i = 0x00084000; i < 0x100000; i += 64) {
        __asm__ __volatile__(
            "\tsq $0, 0(%0) \n"
            "\tsq $0, 16(%0) \n"
            "\tsq $0, 32(%0) \n"
            "\tsq $0, 48(%0) \n" ::"r"(i));
    }
#else
    memset((void *)0x00084000, 0, 0x100000 - 0x00084000);
#endif
}

static int launch_elf_via_bram(const char *filename, const char *partition, int reset_iop, int argc, char *argv[])
{
    u8 *boot_elf = (u8 *)loader_elf;
    elf_header_t *eh = (elf_header_t *)boot_elf;
    elf_pheader_t *eph;
    void *pdata;
    int i;
    int new_argc = argc + 3;

    wipe_bramMem();

    char *new_argv[argc + 3];
    new_argv[0] = partition != NULL ? (char *)partition : "";
    new_argv[1] = (char *)filename;
    new_argv[2] = reset_iop ? "1" : "0";
    for (i = 0; i < argc; i++) {
        new_argv[i + 3] = argv[i];
    }

    if (*(u32 *)&eh->ident[0] != ELF_MAGIC) {
        return -1;
    }

    eph = (elf_pheader_t *)(boot_elf + eh->phoff);

    for (i = 0; i < eh->phnum; i++) {
        if (eph[i].type != ELF_PT_LOAD)
            continue;

        pdata = (void *)(boot_elf + eph[i].offset);
        memcpy(eph[i].vaddr, pdata, eph[i].filesz);

        if (eph[i].memsz > eph[i].filesz) {
            memset((void *)((u8 *)(eph[i].vaddr) + eph[i].filesz), 0, eph[i].memsz - eph[i].filesz);
        }
    }

    /* Salir de SIF RPC antes de saltar a BRAM (loader.c en BRAM reinicia su propio SIF RPC) */
    sceSifExitRpc();

    /* Limpiar caches de la CPU antes de saltar a BRAM */
    FlushCache(0);
    FlushCache(2);

    return ExecPS2((void *)eh->entry, NULL, new_argc, new_argv);
}

void ui_launch_selected(void)
{
    GameEntry *g = ui_get_selected();

    char elf_target[280];
    int exists = resolve_elf_path(g->elf_path, elf_target, sizeof(elf_target));

    if (!exists) {
        snprintf(ui.error_msg, sizeof(ui.error_msg),
                 "ELF no encontrado: %s", elf_target);
        ui.error_time = 3.0f;
        return;
    }

    ui_save_config();
    ui_shutdown();       /* libera texturas y assets */

    /* Cerrar pad y GS limpiamente antes de lanzar (pantalla negra activa sin apagar reloj) */
    extern void pad_shutdown(void);
    pad_shutdown();      /* padPortClose + padEnd */
    gs_shutdown();       /* pantalla negra activa + vsync */

    char kernel_path[280];
    char app_arg0[280];

    strncpy(kernel_path, elf_target, sizeof(kernel_path) - 1);
    kernel_path[sizeof(kernel_path) - 1] = '\0';

    strncpy(app_arg0, elf_target, sizeof(app_arg0) - 1);
    app_arg0[sizeof(app_arg0) - 1] = '\0';

    /* Quitar ;1 de argv[0] para el juego hijo (para que busque assets en ruta limpia) */
    char *semi = strchr(app_arg0, ';');
    if (semi) *semi = '\0';

    char *child_args[2];
    child_args[0] = app_arg0;
    child_args[1] = NULL;

    /* Intentar lanzar primero sin ;1 para no romper argv[0] de los ports */
    char clean_path[280];
    strncpy(clean_path, kernel_path, sizeof(clean_path) - 1);
    clean_path[sizeof(clean_path) - 1] = '\0';
    char *semi_k = strchr(clean_path, ';');
    if (semi_k) *semi_k = '\0';

    /* 1. Lanzar mediante cargador BRAM (sin reset de IOP para compatibilidad total con OPL y CD) */
    launch_elf_via_bram(clean_path, NULL, 0, 1, child_args);

    /* 2. Si el sistema de archivos del CD requiere descriptor estricto ;1, probar con ;1 */
    if (strncmp(kernel_path, "cdrom", 5) == 0 && !strchr(kernel_path, ';')) {
        strncat(kernel_path, ";1", sizeof(kernel_path) - strlen(kernel_path) - 1);
    }
    sceSifInitRpc(0);
    launch_elf_via_bram(kernel_path, NULL, 0, 1, child_args);

    /* 3. Fallback de emergencia con LoadExecPS2 (para CD fisico estandar si BRAM retorna) */
    FlushCache(0);
    FlushCache(2);
    LoadExecPS2(kernel_path, 1, child_args);
}

void ui_init(void)
{
    memset(&list, 0, sizeof(list));
    memset(&ui, 0, sizeof(ui));
    ui.language = LANG_ES; /* Espanol por defecto */
    for (int i = 0; i < MAX_GAMES; i++) {
        list.games[i].anim_scale = 0.6f;
        list.games[i].anim_alpha = 0.0f;
    }
}

void ui_shutdown(void)
{
    assets_shutdown();
}