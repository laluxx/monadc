#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
        fprintf(stderr, "^\n");
    }

    va_end(args);
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
                    AST *body) {
    AST *a = calloc(1, sizeof(AST));
    a->type                = AST_LAMBDA;
    a->lambda.params       = params;
    a->lambda.param_count  = param_count;
    a->lambda.return_type  = return_type ? my_strdup(return_type) : NULL;
    a->lambda.docstring    = docstring   ? my_strdup(docstring)   : NULL;
    a->lambda.body         = body;
    return a;
}

void ast_list_append(AST *list, AST *item) {
    if (list->list.count >= list->list.capacity) {
        list->list.capacity *= 2;
        list->list.items = realloc(list->list.items,
                                   sizeof(AST *) * list->list.capacity);
    }
    list->list.items[list->list.count++] = item;
}

void ast_free(AST *ast) {
    if (!ast) return;
    switch (ast->type) {
    case AST_SYMBOL: free(ast->symbol); break;
    case AST_STRING: free(ast->string); break;
    case AST_LIST:
        for (size_t i = 0; i < ast->list.count; i++)
            ast_free(ast->list.items[i]);
        free(ast->list.items);
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
        ast_free(ast->lambda.body);
        break;
    default: break;
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
static bool is_symbol_char(char c) {
    return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') ||
           c=='-' || c=='+' || c=='*' || c=='/' ||
           c=='<' || c=='>' || c=='=' || c=='!' ||
           c=='?' || c=='_' || c==':';
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

    // Arrow  ->
    if (c == '-' && peek_ahead(lex, 1) == '>') {
        advance(lex); advance(lex);
        tok.type  = TOK_ARROW;
        tok.value = my_strdup("->");
        return tok;
    }

    if (c == '(') { advance(lex); tok.type = TOK_LPAREN;   return tok; }
    if (c == ')') { advance(lex); tok.type = TOK_RPAREN;   return tok; }
    if (c == '[') { advance(lex); tok.type = TOK_LBRACKET; return tok; }
    if (c == ']') { advance(lex); tok.type = TOK_RBRACKET; return tok; }

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
                        fprintf(stderr, "Unterminated char literal\n"); exit(1);
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
                        fprintf(stderr, "Unterminated char literal\n"); exit(1);
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
        while (is_digit(peek(lex)) || peek(lex) == '.') advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_NUMBER; return tok;
    }

    // Decimal number
    if (is_digit(c)) {
        size_t start = lex->pos;
        while (is_digit(peek(lex)) || peek(lex) == '.') advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_NUMBER; return tok;
    }

    // Symbol
    if (is_symbol_char(c)) {
        size_t start = lex->pos;
        while (is_symbol_char(peek(lex))) advance(lex);
        tok.value = my_strndup(lex->source+start, lex->pos-start);
        tok.type  = TOK_SYMBOL; return tok;
    }

    fprintf(stderr, "Unexpected character '%c' at %d:%d\n", c, lex->line, lex->column);
    exit(1);
}

/// Parser

typedef struct {
    Lexer *lexer;
    Token  current;
} Parser;

static void parser_init(Parser *p, Lexer *lex) {
    p->lexer   = lex;
    p->current = lexer_next_token(lex);
}

static AST *parse_expr(Parser *p);

static ASTParam parse_one_param(Parser *p) {
    ASTParam param = {NULL, NULL};

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
    ASTParam *params = NULL;
    int count = 0;
    int capacity = 0;
    char *ret_type = NULL;

    while (p->current.type != TOK_RPAREN &&
           p->current.type != TOK_EOF) {

        if (p->current.type == TOK_LBRACKET) {
            p->current = lexer_next_token(p->lexer);

            ASTParam param = parse_one_param(p);

            if (p->current.type != TOK_RBRACKET) {
                compiler_error(p->current.line, p->current.column,
                             "Expected ']' after parameter");
            }
            p->current = lexer_next_token(p->lexer);

            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                params   = realloc(params, sizeof(ASTParam) * capacity);
            }
            params[count++] = param;

        } else if (p->current.type == TOK_ARROW) {
            p->current = lexer_next_token(p->lexer);

        } else if (p->current.type == TOK_SYMBOL) {
            ret_type   = my_strdup(p->current.value);
            p->current = lexer_next_token(p->lexer);

        } else {
            compiler_error(p->current.line, p->current.column,
                         "Unexpected token in function signature");
        }
    }

    if (p->current.type != TOK_RPAREN) {
        compiler_error(p->current.line, p->current.column,
                     "Expected ')' to close function signature");
    }
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

    AST *body = parse_expr(p);

    return ast_new_lambda(params, count, ret_type, docstring, body);
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

    // Detect (define (fname params...) body) - short-form function definition
    if (p->current.type == TOK_SYMBOL &&
        strcmp(p->current.value, "define") == 0) {

        // Peek ahead to see if it's (define (fname ...) or (define name ...)
        Token define_token = p->current;
        p->current = lexer_next_token(p->lexer);

        // Check if next token is '(' (function definition)
        if (p->current.type == TOK_LPAREN) {
            // Parse as (define (fname signature...) docstring? body)
            p->current = lexer_next_token(p->lexer); // consume '('

            // Get function name
            if (p->current.type != TOK_SYMBOL) {
                compiler_error(p->current.line, p->current.column,
                             "Expected function name after (define (");
            }
            AST *fname = ast_new_symbol(p->current.value);
            fname->line = p->current.line;
            fname->column = p->current.column;
            fname->end_column = p->current.column + strlen(p->current.value);
            p->current = lexer_next_token(p->lexer);

            // Parse signature
            ASTParam *params = NULL;
            int count = 0;
            char *ret_type = NULL;
            parse_fn_signature(p, &params, &count, &ret_type);

            // Optional docstring
            char *docstring = NULL;
            if (p->current.type == TOK_STRING) {
                docstring = my_strdup(p->current.value);
                p->current = lexer_next_token(p->lexer);
            }

            // Body
            AST *body = parse_expr(p);

            // Expect closing ')' for the define
            if (p->current.type != TOK_RPAREN) {
                compiler_error(p->current.line, p->current.column,
                             "Expected ')' after define body");
            }

            int end_column = p->current.column + 1;
            p->current = lexer_next_token(p->lexer);

            // Build (define fname (lambda ...))
            AST *lambda = ast_new_lambda(params, count, ret_type, docstring, body);
            lambda->line = fname->line;
            lambda->column = fname->column;
            lambda->end_column = end_column;

            AST *result = ast_new_list();
            ast_list_append(result, ast_new_symbol("define"));
            ast_list_append(result, fname);
            ast_list_append(result, lambda);

            result->line = start_line;
            result->column = start_column;
            result->end_column = end_column;

            ast_free(list);
            return result;
        } else {
            // Not a function definition, it's (define name value)
            // Put the 'define' symbol into the list and continue normal parsing
            AST *define_sym = ast_new_symbol(define_token.value);
            define_sym->line = define_token.line;
            define_sym->column = define_token.column;
            define_sym->end_column = define_token.column + strlen(define_token.value);
            ast_list_append(list, define_sym);
            // Continue parsing the rest (current token is already the next one after 'define')
            // Fall through to normal list parsing
        }
    }

    // Normal list parsing
    while (p->current.type != TOK_RPAREN &&
           p->current.type != TOK_EOF) {
        AST *item = parse_expr(p);
        ast_list_append(list, item);
    }

    if (p->current.type != TOK_RPAREN) {
        compiler_error(p->current.line, p->current.column, "Expected ')'");
    }

    int end_column = p->current.column + 1;
    p->current = lexer_next_token(p->lexer);

    list->line = start_line;
    list->column = start_column;
    list->end_column = end_column;
    return list;
}

static AST *parse_bracket_list(Parser *p) {
    int start_line = p->current.line;
    int start_column = p->current.column;

    AST *list = ast_new_list();
    p->current = lexer_next_token(p->lexer);

    while (p->current.type != TOK_RBRACKET &&
           p->current.type != TOK_EOF) {
        ast_list_append(list, parse_expr(p));
    }

    if (p->current.type != TOK_RBRACKET) {
        compiler_error(p->current.line, p->current.column, "Expected ']'");
    }

    int end_column = p->current.column + 1;
    p->current = lexer_next_token(p->lexer);

    list->line = start_line;
    list->column = start_column;
    list->end_column = end_column;
    return list;
}

static double parse_number_str(const char *s) {
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) return (double)strtol(s,NULL,16);
    if (s[0]=='0' && (s[1]=='b'||s[1]=='B')) return (double)strtol(s+2,NULL,2);
    if (s[0]=='0' && (s[1]=='o'||s[1]=='O')) return (double)strtol(s+2,NULL,8);
    return atof(s);
}

static AST *parse_expr(Parser *p) {
    Token tok = p->current;

    switch (tok.type) {
    case TOK_NUMBER: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);

        // DEBUG
        fprintf(stderr, ">>> PARSE NUMBER: tok.value='%s'\n", tok.value ? tok.value : "NULL");

        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_number(parse_number_str(tok.value), tok.value);

        // DEBUG
        fprintf(stderr, ">>> AST CREATED: literal_str='%s', number=%g\n",
                ast->literal_str ? ast->literal_str : "NULL", ast->number);

        ast->line = tok.line;
        ast->column = tok.column;
        ast->end_column = end_col;
        return ast;
    }
    case TOK_SYMBOL: {
        int end_col = tok.column + (tok.value ? strlen(tok.value) : 1);
        p->current = lexer_next_token(p->lexer);
        AST *ast = ast_new_symbol(tok.value);
        ast->line = tok.line;
        ast->column = tok.column;
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
    case TOK_LBRACKET:
        return parse_bracket_list(p);
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
    default:
        compiler_error(tok.line, tok.column, "unexpected token type: %d", tok.type);
    }
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
        AST *expr = parse_expr(&p);

        if (list.count >= capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            list.exprs = realloc(list.exprs, sizeof(AST *) * capacity);
        }
        list.exprs[list.count++] = expr;
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
