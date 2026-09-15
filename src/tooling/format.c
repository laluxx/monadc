#include "format.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *data; size_t len, cap; } Buffer;

static void putn(Buffer *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = realloc(b->data, cap); b->cap = cap;
    }
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
static void putsb(Buffer *b, const char *s) { putn(b, s, strlen(s)); }

static const char *code_find(const char *s, const char *end, const char *needle) {
    bool str = false, chr = false, esc = false;
    size_t n = strlen(needle);
    for (const char *p = s; p + n <= end; p++) {
        if (esc) { esc = false; continue; }
        if ((str || chr) && *p == '\\') { esc = true; continue; }
        if (!chr && *p == '"') { str = !str; continue; }
        if (!str && *p == '\'') { chr = !chr; continue; }
        if (!str && !chr && *p == ';') return NULL;
        if (!str && !chr && memcmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static const char *guard_pipe(const char *s, const char *end) {
    bool str = false, chr = false, esc = false;
    int round = 0, square = 0, brace = 0;
    for (const char *p = s; p < end; p++) {
        if (esc) { esc = false; continue; }
        if ((str || chr) && *p == '\\') { esc = true; continue; }
        if (!chr && *p == '"') { str = !str; continue; }
        if (!str && *p == '\'') { chr = !chr; continue; }
        if (str || chr) continue;
        if (*p == ';') return NULL;
        if (*p == '(') round++; else if (*p == ')' && round) round--;
        else if (*p == '[') square++; else if (*p == ']' && square) square--;
        else if (*p == '{') brace++; else if (*p == '}' && brace) brace--;
        else if (*p == '|' && round == 0 && square == 0 && brace == 0 &&
                 p > s && p + 1 < end &&
                 (p[-1] == ' ' || p[-1] == '\t') &&
                 (p[1] == ' ' || p[1] == '\t')) return p;
    }
    return NULL;
}

static bool blank_prefix(const char *s, const char *end) {
    for (; s < end; s++) if (*s != ' ' && *s != '\t') return false;
    return true;
}

static char **split_lines(const char *source, size_t *count) {
    char **lines = NULL; size_t n = 0, cap = 0;
    const char *p = source;
    while (*p) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        if (n == cap) { cap = cap ? cap * 2 : 64; lines = realloc(lines, cap * sizeof(*lines)); }
        lines[n++] = strndup(p, (size_t)(e - p));
        p = *e ? e + 1 : e;
    }
    *count = n; return lines;
}

static char *format_glyph(const char *source) {
    size_t count = 0; char **lines = split_lines(source, &count); Buffer out = {0};
    for (size_t i = 0; i < count; i++) {
        const char *line = lines[i], *end = line + strlen(line);
        const char *pipe = guard_pipe(line, end);
        const char *arrow = pipe ? code_find(pipe + 1, end, "->") : NULL;
        if (!pipe || !arrow || blank_prefix(line, pipe)) {
            putsb(&out, line); if (i + 1 < count || source[strlen(source)-1] == '\n') putsb(&out, "\n");
            continue;
        }
        size_t prefix = (size_t)(pipe - line);
        size_t j = i + 1;
        while (j < count) {
            const char *e = lines[j] + strlen(lines[j]);
            const char *p = guard_pipe(lines[j], e);
            if (!p || !blank_prefix(lines[j], p) || !code_find(p + 1, e, "->")) break;
            j++;
        }
        putn(&out, line, prefix); putsb(&out, "├─"); putn(&out, pipe + 1, (size_t)(end - pipe - 1)); putsb(&out, "\n");
        if (j > i + 1) {
            for (size_t k = 0; k < prefix; k++) putsb(&out, " ");
            putsb(&out, "╰─╮\n");
        }
        for (size_t k = i + 1; k < j; k++) {
            const char *e = lines[k] + strlen(lines[k]);
            const char *p = guard_pipe(lines[k], e);
            const char *rhs = p + 1; while (rhs < e && (*rhs == ' ' || *rhs == '\t')) rhs++;
            bool fallback = strncmp(rhs, "otherwise", 9) == 0 &&
                            (rhs[9] == ' ' || rhs[9] == '\t');
            for (size_t x = 0; x < prefix + 2; x++) putsb(&out, " ");
            if (fallback) {
                const char *a = code_find(rhs + 9, e, "->");
                putsb(&out, "╰───▶ ");
                a += 2; while (a < e && (*a == ' ' || *a == '\t')) a++;
                putn(&out, a, (size_t)(e - a));
            } else {
                putsb(&out, "├─ "); putn(&out, rhs, (size_t)(e - rhs));
            }
            if (k + 1 < count || source[strlen(source)-1] == '\n') putsb(&out, "\n");
        }
        i = j - 1;
    }
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return out.data ? out.data : strdup("");
}

static char *format_ascii(const char *source) {
    size_t count = 0; char **lines = split_lines(source, &count); Buffer out = {0};
    size_t base = 0; bool in_tree = false;
    for (size_t i = 0; i < count; i++) {
        const char *line = lines[i], *end = line + strlen(line);
        const char *branch = code_find(line, end, "├─");
        const char *last = code_find(line, end, "╰─");
        if (branch && !code_find(branch + strlen("├─"), end, "->"))
            branch = NULL;
        if (branch) {
            size_t col = (size_t)(branch - line);
            if (!blank_prefix(line, branch)) { base = col; in_tree = true; putn(&out, line, col); }
            else { if (in_tree && col >= base + 2) col = base; putn(&out, line, col); }
            putsb(&out, "|"); putn(&out, branch + strlen("├─"), (size_t)(end - branch - strlen("├─")));
        } else if (last && strstr(last, "▶")) {
            const char *leaf = strstr(last, "▶") + strlen("▶");
            size_t col = in_tree ? base : (size_t)(last - line);
            putn(&out, line, col); putsb(&out, "| otherwise ->");
            putn(&out, leaf, (size_t)(end - leaf)); in_tree = false;
        } else if (last && strstr(last, "╮")) {
            continue;
        } else {
            putsb(&out, line); in_tree = false;
        }
        if (i + 1 < count || source[strlen(source)-1] == '\n') putsb(&out, "\n");
    }
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return out.data ? out.data : strdup("");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *s = malloc((size_t)n + 1); if (!s) { fclose(f); return NULL; }
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); fclose(f); return NULL; }
    s[n] = 0; fclose(f); return s;
}

static int format_file(const char *path, FormatControlStyle style, bool write, bool check, size_t *changed) {
    char *source = read_file(path); if (!source) { fprintf(stderr, "monad format: cannot read %s\n", path); return 2; }
    char *result = style == FORMAT_CONTROL_GLYPH ? format_glyph(source) : format_ascii(source);
    bool differs = strcmp(source, result) != 0; if (differs) (*changed)++;
    int rc = 0;
    if (write && differs) {
        size_t path_len = strlen(path);
        char *temporary = malloc(path_len + 16);
        sprintf(temporary, "%s.format.XXXXXX", path);
        int fd = mkstemp(temporary);
        struct stat original;
        if (fd >= 0 && stat(path, &original) == 0)
            fchmod(fd, original.st_mode & 07777);
        FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
        bool failed = !f;
        if (f) {
            failed = fwrite(result, 1, strlen(result), f) != strlen(result) ||
                     fflush(f) != 0;
            if (fclose(f) != 0) failed = true;
        }
        if (!failed && rename(temporary, path) != 0) failed = true;
        if (failed) {
            remove(temporary);
            fprintf(stderr, "monad format: cannot replace %s: %s\n", path, strerror(errno));
            rc = 2;
        }
        free(temporary);
    }
    else if (!check) fputs(result, stdout);
    free(source); free(result); return rc;
}

static int walk(const char *path, FormatControlStyle style, bool write, bool check, size_t *files, size_t *changed) {
    struct stat st; if (lstat(path, &st) != 0) return 2;
    if (S_ISLNK(st.st_mode)) return 0;
    if (S_ISREG(st.st_mode)) { (*files)++; return format_file(path, style, write, check, changed); }
    DIR *d = opendir(path); if (!d) return 2; int rc = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat cs; if (lstat(child, &cs) != 0) { rc = 2; continue; }
        if (S_ISLNK(cs.st_mode)) continue;
        if (S_ISDIR(cs.st_mode)) { if (walk(child, style, write, true, files, changed)) rc = 2; }
        else { size_t n = strlen(child); if (n >= 4 && !strcmp(child + n - 4, ".mon")) { (*files)++; if (format_file(child, style, write, true, changed)) rc = 2; } }
    }
    closedir(d); return rc;
}

int cmd_format(const char *path, FormatControlStyle style, bool write, bool check) {
    if (!path) { fputs("Usage: monad format --control-flow=<glyph|ascii> [--write|--check] <path>\n", stderr); return 2; }
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "monad format: cannot inspect %s: %s\n", path, strerror(errno));
        return 2;
    }
    size_t files = 0, changed = 0; int rc = walk(path, style, write, check, &files, &changed);
    if (check || write || S_ISDIR(st.st_mode)) fprintf(stderr, "monad format: %zu file%s, %zu would change\n", files, files == 1 ? "" : "s", changed);
    if (rc) return rc;
    return check && changed ? 1 : 0;
}
