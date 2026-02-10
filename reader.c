#include "reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Helpers

static char *my_strndup(const char *s, size_t n) {
    char *result = malloc(n + 1);
    if (!result) return NULL;
    memcpy(result, s, n);
    result[n] = '\0';
    return result;
}

static char *my_strdup(const char *s) {
    size_t len = strlen(s);
    return my_strndup(s, len);
}

/// AST Implementation

AST *ast_new_number(double value, const char *literal) {
    AST *ast = malloc(sizeof(AST));
    ast->type = AST_NUMBER;
    ast->number = value;
    ast->literal_str = literal ? my_strdup(literal) : NULL;
    return ast;
}

AST *ast_new_symbol(const char *name) {
    AST *ast = malloc(sizeof(AST));
    ast->type = AST_SYMBOL;
    ast->symbol = my_strdup(name);
    ast->literal_str = NULL;
    return ast;
}

AST *ast_new_string(const char *value) {
    AST *ast = malloc(sizeof(AST));
    ast->type = AST_STRING;
    ast->string = my_strdup(value);
    ast->literal_str = NULL;
    return ast;
}

AST *ast_new_char(char value) {
    AST *ast = malloc(sizeof(AST));
    ast->type = AST_CHAR;
    ast->character = value;
    ast->literal_str = NULL;
    return ast;
}

AST *ast_new_list(void) {
    AST *ast = malloc(sizeof(AST));
    ast->type = AST_LIST;
    ast->list.count = 0;
    ast->list.capacity = 4;
    ast->list.items = malloc(sizeof(AST*) * ast->list.capacity);
    ast->literal_str = NULL;
    return ast;
}

void ast_list_append(AST *list, AST *item) {
    if (list->list.count >= list->list.capacity) {
        list->list.capacity *= 2;
        list->list.items = realloc(list->list.items,
                                   sizeof(AST*) * list->list.capacity);
    }
    list->list.items[list->list.count++] = item;
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
        for (size_t i = 0; i < ast->list.count; i++) {
            ast_free(ast->list.items[i]);
        }
        free(ast->list.items);
        break;
    default:
        break;
    }
    if (ast->literal_str) free(ast->literal_str);
    free(ast);
}

void ast_print(AST *ast) {
    if (!ast) {
        printf("nil");
        return;
    }

    switch (ast->type) {
    case AST_NUMBER:
        printf("%g", ast->number);
        break;
    case AST_SYMBOL:
        printf("%s", ast->symbol);
        break;
    case AST_STRING:
        printf("\"%s\"", ast->string);
        break;
    case AST_CHAR:
        printf("'%c'", ast->character);
        break;
    case AST_LIST:
        printf("(");
        for (size_t i = 0; i < ast->list.count; i++) {
            if (i > 0) printf(" ");
            ast_print(ast->list.items[i]);
        }
        printf(")");
        break;
    }
}

/// Lexer Implementation

void lexer_init(Lexer *lex, const char *source) {
    lex->source = source;
    lex->pos = 0;
    lex->line = 1;
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
    if (c == '\n') {
        lex->line++;
        lex->column = 1;
    } else {
        lex->column++;
    }
    return c;
}

static void skip_whitespace(Lexer *lex) {
    while (peek(lex) == ' ' || peek(lex) == '\t' ||
           peek(lex) == '\n' || peek(lex) == '\r') {
        advance(lex);
    }
}

static void skip_line_comment(Lexer *lex) {
    while (peek(lex) != '\n' && peek(lex) != '\0') {
        advance(lex);
    }
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_symbol_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '+' || c == '*' || c == '/' ||
           c == '<' || c == '>' || c == '=' || c == '!' ||
           c == '?' || c == '_' || c == ':';
}

Token lexer_next_token(Lexer *lex) {
    Token tok = {0};

    skip_whitespace(lex);

    while (peek(lex) == ';') {
        skip_line_comment(lex);
        skip_whitespace(lex);
    }

    tok.line = lex->line;
    tok.column = lex->column;

    char c = peek(lex);

    if (c == '\0') {
        tok.type = TOK_EOF;
        return tok;
    }

    if (c == '(') {
        advance(lex);
        tok.type = TOK_LPAREN;
        return tok;
    }

    if (c == ')') {
        advance(lex);
        tok.type = TOK_RPAREN;
        return tok;
    }

    if (c == '[') {
        advance(lex);
        tok.type = TOK_LBRACKET;
        return tok;
    }

    if (c == ']') {
        advance(lex);
        tok.type = TOK_RBRACKET;
        return tok;
    }

    if (c == '\'') {
        // Look ahead to determine if it's a char literal or quote
        // Char literal: 'x' (exactly 3 chars) or '\n' (escape, 4 chars)
        // Quote: '(expr) or 'symbol

        size_t lookahead_pos = lex->pos + 1;
        if (lookahead_pos < strlen(lex->source)) {
            char next = lex->source[lookahead_pos];

            // Check for character literal pattern
            if (next == '\\') {
                // Escape sequence: '\n', '\t', etc.
                if (lookahead_pos + 2 < strlen(lex->source) &&
                    lex->source[lookahead_pos + 2] == '\'') {
                    // It's a char literal with escape
                    advance(lex); // skip opening '
                    advance(lex); // skip '\'
                    char ch = peek(lex);
                    // Handle escape sequences
                    switch (ch) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case '\\': ch = '\\'; break;
                    case '\'': ch = '\''; break;
                    case '0': ch = '\0'; break;
                    }
                    advance(lex); // skip escape char
                    if (peek(lex) != '\'') {
                        fprintf(stderr, "Unterminated character literal at %d:%d\n",
                                lex->line, lex->column);
                        exit(1);
                    }
                    advance(lex); // skip closing '
                    tok.value = malloc(2);
                    tok.value[0] = ch;
                    tok.value[1] = '\0';
                    tok.type = TOK_CHAR;
                    return tok;
                }
            } else if (next != '\'' && next != '\0') {
                // Check if there's a closing quote 2 positions ahead
                if (lookahead_pos + 1 < strlen(lex->source) &&
                    lex->source[lookahead_pos + 1] == '\'') {
                    // It's a char literal: 'x'
                    advance(lex); // skip opening '
                    char ch = peek(lex);
                    advance(lex); // skip the character
                    if (peek(lex) != '\'') {
                        fprintf(stderr, "Unterminated character literal at %d:%d\n",
                                lex->line, lex->column);
                        exit(1);
                    }
                    advance(lex); // skip closing '
                    tok.value = malloc(2);
                    tok.value[0] = ch;
                    tok.value[1] = '\0';
                    tok.type = TOK_CHAR;
                    return tok;
                }
            }
        }

        // Otherwise, it's a quote operator
        advance(lex);
        tok.type = TOK_QUOTE;
        return tok;
    }

    if (c == '"') {
        advance(lex);
        size_t start = lex->pos;
        while (peek(lex) != '"' && peek(lex) != '\0') {
            if (peek(lex) == '\\') advance(lex);
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        advance(lex);
        tok.type = TOK_STRING;
        return tok;
    }

    // Handle hex (0x), binary (0b), octal (0o) numbers
    if (c == '0' && (peek_ahead(lex, 1) == 'x' || peek_ahead(lex, 1) == 'X')) {
        size_t start = lex->pos;
        advance(lex); // skip '0'
        advance(lex); // skip 'x'
        while (is_hex_digit(peek(lex))) {
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (c == '0' && (peek_ahead(lex, 1) == 'b' || peek_ahead(lex, 1) == 'B')) {
        size_t start = lex->pos;
        advance(lex); // skip '0'
        advance(lex); // skip 'b'
        while (peek(lex) == '0' || peek(lex) == '1') {
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (c == '0' && (peek_ahead(lex, 1) == 'o' || peek_ahead(lex, 1) == 'O')) {
        size_t start = lex->pos;
        advance(lex); // skip '0'
        advance(lex); // skip 'o'
        while (peek(lex) >= '0' && peek(lex) <= '7') {
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (is_digit(c) || (c == '-' && is_digit(peek_ahead(lex, 1)))) {
        size_t start = lex->pos;
        if (c == '-') advance(lex);
        while (is_digit(peek(lex)) || peek(lex) == '.') {
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (is_symbol_char(c)) {
        size_t start = lex->pos;
        while (is_symbol_char(peek(lex))) {
            advance(lex);
        }
        size_t len = lex->pos - start;
        tok.value = my_strndup(lex->source + start, len);
        tok.type = TOK_SYMBOL;
        return tok;
    }

    fprintf(stderr, "Unexpected character: '%c' at %d:%d\n",
            c, lex->line, lex->column);
    exit(1);
}

/// Parser

typedef struct {
    Lexer *lexer;
    Token current;
} Parser;

static void parser_init(Parser *p, Lexer *lex) {
    p->lexer = lex;
    p->current = lexer_next_token(lex);
}

static AST *parse_expr(Parser *p);

static AST *parse_bracket_list(Parser *p) {
    AST *list = ast_new_list();

    p->current = lexer_next_token(p->lexer);

    while (p->current.type != TOK_RBRACKET && p->current.type != TOK_EOF) {
        AST *item = parse_expr(p);
        ast_list_append(list, item);
    }

    if (p->current.type != TOK_RBRACKET) {
        fprintf(stderr, "Expected ']'\n");
        exit(1);
    }

    p->current = lexer_next_token(p->lexer);

    return list;
}

static AST *parse_list(Parser *p) {
    AST *list = ast_new_list();

    p->current = lexer_next_token(p->lexer);

    while (p->current.type != TOK_RPAREN && p->current.type != TOK_EOF) {
        AST *item = parse_expr(p);
        ast_list_append(list, item);
    }

    if (p->current.type != TOK_RPAREN) {
        fprintf(stderr, "Expected ')'\n");
        exit(1);
    }

    p->current = lexer_next_token(p->lexer);

    return list;
}

static double parse_number(const char *str) {
    // Handle hex
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        return (double)strtol(str, NULL, 16);
    }
    // Handle binary
    if (str[0] == '0' && (str[1] == 'b' || str[1] == 'B')) {
        return (double)strtol(str + 2, NULL, 2);
    }
    // Handle octal
    if (str[0] == '0' && (str[1] == 'o' || str[1] == 'O')) {
        return (double)strtol(str + 2, NULL, 8);
    }
    // Regular decimal
    return atof(str);
}

static AST *parse_expr(Parser *p) {
    Token tok = p->current;

    switch (tok.type) {
    case TOK_NUMBER: {
      p->current = lexer_next_token(p->lexer);
      return ast_new_number(parse_number(tok.value), tok.value);
    }

    case TOK_SYMBOL: {
        p->current = lexer_next_token(p->lexer);
        return ast_new_symbol(tok.value);
    }

    case TOK_STRING: {
        p->current = lexer_next_token(p->lexer);
        return ast_new_string(tok.value);
    }

    case TOK_CHAR: {
        p->current = lexer_next_token(p->lexer);
        return ast_new_char(tok.value[0]);
    }

    case TOK_LPAREN:
        return parse_list(p);

    case TOK_LBRACKET:
        return parse_bracket_list(p);

    case TOK_QUOTE: {
        p->current = lexer_next_token(p->lexer);
        AST *quoted = parse_expr(p);
        AST *list = ast_new_list();
        ast_list_append(list, ast_new_symbol("quote"));
        ast_list_append(list, quoted);
        return list;
    }

    default:
        fprintf(stderr, "Unexpected token type: %d\n", tok.type);
        exit(1);
    }
}

AST *parse(const char *source) {
    Lexer lex;
    lexer_init(&lex, source);

    Parser p;
    parser_init(&p, &lex);

    return parse_expr(&p);
}
