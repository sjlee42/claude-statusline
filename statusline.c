/*
 * Claude Code statusline: fast, dependency-free C.
 *
 *   Opus 5.5 | high | main* | 42% | 5h 23% (1h12m) | 7d 41% (3d4h)
 *   ▸ 14:20 Add dark mode to the settings page
 *   └ ~/projects/my-app
 *
 * The last prompt is found by reading the transcript backward from its end,
 * stopping at the first match, so the cost does not grow with session length.
 * git runs at most once, and only inside a repository, alongside that scan.
 * Everything that gets drawn is untrusted and goes through text_add().
 */
#if defined(__linux__)
#define _GNU_SOURCE /* timegm, posix_spawn */
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define FG(n) "\033[38;5;" #n "m"
#define RST "\033[0m"
#define DIM FG(245)
#define SEP FG(240)
#define YELLOW FG(179)
#define PURPLE FG(147)
#define BLUE FG(110)
#define GREEN FG(114)
#define RED FG(203)

#define MAX_PAYLOAD (1 << 20)      /* the payload is a few KB */
#define CHUNK (64 * 1024)          /* transcript read size */
#define MAX_LINE (8 * 1024 * 1024) /* longer transcript lines are skipped */
#define MAX_SCAN (64 * 1024 * 1024) /* stop looking for a prompt after this */
#define PROMPT_CHARS 60
#define GIT_TIMEOUT 5 /* seconds */

/* ---------- JSON: path lookup on NUL-terminated text, no allocation ---------- */
static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p is at an opening quote; returns just past the closing one */
static const char *skip_str(const char *p) {
    for (const char *q = p + 1;;) {
        const char *e = strchr(q, '"');
        if (!e) return q + strlen(q);
        const char *b = e;
        while (b > p + 1 && b[-1] == '\\') b--;
        if (!((e - b) & 1)) return e + 1;
        q = e + 1;
    }
}

/* p is at a bracket; returns just past the matching one, NULL if it never closes */
static const char *closing(const char *p) {
    int depth = 0;
    for (;;) {
        p += strcspn(p, "\"{}[]");
        if (!*p) return NULL;
        if (*p == '"') { p = skip_str(p); continue; }
        depth += (*p == '{' || *p == '[') ? 1 : -1;
        p++;
        if (!depth) return p;
    }
}

static const char *skip_val(const char *p) {
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        const char *e = closing(p);
        return e ? e : p + strlen(p);
    }
    return p + strcspn(p, ",}] \t\r\n");
}

/* value of key in the object at p; NULL when absent or null */
static const char *member(const char *p, const char *key) {
    size_t klen = strlen(key);
    if (!p || *p != '{') return NULL;
    p = ws(p + 1);
    while (*p == '"') {
        const char *k = p + 1, *e = skip_str(p);
        p = ws(e);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if ((size_t)(e - 1 - k) == klen && !memcmp(k, key, klen))
            return strncmp(p, "null", 4) ? p : NULL;
        p = ws(skip_val(p));
        if (*p != ',') return NULL;
        p = ws(p + 1);
    }
    return NULL;
}

static int num(const char *p, double *out) {
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = strtod(p, NULL);
    return *out > -1e12 && *out < 1e12;
}

static int is_true(const char *p) { return p && !strncmp(p, "true", 4); }

static int is_str(const char *p, const char *s) {
    size_t n = strlen(s);
    return p && *p == '"' && !strncmp(p + 1, s, n) && p[n + 1] == '"';
}

static int hex4(const char *p, uint32_t *out) {
    *out = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int v = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        *out = *out << 4 | (uint32_t)v;
    }
    return 1;
}

/* next decoded code point of a JSON string body; 0 at its end */
static uint32_t next_cp(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (!c || c == '"') return 0;
    p++;
    if (c == '\\') {
        if (!*p) return 0;
        c = *p++;
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
            uint32_t lo;
            if (!hex4((const char *)p, &c)) return 0;
            p += 4;
            if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u' &&
                hex4((const char *)p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            if (!c || (c >= 0xD800 && c < 0xE000)) c = 0xFFFD;
            break;
        }
        default: break; /* quote, backslash and slash stand for themselves */
        }
    } else if (c >= 0x80) {
        int n = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xC0 && c < 0xE0 ? 1 : 0;
        if (!n) c = 0xFFFD;
        else {
            static const uint32_t least[] = { 0, 0x80, 0x800, 0x10000 };
            const uint32_t min = least[n];
            c &= 0x3Fu >> n;
            for (; n; n--) {
                if ((*p & 0xC0) != 0x80) { c = 0xFFFD; break; }
                c = c << 6 | (*p++ & 0x3F);
            }
            if (c < min || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) c = 0xFFFD;
        }
    }
    *pp = (const char *)p;
    return c;
}

static int put_utf8(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63));
        o[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63));
    o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Text that is safe to draw: no control, bidi or invisible characters. */
typedef struct { char *buf; size_t cap, len; int chars, limit, space, verbatim; } Text;

static int is_blank(uint32_t c) {
    return c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F || c == 0x205F || c == 0x2800 || c == 0x3000;
}

static int is_unsafe(uint32_t c) {
    return c < 0x20 || (c >= 0x7F && c < 0xA0) ||                 /* C0, C1: escape sequences */
           c == 0x61C || c == 0x200E || c == 0x200F ||            /* bidi */
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
           c == 0xAD || c == 0x34F || c == 0x115F || c == 0x1160 || /* invisible */
           c == 0x180E || c == 0x200B || c == 0x3164 || c == 0xFEFF || c == 0xFFA0 ||
           (c >= 0xFFF9 && c <= 0xFFFB) || (c >= 0xE0000 && c <= 0xE0FFF);
}

/* verbatim (names and paths) keeps blanks; line breaks and tabs always collapse */
static void text_add(Text *t, uint32_t c) {
    if ((c >= '\t' && c <= '\r') || c == 0x85 || c == 0x2028 || c == 0x2029 ||
        (!t->verbatim && is_blank(c))) {
        t->space = t->chars > 0;
        return;
    }
    if (is_unsafe(c)) return;
    if (t->len + 6 > t->cap) return;
    if (t->space && t->chars < t->limit) { t->buf[t->len++] = ' '; t->chars++; }
    t->space = 0;
    if (t->chars < t->limit) { t->len += (size_t)put_utf8(t->buf + t->len, c); t->chars++; }
    t->buf[t->len] = 0;
}

static void text_json(Text *t, const char *p) {
    uint32_t c;
    if (!p || *p != '"') return;
    p++;
    while (t->chars < t->limit && t->len + 6 <= t->cap && (c = next_cp(&p))) text_add(t, c);
}

/* exact string value, for paths handed to the system */
static int raw_json(const char *p, char *out, size_t cap) {
    size_t n = 0;
    uint32_t c;
    if (!p || *p != '"') return 0;
    p++;
    while ((c = next_cp(&p))) {
        if (n + 5 > cap) return 0;
        n += (size_t)put_utf8(out + n, c);
    }
    out[n] = 0;
    return n > 0;
}

/* ---------- last prompt ---------- */
/* Cheap filter before any parsing. Searching for the rare ':' beats memmem, slow on macOS. */
static int has_user_type(const char *line, size_t len) {
    static const char KEY[] = "\"type\":\"user\"";
    const size_t colon = 6, klen = sizeof KEY - 1;
    if (len < klen) return 0;
    const char *p = line + colon, *stop = line + len - (klen - colon) + 1;
    while (p < stop && (p = memchr(p, ':', (size_t)(stop - p)))) {
        if (!memcmp(p - colon, KEY, klen)) return 1;
        p++;
    }
    return 0;
}

typedef struct { char when[8], text[PROMPT_CHARS * 4 + 8]; } Prompt;

static int starts_tag(const char *s) {
    if (!s || *s != '"') return 0;
    s++;
    return next_cp(&s) == '<'; /* decoded, as it may be written < */
}

/* Fills pr and returns 1 when line is a prompt the user typed. */
static int parse_prompt(const char *line, size_t len, Prompt *pr) {
    if (len > MAX_LINE) return 0;
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    if (!len || line[len - 1] != '}') return 0; /* blank or still being written */
    if (!has_user_type(line, len)) return 0;
    if (!is_str(member(line, "type"), "user")) return 0;

    const char *c = member(member(line, "message"), "content");
    if (!c) return 0;
    Text t = { pr->text, sizeof pr->text, 0, 0, PROMPT_CHARS, 0, 0 };
    pr->text[0] = 0;
    if (*c == '"') {
        if (starts_tag(c)) return 0; /* command output, notifications */
        text_json(&t, c);
    } else if (*c == '[') {
        int first = 1;
        for (c = ws(c + 1); *c == '{';) {
            const char *type = member(c, "type");
            if (is_str(type, "tool_result")) return 0; /* tool output cannot pose as a prompt */
            if (is_str(type, "text")) {
                const char *s = member(c, "text");
                if (first && starts_tag(s)) return 0;
                if (!first) text_add(&t, ' ');
                first = 0;
                text_json(&t, s);
            }
            c = ws(skip_val(c));
            if (*c != ',') break;
            c = ws(c + 1);
        }
    }
    if (!t.len || !strncmp(pr->text, "[Request interrupted by user", 28)) return 0;
    /* last, as an absent key costs a walk over the whole line, and tool results can be megabytes */
    if (is_true(member(line, "isMeta")) || is_true(member(line, "isSidechain")) ||
        is_true(member(line, "isCompactSummary")) || member(line, "toolUseResult")) return 0;
    /* still being written, damaged, or two records fused: '}' at the end is not enough */
    if (closing(line) != line + len) return 0;

    pr->when[0] = 0;
    const char *ts = member(line, "timestamp");
    struct tm tm = {0};
    if (ts && *ts == '"' && sscanf(ts + 1, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon,
                                  &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t at = timegm(&tm); /* the transcript is in UTC */
        struct tm local;
        if (localtime_r(&at, &local)) strftime(pr->when, sizeof pr->when, "%H:%M", &local);
    }
    return 1;
}

static const char *rfind_nl(const char *start, const char *end) {
    const uint64_t ones = 0x0101010101010101ULL;
    while (end - start >= 512 && !memchr(end - 512, '\n', 512)) end -= 512; /* long lines */
    while (end - start >= 8) {
        uint64_t w;
        memcpy(&w, end - 8, 8);
        w ^= ones * '\n';
        if ((w - ones) & ~w & (ones << 7)) break; /* this word holds a newline */
        end -= 8;
    }
    while (end > start)
        if (*--end == '\n') return end;
    return NULL;
}

/*
 * buf[head..cap) always holds the unfinished line that starts at or before file
 * offset pos. Only that one line is kept, so memory follows the longest line
 * visited, not the file size: the buffer has room for MAX_LINE but fills from
 * its top, and pages never touched use no RAM.
 */
static int last_prompt(const char *path, Prompt *pr) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    size_t cap = MAX_LINE + CHUNK, head = cap; /* a chunk of room for the line before */
    char *buf = NULL;
    int found = 0, skipping = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || !(buf = malloc(cap + 1))) goto done;

    off_t pos = st.st_size;
    while (pos > 0 && st.st_size - pos < MAX_SCAN) {
        size_t n = pos < CHUNK ? (size_t)pos : CHUNK;
        if (head < n) { /* line too long to hold */
            skipping = 1;
            head = cap;
        }
        head -= n;
        pos -= (off_t)n;
        if (pread(fd, buf + head, n, pos) != (ssize_t)n) goto done;

        char *end = buf + cap, *from = buf + head + n;
        const char *nl;
        while ((nl = rfind_nl(buf + head, from))) {
            char *line = buf + (nl - buf) + 1;
            *end = 0;
            if (!skipping && parse_prompt(line, (size_t)(end - line), pr)) { found = 1; goto done; }
            skipping = 0;
            end = from = line - 1;
        }
        if (skipping) {
            head = cap;
        } else if (end != buf + cap) {
            size_t keep = (size_t)(end - (buf + head));
            memmove(buf + cap - keep, buf + head, keep);
            head = cap - keep;
        }
        if (!pos && !skipping) {
            buf[cap] = 0;
            found = parse_prompt(buf + head, cap - head, pr);
        }
    }
done:
    free(buf);
    close(fd);
    return found;
}

/* ---------- git ---------- */
/* the nearest directory holding .git, on the physical path as git sees it */
static int find_root(const char *dir, char *p, size_t cap) {
    if (*dir != '/') return 0;
    char *real = realpath(dir, NULL);
    if (!real) return 0;
    size_t n = strlen(real);
    int fits = n + 6 < cap;
    if (fits) memcpy(p, real, n + 1);
    free(real);
    if (!fits) return 0;
    for (;;) {
        struct stat st;
        strcpy(p + n, "/.git");
        if (!stat(p, &st)) {
            if (n) p[n] = 0;
            else strcpy(p, "/");
            return 1;
        }
        while (n && p[n - 1] != '/') n--;
        if (!n) return 0;
        n--;
    }
}

static volatile sig_atomic_t child; /* the running git */

/* a git that hangs is killed; again a second later, in case its child holds the pipe */
static void on_timeout(int sig) {
    (void)sig;
    if (child > 0) kill((pid_t)child, SIGKILL);
    alarm(1);
}

/* a cancelled status line takes its git with it */
static void on_cancel(int sig) {
    if (child > 0) kill((pid_t)child, SIGTERM);
    signal(sig, SIG_DFL);
    raise(sig);
}

/*
 * Starts git without waiting, so that it runs while the transcript is being read.
 * A repository's contents must not be able to run anything:
 * - git starts at the root and refuses bare repositories, as a clone can carry
 *   one, config included, as ordinary files in a subdirectory;
 * - fsmonitor and hooks are off, as config can point them at any program;
 * - nothing is fetched, as that runs transports named in the config;
 * - the working directory is / (see main), so PATH cannot find the project's "git".
 * A clean or process filter in .git/config itself still runs, as it does for
 * git status. A clone cannot deliver one; a copied .git directory can.
 */
static int git_start(const char *dir) {
    char root[4096];
    if (!find_root(dir, root, sizeof root)) return -1;
    if (setenv("GIT_NO_LAZY_FETCH", "1", 1) || setenv("GIT_ALLOW_PROTOCOL", "", 1)) return -1;
    /* spares git the locale lookup and translations; its character set is left alone */
    setenv("LANG", "C", 0);
    setenv("LC_MESSAGES", "C", 1);
    char *const argv[] = {
        "git", "--no-optional-locks",
        "-c", "core.fsmonitor=", /* empty, as git before 2.36 runs "false" as a command */
        "-c", "core.hooksPath=/dev/null",
        "-c", "safe.bareRepository=explicit",
        "-c", "status.renames=false",
        "-C", root, "status", "--porcelain=v2", "--branch",
        "--no-ahead-behind", /* that count can walk thousands of commits */
        NULL,
    };
    int fd[2];
    pid_t pid;
    if (pipe(fd)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    /* or whatever git leaves in the background would keep the pipe open */
    if (fd[1] > 2) posix_spawn_file_actions_addclose(&fa, fd[1]);

    struct sigaction sa; /* no SA_RESTART, so a timeout ends a blocked read */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_timeout;
    sigaction(SIGALRM, &sa, NULL);

    /* no cancel between the spawn and recording the pid; git gets the original mask */
    const int cancels[] = { SIGTERM, SIGINT, SIGHUP };
    sigset_t hold, old;
    sigemptyset(&hold);
    for (size_t i = 0; i < sizeof cancels / sizeof *cancels; i++) {
        if (signal(cancels[i], on_cancel) == SIG_IGN) signal(cancels[i], SIG_IGN);
        sigaddset(&hold, cancels[i]);
    }
    sigprocmask(SIG_BLOCK, &hold, &old);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setsigmask(&at, &old);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    int err = posix_spawnp(&pid, "git", &fa, &at, argv, environ);
    if (!err) child = pid;
    sigprocmask(SIG_SETMASK, &old, NULL);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (err) { close(fd[0]); return -1; }
    alarm(GIT_TIMEOUT);
    return fd[0];
}

/* "main", "main*" when dirty, or a short hash when detached */
static void git_finish(int fd, char *out, size_t cap) {
    if (fd < 0) return;
    FILE *f = fdopen(fd, "r");
    char line[2048], oid[8] = "", name[1024] = "";
    int dirty = 0, bol = 1;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            size_t n = strcspn(line, "\n");
            int starts = bol;
            bol = line[n] == '\n';
            line[n] = 0;
            if (!starts) continue;
            if (line[0] != '#') { dirty = 1; break; } /* one change is enough */
            if (!strncmp(line, "# branch.oid ", 13)) snprintf(oid, sizeof oid, "%.7s", line + 13);
            else if (!strncmp(line, "# branch.head ", 14)) snprintf(name, sizeof name, "%.1023s", line + 14);
        }
        fclose(f);
    } else {
        close(fd);
    }
    pid_t pid = (pid_t)child;
    if (dirty) kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    child = 0;
    alarm(0);

    if (!strcmp(name, "(unknown)")) return;
    const char *label = strcmp(name, "(detached)") ? name : oid;
    if (*label) snprintf(out, cap, "%s%s", label, dirty ? "*" : "");
    for (char *c = out; (c = strchr(c, '|'));) *c = '_'; /* must not pass for a separator */
}

/* ---------- rendering ---------- */
static void put_raw(const char *s, int limit) {
    char buf[4096];
    Text t = { buf, sizeof buf, 0, 0, limit, 0, 1 };
    buf[0] = 0;
    uint32_t c;
    while (*s) {
        if (*s == '"' || *s == '\\') { text_add(&t, (unsigned char)*s++); continue; }
        if (!(c = next_cp(&s))) break;
        text_add(&t, c);
    }
    fputs(buf, stdout);
}

static void sep(void) { fputs(" " SEP "|" RST " ", stdout); }

static void put_pct(double v) {
    long long p = (long long)v;
    if ((double)p > v) p--;
    printf("%s%lld%%" RST, p < 50 ? GREEN : p < 80 ? YELLOW : RED, p);
}

static void put_limit(const char *win, const char *label, double now) {
    double used, reset;
    if (!num(member(win, "used_percentage"), &used)) return;
    sep();
    printf(DIM "%s" RST " ", label);
    put_pct(used);
    if (!num(member(win, "resets_at"), &reset)) return;
    long long s = reset > now ? (long long)(reset - now) : 0;
    fputs(" " DIM "(", stdout);
    if (s >= 86400) printf("%lldd%lldh", s / 86400, s % 86400 / 3600);
    else if (s >= 3600) printf("%lldh%lldm", s / 3600, s % 3600 / 60);
    else printf("%lldm", s / 60);
    fputs(")" RST, stdout);
}

static char *read_stdin(void) {
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        ssize_t n = read(0, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
        if (len + 1 == cap) {
            char *nb = cap < MAX_PAYLOAD ? realloc(buf, cap * 2) : NULL;
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
    }
    buf[len] = 0;
    return buf;
}

int main(void) {
    char *input = read_stdin();
    if (!input) return 1;
    const char *j = ws(input);

    char dir[4096], path[4096], branch[1100] = "";
    if (!raw_json(member(member(j, "workspace"), "current_dir"), dir, sizeof dir) &&
        !raw_json(member(j, "cwd"), dir, sizeof dir) && !getcwd(dir, sizeof dir)) dir[0] = 0;
    if (chdir("/")) return 1; /* from here on nothing relative resolves inside the project */
    int git = git_start(dir);
    Prompt pr;
    int have_prompt = raw_json(member(j, "transcript_path"), path, sizeof path) && *path == '/' &&
                      last_prompt(path, &pr);
    git_finish(git, branch, sizeof branch);

    /* line 1 */
    char word[256] = "";
    Text t = { word, sizeof word, 0, 0, 48, 0, 0 };
    text_json(&t, member(member(j, "model"), "display_name"));
    printf(DIM "%s" RST, t.len ? word : "Claude");

    t = (Text){ word, sizeof word, 0, 0, 16, 0, 0 };
    text_json(&t, member(member(j, "effort"), "level"));
    if (t.len) { sep(); printf(DIM "%s" RST, word); }

    if (*branch) { sep(); fputs(YELLOW, stdout); put_raw(branch, 1024); fputs(RST, stdout); }

    double used = 0;
    if (!num(member(member(j, "context_window"), "used_percentage"), &used)) used = 0;
    sep();
    put_pct(used);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = (double)tv.tv_sec + tv.tv_usec / 1e6;
    const char *limits = member(j, "rate_limits");
    put_limit(member(limits, "five_hour"), "5h", now);
    put_limit(member(limits, "seven_day"), "7d", now);

    /* line 2 */
    if (have_prompt) {
        fputs("\n" SEP "▸" RST " ", stdout);
        if (*pr.when) printf(PURPLE "%s" RST " ", pr.when);
        fputs(pr.text, stdout);
    }

    /* line 3 */
    fputs("\n" SEP "└" RST " " BLUE, stdout);
    const char *home = getenv("HOME"), *d = dir;
    size_t h = home ? strlen(home) : 0;
    if (h > 1 && !strncmp(dir, home, h) && (!dir[h] || dir[h] == '/')) { putchar('~'); d += h; }
    put_raw(d, 1024);
    fputs(RST, stdout);

    free(input);
    return 0;
}
