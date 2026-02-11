#ifndef READER_H
#define READER_H
#include <stddef.h>
#include <stdbool.h>

/// AST

typedef enum {
    AST_NUMBER,
    AST_SYMBOL,
    AST_STRING,
    AST_CHAR,
    AST_LIST,
    AST_LAMBDA, // (lambda ([x :: T] -> ... -> Ret<T>) "doc" body)
} ASTType;

// A single parsed function parameter: name + optional type annotation
typedef struct ASTParam {
    char *name;       // parameter name
    char *type_name;  // type annotation string, NULL if absent
} ASTParam;

typedef struct AST {
    ASTType type;
    union {
        double number;
        char *symbol;
        char *string;
        char character;
        // AST_LIST
        struct {
            struct AST **items;
            size_t count;
            size_t capacity;
        } list;
        // AST_LAMBDA
        struct {
            ASTParam *params;  // parameter list
            int param_count;
            char *return_type; // return type name, NULL if absent
            char *docstring;   // NULL if absent
            struct AST *body;  // body expression
        } lambda;
    };
    char *literal_str; // original literal string for numbers (e.g. "0xFF")
    // Location tracking
    int line;
    int column;
    int end_column;
} AST;

AST *ast_new_number(double value, const char *literal);
AST *ast_new_symbol(const char *name);
AST *ast_new_string(const char *value);
AST *ast_new_char(char value);
AST *ast_new_list(void);
AST *ast_new_lambda(ASTParam *params, int param_count,
                     const char *return_type,
                     const char *docstring,
                     AST *body);

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
    TOK_CHAR,
    TOK_QUOTE,
    TOK_ARROW,
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

void  lexer_init(Lexer *lex, const char *source);
Token lexer_next_token(Lexer *lex);

/// Parser

typedef struct {
    AST **exprs;
    size_t count;
} ASTList;

// Set the current file being parsed (for error messages)
void parser_set_context(const char *filename, const char *source);
// Parse all expressions from source
ASTList parse_all(const char *source);
// Parse a single expression (for REPL)
AST *parse(const char *source);
const char *parser_get_filename(void);

#endif
