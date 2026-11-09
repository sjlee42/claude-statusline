/*
 * Claude Code statusline — fast, dependency-free C.
 *
 *   Opus 5.5  │  ctx ━━━━━━━━━━━━ 42%  │  high  ⚡fast
 *   current   ━━━━━━━━━━━━   28%   ↻ 19:00
 *   weekly    ━━━━━━━━━━━━   79%   ↻ Thu 11/12 10:00
 *
 * Reads the statusLine JSON payload on stdin and prints up to three lines.
 * Usage: statusline [--light]   (--light = palette for light terminal backgrounds)
 * Build: cc -O2 -o statusline statusline.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- 256-color palettes (dark default, --light for light terminals) ---------- */
#define ESC(n) "\033[38;5;" #n "m"
#define RST    "\033[0m"

typedef struct {
    const char *model, *dim, *sep, *empty, *text, *eff, *fast, *ok, *warn, *crit;
} Palette;

static const Palette DARK = {
    ESC(117), ESC(243), ESC(240), ESC(238), ESC(252),
    ESC(215), ESC(221), ESC(114), ESC(221), ESC(203),
};
static const Palette LIGHT = {
    ESC(25),  ESC(244), ESC(249), ESC(252), ESC(236),
    ESC(166), ESC(130), ESC(28),  ESC(172), ESC(160),
};
static const Palette *P = &DARK;

#define BAR_CELLS 12

static void put_sep(void) { printf("%s  │  " RST, P->sep); }

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

/* dotted path lookup: "rate_limits.five_hour.used_percentage" */
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

static int get_str(const char *root, const char *path, char *out, size_t cap) {
    const char *p = get(root, path);
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'u') { /* rare in these fields: replace \uXXXX with '?' */
                int k = 0;
                while (k < 4 && p[1]) { p++; k++; }
                c = '?';
            } else if (c == 'n' || c == 't' || c == 'r') c = ' ';
            out[i++] = c;
            p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

static int get_true(const char *root, const char *path) {
    const char *p = get(root, path);
    return p && strncmp(p, "true", 4) == 0;
}

/* ---------- rendering ---------- */
static const char *pct_color(double p) {
    return p < 50 ? P->ok : (p < 80 ? P->warn : P->crit);
}

static double clamp(double p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

static void put_bar(double p, int known) {
    int filled = known ? (int)(clamp(p) / 100.0 * BAR_CELLS + 0.5) : 0;
    if (known && p > 0 && filled == 0) filled = 1; /* show any non-zero usage */
    if (filled) {
        fputs(pct_color(p), stdout);
        for (int i = 0; i < filled; i++) fputs("━", stdout);
    }
    fputs(P->empty, stdout);
    for (int i = filled; i < BAR_CELLS; i++) fputs("━", stdout);
    fputs(RST, stdout);
}

static void put_limit(const char *json, const char *label, const char *win, int long_fmt) {
    char path[64];
    double pct, reset;
    snprintf(path, sizeof path, "rate_limits.%s.used_percentage", win);
    if (!get_num(json, path, &pct)) return;
    snprintf(path, sizeof path, "rate_limits.%s.resets_at", win);
    int has_reset = get_num(json, path, &reset);

    printf("\n%s%-8s" RST " ", P->dim, label);
    put_bar(pct, 1);
    printf("%s  %3d%%" RST, pct_color(pct), (int)(clamp(pct) + 0.5));

    if (has_reset) {
        time_t t = (time_t)reset, now = time(NULL);
        struct tm rt, nt;
        localtime_r(&t, &rt);
        localtime_r(&now, &nt);
        char buf[32];
        const char *fmt = long_fmt ? "%a %m/%d %H:%M"
                        : (rt.tm_yday == nt.tm_yday && rt.tm_year == nt.tm_year) ? "%H:%M"
                        : "%a %H:%M";
        strftime(buf, sizeof buf, fmt, &rt);
        printf("%s   ↻ %s%s" RST, P->dim, P->text, buf);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--light") == 0) P = &LIGHT;

    /* read all of stdin */
    size_t cap = 16384, len = 0, n;
    char *json = malloc(cap);
    if (!json) return 1;
    while ((n = fread(json + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (cap - len < 4096) {
            char *nj = realloc(json, cap *= 2);
            if (!nj) break;
            json = nj;
        }
    }
    json[len] = 0;

    /* line 1: model │ ctx │ effort fast */
    char model[128] = "Claude", effort[32];
    get_str(json, "model.display_name", model, sizeof model);
    printf("%s%s" RST, P->model, model);
    put_sep();
    printf("%sctx " RST, P->dim);

    double ctx = 0;
    int has_ctx = get_num(json, "context_window.used_percentage", &ctx);
    put_bar(ctx, has_ctx);
    if (has_ctx) printf("%s %d%%" RST, pct_color(ctx), (int)(clamp(ctx) + 0.5));
    else         printf("%s --%%" RST, P->dim);

    int has_eff = get_str(json, "effort.level", effort, sizeof effort);
    int fast = get_true(json, "fast_mode");
    if (has_eff || fast) put_sep();
    if (has_eff) printf("%s%s" RST, P->eff, effort);
    if (fast) printf("%s%s⚡fast" RST, has_eff ? "  " : "", P->fast);

    /* lines 2–3: rate limits (only when present) */
    put_limit(json, "current", "five_hour", 0);
    put_limit(json, "weekly", "seven_day", 1);

    putchar('\n');
    free(json);
    return 0;
}
