#ifndef READER_H
#define READER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct Type;
struct AST;
typedef struct AST AST;

extern const char *current_filename;
extern const char *current_source;

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
    AST_DATA,       // (data Color Red | Green | Blue)
    AST_CLASS,      // (class Eq a where ...)
    AST_INSTANCE,   // (instance Eq TrafficLight where ...)
    TOK_LAMBDA_LIT, // λx. — pure lambda calculus literal
} ASTType;

// A single parsed function parameter: name + optional type annotation
typedef struct ASTParam {
    char *name;       // parameter name (generated if is_anon)
    char *type_name;  // type annotation string, NULL if absent
    bool is_rest;     // Variadic . args
    bool is_anon;     // Name was generated, user wrote type only e.g. Int
} ASTParam;


/// Comment map — pre-scanned -| ... |- pairs

typedef struct CommentSpan {
    int open_pos;   /* position of '-' in '-|' */
    int close_pos;  /* position of '-' in '|-', or -1 if paragraph */
    int para_end;   /* if paragraph: position after last comment char */
} CommentSpan;

extern CommentSpan *g_comment_spans;
extern int          g_comment_count;
extern int          g_comment_cap;

void comment_map_build(const char *source);

/// Pattern matching

typedef enum {
    PAT_WILDCARD,       // _
    PAT_VAR,            // x  (binds name)
    PAT_LITERAL_INT,    // 0, 42, -1
    PAT_LITERAL_FLOAT,  // 3.14
    PAT_LITERAL_STRING, // "abc"
    PAT_LIST_EMPTY,     // []
    PAT_LIST,           // [p1 p2 ... | tail?]
    PAT_CONSTRUCTOR,    // Red, Green, Circle, Rectangle ...
} PatternKind;

typedef struct ASTPattern {
    PatternKind kind;
    char       *var_name;           // PAT_VAR: bound name
    double      lit_value;          // PAT_LITERAL_INT / PAT_LITERAL_FLOAT
    // PAT_LIST:
    struct ASTPattern *elements;    // per-element sub-patterns
    int                element_count;
    struct ASTPattern *tail;        // NULL or PAT_VAR/PAT_WILDCARD after |
    /* For PAT_CONSTRUCTOR: field sub-patterns */
    struct ASTPattern *ctor_fields;
    int         ctor_field_count;
} ASTPattern;

// One clause: patterns (one per param) + body
typedef struct ASTPMatchClause {
    ASTPattern  *patterns;    // one pattern per matched param
    int          pattern_count;
    struct AST  *body;
    // guarded clause: | cond -> body | cond -> body ...
    struct AST **guard_conds;
    struct AST **guard_bodies;
    int          guard_count;
} ASTPMatchClause;

// A single constructor in a data definition
// e.g. Circle Float  or  Rectangle Float Float
typedef struct ASTDataConstructor {
    char  *name;         // "Circle"
    char **field_types;  // ["Float"] or ["Float", "Float"]
    int    field_count;
} ASTDataConstructor;

// A single field in a layout definition
typedef struct ASTLayoutField {
    char *name;        // field name
    char *type_name;   // base type (e.g. "Float", "Point") — NULL if array
    bool  is_array;    // true if [ElemType Size] sugar was used
    bool  is_ptr;      // true if field used -> (pointer), false if :: (inline)
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

        // AST_DATA
        struct {
            char *name;           // "Color", "Shape"
            ASTDataConstructor *constructors;
            int constructor_count;
            char **deriving;      // ["show", "eq", ...]
            int deriving_count;
        } data;

        // AST_CLASS
        struct {
            char  *name;           // "Eq"
            char  *type_var;       // "a"
            // Method signatures: name + type string
            char **method_names;   // ["=", "!="]
            char **method_types;   // ["a -> a -> Bool", "a -> a -> Bool"]
            int    method_count;
            // Default implementations: method name + pmatch body AST
            char **default_names;  // ["!="]
            AST  **default_bodies; // [lambda AST for default impl]
            int    default_count;
        } class_decl;

        // AST_INSTANCE
        struct {
            char  *class_name;     // "Eq"
            char  *type_name;      // "TrafficLight"
            // Method implementations: name + pmatch clauses as lambda
            char **method_names;
            AST  **method_bodies;  // lambda ASTs
            int    method_count;
        } instance_decl;
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
AST *ast_new_data(const char *name,
                  ASTDataConstructor *constructors, int constructor_count,
                  char **deriving, int deriving_count);
AST *ast_new_class(const char *name, const char *type_var,
                   char **method_names, char **method_types, int method_count,
                   char **default_names, AST **default_bodies, int default_count);
AST *ast_new_instance(const char *class_name, const char *type_name,
                      char **method_names, AST **method_bodies, int method_count);
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
    TOK_DOT,            // .field  (postfix field access)
    TOK_LBRACE,         // {
    TOK_RBRACE,         // }
    TOK_HASH_LBRACE,    // #{
    TOK_PIPE,           // |
    TOK_LINE_DIRECTIVE, // #line N COL (internal, emitted by wisp, never reaches parser)
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

AST  *desugar_cond_ast(AST *cond_list);
AST  *desugar_let_ast(AST *let_list);

// Parse an I<n>/U<n> type name. Returns true and fills *out_width and
// *out_signed when the name matches the pattern and the width is valid.
// Calls compiler_error (fatal) when the pattern matches but width is bad.
// Returns false when the name does not match I/U at all.
bool parse_int_type(const char *name, int line, int col,
                    int *out_width, bool *out_signed);

/// String builder

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} SB;

void sb_init(SB *b);
void sb_free(SB *b);
char *sb_take(SB *b);
void sb_putc(SB *b, char c);
void sb_puts(SB *b, const char *s);

/// Ast -> Json

char *ast_to_json(AST *ast);

/// ERROR Handling

#include <setjmp.h>
#include <stdbool.h>
extern jmp_buf  g_reader_escape;
extern bool     g_reader_escape_set;
extern char     g_reader_error_msg[512];

// Source map: set by the lexer when it sees a #line N COL directive
// emitted by the wisp transformer. Shifts all subsequent line/column
// reporting back to original source coordinates.
extern int g_srcmap_line_bias;
extern int g_srcmap_col_bias;
extern int g_srcmap_abs_line;    // when >0, overrides lex->line directly
extern int g_quote_depth;        // >0 means we are inside a quoted form

// Param-kind lookup hook — set by wisp before parsing so the reader
// can decide whether a symbol in non-head position is infix or an argument.
// Returns 1 if the given argument slot of `func_name` expects a function
// (i.e. PARAM_FUNC), 0 otherwise. NULL = always return 0.
extern int (*g_param_kind_is_func)(const char *func_name, int arg_index);
extern int (*g_is_known_function)(const char *name);

#endif
