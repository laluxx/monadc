#include "reader.h"
#include "features.h"
#include "pmatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// TODO AST->JSON

/* Replace all exit(1) calls in the reader/parser.
 * If a recovery point is set (we're inside repl_eval_line), longjmp back.
 * Otherwise fall back to real exit so the standalone compiler still works. */

jmp_buf  g_reader_escape;
bool     g_reader_escape_set = false;
char     g_reader_error_msg[512];

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

/// Error reporting context

static const char *current_filename = NULL;
static const char *current_source = NULL;

void parser_set_context(const char *filename, const char *source) {
    current_filename = filename;
    current_source = source;
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

AST *ast_new_type_alias(const char *alias_name, const char *target_name) {
    AST *a = calloc(1, sizeof(AST));
    a->type = AST_TYPE_ALIAS;
    a->type_alias.alias_name  = my_strdup(alias_name);
    a->type_alias.target_name = my_strdup(target_name);
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

    case AST_TYPE_ALIAS:
        free(ast->type_alias.alias_name);
        free(ast->type_alias.target_name);
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
    case AST_NUMBER:  printf("%g", ast->number); break;
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
        printf(" ");
        ast_print(ast->lambda.body);
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
    case AST_TYPE_ALIAS:
        printf("(type %s %s)", ast->type_alias.alias_name, ast->type_alias.target_name);
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

    tok.line   = lex->line;
    tok.column = lex->column;

    char c = peek(lex);

    if (c == '\0') { tok.type = TOK_EOF; return tok; }

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
    if (c == '|') { advance(lex); tok.type = TOK_PIPE;     return tok; }

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
        while (is_digit(peek(lex)) ||
              (peek(lex) == '.' && peek_ahead(lex, 1) != '.')) advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
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


    // Decimal number
    if (is_digit(c)) {
        size_t start = lex->pos;
        while (is_digit(peek(lex)) ||
               (peek(lex) == '.' && peek_ahead(lex, 1) != '.')) advance(lex);
        // Check for ratio (/)
        if (peek(lex) == '/') {
            advance(lex); // consume '/'
            if (!is_digit(peek(lex))) {
                fprintf(stderr, "Invalid ratio: missing denominator\n");
                exit(1);
            }
            while (is_digit(peek(lex))) advance(lex);
        }

        tok.value = my_strndup(lex->source+start, lex->pos-start);
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
            if (p->current.type == TOK_SYMBOL) {
                param.type_name = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
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
            /* Consume -> and continue — bare symbol after -> may be
             * return type only if it's the very last token before )  */
            p->current = lexer_next_token(p->lexer);

            /* If next token is a symbol, peek one more ahead */
            if (p->current.type == TOK_SYMBOL) {
                char *sym  = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);

                if (p->current.type == TOK_RPAREN ||
                    p->current.type == TOK_EOF) {
                    /* Last token before ) — it's the return type */
                    free(ret_type);
                    ret_type = sym;
                } else {
                    /* More tokens follow — it's another parameter */
                    if (count >= capacity) {
                        capacity = capacity == 0 ? 4 : capacity * 2;
                        params   = realloc(params, sizeof(ASTParam) * capacity);
                    }
                    params[count].name      = sym;
                    params[count].type_name = NULL;
                    params[count].is_rest   = false;
                    count++;
                }
            }
            /* If next token is [ or another ->, just continue the loop */
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
            free(ret_type);
            ret_type   = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);
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
        // Plain string docstring
        if (p->current.type == TOK_STRING && !m.docstring) {
            // But only if the token AFTER it is not TOK_RPAREN alone
            // (i.e. there's still a body coming) — we peek by trying:
            // Actually we just consume it; the body must follow.
            m.docstring = my_strdup(p->current.value);
            p->current  = lexer_next_token(p->lexer);
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
            fprintf(stderr, "DEBUG: found :naked keyword\n");
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
        strcmp(p->current.value, "let") == 0) {
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
        return build_let(params, param_count, inits, body_exprs, body_count, start_line, start_column);
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
            if (p->current.type != TOK_LPAREN &&
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
                name_ast = parse_expr(p);
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

    // Type alias
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "type") == 0) {

        int tline = p->current.line, tcol = p->current.column;
        p->current = lexer_next_token(p->lexer); // consume 'type'

        if (p->current.type != TOK_SYMBOL) {
            compiler_error(p->current.line, p->current.column,
                           "Expected alias name after 'type'");
        }
        char *alias_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        if (p->current.type != TOK_SYMBOL) {
            compiler_error(p->current.line, p->current.column,
                           "Expected target type name after alias name");
        }
        char *target_name = my_strdup(p->current.value);
        p->current = lexer_next_token(p->lexer);

        if (p->current.type != TOK_RPAREN) {
            compiler_error(p->current.line, p->current.column,
                           "Expected ')' to close type alias");
        }
        int end_col = p->current.column + 1;
        p->current = lexer_next_token(p->lexer);

        ast_free(list);
        AST *node = ast_new_type_alias(alias_name, target_name);
        free(alias_name);
        free(target_name);
        node->line = tline;
        node->column = tcol;
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
            ast_list_append(list, first);
            if (step) {
                /* step was parsed but no .., treat comma+step as error or
                   just add them as list elements — but comma isn't valid
                   in a normal list so error */
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
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) return (double)strtol(s,NULL,16);
    if (s[0]=='0' && (s[1]=='b'||s[1]=='B')) return (double)strtol(s+2,NULL,2);
    if (s[0]=='0' && (s[1]=='o'||s[1]=='O')) return (double)strtol(s+2,NULL,8);
    return atof(s);
}

static AST *parse_set(Parser *p) {
    int start_line = p->current.line;
    int start_col  = p->current.column;
    p->current = lexer_next_token(p->lexer); /* consume '{' */

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
        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_SYMBOL: {
        // & reader macro — address-of
        if (tok.value && strcmp(tok.value, "&") == 0) {
            int addr_line = tok.line, addr_col = tok.column;
            p->current = lexer_next_token(p->lexer);
            AST *operand = parse_expr(p);
            AST *node = ast_new_address_of(operand);
            node->line       = addr_line;
            node->column     = addr_col;
            node->end_column = operand->end_column;
            return node;
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
        AST *quoted = parse_expr(p);
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
                // Skip the expression
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

AST *parse(const char *source) {
    Lexer lex;
    lexer_init(&lex, source);
    Parser p;
    parser_init(&p, &lex);
    return parse_expr(&p);
}
