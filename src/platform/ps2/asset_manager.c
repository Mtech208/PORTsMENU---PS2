// asset_manager.c - cache LRU de texturas con downscale y eviction
// Formatos de imagen: JPG, PNG, BMP, TGA, GIF (via stb_image, public domain)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asset_manager.h"
#include "gs_renderer.h"
#include "stb_image.h"

#define MAX_CACHE 12

static const char *search_prefixes[] = {
    "",        /* device por defecto: medio de arranque (cdrom0: en ISO) */
    "cdrom0:",
    "cdrom:",
    "host:",
    "mass:",
    "mc0:",
    "hdd0:",
    "cdfs:",
    "romfs:",
};
#define NUM_PREFIXES ((int)(sizeof(search_prefixes) / sizeof(search_prefixes[0])))

static u32 *img_load_raw(const char *path, int *outW, int *outH)
{
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    if (!data)
        return NULL;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        stbi_image_free(data);
        return NULL;
    }

    /* downscale a la mitad hasta encuadrar (el fondo global 640x448 pasa entero) */
    while (w > 640 || h > 448) {
        int nw = w / 2, nh = h / 2;
        u32 *small = malloc((size_t)nw * nh * 4);
        if (!small) break;
        u32 *src = (u32 *)data;
        for (int y2 = 0; y2 < nh; y2++)
            for (int x2 = 0; x2 < nw; x2++)
                small[y2 * nw + x2] = src[(y2 * 2) * w + (x2 * 2)];
        stbi_image_free(data);
        data = (unsigned char *)small;
        w = nw;
        h = nh;
    }

    *outW = w;
    *outH = h;
    return (u32 *)data;
}

/* ---------- cache LRU ---------- */

typedef struct CacheEntry {
    char path[256];
    GsTexture *tex;
    int failed;   /* no existe: no reintentar cada frame */
    int pinned;   /* uso permanente (fondo): nunca se evicta */
    struct CacheEntry *prev, *next;
} CacheEntry;


static CacheEntry *cache_front = NULL, *cache_back = NULL;
static int cache_count = 0;

static void cache_unlink(CacheEntry *e)
{
    if (e->prev) e->prev->next = e->next; else cache_front = e->next;
    if (e->next) e->next->prev = e->prev; else cache_back = e->prev;
    e->prev = e->next = NULL;
    cache_count--;
}

static void cache_move_front(CacheEntry *e)
{
    if (cache_front == e) return;
    cache_unlink(e);
    e->next = cache_front;
    e->prev = NULL;
    if (cache_front) cache_front->prev = e;
    cache_front = e;
    if (!cache_back) cache_back = e;
    cache_count++;
}

static void cache_evict_oldest(void)
{
    CacheEntry *e = cache_back;
    while (e && (e->pinned || !e->tex)) e = e->prev;
    if (!e) e = cache_back;
    if (!e) return;
    cache_unlink(e);
    if (e->tex) gs_texture_free(e->tex);
    free(e);
}

static GsTexture *load_first_prefix(const char *rel)
{
    char path[512];
    for (int i = 0; i < NUM_PREFIXES; i++) {
        snprintf(path, sizeof(path), "%s%s", search_prefixes[i], rel);
        int w, h;
        u32 *rgba = img_load_raw(path, &w, &h);
        if (rgba) {
            GsTexture *t = gs_texture_create(w, h, rgba);
            stbi_image_free(rgba);
            if (t) return t;
            gs_log("asset: %s VRAM llena", rel);
            return NULL;
        }
    }
    return NULL;
}

GsTexture *assets_get(const char *path)
{
    if (!path || !*path) return NULL;

    for (CacheEntry *e = cache_front; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            if (e->failed)
                return NULL; /* ya sabemos que no existe: sin reintentos */
            if (!e->tex)
                e->tex = load_first_prefix(path);
            cache_move_front(e);
            return e->tex;
        }
    }

    GsTexture *tex = load_first_prefix(path);
    if (!tex) {
        if (cache_count > 0) {
            cache_evict_oldest();
            tex = load_first_prefix(path);
        }
    }

    CacheEntry *e = calloc(1, sizeof(CacheEntry));
    if (e) {
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->tex = tex;
        e->failed = (tex == NULL); /* marca para no reintentar cada frame */
        e->next = cache_front;
        if (cache_front) cache_front->prev = e;
        cache_front = e;
        if (!cache_back) cache_back = e;
        cache_count++;
    }

    while (cache_count > MAX_CACHE)
        cache_evict_oldest();

    return tex;
}

/* uso permanente: carga (o recupera del cache) y evita que se evicte */
GsTexture *assets_pin(const char *path)
{
    GsTexture *t = assets_get(path);
    if (t) {
        for (CacheEntry *e = cache_front; e; e = e->next) {
            if (strcmp(e->path, path) == 0) {
                e->pinned = 1;
                break;
            }
        }
    }
    return t;
}

void assets_init(void)
{
    gs_log("assets: init (stb_image: jpg/png/bmp/tga/gif)");
}

void assets_shutdown(void)
{
    CacheEntry *e = cache_front;
    while (e) {
        CacheEntry *n = e->next;
        if (e->tex) gs_texture_free(e->tex);
        free(e);
        e = n;
    }
    cache_front = cache_back = NULL;
    cache_count = 0;
    gs_log("assets: shutdown");
}