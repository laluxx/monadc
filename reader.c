#include "reader.h"
#include "features.h"
#include "pmatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* Replace all exit(1) calls in the reader/parser.
 * If a recovery point is set (we're inside repl_eval_line), longjmp back.
 * Otherwise fall back to real exit so the standalone compiler still works. */

jmp_buf  g_reader_escape;
bool     g_reader_escape_set = false;
char     g_reader_error_msg[512];

int g_quote_depth       = 0;
int g_srcmap_line_bias  = 0;
int g_srcmap_col_bias   = 0;
int g_srcmap_abs_line   = 0;

int (*g_param_kind_is_func)(const char *func_name, int arg_index) = NULL;
int (*g_is_known_function)(const char *name) = NULL;

#define READER_ERROR(line, col, fmt, ...) \
    do { \
        snprintf(g_reader_error_msg, sizeof(g_reader_error_msg), \
                 "%s:%d:%d: error: " fmt, \
                 current_filename ? current_filename : "<input>", \
                 (line), (col), ##__VA_ARGS__); \
        fprintf(stderr, "%s\n", g_reader_error_msg); \
        if (g_reader_escape_set) { \
            g_reader_escape_set = false; \
            longjmp(g_reader_escape, 1); \
        } \
        exit(1); \
    } while(0)


/// Multiline -| comments |-

CommentSpan *g_comment_spans = NULL;
int          g_comment_count = 0;
int          g_comment_cap   = 0;

static void comment_map_free(void) {
    free(g_comment_spans);
    g_comment_spans = NULL;
    g_comment_count = 0;
    g_comment_cap   = 0;
}

static void comment_map_add(int open_pos, int close_pos, int para_end) {
    if (g_comment_count >= g_comment_cap) {
        g_comment_cap = g_comment_cap == 0 ? 8 : g_comment_cap * 2;
        g_comment_spans = realloc(g_comment_spans,
                                  sizeof(CommentSpan) * g_comment_cap);
    }
    g_comment_spans[g_comment_count].open_pos  = open_pos;
    g_comment_spans[g_comment_count].close_pos = close_pos;
    g_comment_spans[g_comment_count].para_end  = para_end;
    g_comment_count++;
}

/* Pre-scan the entire source once, building the comment map.
 * For each -| found:
 *   - scan forward for |-
 *   - if found: block comment, record open/close positions
 *   - if EOF first: paragraph comment, end = first blank line after open
 */
void comment_map_build(const char *source) {
    comment_map_free();
    int len = (int)strlen(source);
    int i   = 0;
    while (i < len - 1) {
        /* Skip ; line comments so we don't find -| inside them */
        if (source[i] == ';') {
            while (i < len && source[i] != '\n') i++;
            continue;
        }
        /* Skip string literals */
        if (source[i] == '"') {
            i++;
            while (i < len && source[i] != '"') {
                if (source[i] == '\\') i++;
                i++;
            }
            if (i < len) i++; /* skip closing " */
            continue;
        }
        /* Detect -| */
        if (source[i] == '-' && source[i+1] == '|') {
            int open_pos = i;
            i += 2; /* skip past -| */

            /* Scan forward for |-
             * If we hit another -| before finding |-, the original -| is a
             * paragraph comment. Reset i to the new -| so the outer loop
             * processes it next.                                             */
            int close_pos = -1;
            int saved_i = i;
            while (i < len - 1) {
                if (source[i] == '|' && source[i+1] == '-') {
                    close_pos = i;
                    i += 2;
                    break;
                }
                if (source[i] == '-' && source[i+1] == '|') {
                    /* found another -| before |- : original is paragraph comment */
                    /* leave i here so outer loop picks up this new -| next */
                    break;
                }
                i++;
            }

            if (close_pos >= 0) {
                /* Block comment — open_pos to close_pos+2 */
                comment_map_add(open_pos, close_pos, -1);
            } else {
                /* No |- found — paragraph comment.
                 * End = position after first blank line following open_pos.
                 * A blank line = \n\n or \n followed by whitespace-only line. */
                int para_end = len; /* default: to EOF */
                int j = open_pos + 2;
                while (j < len) {
                    if (source[j] == '\n') {
                        /* Check if next line is blank (empty or whitespace only) */
                        int k = j + 1;
                        while (k < len && source[k] == ' ' ||
                               k < len && source[k] == '\t') k++;
                        if (k >= len || source[k] == '\n') {
                            /* blank line found — paragraph ends here */
                            para_end = j;
                            break;
                        }
                    }
                    j++;
                }
                comment_map_add(open_pos, -1, para_end);
            }
            continue;
        }
        i++;
    }
}

/* Look up position in comment map — returns the CommentSpan if pos
 * is at an open_pos, NULL otherwise. */
static CommentSpan *comment_map_lookup(int pos) {
    for (int i = 0; i < g_comment_count; i++)
        if (g_comment_spans[i].open_pos == pos)
            return &g_comment_spans[i];
    return NULL;
}

/// Error reporting context

static const char *current_filename = NULL;
static const char *current_source = NULL;

void parser_set_context(const char *filename, const char *source) {
    current_filename   = filename;
    current_source     = source;
    g_srcmap_line_bias = 0;
    g_srcmap_col_bias  = 0;
    g_srcmap_abs_line  = 0;
    if (source) comment_map_build(source);
}

const char *parser_get_filename(void) {
    return current_filename ? current_filename : "<input>";
}

static void compiler_error_range(int line, int column, int end_column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    fprintf(stderr, "%s:%d:%d: error: ", current_filename ? current_filename : "<input>", line, column);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");

    if (current_source) {
        const char *line_start = current_source;
        int current_line = 1;

        while (current_line < line && *line_start) {
            if (*line_start == '\n') current_line++;
            line_start++;
        }

        const char *line_end = line_start;
        while (*line_end && *line_end != '\n') line_end++;

        fprintf(stderr, "%5d | %.*s\n", line, (int)(line_end - line_start), line_start);

        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) {
            fprintf(stderr, " ");
        }

        if (end_column > column) {
            for (int i = column; i < end_column; i++) {
                if (i == column) {
                    fprintf(stderr, "^");
                } else {
                    fprintf(stderr, "~");
                }
            }
        } else {
            fprintf(stderr, "^");
        }
        fprintf(stderr, "\n");
    }

    va_end(args);
    exit(1);
}

static void compiler_error(int line, int column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_reader_error_msg, sizeof(g_reader_error_msg), fmt, args);
    va_end(args);

    /* print source context */
    fprintf(stderr, "%s:%d:%d: error: %s\n",
            current_filename ? current_filename : "<input>",
            line, column, g_reader_error_msg);
    if (current_source) {
        const char *ls = current_source;
        int cl = 1;
        while (cl < line && *ls) { if (*ls=='\n') cl++; ls++; }
        const char *le = ls;
        while (*le && *le != '\n') le++;
        fprintf(stderr, "%5d | %.*s\n", line, (int)(le-ls), ls);
        fprintf(stderr, "      | ");
        for (int i = 1; i < column; i++) fprintf(stderr, " ");
        fprintf(stderr, "^\n");
    }

    if (g_reader_escape_set) {
        g_reader_escape_set = false;
        longjmp(g_reader_escape, 1);
    }
    exit(1);
}

/// Helpers

static char *my_strndup(const char *s, size_t n) {
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *my_strdup(const char *s) {
    return my_strndup(s, strlen(s));
}

/// AST constructors

AST *ast_new_number(double value, const char *literal) {
    AST *a = calloc(1, sizeof(AST));
    a->type        = AST_NUMBER;
    a->number      = value;
    a->literal_str = literal ? my_strdup(literal) : NULL;
    return a;
}

AST *ast_new_symbol(const char *name) {
    AST *a = calloc(1, sizeof(AST));
    a->type   = AST_SYMBOL;
    a->symbol = my_strdup(name);
    return a;
}

AST *ast_new_string(const char *value) {
    AST *a = calloc(1, sizeof(AST));
    a->type   = AST_STRING;
    a->string = my_strdup(value);
    return a;
}

AST *ast_new_char(char value) {
    AST *a = calloc(1, sizeof(AST));
    a->type      = AST_CHAR;
    a->character = value;
    return a;
}

AST *ast_new_list(void) {
    AST *a = calloc(1, sizeof(AST));
    a->type           = AST_LIST;
    a->list.capacity  = 4;
    a->list.items     = malloc(sizeof(AST *) * 4);
    return a;
}

AST *ast_new_lambda(ASTParam *params, int param_count,
                    const char *return_type,
                    const char *docstring,
                    const char *alias_name,
                    bool naked,
                    AST *body,
                    AST **body_exprs,
                    int body_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type                = AST_LAMBDA;
    a->lambda.params       = params;
    a->lambda.param_count  = param_count;
    a->lambda.return_type  = return_type ? my_strdup(return_type) : NULL;
    a->lambda.docstring    = docstring   ? my_strdup(docstring)   : NULL;
    a->lambda.alias_name   = alias_name  ? my_strdup(alias_name)  : NULL;
    a->lambda.naked        = naked;
    a->lambda.body         = body;        // points to body_exprs[body_count-1]
    a->lambda.body_exprs   = body_exprs;  // owned array
    a->lambda.body_count   = body_count;
    return a;
}

AST *ast_new_asm(AST **instructions, size_t instruction_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_ASM;
    a->asm_block.instructions = instructions;
    a->asm_block.instruction_count = instruction_count;
    return a;
}

AST *ast_new_keyword(const char *name) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_KEYWORD;
    a->keyword = my_strdup(name);
    return a;
}

AST *ast_new_ratio(long long numerator, long long denominator) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_RATIO;
    a->ratio.numerator = numerator;
    a->ratio.denominator = denominator;
    return a;
}

AST *ast_new_array(void) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_ARRAY;
    a->array.element_capacity = 4;
    a->array.elements = malloc(sizeof(AST *) * 4);
    a->array.element_count = 0;
    return a;
}

AST *ast_new_refinement(const char *name, const char *var,
                        const char *base_type, AST *predicate,
                        const char *docstring, const char *alias_name) {
    AST *a = calloc(1, sizeof(AST));
    a->type                    = AST_REFINEMENT;
    a->refinement.name         = name      ? my_strdup(name)      : NULL;
    a->refinement.var          = var       ? my_strdup(var)        : NULL;
    a->refinement.base_type    = base_type ? my_strdup(base_type)  : NULL;
    a->refinement.predicate    = predicate;
    a->refinement.docstring    = docstring   ? my_strdup(docstring)   : NULL;
    a->refinement.alias_name   = alias_name  ? my_strdup(alias_name)  : NULL;
    return a;
}

AST *ast_new_address_of(AST *operand) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_ADDRESS_OF;
    a->list.items = malloc(sizeof(AST*) * 1);
    a->list.items[0] = operand;
    a->list.count = 1;
    a->list.capacity = 1;
    return a;
}

AST *ast_new_range(AST *start, AST *step, AST *end, bool is_array) {
    AST *a = calloc(1, sizeof(AST));
    a->type        = AST_RANGE;
    a->range.start = start;
    a->range.step  = step;
    a->range.end   = end;
    a->range.is_array = is_array;
    return a;
}

AST *ast_new_layout(const char *name,
                    ASTLayoutField *fields, int field_count,
                    bool packed, int align) {
    AST *a = calloc(1, sizeof(AST));
    a->type                = AST_LAYOUT;
    a->layout.name         = my_strdup(name);
    a->layout.fields       = fields;
    a->layout.field_count  = field_count;
    a->layout.packed       = packed;
    a->layout.align        = align;
    return a;
}

AST *ast_new_set(void) {
    AST *a = calloc(1, sizeof(AST));
    a->type              = AST_SET;
    a->set.element_capacity = 4;
    a->set.elements      = malloc(sizeof(AST*) * 4);
    a->set.element_count = 0;
    return a;
}

AST *ast_new_map(void) {
    AST *a = calloc(1, sizeof(AST));
    a->type         = AST_MAP;
    a->map.capacity = 4;
    a->map.keys     = malloc(sizeof(AST*) * 4);
    a->map.vals     = malloc(sizeof(AST*) * 4);
    a->map.count    = 0;
    return a;
}

void ast_pattern_free(ASTPattern *p) {
    if (!p) return;
    free(p->var_name);
    if (p->elements) {
        for (int i = 0; i < p->element_count; i++)
            ast_pattern_free(&p->elements[i]);
        free(p->elements);
    }
    if (p->tail) {
        ast_pattern_free(p->tail);
        free(p->tail);
    }
}

AST *ast_new_data(const char *name,
                  ASTDataConstructor *constructors, int constructor_count,
                  char **deriving, int deriving_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type                     = AST_DATA;
    a->data.name                = my_strdup(name);
    a->data.constructors        = constructors;
    a->data.constructor_count   = constructor_count;
    a->data.deriving            = deriving;
    a->data.deriving_count      = deriving_count;
    return a;
}

AST *ast_new_class(const char *name, const char *type_var,
                   char **method_names, char **method_types, int method_count,
                   char **default_names, AST **default_bodies, int default_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type                          = AST_CLASS;
    a->class_decl.name               = my_strdup(name);
    a->class_decl.type_var           = my_strdup(type_var);
    a->class_decl.method_names       = method_names;
    a->class_decl.method_types       = method_types;
    a->class_decl.method_count       = method_count;
    a->class_decl.default_names      = default_names;
    a->class_decl.default_bodies     = default_bodies;
    a->class_decl.default_count      = default_count;
    return a;
}

AST *ast_new_instance(const char *class_name, const char *type_name,
                      char **method_names, AST **method_bodies, int method_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type                            = AST_INSTANCE;
    a->instance_decl.class_name        = my_strdup(class_name);
    a->instance_decl.type_name         = my_strdup(type_name);
    a->instance_decl.method_names      = method_names;
    a->instance_decl.method_bodies     = method_bodies;
    a->instance_decl.method_count      = method_count;
    return a;
}

AST *ast_new_pmatch(ASTPMatchClause *clauses, int clause_count) {
    AST *a = calloc(1, sizeof(AST));
    a->type                = AST_PMATCH;
    a->pmatch.clauses      = clauses;
    a->pmatch.clause_count = clause_count;
    return a;
}

void ast_array_append(AST *array, AST *item) {
    if (array->array.element_count >= array->array.element_capacity) {
        array->array.element_capacity *= 2;
        array->array.elements = realloc(array->array.elements,
                                       sizeof(AST *) * array->array.element_capacity);
    }
    array->array.elements[array->array.element_count++] = item;
}

void ast_list_append(AST *list, AST *item) {
    if (list->list.count >= list->list.capacity) {
        list->list.capacity *= 2;
        list->list.items = realloc(list->list.items,
                                   sizeof(AST *) * list->list.capacity);
    }
    list->list.items[list->list.count++] = item;
}

AST *ast_clone(AST *ast) {
    if (!ast) return NULL;
    AST *c = calloc(1, sizeof(AST));
    *c = *ast;  /* shallow copy all fields */
    c->literal_str = ast->literal_str ? strdup(ast->literal_str) : NULL;

    switch (ast->type) {
    case AST_SYMBOL:
        c->symbol = ast->symbol ? strdup(ast->symbol) : NULL;
        break;
    case AST_STRING:
        c->string = ast->string ? strdup(ast->string) : NULL;
        break;
    case AST_KEYWORD:
        c->keyword = ast->keyword ? strdup(ast->keyword) : NULL;
        break;
    case AST_LIST: {
        c->list.items    = malloc(sizeof(AST*) * (ast->list.capacity ? ast->list.capacity : 1));
        c->list.count    = ast->list.count;
        c->list.capacity = ast->list.capacity;
        for (size_t i = 0; i < ast->list.count; i++)
            c->list.items[i] = ast_clone(ast->list.items[i]);
        break;
    }
    case AST_LAMBDA: {
        c->lambda.params = malloc(sizeof(ASTParam) * (ast->lambda.param_count ? ast->lambda.param_count : 1));
        for (int i = 0; i < ast->lambda.param_count; i++) {
            c->lambda.params[i].name      = ast->lambda.params[i].name
                                          ? strdup(ast->lambda.params[i].name) : NULL;
            c->lambda.params[i].type_name = ast->lambda.params[i].type_name
                                          ? strdup(ast->lambda.params[i].type_name) : NULL;
            c->lambda.params[i].is_rest   = ast->lambda.params[i].is_rest;
        }
        c->lambda.return_type = ast->lambda.return_type ? strdup(ast->lambda.return_type) : NULL;
        c->lambda.docstring   = ast->lambda.docstring   ? strdup(ast->lambda.docstring)   : NULL;
        c->lambda.alias_name  = ast->lambda.alias_name  ? strdup(ast->lambda.alias_name)  : NULL;
        c->lambda.body        = ast_clone(ast->lambda.body);
        c->lambda.body_exprs  = malloc(sizeof(AST*) * (ast->lambda.body_count ? ast->lambda.body_count : 1));
        for (int i = 0; i < ast->lambda.body_count; i++)
            c->lambda.body_exprs[i] = ast_clone(ast->lambda.body_exprs[i]);
        break;
    }
    case AST_ARRAY: {
        c->array.elements        = malloc(sizeof(AST*) * (ast->array.element_capacity ? ast->array.element_capacity : 1));
        c->array.element_count    = ast->array.element_count;
        c->array.element_capacity = ast->array.element_capacity;
        for (size_t i = 0; i < ast->array.element_count; i++)
            c->array.elements[i] = ast_clone(ast->array.elements[i]);
        break;
    }
    case AST_SET: {
        c->set.elements        = malloc(sizeof(AST*) * (ast->set.element_capacity ? ast->set.element_capacity : 1));
        c->set.element_count    = ast->set.element_count;
        c->set.element_capacity = ast->set.element_capacity;
        for (size_t i = 0; i < ast->set.element_count; i++)
            c->set.elements[i] = ast_clone(ast->set.elements[i]);
        break;
    }
    case AST_MAP: {
        c->map.keys     = malloc(sizeof(AST*) * (ast->map.capacity ? ast->map.capacity : 1));
        c->map.vals     = malloc(sizeof(AST*) * (ast->map.capacity ? ast->map.capacity : 1));
        c->map.count    = ast->map.count;
        c->map.capacity = ast->map.capacity;
        for (size_t i = 0; i < ast->map.count; i++) {
            c->map.keys[i] = ast_clone(ast->map.keys[i]);
            c->map.vals[i] = ast_clone(ast->map.vals[i]);
        }
        break;
    }
    case AST_RANGE:
        c->range.start = ast_clone(ast->range.start);
        c->range.step  = ast_clone(ast->range.step);
        c->range.end   = ast_clone(ast->range.end);
        break;

    case AST_REFINEMENT:
        c->refinement.name       = ast->refinement.name       ? strdup(ast->refinement.name)       : NULL;
        c->refinement.var        = ast->refinement.var        ? strdup(ast->refinement.var)        : NULL;
        c->refinement.base_type  = ast->refinement.base_type  ? strdup(ast->refinement.base_type)  : NULL;
        c->refinement.predicate  = ast_clone(ast->refinement.predicate);
        c->refinement.docstring  = ast->refinement.docstring  ? strdup(ast->refinement.docstring)  : NULL;
        c->refinement.alias_name = ast->refinement.alias_name ? strdup(ast->refinement.alias_name) : NULL;
        break;

    default:
        break;
    }

    return c;
}

void ast_free(AST *ast) {
    if (!ast) return;
    switch (ast->type) {
    case AST_SYMBOL:
        free(ast->symbol);
        break;

    case AST_STRING:
        free(ast->string);
        break;

    case AST_LIST:
        for (size_t i = 0; i < ast->list.count; i++)
            ast_free(ast->list.items[i]);
        free(ast->list.items);
        break;

    case AST_RATIO:
        // No dynamic memory to free
        break;

    case AST_ARRAY:
        for (size_t i = 0; i < ast->array.element_count; i++)
            ast_free(ast->array.elements[i]);
        free(ast->array.elements);
        break;

    case AST_LAMBDA:
    if (ast->lambda.params) {
        for (int i = 0; i < ast->lambda.param_count; i++) {
            free(ast->lambda.params[i].name);
            free(ast->lambda.params[i].type_name);
        }
        free(ast->lambda.params);
    }
    free(ast->lambda.return_type);
    free(ast->lambda.docstring);
    free(ast->lambda.alias_name);
    // Free each body expression individually
    if (ast->lambda.body_exprs) {
        for (int i = 0; i < ast->lambda.body_count; i++)
            ast_free(ast->lambda.body_exprs[i]);
        free(ast->lambda.body_exprs);
    }
    // body points into body_exprs so do NOT free it separately
    break;

    case AST_ASM:
        if (ast->asm_block.instructions) {
            for (size_t i = 0; i < ast->asm_block.instruction_count; i++) {
                ast_free(ast->asm_block.instructions[i]);
            }
            free(ast->asm_block.instructions);
        }
        break;

    case AST_KEYWORD:
        free(ast->keyword);
        break;

    case AST_REFINEMENT:
        free(ast->refinement.name);
        free(ast->refinement.var);
        free(ast->refinement.base_type);
        ast_free(ast->refinement.predicate);
        free(ast->refinement.docstring);
        free(ast->refinement.alias_name);
        break;

    case AST_TESTS:
        for (int i = 0; i < ast->tests.count; i++)
            ast_free(ast->tests.assertions[i]);
        free(ast->tests.assertions);
        break;

    case AST_ADDRESS_OF:
        ast_free(ast->list.items[0]);
        free(ast->list.items);
        break;

    case AST_RANGE:
        ast_free(ast->range.start);
        ast_free(ast->range.step);
        ast_free(ast->range.end);
        break;

    case AST_LAYOUT:
        free(ast->layout.name);
        for (int i = 0; i < ast->layout.field_count; i++) {
            free(ast->layout.fields[i].name);
            free(ast->layout.fields[i].type_name);
            free(ast->layout.fields[i].array_elem);
        }
        free(ast->layout.fields);
        break;

    case AST_SET:
        for (size_t i = 0; i < ast->set.element_count; i++)
            ast_free(ast->set.elements[i]);
        free(ast->set.elements);
        break;

    case AST_MAP:
        for (size_t i = 0; i < ast->map.count; i++) {
            ast_free(ast->map.keys[i]);
            ast_free(ast->map.vals[i]);
        }
        free(ast->map.keys);
        free(ast->map.vals);
        break;

    case AST_CLASS:
        free(ast->class_decl.name);
        free(ast->class_decl.type_var);
        for (int i = 0; i < ast->class_decl.method_count; i++) {
            free(ast->class_decl.method_names[i]);
            free(ast->class_decl.method_types[i]);
        }
        free(ast->class_decl.method_names);
        free(ast->class_decl.method_types);
        for (int i = 0; i < ast->class_decl.default_count; i++) {
            free(ast->class_decl.default_names[i]);
            ast_free(ast->class_decl.default_bodies[i]);
        }
        free(ast->class_decl.default_names);
        free(ast->class_decl.default_bodies);
        break;

    case AST_INSTANCE:
        free(ast->instance_decl.class_name);
        free(ast->instance_decl.type_name);
        for (int i = 0; i < ast->instance_decl.method_count; i++) {
            free(ast->instance_decl.method_names[i]);
            ast_free(ast->instance_decl.method_bodies[i]);
        }
        free(ast->instance_decl.method_names);
        free(ast->instance_decl.method_bodies);
        break;

    case AST_DATA:
        free(ast->data.name);
        for (int i = 0; i < ast->data.constructor_count; i++) {
            free(ast->data.constructors[i].name);
            for (int j = 0; j < ast->data.constructors[i].field_count; j++)
                free(ast->data.constructors[i].field_types[j]);
            free(ast->data.constructors[i].field_types);
        }
        free(ast->data.constructors);
        for (int i = 0; i < ast->data.deriving_count; i++)
            free(ast->data.deriving[i]);
        free(ast->data.deriving);
        break;

    case AST_PMATCH:
        for (int i = 0; i < ast->pmatch.clause_count; i++) {
            ASTPMatchClause *cl = &ast->pmatch.clauses[i];
            for (int j = 0; j < cl->pattern_count; j++)
                ast_pattern_free(&cl->patterns[j]);
            free(cl->patterns);
            ast_free(cl->body);
        }
        free(ast->pmatch.clauses);
        break;

    default:
        break;
    }
    free(ast->literal_str);
    free(ast);
}


void ast_print(AST *ast) {
    if (!ast) { printf("nil"); return; }
    switch (ast->type) {
    case AST_NUMBER:
        if (ast->has_raw_int && ast->literal_str)
            printf("%s", ast->literal_str);
        else
            printf("%g", ast->number);
    break;
    case AST_SYMBOL:  printf("%s", ast->symbol); break;
    case AST_STRING:  printf("\"%s\"", ast->string); break;
    case AST_CHAR:    printf("'%c'", ast->character); break;
    case AST_LIST:
        printf("(");
        for (size_t i = 0; i < ast->list.count; i++) {
            if (i > 0) printf(" ");
            ast_print(ast->list.items[i]);
        }
        printf(")");
        break;

    case AST_RATIO:
        printf("%lld/%lld", ast->ratio.numerator, ast->ratio.denominator);
        break;

    case AST_ARRAY:
        printf("[");
        for (size_t i = 0; i < ast->array.element_count; i++) {
            if (i > 0) printf(" ");
            ast_print(ast->array.elements[i]);
        }
        printf("]");
        break;

    case AST_LAMBDA:
        printf("(lambda (");
        for (int i = 0; i < ast->lambda.param_count; i++) {
            if (i > 0) printf(" ");
            printf("[%s", ast->lambda.params[i].name);
            if (ast->lambda.params[i].type_name)
                printf(" :: %s", ast->lambda.params[i].type_name);
            printf("]");
        }
        if (ast->lambda.return_type)
            printf(" -> %s", ast->lambda.return_type);
        printf(")");
        if (ast->lambda.docstring)
            printf(" \"%s\"", ast->lambda.docstring);
        for (int i = 0; i < ast->lambda.body_count; i++) {
            printf(" ");
            ast_print(ast->lambda.body_exprs[i]);
        }
        printf(")");
        break;

    case AST_ASM:
        printf("(asm");
        for (size_t i = 0; i < ast->asm_block.instruction_count; i++) {
            printf(" ");
            ast_print(ast->asm_block.instructions[i]);
        }
        printf(")");
        break;
    case AST_KEYWORD:
        printf(":%s", ast->keyword);
        break;
    case AST_ADDRESS_OF:
        printf("&");
        ast_print(ast->list.items[0]);
        break;
    case AST_RANGE:
        printf(ast->range.is_array ? "[" : "(");
        ast_print(ast->range.start);
        if (ast->range.step) { printf(","); ast_print(ast->range.step); }
        printf("..");
        if (ast->range.end) ast_print(ast->range.end);
        printf(ast->range.is_array ? "]" : ")");
        break;
    case AST_PMATCH:
        for (int i = 0; i < ast->pmatch.clause_count; i++) {
            ASTPMatchClause *cl = &ast->pmatch.clauses[i];
            if (i > 0) printf("\n  ");
            for (int j = 0; j < cl->pattern_count; j++) {
                if (j > 0) printf(" ");
                ASTPattern *pat = &cl->patterns[j];
                switch (pat->kind) {
                case PAT_WILDCARD: printf("_"); break;
                case PAT_VAR:      printf("%s", pat->var_name); break;
                case PAT_LITERAL_INT:   printf("%lld", (long long)pat->lit_value); break;
                case PAT_LITERAL_FLOAT: printf("%g",   pat->lit_value); break;
                case PAT_LIST_EMPTY: printf("[]"); break;
                case PAT_LIST:
                    printf("[");
                    for (int k = 0; k < pat->element_count; k++) {
                        if (k > 0) printf(" ");
                        ASTPattern *ep = &pat->elements[k];
                        switch (ep->kind) {
                        case PAT_WILDCARD: printf("_"); break;
                        case PAT_VAR:      printf("%s", ep->var_name); break;
                        case PAT_LITERAL_INT:   printf("%lld", (long long)ep->lit_value); break;
                        case PAT_LITERAL_FLOAT: printf("%g", ep->lit_value); break;
                        default: printf("_"); break;
                        }
                    }
                    if (pat->tail) {
                        printf("|");
                        if (pat->tail->kind == PAT_VAR)
                            printf("%s", pat->tail->var_name);
                        else
                            printf("_");
                    }
                    printf("]");
                    break;
                }
            }
            printf(" -> ");
            ast_print(cl->body);
        }
        break;
    case AST_LAYOUT:
        printf("(layout %s", ast->layout.name);
        for (int i = 0; i < ast->layout.field_count; i++) {
            ASTLayoutField *f = &ast->layout.fields[i];
            if (f->is_array)
                printf(" [%s :: [%s %d]]", f->name,
                       f->array_elem ? f->array_elem : "?", f->array_size);
            else
                printf(" [%s :: %s]", f->name,
                       f->type_name ? f->type_name : "?");
        }
        if (ast->layout.packed) printf(" :packed True");
        if (ast->layout.align)  printf(" :align %d", ast->layout.align);
        printf(")");
        break;

    case AST_SET:
        printf("{");
        for (size_t i = 0; i < ast->set.element_count; i++) {
            if (i > 0) printf(" ");
            ast_print(ast->set.elements[i]);
        }
        printf("}");
        break;

    case AST_MAP:
        printf("#{");
        for (size_t i = 0; i < ast->map.count; i++) {
            if (i > 0) printf(" ");
            ast_print(ast->map.keys[i]);
            printf(" ");
            ast_print(ast->map.vals[i]);
        }
        printf("}");
        break;

    case AST_DATA:
        printf("(data %s", ast->data.name);
        for (int i = 0; i < ast->data.constructor_count; i++) {
            if (i > 0) printf(" |");
            printf(" %s", ast->data.constructors[i].name);
            for (int j = 0; j < ast->data.constructors[i].field_count; j++)
                printf(" %s", ast->data.constructors[i].field_types[j]);
        }
        if (ast->data.deriving_count > 0) {
            printf(" deriving [");
            for (int i = 0; i < ast->data.deriving_count; i++) {
                if (i > 0) printf(" ");
                printf("%s", ast->data.deriving[i]);
            }
            printf("]");
        }
        printf(")");
        break;

    case AST_REFINEMENT:
        if (ast->refinement.name)
            printf("(type %s { %s ∈ %s | ",
                   ast->refinement.name,
                   ast->refinement.var  ? ast->refinement.var  : "?",
                   ast->refinement.base_type ? ast->refinement.base_type : "?");
        else
            printf("{ %s ∈ %s | ",
                   ast->refinement.var  ? ast->refinement.var  : "?",
                   ast->refinement.base_type ? ast->refinement.base_type : "?");
        ast_print(ast->refinement.predicate);
        if (ast->refinement.name) {
            printf(" }");
            if (ast->refinement.docstring)
                printf(" \"%s\"", ast->refinement.docstring);
            if (ast->refinement.alias_name)
                printf(" :alias %s", ast->refinement.alias_name);
            printf(")");
        } else {
            printf(" }");
        }
        break;

    case AST_CLASS:
        printf("(class %s %s where",
               ast->class_decl.name     ? ast->class_decl.name     : "?",
               ast->class_decl.type_var ? ast->class_decl.type_var : "?");
        for (int i = 0; i < ast->class_decl.method_count; i++)
            printf(" (%s) :: %s",
                   ast->class_decl.method_names[i]
                       ? ast->class_decl.method_names[i] : "?",
                   ast->class_decl.method_types[i]
                       ? ast->class_decl.method_types[i] : "?");
        printf(")");
        break;

    case AST_INSTANCE:
        printf("(instance %s %s where",
               ast->instance_decl.class_name
                   ? ast->instance_decl.class_name : "?",
               ast->instance_decl.type_name
                   ? ast->instance_decl.type_name  : "?");
        for (int i = 0; i < ast->instance_decl.method_count; i++) {
            printf("\n  (%s", ast->instance_decl.method_names[i]
                           ? ast->instance_decl.method_names[i] : "?");
            if (ast->instance_decl.method_bodies[i]) {
                printf(" = ");
                ast_print(ast->instance_decl.method_bodies[i]);
            }
            printf(")");
        }
        printf(")");
        break;
    }
}


/// Lexer

void lexer_init(Lexer *lex, const char *source) {
    lex->source = source;
    lex->pos    = 0;
    lex->line   = 1;
    lex->column = 1;
}

static char peek(Lexer *lex) {
    return lex->source[lex->pos];
}

static char peek_ahead(Lexer *lex, int offset) {
    return lex->source[lex->pos + offset];
}

static char advance(Lexer *lex) {
    char c = lex->source[lex->pos++];
    if (c == '\n') { lex->line++; lex->column = 1; }
    else            lex->column++;
    return c;
}

static void skip_whitespace(Lexer *lex) {
    while (peek(lex) == ' '  || peek(lex) == '\t' ||
           peek(lex) == '\n' || peek(lex) == '\r')
        advance(lex);
}

static void skip_line_comment(Lexer *lex) {
    while (peek(lex) != '\n' && peek(lex) != '\0')
        advance(lex);
}

static bool is_digit(char c)     { return c >= '0' && c <= '9'; }

/* Strip underscores from a number string (digit grouping).
 * Returns a newly malloc'd string — caller must free. */
static char *strip_underscores(const char *s, size_t len) {
    char *out = malloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] != '_') out[j++] = s[i];
    out[j] = '\0';
    return out;
}

static bool is_hex_digit(char c) {
    return is_digit(c) || (c>='a'&&c<='f') || (c>='A'&&c<='F');
}

/* A '.' is allowed INSIDE a symbol (for module-qualified access like M.phi
   or multi-part module names like Std.Math) but only when it is followed by
   a letter or digit – this prevents it from being mistaken for a decimal
   point in numeric literals, which are handled before symbols anyway. */
static bool is_symbol_start(unsigned char c) {
    return (c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
           c=='-' || c=='+' || c=='*' || c=='/' ||
           c=='<' || c=='>' || c=='=' || c=='!' ||
           c=='?' || c=='_' || c==':' || c=='%' ||
           c=='^' || c=='~' || c=='|' ||
           c > 127;  // UTF-8 multi-byte sequences
}

static bool is_symbol_char(unsigned char c) {
    return is_symbol_start(c) || (c>='0'&&c<='9') || c=='.';
}

/* When we are inside a symbol and see '.', only consume it if the next
   character is also a valid symbol character (letter/digit).  This avoids
   eating the '.' that belongs to ratio denominators or trailing punctuation. */
static bool is_symbol_dot_continuation(Lexer *lex) {
    unsigned char next = (unsigned char)lex->source[lex->pos + 1];
    return (next>='a'&&next<='z') || (next>='A'&&next<='Z') ||
           (next>='0'&&next<='9') || next=='_' || next > 127;
}

Token lexer_next_token(Lexer *lex) {
    Token tok = {0};

    skip_whitespace(lex);
    while (peek(lex) == ';') {
        skip_line_comment(lex);
        skip_whitespace(lex);
    }

    tok.line   = g_srcmap_abs_line > 0 ? g_srcmap_abs_line
                                       : lex->line + g_srcmap_line_bias;
    tok.column = lex->column + g_srcmap_col_bias;

    char c = peek(lex);

    if (c == '\0') { tok.type = TOK_EOF; return tok; }

    // #line N COL directive (emitted by wisp, maps transformed pos back to original)
    if (c == '(' && peek_ahead(lex, 1) == '#' &&
        peek_ahead(lex, 2) == 'l' && peek_ahead(lex, 3) == 'i' &&
        peek_ahead(lex, 4) == 'n' && peek_ahead(lex, 5) == 'e' &&
        peek_ahead(lex, 6) == ' ') {
        advance(lex); advance(lex); // skip '(' '#'
        /* skip "line " */
        while (peek(lex) != ' ' && peek(lex) != '\0') advance(lex);
        advance(lex); // skip space
        /* read line number */
        size_t nstart = lex->pos;
        while (is_digit(peek(lex))) advance(lex);
        char *linestr = my_strndup(lex->source + nstart, lex->pos - nstart);
        int orig_line = atoi(linestr);
        free(linestr);
        /* read col (currently unused but consumed) */
        advance(lex); // skip space
        while (is_digit(peek(lex))) advance(lex);
        /* consume closing ')' */
        while (peek(lex) != ')' && peek(lex) != '\0') advance(lex);
        if (peek(lex) == ')') advance(lex);
        /* store absolute original line; tok.line will use it directly */
        g_srcmap_abs_line  = orig_line;
        g_srcmap_line_bias = 0;
        return lexer_next_token(lex);
    }

    // Feature directives: #+ and #---
    if (c == '#') {
        if (peek_ahead(lex, 1) == '+') {
            advance(lex); // skip '#'
            advance(lex); // skip '+'
            tok.type = TOK_FEATURE_BEGIN;
            return tok;
        }
        if (peek_ahead(lex, 1) == '-' && peek_ahead(lex, 2) == '-' &&
            peek_ahead(lex, 3) == '-') {
            advance(lex); // skip '#'
            advance(lex); // skip '-'
            advance(lex); // skip '-'
            advance(lex); // skip '-'
            tok.type = TOK_FEATURE_END;
            return tok;
        }
    }

    // -| comment
    if (c == '-' && peek_ahead(lex, 1) == '|') {
        int cur_pos = (int)lex->pos;
        CommentSpan *span = comment_map_lookup(cur_pos);
        if (span) {
            if (span->close_pos >= 0) {
                /* Block comment — skip to after |- */
                while ((int)lex->pos < span->close_pos + 2)
                    advance(lex);
            } else {
                /* Paragraph comment — skip to para_end */
                while ((int)lex->pos < span->para_end)
                    advance(lex);
            }
            /* Tail-recurse: get next real token */
            return lexer_next_token(lex);
        }
    }

    // Arrow  ->
    if (c == '-' && peek_ahead(lex, 1) == '>') {
        advance(lex); advance(lex);
        tok.type  = TOK_ARROW;
        tok.value = my_strdup("->");
        return tok;
    }

    // Keyword :name  (colon NOT followed by another colon)
    if (c == ':' && peek_ahead(lex, 1) != ':' && peek_ahead(lex, 1) != '\0' &&
        is_symbol_start(peek_ahead(lex, 1))) {

        advance(lex); // skip ':'
        size_t start = lex->pos;
        while (is_symbol_char(peek(lex))) advance(lex);
        tok.value = my_strndup(lex->source + start, lex->pos - start);
        tok.type  = TOK_KEYWORD;
        return tok;
    }

    if (c == '(') { advance(lex); tok.type = TOK_LPAREN;   return tok; }
    if (c == ')') { advance(lex); tok.type = TOK_RPAREN;   return tok; }
    if (c == '[') { advance(lex); tok.type = TOK_LBRACKET; return tok; }
    if (c == ']') { advance(lex); tok.type = TOK_RBRACKET; return tok; }
    if (c == '#' && peek_ahead(lex, 1) == '{') {
        advance(lex); advance(lex);
        tok.type = TOK_HASH_LBRACE;
        return tok;
    }
    if (c == '{') { advance(lex); tok.type = TOK_LBRACE;   return tok; }
    if (c == '}') { advance(lex); tok.type = TOK_RBRACE;   return tok; }
    if (c == ',') { advance(lex); tok.type = TOK_SYMBOL; tok.value = my_strdup(","); return tok; }
    if (c == '|' && peek_ahead(lex, 1) == '-') {
        /* Stray |- closer — already consumed as part of a comment span,
         * but if we somehow reach it, skip it silently */
        advance(lex); advance(lex);
        return lexer_next_token(lex);
    }

    // Character literal or quote
    if (c == '\'') {
        size_t lp = lex->pos + 1;
        if (lp < strlen(lex->source)) {
            char next = lex->source[lp];
            if (next == '\\') {
                if (lp + 2 < strlen(lex->source) &&
                    lex->source[lp + 2] == '\'') {
                    advance(lex); advance(lex);
                    char ch = peek(lex);
                    switch (ch) {
                    case 'n': ch='\n'; break; case 't': ch='\t'; break;
                    case 'r': ch='\r'; break; case '\\': ch='\\'; break;
                    case '\'': ch='\''; break; case '0': ch='\0'; break;
                    }
                    advance(lex);
                    if (peek(lex) != '\'') {
                        /* fprintf(stderr, "Unterminated char literal\n"); exit(1); */
                        READER_ERROR(lex->line, lex->column, "unterminated character literal");
                    }
                    advance(lex);
                    tok.value    = malloc(2);
                    tok.value[0] = ch; tok.value[1] = '\0';
                    tok.type     = TOK_CHAR;
                    return tok;
                }
            } else if (next != '\'' && next != '\0') {
                if (lp + 1 < strlen(lex->source) &&
                    lex->source[lp + 1] == '\'') {
                    advance(lex);
                    char ch = peek(lex);
                    advance(lex);
                    if (peek(lex) != '\'') {
                        /* fprintf(stderr, "Unterminated char literal\n"); exit(1); */
                        READER_ERROR(lex->line, lex->column, "unterminated character literal");
                    }
                    advance(lex);
                    tok.value    = malloc(2);
                    tok.value[0] = ch; tok.value[1] = '\0';
                    tok.type     = TOK_CHAR;
                    return tok;
                }
            }
        }
        advance(lex);
        tok.type = TOK_QUOTE;
        return tok;
    }

    // String
    if (c == '"') {
        advance(lex);
        size_t start = lex->pos;
        while (peek(lex) != '"' && peek(lex) != '\0') {
            if (peek(lex) == '\\') advance(lex);
            advance(lex);
        }
        tok.value = my_strndup(lex->source + start, lex->pos - start);
        advance(lex);
        tok.type  = TOK_STRING;
        return tok;
    }

    // Hex / Binary / Octal
    if (c == '0' && (peek_ahead(lex,1)=='x'||peek_ahead(lex,1)=='X')) {
        size_t start = lex->pos;
        advance(lex); advance(lex);
        while (is_hex_digit(peek(lex))) advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_NUMBER; return tok;
    }
    if (c == '0' && (peek_ahead(lex,1)=='b'||peek_ahead(lex,1)=='B')) {
        size_t start = lex->pos;
        advance(lex); advance(lex);
        while (peek(lex)=='0'||peek(lex)=='1') advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_NUMBER; return tok;
    }
    if (c == '0' && (peek_ahead(lex,1)=='o'||peek_ahead(lex,1)=='O')) {
        size_t start = lex->pos;
        advance(lex); advance(lex);
        while (peek(lex)>='0'&&peek(lex)<='7') advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_NUMBER; return tok;
    }

    // Negative number  (but not standalone '-' which is a symbol)
    if (c == '-' && is_digit(peek_ahead(lex, 1))) {
        size_t start = lex->pos;
        advance(lex);
        while (is_digit(peek(lex)) || peek(lex) == '_' ||
              (peek(lex) == '.' && peek_ahead(lex, 1) != '.')) advance(lex);
        if (peek(lex) == 'e' && (is_digit(peek_ahead(lex, 1)) ||
            peek_ahead(lex, 1) == '+' || peek_ahead(lex, 1) == '-')) {
            advance(lex); // consume 'e'
            if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
            while (is_digit(peek(lex)) || peek(lex) == '_') advance(lex);
        }
        char *raw = my_strndup(lex->source+start, lex->pos-start);
        tok.value = strip_underscores(raw, strlen(raw));
        free(raw);
        tok.type  = TOK_NUMBER; return tok;
    }


    // .. range operator, must be checked before '.' in numbers
    if (c == '.' && peek_ahead(lex, 1) == '.') {
        advance(lex); advance(lex);
        tok.type  = TOK_DOTDOT;
        tok.value = my_strdup("..");
        return tok;
    }

    // Standalone . for rest params (define (f . args) ...)
    if (c == '.' && peek_ahead(lex, 1) == ' ') {
        advance(lex);
        tok.type  = TOK_SYMBOL;
        tok.value = my_strdup(".");
        return tok;
    }

    // Decimal number  (also handles NrDIGITS radix literals and _ grouping)
    if (is_digit(c)) {
        size_t start = lex->pos;
        while (is_digit(peek(lex)) || peek(lex) == '_') advance(lex);

        /* Radix literal: <base>r<digits>  e.g. 16rFF  2r1010  8r77 */
        if (peek(lex) == 'r') {
            /* extract base string, strip underscores */
            char *base_raw = my_strndup(lex->source + start, lex->pos - start);
            char *base_clean = strip_underscores(base_raw, strlen(base_raw));
            free(base_raw);
            int radix = atoi(base_clean);
            free(base_clean);
            if (radix < 2 || radix > 36)
                READER_ERROR(lex->line, lex->column,
                             "radix %d out of range (must be 2–36)", radix);
            advance(lex); /* consume 'r' */
            size_t digit_start = lex->pos;
            while (is_hex_digit(peek(lex)) || peek(lex) == '_') advance(lex);
            if (lex->pos == digit_start)
                READER_ERROR(lex->line, lex->column,
                             "expected digits after radix prefix '%dr'", radix);
            char *digits_raw = my_strndup(lex->source + digit_start,
                                          lex->pos - digit_start);
            char *digits = strip_underscores(digits_raw, strlen(digits_raw));
            free(digits_raw);
            /* convert to decimal string for the rest of the pipeline */
            uint64_t val = strtoull(digits, NULL, radix);
            free(digits);
            char buf[32];
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
            /* store original notation as literal_str via tok.value;
             * parse_number_str will see a plain decimal and work correctly.
             * We embed the original as a comment prefix so set_raw_int works. */
            tok.value    = my_strdup(buf);
            tok.type     = TOK_NUMBER;
            return tok;
        }

        /* Floating point / ratio / plain integer */
        if (peek(lex) == '.' && peek_ahead(lex, 1) != '.') {
            advance(lex); /* consume '.' */
            while (is_digit(peek(lex)) || peek(lex) == '_') advance(lex);
        }
        if (peek(lex) == '/') {
            advance(lex); /* consume '/' */
            if (!is_digit(peek(lex))) {
                fprintf(stderr, "Invalid ratio: missing denominator\n");
                exit(1);
            }
            while (is_digit(peek(lex)) || peek(lex) == '_') advance(lex);
        } else if (peek(lex) == 'e' && (is_digit(peek_ahead(lex, 1)) ||
                   peek_ahead(lex, 1) == '+' || peek_ahead(lex, 1) == '-')) {
            advance(lex); /* consume 'e' */
            if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
            while (is_digit(peek(lex)) || peek(lex) == '_') advance(lex);
        }
        char *raw = my_strndup(lex->source + start, lex->pos - start);
        tok.value = strip_underscores(raw, strlen(raw));
        free(raw);
        tok.type  = TOK_NUMBER;
        return tok;
    }

    // & prefix — address-of reader macro
    if (c == '&') {
        advance(lex);
        tok.type  = TOK_SYMBOL;
        tok.value = my_strdup("&");
        return tok;
    }

    // λ — pure lambda calculus literal (UTF-8: 0xCE 0xBB)
    // Syntax: λx.body  where x is a single param name
    if ((unsigned char)c == 0xCE &&
        (unsigned char)lex->source[lex->pos + 1] == 0xBB) {
        advance(lex); // consume 0xCE
        advance(lex); // consume 0xBB

        skip_whitespace(lex);

        // Read exactly one param name — stop at '.' or whitespace
        if (!is_symbol_start((unsigned char)peek(lex))) {
            READER_ERROR(lex->line, lex->column,
                         "expected parameter name after λ");
        }
        size_t start = lex->pos;
        while (peek(lex) != '.' && peek(lex) != ' ' &&
               peek(lex) != '\t' && peek(lex) != '\n' &&
               peek(lex) != '\0' &&
               is_symbol_char((unsigned char)peek(lex))) advance(lex);
        char *param_name = my_strndup(lex->source + start, lex->pos - start);

        skip_whitespace(lex);

        // Must be followed by '.' — if not, user wrote λx y. which is invalid
        if (peek(lex) != '.') {
            READER_ERROR(lex->line, lex->column,
                         "λ-literal takes exactly one parameter — "
                         "use λx.λy.body for curried functions, not λx y.body");
        }
        advance(lex); // consume '.'

        tok.type  = TOK_LAMBDA_LIT;
        tok.value = param_name;
        return tok;
    }

    // Symbol  (letters, operator chars, digits mid-token, and '.' mid-token)
    if (is_symbol_start((unsigned char)c)) {
        size_t start = lex->pos;
        advance(lex); // consume the first character

        /* Continue consuming symbol characters.  For '.', only consume it
           when it is followed by a letter/digit (module-access dot), not
           when it might be trailing punctuation. */
        while (true) {
            char nc = peek(lex);
            if (nc == '\0') break;
            if (nc == '.') {
                /* peek at the character after the dot */
                char after = lex->source[lex->pos + 1];
                if ((after>='a'&&after<='z') || (after>='A'&&after<='Z') ||
                    (after>='0'&&after<='9') || after=='_') {
                    advance(lex); // consume '.'
                    continue;
                } else {
                    break; // trailing dot – stop here
                }
            }
            if (is_symbol_char(nc) && nc != '.') {
                advance(lex);
            } else {
                break;
            }
        }

        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_SYMBOL; return tok;
    }

    READER_ERROR(lex->line, lex->column, "unexpected character '%c'", c);
}

/// Parser

static void parser_init(Parser *p, Lexer *lex) {
    p->lexer   = lex;
    p->current = lexer_next_token(lex);
}

static ASTParam parse_one_param(Parser *p) {
    ASTParam param = {NULL, NULL, false};

    if (p->current.type == TOK_SYMBOL) {
        param.name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        if (p->current.type == TOK_SYMBOL &&
            strcmp(p->current.value, "::") == 0) {
            p->current = lexer_next_token(p->lexer);

            char type_buf[512] = {0};
            while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
                const char *tok_str = p->current.value ? p->current.value
                                    : (p->current.type == TOK_ARROW ? "->" : NULL);
                if (tok_str) {
                    if (type_buf[0]) strncat(type_buf, " ", sizeof(type_buf) - strlen(type_buf) - 1);
                    strncat(type_buf, tok_str, sizeof(type_buf) - strlen(type_buf) - 1);
                }
                p->current = lexer_next_token(p->lexer);
            }
            if (type_buf[0]) {
                param.type_name = my_strdup(type_buf);
            }
        }
    }
    return param;
}

static void parse_fn_signature(Parser *p, ASTParam **out_params,
                               int *out_count, char **out_return_type) {
    ASTParam *params   = NULL;
    int       count    = 0;
    int       capacity = 0;
    char     *ret_type = NULL;

    while (p->current.type != TOK_RPAREN &&
           p->current.type != TOK_EOF) {
        // Rest parameter: . args  or  . [args :: Type]
        if (p->current.type == TOK_SYMBOL &&
            strcmp(p->current.value, ".") == 0) {
            p->current = lexer_next_token(p->lexer);

            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params   = realloc(params, sizeof(ASTParam) * capacity);
            }

            if (p->current.type == TOK_LBRACKET) {
                // Typed rest: . [args :: Type]
                p->current = lexer_next_token(p->lexer); // consume '['
                ASTParam param = parse_one_param(p);
                if (p->current.type != TOK_RBRACKET)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected ']' after typed rest parameter");
                p->current = lexer_next_token(p->lexer); // consume ']'
                param.is_rest = true;
                param.is_anon = false;
                params[count++] = param;
            } else if (p->current.type == TOK_ARROW ||
                       p->current.type == TOK_RPAREN) {
                /* Bare `.` with no name and no type — anonymous untyped rest */
                params[count].name      = my_strdup("__pm_args");
                params[count].type_name = NULL;
                params[count].is_rest   = true;
                params[count].is_anon   = true;
                count++;
                /* Do NOT advance — let the arrow/rparen be consumed normally */
            } else if (p->current.type == TOK_SYMBOL) {
                bool rest_type_only = (p->current.value[0] >= 'A' &&
                                       p->current.value[0] <= 'Z');
                if (rest_type_only) {
                    /* Anonymous typed rest: . Int */
                    char gen_name[32];
                    snprintf(gen_name, sizeof(gen_name), "__pm_args");
                    params[count].name      = my_strdup(gen_name);
                    params[count].type_name = my_strdup(p->current.value);
                    params[count].is_rest   = true;
                    params[count].is_anon   = true;
                } else {
                    /* Bare named rest: . args */
                    params[count].name      = my_strdup(p->current.value);
                    params[count].type_name = NULL;
                    params[count].is_rest   = true;
                    params[count].is_anon   = false;
                }
                count++;
                p->current = lexer_next_token(p->lexer);
            } else {
                compiler_error(p->current.line, p->current.column,
                               "Expected parameter name or '[name :: Type]' after '.'");
            }
            // Rest param must be last — skip to closing paren
            break;
        }
        if (p->current.type == TOK_LBRACKET) {
            /* Typed or annotated parameter: [name] or [name :: Type] */
            p->current = lexer_next_token(p->lexer);
            ASTParam param = parse_one_param(p);
            if (p->current.type != TOK_RBRACKET)
                compiler_error(p->current.line, p->current.column,
                               "Expected ']' after parameter");
            p->current = lexer_next_token(p->lexer);
            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params   = realloc(params, sizeof(ASTParam) * capacity);
            }
            param.is_anon = false;
            params[count++] = param;

        } else if (p->current.type == TOK_ARROW) {
            p->current = lexer_next_token(p->lexer);

            if (p->current.type == TOK_SYMBOL) {
                char type_buf[512] = {0};

                /* Keep consuming until we hit ')' */
                while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                    const char *tok_str = p->current.value ? p->current.value
                                        : (p->current.type == TOK_ARROW ? "->" : NULL);
                    if (tok_str) {
                        if (type_buf[0]) strncat(type_buf, " ", sizeof(type_buf) - strlen(type_buf) - 1);
                        strncat(type_buf, tok_str, sizeof(type_buf) - strlen(type_buf) - 1);
                    }
                    p->current = lexer_next_token(p->lexer);
                }

                free(ret_type);
                ret_type = my_strdup(type_buf);
            }
        } else if (p->current.type == TOK_SYMBOL) {
            /* Bare symbol NOT after -> — could be a named param or an
             * anonymous typed param (uppercase = type only, e.g. Int).  */
            char *sym  = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params   = realloc(params, sizeof(ASTParam) * capacity);
            }
            /* Heuristic: starts with uppercase → type-only anonymous param */
            bool is_type_only = (sym[0] >= 'A' && sym[0] <= 'Z');
            if (is_type_only) {
                char gen_name[32];
                snprintf(gen_name, sizeof(gen_name), "__p_%d", count);
                params[count].name      = my_strdup(gen_name);
                params[count].type_name = sym;  /* sym is the type name */
                params[count].is_rest   = false;
                params[count].is_anon   = true;
            } else {
                params[count].name      = sym;
                params[count].type_name = NULL;
                params[count].is_rest   = false;
                params[count].is_anon   = false;
            }
            count++;


        } else {
            compiler_error(p->current.line, p->current.column,
                           "Unexpected token in function signature");
        }
    }

    // Consume optional `-> ReturnType` that follows the last parameter
    // (common when the last param is a rest param and the loop broke early)
    if (p->current.type == TOK_ARROW) {
        p->current = lexer_next_token(p->lexer);
        if (p->current.type == TOK_SYMBOL) {
            char type_buf[512] = {0};
            while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                const char *tok_str = p->current.value ? p->current.value
                                    : (p->current.type == TOK_ARROW ? "->" : NULL);
                if (tok_str) {
                    if (type_buf[0]) strncat(type_buf, " ", sizeof(type_buf) - strlen(type_buf) - 1);
                    strncat(type_buf, tok_str, sizeof(type_buf) - strlen(type_buf) - 1);
                }
                p->current = lexer_next_token(p->lexer);
            }
            free(ret_type);
            ret_type = my_strdup(type_buf);
        }
    }

    if (p->current.type != TOK_RPAREN)
        compiler_error(p->current.line, p->current.column,
                       "Expected ')' to close function signature");
    p->current = lexer_next_token(p->lexer);

    *out_params      = params;
    *out_count       = count;
    *out_return_type = ret_type;
}

static AST *parse_lambda(Parser *p) {
    if (p->current.type != TOK_LPAREN) {
        compiler_error(p->current.line, p->current.column,
                       "Expected '(' after 'lambda'");
    }
    p->current = lexer_next_token(p->lexer);

    ASTParam *params   = NULL;
    int       count    = 0;
    char     *ret_type = NULL;
    parse_fn_signature(p, &params, &count, &ret_type);

    char *docstring = NULL;
    if (p->current.type == TOK_STRING) {
        docstring  = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);
    }

    // Collect multiple body expressions — last one is the return value
    AST **body_exprs = NULL;
    int   body_count = 0;
    while (p->current.type != TOK_RPAREN &&
           p->current.type != TOK_EOF    &&
           p->current.type != TOK_KEYWORD) {
        body_exprs = realloc(body_exprs, sizeof(AST*) * (body_count + 1));
        body_exprs[body_count++] = parse_expr(p);
    }
    if (body_count == 0) {
        compiler_error(p->current.line, p->current.column,
                       "lambda body cannot be empty");
    }
    AST *body = body_exprs[body_count - 1];

    AST *result = ast_new_lambda(params, count, ret_type, docstring, NULL, false,
                                  body, body_exprs, body_count);
    free(docstring);
    free(ret_type);
    return result;
}

typedef struct {
    char *docstring;
    char *alias_name;
    bool naked;
    bool naked_set;    // whether :naked was explicitly provided
    size_t consumed;   // how many items were consumed
} DefineMetadata;

// Scans items[start .. end-1] for optional metadata before the body.
// The LAST item is always the body — we scan everything before it.
static DefineMetadata parse_define_metadata(Parser *p) {
    DefineMetadata m = {NULL, NULL, 0};

    // We peek at the current token repeatedly.
    // Metadata items are:
    //   "string"        -> docstring (only if no :doc seen yet)
    //   :doc "string"   -> docstring
    //   :alias symbol   -> alias
    // Anything else = stop, that's the body.

    while (true) {
        // Plain string docstring — only if something follows before ')'
        // If the string is the only thing left, it's the function body.
        if (p->current.type == TOK_STRING && !m.docstring) {
            Lexer saved_lex = *p->lexer;
            Token saved_cur = p->current;
            char *s = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
            if (p->current.type == TOK_RPAREN ||
                p->current.type == TOK_EOF) {
                /* Nothing after — restore and let body parsing take it */
                *p->lexer  = saved_lex;
                p->current = saved_cur;
                free(s);
                break;
            }
            m.docstring = s;
            m.consumed++;
            continue;
        }

        // :doc "string"
        if (p->current.type == TOK_KEYWORD &&
            strcmp(p->current.value, "doc") == 0) {
            p->current = lexer_next_token(p->lexer);
            if (p->current.type == TOK_STRING) {
                free(m.docstring);  // override if already set
                m.docstring = my_strdup(p->current.value);
                p->current  = lexer_next_token(p->lexer);
            }
            m.consumed++;
            continue;
        }

        // :alias symbol
        if (p->current.type == TOK_KEYWORD &&
            strcmp(p->current.value, "alias") == 0) {
            p->current = lexer_next_token(p->lexer);
            if (p->current.type == TOK_SYMBOL) {
                free(m.alias_name);
                m.alias_name = my_strdup(p->current.value);
                p->current   = lexer_next_token(p->lexer);
            }
            m.consumed++;
            continue;
        }

        // :naked True / :naked False
        if (p->current.type == TOK_KEYWORD &&
            strcmp(p->current.value, "naked") == 0) {
            p->current = lexer_next_token(p->lexer);
            if (p->current.type == TOK_SYMBOL) {
                if (strcmp(p->current.value, "True") == 0) {
                    m.naked     = true;
                    m.naked_set = true;
                    p->current  = lexer_next_token(p->lexer);
                } else if (strcmp(p->current.value, "False") == 0) {
                    m.naked     = false;
                    m.naked_set = true;
                    p->current  = lexer_next_token(p->lexer);
                } else {
                    compiler_error(p->current.line, p->current.column,
                                   ":naked requires True or False");
                }
            } else {
                compiler_error(p->current.line, p->current.column,
                               ":naked requires True or False");
            }
            m.consumed++;
            continue;
        }

        break;  // not a metadata token — must be the body
    }

    return m;
}

static AST *build_let(ASTParam *params, int param_count,
                      AST **inits,
                      AST **body_exprs, int body_count,
                      int line, int col) {
    AST *lam = ast_new_lambda(params, param_count, NULL,
                               NULL, NULL, false,
                               body_exprs[body_count - 1],
                               body_exprs, body_count);
    lam->line   = line;
    lam->column = col;

    AST *call = ast_new_list();
    ast_list_append(call, lam);
    for (int i = 0; i < param_count; i++)
        ast_list_append(call, inits[i]);
    call->line   = line;
    call->column = col;
    return call;
}

static AST *parse_cond(Parser *p) {

    if (p->current.type == TOK_RPAREN || p->current.type == TOK_EOF) {
        compiler_error(p->current.line, p->current.column,
                       "'cond' requires at least one clause");
    }

    if (p->current.type != TOK_LBRACKET) {
        compiler_error(p->current.line, p->current.column,
                       "'cond' clauses must use bracket syntax [test body...]");
    }

    int clause_line = p->current.line;
    int clause_col  = p->current.column;

    p->current = lexer_next_token(p->lexer); // consume '['

    // Check for 'else'
    bool is_else = (p->current.type == TOK_SYMBOL &&
                    strcmp(p->current.value, "else") == 0);

    if (is_else) {
        p->current = lexer_next_token(p->lexer); // consume 'else'

        AST **body = NULL;
        int   body_count = 0;
        while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
            body = realloc(body, sizeof(AST*) * (body_count + 1));
            body[body_count++] = parse_expr(p);
        }
        if (p->current.type != TOK_RBRACKET)
            compiler_error(p->current.line, p->current.column,
                           "Expected ']' to close 'else' clause");
        p->current = lexer_next_token(p->lexer); // consume ']'

        if (p->current.type != TOK_RPAREN)
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' after 'else' clause — 'else' must be last");
        p->current = lexer_next_token(p->lexer); // consume ')'

        if (body_count == 0)
            compiler_error(clause_line, clause_col,
                           "'else' clause requires at least one expression");

        if (body_count == 1) {
            AST *r = body[0];
            free(body);
            return r;
        }

        AST *begin = ast_new_list();
        ast_list_append(begin, ast_new_symbol("begin"));
        for (int i = 0; i < body_count; i++)
            ast_list_append(begin, body[i]);
        free(body);
        begin->line   = clause_line;
        begin->column = clause_col;
        return begin;
    }

    // Normal clause: parse test expression
    AST *test = parse_expr(p);

    // Short form: [test] with no body
    if (p->current.type == TOK_RBRACKET) {
        p->current = lexer_next_token(p->lexer); // consume ']'

        AST *rest = NULL;
        if (p->current.type != TOK_RPAREN)
            rest = parse_cond(p);
        else
            p->current = lexer_next_token(p->lexer); // consume ')'

        if (!rest)
            return test;

        // (let ([__cond_tmp_N test]) (if __cond_tmp_N __cond_tmp_N rest))
        static int cond_tmp_counter = 0;
        char tmp_name[32];
        snprintf(tmp_name, sizeof(tmp_name), "__cond_tmp_%d", cond_tmp_counter++);

        ASTParam *params = malloc(sizeof(ASTParam));
        params[0].name      = strdup(tmp_name);
        params[0].type_name = NULL;

        AST *if_node = ast_new_list();
        ast_list_append(if_node, ast_new_symbol("if"));
        ast_list_append(if_node, ast_new_symbol(tmp_name));
        ast_list_append(if_node, ast_new_symbol(tmp_name));
        ast_list_append(if_node, rest);
        if_node->line   = clause_line;
        if_node->column = clause_col;

        AST **inits      = malloc(sizeof(AST*));      inits[0]      = test;
        AST **body_exprs = malloc(sizeof(AST*));      body_exprs[0] = if_node;
        return build_let(params, 1, inits, body_exprs, 1, clause_line, clause_col);
    }

    // Check for => operator
    bool is_arrow = (p->current.type == TOK_SYMBOL &&
                     strcmp(p->current.value, "=>") == 0);

    if (is_arrow) {
        p->current = lexer_next_token(p->lexer); // consume '=>'

        AST *fn = parse_expr(p);

        if (p->current.type != TOK_RBRACKET)
            compiler_error(p->current.line, p->current.column,
                           "Expected ']' after function in '=>' clause");
        p->current = lexer_next_token(p->lexer); // consume ']'

        AST *rest = NULL;
        if (p->current.type != TOK_RPAREN)
            rest = parse_cond(p);
        else
            p->current = lexer_next_token(p->lexer); // consume ')'

        static int arrow_tmp_counter = 0;
        char tmp_name[32];
        snprintf(tmp_name, sizeof(tmp_name), "__cond_arrow_%d", arrow_tmp_counter++);

        // (fn __cond_arrow_N)
        AST *call = ast_new_list();
        ast_list_append(call, fn);
        ast_list_append(call, ast_new_symbol(tmp_name));
        call->line   = clause_line;
        call->column = clause_col;

        // (if __cond_arrow_N (fn __cond_arrow_N) rest)
        AST *if_node = ast_new_list();
        ast_list_append(if_node, ast_new_symbol("if"));
        ast_list_append(if_node, ast_new_symbol(tmp_name));
        ast_list_append(if_node, call);
        if (rest)
            ast_list_append(if_node, rest);
        if_node->line   = clause_line;
        if_node->column = clause_col;

        ASTParam *params = malloc(sizeof(ASTParam));
        params[0].name      = strdup(tmp_name);
        params[0].type_name = NULL;

        AST **inits      = malloc(sizeof(AST*));      inits[0]      = test;
        AST **body_exprs = malloc(sizeof(AST*));      body_exprs[0] = if_node;
        return build_let(params, 1, inits, body_exprs, 1, clause_line, clause_col);
    }

    // Normal clause: [test body...]
    AST **body = NULL;
    int   body_count = 0;
    while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
        body = realloc(body, sizeof(AST*) * (body_count + 1));
        body[body_count++] = parse_expr(p);
    }
    if (p->current.type != TOK_RBRACKET)
        compiler_error(p->current.line, p->current.column,
                       "Expected ']' to close cond clause");
    p->current = lexer_next_token(p->lexer); // consume ']'

    AST *rest = NULL;
    if (p->current.type != TOK_RPAREN)
        rest = parse_cond(p);
    else
        p->current = lexer_next_token(p->lexer); // consume ')'

    AST *then;
    if (body_count == 0) {
        then = test;
    } else if (body_count == 1) {
        then = body[0];
        free(body);
    } else {
        then = ast_new_list();
        ast_list_append(then, ast_new_symbol("begin"));
        for (int i = 0; i < body_count; i++)
            ast_list_append(then, body[i]);
        free(body);
        then->line   = clause_line;
        then->column = clause_col;
    }

    AST *if_node = ast_new_list();
    ast_list_append(if_node, ast_new_symbol("if"));
    ast_list_append(if_node, test);
    ast_list_append(if_node, then);
    if (rest)
        ast_list_append(if_node, rest);
    if_node->line   = clause_line;
    if_node->column = clause_col;
    return if_node;
}

static AST *desugar_cond_clauses(AST **clauses, int count, int line, int col) {
    if (count == 0) {
        // No more clauses — return 0 as fallthrough (should be unreachable
        // since pmatch always adds a final wildcard/undefined clause)
        return ast_new_number(0, "0");
    }

    AST *clause = clauses[0];
    if (clause->type != AST_LIST || clause->list.count < 2)
        return desugar_cond_clauses(clauses + 1, count - 1, line, col);

    AST *guard = clause->list.items[0];
    int  nbody = (int)clause->list.count - 1;

    // else clause
    if (guard->type == AST_SYMBOL && strcmp(guard->symbol, "else") == 0) {
        if (nbody == 1) return ast_clone(clause->list.items[1]);
        AST *begin = ast_new_list();
        ast_list_append(begin, ast_new_symbol("begin"));
        for (int i = 0; i < nbody; i++)
            ast_list_append(begin, ast_clone(clause->list.items[1 + i]));
        return begin;
    }

    AST *then;
    if (nbody == 1) {
        then = ast_clone(clause->list.items[1]);
    } else {
        then = ast_new_list();
        ast_list_append(then, ast_new_symbol("begin"));
        for (int i = 0; i < nbody; i++)
            ast_list_append(then, ast_clone(clause->list.items[1 + i]));
    }

    AST *rest = desugar_cond_clauses(clauses + 1, count - 1, line, col);

    AST *if_node = ast_new_list();
    ast_list_append(if_node, ast_new_symbol("if"));
    ast_list_append(if_node, ast_clone(guard));
    ast_list_append(if_node, then);
    ast_list_append(if_node, rest);
    if_node->line   = line;
    if_node->column = col;
    return if_node;
}

AST *desugar_cond_ast(AST *cond_list) {
    if (!cond_list || cond_list->type != AST_LIST || cond_list->list.count < 2)
        return cond_list;
    // items[0] is "cond", items[1..] are clauses
    int   nclause = (int)cond_list->list.count - 1;
    AST **clauses = &cond_list->list.items[1];
    return desugar_cond_clauses(clauses, nclause,
                                cond_list->line, cond_list->column);
}


// Desugar a synthetically built (let ([n e]...) body) AST_LIST
// into a build_let lambda-call. The input list owns its children —
// we steal them so do NOT free the input after calling this.
AST *desugar_let_ast(AST *let_list) {
    // let_list->list.items:
    //   [0] = symbol "let"
    //   [1] = list of binding pairs ([name expr]...)
    //   [2..] = body expressions
    if (!let_list || let_list->type != AST_LIST || let_list->list.count < 3)
        return let_list;

    AST *bindings_node = let_list->list.items[1];

    ASTParam *params     = NULL;
    int       param_count = 0;
    AST     **inits      = NULL;
    int       init_count = 0;

    for (size_t i = 0; i < bindings_node->list.count; i++) {
        AST *pair = bindings_node->list.items[i];
        if (pair->type != AST_LIST || pair->list.count < 2) continue;
        char *bname = my_strdup(pair->list.items[0]->symbol);
        AST  *init  = pair->list.items[1];
        // steal init from pair so it isn't freed with let_list
        pair->list.items[1] = NULL;

        params = realloc(params, sizeof(ASTParam) * (param_count + 1));
        params[param_count].name      = bname;
        params[param_count].type_name = NULL;
        params[param_count].is_rest   = false;
        params[param_count].is_anon   = false;
        param_count++;

        inits = realloc(inits, sizeof(AST*) * (init_count + 1));
        inits[init_count++] = init;
    }

    int   body_count = (int)let_list->list.count - 2;
    AST **body_exprs = malloc(sizeof(AST*) * (body_count ? body_count : 1));
    for (int i = 0; i < body_count; i++) {
        body_exprs[i] = let_list->list.items[2 + i];
        // steal from let_list
        let_list->list.items[2 + i] = NULL;
    }

    int line = let_list->line, col = let_list->column;
    fprintf(stderr, "DEBUG desugar_let_ast: param_count=%d body_count=%d\n",
            param_count, body_count);
    for (int i = 0; i < param_count; i++)
        fprintf(stderr, "  param[%d] name='%s' type_name='%s'\n",
                i,
                params[i].name ? params[i].name : "NULL",
                params[i].type_name ? params[i].type_name : "NULL");
    return build_let(params, param_count, inits, body_exprs, body_count, line, col);
}

static AST *parse_list(Parser *p) {
    int start_line = p->current.line;
    int start_column = p->current.column;

    AST *list = ast_new_list();

    p->current = lexer_next_token(p->lexer);

    // Detect  (lambda ...)
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "lambda") == 0) {
        p->current = lexer_next_token(p->lexer);
        AST *lam = parse_lambda(p);

        if (p->current.type != TOK_RPAREN) {
            compiler_error(p->current.line, p->current.column,
                         "Expected ')' after lambda body");
        }

        int end_column = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        lam->line = start_line;
        lam->column = start_column;
        lam->end_column = end_column;
        return lam;
    }

    // Detect (cond [clause...] ...)
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "cond") == 0) {
        p->current = lexer_next_token(p->lexer);
        ast_free(list);
        AST *result = parse_cond(p);
        result->line   = start_line;
        result->column = start_column;
        return result;
    }

if (p->current.type == TOK_SYMBOL &&
        (strcmp(p->current.value, "let") == 0 ||
         strcmp(p->current.value, "let*") == 0)) {
        bool is_sequential = (strcmp(p->current.value, "let*") == 0);
        p->current = lexer_next_token(p->lexer);

        // Support both (let ([x e] ...) body) and (let [x e] body)
        bool single_binding = (p->current.type == TOK_LBRACKET);
        if (!single_binding) {
            if (p->current.type != TOK_LPAREN)
                compiler_error(p->current.line, p->current.column,
                               "Expected '(' or '[' after 'let'");
            p->current = lexer_next_token(p->lexer);
        }

        ASTParam *params  = NULL;
        int       param_count = 0;
        AST     **inits   = NULL;
        int       init_count = 0;

        if (single_binding) {
            // (let [x e] body) — single binding, no outer parens
            if (p->current.type != TOK_LBRACKET)
                compiler_error(p->current.line, p->current.column,
                               "Expected '[' in let binding");
            p->current = lexer_next_token(p->lexer);
            if (p->current.type != TOK_SYMBOL)
                compiler_error(p->current.line, p->current.column,
                               "Expected symbol in let binding");
            char *bname = strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
            AST *init_expr = parse_expr(p);
            if (p->current.type != TOK_RBRACKET)
                compiler_error(p->current.line, p->current.column,
                               "Expected ']' after let binding value");
            p->current = lexer_next_token(p->lexer);
            params = realloc(params, sizeof(ASTParam) * (param_count + 1));
            params[param_count].name      = bname;
            params[param_count].type_name = NULL;
            params[param_count].is_rest   = false;
            params[param_count].is_anon   = false;
            param_count++;
            inits = realloc(inits, sizeof(AST*) * (init_count + 1));
            inits[init_count++] = init_expr;
        } else {
            while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
                if (p->current.type != TOK_LBRACKET)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected '[' in let binding");
                p->current = lexer_next_token(p->lexer);
                if (p->current.type != TOK_SYMBOL)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected symbol in let binding");
                char *bname = strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
                AST *init_expr = parse_expr(p);
                if (p->current.type != TOK_RBRACKET)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected ']' after let binding value");
                p->current = lexer_next_token(p->lexer);
                params = realloc(params, sizeof(ASTParam) * (param_count + 1));
                params[param_count].name      = bname;
                params[param_count].type_name = NULL;
                params[param_count].is_rest   = false;
                params[param_count].is_anon   = false;
                param_count++;
                inits = realloc(inits, sizeof(AST*) * (init_count + 1));
                inits[init_count++] = init_expr;
            }
            p->current = lexer_next_token(p->lexer); // consume ')'
        }

        AST **body_exprs = NULL;
        int   body_count = 0;
        while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
            body_exprs = realloc(body_exprs, sizeof(AST*) * (body_count + 1));
            body_exprs[body_count++] = parse_expr(p);
        }
        if (body_count == 0)
            compiler_error(p->current.line, p->current.column, "let body cannot be empty");
        p->current = lexer_next_token(p->lexer); // consume final ')'

        ast_free(list);
        if (!is_sequential || param_count <= 1) {
            return build_let(params, param_count, inits, body_exprs, body_count, start_line, start_column);
        }
        /* let* — desugar into nested single-binding lets, right to left.
         * (let* ([a e1] [b e2] [c e3]) body)
         * => (let ([a e1]) (let ([b e2]) (let ([c e3]) body))) */
        AST **inner_body   = body_exprs;
        int   inner_count  = body_count;
        for (int i = param_count - 1; i >= 0; i--) {
            ASTParam *p_single = malloc(sizeof(ASTParam));
            p_single[0] = params[i];
            AST **init_single = malloc(sizeof(AST*));
            init_single[0] = inits[i];
            AST *nested = build_let(p_single, 1, init_single,
                                    inner_body, inner_count,
                                    start_line, start_column);
            inner_body  = malloc(sizeof(AST*));
            inner_body[0] = nested;
            inner_count   = 1;
        }
        free(params);
        free(inits);
        return inner_body[0];
    }

    // Detect (define (fname params...) body) - short-form function definition
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "define") == 0) {

        // Peek ahead to see if it's (define (fname ...) or (define name ...)
        Token define_token = p->current;
        p->current = lexer_next_token(p->lexer);

        // Check if next token is '(' (function definition)
        if (p->current.type == TOK_LPAREN) {
            // Parse as (define (fname signature...) metadata? body)
            p->current = lexer_next_token(p->lexer); // consume '('

            // Get function name
            if (p->current.type != TOK_SYMBOL) {
                compiler_error(p->current.line, p->current.column,
                               "Expected function name after (define (");
            }
            AST *fname = ast_new_symbol(p->current.value);
            fname->line       = p->current.line;
            fname->column     = p->current.column;
            fname->end_column = p->current.column + strlen(p->current.value);
            p->current = lexer_next_token(p->lexer);

            // Parse signature
            ASTParam *params = NULL;
            int       count  = 0;
            char     *ret_type = NULL;
            parse_fn_signature(p, &params, &count, &ret_type);

            // Parse optional metadata BEFORE body
            DefineMetadata meta = parse_define_metadata(p);

            // Body
            /* AST *body = parse_expr(p); */
            AST **body_exprs = NULL;
            int   body_count = 0;

            // Pattern matching sugar: body does not start with (
            if (ret_type != NULL &&
                p->current.type != TOK_LPAREN &&
                p->current.type != TOK_RPAREN &&
                p->current.type != TOK_EOF) {
                AST *pm = parse_pmatch_clauses(p, count);
                // Desugar immediately at parse time — consistent with how
                // cond is desugared at parse time, so ast_clone and (code f)
                // always see the expanded if-chain, never raw AST_PMATCH.
                ASTParam *pm_params = params;
                AST *desugared = pmatch_desugar(pm, pm_params, count);
                ast_free(pm);
                body_exprs = malloc(sizeof(AST*));
                body_exprs[0] = desugared;
                body_count = 1;
            } else {
                while (p->current.type != TOK_RPAREN &&
                       p->current.type != TOK_EOF    &&
                       p->current.type != TOK_KEYWORD) {
                    body_exprs = realloc(body_exprs, sizeof(AST*) * (body_count + 1));
                    body_exprs[body_count++] = parse_expr(p);
                }
            }
            if (body_count == 0) {
                compiler_error(p->current.line, p->current.column,
                               "function body cannot be empty");
            }
            AST *body = body_exprs[body_count - 1];  // last expr = return value


            // Parse optional metadata AFTER body
            DefineMetadata meta2 = parse_define_metadata(p);
            if (meta2.docstring)  { free(meta.docstring);  meta.docstring  = meta2.docstring;  meta2.docstring  = NULL; }
            if (meta2.alias_name) { free(meta.alias_name); meta.alias_name = meta2.alias_name; meta2.alias_name = NULL; }
            if (meta2.naked_set)  { meta.naked = meta2.naked; meta.naked_set = true;}
            free(meta2.docstring);
            free(meta2.alias_name);

            // Expect closing ')' for the define
            if (p->current.type != TOK_RPAREN) {
                compiler_error(p->current.line, p->current.column,
                               "Expected ')' after define body");
            }
            int end_column = p->current.column + 1;
            p->current = lexer_next_token(p->lexer);

            // Validate naked functions have at least one (asm ...) block
            if (meta.naked) {
                bool has_asm = false;

                if (body->type == AST_ASM) {
                    has_asm = true;
                } else if (body->type == AST_LIST) {
                    for (size_t i = 0; i < body->list.count; i++) {
                        if (body->list.items[i]->type == AST_ASM) {
                            has_asm = true;
                            break;
                        }
                    }
                }

                if (!has_asm) {
                    compiler_error(body->line, body->column,
                                   "naked function '%s' must contain at least one "
                                   "(asm ...) block to manage the prologue and epilogue manually. ",
                                   fname->symbol);
                }
            }

            AST *lambda = ast_new_lambda(params, count, ret_type,
                                         meta.docstring, meta.alias_name, meta.naked,
                                         body, body_exprs, body_count);

            free(meta.docstring);
            free(meta.alias_name);

            lambda->line       = fname->line;
            lambda->column     = fname->column;
            lambda->end_column = end_column;

            AST *result = ast_new_list();
            ast_list_append(result, ast_new_symbol("define"));
            ast_list_append(result, fname);
            ast_list_append(result, lambda);

            result->line       = start_line;
            result->column     = start_column;
            result->end_column = end_column;

            ast_free(list);
            return result;

        } else {
            // (define name value [:doc ""] [:alias sym])
            AST *name_ast;

            if (p->current.type == TOK_LBRACKET) {
                int bracket_line = p->current.line;
                int bracket_col  = p->current.column;
                p->current = lexer_next_token(p->lexer); // consume '['
                if (p->current.type != TOK_SYMBOL)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected variable name in typed define");
                /* Build a list AST [name :: Type] so parse_type_annotation works */
                AST *bracket_list = ast_new_list();
                bracket_list->line   = bracket_line;
                bracket_list->column = bracket_col;
                name_ast = ast_new_symbol(p->current.value);
                name_ast->line   = p->current.line;
                name_ast->column = p->current.column;
                ast_list_append(bracket_list, name_ast);
                p->current = lexer_next_token(p->lexer); // consume name
                if (p->current.type == TOK_SYMBOL &&
                    strcmp(p->current.value, "::") == 0) {
                    ast_list_append(bracket_list, ast_new_symbol("::"));
                    p->current = lexer_next_token(p->lexer); // consume '::'
                    /* Collect full type annotation (may be multi-token: Pointer :: T) */
                    while (p->current.type == TOK_SYMBOL &&
                           p->current.type != TOK_RBRACKET) {
                        ast_list_append(bracket_list, ast_new_symbol(p->current.value));
                        p->current = lexer_next_token(p->lexer);
                        if (p->current.type != TOK_SYMBOL ||
                            strcmp(p->current.value, "::") != 0) break;
                        ast_list_append(bracket_list, ast_new_symbol("::"));
                        p->current = lexer_next_token(p->lexer); // consume '::'
                    }
                }
                if (p->current.type != TOK_RBRACKET)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected ']' to close typed variable binding");
                p->current = lexer_next_token(p->lexer); // consume ']'
                name_ast = bracket_list;
            } else if (p->current.type == TOK_SYMBOL) {
                name_ast = ast_new_symbol(p->current.value);
                name_ast->line   = p->current.line;
                name_ast->column = p->current.column;
                p->current = lexer_next_token(p->lexer);
            } else {
                compiler_error(p->current.line, p->current.column,
                               "Expected variable name after define");
            }

            // Parse the value FIRST — no pre-value metadata scan
            AST *value_ast = parse_expr(p);

            // THEN parse metadata after the value
            DefineMetadata meta = parse_define_metadata(p);

            // Also accept a bare string as docstring (wisp style)
            if (!meta.docstring &&
                p->current.type == TOK_STRING) {
                meta.docstring = strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
            }

            if (p->current.type != TOK_RPAREN) {
                compiler_error(p->current.line, p->current.column,
                               "Expected ')' to close define");
            }
            int end_col = p->current.column + 1;
            p->current = lexer_next_token(p->lexer);

            AST *result = ast_new_list();
            ast_list_append(result, ast_new_symbol("define"));
            ast_list_append(result, name_ast);
            ast_list_append(result, value_ast);

            if (meta.docstring || meta.alias_name) {
                ast_list_append(result,
                                meta.docstring  ? ast_new_string(meta.docstring)
                                : ast_new_symbol("__no_doc__"));
                ast_list_append(result,
                                meta.alias_name ? ast_new_symbol(meta.alias_name)
                                : ast_new_symbol("__no_alias__"));
            }

            free(meta.docstring);
            free(meta.alias_name);

            ast_free(list);
            result->line       = start_line;
            result->column     = start_column;
            result->end_column = end_col;
            return result;
        }
    }

    // Detect (asm mnemonic operand1 operand2 ...)
    // Flat syntax: all tokens are part of one instruction line
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "asm") == 0) {

        int asm_start_line = p->current.line;
        int asm_start_column = p->current.column;

        p->current = lexer_next_token(p->lexer);

        // Collect all tokens as a single flat instruction
        AST *inst_list = ast_new_list();

        while (p->current.type != TOK_RPAREN &&
               p->current.type != TOK_EOF) {

            AST *token = parse_expr(p);
            ast_list_append(inst_list, token);
        }

        if (p->current.type != TOK_RPAREN) {
            compiler_error(p->current.line, p->current.column,
                         "Expected ')' to close asm block");
        }

        int end_column = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        // Wrap the single instruction in an array
        AST **instructions = malloc(sizeof(AST *));
        instructions[0] = inst_list;

        AST *asm_node = ast_new_asm(instructions, 1);
        asm_node->line = asm_start_line;
        asm_node->column = asm_start_column;
        asm_node->end_column = end_column;

        ast_free(list);
        return asm_node;
    }

    // Type alias or refinement type
    // (type Code List)                        — simple alias
    // (type Positive { x ∈ Int | (> x 0) })  — refinement type
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "type") == 0) {

        int tline = p->current.line, tcol = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'type'

        if (p->current.type != TOK_SYMBOL) {
            compiler_error(p->current.line, p->current.column,
                           "Expected type name after 'type'");
        }
        char *type_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        /* Refinement type: { x ∈ Base | predicate }
         * parse_expr → parse_set → parse_refinement_expr handles the {}
         * we just need to extract var/base/pred from the result          */
        if (p->current.type == TOK_LBRACE) {
            AST *ref = parse_expr(p); /* parse_set detects ∈ and returns AST_REFINEMENT */
            if (!ref || ref->type != AST_REFINEMENT) {
                compiler_error(p->current.line, p->current.column,
                               "Expected refinement type { x ∈ T | pred }");
            }
            /* set the name on the anonymous refinement */
            free(ref->refinement.name);
            ref->refinement.name = my_strdup(type_name);
            free(type_name);

            /* Optional metadata after {}: docstring and/or keywords */
            char *docstring  = NULL;
            char *alias_name = NULL;
            if (p->current.type == TOK_STRING) {
                docstring  = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
            }

            while (p->current.type == TOK_KEYWORD) {
                if (strcmp(p->current.value, "doc") == 0) {
                    p->current = lexer_next_token(p->lexer);
                    if (p->current.type == TOK_STRING) {
                        free(docstring);
                        docstring  = my_strdup(p->current.value);
                        p->current = lexer_next_token(p->lexer);
                    }
                } else if (strcmp(p->current.value, "alias") == 0) {
                    p->current = lexer_next_token(p->lexer);
                    if (p->current.type == TOK_SYMBOL) {
                        free(alias_name);
                        alias_name = my_strdup(p->current.value);
                        p->current = lexer_next_token(p->lexer);
                    }
                } else break;
            }
            ref->refinement.docstring  = docstring;
            ref->refinement.alias_name = alias_name;

            if (p->current.type != TOK_RPAREN)
                compiler_error(p->current.line, p->current.column,
                               "Expected ')' to close type definition");
            int end_col = p->current.column + 1;
            p->current = lexer_next_token(p->lexer);

            ast_free(list);
            ref->line = tline; ref->column = tcol;
            ref->end_column = end_col;
            return ref;
        }

        /* Simple alias: (type Code List) — sugar for a refinement with no predicate */
        if (p->current.type != TOK_SYMBOL) {
            compiler_error(p->current.line, p->current.column,
                           "Expected target type name or '{' after type name");
        }
        char *target_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        /* Consume optional metadata: docstring and/or :doc/:alias */
        char *alias_doc  = NULL;
        char *alias_name = NULL;
        if (p->current.type == TOK_STRING) {
            alias_doc  = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
        }
        while (p->current.type == TOK_KEYWORD) {
            if (strcmp(p->current.value, "doc") == 0) {
                p->current = lexer_next_token(p->lexer);
                if (p->current.type == TOK_STRING) {
                    free(alias_doc);
                    alias_doc  = my_strdup(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                }
            } else if (strcmp(p->current.value, "alias") == 0) {
                p->current = lexer_next_token(p->lexer);
                if (p->current.type == TOK_SYMBOL) {
                    free(alias_name);
                    alias_name = my_strdup(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                }
            } else break;
        }

        if (p->current.type != TOK_RPAREN) {
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' to close type alias");
        }
        int end_col = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        /* Represent as AST_REFINEMENT with NULL predicate = pure alias */
        AST *node = ast_new_refinement(type_name, NULL, target_name,
                                       NULL, alias_doc, alias_name);
        free(type_name); free(target_name);
        free(alias_doc); free(alias_name);
        node->line = tline; node->column = tcol;
        node->end_column = end_col;
        return node;
    }

    // (class ClassName typevar where
    //     method :: type
    //     (default x y) => body)
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "class") == 0) {

        int cls_line = p->current.line, cls_col = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'class'

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected class name after 'class'");
        char *cls_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected type variable after class name");
        char *type_var = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        /* consume 'where' */
        if (p->current.type == TOK_SYMBOL &&
            strcmp(p->current.value, "where") == 0)
            p->current = lexer_next_token(p->lexer);

        char **method_names   = NULL;
        char **method_types   = NULL;
        int    method_count   = 0;
        char **default_names  = NULL;
        AST  **default_bodies = NULL;
        int    default_count  = 0;

        while (p->current.type != TOK_RPAREN &&
               p->current.type != TOK_EOF) {

            /* Method signature: (=) :: a -> a -> Bool
             * Must check for (method) :: pattern BEFORE checking
             * for default impls — peek ahead for '::' after the paren group */
            if (p->current.type == TOK_LPAREN) {
                /* Save position to distinguish (method) :: sig from (pat) => body */
                Lexer saved_lex = *p->lexer;
                Token saved_cur = p->current;

                p->current = lexer_next_token(p->lexer); // consume '('
                char *peek_name = NULL;
                if (p->current.type == TOK_SYMBOL) {
                    peek_name = my_strdup(p->current.value);
                    p->current = lexer_next_token(p->lexer); // consume name
                }
                bool is_sig = (p->current.type == TOK_RPAREN);
                if (is_sig) {
                    p->current = lexer_next_token(p->lexer); // consume ')'
                    /* Now check for '::' */
                    is_sig = (p->current.type == TOK_SYMBOL &&
                              strcmp(p->current.value, "::") == 0);
                }

                if (is_sig && peek_name) {
                    /* It's a method signature: (=) :: type */
                    p->current = lexer_next_token(p->lexer); // consume '::'

                    char type_buf[512] = {0};
                    while (p->current.type != TOK_RPAREN &&
                           p->current.type != TOK_EOF    &&
                           p->current.type != TOK_LPAREN) {
                        /* Stop if we see symbol followed by '::' — next method */
                        Token cur = p->current;
                        p->current = lexer_next_token(p->lexer);
                        if (p->current.type == TOK_SYMBOL &&
                            strcmp(p->current.value, "::") == 0) {
                            /* cur was a method name — don't include it */
                            /* restore: put cur back as current */
                            /* We can't push back, so just stop here */
                            /* The outer loop will pick up cur next iteration */
                            /* Trick: manually set current to cur and break */
                            p->current = cur;
                            break;
                        }
                        const char *tok_str = cur.value ? cur.value
                                            : (cur.type == TOK_ARROW ? "->" : NULL);
                        if (tok_str) {
                            if (type_buf[0]) strncat(type_buf, " ",
                                sizeof(type_buf) - strlen(type_buf) - 1);
                            strncat(type_buf, tok_str,
                                sizeof(type_buf) - strlen(type_buf) - 1);
                        }
                        /* Also add '->' tokens */
                        if (cur.type == TOK_ARROW) {
                            /* already added "->", continue */
                        }
                    }

                    method_names = realloc(method_names,
                                           sizeof(char*) * (method_count + 1));
                    method_types = realloc(method_types,
                                           sizeof(char*) * (method_count + 1));
                    method_names[method_count] = peek_name;
                    method_types[method_count] = my_strdup(type_buf);
                    method_count++;
                    continue;
                }

                /* Not a signature — restore and parse as default impl */
                free(peek_name);
                *p->lexer  = saved_lex;
                p->current = saved_cur;
            }

            /* Default implementation: (x `!=` y) => body */
            if (p->current.type == TOK_LPAREN) {
                p->current = lexer_next_token(p->lexer); // consume '('

                char *def_method = NULL;
                AST *pattern_arg1 = NULL;
                AST *pattern_arg2 = NULL;

                if (p->current.type == TOK_SYMBOL) {
                    pattern_arg1 = parse_expr(p);
                    if (p->current.type == TOK_SYMBOL) {
                        /* infix: (pat1 method pat2) */
                        def_method = my_strdup(p->current.value);
                        p->current = lexer_next_token(p->lexer);
                        pattern_arg2 = parse_expr(p);
                    } else {
                        if (pattern_arg1->type == AST_SYMBOL)
                            def_method = my_strdup(pattern_arg1->symbol);
                        while (p->current.type != TOK_RPAREN &&
                               p->current.type != TOK_EOF) {
                            ast_free(pattern_arg2);
                            pattern_arg2 = parse_expr(p);
                        }
                    }
                }

                if (p->current.type == TOK_RPAREN)
                    p->current = lexer_next_token(p->lexer);

                if (p->current.type == TOK_SYMBOL &&
                    strcmp(p->current.value, "=>") == 0)
                    p->current = lexer_next_token(p->lexer);

                AST *def_body = parse_expr(p);

                if (def_method) {
                    ASTParam *dparams = malloc(sizeof(ASTParam) * 2);
                    dparams[0].name = (pattern_arg1 &&
                                       pattern_arg1->type == AST_SYMBOL)
                                    ? my_strdup(pattern_arg1->symbol)
                                    : my_strdup("__x");
                    dparams[0].type_name = NULL;
                    dparams[0].is_rest   = false;
                    dparams[0].is_anon   = false;
                    dparams[1].name = (pattern_arg2 &&
                                       pattern_arg2->type == AST_SYMBOL)
                                    ? my_strdup(pattern_arg2->symbol)
                                    : my_strdup("__y");
                    dparams[1].type_name = NULL;
                    dparams[1].is_rest   = false;
                    dparams[1].is_anon   = false;

                    AST **dbody_exprs = malloc(sizeof(AST*));
                    dbody_exprs[0]    = def_body;
                    AST *def_lam = ast_new_lambda(dparams, 2, NULL, NULL, NULL,
                                                  false, def_body,
                                                  dbody_exprs, 1);
                    default_names  = realloc(default_names,
                                     sizeof(char*) * (default_count + 1));
                    default_bodies = realloc(default_bodies,
                                     sizeof(AST*)  * (default_count + 1));
                    default_names[default_count]  = def_method;
                    default_bodies[default_count] = def_lam;
                    default_count++;
                } else {
                    ast_free(def_body);
                }
                ast_free(pattern_arg1);
                ast_free(pattern_arg2);
                continue;
            }

            /* Bare symbol method: = :: a -> a -> Bool (without parens) */
            if (p->current.type == TOK_SYMBOL) {
                char *mname = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);

                if (p->current.type == TOK_SYMBOL &&
                    strcmp(p->current.value, "::") == 0) {
                    p->current = lexer_next_token(p->lexer);

                    char type_buf[512] = {0};
                    while (p->current.type != TOK_RPAREN &&
                           p->current.type != TOK_EOF    &&
                           p->current.type != TOK_LPAREN) {
                        Token cur = p->current;
                        p->current = lexer_next_token(p->lexer);
                        /* Stop before next method name */
                        if (p->current.type == TOK_SYMBOL &&
                            strcmp(p->current.value, "::") == 0) {
                            p->current = cur;
                            break;
                        }
                        if (cur.value) {
                            if (type_buf[0]) strncat(type_buf, " ",
                                sizeof(type_buf) - strlen(type_buf) - 1);
                            strncat(type_buf, cur.value,
                                sizeof(type_buf) - strlen(type_buf) - 1);
                        }
                    }
                    method_names = realloc(method_names,
                                           sizeof(char*) * (method_count + 1));
                    method_types = realloc(method_types,
                                           sizeof(char*) * (method_count + 1));
                    method_names[method_count] = mname;
                    method_types[method_count] = my_strdup(type_buf);
                    method_count++;
                } else {
                    free(mname);
                }
                continue;
            }

            /* Skip anything unexpected */
            p->current = lexer_next_token(p->lexer);
        }

        if (p->current.type == TOK_RPAREN)
            p->current = lexer_next_token(p->lexer);

        ast_free(list);
        AST *node = ast_new_class(cls_name, type_var,
                                   method_names, method_types, method_count,
                                   default_names, default_bodies, default_count);
        free(cls_name); free(type_var);
        node->line = cls_line; node->column = cls_col;
        return node;
    }

    // (instance ClassName TypeName where
    //     (pattern) => body ...)
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "instance") == 0) {

        int ins_line = p->current.line, ins_col = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'instance'

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected class name after 'instance'");
        char *cls_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected type name after class name");
        char *type_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        /* consume 'where' */
        if (p->current.type == TOK_SYMBOL &&
            strcmp(p->current.value, "where") == 0)
            p->current = lexer_next_token(p->lexer);

        /* Collect all clauses grouped by method name.
         * Each clause: (pat1 `op` pat2) => body
         * or           (ctor `op` ctor2) => body               */

        /* We accumulate clauses per method, then build lambdas  */
        typedef struct {
            char *method;
            ASTPMatchClause *clauses;
            int clause_count;
            int clause_cap;
        } MethodClauses;

        MethodClauses *methods = NULL;
        int nethods = 0;

        while (p->current.type != TOK_RPAREN &&
               p->current.type != TOK_EOF) {

            if (p->current.type != TOK_LPAREN)
                compiler_error(p->current.line, p->current.column,
                               "Expected '(' to start instance method clause");

            p->current = lexer_next_token(p->lexer); // consume '('

            /* Parse pattern: could be (Pat1 `op` Pat2) or (Pat1 Pat2) */
            /* We need to identify the method name and the patterns      */

            /* Use a mini-parser: collect tokens, find method name       */
            /* Strategy: parse as pmatch pattern using existing machinery */

            /* First: detect method name */
            char *method_name = NULL;
            ASTPattern pat1 = {0}, pat2 = {0};
            bool has_pat2 = false;

            /* Pattern token 1 */
            /* Re-use parse_single_pattern from pmatch via the Parser    */
            /* We'll build a temporary Parser around the current lexer   */
            Parser pm_parser;
            pm_parser.lexer   = p->lexer;
            pm_parser.current = p->current;

            /* Parse first pattern */
            pat1 = parse_single_pattern(&pm_parser);
            p->current = pm_parser.current;

            if (p->current.type == TOK_SYMBOL &&
                p->current.value && pat1.kind != PAT_CONSTRUCTOR) {
                /* infix: pat1 method pat2 */
                method_name = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer); // consume method
                pm_parser.current = p->current;
                pat2 = parse_single_pattern(&pm_parser);
                p->current = pm_parser.current;
                has_pat2 = true;
            } else if (p->current.type == TOK_SYMBOL &&
                       pat1.kind == PAT_CONSTRUCTOR) {
                /* prefix: (method pat1 pat2) — first token was method */
                method_name = my_strdup(pat1.var_name);
                /* pat1 was actually the method name, pat2 is first real pat */
                pat1 = (ASTPattern){0};
                pm_parser.current = p->current;
                pat1 = parse_single_pattern(&pm_parser);
                p->current = pm_parser.current;
                if (p->current.type != TOK_RPAREN) {
                    pm_parser.current = p->current;
                    pat2 = parse_single_pattern(&pm_parser);
                    p->current = pm_parser.current;
                    has_pat2 = true;
                }
            }

            if (p->current.type == TOK_RPAREN)
                p->current = lexer_next_token(p->lexer); // consume ')'

            /* consume '=>' */
            if (p->current.type == TOK_SYMBOL &&
                strcmp(p->current.value, "=>") == 0)
                p->current = lexer_next_token(p->lexer);

            AST *body = parse_expr(p);

            if (!method_name) {
                ast_free(body);
                continue;
            }

            /* Find or create method entry */
            int mi = -1;
            for (int i = 0; i < nethods; i++) {
                if (strcmp(methods[i].method, method_name) == 0) {
                    mi = i; break;
                }
            }
            if (mi < 0) {
                methods = realloc(methods, sizeof(MethodClauses) * (nethods + 1));
                methods[nethods].method      = my_strdup(method_name);
                methods[nethods].clauses     = NULL;
                methods[nethods].clause_count = 0;
                methods[nethods].clause_cap   = 0;
                mi = nethods++;
            }
            free(method_name);

            /* Add clause */
            MethodClauses *mc = &methods[mi];
            if (mc->clause_count >= mc->clause_cap) {
                mc->clause_cap = mc->clause_cap == 0 ? 4 : mc->clause_cap * 2;
                mc->clauses = realloc(mc->clauses,
                                      sizeof(ASTPMatchClause) * mc->clause_cap);
            }
            ASTPMatchClause *cl = &mc->clauses[mc->clause_count++];
            int npats = has_pat2 ? 2 : 1;
            cl->patterns      = malloc(sizeof(ASTPattern) * npats);
            cl->patterns[0]   = pat1;
            if (has_pat2) cl->patterns[1] = pat2;
            cl->pattern_count = npats;
            cl->body          = body;
        }

        if (p->current.type == TOK_RPAREN)
            p->current = lexer_next_token(p->lexer);

        /* Build method lambdas from collected clauses */
        char **inst_method_names  = malloc(sizeof(char*) * (nethods ? nethods : 1));
        AST  **inst_method_bodies = malloc(sizeof(AST*)  * (nethods ? nethods : 1));

        for (int mi = 0; mi < nethods; mi++) {
            MethodClauses *mc = &methods[mi];
            inst_method_names[mi] = my_strdup(mc->method);

            /* Create params for the lambda — use generic names */
            int npats = mc->clauses[0].pattern_count;
            ASTParam *params = malloc(sizeof(ASTParam) * npats);
            for (int pi = 0; pi < npats; pi++) {
                char pname[32];
                snprintf(pname, sizeof(pname), "__inst_p%d", pi);
                params[pi].name      = my_strdup(pname);
                params[pi].type_name = NULL;
                params[pi].is_rest   = false;
                params[pi].is_anon   = false;
            }

            /* Desugar pmatch clauses */
            AST *pm = ast_new_pmatch(mc->clauses, mc->clause_count);
            AST *desugared = pmatch_desugar(pm, params, npats);
            /* Do NOT ast_free(pm) — pmatch_desugar takes ownership of
             * clause bodies. Just free the container without freeing
             * the clauses themselves.                                  */
            free(pm); /* free only the AST node, not via ast_free */

            AST **body_exprs = malloc(sizeof(AST*));
            body_exprs[0] = desugared;
            inst_method_bodies[mi] = ast_new_lambda(params, npats, NULL, NULL,
                                                     NULL, false, desugared,
                                                     body_exprs, 1);
            free(mc->method);
            /* clauses array freed but not the contents — owned by desugared */
            free(mc->clauses);
        }
        free(methods);

        ast_free(list);
        AST *node = ast_new_instance(cls_name, type_name,
                                     inst_method_names, inst_method_bodies,
                                     nethods);
        free(cls_name); free(type_name);
        node->line = ins_line; node->column = ins_col;
        return node;
    }

    // (data Name Ctor1 Type... | Ctor2 Type... deriving [show eq])
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "data") == 0) {

        int dat_line = p->current.line, dat_col = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'data'

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected type name after ‘data’");
        char *dat_name = my_strdup(p->current.value);
        if (dat_name[0] < 'A' || dat_name[0] > 'Z')
            compiler_error(p->current.line, p->current.column,
                           "data type name ‘%s’ must start with an uppercase letter "
                           "(e.g. ‘data %c%s’ instead of ‘data %s’)",
                           dat_name,
                           (char)(dat_name[0] - 32), dat_name + 1,
                           dat_name);
        p->current = lexer_next_token(p->lexer);

        ASTDataConstructor *ctors   = NULL;
        int                 nctors  = 0;
        char              **deriving = NULL;
        int                 nderiving = 0;

        /* Shorthand: data Point Float Float
         * If the first token after the type name is an uppercase known type
         * (Float, Int, Bool, etc.) with no constructor name, treat as single
         * constructor sharing the type name.                                */
        {
            bool is_shorthand = false;
            if (p->current.type == TOK_SYMBOL && p->current.value &&
                p->current.value[0] >= 'A' && p->current.value[0] <= 'Z') {
                /* Peek: if this looks like a type name (known primitive) rather
                 * than a constructor name, use shorthand.
                 * Heuristic: if it's a known primitive type OR if there's no |
                 * separator and all remaining tokens until ) are uppercase,
                 * treat as shorthand single-constructor.                      */
                const char *known_types[] = {
                    "Float", "Int", "Bool", "Char", "String",
                    "F32", "I8", "U8", "I16", "U16", "I32", "U32",
                    "I64", "U64", "I128", "U128", NULL
                };
                for (int _ki = 0; known_types[_ki]; _ki++) {
                    if (strcmp(p->current.value, known_types[_ki]) == 0) {
                        is_shorthand = true;
                        break;
                    }
                }
                /* Also shorthand if the first token matches a previously
                 * defined data type name (e.g. data Triangle Point Point Point) */
                /* We can't check env here, so use a simpler rule:
                 * if no | appears before ) then it's shorthand             */
                if (!is_shorthand) {
                    /* Scan ahead for | — if none found, it's shorthand */
                    /* We can't easily scan ahead with this lexer, so use
                     * the known_types check only for now. User-defined types
                     * as fields still need explicit constructor name.
                     * TODO: improve with lookahead                          */
                }
            }

            if (is_shorthand) {
                /* Single constructor, same name as type, fields are all remaining tokens */
                ASTDataConstructor ctor = {0};
                ctor.name = my_strdup(dat_name); /* constructor = type name */
                while (p->current.type != TOK_RPAREN &&
                       p->current.type != TOK_EOF    &&
                       !(p->current.type == TOK_SYMBOL &&
                         p->current.value &&
                         strcmp(p->current.value, "|") == 0)) {
                    if (p->current.type == TOK_SYMBOL && p->current.value) {
                        ctor.field_types = realloc(ctor.field_types,
                            sizeof(char*) * (ctor.field_count + 1));
                        ctor.field_types[ctor.field_count++] =
                            my_strdup(p->current.value);
                    }
                    p->current = lexer_next_token(p->lexer);
                }
                ctors = realloc(ctors, sizeof(ASTDataConstructor) * (nctors + 1));
                ctors[nctors++] = ctor;
            }
        }

        /* Parse constructors separated by | */
        while (p->current.type != TOK_RPAREN &&
               p->current.type != TOK_EOF) {

            /* skip leading | */
            if (p->current.type == TOK_SYMBOL &&
                p->current.value &&
                strcmp(p->current.value, "|") == 0) {
                p->current = lexer_next_token(p->lexer);
                continue;
            }

            /* deriving Eq  or  deriving [Eq Show ...] */
            if (p->current.type == TOK_SYMBOL &&
                strcmp(p->current.value, "deriving") == 0) {
                p->current = lexer_next_token(p->lexer);
                if (p->current.type == TOK_LBRACKET) {
                    /* deriving [Eq Show ...] */
                    p->current = lexer_next_token(p->lexer); // consume '['
                    while (p->current.type != TOK_RBRACKET &&
                           p->current.type != TOK_EOF) {
                        if (p->current.type == TOK_SYMBOL && p->current.value) {
                            deriving = realloc(deriving, sizeof(char*) * (nderiving + 1));
                            deriving[nderiving++] = my_strdup(p->current.value);
                        }
                        p->current = lexer_next_token(p->lexer);
                    }
                    if (p->current.type == TOK_RBRACKET)
                        p->current = lexer_next_token(p->lexer); // consume ']'
                } else if (p->current.type == TOK_SYMBOL && p->current.value) {
                    /* deriving Eq — single, no brackets */
                    deriving = realloc(deriving, sizeof(char*) * (nderiving + 1));
                    deriving[nderiving++] = my_strdup(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                } else {
                    compiler_error(p->current.line, p->current.column,
                                   "'deriving' requires at least one typeclass — "
                                   "use 'deriving Eq' or 'deriving [Eq Show ...]'");
                }
                continue;
            }


            /* Constructor name — must start with uppercase */
            if (p->current.type != TOK_SYMBOL || !p->current.value)
                compiler_error(p->current.line, p->current.column,
                               "Expected constructor name in ‘data %s’", dat_name);
            if (p->current.value[0] < 'A' || p->current.value[0] > 'Z')
                compiler_error(p->current.line, p->current.column,
                               "constructor name ‘%s’ in ‘data %s’ must start with "
                               "an uppercase letter — did you mean ‘%c%s’?",
                               p->current.value, dat_name,
                               (char)(p->current.value[0] >= 'a' && p->current.value[0] <= 'z'
                                      ? p->current.value[0] - 32
                                      : p->current.value[0]),
                               p->current.value + 1);

            ASTDataConstructor ctor = {0};
            ctor.name = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);

            /* Collect field types until | or ) or 'deriving' */
            while (!(p->current.type == TOK_SYMBOL &&
                     p->current.value &&
                     strcmp(p->current.value, "|") == 0) &&
                   p->current.type != TOK_RPAREN   &&
                   p->current.type != TOK_EOF      &&
                   !(p->current.type == TOK_SYMBOL &&
                     p->current.value &&
                     strcmp(p->current.value, "deriving") == 0)) {
                if (p->current.type == TOK_SYMBOL && p->current.value) {
                    ctor.field_types = realloc(ctor.field_types,
                        sizeof(char*) * (ctor.field_count + 1));
                    ctor.field_types[ctor.field_count++] =
                        my_strdup(p->current.value);
                }
                p->current = lexer_next_token(p->lexer);
            }

            ctors = realloc(ctors, sizeof(ASTDataConstructor) * (nctors + 1));
            ctors[nctors++] = ctor;
        }

        if (p->current.type != TOK_RPAREN)
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' to close 'data' definition");
        int end_col = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        AST *node = ast_new_data(dat_name, ctors, nctors, deriving, nderiving);
        free(dat_name);
        node->line       = dat_line;
        node->column     = dat_col;
        node->end_column = end_col;
        return node;
    }

    // (layout Name [field :: Type] ... :packed True/False :align N)
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "layout") == 0) {

        int lay_line = p->current.line, lay_col = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'layout'

        if (p->current.type != TOK_SYMBOL)
            compiler_error(p->current.line, p->current.column,
                           "Expected struct name after 'layout'");
        char *lay_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        ASTLayoutField *fields   = NULL;
        int             nfields  = 0;
        bool            packed   = false;
        int             align    = 0;

        while (p->current.type != TOK_RPAREN &&
               p->current.type != TOK_EOF) {

            // :packed True/False
            if (p->current.type == TOK_KEYWORD &&
                strcmp(p->current.value, "packed") == 0) {
                p->current = lexer_next_token(p->lexer);
                if (p->current.type != TOK_SYMBOL ||
                    (strcmp(p->current.value, "True")  != 0 &&
                     strcmp(p->current.value, "False") != 0))
                    compiler_error(p->current.line, p->current.column,
                                   ":packed requires True or False");
                packed = (strcmp(p->current.value, "True") == 0);
                p->current = lexer_next_token(p->lexer);
                continue;
            }

            // :align N
            if (p->current.type == TOK_KEYWORD &&
                strcmp(p->current.value, "align") == 0) {
                p->current = lexer_next_token(p->lexer);
                if (p->current.type != TOK_NUMBER)
                    compiler_error(p->current.line, p->current.column,
                                   ":align requires a number");
                align = (int)atof(p->current.value);
                p->current = lexer_next_token(p->lexer);
                continue;
            }

            // [field :: Type]  or  [field :: [ElemType Size]]
            if (p->current.type != TOK_LBRACKET)
                compiler_error(p->current.line, p->current.column,
                               "Expected '[' for layout field, got '%s'",
                               p->current.value ? p->current.value : "?");
            p->current = lexer_next_token(p->lexer); // consume '['

            if (p->current.type != TOK_SYMBOL)
                compiler_error(p->current.line, p->current.column,
                               "Expected field name");
            char *fname = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);

            // expect ::
            if (p->current.type != TOK_SYMBOL ||
                strcmp(p->current.value, "::") != 0)
                compiler_error(p->current.line, p->current.column,
                               "Expected '::' after field name '%s'", fname);
            p->current = lexer_next_token(p->lexer);

            ASTLayoutField field = {0};
            field.name       = fname;
            field.array_size = -1;

            // Array sugar: [ElemType Size]
            if (p->current.type == TOK_LBRACKET) {
                p->current = lexer_next_token(p->lexer); // consume inner '['
                if (p->current.type != TOK_SYMBOL)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected element type in array field");
                field.is_array   = true;
                field.array_elem = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);

                if (p->current.type == TOK_NUMBER) {
                    field.array_size = (int)atof(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                }
                if (p->current.type != TOK_RBRACKET)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected ']' to close array type");
                p->current = lexer_next_token(p->lexer); // consume inner ']'
            } else {
                // Plain type name
                if (p->current.type != TOK_SYMBOL)
                    compiler_error(p->current.line, p->current.column,
                                   "Expected type name for field '%s'", fname);
                field.type_name = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
            }

            // consume outer ']'
            if (p->current.type != TOK_RBRACKET)
                compiler_error(p->current.line, p->current.column,
                               "Expected ']' to close field '%s'", fname);
            p->current = lexer_next_token(p->lexer);

            fields = realloc(fields, sizeof(ASTLayoutField) * (nfields + 1));
            fields[nfields++] = field;
        }

        if (p->current.type != TOK_RPAREN)
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' to close layout '%s'", lay_name);
        int end_col = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        AST *node = ast_new_layout(lay_name, fields, nfields, packed, align);
        free(lay_name);
        node->line       = lay_line;
        node->column     = lay_col;
        node->end_column = end_col;
        return node;
    }

    /* (include <stdio.h>) or (include "myheader.h") */
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "include") == 0) {
        int inc_line = p->current.line;
        int inc_col  = p->current.column;
        p->current = lexer_next_token(p->lexer); /* consume 'include' */

        /* Read the header name — could be a symbol like <stdio.h>
         * which the lexer tokenises as a comparison chain, or a string */
        bool system_include = false;
        char header_name[256] = {0};

        if (p->current.type == TOK_STRING) {
            /* "myheader.h" */
            strncpy(header_name, p->current.value, sizeof(header_name) - 1);
            system_include = false;
            p->current = lexer_next_token(p->lexer);
        } else if (p->current.type == TOK_SYMBOL &&
                   p->current.value[0] == '<') {
            /* <stdio.h> tokenised as a single symbol by some lexers */
            const char *s = p->current.value;
            size_t len = strlen(s);
            if (s[len-1] == '>') {
                /* strip < and > */
                strncpy(header_name, s + 1, len - 2);
                header_name[len - 2] = '\0';
            } else {
                strncpy(header_name, s + 1, sizeof(header_name) - 1);
            }
            system_include = true;
            p->current = lexer_next_token(p->lexer);
        } else {
            /* Fallback: collect tokens until ) building the header name */
            /* This handles <stdio.h> that got split across tokens */
            char buf[256] = {0};
            while (p->current.type != TOK_RPAREN &&
                   p->current.type != TOK_EOF) {
                if (p->current.value) {
                    strncat(buf, p->current.value,
                            sizeof(buf) - strlen(buf) - 1);
                }
                p->current = lexer_next_token(p->lexer);
            }
            /* strip < > if present */
            if (buf[0] == '<') {
                size_t blen = strlen(buf);
                if (buf[blen-1] == '>') buf[blen-1] = '\0';
                strncpy(header_name, buf + 1, sizeof(header_name) - 1);
                system_include = true;
            } else {
                strncpy(header_name, buf, sizeof(header_name) - 1);
                system_include = false;
            }
        }

        /* Parse optional :unprefix PREFIX1 PREFIX2 ... */
        char *unprefix_strs[16] = {0};
        int   unprefix_count    = 0;
        if (p->current.type == TOK_KEYWORD &&
            strcmp(p->current.value, "unprefix") == 0) {
            p->current = lexer_next_token(p->lexer); /* consume :unprefix */
            while (p->current.type == TOK_SYMBOL &&
                   p->current.type != TOK_RPAREN &&
                   p->current.type != TOK_EOF &&
                   unprefix_count < 16) {
                unprefix_strs[unprefix_count++] = strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
            }
        }
        if (p->current.type == TOK_RPAREN)
            p->current = lexer_next_token(p->lexer);
        /* Build (include "header_name" system_flag "pfx1" "pfx2" ...) */
        AST *result = ast_new_list();
        ast_list_append(result, ast_new_symbol("include"));
        ast_list_append(result, ast_new_string(header_name));
        ast_list_append(result, ast_new_symbol(system_include ? "system" : "local"));
        for (int i = 0; i < unprefix_count; i++) {
            ast_list_append(result, ast_new_string(unprefix_strs[i]));
            free(unprefix_strs[i]);
        }
        ast_free(list);
        result->line   = inc_line;
        result->column = inc_col;
        return result;
    }

    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "tests") == 0) {
        p->current = lexer_next_token(p->lexer);

        AST *node = calloc(1, sizeof(AST));
        node->type = AST_TESTS;
        node->tests.assertions = NULL;
        node->tests.count = 0;

        while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
            AST *assertion = parse_expr(p);
            node->tests.count++;
            node->tests.assertions = realloc(node->tests.assertions,
                                             sizeof(AST*) * node->tests.count);
            node->tests.assertions[node->tests.count - 1] = assertion;
        }

        if (p->current.type != TOK_RPAREN) {
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' to close tests block");
        }
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        node->line = start_line;
        node->column = start_column;
        return node;
    }


    // Normal list — but check for range after first element
    // (start..end) or (start,step..end) or (start..)
    {
        // peek: is first non-whitespace token a number/symbol followed by .. ?
        // We parse first element then check
        if (p->current.type == TOK_RPAREN || p->current.type == TOK_EOF) {
            /* empty list () */
        } else {
            AST *first = parse_expr(p);

            /* step syntax: (1,3..10) — comma between start and step */
            AST *step = NULL;
            /* comma is lexed as a symbol in your lexer */
            if (p->current.type == TOK_SYMBOL &&
                strcmp(p->current.value, ",") == 0) {
                p->current = lexer_next_token(p->lexer);
                step = parse_expr(p);
            }

            if (p->current.type == TOK_DOTDOT) {
                /* RANGE */
                p->current = lexer_next_token(p->lexer);
                AST *end = NULL;
                if (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF)
                    end = parse_expr(p);
                if (p->current.type != TOK_RPAREN)
                    compiler_error(p->current.line, p->current.column, "Expected ')'");
                int end_col = p->current.column + 1;
                p->current = lexer_next_token(p->lexer);
                ast_free(list);
                AST *node = ast_new_range(first, step, end, false);
                node->line = start_line; node->column = start_column;
                node->end_column = end_col;
                return node;
            }

            /* Not a range — proceed as normal list with first already parsed */

            /* ── Automatic infix detection ─────────────────────────────────
             * (a f b) => (f a b)  when:
             *   - next token is a symbol (candidate infix op)
             *   - the token after that is NOT ')' or EOF (so f has a rhs)
             *   - first is NOT a known callable (already a prefix call head)
             *   - the head `first` does NOT expect a function at the next slot
             * Chains left-to-right: (a f b g c) => (g (f a b) c)          */
            bool first_is_known_fn = false;
            if (first->type == AST_SYMBOL && g_is_known_function)
                first_is_known_fn = g_is_known_function(first->symbol);

            bool candidate_is_known_fn = true;
            if (p->current.type == TOK_SYMBOL && p->current.value && g_is_known_function)
                candidate_is_known_fn = g_is_known_function(p->current.value);

            if (!first_is_known_fn && p->current.type == TOK_SYMBOL && p->current.value && candidate_is_known_fn) {
                /* peek: is there a rhs after the candidate? */
                /* We need at least one more token that isn't ')' */
                /* Strategy: tentatively check using saved lexer state */
                Lexer saved_lex = *p->lexer;
                Token candidate  = p->current;
                Token after      = lexer_next_token(p->lexer);
                *p->lexer        = saved_lex; /* restore — we only peeked */

                bool has_rhs = (after.type != TOK_RPAREN &&
                                after.type != TOK_EOF);
                free(after.value);

                /* Check whether `first`'s head wants a func at slot 0 */
                bool head_wants_func = false;
                if (g_param_kind_is_func) {
                    const char *head_name = NULL;
                    int slot = 0;
                    if (first->type == AST_SYMBOL) {
                        head_name = first->symbol;
                        slot = 0;
                    } else if (first->type == AST_LIST &&
                               first->list.count >= 1 &&
                               first->list.items[0]->type == AST_SYMBOL) {
                        head_name = first->list.items[0]->symbol;
                        slot = (int)first->list.count - 1;
                    }
                    if (head_name)
                        head_wants_func = g_param_kind_is_func(head_name, slot);
                }

                if (has_rhs && !head_wants_func) {
                    /* Infix rewrite — consume chain */
                    AST *acc = first;
                    while (p->current.type == TOK_SYMBOL && p->current.value) {
                        bool chain_is_known_fn = true;
                        if (g_is_known_function)
                            chain_is_known_fn = g_is_known_function(p->current.value);
                        if (!chain_is_known_fn) break;

                        /* peek again to confirm rhs exists */
                        Lexer sl2   = *p->lexer;
                        Token after2 = lexer_next_token(p->lexer);
                        *p->lexer   = sl2;
                        bool has_rhs2 = (after2.type != TOK_RPAREN &&
                                         after2.type != TOK_EOF);
                        free(after2.value);
                        if (!has_rhs2) break;

                        /* check the current acc head's next slot */
                        bool wants_func = false;
                        if (g_param_kind_is_func) {
                            const char *hname = NULL;
                            int sl = 0;
                            if (acc->type == AST_LIST &&
                                acc->list.count >= 1 &&
                                acc->list.items[0]->type == AST_SYMBOL) {
                                hname = acc->list.items[0]->symbol;
                                sl    = (int)acc->list.count - 1;
                            } else if (acc->type == AST_SYMBOL) {
                                hname = acc->symbol;
                                sl    = 0;
                            }
                            if (hname)
                                wants_func = g_param_kind_is_func(hname, sl);
                        }
                        if (wants_func) break;

                        int fn_line   = p->current.line;
                        int fn_col    = p->current.column;
                        char *fn_name = my_strdup(p->current.value);
                        p->current    = lexer_next_token(p->lexer);

                        AST *rhs  = parse_expr(p);
                        AST *fn_sym = ast_new_symbol(fn_name);
                        free(fn_name);
                        fn_sym->line   = fn_line;
                        fn_sym->column = fn_col;
                        AST *call = ast_new_list();
                        call->line   = fn_line;
                        call->column = fn_col;
                        ast_list_append(call, fn_sym);
                        ast_list_append(call, acc);
                        ast_list_append(call, rhs);
                        acc = call;

                        if (p->current.type == TOK_RPAREN) break;
                    }

                    if (p->current.type != TOK_RPAREN)
                        compiler_error(p->current.line, p->current.column,
                                       "Expected ')' after infix expression");
                    int end_col = p->current.column + 1;
                    p->current  = lexer_next_token(p->lexer);
                    /* list was never appended to — safe to free the empty shell */
                    free(list->list.items);
                    free(list);
                    acc->line       = start_line;
                    acc->column     = start_column;
                    acc->end_column = end_col;
                    return acc;
                }
            }

            /* Guard: literals cannot appear in function position
             * unless we are inside a quoted form or were rewritten as infix. */
            if (g_quote_depth == 0 &&
                (first->type == AST_NUMBER  ||
                 first->type == AST_STRING  ||
                 first->type == AST_CHAR    ||
                 first->type == AST_RATIO   ||
                 first->type == AST_KEYWORD)) {
                const char *kind =
                    first->type == AST_NUMBER  ? "number"    :
                    first->type == AST_STRING  ? "string"    :
                    first->type == AST_CHAR    ? "character" :
                    first->type == AST_RATIO   ? "ratio"     : "keyword";
                const char *lit =
                    first->literal_str         ? first->literal_str :
                    first->type == AST_STRING  ? first->string      :
                    first->type == AST_KEYWORD ? first->keyword     : "?";
                compiler_error(first->line, first->column,
                               "'%s' is a %s literal and cannot be called as a function",
                               lit, kind);
            }

            ast_list_append(list, first);
            if (step) {
                compiler_error(p->current.line, p->current.column,
                               "Unexpected ',' in list — did you mean a range (start,step..end)?");
            }
        }

        while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
            ast_list_append(list, parse_expr(p));
        }
        if (p->current.type != TOK_RPAREN)
            compiler_error(p->current.line, p->current.column, "Expected ')'");
        int end_column = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);
        list->line = start_line; list->column = start_column;
        list->end_column = end_column;
        return list;
    }
}

static AST *parse_bracket_list(Parser *p) {
    int start_line = p->current.line;
    int start_column = p->current.column;

    AST *list = ast_new_list();
    p->current = lexer_next_token(p->lexer);

    /* Range detection: [start..end] or [start,step..end] */
    if (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
        AST *first = parse_expr(p);

        AST *step = NULL;
        if (p->current.type == TOK_SYMBOL &&
            strcmp(p->current.value, ",") == 0) {
            p->current = lexer_next_token(p->lexer);
            step = parse_expr(p);
        }

        if (p->current.type == TOK_DOTDOT) {
            p->current = lexer_next_token(p->lexer);
            AST *end = NULL;
            if (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF)
                end = parse_expr(p);
            if (!end) {
                ast_free(first);
                ast_free(step);
                ast_free(list);
                READER_ERROR(p->current.line, p->current.column,
                    "Can't do infinite arrays. Are you trying to blow up the stack?");
            }
            if (p->current.type != TOK_RBRACKET)
                compiler_error(p->current.line, p->current.column, "Expected ']'");
            int end_col = p->current.column + 1;
            p->current = lexer_next_token(p->lexer);
            ast_free(list);
            AST *node = ast_new_range(first, step, end, true);
            node->line = start_line;
            node->column = start_column;
            node->end_column = end_col;
            return node;
        }

        /* Not a range — add first (and step if somehow parsed) back to list */
        ast_list_append(list, first);
        if (step) {
            compiler_error(p->current.line, p->current.column,
                           "Unexpected ',' — did you mean a range [start,step..end]?");
        }
    }

    while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
        ast_list_append(list, parse_expr(p));
    }
    if (p->current.type != TOK_RBRACKET) {
        compiler_error(p->current.line, p->current.column, "Expected ']'");
    }
    int end_column = p->current.column + 1;
    p->current = lexer_next_token(p->lexer);

    bool has_double_colon = false;
    for (size_t i = 0; i < list->list.count; i++) {
        if (list->list.items[i]->type == AST_SYMBOL &&
            strcmp(list->list.items[i]->symbol, "::") == 0) {
            has_double_colon = true;
            break;
        }
    }
    if (has_double_colon) {
        list->line = start_line;
        list->column = start_column;
        list->end_column = end_column;
        return list;
    }

    AST *array = ast_new_array();
    for (size_t i = 0; i < list->list.count; i++)
        ast_array_append(array, list->list.items[i]);
    free(list->list.items);
    free(list);
    array->line = start_line;
    array->column = start_column;
    array->end_column = end_column;
    return array;
}

static double parse_number_str(const char *s) {
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) return (double)(uint64_t)strtoull(s,NULL,16);
    if (s[0]=='0' && (s[1]=='b'||s[1]=='B')) return (double)(uint64_t)strtoull(s+2,NULL,2);
    if (s[0]=='0' && (s[1]=='o'||s[1]=='O')) return (double)(uint64_t)strtoull(s+2,NULL,8);
    return atof(s);
}

static void set_raw_int(AST *ast, const char *s) {
    if (!s) return;
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
        ast->raw_int     = strtoull(s, NULL, 16);
        ast->has_raw_int = true;
    } else if (s[0]=='0' && (s[1]=='b'||s[1]=='B')) {
        ast->raw_int     = strtoull(s+2, NULL, 2);
        ast->has_raw_int = true;
    } else if (s[0]=='0' && (s[1]=='o'||s[1]=='O')) {
        ast->raw_int     = strtoull(s+2, NULL, 8);
        ast->has_raw_int = true;
    }
}

static bool is_elem_of(const char *s) {
    if (!s) return false;
    unsigned char u0 = (unsigned char)s[0];
    unsigned char u1 = (unsigned char)s[1];
    unsigned char u2 = (unsigned char)s[2];
    return (u0 == 0xE2 && u1 == 0x88 && u2 == 0x88) || strcmp(s, "in") == 0;
}

/* Parse { x ∈ T | pred } as a refinement expression (anonymous).
 * Called from parse_set when we detect the set-builder pattern.
 * Returns an AST_REFINEMENT with name=NULL (anonymous). */
static AST *parse_refinement_expr(Parser *p, int start_line, int start_col) {
    /* var already confirmed to be current token */
    if (!p->current.value)
        compiler_error(p->current.line, p->current.column,
                       "Expected variable name in refinement { x ∈ T | pred }");
    char *var = my_strdup(p->current.value);
    p->current = lexer_next_token(p->lexer); // consume var

    /* consume ∈ or 'in' */
    if (!p->current.value)
        compiler_error(p->current.line, p->current.column,
                       "Expected '∈' after variable name");
    p->current = lexer_next_token(p->lexer); // consume ∈

    if (!p->current.value || p->current.type != TOK_SYMBOL)
        compiler_error(p->current.line, p->current.column,
                       "Expected base type after '∈'");

    if (p->current.type != TOK_SYMBOL)
        compiler_error(p->current.line, p->current.column,
                       "Expected base type after '∈'");
    char *base = my_strdup(p->current.value);
    p->current = lexer_next_token(p->lexer); // consume base type

    if (!(p->current.type == TOK_SYMBOL &&
          p->current.value &&
          strcmp(p->current.value, "|") == 0))
        compiler_error(p->current.line, p->current.column,
                       "Expected '|' after base type");
    p->current = lexer_next_token(p->lexer); // consume '|'

    /* Predicate: everything until '}' parsed as one expression.
     * Support both s-expr (>= x 0) and infix x >= 0 by checking:
     * if next token is '(' parse normally, otherwise wrap tokens
     * until '}' into an infix expression using existing backtick logic */
    AST *pred = NULL;
    if (p->current.type == TOK_LPAREN ||
        p->current.type == TOK_LBRACE) {
        pred = parse_expr(p);
    } else {
        /* Collect tokens until '}' and build (op lhs rhs) */
        /* lhs */
        AST *lhs = parse_expr(p);
        if (p->current.type != TOK_RBRACE) {
            /* op */
            if (p->current.type != TOK_SYMBOL)
                compiler_error(p->current.line, p->current.column,
                               "Expected operator in refinement predicate");
            char *op = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
            /* rhs — collect remaining until '}' into a begin if multiple */
            AST *rhs = parse_expr(p);
            /* handle chained: x >= 0 and x < 100 etc. */
            pred = ast_new_list();
            ast_list_append(pred, ast_new_symbol(op));
            ast_list_append(pred, lhs);
            ast_list_append(pred, rhs);
            free(op);
            /* chain: either arithmetic continuation (x % 2 = 0 → (= (% x 2) 0))
             * or logical connector (x > 0 and x < 10 → (and (> x 0) (< x 10))) */
            while (p->current.type != TOK_RBRACE &&
                   p->current.type != TOK_EOF &&
                   p->current.type == TOK_SYMBOL) {
                const char *sv = p->current.value;
                unsigned char c0 = (unsigned char)sv[0];
                unsigned char c1 = (unsigned char)sv[1];
                unsigned char c2 = (unsigned char)sv[2];

                /* logical connector: and ∧ or ∨ */
                const char *conn = NULL;
                if (strcmp(sv, "and") == 0 || (c0==0xE2 && c1==0x88 && c2==0xA7))
                    conn = "and";
                else if (strcmp(sv, "or") == 0 || (c0==0xE2 && c1==0x88 && c2==0xA8))
                    conn = "or";

                if (conn) {
                    p->current = lexer_next_token(p->lexer); // consume connector
                    /* parse next comparison: lhs op rhs */
                    AST *lhs2 = parse_expr(p);
                    if (p->current.type == TOK_RBRACE ||
                        p->current.type == TOK_EOF) {
                        /* bare predicate call like (prime? x) */
                        AST *node = ast_new_list();
                        ast_list_append(node, ast_new_symbol(conn));
                        ast_list_append(node, pred);
                        ast_list_append(node, lhs2);
                        pred = node;
                        break;
                    }
                    /* op rhs */
                    char *op2 = my_strdup(p->current.value);
                    p->current = lexer_next_token(p->lexer);
                    AST *rhs2 = parse_expr(p);
                    AST *cmp2 = ast_new_list();
                    ast_list_append(cmp2, ast_new_symbol(op2));
                    ast_list_append(cmp2, lhs2);
                    ast_list_append(cmp2, rhs2);
                    free(op2);
                    /* handle further chaining on rhs2: x % 2 = 0 */
                    while (p->current.type == TOK_SYMBOL &&
                           p->current.type != TOK_RBRACE) {
                        const char *sv2 = p->current.value;
                        unsigned char d0 = (unsigned char)sv2[0];
                        unsigned char d1 = (unsigned char)sv2[1];
                        unsigned char d2 = (unsigned char)sv2[2];
                        if (strcmp(sv2,"and")==0 || strcmp(sv2,"or")==0 ||
                            (d0==0xE2 && d1==0x88 && (d2==0xA7||d2==0xA8)))
                            break; /* logical connector — outer loop handles it */
                        char *op3 = my_strdup(sv2);
                        p->current = lexer_next_token(p->lexer);
                        AST *rhs3 = parse_expr(p);
                        AST *cmp3 = ast_new_list();
                        ast_list_append(cmp3, ast_new_symbol(op3));
                        ast_list_append(cmp3, cmp2);
                        ast_list_append(cmp3, rhs3);
                        free(op3);
                        cmp2 = cmp3;
                    }
                    AST *node = ast_new_list();
                    ast_list_append(node, ast_new_symbol(conn));
                    ast_list_append(node, pred);
                    ast_list_append(node, cmp2);
                    pred = node;
                } else {
                    /* arithmetic continuation: (pred) op rhs
                     * e.g. after (% x 2) we see = 0 → (= (% x 2) 0) */
                    char *op2 = my_strdup(sv);
                    p->current = lexer_next_token(p->lexer);
                    AST *rhs2 = parse_expr(p);
                    AST *node = ast_new_list();
                    ast_list_append(node, ast_new_symbol(op2));
                    ast_list_append(node, pred);
                    ast_list_append(node, rhs2);
                    free(op2);
                    pred = node;
                }
            }
        } else {
            pred = lhs; /* single predicate call like (prime? x) */
        }
    }

    if (p->current.type != TOK_RBRACE)
        compiler_error(p->current.line, p->current.column,
                       "Expected '}' to close refinement, got '%s'",
                       p->current.value ? p->current.value : "?");
    p->current = lexer_next_token(p->lexer); // consume '}'

    AST *node = ast_new_refinement(NULL, var, base, pred, NULL, NULL);
    free(var); free(base);
    node->line   = start_line;
    node->column = start_col;
    return node;
}

static AST *parse_set(Parser *p) {
    int start_line = p->current.line;
    int start_col  = p->current.column;
    p->current = lexer_next_token(p->lexer); /* consume '{' */

    /* Detect set-builder / refinement: { x ∈ T | pred } */
    if (p->current.type == TOK_SYMBOL && p->current.value) {
        /* peek ahead — if next token is ∈ or 'in', it's a refinement */
        Lexer saved_lex = *p->lexer;
        Token peek2 = lexer_next_token(p->lexer);
        bool is_ref = peek2.value && is_elem_of(peek2.value);
        free(peek2.value);
        *p->lexer = saved_lex; /* restore lexer position */
        /* p->current is unchanged — still the var token */
        if (is_ref)
            return parse_refinement_expr(p, start_line, start_col);
    }

    AST *node = ast_new_set();
    while (p->current.type != TOK_RBRACE &&
           p->current.type != TOK_EOF) {
        AST *elem = parse_expr(p);
        if (node->set.element_count >= node->set.element_capacity) {
            node->set.element_capacity *= 2;
            node->set.elements = realloc(node->set.elements,
                sizeof(AST*) * node->set.element_capacity);
        }
        node->set.elements[node->set.element_count++] = elem;
    }
    if (p->current.type != TOK_RBRACE)
        compiler_error(p->current.line, p->current.column,
                       "Expected '}' to close set literal");
    int end_col = p->current.column + 1;
    p->current = lexer_next_token(p->lexer);

    node->line       = start_line;
    node->column     = start_col;
    node->end_column = end_col;
    return node;
}

static AST *parse_map(Parser *p) {
    int start_line = p->current.line;
    int start_col  = p->current.column;
    p->current = lexer_next_token(p->lexer); /* consume '{' (after '#') */

    AST *node = ast_new_map();
    while (p->current.type != TOK_RBRACE &&
           p->current.type != TOK_EOF) {
        AST *key = parse_expr(p);
        if (p->current.type == TOK_RBRACE || p->current.type == TOK_EOF) {
            compiler_error(p->current.line, p->current.column,
                           "map literal requires an even number of forms — "
                           "key '%s' has no value", "?");
        }
        AST *val = parse_expr(p);

        if (node->map.count >= node->map.capacity) {
            node->map.capacity *= 2;
            node->map.keys = realloc(node->map.keys,
                                     sizeof(AST*) * node->map.capacity);
            node->map.vals = realloc(node->map.vals,
                                     sizeof(AST*) * node->map.capacity);
        }
        node->map.keys[node->map.count] = key;
        node->map.vals[node->map.count] = val;
        node->map.count++;
    }
    if (p->current.type != TOK_RBRACE)
        compiler_error(p->current.line, p->current.column,
                       "Expected '}' to close map literal");
    int end_col = p->current.column + 1;
    p->current = lexer_next_token(p->lexer);

    node->line       = start_line;
    node->column     = start_col;
    node->end_column = end_col;
    return node;
}

AST *parse_expr(Parser *p) {
    Token tok = p->current;

    switch (tok.type) {
    case TOK_NUMBER: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);

        // Check if it's a ratio (contains '/')
        if (tok.value && strchr(tok.value, '/')) {
            char *slash = strchr(tok.value, '/');
            *slash = '\0'; // temporarily split
            long long num = atoll(tok.value);
            long long denom = atoll(slash + 1);
            *slash = '/'; // restore

            AST *ast = ast_new_ratio(num, denom);
            ast->line = tok.line;
            ast->column = tok.column;
            ast->end_column = end_col;
            ast->literal_str = strdup(tok.value);
            return ast;
        }

        // Regular number
        AST *ast = ast_new_number(parse_number_str(tok.value), tok.value);
        set_raw_int(ast, tok.value);
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_SYMBOL: {
        // & is address-of only when the & is immediately adjacent to the next
        // token (no whitespace between them), meaning tok.end_column == next tok.column.
        // We detect this by comparing columns: if & is at col N and next token
        // is also at col N+1, they are adjacent → address-of.
        // If there's a space between them → bitwise AND symbol.
        if (tok.value && strcmp(tok.value, "&") == 0) {
            /* Advance past & first, then check what follows */
            p->current = lexer_next_token(p->lexer);
            int next_col = p->current.column;
            int amp_col  = tok.column;
            bool adjacent = (next_col == amp_col + 1);
            if (adjacent &&
                (p->current.type == TOK_SYMBOL ||
                 p->current.type == TOK_NUMBER ||
                 p->current.type == TOK_LPAREN)) {
                int addr_line = tok.line, addr_col = tok.column;
                AST *operand = parse_expr(p);
                AST *node = ast_new_address_of(operand);
                node->line       = addr_line;
                node->column     = addr_col;
                node->end_column = operand->end_column;
                return node;
            }
            /* Not adjacent — just a bitwise AND symbol, already advanced */
            AST *ast = ast_new_symbol("&");
            ast->line       = tok.line;
            ast->column     = tok.column;
            ast->end_column = tok.column + 1;
            return ast;
        }
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_symbol(tok.value);
        ast->line       = tok.line;
        ast->column     = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_STRING: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1) + 2;
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_string(tok.value);
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_CHAR: {
        int end_col = tok.column + 3;
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_char(tok.value[0]);
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_LPAREN:
        return parse_list(p);
    case TOK_HASH_LBRACE:
        return parse_map(p);
    case TOK_LBRACKET:
        return parse_bracket_list(p);
    case TOK_LBRACE:
        return parse_set(p);
    case TOK_QUOTE: {
        int quote_line = tok.line;
        int quote_column = tok.column;
        p->current = lexer_next_token(p->lexer);
        g_quote_depth++;
        AST *quoted = parse_expr(p);
        g_quote_depth--;
        AST *list   = ast_new_list();
        ast_list_append(list, ast_new_symbol("quote"));
        ast_list_append(list, quoted);
        list->line = quote_line;
        list->column = quote_column;
        list->end_column = quoted->end_column;
        return list;
    }

    case TOK_ARROW: {
        int end_col = tok.column + 2;
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_symbol("->");
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }

    case TOK_LAMBDA_LIT: {
        // tok.value is the single param name (e.g. "x" from λx.body)
        int lam_line = tok.line;
        int lam_col  = tok.column;
        p->current   = lexer_next_token(p->lexer);

        ASTParam *params  = malloc(sizeof(ASTParam));
        params[0].name      = my_strdup(tok.value);
        params[0].type_name = NULL;
        params[0].is_rest   = false;
        params[0].is_anon   = false;

        // Parse body — if it starts with another λ it naturally nests:
        // λx.λy.x → (lambda (x) (lambda (y) x))
        AST *body = parse_expr(p);

        AST **body_exprs  = malloc(sizeof(AST*));
        body_exprs[0]     = body;

        AST *lam = ast_new_lambda(params, 1,
                                  NULL, NULL, NULL, false,
                                  body, body_exprs, 1);
        lam->line   = lam_line;
        lam->column = lam_col;
        return lam;
    }
    case TOK_KEYWORD: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1) + 1; // +1 for ':'
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_keyword(tok.value);
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }

    default:
        compiler_error(tok.line, tok.column, "unexpected token type: %d", tok.type);
    }
}

// Helper to skip an expression without parsing it
static void skip_expr(Parser *p) {
    if (p->current.type == TOK_LPAREN) {
        p->current = lexer_next_token(p->lexer);
        int depth = 1;
        while (depth > 0 && p->current.type != TOK_EOF) {
            if (p->current.type == TOK_LPAREN) depth++;
            if (p->current.type == TOK_RPAREN) depth--;
            p->current = lexer_next_token(p->lexer);
        }
    } else if (p->current.type == TOK_LBRACKET) {
        p->current = lexer_next_token(p->lexer);
        int depth = 1;
        while (depth > 0 && p->current.type != TOK_EOF) {
            if (p->current.type == TOK_LBRACKET) depth++;
            if (p->current.type == TOK_RBRACKET) depth--;
            p->current = lexer_next_token(p->lexer);
        }
    } else if (p->current.type == TOK_LBRACE ||
               p->current.type == TOK_HASH_LBRACE) {
        /* Both {} (set) and #{} (map) close with TOK_RBRACE */
        p->current = lexer_next_token(p->lexer);
        int depth = 1;
        while (depth > 0 && p->current.type != TOK_EOF) {
            if (p->current.type == TOK_LBRACE ||
                p->current.type == TOK_HASH_LBRACE) depth++;
            if (p->current.type == TOK_RBRACE)       depth--;
            p->current = lexer_next_token(p->lexer);
        }
    } else {
        p->current = lexer_next_token(p->lexer);
    }
}


// Check if a feature is active
static bool is_feature_active(const char *feature_name) {
    return has_feature(feature_name);
}

// Parse feature blocks - #--- closes ALL nested blocks at current level
static void parse_feature_blocks(Parser *p, AST ***exprs, size_t *count, size_t *capacity, bool parent_enabled) {
    while (p->current.type == TOK_FEATURE_BEGIN) {
        // Read feature name
        p->current = lexer_next_token(p->lexer);

        if (p->current.type != TOK_KEYWORD && p->current.type != TOK_SYMBOL) {
            compiler_error(p->current.line, p->current.column,
                          "Expected feature name after #+");
        }

        const char *feature_name = p->current.value;

        // Feature is only enabled if parent is also enabled (for nesting)
        bool feature_enabled = parent_enabled && is_feature_active(feature_name);

        // Skip the feature name
        p->current = lexer_next_token(p->lexer);

        // Parse content for this feature block
        while (p->current.type != TOK_EOF &&
               p->current.type != TOK_FEATURE_END) {

            // If we hit another #+, it's a sibling block at same level
            if (p->current.type == TOK_FEATURE_BEGIN) {
                break;
            }

            if (feature_enabled) {
                AST *expr = parse_expr(p);

                if (*count >= *capacity) {
                    *capacity = *capacity == 0 ? 4 : *capacity * 2;
                    *exprs = realloc(*exprs, sizeof(AST *) * *capacity);
                }
                (*exprs)[(*count)++] = expr;
            } else {
                skip_expr(p);
            }
        }
    }

    // Must end with #---
    if (p->current.type != TOK_FEATURE_END) {
        compiler_error(p->current.line, p->current.column,
                      "Expected #--- to close feature block");
    }

    // Consume the #---
    p->current = lexer_next_token(p->lexer);
}

ASTList parse_all(const char *source) {
    Lexer lex;
    lexer_init(&lex, source);

    ASTList list = {0};
    list.exprs = NULL;
    list.count = 0;
    size_t capacity = 0;

    Parser p;
    parser_init(&p, &lex);

    while (p.current.type != TOK_EOF) {
        if (p.current.type == TOK_FEATURE_BEGIN) {
            parse_feature_blocks(&p, &list.exprs, &list.count, &capacity, true);
        } else {
            AST *expr = parse_expr(&p);

            if (list.count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                list.exprs = realloc(list.exprs, sizeof(AST *) * capacity);
            }
            list.exprs[list.count++] = expr;
        }
    }

    return list;
}

/// String builder

void sb_init(SB *b)  { b->data = malloc(256); b->data[0]='\0'; b->len=0; b->cap=256; }
void sb_free(SB *b)  { free(b->data); }
char *sb_take(SB *b) { char *r = b->data; b->data = NULL; return r; }

void sb_putc(SB *b, char c) {
    if (b->len + 1 >= b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

void sb_puts(SB *b, const char *s) {
    size_t l = strlen(s);
    while (b->len + l + 1 >= b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, l + 1);
    b->len += l;
}



/// AST -> JSON

static void json_escape(SB *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        switch (*s) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n");  break;
        case '\r': sb_puts(b, "\\r");  break;
        case '\t': sb_puts(b, "\\t");  break;
        default:   sb_putc(b, *s);     break;
        }
    }
    sb_putc(b, '"');
}

static void json_loc(SB *b, AST *a) {
    char buf[64];
    snprintf(buf, sizeof(buf), "\"line\":%d,\"col\":%d", a->line, a->column);
    sb_puts(b, buf);
}

static void ast_to_json_sb(SB *b, AST *ast);

static void json_pattern(SB *b, ASTPattern *p) {
    sb_puts(b, "{\"kind\":");
    switch (p->kind) {
    case PAT_WILDCARD:     sb_puts(b, "\"wildcard\""); break;
    case PAT_VAR:
        sb_puts(b, "\"var\",\"name\":");
        json_escape(b, p->var_name ? p->var_name : "");
        break;
    case PAT_LITERAL_INT:
        sb_puts(b, "\"literal_int\",\"value\":");
        { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)p->lit_value);
          sb_puts(b, buf); }
        break;
    case PAT_LITERAL_FLOAT:
        sb_puts(b, "\"literal_float\",\"value\":");
        { char buf[32]; snprintf(buf, sizeof(buf), "%g", p->lit_value);
          sb_puts(b, buf); }
        break;
    case PAT_LIST_EMPTY:
        sb_puts(b, "\"list_empty\"");
        break;
    case PAT_LIST:
        sb_puts(b, "\"list\",\"elements\":[");
        for (int i = 0; i < p->element_count; i++) {
            if (i) sb_putc(b, ',');
            json_pattern(b, &p->elements[i]);
        }
        sb_puts(b, "]");
        if (p->tail) {
            sb_puts(b, ",\"tail\":");
            json_pattern(b, p->tail);
        }
        break;
    case PAT_CONSTRUCTOR:
        sb_puts(b, "\"constructor\",\"name\":");
        json_escape(b, p->var_name ? p->var_name : "");
        if (p->ctor_field_count > 0) {
            sb_puts(b, ",\"fields\":[");
            for (int i = 0; i < p->ctor_field_count; i++) {
                if (i) sb_putc(b, ',');
                json_pattern(b, &p->ctor_fields[i]);
            }
            sb_puts(b, "]");
        }
        break;
    }
    sb_putc(b, '}');
}

static void ast_to_json_sb(SB *b, AST *ast) {
    if (!ast) { sb_puts(b, "null"); return; }

    sb_puts(b, "{");
    json_loc(b, ast);
    sb_puts(b, ",\"type\":");

    switch (ast->type) {

    case AST_NUMBER:
        sb_puts(b, "\"number\",\"value\":");
        if (ast->literal_str) json_escape(b, ast->literal_str);
        else {
            char buf[64]; snprintf(buf, sizeof(buf), "%g", ast->number);
            sb_puts(b, buf);
        }
        break;

    case AST_SYMBOL:
        sb_puts(b, "\"symbol\",\"name\":");
        json_escape(b, ast->symbol ? ast->symbol : "");
        break;

    case AST_STRING:
        sb_puts(b, "\"string\",\"value\":");
        json_escape(b, ast->string ? ast->string : "");
        break;

    case AST_CHAR: {
        char buf[3] = {ast->character, 0};
        sb_puts(b, "\"char\",\"value\":");
        json_escape(b, buf);
        break;
    }

    case AST_KEYWORD:
        sb_puts(b, "\"keyword\",\"name\":");
        json_escape(b, ast->keyword ? ast->keyword : "");
        break;

    case AST_RATIO: {
        char buf[64];
        snprintf(buf, sizeof(buf), "\"ratio\",\"numerator\":%lld,\"denominator\":%lld",
                 ast->ratio.numerator, ast->ratio.denominator);
        sb_puts(b, buf);
        break;
    }

    case AST_LIST:
        sb_puts(b, "\"list\",\"items\":[");
        for (size_t i = 0; i < ast->list.count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->list.items[i]);
        }
        sb_puts(b, "]");
        break;

    case AST_ARRAY:
        sb_puts(b, "\"array\",\"elements\":[");
        for (size_t i = 0; i < ast->array.element_count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->array.elements[i]);
        }
        sb_puts(b, "]");
        break;

    case AST_SET:
        sb_puts(b, "\"set\",\"elements\":[");
        for (size_t i = 0; i < ast->set.element_count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->set.elements[i]);
        }
        sb_puts(b, "]");
        break;

    case AST_MAP:
        sb_puts(b, "\"map\",\"entries\":[");
        for (size_t i = 0; i < ast->map.count; i++) {
            if (i) sb_putc(b, ',');
            sb_puts(b, "{\"key\":");
            ast_to_json_sb(b, ast->map.keys[i]);
            sb_puts(b, ",\"val\":");
            ast_to_json_sb(b, ast->map.vals[i]);
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        break;

    case AST_LAMBDA:
        sb_puts(b, "\"lambda\",\"params\":[");
        for (int i = 0; i < ast->lambda.param_count; i++) {
            if (i) sb_putc(b, ',');
            sb_puts(b, "{\"name\":");
            json_escape(b, ast->lambda.params[i].name ? ast->lambda.params[i].name : "");
            if (ast->lambda.params[i].type_name) {
                sb_puts(b, ",\"type\":");
                json_escape(b, ast->lambda.params[i].type_name);
            }
            if (ast->lambda.params[i].is_rest) sb_puts(b, ",\"rest\":true");
            if (ast->lambda.params[i].is_anon) sb_puts(b, ",\"anon\":true");
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        if (ast->lambda.return_type) {
            sb_puts(b, ",\"return_type\":");
            json_escape(b, ast->lambda.return_type);
        }
        if (ast->lambda.docstring) {
            sb_puts(b, ",\"docstring\":");
            json_escape(b, ast->lambda.docstring);
        }
        if (ast->lambda.naked) sb_puts(b, ",\"naked\":true");
        sb_puts(b, ",\"body\":[");
        for (int i = 0; i < ast->lambda.body_count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->lambda.body_exprs[i]);
        }
        sb_puts(b, "]");
        break;

    case AST_RANGE:
        sb_puts(b, "\"range\"");
        sb_puts(b, ",\"is_array\":");
        sb_puts(b, ast->range.is_array ? "true" : "false");
        sb_puts(b, ",\"start\":");
        ast_to_json_sb(b, ast->range.start);
        if (ast->range.step) {
            sb_puts(b, ",\"step\":");
            ast_to_json_sb(b, ast->range.step);
        }
        if (ast->range.end) {
            sb_puts(b, ",\"end\":");
            ast_to_json_sb(b, ast->range.end);
        }
        break;

    case AST_ADDRESS_OF:
        sb_puts(b, "\"address_of\",\"operand\":");
        ast_to_json_sb(b, ast->list.items[0]);
        break;

    case AST_REFINEMENT:
        sb_puts(b, "\"refinement\"");
        if (ast->refinement.name) {
            sb_puts(b, ",\"name\":");
            json_escape(b, ast->refinement.name);
        }
        if (ast->refinement.var) {
            sb_puts(b, ",\"var\":");
            json_escape(b, ast->refinement.var);
        }
        if (ast->refinement.base_type) {
            sb_puts(b, ",\"base_type\":");
            json_escape(b, ast->refinement.base_type);
        }
        if (ast->refinement.predicate) {
            sb_puts(b, ",\"predicate\":");
            ast_to_json_sb(b, ast->refinement.predicate);
        }
        if (ast->refinement.docstring) {
            sb_puts(b, ",\"docstring\":");
            json_escape(b, ast->refinement.docstring);
        }
        break;

    case AST_LAYOUT:
        sb_puts(b, "\"layout\",\"name\":");
        json_escape(b, ast->layout.name ? ast->layout.name : "");
        sb_puts(b, ",\"fields\":[");
        for (int i = 0; i < ast->layout.field_count; i++) {
            if (i) sb_putc(b, ',');
            ASTLayoutField *f = &ast->layout.fields[i];
            sb_puts(b, "{\"name\":");
            json_escape(b, f->name ? f->name : "");
            if (f->is_array) {
                sb_puts(b, ",\"is_array\":true,\"array_elem\":");
                json_escape(b, f->array_elem ? f->array_elem : "");
                char buf[32];
                snprintf(buf, sizeof(buf), ",\"array_size\":%d", f->array_size);
                sb_puts(b, buf);
            } else {
                sb_puts(b, ",\"type\":");
                json_escape(b, f->type_name ? f->type_name : "");
            }
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        if (ast->layout.packed) sb_puts(b, ",\"packed\":true");
        if (ast->layout.align) {
            char buf[32];
            snprintf(buf, sizeof(buf), ",\"align\":%d", ast->layout.align);
            sb_puts(b, buf);
        }
        break;

    case AST_DATA:
        sb_puts(b, "\"data\",\"name\":");
        json_escape(b, ast->data.name ? ast->data.name : "");
        sb_puts(b, ",\"constructors\":[");
        for (int i = 0; i < ast->data.constructor_count; i++) {
            if (i) sb_putc(b, ',');
            ASTDataConstructor *ct = &ast->data.constructors[i];
            sb_puts(b, "{\"name\":");
            json_escape(b, ct->name ? ct->name : "");
            sb_puts(b, ",\"fields\":[");
            for (int j = 0; j < ct->field_count; j++) {
                if (j) sb_putc(b, ',');
                json_escape(b, ct->field_types[j] ? ct->field_types[j] : "");
            }
            sb_puts(b, "]}");
        }
        sb_puts(b, "]");
        if (ast->data.deriving_count > 0) {
            sb_puts(b, ",\"deriving\":[");
            for (int i = 0; i < ast->data.deriving_count; i++) {
                if (i) sb_putc(b, ',');
                json_escape(b, ast->data.deriving[i] ? ast->data.deriving[i] : "");
            }
            sb_puts(b, "]");
        }
        break;

    case AST_PMATCH:
        sb_puts(b, "\"pmatch\",\"clauses\":[");
        for (int i = 0; i < ast->pmatch.clause_count; i++) {
            if (i) sb_putc(b, ',');
            ASTPMatchClause *cl = &ast->pmatch.clauses[i];
            sb_puts(b, "{\"patterns\":[");
            for (int j = 0; j < cl->pattern_count; j++) {
                if (j) sb_putc(b, ',');
                json_pattern(b, &cl->patterns[j]);
            }
            sb_puts(b, "],\"body\":");
            ast_to_json_sb(b, cl->body);
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        break;

    case AST_CLASS:
        sb_puts(b, "\"class\",\"name\":");
        json_escape(b, ast->class_decl.name ? ast->class_decl.name : "");
        sb_puts(b, ",\"type_var\":");
        json_escape(b, ast->class_decl.type_var ? ast->class_decl.type_var : "");
        sb_puts(b, ",\"methods\":[");
        for (int i = 0; i < ast->class_decl.method_count; i++) {
            if (i) sb_putc(b, ',');
            sb_puts(b, "{\"name\":");
            json_escape(b, ast->class_decl.method_names[i] ? ast->class_decl.method_names[i] : "");
            sb_puts(b, ",\"type\":");
            json_escape(b, ast->class_decl.method_types[i] ? ast->class_decl.method_types[i] : "");
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        if (ast->class_decl.default_count > 0) {
            sb_puts(b, ",\"defaults\":[");
            for (int i = 0; i < ast->class_decl.default_count; i++) {
                if (i) sb_putc(b, ',');
                sb_puts(b, "{\"name\":");
                json_escape(b, ast->class_decl.default_names[i] ? ast->class_decl.default_names[i] : "");
                sb_puts(b, ",\"body\":");
                ast_to_json_sb(b, ast->class_decl.default_bodies[i]);
                sb_putc(b, '}');
            }
            sb_puts(b, "]");
        }
        break;

    case AST_INSTANCE:
        sb_puts(b, "\"instance\",\"class\":");
        json_escape(b, ast->instance_decl.class_name ? ast->instance_decl.class_name : "");
        sb_puts(b, ",\"type\":");
        json_escape(b, ast->instance_decl.type_name ? ast->instance_decl.type_name : "");
        sb_puts(b, ",\"methods\":[");
        for (int i = 0; i < ast->instance_decl.method_count; i++) {
            if (i) sb_putc(b, ',');
            sb_puts(b, "{\"name\":");
            json_escape(b, ast->instance_decl.method_names[i] ? ast->instance_decl.method_names[i] : "");
            sb_puts(b, ",\"body\":");
            ast_to_json_sb(b, ast->instance_decl.method_bodies[i]);
            sb_putc(b, '}');
        }
        sb_puts(b, "]");
        break;

    case AST_ASM:
        sb_puts(b, "\"asm\",\"instructions\":[");
        for (size_t i = 0; i < ast->asm_block.instruction_count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->asm_block.instructions[i]);
        }
        sb_puts(b, "]");
        break;

    case AST_TESTS:
        sb_puts(b, "\"tests\",\"assertions\":[");
        for (int i = 0; i < ast->tests.count; i++) {
            if (i) sb_putc(b, ',');
            ast_to_json_sb(b, ast->tests.assertions[i]);
        }
        sb_puts(b, "]");
        break;

    default:
        sb_puts(b, "\"unknown\"");
        break;
    }

    sb_putc(b, '}');
}

char *ast_to_json(AST *ast) {
    SB b;
    sb_init(&b);
    ast_to_json_sb(&b, ast);
    return sb_take(&b);
}

AST *parse(const char *source) {
    Lexer lex;
    lexer_init(&lex, source);
    Parser p;
    parser_init(&p, &lex);
    return parse_expr(&p);
}
