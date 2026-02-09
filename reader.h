#ifndef READER_H
#define READER_H

#include <stddef.h>
#include <stdbool.h>

/// AST

typedef enum {
    AST_NUMBER,
    AST_SYMBOL,
    AST_STRING,
    AST_LIST,
} ASTType;

typedef struct AST {
    ASTType type;
    union {
        double number;
        char *symbol;
        char *string;
        struct {
            struct AST **items;
            size_t count;
            size_t capacity;
        } list;
    };
} AST;

AST *ast_new_number(double value);
AST *ast_new_symbol(const char *name);
AST *ast_new_string(const char *value);
AST *ast_new_list(void);
void ast_list_append(AST *list, AST *item);
void ast_free(AST *ast);
void ast_print(AST *ast);

/// Lexer

typedef enum {
    TOK_EOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_SYMBOL,
    TOK_NUMBER,
    TOK_STRING,
    TOK_QUOTE,
} TokenType;

typedef struct {
    TokenType type;
    char *value;
    int line;
    int column;
} Token;

typedef struct {
    const char *source;
    size_t pos;
    int line;
    int column;
} Lexer;

void lexer_init(Lexer *lex, const char *source);
Token lexer_next_token(Lexer *lex);

/// Parser

AST *parse(const char *source);

#endif
