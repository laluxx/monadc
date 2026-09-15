#include "typst_emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

//
// NOTE This is experimental, and doesn't look good yet.
// This isn't a priority for me, but I liked the idea --
// so I gave it the initial push torwards the right direction.
// If you are interested in making this beautiful, please do so.
//
//
// With this function as input I imagine
// a trnasformation similar to this:
//
// define foldl :: (b -> a -> b) -> b -> [a] -> b
//   _ acc []     -> acc
//   f acc [x|xs] -> foldl f (f acc x) xs
//
//                  ↓
//
// $"foldl" : (b -> a -> b) -> b -> [a] -> b$
//
// $
// "foldl"(f, "acc", "coll") = cases(
//   "acc" & "if" "coll" = [],
//   "foldl"(f, f("acc", x), "xs") & "if" "coll" = [x | "xs"]
// )
// $
//
// The main obstacle is that by the time the emitter sees
// a (define ...) node, the pattern match clauses have already
// been desugared into nested (if ...) expressions by
// pmatch_desugar() in the parser. The raw ASTPMatchClause data is
// gone. The clean fix is in parse_list(): preserve the raw AST_PMATCH
// node before calling pmatch_desugar(), and attach it to the define
// list node as an extra item (items[3]). The emitter can then read
// it directly instead of trying to reconstruct clauses from if-chains.
//

/// Internal helpers — string builder shims
//
//  Thin wrappers around the SB API so the rest of the file stays terse.
//  EMITF is limited to 512 bytes per call; use it only for short fragments.
//
#define EMIT(b, s)          sb_puts((b), (s))
#define EMITC(b, c)         sb_putc((b), (c))
#define EMITF(b, fmt, ...) \
    do { \
        char _buf[512]; \
        snprintf(_buf, sizeof(_buf), (fmt), ##__VA_ARGS__); \
        sb_puts((b), _buf); \
    } while (0)

// Parenthesise when the child's precedence is strictly weaker than the
// surrounding context.  Higher number = tighter.
#define NEEDS_PARENS(ctx_prec, child_prec)  ((child_prec) < (ctx_prec))

// Typst math parentheses use lr() so delimiters scale with content.
#define LPAREN(b)  EMIT((b), "lr(\"(\"")
#define RPAREN(b)  EMIT((b), "\")\"|)")

static const TypstEmitOpts g_default_opts = {
    .mode                  = TYPST_DOC_FULL,
    .prefer_inferred_types = true,
    .sequent_style         = true,
    .emit_labels           = true,
    .number_equations      = false,
    .title                 = "Program Listing",
    .author                = "",
    .paper                 = "a4",
};

static const TypstEmitOpts *opts_or_default(const TypstEmitOpts *o) {
    return o ? o : &g_default_opts;
}


/// Symbol → Typst math table
//
//  Maps every operator and special symbol to its canonical Typst math atom.
//  Typst math uses plain words for most symbols (no backslash), which makes
//  the table much cleaner than the equivalent LaTeX one.
//
//  Columns: { symbol string, typst math atom, precedence, is_infix }
//
typedef struct { const char *sym; const char *typst; int prec; bool infix; } SymEntry;

static const SymEntry g_sym_table[] = {
    // Arithmetic
    { "%",    "mod",              TYPST_PREC_MUL,  true  },
    { "*",    "times",            TYPST_PREC_MUL,  true  },
    { "+",    "+",                TYPST_PREC_ADD,  true  },
    { "-",    "-",                TYPST_PREC_ADD,  true  },
    { "/",    "/",                TYPST_PREC_MUL,  true  },
    // Comparison
    { "!=",   "eq.not",           TYPST_PREC_CMP,  true  },
    { "<",    "<",                TYPST_PREC_CMP,  true  },
    { "<=",   "lt.eq",            TYPST_PREC_CMP,  true  },
    { "=",    "=",                TYPST_PREC_CMP,  true  },
    { "==",   "equiv",            TYPST_PREC_CMP,  true  },
    { ">",    ">",                TYPST_PREC_CMP,  true  },
    { ">=",   "gt.eq",            TYPST_PREC_CMP,  true  },
    // Logical
    { "&&",   "and",              TYPST_PREC_AND,  true  },
    { "and",  "and",              TYPST_PREC_AND,  true  },
    { "not",  "not",              TYPST_PREC_UNARY,false },
    { "or",   "or",               TYPST_PREC_OR,   true  },
    { "||",   "or",               TYPST_PREC_OR,   true  },
    // Type-level
    { "->",   "arrow.r",          TYPST_PREC_ARROW,true  },
    { "::",   ":",                TYPST_PREC_TOP,  false },
    { "∀",    "forall",           TYPST_PREC_TOP,  false },
    { "∃",    "exists",           TYPST_PREC_TOP,  false },
    { "∈",    "in",               TYPST_PREC_CMP,  true  },
    { "∉",    "in.not",           TYPST_PREC_CMP,  true  },
    { "⊂",    "subset",           TYPST_PREC_CMP,  true  },
    { "⊆",    "subset.eq",        TYPST_PREC_CMP,  true  },
    { "⊢",    "tack.r",           TYPST_PREC_TOP,  false },
    { "≡",    "equiv",            TYPST_PREC_CMP,  true  },
    { "≠",    "eq.not",           TYPST_PREC_CMP,  true  },
    { "≤",    "lt.eq",            TYPST_PREC_CMP,  true  },
    { "≥",    "gt.eq",            TYPST_PREC_CMP,  true  },
    { "λ",    "lambda",           TYPST_PREC_TOP,  false },
    // List / collection
    { "cons", "::",               TYPST_PREC_ADD,  true  },
    { "++",   "+",                TYPST_PREC_ADD,  true  },
};

#define SYM_TABLE_LEN  (int)(sizeof(g_sym_table) / sizeof(g_sym_table[0]))

const char *typst_symbol_map(const char *sym) {
    if (!sym) return NULL;
    for (int i = 0; i < SYM_TABLE_LEN; i++)
        if (strcmp(g_sym_table[i].sym, sym) == 0)
            return g_sym_table[i].typst;
    return NULL;
}

bool typst_is_infix(const char *sym) {
    for (int i = 0; i < SYM_TABLE_LEN; i++)
        if (strcmp(g_sym_table[i].sym, sym) == 0)
            return g_sym_table[i].infix;
    return false;
}

int typst_op_precedence(const char *sym) {
    for (int i = 0; i < SYM_TABLE_LEN; i++)
        if (strcmp(g_sym_table[i].sym, sym) == 0)
            return g_sym_table[i].prec;
    return TYPST_PREC_APP;
}


/// Identifier escaping
//
//  Typst math mode treats multi-character names as sequences of separate
//  glyphs unless they are quoted with  "name"  or wrapped in upright().
//  We use upright() for multi-char identifiers so they render as roman
//  text inside a math block, matching standard mathematical convention
//  for named functions and type constructors.
//
//  Single ASCII letters are emitted bare (they are already italic in math).
//
//  Characters that are special inside Typst strings ( " \ ) are escaped.
//
static void emit_typst_string_content(const char *s, SB *out) {
    for (; *s; s++) {
        switch (*s) {
        case '"':  EMIT(out, "\\\""); break;
        case '\\': EMIT(out, "\\\\"); break;
        default:   EMITC(out, *s);   break;
        }
    }
}

static void emit_ident(const char *name, SB *out, const TypstEmitOpts *opts) {
    (void)opts;
    const char *mapped = typst_symbol_map(name);
    if (mapped) { EMIT(out, mapped); return; }

    // Single ASCII letter → bare math italic
    if (name[0] != '\0' && name[1] == '\0' && isalpha((unsigned char)name[0])) {
        EMITC(out, name[0]);
        return;
    }

    // Multi-character: upright("name") keeps it roman in math mode
    EMIT(out, "upright(\"");
    emit_typst_string_content(name, out);
    EMIT(out, "\")");
}

// Emit a type name.  Built-in numeric types use blackboard-bold via
// Typst's  bb("Z")  notation; all others use upright.
static void emit_type_name(const char *name, SB *out) {
    if (!name) return;
    if (strcmp(name, "Int")    == 0) { EMIT(out, "bb(ZZ)");         return; }
    if (strcmp(name, "Float")  == 0) { EMIT(out, "bb(RR)");         return; }
    if (strcmp(name, "Bool")   == 0) { EMIT(out, "bb(BB)");         return; }
    if (strcmp(name, "String") == 0) { EMIT(out, "upright(\"String\")"); return; }
    if (strcmp(name, "Char")   == 0) { EMIT(out, "upright(\"Char\")");   return; }
    if (strcmp(name, "Unit")   == 0) { EMIT(out, "bold(1)");        return; }
    if (strcmp(name, "Void")   == 0) { EMIT(out, "bold(0)");        return; }
    // I<n>/U<n> fixed-width integers → ℤ_n / ℕ_n
    if ((name[0] == 'I' || name[0] == 'U') && isdigit((unsigned char)name[1])) {
        char set = (name[0] == 'I') ? 'Z' : 'N';
        EMITF(out, "bb(%c)_%s", set, name + 1);
        return;
    }
    EMIT(out, "upright(\"");
    emit_typst_string_content(name, out);
    EMIT(out, "\")");
}


/// Pattern emission
//
//  Patterns are emitted as math atoms on the left-hand side of pmatch
//  clauses.  Constructor patterns are  C space p₁ space … space pₙ.
//  List patterns use Typst's bracket notation.
//
void typst_emit_pattern(ASTPattern *pat, SB *out, const TypstEmitOpts *opts) {
    if (!pat) { EMIT(out, "_"); return; }
    opts = opts_or_default(opts);

    switch (pat->kind) {
    case PAT_WILDCARD:
        EMIT(out, "_");
        break;

    case PAT_VAR:
        emit_ident(pat->var_name, out, opts);
        break;

    case PAT_LITERAL_INT:
        EMITF(out, "%lld", (long long)pat->lit_value);
        break;

    case PAT_RANGE_INT:
        EMITF(out, "%lld..%lld", (long long)pat->lit_value,
              (long long)pat->range_end);
        break;

    case PAT_LITERAL_FLOAT:
        EMITF(out, "%g", pat->lit_value);
        break;

    case PAT_LITERAL_STRING:
        // Typst math: use  "text"  for string literals
        EMIT(out, "\"");
        emit_typst_string_content(pat->var_name, out);
        EMIT(out, "\"");
        break;

    case PAT_LIST_EMPTY:
        EMIT(out, "[]");
        break;

    case PAT_LIST:
        EMIT(out, "[");
        for (int i = 0; i < pat->element_count; i++) {
            if (i > 0) EMIT(out, ", ");
            typst_emit_pattern(&pat->elements[i], out, opts);
        }
        if (pat->tail) {
            EMIT(out, " | ");
            typst_emit_pattern(pat->tail, out, opts);
        }
        EMIT(out, "]");
        break;

    case PAT_CONSTRUCTOR:
        emit_type_name(pat->var_name, out);
        for (int i = 0; i < pat->ctor_field_count; i++) {
            EMIT(out, " space ");
            typst_emit_pattern(&pat->ctor_fields[i], out, opts);
        }
        break;
    }
}


/// Type expression emission
//
//  Handles the three forms a type can take in the AST:
//    AST_SYMBOL   — type name or type variable
//    AST_LIST     — type application or arrow type  (-> A B)
//    AST_ARRAY    — list type  [T]
//
//  Arrow types are right-associative:  A → B → C  renders without parens,
//  but  (A → B) → C  requires the left side to be parenthesised.
//
void typst_emit_type(AST *ast, SB *out, const TypstEmitOpts *opts) {
    if (!ast) { EMIT(out, "?"); return; }
    opts = opts_or_default(opts);

    switch (ast->type) {
    case AST_SYMBOL:
        emit_type_name(ast->symbol, out);
        break;

    case AST_LIST: {
        if (ast->list.count == 0) { EMIT(out, "()"); break; }
        AST *head = ast->list.items[0];
        // Arrow type:  (-> A B C)  →  A arrow.r B arrow.r C
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "->") == 0) {
            for (size_t i = 1; i < ast->list.count; i++) {
                if (i > 1) EMIT(out, " arrow.r ");
                bool needs_paren = ast->list.items[i]->type == AST_LIST;
                if (needs_paren) EMIT(out, "(");
                typst_emit_type(ast->list.items[i], out, opts);
                if (needs_paren) EMIT(out, ")");
            }
            break;
        }
        // Type application:  (Maybe a)  →  upright("Maybe") space a
        typst_emit_type(head, out, opts);
        for (size_t i = 1; i < ast->list.count; i++) {
            EMIT(out, " space ");
            typst_emit_type(ast->list.items[i], out, opts);
        }
        break;
    }

    case AST_ARRAY:
        EMIT(out, "[");
        if (ast->array.element_count == 1)
            typst_emit_type(ast->array.elements[0], out, opts);
        EMIT(out, "]");
        break;

    default:
        typst_emit_expr(ast, out, TYPST_PREC_TOP, opts);
        break;
    }
}


/// Expression emission — core recursive function
//
//  Every AST node kind is handled here.  The `prec` argument is the
//  precedence of the surrounding context; we wrap in lr("("…")") when
//  the current node's natural precedence is strictly weaker.
//
//  Left-associative binary operators pass prec+1 on the right operand
//  to force parenthesisation of same-precedence right sub-expressions.
//
void typst_emit_expr(AST *ast, SB *out, int prec, const TypstEmitOpts *opts) {
    if (!ast) return;
    opts = opts_or_default(opts);

    switch (ast->type) {

    // ── Literals ─────────────────────────────────────────────────────────────

    case AST_NUMBER:
        if (ast->has_raw_int)
            EMITF(out, "%llu", (unsigned long long)ast->raw_int);
        else if (ast->literal_str)
            EMIT(out, ast->literal_str);
        else {
            long long iv = (long long)ast->number;
            if ((double)iv == ast->number)
                EMITF(out, "%lld", iv);
            else
                EMITF(out, "%g", ast->number);
        }
        break;

    case AST_RATIO:
        EMIT(out, "frac(");
        EMITF(out, "%lld", ast->ratio.numerator);
        EMIT(out, ", ");
        EMITF(out, "%lld", ast->ratio.denominator);
        EMITC(out, ')');
        break;

    case AST_STRING:
        // Typst math: inline string literals as  "text"
        EMIT(out, "\"");
        emit_typst_string_content(ast->string, out);
        EMIT(out, "\"");
        break;

    case AST_CHAR:
        EMIT(out, "\"");
        EMITC(out, ast->character);
        EMIT(out, "\"");
        break;

    case AST_KEYWORD:
        // Keywords are content-mode atoms; wrap in mono text
        EMIT(out, "mono(\":");
        emit_typst_string_content(ast->keyword, out);
        EMIT(out, "\")");
        break;

    // ── Symbol / identifier ──────────────────────────────────────────────────

    case AST_SYMBOL: {
        const char *mapped = typst_symbol_map(ast->symbol);
        if (mapped) { EMIT(out, mapped); break; }
        emit_ident(ast->symbol, out, opts);
        break;
    }

    // ── Lambda abstraction  λx. body ─────────────────────────────────────────
    //
    //  Both the full AST_LAMBDA (with typed params) and the bare
    //  TOK_LAMBDA_LIT are rendered identically:
    //
    //    lambda x_1 space x_2. space body
    //
    //  When params carry type annotations we emit the bound form
    //
    //    lambda (x_1 : T_1). space body
    //
    case AST_LAMBDA:
    case TOK_LAMBDA_LIT: {
        bool paren = NEEDS_PARENS(prec, TYPST_PREC_ARROW);
        if (paren) EMIT(out, "(");
        EMIT(out, "lambda ");
        for (int i = 0; i < ast->lambda.param_count; i++) {
            ASTParam *p = &ast->lambda.params[i];
            if (i > 0) EMIT(out, " space ");
            if (p->type_name) {
                EMIT(out, "(");
                emit_ident(p->name, out, opts);
                EMIT(out, " : ");
                emit_type_name(p->type_name, out);
                EMIT(out, ")");
            } else {
                emit_ident(p->name, out, opts);
            }
        }
        EMIT(out, ". space ");
        if (ast->lambda.body_count > 1) {
            // Multi-expression body: use a cases block
            EMIT(out, "cases(");
            for (int i = 0; i < ast->lambda.body_count; i++) {
                if (i > 0) EMIT(out, ", ");
                typst_emit_expr(ast->lambda.body_exprs[i], out, TYPST_PREC_TOP, opts);
            }
            EMITC(out, ')');
        } else if (ast->lambda.body) {
            typst_emit_expr(ast->lambda.body, out, TYPST_PREC_ARROW, opts);
        }
        if (paren) EMITC(out, ')');
        break;
    }

    // ── Application and infix operators ──────────────────────────────────────
    //
    //  AST_LIST encodes both function application  (f a b)  and all
    //  special forms.  We detect special forms first, then fall through
    //  to the infix / application cases.
    //
    case AST_LIST: {
        if (ast->list.count == 0) { EMIT(out, "()"); break; }
        AST *head = ast->list.items[0];

        // ── Special forms ───────────────────────────────────────────────────

        // (if cond then else)
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "if") == 0
                && ast->list.count == 4) {
            EMIT(out, "cases(");
            typst_emit_expr(ast->list.items[2], out, TYPST_PREC_TOP, opts);
            EMIT(out, " \"if\" ");
            typst_emit_expr(ast->list.items[1], out, TYPST_PREC_TOP, opts);
            EMIT(out, ", ");
            typst_emit_expr(ast->list.items[3], out, TYPST_PREC_TOP, opts);
            EMIT(out, " \"otherwise\")");
            break;
        }

        // (let [x v] body)
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "let") == 0
                && ast->list.count >= 3) {
            EMIT(out, "bold(\"let\") space ");
            AST *binding = ast->list.items[1];
            if (binding->type == AST_ARRAY && binding->array.element_count == 2) {
                typst_emit_expr(binding->array.elements[0], out, TYPST_PREC_ATOM, opts);
                EMIT(out, " = ");
                typst_emit_expr(binding->array.elements[1], out, TYPST_PREC_TOP, opts);
            } else {
                typst_emit_expr(binding, out, TYPST_PREC_TOP, opts);
                if (ast->list.count >= 4) {
                    EMIT(out, " = ");
                    typst_emit_expr(ast->list.items[2], out, TYPST_PREC_TOP, opts);
                }
            }
            EMIT(out, " space bold(\"in\") space ");
            typst_emit_expr(ast->list.items[ast->list.count - 1],
                            out, TYPST_PREC_TOP, opts);
            break;
        }

        // (do e1 e2 … en) — monadic / sequential composition
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "do") == 0) {
            EMIT(out, "cases(");
            for (size_t i = 1; i < ast->list.count; i++) {
                if (i > 1) EMIT(out, ", ");
                typst_emit_expr(ast->list.items[i], out, TYPST_PREC_TOP, opts);
            }
            EMITC(out, ')');
            break;
        }

        // (:: expr TypeName) — type ascription  e : T
        if (head->type == AST_SYMBOL && strcmp(head->symbol, "::") == 0
                && ast->list.count == 3) {
            bool paren = NEEDS_PARENS(prec, TYPST_PREC_CMP);
            if (paren) EMIT(out, "(");
            typst_emit_expr(ast->list.items[1], out, TYPST_PREC_CMP + 1, opts);
            EMIT(out, " : ");
            typst_emit_type(ast->list.items[2], out, opts);
            if (paren) EMITC(out, ')');
            break;
        }

        // ── Infix binary operator ───────────────────────────────────────────
        if (head->type == AST_SYMBOL && ast->list.count == 3
                && typst_is_infix(head->symbol)) {
            int  op_prec = typst_op_precedence(head->symbol);
            bool paren   = NEEDS_PARENS(prec, op_prec);
            if (paren) EMIT(out, "(");
            typst_emit_expr(ast->list.items[1], out, op_prec,     opts);
            EMITC(out, ' ');
            EMIT(out, typst_symbol_map(head->symbol));
            EMITC(out, ' ');
            typst_emit_expr(ast->list.items[2], out, op_prec + 1, opts);
            if (paren) EMITC(out, ')');
            break;
        }

        // ── Unary prefix operator ───────────────────────────────────────────
        if (head->type == AST_SYMBOL && ast->list.count == 2) {
            const char *mapped = typst_symbol_map(head->symbol);
            if (mapped && !typst_is_infix(head->symbol)) {
                bool paren = NEEDS_PARENS(prec, TYPST_PREC_UNARY);
                if (paren) EMIT(out, "(");
                EMIT(out, mapped);
                EMIT(out, " space ");
                typst_emit_expr(ast->list.items[1], out, TYPST_PREC_UNARY, opts);
                if (paren) EMITC(out, ')');
                break;
            }
        }

        // ── Generic function application  f space a₁ space … space aₙ ───────
        {
            bool paren = NEEDS_PARENS(prec, TYPST_PREC_APP);
            if (paren) EMIT(out, "(");
            typst_emit_expr(head, out, TYPST_PREC_ATOM, opts);
            for (size_t i = 1; i < ast->list.count; i++) {
                EMIT(out, " space ");
                typst_emit_expr(ast->list.items[i], out, TYPST_PREC_ATOM, opts);
            }
            if (paren) EMITC(out, ')');
        }
        break;
    }

    // ── Array literal  [1, 2, 3] ─────────────────────────────────────────────

    case AST_ARRAY:
        EMITC(out, '[');
        for (size_t i = 0; i < ast->array.element_count; i++) {
            if (i > 0) EMIT(out, ", ");
            typst_emit_expr(ast->array.elements[i], out, TYPST_PREC_TOP, opts);
        }
        EMITC(out, ']');
        break;

    // ── Set literal  {1, 2, 3} ───────────────────────────────────────────────

    case AST_SET:
        EMIT(out, "{");
        for (size_t i = 0; i < ast->set.element_count; i++) {
            if (i > 0) EMIT(out, ", ");
            typst_emit_expr(ast->set.elements[i], out, TYPST_PREC_TOP, opts);
        }
        EMIT(out, "}");
        break;

    // ── Map literal  { k₁ ↦ v₁, … } ────────────────────────────────────────

    case AST_MAP:
        EMIT(out, "{");
        for (size_t i = 0; i < ast->map.count; i++) {
            if (i > 0) EMIT(out, ", ");
            typst_emit_expr(ast->map.keys[i], out, TYPST_PREC_TOP, opts);
            EMIT(out, " arrow.r.bar ");
            typst_emit_expr(ast->map.vals[i], out, TYPST_PREC_TOP, opts);
        }
        EMIT(out, "}");
        break;

    // ── Range literal ─────────────────────────────────────────────────────────

    case AST_RANGE: {
        char open  = ast->range.is_array ? '[' : '(';
        char close = ast->range.is_array ? ']' : ')';
        EMITC(out, open);
        typst_emit_expr(ast->range.start, out, TYPST_PREC_TOP, opts);
        if (ast->range.step) {
            EMIT(out, ", ");
            typst_emit_expr(ast->range.step, out, TYPST_PREC_TOP, opts);
        }
        EMIT(out, " dots.h ");
        if (ast->range.end)
            typst_emit_expr(ast->range.end, out, TYPST_PREC_TOP, opts);
        EMITC(out, close);
        break;
    }

    // ── Refinement type  { x ∈ T | P(x) } ───────────────────────────────────

    case AST_REFINEMENT:
        typst_emit_refinement_type(ast, out, opts);
        break;

    // ── Pattern match ─────────────────────────────────────────────────────────

    case AST_PMATCH: {
        EMIT(out, "cases(");
        for (int ci = 0; ci < ast->pmatch.clause_count; ci++) {
            ASTPMatchClause *cl = &ast->pmatch.clauses[ci];
            if (ci > 0) EMIT(out, ", ");
            if (cl->guard_count > 0) {
                for (int gi = 0; gi < cl->guard_count; gi++) {
                    if (gi > 0) EMIT(out, ", ");
                    typst_emit_expr(cl->guard_bodies[gi], out, TYPST_PREC_TOP, opts);
                    EMIT(out, " \"if\" ");
                    for (int pi = 0; pi < cl->pattern_count; pi++) {
                        if (pi > 0) EMIT(out, " and ");
                        typst_emit_pattern(&cl->patterns[pi], out, opts);
                    }
                    EMIT(out, " and ");
                    typst_emit_expr(cl->guard_conds[gi], out, TYPST_PREC_TOP, opts);
                }
            } else {
                typst_emit_expr(cl->body, out, TYPST_PREC_TOP, opts);
                EMIT(out, " \"if\" ");
                for (int pi = 0; pi < cl->pattern_count; pi++) {
                    if (pi > 0) EMIT(out, " and ");
                    typst_emit_pattern(&cl->patterns[pi], out, opts);
                }
            }
        }
        EMITC(out, ')');
        break;
    }

    // ── Structured nodes ─────────────────────────────────────────────────────

    case AST_DATA:     typst_emit_data_rules(ast, out, opts);    break;
    case AST_CLASS:    typst_emit_class_decl(ast, out, opts);    break;
    case AST_INSTANCE: typst_emit_instance_decl(ast, out, opts); break;

    // ── Inline assembly — rendered as an opaque monospace atom ───────────────

    case AST_ASM:
        EMIT(out, "mono(\"asm { ... }\")");
        break;

    // ── Tests — suppressed in math output ────────────────────────────────────

    case AST_TESTS:
        EMIT(out, "italic(\"(tests omitted)\")");
        break;

    default:
        EMIT(out, "dots.h");
        break;
    }
}


/// Refinement type
//
//  Renders  (type Name { x ∈ Base | predicate })  as the Typst math:
//
//    upright("Name") triangle.r.small { x in bb(ZZ) mid P(x) }
//
void typst_emit_refinement_type(AST *ref, SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (!ref || ref->type != AST_REFINEMENT) { EMIT(out, "bot"); return; }

    if (ref->refinement.name) {
        emit_type_name(ref->refinement.name, out);
        EMIT(out, " triangle.r.small ");
    }
    EMIT(out, "{ ");
    emit_ident(ref->refinement.var ? ref->refinement.var : "x", out, opts);
    EMIT(out, " in ");
    emit_type_name(ref->refinement.base_type ? ref->refinement.base_type : "?", out);
    EMIT(out, " mid ");
    if (ref->refinement.predicate)
        typst_emit_expr(ref->refinement.predicate, out, TYPST_PREC_TOP, opts);
    else
        EMIT(out, "top");
    EMIT(out, " }");
}


/// Typing judgement
//
//  Renders  Γ ⊢ e : τ  in Typst math as:
//
//    x_1 : T_1 , dots , x_n : T_n tack.r e : tau
//
//  An empty context is rendered as  emptyset tack.r.
//
void typst_emit_judgement(const char **ctx_names, const char **ctx_types,
                          int ctx_len,
                          AST *expr, AST *type_ast,
                          SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (opts->sequent_style) {
        if (ctx_len == 0) {
            EMIT(out, "emptyset");
        } else {
            for (int i = 0; i < ctx_len; i++) {
                if (i > 0) EMIT(out, ", ");
                emit_ident(ctx_names[i], out, opts);
                EMIT(out, " : ");
                emit_type_name(ctx_types[i], out);
            }
        }
        EMIT(out, " tack.r ");
    }
    typst_emit_expr(expr, out, TYPST_PREC_CMP + 1, opts);
    EMIT(out, " : ");
    if (type_ast) typst_emit_type(type_ast, out, opts);
    else          EMIT(out, "?");
}


/// Equality judgement
//
//  Renders  Γ ⊢ lhs ≡ rhs : τ  in Typst math.
//
void typst_emit_equality(const char **ctx_names, const char **ctx_types,
                         int ctx_len,
                         AST *lhs, AST *rhs, AST *type_ast,
                         SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (opts->sequent_style) {
        if (ctx_len == 0) {
            EMIT(out, "emptyset");
        } else {
            for (int i = 0; i < ctx_len; i++) {
                if (i > 0) EMIT(out, ", ");
                emit_ident(ctx_names[i], out, opts);
                EMIT(out, " : ");
                emit_type_name(ctx_types[i], out);
            }
        }
        EMIT(out, " tack.r ");
    }
    typst_emit_expr(lhs, out, TYPST_PREC_CMP + 1, opts);
    EMIT(out, " equiv ");
    typst_emit_expr(rhs, out, TYPST_PREC_CMP + 1, opts);
    EMIT(out, " : ");
    if (type_ast) typst_emit_type(type_ast, out, opts);
    else          EMIT(out, "?");
}


/// Data declaration → proof-tree introduction rules
//
//  Uses the curryst package's #proof-tree / #rule macros to render each
//  constructor as a Gentzen-style introduction rule:
//
//    #proof-tree(
//      rule(
//        name: [Circle-I],
//        $ e_1 : bb(RR) $,
//        $ upright("Circle") space e_1 : upright("Shape") $
//      )
//    )
//
void typst_emit_data_rules(AST *data, SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (!data || data->type != AST_DATA) return;

    // Header line in content mode
    EMIT(out, "$ bold(\"data\") space ");
    emit_type_name(data->data.name, out);
    for (int i = 0; i < data->data.type_param_count; i++) {
        EMIT(out, " space ");
        emit_ident(data->data.type_params[i], out, opts);
    }
    EMIT(out, " space bold(\"where\") $\n\n");

    EMIT(out, "#grid(columns: auto, gutter: 1.5em,\n");
    for (int ci = 0; ci < data->data.constructor_count; ci++) {
        ASTDataConstructor *c = &data->data.constructors[ci];
        EMIT(out, "  #proof-tree(\n    rule(\n");
        // Rule name
        EMIT(out, "      name: [");
        emit_typst_string_content(c->name, out);
        EMIT(out, "-I],\n");
        // Premises
        for (int fi = 0; fi < c->field_count; fi++) {
            EMIT(out, "      $e_");
            EMITF(out, "%d", fi + 1);
            EMIT(out, " : ");
            emit_type_name(c->field_types[fi], out);
            EMIT(out, "$,\n");
        }
        // Conclusion
        EMIT(out, "      $");
        emit_type_name(c->name, out);
        for (int fi = 0; fi < c->field_count; fi++)
            EMITF(out, " space e_%d", fi + 1);
        EMIT(out, " : ");
        emit_type_name(data->data.name, out);
        for (int i = 0; i < data->data.type_param_count; i++) {
            EMIT(out, " space ");
            emit_ident(data->data.type_params[i], out, opts);
        }
        EMIT(out, "$\n    )\n  )");
        if (ci < data->data.constructor_count - 1) EMITC(out, ',');
        EMITC(out, '\n');
    }
    EMIT(out, ")\n");
}


/// Raw type string translation
//
//  Method type signatures are stored by the parser as raw strings
//  e.g. "a -> a -> Bool" or "a -> a -> Bool".  We tokenise them
//  on whitespace and translate each token through the symbol map
//  so arrows, operators and type names render correctly in Typst math.
//
static void emit_raw_type_string(const char *raw, SB *out,
                                 const TypstEmitOpts *opts) {
    if (!raw) { EMIT(out, "?"); return; }
    char buf[512];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = buf;
    bool first = true;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *tok_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';
        if (!first) EMIT(out, " ");
        first = false;
        const char *mapped = typst_symbol_map(tok_start);
        if (mapped)
            EMIT(out, mapped);
        else
            emit_type_name(tok_start, out);
        *p = saved;
    }
}

/// Class declaration
//
//  Renders as a content block using a definition environment from ctheorems:
//
//    #definition[
//      *class* $upright("Eq") space a$ *where*
//      - $(=) : a arrow.r a arrow.r bb(BB)$
//      - $(eq.not) : a arrow.r a arrow.r bb(BB)$
//    ]
//
void typst_emit_class_decl(AST *cls, SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (!cls || cls->type != AST_CLASS) return;

    if (opts->emit_labels)
        EMITF(out, "#label(\"class-%s\")\n", cls->class_decl.name);

    EMIT(out, "#definition[\n  *class* $");
    emit_type_name(cls->class_decl.name, out);
    if (cls->class_decl.type_var) {
        EMIT(out, " space ");
        emit_ident(cls->class_decl.type_var, out, opts);
    }
    EMIT(out, "$ *where*\n");

    for (int i = 0; i < cls->class_decl.assoc_count; i++) {
        EMIT(out, "  - *type* $");
        emit_type_name(cls->class_decl.assoc_types[i], out);
        EMIT(out, "$\n");
    }

    for (int i = 0; i < cls->class_decl.method_count; i++) {
        const char *mapped = typst_symbol_map(cls->class_decl.method_names[i]);
        EMIT(out, "  - $(");
        if (mapped) EMIT(out, mapped);
        else emit_ident(cls->class_decl.method_names[i], out, opts);
        EMIT(out, ") : ");
        emit_raw_type_string(cls->class_decl.method_types[i], out, opts);
        EMIT(out, "$\n");
    }
    EMIT(out, "]\n");
}


/// Instance declaration
//
//  Renders as a plain content block listing each method implementation.
//
void typst_emit_instance_decl(AST *inst, SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (!inst || inst->type != AST_INSTANCE) return;

    EMIT(out, "#block[\n  *instance* $");
    emit_type_name(inst->instance_decl.class_name, out);
    EMIT(out, " space ");
    emit_type_name(inst->instance_decl.type_name, out);
    EMIT(out, "$ *where*\n");

    for (int i = 0; i < inst->instance_decl.method_count; i++) {
        const char *mapped = typst_symbol_map(inst->instance_decl.method_names[i]);
        EMIT(out, "  - $(");
        if (mapped) EMIT(out, mapped);
        else emit_ident(inst->instance_decl.method_names[i], out, opts);
        EMIT(out, ") triangle.r.small ");
        if (inst->instance_decl.method_bodies[i])
            typst_emit_expr(inst->instance_decl.method_bodies[i],
                            out, TYPST_PREC_TOP, opts);
        EMIT(out, "$\n");
    }
    EMIT(out, "]\n");
}


/// Top-level form emission
//
//  Detects definition forms by their head symbol and emits them with the
//  appropriate mathematical notation.  Everything else falls through to
//  typst_emit_expr wrapped in a $ … $ display math block.
//
void typst_emit_toplevel(AST *ast, SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    if (!ast) return;

    // Structured node types bypass the list dispatch entirely
    switch (ast->type) {
    case AST_DATA:
        typst_emit_data_rules(ast, out, opts);
        return;
    case AST_CLASS:
        typst_emit_class_decl(ast, out, opts);
        return;
    case AST_INSTANCE:
        typst_emit_instance_decl(ast, out, opts);
        return;
    case AST_REFINEMENT:
        EMIT(out, "$ ");
        typst_emit_refinement_type(ast, out, opts);
        EMIT(out, " $\n");
        return;
    case AST_TESTS:
    case AST_ASM:
        return;  // suppressed at document level
    default:
        break;
    }

    if (ast->type != AST_LIST || ast->list.count == 0) {
        EMIT(out, "$ ");
        typst_emit_expr(ast, out, TYPST_PREC_TOP, opts);
        EMIT(out, " $\n");
        return;
    }

    AST *head = ast->list.items[0];
    if (head->type != AST_SYMBOL) {
        EMIT(out, "$ ");
        typst_emit_expr(ast, out, TYPST_PREC_TOP, opts);
        EMIT(out, " $\n");
        return;
    }

    // (def name value) ───────────────────────────────────────────────────────
    if (strcmp(head->symbol, "def") == 0 && ast->list.count >= 3) {
        AST *name = ast->list.items[1];
        AST *val  = ast->list.items[2];
        if (opts->emit_labels && name->type == AST_SYMBOL)
            EMITF(out, "#label(\"def-%s\")\n", name->symbol);
        EMIT(out, "$ ");
        emit_ident(name->symbol, out, opts);
        EMIT(out, " triangle.r.small ");
        typst_emit_expr(val, out, TYPST_PREC_TOP, opts);
        EMIT(out, " $\n");
        return;
    }

    // (defn name params… body) ───────────────────────────────────────────────
    if ((strcmp(head->symbol, "defn") == 0 ||
         strcmp(head->symbol, "fn")   == 0)
             && ast->list.count >= 3) {
        AST *name = ast->list.items[1];
        if (opts->emit_labels && name->type == AST_SYMBOL)
            EMITF(out, "#label(\"def-%s\")\n", name->symbol);
        EMIT(out, "$ ");
        emit_ident(name->symbol, out, opts);
        for (size_t i = 2; i < ast->list.count - 1; i++) {
            EMIT(out, " space ");
            typst_emit_expr(ast->list.items[i], out, TYPST_PREC_ATOM, opts);
        }
        EMIT(out, " triangle.r.small ");
        typst_emit_expr(ast->list.items[ast->list.count - 1],
                        out, TYPST_PREC_TOP, opts);
        EMIT(out, " $\n");
        return;
    }

    // (type Name Target) ─────────────────────────────────────────────────────
    if (strcmp(head->symbol, "type") == 0 && ast->list.count >= 3) {
        EMIT(out, "$ bold(\"type\") space ");
        typst_emit_type(ast->list.items[1], out, opts);
        EMIT(out, " = ");
        typst_emit_type(ast->list.items[2], out, opts);
        EMIT(out, " $\n");
        return;
    }

    // Generic expression
    EMIT(out, "$ ");
    typst_emit_expr(ast, out, TYPST_PREC_TOP, opts);
    EMIT(out, " $\n");
}


/// Typst preamble
//
//  Emits a minimal but complete Typst document header that imports the
//  packages needed by this backend and configures the page and math fonts.
//
void typst_emit_preamble(SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);
    const char *paper = opts->paper ? opts->paper : "a4";

    EMIT(out, "#import \"@preview/curryst:0.3.0\": rule, proof-tree\n");
    EMIT(out, "#import \"@preview/ctheorems:1.1.3\": *\n");
    EMIT(out, "\n");
    EMIT(out, "#show: thmrules.with(qed-symbol: $square$)\n");
    EMIT(out, "\n");
    EMIT(out, "#let definition = thmbox(\"definition\", \"Definition\", fill: rgb(\"#eef\"))\n");
    EMIT(out, "#let lemma      = thmbox(\"lemma\",      \"Lemma\",      fill: rgb(\"#efe\"))\n");
    EMIT(out, "#let theorem    = thmbox(\"theorem\",    \"Theorem\",    fill: rgb(\"#fee\"))\n");
    EMIT(out, "\n");
    EMIT(out, "// Shorthand macros used by the emitted math\n");
    EMIT(out, "#let judge(ctx, e, t)      = $ctx tack.r e : t$\n");
    EMIT(out, "#let eqjudge(ctx, l, r, t) = $ctx tack.r l equiv r : t$\n");
    EMIT(out, "#let defeq                 = $triangle.r.small$\n");
    EMIT(out, "\n");
    EMITF(out, "#set page(paper: \"%s\", margin: 2.5cm)\n", paper);
    EMIT(out, "#set text(font: \"New Computer Modern\", size: 11pt)\n");
    if (opts->number_equations)
        EMIT(out, "#set math.equation(numbering: \"(1)\")\n");
    else
        EMIT(out, "#set math.equation(numbering: none)\n");
    EMIT(out, "\n");

    if (opts->mode == TYPST_DOC_FULL) {
        const char *title  = opts->title  ? opts->title  : "Program Listing";
        const char *author = opts->author ? opts->author : "";
        EMIT(out, "#align(center)[\n");
        EMITF(out, "  #text(size: 18pt, weight: \"bold\")[%s]\\\n", title);
        EMITF(out, "  #text(size: 12pt)[%s]\\\n", author);
        EMIT(out, "  #text(size: 10pt)[#datetime.today().display()]\n");
        EMIT(out, "]\n\n");
    }
}


/// Full document emission
//
//  Iterates over all top-level forms, skipping tests and asm blocks,
//  and emits them separated by vertical space.
//
void typst_emit_document(AST **exprs, size_t count,
                         SB *out, const TypstEmitOpts *opts) {
    opts = opts_or_default(opts);

    if (opts->mode == TYPST_DOC_FULL)
        typst_emit_preamble(out, opts);

    EMIT(out, "#align(left)[\n$ ");
    int current_line = -1;
    int current_col = -1;

    for (size_t i = 0; i < count; i++) {
        AST *e = exprs[i];
        if (!e) continue;
        if (e->type == AST_TESTS || e->type == AST_ASM) continue;

        if (e->type == AST_DATA || e->type == AST_CLASS ||
            e->type == AST_INSTANCE || e->type == AST_REFINEMENT) {
            EMIT(out, " $\n]\n");
            typst_emit_toplevel(e, out, opts);
            EMIT(out, "#align(left)[\n$ ");
            current_line = -1;
            current_col = -1;
            continue;
        }

        if (current_line == -1) {
            current_line = e->line;
            current_col = e->column;
        } else if (e->line > current_line) {
            for (int l = current_line; l < e->line; l++) {
                EMIT(out, " \\ \n");
            }
            current_col = 1;
            current_line = e->line;
        }

        if (e->column > current_col) {
            int spaces = e->column - current_col;
            for (int s = 0; s < spaces; s++) {
                EMIT(out, " space ");
            }
            current_col = e->column;
        }

        typst_emit_expr(e, out, TYPST_PREC_TOP, opts);
        current_col = e->end_column;
    }
    EMIT(out, " $\n]\n");
}


/// PDF emission
//
//  Writes the Typst source to `typ_path` and invokes `typst compile`.
//  The Typst binary must be on PATH.  The caller is responsible for
//  freeing *pdf_path.
//
int typst_emit_pdf(AST **exprs, size_t count,
                   const char *typ_path,
                   char **pdf_path,
                   const TypstEmitOpts *opts) {
    SB src; sb_init(&src);
    typst_emit_document(exprs, count, &src, opts);

    FILE *fp = fopen(typ_path, "w");
    if (!fp) { sb_free(&src); return -1; }
    fwrite(src.data, 1, src.len, fp);
    fclose(fp);
    sb_free(&src);

    // Build the pdf path by replacing .typ with .pdf
    size_t len = strlen(typ_path);
    char *out_path = malloc(len + 1);
    memcpy(out_path, typ_path, len + 1);
    char *dot = strrchr(out_path, '.');
    if (dot && strcmp(dot, ".typ") == 0)
        memcpy(dot, ".pdf", 5);
    if (pdf_path) *pdf_path = out_path;
    else          free(out_path);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "typst compile %s > /dev/null 2>&1", typ_path);
    return system(cmd);
}
