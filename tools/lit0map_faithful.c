/*
 * Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
 * SPDX-License-Identifier: MIT
 *
 * Coder-faithful re-derivation of rc_models.h LIT0_MAP.
 *
 * Replays the ACTUAL shipped adaptive tag0 literal coder (5 histogram-seeded BitTrees adapting at
 * RC_LIT0_RATE, in wire order) over dumps produced by the A1_LITDUMP scaffold (src/enc_emit.c):
 * each dump segment carries the even-parity from-image seed histogram + the surviving span literals
 * { prevlit, byte, tag } in wire order. The cost of coding a tag0 record under candidate map M is the
 * adaptive bit-tree cost of `byte` through tree M[prevlit] (per-bit price taken from the probability
 * BEFORE its update), exactly mirroring the encoder's bt_encode/rc_adapt. tag1 records never touch the
 * map and are dropped (their only role -- advancing prevlit -- was already baked into the recorded
 * prevlit field by the scaffold, which updates prevlit on every content byte).
 *
 * Greedy coordinate descent over the 256 map rows, class 0..LIT0_CTX-1, using a two-tree decomposition
 * (moving row r only perturbs trees old=M[r] and new=c, so only those two are re-simulated). A move is
 * accepted only if it STRICTLY reduces home bits and does NOT increase foreign bits. Class 4 is kept
 * non-empty to preserve the header invariant LIT0_CTX == 1 + max(entry).
 *
 * Build:  cc -O2 -I../src -I.. -o lit0map_faithful lit0map_faithful.c   (needs CORTEX_M0 defined)
 * Usage:  lit0map_faithful <home_dump_dir> <foreign_dump_dir> [out_map.txt] [max_sweeps]
 */
#define CORTEX_M0 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include "rc_models.h"

/* -------- log2 LUT (host-only double) -------- */
static double LOG2[RC_PBIT + 1];
static void log2_init(void){ for (int i = 1; i <= (int)RC_PBIT; i++) LOG2[i] = log2((double)i); }

/* cost (bits) of coding one byte through an adaptive lit0 tree, price from the pre-update prob. */
static inline double replay_byte(A1BitTree *t, uint8_t byte){
    int m = 1; double c = 0.0;
    for (int i = 7; i >= 0; i--){
        int bit = (byte >> i) & 1;
        uint16_t p = a1_bt_get(t, m - 1);
        uint32_t pr = bit ? (RC_PBIT - p) : p;      /* pr in 1..4095 */
        c += 12.0 - LOG2[pr];                       /* -log2(pr/4096) */
        a1_bt_set(t, m - 1, rc_adapt(p, bit, RC_LIT0_RATE));
        m = (m << 1) | bit;
    }
    return c;
}

/* seed a lit0 tree from the even-parity from-image histogram, EXACTLY as lit_tree_seed_e. */
static void seed_from_hist(A1BitTree *t, const uint32_t *hist){
    uint32_t w[512];
    for (int s = 0; s < 256; s++) w[256 + s] = hist[s];
    for (int m = 255; m >= 1; m--) w[m] = w[2*m] + w[2*m+1];
    memset(t->p, 0, sizeof t->p);
    for (int m = 1; m < 256; m++) a1_bt_set(t, m - 1, rc_lit_seed_prob(w[2*m], w[m]));
}

typedef struct {
    A1BitTree seed;
    uint8_t *prev;   /* tag0 record prevlit, wire order */
    uint8_t *byte;   /* tag0 record byte */
    uint32_t n;      /* tag0 record count */
    int is_home;
} DFile;

static DFile *g_files = NULL; static size_t g_nfiles = 0, g_capfiles = 0;

static void add_file(DFile f){
    if (g_nfiles == g_capfiles){ g_capfiles = g_capfiles ? g_capfiles*2 : 512; g_files = realloc(g_files, g_capfiles*sizeof(DFile)); }
    g_files[g_nfiles++] = f;
}

/* parse all self-delimiting segments in one dump file; each -> one DFile (tag0 records only). */
static void load_dump_file(const char *path, int is_home){
    FILE *fp = fopen(path, "rb"); if (!fp) return;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz); if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz){ free(buf); fclose(fp); return; }
    fclose(fp);
    size_t off = 0;
    while (off + 12 + 256*4 <= (size_t)sz){
        uint32_t magic, ver, nrec, hist[256];
        memcpy(&magic, buf+off, 4); memcpy(&ver, buf+off+4, 4); memcpy(&nrec, buf+off+8, 4); off += 12;
        if (magic != 0x4C304631u || ver != 1u){ break; }
        memcpy(hist, buf+off, 256*4); off += 256*4;
        if (off + (size_t)nrec*3u > (size_t)sz) break;
        DFile f; memset(&f, 0, sizeof f);
        seed_from_hist(&f.seed, hist);
        f.is_home = is_home;
        f.prev = malloc(nrec ? nrec : 1); f.byte = malloc(nrec ? nrec : 1); f.n = 0;
        for (uint32_t i = 0; i < nrec; i++){
            uint8_t *rec = buf + off + (size_t)i*3u;
            if (rec[2] == 0){ f.prev[f.n] = rec[0]; f.byte[f.n] = rec[1]; f.n++; }   /* tag0 only */
        }
        off += (size_t)nrec*3u;
        add_file(f);
    }
    free(buf);
}

static void load_dir(const char *dir, int is_home){
    DIR *d = opendir(dir); if (!d){ fprintf(stderr, "cannot open dir %s\n", dir); exit(2); }
    struct dirent *e; char path[8192];
    while ((e = readdir(d))){
        size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 4, ".bin")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        load_dump_file(path, is_home);
    }
    closedir(d);
}

/* Full per-tree cost of `map` over all files, into ch[5] (home) and cf[5] (foreign). */
static void cost_all(const uint8_t *map, double *ch, double *cf){
    for (int t = 0; t < LIT0_CTX; t++){ ch[t] = 0; cf[t] = 0; }
    A1BitTree tr[LIT0_CTX];
    for (size_t fi = 0; fi < g_nfiles; fi++){
        DFile *f = &g_files[fi];
        double local[LIT0_CTX]; for (int t = 0; t < LIT0_CTX; t++){ tr[t] = f->seed; local[t] = 0; }
        for (uint32_t i = 0; i < f->n; i++){ int t = map[f->prev[i]]; local[t] += replay_byte(&tr[t], f->byte[i]); }
        double *dst = f->is_home ? ch : cf;
        for (int t = 0; t < LIT0_CTX; t++) dst[t] += local[t];
    }
}

/* Cost of a SINGLE tree `t` under membership predicate: row r included iff (map[r]==t) matches, with
 * row `rx` forced in (add) or out (remove). Returns home/foreign bits for that one tree. */
static void cost_one_tree(const uint8_t *map, int t, int rx, int add, double *ph, double *pf){
    double home = 0, fgn = 0;
    for (size_t fi = 0; fi < g_nfiles; fi++){
        DFile *f = &g_files[fi];
        A1BitTree tr = f->seed; double loc = 0;
        for (uint32_t i = 0; i < f->n; i++){
            int r = f->prev[i];
            int in = (r == rx) ? add : (map[r] == t);
            if (in) loc += replay_byte(&tr, f->byte[i]);
        }
        if (f->is_home) home += loc; else fgn += loc;
    }
    *ph = home; *pf = fgn;
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: %s home_dir foreign_dir [out_map.txt] [max_sweeps]\n", argv[0]); return 1; }
    const char *out_path = argc > 3 ? argv[3] : NULL;
    int max_sweeps = argc > 4 ? atoi(argv[4]) : 40;
    log2_init();
    load_dir(argv[1], 1);
    load_dir(argv[2], 0);
    uint64_t nh = 0, nf = 0; size_t hf = 0, ff = 0;
    for (size_t i = 0; i < g_nfiles; i++){ if (g_files[i].is_home){ nh += g_files[i].n; hf++; } else { nf += g_files[i].n; ff++; } }
    fprintf(stderr, "loaded %zu files: home %zu files / %llu tag0 recs, foreign %zu files / %llu recs\n",
            g_nfiles, hf, (unsigned long long)nh, ff, (unsigned long long)nf);

    uint8_t map[256]; memcpy(map, LIT0_MAP, 256);
    double ch[LIT0_CTX], cf[LIT0_CTX];
    cost_all(map, ch, cf);
    double sh0 = 0, sf0 = 0; for (int t = 0; t < LIT0_CTX; t++){ sh0 += ch[t]; sf0 += cf[t]; }
    fprintf(stderr, "shipped map : home %.1f bits (%.1f B)  foreign %.1f bits (%.1f B)\n",
            sh0, sh0/8.0, sf0, sf0/8.0);

    int cls_count[LIT0_CTX];
    int moved = 0;
    for (int sweep = 0; sweep < max_sweeps; sweep++){
        int changed = 0;
        for (int r = 0; r < 256; r++){
            int old = map[r];
            for (int t = 0; t < LIT0_CTX; t++) cls_count[t] = 0;
            for (int i = 0; i < 256; i++) cls_count[map[i]]++;
            /* cost of old-tree with row r removed (independent of candidate) */
            double omh, omf; cost_one_tree(map, old, r, 0, &omh, &omf);
            int best_c = old; double best_dh = -1e-6, best_df = 0; /* require strict home improvement */
            double best_new_c_h = 0, best_new_c_f = 0;
            for (int c = 0; c < LIT0_CTX; c++){
                if (c == old) continue;
                /* invariant: keep class 4 non-empty (LIT0_CTX == 1+max) */
                if (old == LIT0_CTX-1 && cls_count[LIT0_CTX-1] == 1) break;
                double cwh, cwf; cost_one_tree(map, c, r, 1, &cwh, &cwf);
                double dh = (omh + cwh) - (ch[old] + ch[c]);
                double df = (omf + cwf) - (cf[old] + cf[c]);
                /* conservative: strictly reduce home AND do not increase foreign */
                if (dh < best_dh && df <= 1e-6){ best_dh = dh; best_df = df; best_c = c; best_new_c_h = cwh; best_new_c_f = cwf; }
            }
            if (best_c != old){
                ch[old] = omh; cf[old] = omf;
                ch[best_c] = best_new_c_h; cf[best_c] = best_new_c_f;
                map[r] = (uint8_t)best_c;
                changed++; moved++;
                (void)best_df;
            }
        }
        double sh = 0, sf = 0; for (int t = 0; t < LIT0_CTX; t++){ sh += ch[t]; sf += cf[t]; }
        fprintf(stderr, "sweep %2d: %d moves | home %.1f B (%+.1f) foreign %.1f B (%+.1f) | total moved %d\n",
                sweep, changed, sh/8.0, (sh-sh0)/8.0, sf/8.0, (sf-sf0)/8.0, moved);
        if (out_path){
            FILE *o = fopen(out_path, "w");
            if (o){ for (int i = 0; i < 256; i++){ fprintf(o, "%d,", map[i]); if ((i&31)==31) fprintf(o, "\n"); } fclose(o); }
        }
        if (!changed) break;
    }

    /* final full recompute (guards against cached drift) */
    cost_all(map, ch, cf);
    double sh = 0, sf = 0; for (int t = 0; t < LIT0_CTX; t++){ sh += ch[t]; sf += cf[t]; }
    int rows_changed = 0; for (int i = 0; i < 256; i++) if (map[i] != LIT0_MAP[i]) rows_changed++;
    fprintf(stderr, "FINAL: rows changed %d | home %.1f B (%+.1f) foreign %.1f B (%+.1f)\n",
            rows_changed, sh/8.0, (sh-sh0)/8.0, sf/8.0, (sf-sf0)/8.0);
    printf("SHIPPED_HOME_BITS %.1f\nSHIPPED_FGN_BITS %.1f\nFINAL_HOME_BITS %.1f\nFINAL_FGN_BITS %.1f\nROWS_CHANGED %d\n",
           sh0, sf0, sh, sf, rows_changed);
    printf("MAP");
    for (int i = 0; i < 256; i++) printf(" %d", map[i]);
    printf("\n");
    return 0;
}
