#include "reader_syntax.h"
#include "reader.h"
#include "compat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    RS_BINDER, RS_CIRCUMFIX, RS_PREFIX, RS_POSTFIX, RS_INFIX
} RSRuleKind;

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

static MONAD_THREAD_LOCAL RSReader *g_readers;
static MONAD_THREAD_LOCAL size_t g_reader_count;
static MONAD_THREAD_LOCAL size_t g_reader_capacity;
static MONAD_THREAD_LOCAL int g_cleanup_registered;

typedef struct {
    char *open;
    char *close;
    char *target;
    char *owner_file;
    int source_line;
} RSTypeRule;

static MONAD_THREAD_LOCAL RSTypeRule *g_type_rules;
static MONAD_THREAD_LOCAL size_t g_type_rule_count;
static MONAD_THREAD_LOCAL size_t g_type_rule_capacity;

typedef struct {
    char *open;
    char *close;
    char *empty_target;
    char *elements_target;
    char *filter_target;
    char *owner_file;
    int source_line;
} RSTermRule;

static MONAD_THREAD_LOCAL RSTermRule *g_term_rules;
static MONAD_THREAD_LOCAL size_t g_term_rule_count;
static MONAD_THREAD_LOCAL size_t g_term_rule_capacity;

typedef struct {
    char *keyword;
    char *target;
    char *owner_file;
    int source_line;
} RSBlockReader;

static MONAD_THREAD_LOCAL RSBlockReader *g_block_readers;
static MONAD_THREAD_LOCAL size_t g_block_reader_count;
static MONAD_THREAD_LOCAL size_t g_block_reader_capacity;
static MONAD_THREAD_LOCAL int g_block_reader_error;

typedef struct RSScope {
    char *owner_file;
    char **allowed_files;
    size_t allowed_count;
    size_t allowed_capacity;
    struct RSScope *previous;
} RSScope;

static MONAD_THREAD_LOCAL RSScope *g_scope;

static int reader_debug(void);

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

static int register_type_syntax(const char *pattern, const char *target,
                                const char *filename, int source_line) {
    const char *hole = strchr(pattern, '_');
    if (!hole || strchr(hole + 1, '_') || hole == pattern || !hole[1]) {
        fprintf(stderr,
                "%s:%d:1: error: invalid type-syntax pattern '%s'\n"
                "  expected: OPEN_CLOSE TARGET\n",
                filename ? filename : "<input>", source_line, pattern);
        return -1;
    }
    char *open = strndup(pattern, (size_t)(hole - pattern));
    char *close = strdup(hole + 1);
    char *owner = owner_key(filename);
    if (!open || !close || !owner) {
        free(open); free(close); free(owner);
        return 0;
    }
    for (size_t i = 0; i < g_type_rule_count; i++) {
        RSTypeRule *prior = &g_type_rules[i];
        if (strcmp(prior->open, open) || strcmp(prior->close, close) ||
            strcmp(prior->owner_file, owner))
            continue;
        if (!strcmp(prior->target, target)) {
            free(open); free(close); free(owner);
            return 1;
        }
        fprintf(stderr,
                "%s:%d:1: error: conflicting type-syntax '%s'\n"
                "  previous declaration: %s:%d\n",
                filename ? filename : "<input>", source_line, pattern,
                prior->owner_file, prior->source_line);
        free(open); free(close); free(owner);
        return -1;
    }
    if (g_type_rule_count == g_type_rule_capacity) {
        size_t capacity = g_type_rule_capacity ? g_type_rule_capacity * 2 : 8;
        RSTypeRule *next = realloc(g_type_rules, capacity * sizeof(*next));
        if (!next) { free(open); free(close); free(owner); return 0; }
        g_type_rules = next;
        g_type_rule_capacity = capacity;
    }
    RSTypeRule *rule = &g_type_rules[g_type_rule_count++];
    rule->open = open;
    rule->close = close;
    rule->target = strdup(target);
    rule->owner_file = owner;
    rule->source_line = source_line;
    return rule->target != NULL;
}

char *reader_type_syntax_expand(const char *type_text) {
    if (!type_text) return NULL;
    const char *start = type_text;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = type_text + strlen(type_text);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    for (size_t i = 0; i < g_type_rule_count; i++) {
        RSTypeRule *rule = &g_type_rules[i];
        if (!owner_is_active(rule->owner_file)) continue;
        size_t open_len = strlen(rule->open);
        size_t close_len = strlen(rule->close);
        if ((size_t)(end - start) <= open_len + close_len ||
            memcmp(start, rule->open, open_len) ||
            memcmp(end - close_len, rule->close, close_len))
            continue;
        const char *inner = start + open_len;
        const char *inner_end = end - close_len;
        while (inner < inner_end && isspace((unsigned char)*inner)) inner++;
        while (inner_end > inner && isspace((unsigned char)inner_end[-1])) inner_end--;
        size_t size = strlen(rule->target) + 1 + (size_t)(inner_end - inner) + 1;
        char *expanded = malloc(size);
        if (!expanded) return NULL;
        snprintf(expanded, size, "%s %.*s", rule->target,
                 (int)(inner_end - inner), inner);
        return expanded;
    }
    return NULL;
}

int reader_type_syntax_is_active(const char *open, const char *close) {
    if (!open || !close) return 0;
    for (size_t i = 0; i < g_type_rule_count; i++) {
        RSTypeRule *rule = &g_type_rules[i];
        if (owner_is_active(rule->owner_file) &&
            !strcmp(rule->open, open) && !strcmp(rule->close, close))
            return 1;
    }
    return 0;
}

static int register_term_syntax(const char *pattern, const char *empty_target,
                                const char *elements_target,
                                const char *filter_target,
                                const char *filename, int source_line) {
    const char *hole = strchr(pattern, '_');
    if (!hole || strchr(hole + 1, '_') || hole == pattern || !hole[1])
        return -1;
    if (g_term_rule_count == g_term_rule_capacity) {
        size_t capacity = g_term_rule_capacity ? g_term_rule_capacity * 2 : 8;
        RSTermRule *next = realloc(g_term_rules, capacity * sizeof(*next));
        if (!next) return 0;
        g_term_rules = next;
        g_term_rule_capacity = capacity;
    }
    RSTermRule *rule = &g_term_rules[g_term_rule_count++];
    rule->open = strndup(pattern, (size_t)(hole - pattern));
    rule->close = strdup(hole + 1);
    rule->empty_target = strdup(empty_target);
    rule->elements_target = strdup(elements_target);
    rule->filter_target = strdup(filter_target);
    rule->owner_file = owner_key(filename);
    rule->source_line = source_line;
    return rule->open && rule->close && rule->empty_target &&
           rule->elements_target && rule->filter_target && rule->owner_file;
}

int reader_term_syntax_lookup(const char *open, const char *close,
                              const char **empty_target,
                              const char **elements_target,
                              const char **filter_target) {
    for (size_t i = 0; i < g_term_rule_count; i++) {
        RSTermRule *rule = &g_term_rules[i];
        if (!owner_is_active(rule->owner_file) || strcmp(rule->open, open) ||
            strcmp(rule->close, close))
            continue;
        if (empty_target) *empty_target = rule->empty_target;
        if (elements_target) *elements_target = rule->elements_target;
        if (filter_target) *filter_target = rule->filter_target;
        return 1;
    }
    return 0;
}

static RSBlockReader *find_block_reader(const char *keyword) {
    RSBlockReader *found = NULL;
    for (size_t i = 0; i < g_block_reader_count; i++) {
        RSBlockReader *candidate = &g_block_readers[i];
        if (strcmp(candidate->keyword, keyword) != 0 ||
            !owner_is_active(candidate->owner_file))
            continue;
        if (found && strcmp(found->owner_file, candidate->owner_file) != 0) {
            fprintf(stderr,
                    "%s:1:1: error: ambiguous reader-block '%s' imported from %s and %s\n",
                    g_scope ? g_scope->owner_file : "<input>", keyword,
                    found->owner_file, candidate->owner_file);
            g_block_reader_error = 1;
            return NULL;
        }
        found = candidate;
    }
    return found;
}

static int register_block_reader(const char *pattern, const char *target,
                                 const char *filename, int source_line) {
    size_t pattern_length = strlen(pattern);
    if (pattern_length < 2 || pattern[pattern_length - 1] != '_' ||
        strchr(pattern, '_') != pattern + pattern_length - 1) {
        fprintf(stderr,
                "%s:%d:1: error: invalid reader-block pattern '%s'\n"
                "  expected: KEYWORD_ TARGET\n",
                filename ? filename : "<input>", source_line, pattern);
        return -1;
    }
    char *keyword = strndup(pattern, pattern_length - 1);
    char *owner = owner_key(filename);
    for (size_t i = 0; i < g_block_reader_count; i++) {
        RSBlockReader *prior = &g_block_readers[i];
        if (strcmp(prior->keyword, keyword) != 0 ||
            strcmp(prior->owner_file, owner) != 0)
            continue;
        if (strcmp(prior->target, target) == 0) {
            free(keyword);
            free(owner);
            return 1;
        }
        fprintf(stderr,
                "%s:%d:1: error: conflicting reader-block '%s'\n"
                "  previous declaration: %s:%d\n",
                filename ? filename : "<input>", source_line, keyword,
                prior->owner_file, prior->source_line);
        free(keyword);
        free(owner);
        return -1;
    }
    if (g_block_reader_count == g_block_reader_capacity) {
        size_t next_capacity = g_block_reader_capacity
                             ? g_block_reader_capacity * 2 : 8;
        RSBlockReader *next = realloc(
            g_block_readers, next_capacity * sizeof(*next));
        if (!next) {
            free(keyword);
            free(owner);
            return 0;
        }
        g_block_readers = next;
        g_block_reader_capacity = next_capacity;
    }
    char *target_copy = strdup(target);
    if (!target_copy) {
        free(keyword);
        free(owner);
        return 0;
    }
    RSBlockReader *reader = &g_block_readers[g_block_reader_count++];
    reader->keyword = keyword;
    reader->target = target_copy;
    reader->owner_file = owner;
    reader->source_line = source_line;
    if (reader_debug())
        fprintf(stderr, "[reader-block] %s -> %s\n", keyword, target);
    return 1;
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

    RSRule *circumfix = match_rule(p, RS_CIRCUMFIX);
    if (circumfix) {
        p->cursor += strlen(circumfix->token);
        const char *close = strstr(p->cursor, circumfix->delimiter);
        if (!close || close > p->end) {
            snprintf(p->error, sizeof(p->error),
                     "circumfix '%s' requires closing '%s'",
                     circumfix->token, circumfix->delimiter);
            return NULL;
        }
        RSParser inner = *p;
        inner.end = close;
        char *operand = parse_expression(&inner, 0);
        skip_space(&inner);
        if (!operand || inner.cursor != inner.end) {
            free(operand);
            snprintf(p->error, sizeof(p->error), "%s",
                     inner.error[0] ? inner.error :
                     "unexpected trailing input in circumfix expression");
            return NULL;
        }
        p->cursor = close + strlen(circumfix->delimiter);
        size_t size = strlen(circumfix->target) + strlen(operand) + 4;
        char *out = malloc(size);
        snprintf(out, size, "(%s %s)", circumfix->target, operand);
        free(operand);
        return out;
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
                 r->kind == RS_INFIX || r->kind == RS_BINDER ||
                 r->kind == RS_CIRCUMFIX) &&
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
    case RS_CIRCUMFIX: return "circumfix";
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
    /* Source files checked out with CRLF retain the carriage return because
     * declaration slices end at '\n'.  Treat it as horizontal whitespace so
     * the final precedence/associativity word has identical semantics on
     * every host. */
    for (char *word = strtok_r(line, " \t\r", &save); word;
         word = strtok_r(NULL, " \t\r", &save)) {
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
        const char *value_hole = strchr(pattern, '$');
        target = words[1];

        /* '$' is the explicit expression placeholder.  In particular,
         * OPEN$CLOSE is circumfix notation; '_' remains reserved for the
         * operator-spacing roles used by prefix/postfix/infix declarations. */
        if (value_hole && !strchr(value_hole + 1, '$') &&
            !first_hole && word_count == 2 && value_hole != pattern &&
            value_hole != pattern + pattern_length - 1) {
            parsed_kind = RS_CIRCUMFIX;
            surface = strndup(pattern, (size_t)(value_hole - pattern));
            delimiter = strdup(value_hole + 1);
            valid = surface && surface[0] && delimiter && delimiter[0];
        } else if (!value_hole && first_hole && !third_hole) {
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
                "            OPEN$CLOSE TARGET\n"
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
        char type_pattern[128];
        char type_target[128];
        char type_extra[2];
        char *type_declaration = slice_dup(line, end);
        int type_fields = sscanf(type_declaration,
                                 "type-syntax %127s %127s %1s",
                                 type_pattern, type_target, type_extra);
        free(type_declaration);
        if (type_fields == 2 && line_indent(line, end) == 0) {
            int registered = register_type_syntax(type_pattern, type_target,
                                                  filename, line_number);
            if (registered <= 0) { free(out); return NULL; }
            memset(line, ' ', (size_t)(end - line));
            line = *end ? end + 1 : end;
            line_number++;
            continue;
        }
        char term_pattern[128], term_empty[128], term_elements[128];
        char term_filter[128], term_extra[2];
        char *term_declaration = slice_dup(line, end);
        int term_fields = sscanf(term_declaration,
            "term-syntax %127s %127s %127s %127s %1s",
            term_pattern, term_empty, term_elements, term_filter, term_extra);
        free(term_declaration);
        if (term_fields == 4 && line_indent(line, end) == 0) {
            int registered = register_term_syntax(
                term_pattern, term_empty, term_elements, term_filter,
                filename, line_number);
            if (registered <= 0) { free(out); return NULL; }
            memset(line, ' ', (size_t)(end - line));
            line = *end ? end + 1 : end;
            line_number++;
            continue;
        }
        char block_pattern[128];
        char block_target[128];
        char block_extra[2];
        char *block_declaration = slice_dup(line, end);
        int block_fields = sscanf(block_declaration, "reader-block %127s %127s %1s",
                                  block_pattern, block_target, block_extra);
        free(block_declaration);
        if (block_fields == 2 && line_indent(line, end) == 0) {
            int registered = register_block_reader(block_pattern, block_target,
                                                   filename, line_number);
            if (registered <= 0) { free(out); return NULL; }
            memset(line, ' ', (size_t)(end - line));
            line = *end ? end + 1 : end;
            line_number++;
            continue;
        }
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

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} RSBuffer;

static int rs_buffer_reserve(RSBuffer *buffer, size_t extra) {
    size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) return 1;
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < required) capacity *= 2;
    char *next = realloc(buffer->data, capacity);
    if (!next) return 0;
    buffer->data = next;
    buffer->capacity = capacity;
    return 1;
}

static int rs_buffer_append(RSBuffer *buffer, const char *text, size_t length) {
    if (!rs_buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int rs_buffer_text(RSBuffer *buffer, const char *text) {
    return rs_buffer_append(buffer, text, strlen(text));
}

static int rs_buffer_string_literal(RSBuffer *buffer,
                                    const char *text, size_t length) {
    if (!rs_buffer_text(buffer, "\"")) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\\' || c == '"') {
            if (!rs_buffer_text(buffer, "\\")) return 0;
        }
        if (c == '\n') {
            if (!rs_buffer_text(buffer, "\\n")) return 0;
        } else if (c == '\r') {
            if (!rs_buffer_text(buffer, "\\r")) return 0;
        } else if (!rs_buffer_append(buffer, &c, 1)) {
            return 0;
        }
    }
    return rs_buffer_text(buffer, "\"");
}

static int blank_line(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r'))
        start++;
    return start == end;
}

static RSBlockReader *block_reader_at_line(const char *start, const char *end,
                                           const char **header) {
    int indent = line_indent(start, end);
    const char *content = start + indent;
    const char *word_end = content;
    while (word_end < end && !isspace((unsigned char)*word_end)) word_end++;
    if (word_end == content) return NULL;
    char *keyword = slice_dup(content, word_end);
    RSBlockReader *reader = find_block_reader(keyword);
    free(keyword);
    if (!reader) return NULL;
    const char *rest = word_end;
    while (rest < end && (*rest == ' ' || *rest == '\t')) rest++;
    if (rest == end) return NULL;
    *header = rest;
    return reader;
}

/* Lower a claimed indentation block to a neutral Syntax-shaped call. The
 * compiler groups lines and records their relative indentation; the target
 * transformer, written in Monad, owns every domain-specific interpretation.
 *
 *   decree answer              (expand-decree answer
 *     42              =>         (reader-block (reader-line 2 42)))
 */
static char *expand_block_readers(const char *source, const char *filename) {
    RSBuffer output = {0};
    const char *line = source;
    int line_number = 1;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *header = NULL;
        RSBlockReader *reader = block_reader_at_line(line, end, &header);
        if (g_block_reader_error) {
            free(output.data);
            return NULL;
        }
        if (!reader) {
            if (!rs_buffer_append(&output, line, (size_t)(end - line)) ||
                (*end && !rs_buffer_text(&output, "\n")))
                goto allocation_failure;
            line = *end ? end + 1 : end;
            line_number++;
            continue;
        }

        int base_indent = line_indent(line, end);
        const char *scan = *end ? end + 1 : end;
        const char *block_end = scan;
        size_t body_lines = 0;
        while (*scan) {
            const char *scan_end = strchr(scan, '\n');
            if (!scan_end) scan_end = scan + strlen(scan);
            if (!blank_line(scan, scan_end) &&
                line_indent(scan, scan_end) <= base_indent)
                break;
            block_end = *scan_end ? scan_end + 1 : scan_end;
            body_lines++;
            scan = block_end;
        }
        if (!body_lines) {
            fprintf(stderr,
                    "%s:%d:1: error: reader-block '%s' requires an indented body\n",
                    filename ? filename : "<input>", line_number,
                    reader->keyword);
            free(output.data);
            return NULL;
        }

        if (!rs_buffer_append(&output, line, (size_t)base_indent) ||
            !rs_buffer_text(&output, "(") ||
            !rs_buffer_text(&output, reader->target) ||
            !rs_buffer_text(&output, " ") ||
            !rs_buffer_append(&output, header, (size_t)(end - header)) ||
            !rs_buffer_text(&output, " (reader-block"))
            goto allocation_failure;

        const char *body = *end ? end + 1 : end;
        while (body < block_end) {
            const char *body_end = strchr(body, '\n');
            if (!body_end || body_end > block_end) body_end = block_end;
            /* Keep the synthetic reader-block payload inside one explicit
             * list.  Feeding layout-significant newlines back through the
             * Wisp pass lets it close `(reader-block ...)` after the head and
             * turns every reader-line into an extra macro argument.  Source
             * coordinates already live in each reader-line node. */
            if (!rs_buffer_text(&output, " ")) goto allocation_failure;
            int indent = line_indent(body, body_end);
            int relative_indent = indent > base_indent
                                ? indent - base_indent : 0;
            const char *content = body + indent;
            char indentation[32];
            snprintf(indentation, sizeof(indentation), "%d", relative_indent);
            for (int i = 0; i < relative_indent; i++)
                if (!rs_buffer_text(&output, " ")) goto allocation_failure;
            if (blank_line(body, body_end)) {
                if (!rs_buffer_text(&output, "(reader-blank ") ||
                    !rs_buffer_text(&output, indentation) ||
                    !rs_buffer_text(&output, ")"))
                    goto allocation_failure;
            } else if ((size_t)(body_end - content) >= 2 &&
                       content[0] == ';' && content[1] == ';') {
                if (!rs_buffer_text(&output, "(reader-comment ") ||
                    !rs_buffer_text(&output, indentation) ||
                    !rs_buffer_text(&output, " ") ||
                    !rs_buffer_string_literal(&output, content,
                                              (size_t)(body_end - content)) ||
                    !rs_buffer_text(&output, ")"))
                    goto allocation_failure;
            } else if (!rs_buffer_text(&output, "(reader-line ") ||
                       !rs_buffer_text(&output, indentation) ||
                       !rs_buffer_text(&output, " ") ||
                       !rs_buffer_append(&output, content,
                                         (size_t)(body_end - content)) ||
                       !rs_buffer_text(&output, ")")) {
                goto allocation_failure;
            }
            body = body_end < block_end && *body_end ? body_end + 1 : body_end;
        }
        if (!rs_buffer_text(&output, "))\n")) goto allocation_failure;
        line = block_end;
        line_number += (int)body_lines + 1;
    }
    if (!output.data) return strdup("");
    return output.data;

allocation_failure:
    free(output.data);
    return NULL;
}

static char *normalize_source_newlines(const char *source) {
    size_t length = strlen(source);
    char *normalized = malloc(length + 1);
    if (!normalized) return NULL;
    size_t write = 0;
    for (size_t read = 0; read < length; read++) {
        if (source[read] == '\r' && read + 1 < length &&
            source[read + 1] == '\n')
            continue;
        normalized[write++] = source[read];
    }
    normalized[write] = '\0';
    return normalized;
}

char *reader_syntax_expand(const char *source, const char *filename) {
    if (!g_cleanup_registered) {
        atexit(reader_syntax_clear);
        g_cleanup_registered = 1;
    }
    /* The reader owns the first source-to-source pass, so normalize CRLF once
     * at its boundary.  This keeps declaration tokens, expression extents,
     * and the Wisp source returned to later compiler stages byte-consistent. */
    char *normalized = normalize_source_newlines(source);
    if (!normalized) return NULL;
    char *out = scan_declarations(normalized, filename);
    free(normalized);
    if (!out) return NULL;
    char *block_expanded = expand_block_readers(out, filename);
    free(out);
    out = block_expanded;
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
            char *body_content_end = body_end;
            if (body_content_end > body && body_content_end[-1] == '\r')
                body_content_end--;
            int header_indent = line_indent(line, end);
            int body_indent = line_indent(body, body_content_end);
            if (body_indent > header_indent) {
                if (canonical_target_call(reader, body + body_indent,
                                          body_content_end)) {
                    const char *expression = body + body_indent;
                    size_t expression_length =
                        (size_t)(body_content_end - expression);
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
                parser.end = body_content_end;
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
    for (size_t i = 0; i < g_type_rule_count; i++) {
        free(g_type_rules[i].open);
        free(g_type_rules[i].close);
        free(g_type_rules[i].target);
        free(g_type_rules[i].owner_file);
    }
    free(g_type_rules);
    g_type_rules = NULL;
    g_type_rule_count = 0;
    g_type_rule_capacity = 0;
    for (size_t i = 0; i < g_term_rule_count; i++) {
        free(g_term_rules[i].open);
        free(g_term_rules[i].close);
        free(g_term_rules[i].empty_target);
        free(g_term_rules[i].elements_target);
        free(g_term_rules[i].filter_target);
        free(g_term_rules[i].owner_file);
    }
    free(g_term_rules);
    g_term_rules = NULL;
    g_term_rule_count = 0;
    g_term_rule_capacity = 0;
    for (size_t i = 0; i < g_block_reader_count; i++) {
        free(g_block_readers[i].keyword);
        free(g_block_readers[i].target);
        free(g_block_readers[i].owner_file);
    }
    free(g_block_readers);
    g_block_readers = NULL;
    g_block_reader_count = 0;
    g_block_reader_capacity = 0;
    g_block_reader_error = 0;
    while (g_scope) reader_syntax_scope_pop();
}

static int reader_owner_is_core(const char *owner, const char *core_prefix)
{
    if (!owner || !core_prefix) return 0;
    size_t n = strlen(core_prefix);
    while (n > 0 && (core_prefix[n - 1] == '/' ||
                     core_prefix[n - 1] == '\\')) n--;
    if (n == 0 || strncmp(owner, core_prefix, n) != 0) return 0;
    return owner[n] == '\0' || owner[n] == '/' || owner[n] == '\\';
}

void reader_syntax_clear_noncore(const char *core_dir)
{
    char *absolute = core_dir ? reader_syntax_absolute_path(core_dir) : NULL;
    const char *prefix = absolute ? absolute : core_dir;
    if (!prefix || !*prefix) {
        reader_syntax_clear();
        free(absolute);
        return;
    }

    size_t reader_kept = 0;
    for (size_t i = 0, out = 0; i < g_reader_count; i++) {
        RSReader *reader = &g_readers[i];
        if (reader_owner_is_core(reader->owner_file, prefix)) {
            if (out != i) g_readers[out] = g_readers[i];
            out++;
            reader_kept++;
            continue;
        }
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
    /* Compacting structs with owned pointers is safe after the pass above;
     * retained entries were copied byte-for-byte and now occupy the prefix. */
    g_reader_count = reader_kept;
    if (g_reader_count == 0) {
        free(g_readers); g_readers = NULL; g_reader_capacity = 0;
    } else if (g_reader_capacity > g_reader_count * 2) {
        RSReader *shrunk = realloc(g_readers,
                                   g_reader_count * sizeof(*g_readers));
        if (shrunk) {
            g_readers = shrunk;
            g_reader_capacity = g_reader_count;
        }
    }

    for (size_t i = 0, out = 0; i < g_type_rule_count; i++) {
        RSTypeRule *rule = &g_type_rules[i];
        if (reader_owner_is_core(rule->owner_file, prefix)) {
            if (out != i) g_type_rules[out] = g_type_rules[i];
            out++;
            continue;
        }
        free(rule->open); free(rule->close); free(rule->target);
        free(rule->owner_file);
    }
    /* The loop's output index is intentionally recomputed to avoid exposing
     * the private temporary variable outside its scope. */
    {
        size_t kept = 0;
        while (kept < g_type_rule_count &&
               reader_owner_is_core(g_type_rules[kept].owner_file, prefix))
            kept++;
        g_type_rule_count = kept;
        if (g_type_rule_count == 0) {
            free(g_type_rules); g_type_rules = NULL; g_type_rule_capacity = 0;
        } else if (g_type_rule_capacity > g_type_rule_count * 2) {
            RSTypeRule *shrunk = realloc(g_type_rules,
                                         g_type_rule_count * sizeof(*g_type_rules));
            if (shrunk) {
                g_type_rules = shrunk;
                g_type_rule_capacity = g_type_rule_count;
            }
        }
    }

    for (size_t i = 0, out = 0; i < g_term_rule_count; i++) {
        RSTermRule *rule = &g_term_rules[i];
        if (reader_owner_is_core(rule->owner_file, prefix)) {
            if (out != i) g_term_rules[out] = g_term_rules[i];
            out++;
            continue;
        }
        free(rule->open); free(rule->close); free(rule->empty_target);
        free(rule->elements_target); free(rule->filter_target);
        free(rule->owner_file);
    }
    {
        size_t kept = 0;
        while (kept < g_term_rule_count &&
               reader_owner_is_core(g_term_rules[kept].owner_file, prefix))
            kept++;
        g_term_rule_count = kept;
        if (g_term_rule_count == 0) {
            free(g_term_rules); g_term_rules = NULL; g_term_rule_capacity = 0;
        } else if (g_term_rule_capacity > g_term_rule_count * 2) {
            RSTermRule *shrunk = realloc(g_term_rules,
                                         g_term_rule_count * sizeof(*g_term_rules));
            if (shrunk) {
                g_term_rules = shrunk;
                g_term_rule_capacity = g_term_rule_count;
            }
        }
    }

    for (size_t i = 0, out = 0; i < g_block_reader_count; i++) {
        RSBlockReader *reader = &g_block_readers[i];
        if (reader_owner_is_core(reader->owner_file, prefix)) {
            if (out != i) g_block_readers[out] = g_block_readers[i];
            out++;
            continue;
        }
        free(reader->keyword); free(reader->target); free(reader->owner_file);
    }
    {
        size_t kept = 0;
        while (kept < g_block_reader_count &&
               reader_owner_is_core(g_block_readers[kept].owner_file, prefix))
            kept++;
        g_block_reader_count = kept;
        if (g_block_reader_count == 0) {
            free(g_block_readers); g_block_readers = NULL;
            g_block_reader_capacity = 0;
        } else if (g_block_reader_capacity > g_block_reader_count * 2) {
            RSBlockReader *shrunk = realloc(
                g_block_readers, g_block_reader_count * sizeof(*g_block_readers));
            if (shrunk) {
                g_block_readers = shrunk;
                g_block_reader_capacity = g_block_reader_count;
            }
        }
    }
    g_block_reader_error = 0;
    while (g_scope) reader_syntax_scope_pop();
    free(absolute);
}
