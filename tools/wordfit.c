/* SPDX-License-Identifier: GPL-3.0-only */
/* wordfit: speak words through SAM the way dinghy.c's render_bank() does, in all eight voices, and say what each
 * costs in the word pool (8 bit, 11025 Hz) and whether a bank of 27 fits. Run it before you change SAY[].
 *
 *   cc -O2 -w -Ifirmware/src/sam -Ifirmware/src/sam/shim -o build/wordfit tools/wordfit.c \
 *       firmware/src/sam/sam.c firmware/src/sam/render.c firmware/src/sam/reciter.c
 *   build/wordfit LIST.txt [WAVDIR]
 *
 * LIST.txt: one word a line, spelled as in SAY[] ('/' = SAM phonemes); '#' lines are skipped. Every word gets its
 * pool cost in the slowest voice, the phonemes SAM was given, and a warning when it is TOO LONG (over 2.9 s: the
 * deck's Q16 position overflows) or SILENT (a phoneme SAM did not take). With exactly 27 words, in SAY[] order, it
 * also fills the bank like render_bank() for every voice: a voice that DOES NOT FIT loses its last words.
 * WAVDIR: a WAV of every word in the MC and ROBOT voices, to listen to. WPOOL=... in the environment models another
 * pool size. VOICES[], WPOOL and TONE_BYTES must match dinghy.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "SamData.h"
#include "sam.h"
#include "reciter.h"

#define WFS 11025u
static uint32_t WPOOL = 344064u;                /* dinghy.c */
#define TONE_BYTES (2u * (3795u + 1u))          /* dinghy.c: TONE and BUZZ at the start of the pool */
#define NKEYW 27
#define MAXW 128

SamData *samdata;
static SamData sam_mem;
static int8_t pool[1u << 20];
static char last_ph[256];                       /* the phonemes SAM was given */
static uint32_t pw, half, acc;
static void out(void *u, unsigned char b)       /* as dinghy.c's sam_out: 22050 Hz, two samples make one */
{
    (void)u;
    if (!half) { acc = b; half = 1; return; }
    half = 0;
    if (pw < WPOOL) pool[pw++] = (int8_t)(((int32_t)(acc + b) - 256) >> 1);
}

static const struct { const char *name; uint8_t speed, pitch, throat, mouth; } V[8] = {   /* dinghy.c VOICES[] */
    {"MC", 62, 58, 150, 175}, {"GROWL", 96, 150, 80, 80}, {"ROBOT", 92, 60, 190, 190}, {"SAM", 72, 64, 128, 128},
    {"ELF", 72, 64, 110, 160}, {"E.T.", 100, 64, 150, 200}, {"STUFFY", 82, 72, 110, 105}, {"OLD LADY", 82, 32, 145, 145}};

/* one word at pw: 0 if the reciter rejects it; *used = pool bytes taken, *len and *off = the word without its silence */
static int say(const char *s, int v, uint32_t *used, uint32_t *len, uint32_t *off)
{
    char in[256];
    uint32_t n = 0, a, b, start = pw;
    int i;
    while (s[n] && n < 200u) { in[n] = s[n]; n++; }
    in[n++] = '[';
    in[n] = 0;
    memset(&sam_mem, 0, sizeof sam_mem);
    samdata = &sam_mem;
    EnableSingmode(0);
    SetSpeed(V[v].speed); SetPitch(V[v].pitch); SetThroat(V[v].throat); SetMouth(V[v].mouth);
    half = 0;
    if (s[0] == '/') {
        for (n = 0; s[n + 1u] && n < 200u; n++) in[n] = s[n + 1u];
        in[n++] = (char)155;
        in[n] = 0;
    } else if (!TextToPhonemes(in)) { *used = *len = 0; return 0; }
    for (i = 0; in[i] && (unsigned char)in[i] != 155 && i < 255; i++) last_ph[i] = in[i];
    last_ph[i] = 0;
    SetInput(in);
    SAMMain(out, 0);
    a = start; b = pw;
    while (a < b && pool[a] > -3 && pool[a] < 3) a++;
    while (b > a && pool[b - 1] > -3 && pool[b - 1] < 3) b--;
    *used = pw - start;
    *len = b > a + 8u ? b - a : 0;
    *off = a;
    return 1;
}

static void wav(const char *path, const int8_t *d, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, v;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); v = 36 + n; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    { uint16_t h[2] = {1, 1}; fwrite(h, 2, 2, f); }
    v = WFS; fwrite(&v, 4, 1, f); fwrite(&v, 4, 1, f);
    { uint16_t h[2] = {1, 8}; fwrite(h, 2, 2, f); }
    fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f);
    for (i = 0; i < n; i++) fputc((uint8_t)(d[i] + 128), f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static char words[MAXW][256];
    int nw = 0, w, v;
    FILE *f;
    if (getenv("WPOOL")) WPOOL = (uint32_t)atoi(getenv("WPOOL"));
    if (WPOOL > sizeof pool) WPOOL = sizeof pool;
    if (argc < 2 || !(f = fopen(argv[1], "r"))) { fprintf(stderr, "usage: wordfit LIST.txt [WAVDIR]\n"); return 2; }
    while (nw < MAXW && fgets(words[nw], sizeof words[nw], f)) {
        words[nw][strcspn(words[nw], "\r\n")] = 0;
        if (words[nw][0] && words[nw][0] != '#') nw++;
    }
    fclose(f);

    /* every word alone: the pool it takes in the slowest voice */
    printf("%-34s %8s %6s  %s\n", "word", "max B", "sec", "phonemes");
    for (w = 0; w < nw; w++) {
        uint32_t worst = 0, used, len, off;
        int wv = 0;
        for (v = 0; v < 8; v++) {
            pw = 0;
            if (!say(words[w], v, &used, &len, &off)) { wv = -1; break; }
            if (used > worst) { worst = used; wv = v; }
            if (argc > 2 && (v == 0 || v == 2)) {
                char p[512], nm[256], *q;
                snprintf(nm, sizeof nm, "%s%s", words[w][0] == '/' ? "phonemes_" : "", words[w] + (words[w][0] == '/'));
                for (q = nm; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))) *q = '_';
                snprintf(p, sizeof p, "%s/%02d-%s-%s.wav", argv[2], w, v ? "robot" : "mc", nm);
                wav(p, pool + off, len);
            }
        }
        if (wv < 0) printf("%-34s   REJECTED by the reciter\n", words[w]);
        else printf("%-34s %8u %6.2f  %s%s\n", words[w], worst, worst / (double)WFS, last_ph,
                    worst > 32000u ? "   TOO LONG: over 2.9 s, the deck position (Q16) overflows"
                    : worst < 600u ? "   SILENT: SAM made no sound (a bad phoneme?)" : "");
    }

    /* the bank as render_bank() fills it: a word starts only with half a second of pool left */
    if (nw == NKEYW) {
        printf("\nbank of %d words, budget %u B (%.1f s) after the tones\n", NKEYW, WPOOL - TONE_BYTES, (WPOOL - TONE_BYTES) / (double)WFS);
        for (v = 0; v < 8; v++) {
            uint32_t used, len, off, silent = 0, cut = 0;
            pw = TONE_BYTES;
            for (w = 0; w < NKEYW; w++) {
                if (WPOOL - pw < WFS / 2u) { silent++; continue; }
                say(words[w], v, &used, &len, &off);
                if (pw >= WPOOL) cut++;
            }
            printf("  %-9s used %6u B  free %6u B (%5.2f s)  %s\n", V[v].name, pw - TONE_BYTES, WPOOL - pw,
                   (WPOOL - pw) / (double)WFS, silent || cut ? "DOES NOT FIT" : "fits");
            if (silent || cut) printf("            %u word(s) silent, %u cut short\n", silent, cut);
        }
    }
    return 0;
}
