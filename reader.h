#ifndef READER_H
#define READER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct Type;
struct AST;

/// AST

typedef enum {
    AST_NUMBER,
    AST_SYMBOL,
    AST_STRING,
    AST_CHAR,
    AST_LIST,
    AST_KEYWORD,    // :keyword
    AST_RATIO,      // ratio literal (1/3)
    AST_ARRAY,      // array literal [1 2 3]
    AST_LAMBDA,     // (lambda ([x :: T] -> ... -> Ret<T>) "doc" body)
    AST_ASM,        // (asm instruction operand1 operand2 ...)
    AST_REFINEMENT, // (type Positive { x ∈ Int | (> x 0) })
    AST_TESTS,      // (tests ...)
    AST_ADDRESS_OF, // &expr
    AST_RANGE,      // (0..10) [0..10] [1,3..40] (0..)
    AST_LAYOUT,     // (layout Name [field :: Type] ... :packed True :align 16)
    AST_SET,        // {val val val}
    AST_MAP,        // #{"key" val "key" val}
    AST_PMATCH,     // pattern matching clauses
    TOK_LAMBDA_LIT, // λx. — pure lambda calculus literal
} ASTType;

// A single parsed function parameter: name + optional type annotation
typedef struct ASTParam {
    char *name;       // parameter name (generated if is_anon)
    char *type_name;  // type annotation string, NULL if absent
    bool is_rest;     // Variadic . args
    bool is_anon;     // Name was generated, user wrote type only e.g. Int
} ASTParam;

/// Pattern matching

typedef enum {
    PAT_WILDCARD,       // _
    PAT_VAR,            // x  (binds name)
    PAT_LITERAL_INT,    // 0, 42, -1
    PAT_LITERAL_FLOAT,  // 3.14
    PAT_LIST_EMPTY,     // []
    PAT_LIST,           // [p1 p2 ... | tail?]
} PatternKind;

typedef struct ASTPattern {
    PatternKind kind;
    char       *var_name;           // PAT_VAR: bound name
    double      lit_value;          // PAT_LITERAL_INT / PAT_LITERAL_FLOAT
    // PAT_LIST:
    struct ASTPattern *elements;    // per-element sub-patterns
    int                element_count;
    struct ASTPattern *tail;        // NULL or PAT_VAR/PAT_WILDCARD after |
} ASTPattern;

// One clause: patterns (one per param) + body
typedef struct ASTPMatchClause {
    ASTPattern  *patterns;    // one pattern per matched param
    int          pattern_count;
    struct AST  *body;
} ASTPMatchClause;

// A single field in a layout definition
typedef struct ASTLayoutField {
    char *name;        // field name
    char *type_name;   // base type (e.g. "Float", "Point") — NULL if array
    bool  is_array;    // true if [ElemType Size] sugar was used
    char *array_elem;  // element type name, NULL if not array
    int   array_size;  // array size, -1 if not specified
} ASTLayoutField;

typedef struct AST {
    ASTType type;
    union {
        double number;
        char *symbol;
        char *string;
        char character;
        char *keyword;  // keyword name (without the ':')

        // AST_RATIO
        struct {
            long long numerator;
            long long denominator;
        } ratio;

        // AST_LIST
        struct {
            struct AST **items;
            size_t count;
            size_t capacity;
        } list;

        // AST_ARRAY
        struct {
            struct AST **elements;
            size_t element_count;
            size_t element_capacity;
        } array;

        // AST_LAMBDA
        struct {
            ASTParam *params;  // parameter list
            int param_count;
            char *return_type; // return type name, NULL if absent
            char *docstring;   // NULL if absent
            char *alias_name;  // NULL if absent
            bool naked;
            struct AST *body;  // last expression (return value) - kept for compatibility
            struct AST **body_exprs; // all body expressions
            int body_count;
        } lambda;

        // AST_ASM - inline assembly block
        struct {
            struct AST **instructions; // list of AST lists (instruction + operands)
            size_t instruction_count;
        } asm_block;

        // AST_TYPE_ALIAS
        struct {
            char *alias_name;   // "Code"
            char *target_name;  // "List"
        } type_alias;

        // AST_REFINEMENT
        struct {
            char       *name;        // "Positive"
            char       *var;         // "x"
            char       *base_type;   // "Int"
            struct AST *predicate;   // the expression after |
            char       *docstring;   // NULL if absent
            char       *alias_name;  // NULL if absent
        } refinement;

        struct {
            struct AST **assertions;
            int count;
        } tests;

        // AST_RANGE
        struct {
            struct AST *start;   // always present
            struct AST *step;    // NULL if not specified
            struct AST *end;     // NULL if infinite
            bool is_array;       // true = [...], false = (...)
        } range;

        // AST_LAYOUT
        struct {
            char           *name;
            ASTLayoutField *fields;
            int             field_count;
            bool            packed;   // :packed True/False
            int             align;    // :align N, 0 = natural
        } layout;

        // AST_SET
        struct {
            struct AST **elements;
            size_t       element_count;
            size_t       element_capacity;
        } set;

        // AST_MAP
        struct {
            struct AST **keys;
            struct AST **vals;
            size_t       count;
            size_t       capacity;
        } map;

        // AST_PMATCH
        struct {
            ASTPMatchClause *clauses;
            int              clause_count;
        } pmatch;
    };

    char *literal_str; // original literal string for numbers (e.g. "0xFF")
    // Raw integer value for hex/bin/oct literals that exceed double precision
    uint64_t raw_int;
    bool     has_raw_int;


    // HM type inference result — set by infer_zonk_ast, NULL before inference
    struct Type *inferred_type;


    // Location tracking
    int line;
    int column;
    int end_column;
} AST;

AST *ast_new_number(double value, const char *literal);
AST *ast_new_symbol(const char *name);
AST *ast_new_string(const char *value);
AST *ast_new_char(char value);
AST *ast_new_keyword(const char *name);
AST *ast_new_ratio(long long numerator, long long denominator);
AST *ast_new_array(void);
AST *ast_new_list(void);
AST *ast_new_lambda(ASTParam *params, int param_count,
                    const char *return_type,
                    const char *docstring,
                    const char *alias_name,
                    bool naked,
                    AST *body,
                    AST **body_exprs,
                    int body_count);
AST *ast_new_asm(AST **instructions, size_t instruction_count);
AST *ast_new_type_alias(const char *alias_name, const char *target_name);
AST *ast_new_refinement(const char *name, const char *var,
                        const char *base_type, AST *predicate,
                        const char *docstring, const char *alias_name);
AST *ast_new_address_of(AST *operand);
AST *ast_new_range(AST *start, AST *step, AST *end, bool is_array);
AST *ast_new_layout(const char *name,
                    ASTLayoutField *fields, int field_count,
                    bool packed, int align);
AST *ast_new_set(void);
AST *ast_new_map(void);
AST *ast_new_pmatch(ASTPMatchClause *clauses, int clause_count);
void ast_pattern_free(ASTPattern *p);


void ast_list_append(AST *list, AST *item);
void ast_array_append(AST *array, AST *item);
AST *ast_clone(AST *ast);
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
    TOK_KEYWORD,
    TOK_QUOTE,          // '
    TOK_ARROW,          // ->
    TOK_FEATURE_BEGIN,  // #+
    TOK_FEATURE_END,    // #---
    TOK_DOTDOT,         // ..
    TOK_LBRACE,         // {
    TOK_RBRACE,         // }
    TOK_HASH_LBRACE,    // #{
    TOK_PIPE,           // |
    TOK_BACKTICK,       // `
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
    Lexer *lexer;
    Token  current;
} Parser;

AST *parse_expr(Parser *p);

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

char *ast_to_string(AST *ast);
AST  *desugar_cond_ast(AST *cond_list);
AST  *desugar_let_ast(AST *let_list);


/// ERROR Handling

#include <setjmp.h>
#include <stdbool.h>
extern jmp_buf  g_reader_escape;
extern bool     g_reader_escape_set;
extern char     g_reader_error_msg[512];

#endif
