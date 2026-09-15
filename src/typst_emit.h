#ifndef TYPST_EMIT_H
#define TYPST_EMIT_H

#include "reader.h"

/// Typst Emission Backend
//
//  Converts a parsed (and optionally type-inferred) AST into Typst source.
//  The output is mathematically typeset using Typst's built-in math mode
//  together with the following packages (auto-imported in the preamble):
//
//    curryst   — proof trees and inference rules  (#proof-tree, #rule)
//    ctheorems — theorem / definition / lemma environments
//
//  Typst math notation used throughout:
//
//    lambda x. e       lambda abstraction
//    tack.r            ⊢  (turnstile / typing judgement)
//    equiv             ≡  (definitional equality)
//    arrow.r           →  (function type / implication)
//    forall / exists   quantifiers
//    in / subset.eq    set membership / inclusion
//    triangle.r.small  ≜  (definition equality)
//
/// Emit options
//
//  Zero-initialise for sensible defaults.
//
typedef struct TypstEmitOpts {

    // Document mode
    //   TYPST_DOC_FULL     — emit a complete Typst document with preamble
    //   TYPST_DOC_FRAGMENT — emit only math / content blocks, no preamble
    int  mode;

    // Type display
    //   true  — use inferred_type if present, fall back to annotation
    //   false — use only syntactic annotations, never inferred types
    bool prefer_inferred_types;

    // Render typing judgements in sequent style  Γ ⊢ e : τ
    // When false, types appear inline as  e : τ
    bool sequent_style;

    // Emit Typst labels  <def-name>  for every named definition,
    // enabling cross-references with @def-name
    bool emit_labels;

    // Place each top-level form in its own numbered equation block
    // instead of a running math block
    bool number_equations;

    // Override document title / author (TYPST_DOC_FULL only)
    const char *title;
    const char *author;

    // Paper size string passed to Typst set page(...)
    // e.g. "a4", "us-letter".  NULL defaults to "a4".
    const char *paper;

} TypstEmitOpts;

#define TYPST_DOC_FULL     0
#define TYPST_DOC_FRAGMENT 1


/// Precedence levels
//
//  Higher value = tighter binding.  Pass TYPST_PREC_TOP when calling
//  from outside; the emitter inserts lr("(" … ")") as needed.
//
#define TYPST_PREC_TOP     0
#define TYPST_PREC_COMMA   1
#define TYPST_PREC_ARROW   2    // a -> b
#define TYPST_PREC_OR      3    // ∨
#define TYPST_PREC_AND     4    // ∧
#define TYPST_PREC_CMP     5    // = ≠ < ≤ > ≥ ≡
#define TYPST_PREC_ADD     6    // + −
#define TYPST_PREC_MUL     7    // * / mod
#define TYPST_PREC_UNARY   8    // ¬ − (unary)
#define TYPST_PREC_APP     9    // function application
#define TYPST_PREC_ATOM   10    // literals, parenthesised sub-expressions


/// Core emission functions

// Emit a single expression into `out`.
// `prec` is the precedence of the surrounding context (use TYPST_PREC_TOP).
void typst_emit_expr(AST *ast, SB *out, int prec, const TypstEmitOpts *opts);

// Emit a type expression (AST node known to represent a type).
void typst_emit_type(AST *ast, SB *out, const TypstEmitOpts *opts);

// Emit a pattern (inside pmatch clauses).
void typst_emit_pattern(ASTPattern *pat, SB *out, const TypstEmitOpts *opts);

// Emit a complete top-level definition (def, defn, data, class, instance…).
void typst_emit_toplevel(AST *ast, SB *out, const TypstEmitOpts *opts);

// Emit a typing judgement:  Γ ⊢ expr : τ
// `ctx_names` / `ctx_types` are parallel arrays of length `ctx_len`.
void typst_emit_judgement(const char **ctx_names, const char **ctx_types,
                          int ctx_len,
                          AST *expr, AST *type_ast,
                          SB *out, const TypstEmitOpts *opts);

// Emit a βη-equality judgement:  Γ ⊢ lhs ≡ rhs : τ
void typst_emit_equality(const char **ctx_names, const char **ctx_types,
                         int ctx_len,
                         AST *lhs, AST *rhs, AST *type_ast,
                         SB *out, const TypstEmitOpts *opts);

// Emit a refinement type in set-builder notation:  { x ∈ T | P(x) }
void typst_emit_refinement_type(AST *ref, SB *out, const TypstEmitOpts *opts);

// Emit a data declaration as Gentzen-style introduction rules.
void typst_emit_data_rules(AST *data, SB *out, const TypstEmitOpts *opts);

// Emit a class declaration as a mathematical interface block.
void typst_emit_class_decl(AST *cls, SB *out, const TypstEmitOpts *opts);

// Emit an instance declaration.
void typst_emit_instance_decl(AST *inst, SB *out, const TypstEmitOpts *opts);


/// Document-level functions

// Emit the standard Typst preamble used by TYPST_DOC_FULL mode.
void typst_emit_preamble(SB *out, const TypstEmitOpts *opts);

// Emit a complete Typst document wrapping `count` top-level forms.
void typst_emit_document(AST **exprs, size_t count,
                         SB *out, const TypstEmitOpts *opts);

// Write the Typst source to `typ_path`, then invoke `typst compile`.
// Returns 0 on success, non-zero on failure.
// *pdf_path receives the path of the produced PDF (caller must free).
int typst_emit_pdf(AST **exprs, size_t count,
                   const char *typ_path,
                   char **pdf_path,
                   const TypstEmitOpts *opts);


/// Symbol → Typst math mapping
//
//  Maps operator and keyword symbols to their canonical Typst math
//  representations.  Returns NULL if `sym` has no special mapping.
//
const char *typst_symbol_map(const char *sym);

// Returns true if `sym` is a known infix binary operator.
bool typst_is_infix(const char *sym);

// Returns the precedence of a known operator symbol, or TYPST_PREC_APP.
int typst_op_precedence(const char *sym);

#endif // TYPST_EMIT_H
