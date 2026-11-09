/*
 * Claude Code statusline — fast, dependency-free C.
 *
 *   Opus 5.5  │  context ━━━━━───────
 *   high      │  5h      ━━━━━━━━────   2h05m
 *   ⚡fast     │  7d      ━━━━━━━━━━━─!  3d04h
 *
 * Reads the statusLine JSON payload on stdin and prints up to three lines in two
 * columns: model and effort on the left, three aligned gauges on the right.
 * The flags are chosen once at install time (setup.sh writes them into the
 * statusLine command in ~/.claude/settings.json).
 * Build: cc -O2 -o statusline statusline.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char USAGE[] =
    "usage: statusline [--dark | --light] [--cb] [--pct] < payload.json\n"
    "  --dark   palette for dark terminal backgrounds (default)\n"
    "  --light  palette for light terminal backgrounds\n"
    "  --cb     color-blind friendly palette (red-green safe)\n"
    "  --pct    also print the percentage numbers (default: gauges only)\n";

/* ---------- palettes (dark/light × standard/color-blind) ---------- */
#define ESC(n) "\033[38;5;" #n "m"
#define BOLD(n) "\033[1;38;5;" #n "m"
#define RST    "\033[0m"
#define RGB(r, g, b) "\033[38;2;" #r ";" #g ";" #b "m"      /* 24-bit truecolor */
#define BRGB(r, g, b) "\033[1;38;2;" #r ";" #g ";" #b "m"

/* effort level colors; max is a full pre-colored "max" (one hue per letter) */
typedef struct { const char *low, *medium, *high, *xhigh, *max; } EffortColors;

/* model name tints, indexed like FAMILY: current generation (bold) / legacy */
#define NFAMILY 4
static const char *const FAMILY[NFAMILY] = { "Fable", "Opus", "Sonnet", "Haiku" };
typedef struct { const char *cur[NFAMILY], *old[NFAMILY]; } ModelTints;

/* gauge bands, indexed by band(): < 20 / < 50 / < 80 / >= CRIT */
#define CRIT 80
enum { LOW, OK, WARN, HIGH, NBAND };

typedef struct {
    const char *model, *dim, *sep, *empty, *text, *eff, *fast; /* model/eff: fallbacks */
    const EffortColors *effc;
    const ModelTints *tint;
    const char *num[NBAND]; /* percentage numbers; also the bar in option 1 */
    const char *bar[NBAND]; /* bar fill in option 2 (never bold) */
} Palette;

/*
 * Effort: the dark set matches Claude Code's own /effort picker (static, no
 * shimmer); max is a pastel rainbow (red / yellow / blue). The light set is
 * the same hues darkened to >= 4.5:1 on white. The color-blind sets use a
 * reddish-purple ramp plus an Okabe-Ito max, so effort never borrows a gauge
 * hue (blue / bluish green / orange / vermillion).
 */
static const EffortColors EFF_DARK = {
    RGB(241, 158, 56), RGB(105, 154, 188), RGB(116, 144, 247), RGB(182, 124, 247),
    RGB(232, 119, 108) "m" RGB(216, 192, 96) "a" RGB(106, 169, 230) "x",
};
static const EffortColors EFF_LIGHT = {
    RGB(176, 98, 10), RGB(61, 107, 140), RGB(63, 91, 214), RGB(130, 64, 208),
    RGB(192, 68, 58) "m" RGB(138, 109, 12) "a" RGB(45, 111, 184) "x",
};
static const EffortColors EFF_DARK_CB = {
    RGB(180, 167, 214), RGB(196, 155, 208), RGB(208, 143, 196), RGB(204, 121, 167),
    RGB(204, 121, 167) "m" RGB(240, 228, 66) "a" RGB(86, 180, 233) "x",
};
static const EffortColors EFF_LIGHT_CB = {
    RGB(110, 95, 158), RGB(133, 83, 143), RGB(154, 74, 134), RGB(168, 64, 122),
    RGB(168, 64, 122) "m" RGB(122, 111, 0) "a" RGB(31, 111, 168) "x",
};

/* Model: near-neutral, low-chroma tints (mint / lilac / sand / ice). */
static const ModelTints TINT_DARK = {
    { BRGB(214, 228, 222), BRGB(205, 192, 232), BRGB(232, 214, 184), BRGB(190, 220, 232) },
    { RGB(138, 150, 145),  RGB(136, 128, 156),  RGB(156, 144, 124),  RGB(126, 146, 156) },
};
static const ModelTints TINT_LIGHT = {
    { BRGB(47, 90, 76),    BRGB(74, 58, 120),   BRGB(106, 76, 34),   BRGB(42, 84, 102) },
    { RGB(95, 111, 105),   RGB(107, 99, 128),   RGB(122, 106, 82),   RGB(92, 110, 120) },
};

/*
 * Gauges, standard palette:
 *   < 20% blue  ·  < 50% green  ·  < 80% amber  ·  >= 80% red + bold + "!"
 * Two tones per band, same hue. Bars are the saturated shade; numbers are a
 * muted, low-chroma shade that is easy on the eyes (dark: lighter, >= 6:1;
 * light: darker, >= 4.5:1 except the top band, which is bold).
 */
static const Palette DARK = {
    .model = BOLD(254), .dim = ESC(243), .sep = ESC(240), .empty = ESC(238), .text = ESC(252),
    .eff = ESC(215), .fast = BOLD(221), .effc = &EFF_DARK, .tint = &TINT_DARK,
    .num = { ESC(74), ESC(108), ESC(179), BOLD(174) },
    .bar = { ESC(32), ESC(35), ESC(172), ESC(160) },
};
static const Palette LIGHT = {
    .model = BOLD(236), .dim = ESC(244), .sep = ESC(249), .empty = ESC(252), .text = ESC(236),
    .eff = ESC(130), .fast = BOLD(130), .effc = &EFF_LIGHT, .tint = &TINT_LIGHT,
    .num = { ESC(24), ESC(29), ESC(94), BOLD(131) },
    .bar = { ESC(33), ESC(35), ESC(172), ESC(160) },
};

/*
 * Color-blind palette (--cb), Okabe-Ito based:
 *   < 20% blue  ·  < 50% bluish green  ·  < 80% orange  ·  >= 80% vermillion + bold + "!"
 * Checked with a Machado 2009 protan/deutan simulation: neighboring bands stay
 * apart, and the top two bands also differ in lightness. Severity never relies
 * on hue alone: >= 80% also gets bold and "!", and the filled bar (━) differs
 * in shape from its track (─).
 */
static const Palette DARK_CB = {
    .model = BOLD(255), .dim = ESC(245), .sep = ESC(240), .empty = ESC(240), .text = ESC(252),
    .eff = ESC(183), .fast = BOLD(229), .effc = &EFF_DARK_CB, .tint = &TINT_DARK,
    .num = { ESC(74), ESC(115), ESC(179), BOLD(166) },
    .bar = { ESC(32), ESC(36), ESC(214), ESC(202) },
};
static const Palette LIGHT_CB = {
    .model = BOLD(235), .dim = ESC(243), .sep = ESC(249), .empty = ESC(250), .text = ESC(236),
    .eff = ESC(97), .fast = BOLD(236), .effc = &EFF_LIGHT_CB, .tint = &TINT_LIGHT,
    .num = { ESC(24), ESC(29), ESC(130), BOLD(88) },
    .bar = { ESC(26), ESC(36), ESC(172), ESC(124) },
};
static const Palette *P = &DARK;
static int show_pct = 0; /* --pct: option 2, print numbers next to the bars */

#define BAR_CELLS 12
#define MAX_INPUT (1 << 20) /* the payload is a few KB; never buffer more than this */

static void put_sep(void) { fputs(P->sep, stdout); fputs("  │  " RST, stdout); }

/* ---------- minimal JSON path lookup ---------- */
static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p points at opening quote; returns pointer just past the closing quote */
static const char *skip_str(const char *p) {
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *skip_val(const char *p) {
    p = ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_str(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* p points at an object; returns pointer to the value of `key`, or NULL */
static const char *member(const char *p, const char *key, size_t klen) {
    p = ws(p);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *ks = p + 1, *ke = skip_str(p);
        if (ke <= ks) return NULL;
        size_t len = (size_t)(ke - 1 - ks);
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (len == klen && memcmp(ks, key, klen) == 0) return p;
        p = ws(skip_val(p));
        if (*p != ',') return NULL;
        p++;
    }
}

/* dotted path lookup from any object: "rate_limits.five_hour.used_percentage" */
static const char *get(const char *root, const char *path) {
    const char *p = root;
    while (p && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        p = member(p, path, n);
        path += n;
        if (*path == '.') path++;
    }
    return p ? ws(p) : NULL;
}

static int get_num(const char *root, const char *path, double *out) {
    const char *p = get(root, path);
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = strtod(p, NULL);
    return 1;
}

/*
 * Copies a string value into out (always NUL-terminated); returns 0 when it is
 * missing, not a string, or empty. Control characters are dropped (\n \t \r
 * become spaces) and \uXXXX becomes '?', so the payload can never inject
 * terminal escape sequences. A cut at cap never splits a UTF-8 sequence.
 */
static int get_str(const char *root, const char *path, char *out, size_t cap) {
    const char *p = get(root, path);
    size_t i = 0;
    if (p && *p == '"') {
        for (p++; *p && *p != '"' && i + 1 < cap; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\\' && p[1]) {
                c = (unsigned char)*++p;
                if (c == 'u') { for (int k = 0; k < 4 && p[1]; k++) p++; c = '?'; }
                else if (c == 'n' || c == 't' || c == 'r') c = ' ';
                else if (c != '"' && c != '\\' && c != '/') continue; /* \b \f */
            }
            if (c < 0x20 || c == 0x7f) continue;
            out[i++] = (char)c;
        }
        if (*p && *p != '"') { /* truncated: drop a possibly split last character */
            while (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0x80) i--;
            if (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0xC0) i--;
        }
    }
    out[i] = 0;
    return i > 0;
}

static int get_true(const char *root, const char *path) {
    const char *p = get(root, path);
    return p && strncmp(p, "true", 4) == 0;
}

/* ---------- rendering ---------- */
/* Percentages are rounded once, so the number, its color and "!" always agree. */
static int pct_round(double p) { return p <= 0 ? 0 : p >= 100 ? 100 : (int)(p + 0.5); }

static int band(int p) { return p < 20 ? LOW : p < 50 ? OK : p < CRIT ? WARN : HIGH; }

/* percentage with a non-color severity cue: "!" at >= CRIT, padded otherwise */
static void put_pct(int p, int width, int pad) {
    printf("%s%*d%%%s" RST, P->num[band(p)], width, p, p >= CRIT ? "!" : (pad ? " " : ""));
}

/*
 * Model name tinted per family (FAMILY / P->tint). The current generation
 * (major version >= CURRENT_GEN, or no version in the name) is bold; older
 * ones use the dimmer legacy shade. Unknown families use P->model.
 */
#define CURRENT_GEN 5
static void put_model(const char *m) {
    const char *c = P->model;
    const char *d = m;
    while (*d && (*d < '0' || *d > '9')) d++;
    int legacy = *d && strtol(d, NULL, 10) < CURRENT_GEN;
    for (int i = 0; i < NFAMILY; i++)
        if (strstr(m, FAMILY[i])) { c = legacy ? P->tint->old[i] : P->tint->cur[i]; break; }
    printf("%s%s" RST, c, m);
}

/* Effort level in P->effc colors; unknown levels fall back to P->eff. */
static void put_effort(const char *e) {
    const EffortColors *E = P->effc;
    if (!strcmp(e, "max")) { fputs(E->max, stdout); fputs(RST, stdout); return; }
    const char *c = !strcmp(e, "low")    ? E->low
                  : !strcmp(e, "medium") ? E->medium
                  : !strcmp(e, "high")   ? E->high
                  : !strcmp(e, "xhigh")  ? E->xhigh : P->eff;
    printf("%s%s" RST, c, e);
}

/*
 * Option 2 (--pct): bar in the deep bar[] shade, followed by the number.
 * Option 1 (default): no number, so the bar takes the number color instead,
 * with bold switched off (\033[22m) because the bar is never bold.
 * raw > 0 always fills at least one cell, so any usage shows.
 */
static void put_bar(int p, double raw, int known) {
    int filled = known ? (p * BAR_CELLS + 50) / 100 : 0;
    if (known && raw > 0 && filled == 0) filled = 1;
    if (filled) {
        if (show_pct) fputs(P->bar[band(p)], stdout);
        else        { fputs(P->num[band(p)], stdout); fputs("\033[22m", stdout); }
        for (int i = 0; i < filled; i++) fputs("━", stdout);
    }
    fputs(P->empty, stdout);
    for (int i = filled; i < BAR_CELLS; i++) fputs("─", stdout);
    fputs(RST, stdout);
}

/* the number (option 2) or option 1's non-color cue: bold "!" after the bar */
static void put_value(int p, int width, int pad) {
    if (show_pct)     put_pct(p, width, pad);
    else if (p >= CRIT) printf("%s!" RST, P->num[HIGH]);
    else if (pad)     putchar(' ');
}

/* gauge label, padded so all three bars start in the same column */
static void put_label(const char *label) { printf("%s%-7s" RST " ", P->dim, label); }

/* context row: "context ━━━━━───────" (unknown: "--") */
static void put_ctx(const char *json) {
    double raw = 0;
    int has = get_num(json, "context_window.used_percentage", &raw);
    int p = pct_round(raw);
    put_label("context");
    put_bar(p, raw, has);
    if (show_pct) fputs("  ", stdout);
    if (!has) printf("%s%s" RST, P->dim, show_pct ? " --%" : " --");
    else      put_value(p, 3, 0);
}

/*
 * Time left until resets_at: "42m", "2h05m", "3d04h". Minutes round up, so
 * it never reads 0m while the limit is still in force. Absurd timestamps
 * are skipped.
 */
static int fmt_left(const char *win, char *buf, size_t cap) {
    double reset;
    if (!get_num(win, "resets_at", &reset) || reset < 0 || reset > 1e11) return 0;
    double s = reset - (double)time(NULL);
    long m = s <= 0 ? 0 : (long)((s + 59) / 60);
    long d = m / 1440, h = m / 60 % 24;
    if (d)           snprintf(buf, cap, "%ldd%02ldh", d, h);
    else if (m >= 60) snprintf(buf, cap, "%ldh%02ldm", h, m % 60);
    else             snprintf(buf, cap, "%ldm", m);
    return 1;
}

/* one rate-limit row; win is the object at rate_limits.<window> */
static void put_limit(const char *win, const char *label) {
    double raw;
    get_num(win, "used_percentage", &raw); /* checked by the caller */
    int p = pct_round(raw);
    put_label(label);
    put_bar(p, raw, 1);
    if (show_pct) fputs("  ", stdout);
    put_value(p, 3, 1);
    char buf[32];
    if (fmt_left(win, buf, sizeof buf))
        printf("  %s%5s" RST, P->dim, buf);
}

/* display width of a UTF-8 string (one cell per code point) */
static int u8width(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

/* read stdin into a NUL-terminated heap buffer (NULL on failure) */
static char *read_all(FILE *f) {
    size_t cap = 16384, len = 0, n;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 4096) {
            char *nb = cap < MAX_INPUT ? realloc(buf, cap * 2) : NULL;
            if (!nb) break; /* parse what we have; missing fields degrade */
            buf = nb;
            cap *= 2;
        }
    }
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv) {
    int light = 0, cb = 0; /* chosen once at install time by setup.sh */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--light")) light = 1;
        else if (!strcmp(a, "--dark"))  light = 0;
        else if (!strcmp(a, "--cb"))    cb = 1;
        else if (!strcmp(a, "--pct"))   show_pct = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(USAGE, stdout); return 0; }
    }
    P = cb ? (light ? &LIGHT_CB : &DARK_CB) : (light ? &LIGHT : &DARK);

    char *json = read_all(stdin);
    if (!json) return 1;

    /*
     * Two columns:  model   │  context ━━━━───
     *               effort  │  5h      ━━────   2h05m
     *               ⚡fast   │  7d      ━━━━━─   3d04h
     * The left cells stack up, so a missing effort moves ⚡fast up a row.
     */
    char model[128], effort[32];
    if (!get_str(json, "model.display_name", model, sizeof model)) strcpy(model, "Claude");
    int has_eff = get_str(json, "effort.level", effort, sizeof effort);
    int fast = get_true(json, "fast_mode");

    /* rate-limit rows, only for windows that are present */
    static const char *const WIN[2][2] = { { "five_hour", "5h" }, { "seven_day", "7d" } };
    const char *lim[2], *lab[2];
    int nlim = 0;
    const char *rl = get(json, "rate_limits");
    for (int i = 0; rl && i < 2; i++) {
        const char *w = get(rl, WIN[i][0]);
        double tmp;
        if (w && get_num(w, "used_percentage", &tmp)) { lim[nlim] = w; lab[nlim++] = WIN[i][1]; }
    }

    /* left column: model, effort, ⚡fast (two cells for ⚡), stacked */
    enum { L_MODEL, L_EFF, L_FAST };
    int left[3], lwid[3], nleft = 0;
    left[nleft] = L_MODEL; lwid[nleft++] = u8width(model);
    if (has_eff) { left[nleft] = L_EFF;  lwid[nleft++] = (int)strlen(effort); }
    if (fast)    { left[nleft] = L_FAST; lwid[nleft++] = 6; }
    int lw = 0;
    for (int i = 0; i < nleft; i++) if (lwid[i] > lw) lw = lwid[i];
    int rows = 1 + nlim > nleft ? 1 + nlim : nleft;

    for (int r = 0; r < rows; r++) {
        int w = 0;
        if (r < nleft) {
            if      (left[r] == L_MODEL) put_model(model);
            else if (left[r] == L_EFF)   put_effort(effort);
            else                         printf("%s⚡fast" RST, P->fast);
            w = lwid[r];
        }
        if (r > nlim) { putchar('\n'); continue; } /* no gauge on this row */
        /* Claude Code trims leading whitespace (no-break space included),
         * so a row with an empty left column starts with U+2800 BRAILLE
         * PATTERN BLANK: one blank cell that trim() does not count as space */
        if (w == 0 && lw > 0) { fputs("\xe2\xa0\x80", stdout); w = 1; }
        printf("%*s", lw - w, "");
        put_sep();
        if (r == 0) put_ctx(json);
        else        put_limit(lim[r - 1], lab[r - 1]);
        putchar('\n');
    }
    free(json);
    return 0;
}
