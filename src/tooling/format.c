#include "format.h"
#include "compat.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
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

static size_t line_indent(const char *line) {
    size_t n = 0;
    while (line[n] == ' ' || line[n] == '\t') n++;
    return n;
}

static const char *line_trim(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

static bool line_blank(const char *line) {
    return *line_trim(line) == '\0';
}

static bool declaration_line(const char *line) {
    const char *p = line_trim(line);
    return strncmp(p, "method ", 7) == 0 ||
           strncmp(p, "define ", 7) == 0 ||
           strncmp(p, "type ", 5) == 0 ||
           strncmp(p, "class ", 6) == 0 ||
           strncmp(p, "instance ", 9) == 0 ||
           strncmp(p, "(method ", 8) == 0 ||
           strncmp(p, "(define ", 8) == 0;
}

static char *doc_payload(const char *line, bool *glyph) {
    const char *p = line_trim(line);
    bool is_glyph = strncmp(p, "╭─ ", strlen("╭─ ")) == 0;
    bool is_inline = strncmp(p, ":doc", 4) == 0 &&
                     (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == '"');
    if (!is_glyph && !is_inline) return NULL;
    if (glyph) *glyph = is_glyph;
    if (is_glyph) return strdup(p + strlen("╭─ "));
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return strdup(p);
    p++;
    const char *end = p;
    while (*end) {
        if (*end == '\\' && end[1]) { end += 2; continue; }
        if (*end == '"') break;
        end++;
    }
    /* Leave multiline literals untouched; a physical-line formatter must not
     * detach their continuation lines from the declaration. */
    if (*end != '"') return NULL;
    char *out = malloc((size_t)(end - p) + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (const char *q = p; q < end; q++) {
        if (*q == '\\' && q + 1 < end) {
            q++;
            if (*q == 'n') out[n++] = '\n';
            else if (*q == 't') out[n++] = '\t';
            else out[n++] = *q;
        } else out[n++] = *q;
    }
    out[n] = '\0';
    return out;
}

static void append_doc_inline(Buffer *out, size_t indent, const char *doc) {
    for (size_t i = 0; i < indent; i++) putn(out, " ", 1);
    putsb(out, ":doc \"");
    for (const char *p = doc; *p; p++) {
        if (*p == '\\' || *p == '"') putn(out, "\\", 1);
        if (*p == '\n') putsb(out, "\\n");
        else if (*p == '\t') putsb(out, "\\t");
        else putn(out, p, 1);
    }
    putsb(out, "\"");
}

static void append_doc_glyph(Buffer *out, size_t indent, const char *doc) {
    for (size_t i = 0; i < indent; i++) putn(out, " ", 1);
    putsb(out, "╭─ ");
    putsb(out, doc);
}

static char *doc_for_style(const char *doc, FormatDocStyle style) {
    size_t n = strlen(doc);
    if (style == FORMAT_DOC_GLYPH) {
        /* Glyph headings are presentation labels; terminal punctuation is
         * intentionally omitted from that visual form. */
        if (n && doc[n - 1] == '.') n--;
    } else if (n && doc[n - 1] != '.' && doc[n - 1] != '!' && doc[n - 1] != '?') {
        /* Inline docstrings use sentence punctuation as their presentation. */
        n++;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, doc, n);
    if (style == FORMAT_DOC_INLINE && n == strlen(doc) + 1) out[n - 1] = '.';
    out[n] = '\0';
    return out;
}

/* Convert documentation comments without changing the semantic docstring. */
static char *format_docstrings(const char *source, FormatDocStyle style) {
    size_t count = 0;
    char **lines = split_lines(source, &count);
    if (!count) return strdup(source);
    char **insert = calloc(count + 1, sizeof(*insert));
    bool *skip = calloc(count, sizeof(*skip));
    if (!insert || !skip) { free(insert); free(skip); goto done; }

    for (size_t i = 0; i < count; i++) {
        bool glyph = false;
        char *doc = doc_payload(lines[i], &glyph);
        if (!doc || (style == FORMAT_DOC_GLYPH) == glyph) { free(doc); continue; }
        char *presentation = doc_for_style(doc, style);
        if (!presentation) { free(doc); continue; }
        size_t target = SIZE_MAX;
        size_t indent = line_indent(lines[i]);
        if (style == FORMAT_DOC_INLINE) {
            /* A glyph comment belongs immediately before the declaration. */
            size_t decl = i + 1;
            while (decl < count && line_blank(lines[decl])) decl++;
            if (decl < count && declaration_line(lines[decl])) {
                size_t last = decl;
                size_t decl_indent = line_indent(lines[decl]);
                for (size_t k = decl + 1; k < count; k++) {
                    if (!line_blank(lines[k]) && line_indent(lines[k]) <= decl_indent) break;
                    if (!line_blank(lines[k])) last = k;
                }
                target = last + 1;
                indent = decl_indent + 4;
                Buffer b = {0};
                append_doc_inline(&b, indent, presentation);
                insert[target] = b.data;
            }
        } else {
            /* An inline doc belongs to the nearest enclosing declaration. */
            size_t decl = i;
            while (decl > 0) {
                decl--;
                if (declaration_line(lines[decl]) && line_indent(lines[decl]) <= indent) {
                    target = decl; indent = line_indent(lines[decl]); break;
                }
            }
            if (target != SIZE_MAX) {
                Buffer b = {0};
                append_doc_glyph(&b, indent, presentation);
                insert[target] = b.data;
            }
        }
        if (target != SIZE_MAX && insert[target]) skip[i] = true;
        free(presentation);
        free(doc);
    }

    {
        Buffer out = {0};
        bool trailing_nl = source[0] && source[strlen(source) - 1] == '\n';
        for (size_t i = 0; i <= count; i++) {
            if (insert[i]) {
                putsb(&out, insert[i]);
                putsb(&out, "\n");
            }
            if (i == count || skip[i]) continue;
            putsb(&out, lines[i]);
            if (i + 1 < count || trailing_nl) putsb(&out, "\n");
        }
        for (size_t i = 0; i < count; i++) free(insert[i]);
        free(insert); free(skip);
        for (size_t i = 0; i < count; i++) free(lines[i]);
        free(lines);
        return out.data ? out.data : strdup("");
    }
done:
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return strdup(source);
}

static char *format_glyph(const char *source) {
    size_t count = 0; char **lines = split_lines(source, &count); Buffer out = {0};
    bool trailing_nl = source[0] && source[strlen(source) - 1] == '\n';
    for (size_t i = 0; i < count; i++) {
        const char *line = lines[i], *end = line + strlen(line);
        const char *pipe = guard_pipe(line, end);
        const char *arrow = pipe ? code_find(pipe + 1, end, "->") : NULL;
        if (!pipe || !arrow || blank_prefix(line, pipe)) {
            putsb(&out, line); if (i + 1 < count || trailing_nl) putsb(&out, "\n");
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
            if (k + 1 < count || trailing_nl) putsb(&out, "\n");
        }
        i = j - 1;
    }
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
    return out.data ? out.data : strdup("");
}

static char *format_ascii(const char *source) {
    size_t count = 0; char **lines = split_lines(source, &count); Buffer out = {0};
    bool trailing_nl = source[0] && source[strlen(source) - 1] == '\n';
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
        if (i + 1 < count || trailing_nl) putsb(&out, "\n");
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

static int format_file(const char *path, FormatControlStyle style, FormatDocStyle doc_style,
                       bool write, bool check, size_t *changed) {
    char *source = read_file(path); if (!source) { fprintf(stderr, "monad format: cannot read %s\n", path); return 2; }
    char *docs = format_docstrings(source, doc_style);
    char *result = style == FORMAT_CONTROL_GLYPH ? format_glyph(docs) : format_ascii(docs);
    free(docs);
    bool differs = strcmp(source, result) != 0; if (differs) (*changed)++;
    int rc = 0;
    if (write && differs) {
        size_t path_len = strlen(path);
        char *temporary = malloc(path_len + 16);
        sprintf(temporary, "%s.format.XXXXXX", path);
        int fd = mkstemp(temporary);
        struct stat original;
#if !defined(_WIN32)
        if (fd >= 0 && stat(path, &original) == 0)
            fchmod(fd, original.st_mode & 07777);
#else
        (void)original;
#endif
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

static int walk(const char *path, FormatControlStyle style, FormatDocStyle doc_style,
                bool write, bool check, size_t *files, size_t *changed) {
    struct stat st; if (lstat(path, &st) != 0) return 2;
    if (S_ISLNK(st.st_mode)) return 0;
    if (S_ISREG(st.st_mode)) { (*files)++; return format_file(path, style, doc_style, write, check, changed); }
    DIR *d = opendir(path); if (!d) return 2; int rc = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat cs; if (lstat(child, &cs) != 0) { rc = 2; continue; }
        if (S_ISLNK(cs.st_mode)) continue;
        if (S_ISDIR(cs.st_mode)) { if (walk(child, style, doc_style, write, true, files, changed)) rc = 2; }
        else { size_t n = strlen(child); if (n >= 4 && !strcmp(child + n - 4, ".mon")) { (*files)++; if (format_file(child, style, doc_style, write, true, changed)) rc = 2; } }
    }
    closedir(d); return rc;
}

int cmd_format(const char *path, FormatControlStyle style, FormatDocStyle doc_style,
               bool write, bool check) {
    if (!path) { fputs("Usage: monad format [--control-flow=<glyph|ascii>] [--docstrings=<inline|glyph>] [--write|--check] <path>\n", stderr); return 2; }
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "monad format: cannot inspect %s: %s\n", path, strerror(errno));
        return 2;
    }
    size_t files = 0, changed = 0; int rc = walk(path, style, doc_style, write, check, &files, &changed);
    if (check || write || S_ISDIR(st.st_mode)) fprintf(stderr, "monad format: %zu file%s, %zu would change\n", files, files == 1 ? "" : "s", changed);
    if (rc) return rc;
    return check && changed ? 1 : 0;
}
