/* mkiso.c - generador de ISO9660 minimo para PS2 (level 1, nombres 8.3)
 * Uso: mkiso.exe <dir_raiz> <salida.iso> <etiqueta>
 * Public domain. Estructura identica a las ISOs homebrew de la escena:
 * PVD (sector 16) + terminator + path tables + dirs + archivos.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define SECTOR 2048
#define MAX_NODES 4096
#define MAX_DEPTH 16

typedef struct Node {
    char name[32];
    char path[1024];
    int is_dir;
    long long size;
    unsigned extent;
    int parent;
    int self;
    int dirnum;         /* numero de directorio en la path table (1-based) */
} Node;

static Node flat[MAX_NODES];
static int nflat = 0;

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---------- nombres 8.3 ---------- */

static int valid83(const char *s)
{
    const char *dot = strrchr(s, '.');
    int stem = dot ? (int)(dot - s) : (int)strlen(s);
    int ext = dot ? (int)strlen(dot + 1) : 0;
    if (stem < 1 || stem > 8 || ext > 3) return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '.') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '_') continue;
        return 0;
    }
    return 1;
}

static void sanitize(const char *in, char *out, int is_dir)
{
    const char *dot = strrchr(in, '.');
    int stem = dot ? (int)(dot - in) : (int)strlen(in);
    if (stem > 8) stem = 8;
    if (stem < 1) stem = 1;
    int n = 0;
    for (int i = 0; i < stem; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        out[n++] = c;
    }
    if (!is_dir && dot && dot[1]) {
        out[n++] = '.';
        int ext = 0;
        for (const char *p = dot + 1; *p && ext < 3; p++, ext++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static void unique_name(int parent_idx, const char *in, int is_dir, char *out)
{
    if (valid83(in)) {
        strcpy(out, in);
        return;
    }
    char base[16];
    sanitize(in, base, is_dir);
    strcpy(out, base);
    for (int tries = 1; tries < 100; tries++) {
        int clash = 0;
        for (int i = 0; i < nflat; i++) {
            if (flat[i].parent == parent_idx && strcmp(flat[i].name, out) == 0) {
                clash = 1;
                break;
            }
        }
        if (!clash) return;
        char num[4];
        snprintf(num, sizeof(num), "%d", tries);
        char stem[16], ext[8] = "";
        char *dot = strchr(base, '.');
        int sl;
        if (dot && !is_dir) {
            sl = (int)(dot - base);
            if (sl > 8) sl = 8;
            memcpy(stem, base, sl);
            stem[sl] = '\0';
            strcpy(ext, dot);
        } else {
            strncpy(stem, base, 8);
            stem[8] = '\0';
        }
        sl = (int)strlen(stem);
        int nl = (int)strlen(num);
        if (sl + nl > 8) stem[8 - nl] = '\0';
        if (is_dir) snprintf(out, 32, "%s%s", stem, num);
        else snprintf(out, 32, "%s%s%s", stem, num, ext);
    }
}

/* ---------- arbol ---------- */

static void add_node(const char *path, const char *hostpath, int is_dir, long long size, int parent_idx)
{
    Node *n = &flat[nflat];
    memset(n, 0, sizeof(*n));
    n->self = nflat;
    n->parent = parent_idx;
    n->is_dir = is_dir;
    n->size = size;
    strncpy(n->path, hostpath, sizeof(n->path) - 1);
    if (parent_idx < 0)
        n->name[0] = '\0'; /* root: sin nombre (identificador 0x00 en ISO) */
    else
        unique_name(parent_idx, path, is_dir, n->name);
    nflat++;
}

static void build_dir(const char *dirpath, int parent_idx, int depth)
{
    DIR *d = opendir(dirpath);
    if (!d) {
        fprintf(stderr, "mkiso: no puedo abrir %s\n", dirpath);
        return;
    }
    struct dirent *e;
    char *names[512];
    int nn = 0;
    while ((e = readdir(d)) != NULL && nn < 512) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn - 1; i++)
        for (int j = i + 1; j < nn; j++)
            if (ci_cmp(names[i], names[j]) > 0) {
                char *t = names[i]; names[i] = names[j]; names[j] = t;
            }

    for (int i = 0; i < nn; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirpath, names[i]);
        struct stat st;
        if (stat(full, &st) != 0) { free(names[i]); continue; }
        if (depth < MAX_DEPTH && (st.st_mode & S_IFDIR)) {
            add_node(names[i], full, 1, 0, parent_idx);
            build_dir(full, nflat - 1, depth + 1);
        } else if (st.st_mode & S_IFREG) {
            add_node(names[i], full, 0, st.st_size, parent_idx);
        }
        free(names[i]);
    }
}

/* ---------- ISO9660 ---------- */

static unsigned g_extent = 18;
static unsigned pt_size = 0;
static unsigned pt_l_ext, pt_m_ext;
static unsigned char pvd_date[17] = "2026010100000000";

static void wr32le(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr32be(unsigned char *p, unsigned v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void wr16le(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; }
static void wr16be(unsigned char *p, unsigned v) { p[0]=v>>8; p[1]=v; }

/* tamano del record de un nodo (sin escribir) */
static int dir_record_size(const Node *n)
{
    int nl = n->is_dir ? (int)strlen(n->name) : (int)strlen(n->name) + 2;
    int len = 33 + nl;
    if (len & 1) len++;
    return len;
}

/* escribe un record; dotdot: 0=".", 1="..", 2=nodo normal */
static int dir_record(unsigned char *out, const Node *n, int dotdot)
{
    const char *name;
    char tmp[32];
    unsigned extent, size;
    int nl;
    if (dotdot == 0 || dotdot == 1) {
        name = dotdot == 0 ? "\0" : "\001";
        nl = 1;
        extent = n->extent;
        size = (unsigned)n->size;
    } else {
        if (n->is_dir) name = n->name;
        else { snprintf(tmp, sizeof(tmp), "%s;1", n->name); name = tmp; }
        nl = (int)strlen(name);
        extent = n->extent;
        size = (unsigned)n->size;
    }
    int len = 33 + nl;
    if (len & 1) len++;
    memset(out, 0, len);
    out[0] = (unsigned char)len;
    wr32le(out + 2, extent);
    wr32be(out + 6, extent);
    wr32le(out + 10, size);
    wr32be(out + 14, size);
    out[18] = (unsigned char)(2026 - 1900); /* fecha binaria: 2026-01-01 00:00:00 */
    out[19] = 1;
    out[20] = 1;
    out[21] = out[22] = out[23] = 0;
    out[24] = 0; /* GMT offset */
    out[25] = n->is_dir ? 2 : 0;
    out[26] = 0; /* file unit size */
    out[27] = 0; /* interleave gap */
    wr16le(out + 28, 1);
    wr16be(out + 30, 1);
    out[32] = (unsigned char)nl;
    memcpy(out + 33, name, nl);
    return len;
}

/* path table record: [0]=len_di [1]=ext_attr [2..5]=extent [6..7]=parent [8..]=name */
static int pt_record(unsigned char *out, const Node *n, int be)
{
    int nl;
    unsigned parent_num = (n->parent < 0) ? 1u : (unsigned)flat[n->parent].dirnum;
    if (n->parent < 0) {
        /* root: identificador de 1 byte con valor 0x00 (ISO9660 7.6) */
        nl = 1;
    } else {
        nl = (int)strlen(n->name);
    }
    int len = 8 + nl;
    if (len & 1) len++;
    memset(out, 0, len);
    out[0] = (unsigned char)nl;
    out[1] = 0;
    if (be) {
        wr32be(out + 2, n->extent);
        wr16be(out + 6, parent_num);
    } else {
        wr32le(out + 2, n->extent);
        wr16le(out + 6, parent_num);
    }
    memcpy(out + 8, n->name, n->parent < 0 ? 0 : nl);
    return len;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: mkiso <dir_raiz> <salida.iso> <etiqueta>\n");
        return 1;
    }
    const char *root = argv[1];
    const char *outpath = argv[2];
    const char *label = argv[3];

    add_node("", root, 1, 0, -1); /* root = flat[0] */
    build_dir(root, 0, 0);

    if (nflat >= MAX_NODES) {
        fprintf(stderr, "mkiso: demasiados archivos (%d max)\n", MAX_NODES);
        return 1;
    }

    /* validar nombres 8.3 */
    for (int i = 1; i < nflat; i++) {
        if (!flat[i].is_dir && strlen(flat[i].name) + 3 > 31) {
            fprintf(stderr, "mkiso: nombre invalido %s\n", flat[i].name);
            return 1;
        }
    }

    /* tamanos de directorios */
    for (int i = 0; i < nflat; i++) {
        if (!flat[i].is_dir) continue;
        long long sz = dir_record_size(&flat[i]) + dir_record_size(&flat[i]); /* . y .. */
        for (int j = 0; j < nflat; j++)
            if (flat[j].parent == i && j != i)
                sz += dir_record_size(&flat[j]);
        if (sz < SECTOR) sz = SECTOR;
        flat[i].size = ((sz + SECTOR - 1) / SECTOR) * SECTOR;
    }

    /* extents: path tables primero (layout convencional), luego dirs, luego archivos */
    /* numeros de directorio: orden secuencial de dirs (los parents van antes en flat) */
    int dircounter = 0;
    for (int i = 0; i < nflat; i++)
        if (flat[i].is_dir)
            flat[i].dirnum = ++dircounter;

    pt_size = 0;
    for (int i = 0; i < nflat; i++)
        if (flat[i].is_dir) {
            /* mismo calculo que pt_record: root = 1 byte de nombre */
            int nl = (flat[i].parent < 0) ? 1 : (int)strlen(flat[i].name);
            pt_size += 8 + nl + ((8 + nl) & 1);
        }
    pt_l_ext = g_extent;
    g_extent += (pt_size + SECTOR - 1) / SECTOR;
    if (g_extent & 1) g_extent++; /* M table en extent impar (convencion ISO9660) */
    pt_m_ext = g_extent;
    g_extent += (pt_size + SECTOR - 1) / SECTOR;

    for (int i = 0; i < nflat; i++)
        if (flat[i].is_dir) {
            flat[i].extent = g_extent;
            g_extent += (unsigned)(flat[i].size / SECTOR);
        }

    for (int i = 0; i < nflat; i++) {
        if (!flat[i].is_dir) {
            int dup = -1;
            for (int k = 0; k < i; k++) {
                if (!flat[k].is_dir && flat[k].size == flat[i].size && flat[k].size > 0) {
                    if (strcmp(flat[k].path, flat[i].path) == 0 ||
                        strcmp(flat[k].name, flat[i].name) == 0) {
                        dup = k;
                        break;
                    }
                }
            }
            if (dup >= 0) {
                flat[i].extent = flat[dup].extent;
            } else {
                flat[i].extent = g_extent;
                g_extent += (unsigned)((flat[i].size + SECTOR - 1) / SECTOR);
            }
        }
    }

    unsigned total = g_extent;

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        fprintf(stderr, "mkiso: no puedo crear %s\n", outpath);
        return 1;
    }

    unsigned char *sec = calloc(1, SECTOR);
    for (unsigned s = 0; s < 16; s++) fwrite(sec, 1, SECTOR, f);

    /* PVD */
    memset(sec, 0, SECTOR);
    sec[0] = 1;
    memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;
    memset(sec + 8, ' ', 32);
    memcpy(sec + 8, "PLAYSTATION", 11);
    memset(sec + 40, ' ', 32);
    int ll = (int)strlen(label);
    if (ll > 32) ll = 32;
    memcpy(sec + 40, label, ll);
    wr32le(sec + 80, total);
    wr32be(sec + 84, total);
    wr16le(sec + 120, 1); wr16be(sec + 122, 1);
    wr16le(sec + 124, 1); wr16be(sec + 126, 1);
    wr16le(sec + 128, SECTOR); wr16be(sec + 130, SECTOR);
    wr32le(sec + 132, pt_size); wr32be(sec + 136, pt_size);
    wr32le(sec + 140, pt_l_ext);
    wr32be(sec + 148, pt_m_ext);
    dir_record(sec + 156, &flat[0], 0); /* root record (estilo ".") */
    memset(sec + 190, ' ', 128);
    memset(sec + 318, ' ', 128);
    memcpy(sec + 318, label, ll);
    memset(sec + 446, ' ', 128);
    memcpy(sec + 446, "PORTMENU", 8);
    memset(sec + 574, ' ', 128);
    memcpy(sec + 574, "PORTMENU", 8);
    memset(sec + 702, ' ', 37);
    memset(sec + 739, ' ', 37);
    memset(sec + 776, ' ', 37);
    memcpy(sec + 813, pvd_date, 16);
    memcpy(sec + 830, pvd_date, 16);
    memcpy(sec + 847, pvd_date, 16);
    memcpy(sec + 864, pvd_date, 16);
    sec[881] = 1;
    fwrite(sec, 1, SECTOR, f);

    /* VDT */
    memset(sec, 0, SECTOR);
    sec[0] = 255;
    memcpy(sec + 1, "CD001", 5);
    sec[6] = 1;
    fwrite(sec, 1, SECTOR, f);

    /* path tables */
    unsigned char *pt = malloc(pt_size + 4);
    int off = 0;
    for (int i = 0; i < nflat; i++)
        if (flat[i].is_dir)
            off += pt_record(pt + off, &flat[i], 0);
    if (off & 1) pt[off++] = 0;
    fseek(f, pt_l_ext * SECTOR, SEEK_SET);
    fwrite(pt, 1, pt_size, f);
    off = 0;
    for (int i = 0; i < nflat; i++)
        if (flat[i].is_dir)
            off += pt_record(pt + off, &flat[i], 1);
    if (off & 1) pt[off++] = 0;
    fseek(f, pt_m_ext * SECTOR, SEEK_SET);
    fwrite(pt, 1, pt_size, f);
    free(pt);

    /* directorios */
    unsigned char *db = malloc(1024 * 1024);
    for (int i = 0; i < nflat; i++) {
        if (!flat[i].is_dir) continue;
        int off2 = 0;
        off2 += dir_record(db + off2, &flat[i], 0);
        off2 += dir_record(db + off2, (flat[i].parent >= 0) ? &flat[flat[i].parent] : &flat[0], 1);
        for (int j = 0; j < nflat; j++)
            if (flat[j].parent == i && j != i)
                off2 += dir_record(db + off2, &flat[j], 2);
        fseek(f, flat[i].extent * SECTOR, SEEK_SET);
        fwrite(db, 1, (size_t)flat[i].size, f);
    }
    free(db);

    /* archivos */
    for (int i = 0; i < nflat; i++) {
        if (flat[i].is_dir) continue;
        int already_written = 0;
        for (int k = 0; k < i; k++) {
            if (!flat[k].is_dir && flat[k].extent == flat[i].extent) {
                already_written = 1;
                break;
            }
        }
        if (already_written) continue;

        FILE *src = fopen(flat[i].path, "rb");
        if (!src) {
            fprintf(stderr, "mkiso: no puedo abrir %s\n", flat[i].path);
            fclose(f);
            return 1;
        }
        fseek(f, flat[i].extent * SECTOR, SEEK_SET);
        char buf[65536];
        long long remain = flat[i].size;
        while (remain > 0) {
            size_t chunk = remain > 65536 ? 65536 : (size_t)remain;
            if (fread(buf, 1, chunk, src) != chunk) {
                fprintf(stderr, "mkiso: error leyendo %s\n", flat[i].path);
                fclose(src); fclose(f);
                return 1;
            }
            fwrite(buf, 1, chunk, f);
            remain -= chunk;
        }
        fclose(src);
        long long pad = ((flat[i].size + SECTOR - 1) / SECTOR) * SECTOR - flat[i].size;
        if (pad > 0) fwrite(sec, 1, (size_t)pad, f);
    }

    fclose(f);
    free(sec);
    printf("mkiso: %s OK (%u sectores, %d nodos, %.1f MB)\n",
           outpath, total, nflat, (double)total * SECTOR / 1048576.0);
    return 0;
}