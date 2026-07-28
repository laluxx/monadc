#include "reader_syntax.h"
#include "reader.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RS_BINDER, RS_PREFIX, RS_POSTFIX, RS_INFIX } RSRuleKind;

typedef struct {
    RSRuleKind kind;
    char *token;
    char *target;
    char *delimiter;
    int precedence;
    int right_associative;
    char *source_file;
    int source_line;
} RSRule;

typedef struct {
    char *type;
    char *owner_file;
    RSRule *rules;
    size_t rule_count;
    size_t rule_capacity;
} RSReader;

static RSReader *g_readers;
static size_t g_reader_count;
static size_t g_reader_capacity;
static int g_cleanup_registered;

typedef struct RSScope {
    char *owner_file;
    char **allowed_files;
    size_t allowed_count;
    size_t allowed_capacity;
    struct RSScope *previous;
} RSScope;

static RSScope *g_scope;

static char *reader_syntax_absolute_path(const char *path) {
#if defined(_WIN32)
    return _fullpath(NULL, path, 0);
#else
    return realpath(path, NULL);
#endif
}

static char *owner_key(const char *path) {
    if (!path) return strdup("<input>");
    char *absolute = reader_syntax_absolute_path(path);
    return absolute ? absolute : strdup(path);
}

void reader_syntax_scope_push(const char *owner_file) {
    RSScope *scope = calloc(1, sizeof(*scope));
    if (!scope) return;
    scope->owner_file = owner_key(owner_file);
    scope->previous = g_scope;
    g_scope = scope;
}

void reader_syntax_scope_allow(const char *owner_file) {
    if (!g_scope) return;
    char *key = owner_key(owner_file);
    for (size_t i = 0; i < g_scope->allowed_count; i++) {
        if (strcmp(g_scope->allowed_files[i], key) == 0) {
            free(key);
            return;
        }
    }
    if (g_scope->allowed_count == g_scope->allowed_capacity) {
        size_t next_capacity = g_scope->allowed_capacity
                             ? g_scope->allowed_capacity * 2 : 8;
        char **next = realloc(g_scope->allowed_files,
                              next_capacity * sizeof(*next));
        if (!next) { free(key); return; }
        g_scope->allowed_files = next;
        g_scope->allowed_capacity = next_capacity;
    }
    g_scope->allowed_files[g_scope->allowed_count++] = key;
}

void reader_syntax_scope_pop(void) {
    if (!g_scope) return;
    RSScope *scope = g_scope;
    g_scope = scope->previous;
    free(scope->owner_file);
    for (size_t i = 0; i < scope->allowed_count; i++)
        free(scope->allowed_files[i]);
    free(scope->allowed_files);
    free(scope);
}

static int owner_is_active(const char *owner_file) {
    if (!g_scope) return 1;
    if (strcmp(g_scope->owner_file, owner_file) == 0) return 1;
    for (size_t i = 0; i < g_scope->allowed_count; i++)
        if (strcmp(g_scope->allowed_files[i], owner_file) == 0) return 1;
    return 0;
}

static int reader_debug(void) {
    const char *value = getenv("MONAD_READER_DEBUG");
    return value && value[0] && strcmp(value, "0") != 0;
}

typedef struct {
    const char *cursor;
    const char *end;
    RSReader *reader;
    const char *filename;
    int line;
    char error[256];
} RSParser;

static char *slice_dup(const char *start, const char *end) {
    size_t n = (size_t)(end - start);
    char *s = malloc(n + 1);
    memcpy(s, start, n);
    s[n] = '\0';
    return s;
}

static void skip_space(RSParser *p) {
    while (p->cursor < p->end && (*p->cursor == ' ' || *p->cursor == '\t'))
        p->cursor++;
}

static RSReader *find_reader(const char *type) {
    RSReader *found = NULL;
    for (size_t i = 0; i < g_reader_count; i++) {
        RSReader *candidate = &g_readers[i];
        if (strcmp(candidate->type, type) != 0 ||
            !owner_is_active(candidate->owner_file))
            continue;
        if (found && strcmp(found->owner_file, candidate->owner_file) != 0) {
            fprintf(stderr,
                    "%s:1:1: error: ambiguous reader-syntax %s imported from %s and %s\n",
                    g_scope ? g_scope->owner_file : "<input>", type,
                    found->owner_file, candidate->owner_file);
            return NULL;
        }
        found = candidate;
    }
    return found;
}

static RSReader *ensure_reader(const char *type, const char *owner_file) {
    char *key = owner_key(owner_file);
    for (size_t i = 0; i < g_reader_count; i++) {
        if (strcmp(g_readers[i].type, type) == 0 &&
            strcmp(g_readers[i].owner_file, key) == 0) {
            free(key);
            return &g_readers[i];
        }
    }
    if (g_reader_count == g_reader_capacity) {
        size_t next_capacity = g_reader_capacity ? g_reader_capacity * 2 : 8;
        RSReader *next = realloc(g_readers, next_capacity * sizeof(*next));
        if (!next) return NULL;
        g_readers = next;
        g_reader_capacity = next_capacity;
    }
    RSReader *r = &g_readers[g_reader_count++];
    memset(r, 0, sizeof(*r));
    r->type = strdup(type);
    r->owner_file = key;
    if (!r->type || !r->owner_file) {
        free(r->type); free(r->owner_file); g_reader_count--; return NULL;
    }
    return r;
}

static RSRule *match_rule(RSParser *p, RSRuleKind kind) {
    RSRule *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < p->reader->rule_count; i++) {
        RSRule *r = &p->reader->rules[i];
        size_t n = strlen(r->token);
        int ends_as_identifier = n &&
            (isalnum((unsigned char)r->token[n - 1]) || r->token[n - 1] == '_');
        int continues_identifier = (size_t)(p->end - p->cursor) > n &&
            (isalnum((unsigned char)p->cursor[n]) || p->cursor[n] == '_');
        if (r->kind == kind && n > best_len &&
            (size_t)(p->end - p->cursor) >= n &&
            memcmp(p->cursor, r->token, n) == 0 &&
            !(ends_as_identifier && continues_identifier)) {
            best = r;
            best_len = n;
        }
    }
    return best;
}

static int canonical_target_call(RSReader *reader, const char *start,
                                 const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    if (start >= end || *start != '(') return 0;
    start++;
    while (start < end && isspace((unsigned char)*start)) start++;
    const char *head = start;
    while (start < end && !isspace((unsigned char)*start) && *start != ')') start++;
    size_t head_length = (size_t)(start - head);
    if (!head_length) return 0;
    for (size_t i = 0; i < reader->rule_count; i++) {
        const char *target = reader->rules[i].target;
        if (strlen(target) == head_length && memcmp(target, head, head_length) == 0)
            return 1;
    }
    return 0;
}

static char *parse_expression(RSParser *p, int minimum_precedence);

static char *parse_primary(RSParser *p) {
    skip_space(p);
    if (p->cursor >= p->end) {
        snprintf(p->error, sizeof(p->error), "expected an expression");
        return NULL;
    }

    RSRule *binder = match_rule(p, RS_BINDER);
    if (binder) {
        p->cursor += strlen(binder->token);
        const char *name_start = p->cursor;
        while (p->cursor < p->end &&
               (isalnum((unsigned char)*p->cursor) || *p->cursor == '_'))
            p->cursor++;
        const char *delimiter = binder->delimiter ? binder->delimiter : ".";
        size_t delimiter_length = strlen(delimiter);
        if (p->cursor == name_start ||
            (size_t)(p->end - p->cursor) < delimiter_length ||
            memcmp(p->cursor, delimiter, delimiter_length) != 0) {
            snprintf(p->error, sizeof(p->error),
                     "binder '%s' requires a name followed by '%s'",
                     binder->token, delimiter);
            return NULL;
        }
        char *name = slice_dup(name_start, p->cursor);
        p->cursor += delimiter_length;
        char *body = parse_expression(p, binder->precedence);
        if (!body) { free(name); return NULL; }
        size_t size = strlen(binder->target) + strlen(name) + strlen(body) + 8;
        char *out = malloc(size);
        snprintf(out, size, "(%s \"%s\" %s)", binder->target, name, body);
        free(name); free(body);
        return out;
    }

    RSRule *prefix = match_rule(p, RS_PREFIX);
    if (prefix) {
        if (reader_debug())
            fprintf(stderr, "[reader-syntax] prefix %s -> %s\n",
                    prefix->token, prefix->target);
        p->cursor += strlen(prefix->token);
        char *operand = parse_primary(p);
        if (!operand) return NULL;
        size_t size = strlen(prefix->target) + strlen(operand) + 4;
        char *out = malloc(size);
        snprintf(out, size, "(%s %s)", prefix->target, operand);
        free(operand);
        return out;
    }

    if (*p->cursor == '(') {
        p->cursor++;
        char *inside = parse_expression(p, 0);
        skip_space(p);
        if (!inside || p->cursor >= p->end || *p->cursor != ')') {
            free(inside);
            snprintf(p->error, sizeof(p->error), "unclosed parenthesized expression");
            return NULL;
        }
        p->cursor++;
        return inside;
    }

    const char *start = p->cursor;
    while (p->cursor < p->end && !isspace((unsigned char)*p->cursor) &&
           *p->cursor != '(' && *p->cursor != ')') {
        int is_operator = 0;
        for (size_t i = 0; i < p->reader->rule_count; i++) {
            RSRule *r = &p->reader->rules[i];
            size_t n = strlen(r->token);
            if ((r->kind == RS_PREFIX || r->kind == RS_POSTFIX ||
                 r->kind == RS_INFIX || r->kind == RS_BINDER) &&
                p->cursor > start && (size_t)(p->end - p->cursor) >= n &&
                memcmp(p->cursor, r->token, n) == 0) {
                is_operator = 1;
                break;
            }
        }
        if (is_operator) break;
        p->cursor++;
    }
    if (p->cursor == start) {
        snprintf(p->error, sizeof(p->error), "unexpected token near '%.*s'",
                 12, p->cursor);
        return NULL;
    }
    return slice_dup(start, p->cursor);
}

static char *parse_expression(RSParser *p, int minimum_precedence) {
    char *left = parse_primary(p);
    if (!left) return NULL;
    for (;;) {
        skip_space(p);
        RSRule *postfix = match_rule(p, RS_POSTFIX);
        if (postfix) {
            p->cursor += strlen(postfix->token);
            size_t size = strlen(postfix->target) + strlen(left) + 4;
            char *combined = malloc(size);
            snprintf(combined, size, "(%s %s)", postfix->target, left);
            free(left);
            left = combined;
            continue;
        }
        RSRule *op = match_rule(p, RS_INFIX);
        if (!op || op->precedence < minimum_precedence) break;
        p->cursor += strlen(op->token);
        int next_min = op->right_associative ? op->precedence : op->precedence + 1;
        char *right = parse_expression(p, next_min);
        if (!right) { free(left); return NULL; }
        size_t size = strlen(op->target) + strlen(left) + strlen(right) + 5;
        char *combined = malloc(size);
        snprintf(combined, size, "(%s %s %s)", op->target, left, right);
        free(left); free(right);
        left = combined;
    }
    return left;
}

static int line_indent(const char *start, const char *end) {
    int n = 0;
    while (start < end && (*start == ' ' || *start == '\t')) {
        n += *start++ == '\t' ? 8 : 1;
    }
    return n;
}

static const char *rule_kind_name(RSRuleKind kind) {
    switch (kind) {
    case RS_BINDER: return "binder";
    case RS_PREFIX: return "prefix";
    case RS_POSTFIX:return "postfix";
    case RS_INFIX:  return "infix";
    }
    return "reader";
}

static int optional_text_equal(const char *left, const char *right) {
    if (!left || !right) return left == right;
    return strcmp(left, right) == 0;
}

static int parse_declaration_line(RSReader *reader, const char *start,
                                  const char *end, const char *filename,
                                  int line_number) {
    char *line = slice_dup(start, end);
    char *original = strdup(line);
    char *words[6] = {0};
    size_t word_count = 0;
    int too_many_words = 0;
    char *save = NULL;
    for (char *word = strtok_r(line, " \t", &save); word;
         word = strtok_r(NULL, " \t", &save)) {
        if (word_count == 6) { too_many_words = 1; break; }
        words[word_count++] = word;
    }

    RSRuleKind parsed_kind = RS_PREFIX;
    char *surface = NULL;
    char *delimiter = NULL;
    const char *target = NULL;
    const char *assoc = "left";
    int precedence = 0;
    int valid = 0;

    if (!too_many_words && word_count >= 2) {
        const char *pattern = words[0];
        size_t pattern_length = strlen(pattern);
        const char *first_hole = strchr(pattern, '_');
        const char *second_hole = first_hole ? strchr(first_hole + 1, '_') : NULL;
        const char *third_hole = second_hole ? strchr(second_hole + 1, '_') : NULL;
        target = words[1];

        if (first_hole && !third_hole) {
            if (!second_hole && word_count == 2 && first_hole == pattern + pattern_length - 1 && first_hole != pattern) {
                parsed_kind = RS_PREFIX;
                surface = strndup(pattern, (size_t)(first_hole - pattern));
                valid = surface && surface[0];
            } else if (!second_hole && word_count == 2 && first_hole == pattern && pattern_length > 1) {
                parsed_kind = RS_POSTFIX;
                surface = strdup(pattern + 1);
                valid = surface && surface[0];
            } else if (second_hole && first_hole == pattern &&
                       second_hole == pattern + pattern_length - 1 && word_count == 4) {
                char *power_end = NULL;
                long power = strtol(words[2], &power_end, 10);
                if (power_end && !*power_end && power >= 0 && power <= 2147483647L &&
                    (strcmp(words[3], "left") == 0 || strcmp(words[3], "right") == 0)) {
                    parsed_kind = RS_INFIX;
                    surface = strndup(first_hole + 1,
                                      (size_t)(second_hole - first_hole - 1));
                    precedence = (int)power;
                    assoc = words[3];
                    valid = surface && surface[0];
                }
            } else if (second_hole && first_hole != pattern &&
                       second_hole == pattern + pattern_length - 1 && word_count == 3) {
                char *power_end = NULL;
                long power = strtol(words[2], &power_end, 10);
                if (power_end && !*power_end && power >= 0 && power <= 2147483647L) {
                    parsed_kind = RS_BINDER;
                    surface = strndup(pattern, (size_t)(first_hole - pattern));
                    delimiter = strndup(first_hole + 1,
                                        (size_t)(second_hole - first_hole - 1));
                    precedence = (int)power;
                    valid = surface && surface[0] && delimiter && delimiter[0];
                }
            }
        } else if (!first_hole) {
            /* Backward-compatible spelling for existing source during migration. */
            if (word_count == 3 && strcmp(words[0], "prefix") == 0) {
                parsed_kind = RS_PREFIX; surface = strdup(words[1]); target = words[2]; valid = 1;
            } else if (word_count == 4 && strcmp(words[0], "binder") == 0) {
                char *power_end = NULL; long power = strtol(words[3], &power_end, 10);
                if (power_end && !*power_end && power >= 0 && power <= 2147483647L) {
                    parsed_kind = RS_BINDER; surface = strdup(words[1]); target = words[2];
                    delimiter = strdup("."); precedence = (int)power; valid = 1;
                }
            } else if (word_count == 5 && strcmp(words[0], "infix") == 0) {
                char *power_end = NULL; long power = strtol(words[3], &power_end, 10);
                if (power_end && !*power_end && power >= 0 && power <= 2147483647L &&
                    (strcmp(words[4], "left") == 0 || strcmp(words[4], "right") == 0)) {
                    parsed_kind = RS_INFIX; surface = strdup(words[1]); target = words[2];
                    precedence = (int)power; assoc = words[4]; valid = 1;
                }
            }
        }
    }
    if (!valid) {
        fprintf(stderr,
                "%s:%d:3: error: invalid reader-syntax rule '%s'\n"
                "  expected: OP_ TARGET\n"
                "            _OP TARGET\n"
                "            _OP_ TARGET BINDING-POWER (left|right)\n"
                "            BINDER_DELIMITER_ TARGET BINDING-POWER\n",
                filename ? filename : "<input>", line_number, original);
        free(surface); free(delimiter); free(original); free(line);
        return -1;
    }
    free(original);
    RSRule candidate = {0};
    candidate.kind = parsed_kind;
    candidate.token = surface;
    candidate.target = strdup(target);
    candidate.delimiter = delimiter;
    candidate.precedence = precedence;
    candidate.right_associative = strcmp(assoc, "right") == 0;
    candidate.source_file = strdup(filename ? filename : "<input>");
    candidate.source_line = line_number;
    if (!candidate.token || !candidate.target || !candidate.source_file) {
        free(candidate.token); free(candidate.target); free(candidate.delimiter); free(candidate.source_file);
        free(line);
        return 0;
    }
    free(line);

    for (size_t i = 0; i < reader->rule_count; i++) {
        RSRule *prior = &reader->rules[i];
        if (prior->kind != candidate.kind || strcmp(prior->token, candidate.token) != 0)
            continue;
        if (strcmp(prior->target, candidate.target) == 0 &&
            optional_text_equal(prior->delimiter, candidate.delimiter) &&
            prior->precedence == candidate.precedence &&
            prior->right_associative == candidate.right_associative)
            { free(candidate.token); free(candidate.target); free(candidate.delimiter); free(candidate.source_file); return 1; }
        fprintf(stderr,
                "%s:%d:3: error: conflicting %s rule '%s' for reader %s\n"
                "  previous declaration: %s:%d\n",
                candidate.source_file, candidate.source_line,
                rule_kind_name(candidate.kind), candidate.token, reader->type,
                prior->source_file, prior->source_line);
        free(candidate.token); free(candidate.target); free(candidate.delimiter); free(candidate.source_file);
        return -1;
    }

    if (reader->rule_count == reader->rule_capacity) {
        size_t next_capacity = reader->rule_capacity ? reader->rule_capacity * 2 : 8;
        RSRule *next = realloc(reader->rules, next_capacity * sizeof(*next));
        if (!next) {
            free(candidate.token); free(candidate.target); free(candidate.delimiter); free(candidate.source_file);
            return 0;
        }
        reader->rules = next;
        reader->rule_capacity = next_capacity;
    }
    RSRule *rule = &reader->rules[reader->rule_count];
    *rule = candidate;
    reader->rule_count++;
    if (reader_debug())
        fprintf(stderr, "[reader-syntax] rule %s %s -> %s\n",
                rule_kind_name(rule->kind), rule->token, rule->target);
    return 1;
}

static char *scan_declarations(const char *source, const char *filename) {
    char *out = strdup(source);
    char *line = out;
    int line_number = 1;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        char type[64];
        if (sscanf(line, "reader-syntax %63s", type) == 1 && line_indent(line, end) == 0) {
            RSReader *reader = ensure_reader(type, filename);
            if (reader_debug()) fprintf(stderr, "[reader-syntax] register %s\n", type);
            memset(line, ' ', (size_t)(end - line));
            char *next = *end ? end + 1 : end;
            while (*next) {
                char *next_end = strchr(next, '\n');
                if (!next_end) next_end = next + strlen(next);
                if (line_indent(next, next_end) == 0) break;
                int parsed = reader
                    ? parse_declaration_line(reader, next, next_end, filename,
                                             line_number + 1)
                    : 0;
                if (parsed < 0) { free(out); return NULL; }
                memset(next, ' ', (size_t)(next_end - next));
                next = *next_end ? next_end + 1 : next_end;
                line_number++;
            }
            line = next;
            continue;
        }
        line = *end ? end + 1 : end;
        line_number++;
    }
    return out;
}

char *reader_syntax_expand(const char *source, const char *filename) {
    if (!g_cleanup_registered) {
        atexit(reader_syntax_clear);
        g_cleanup_registered = 1;
    }
    char *out = scan_declarations(source, filename);
    if (!out) return NULL;
    char *line = out;
    int line_number = 1;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        char name[128], type[64], extra[2];
        char *header = slice_dup(line, end);
        int fields = sscanf(header, " define %127s :: %63s %1s", name, type, extra);
        free(header);
        RSReader *reader = fields == 2 ? find_reader(type) : NULL;
        if (reader_debug() && fields >= 2)
            fprintf(stderr, "[reader-syntax] expected %s reader=%s\n",
                    type, reader ? "yes" : "no");
        if (reader && *end) {
            char *body = end + 1;
            char *body_end = strchr(body, '\n');
            if (!body_end) body_end = body + strlen(body);
            int header_indent = line_indent(line, end);
            int body_indent = line_indent(body, body_end);
            if (body_indent > header_indent) {
                if (canonical_target_call(reader, body + body_indent, body_end)) {
                    const char *expression = body + body_indent;
                    size_t expression_length = (size_t)(body_end - expression);
                    size_t old_len = strlen(out);
                    size_t before = (size_t)(line - out);
                    size_t after = old_len - (size_t)(body_end - out);
                    size_t header_len = (size_t)(end - line);
                    size_t replacement = header_len + 1 + expression_length + 1 +
                                         (size_t)body_indent;
                    char *next_out = malloc(before + replacement + after + 1);
                    memcpy(next_out, out, before);
                    memcpy(next_out + before, line, header_len);
                    next_out[before + header_len] = ' ';
                    memcpy(next_out + before + header_len + 1,
                           expression, expression_length);
                    next_out[before + header_len + 1 + expression_length] = '\n';
                    memset(next_out + before + header_len + 2 + expression_length,
                           ' ', (size_t)body_indent);
                    memcpy(next_out + before + replacement, body_end, after + 1);
                    free(out);
                    out = next_out;
                    line = out + before + replacement;
                    end = line;
                    goto next_line;
                }
                RSParser parser = {0};
                parser.cursor = body + body_indent;
                parser.end = body_end;
                parser.reader = reader;
                parser.filename = filename;
                parser.line = line_number + 1;
                char *expanded = parse_expression(&parser, 0);
                skip_space(&parser);
                if (!expanded || parser.cursor != parser.end) {
                    if (reader_debug() && expanded)
                        fprintf(stderr, "[reader-syntax] trailing: '%.*s'\n",
                                (int)(parser.end - parser.cursor), parser.cursor);
                    fprintf(stderr, "%s:%d:%d: error: reader-syntax %s: %s\n",
                            filename ? filename : "<input>", line_number + 1,
                            body_indent + 1, type,
                            parser.error[0] ? parser.error : "unexpected trailing input");
                    free(expanded);
                    free(out);
                    return NULL;
                }
                size_t old_len = strlen(out);
                size_t before = (size_t)(line - out);
                size_t after = old_len - (size_t)(body_end - out);
                size_t header_len = (size_t)(end - line);
                size_t replacement = header_len + 1 + strlen(expanded) + 1 +
                                     (size_t)body_indent;
                char *next_out = malloc(before + replacement + after + 1);
                memcpy(next_out, out, before);
                memcpy(next_out + before, line, header_len);
                next_out[before + header_len] = ' ';
                memcpy(next_out + before + header_len + 1, expanded, strlen(expanded));
                next_out[before + header_len + 1 + strlen(expanded)] = '\n';
                memset(next_out + before + header_len + 2 + strlen(expanded),
                       ' ', (size_t)body_indent);
                memcpy(next_out + before + replacement, body_end, after + 1);
                if (reader_debug())
                    fprintf(stderr, "[reader-syntax] expanded %s at %s:%d => %s\n",
                            type, filename ? filename : "<input>", line_number + 1,
                            expanded);
                free(expanded); free(out);
                out = next_out;
                line = out + before + replacement;
                end = line;
            }
        }
next_line:
        line = *end ? end + 1 : end;
        line_number++;
    }
    return out;
}

void reader_syntax_clear(void) {
    for (size_t i = 0; i < g_reader_count; i++) {
        RSReader *reader = &g_readers[i];
        for (size_t j = 0; j < reader->rule_count; j++) {
            free(reader->rules[j].token);
            free(reader->rules[j].target);
            free(reader->rules[j].delimiter);
            free(reader->rules[j].source_file);
        }
        free(reader->rules);
        free(reader->type);
        free(reader->owner_file);
    }
    free(g_readers);
    g_readers = NULL;
    g_reader_count = 0;
    g_reader_capacity = 0;
    while (g_scope) reader_syntax_scope_pop();
}
