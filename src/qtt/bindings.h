#ifndef MONAD_QTT_BINDINGS_H
#define MONAD_QTT_BINDINGS_H

/* Lexical identity and scope resolution for the QTT subsystem. */

#include "../reader.h"
#include "../types.h"

#define QTT_BINDER_UNRESOLVED UINT64_C(0)

typedef enum {
    QTT_BINDINGS_OK = 0,
    QTT_BINDINGS_OUT_OF_MEMORY,
    QTT_BINDINGS_DUPLICATE_BINDER,
} QttBindingError;

typedef struct {
    QttBindingError error;
    uint64_t binder_count;
    uint64_t closure_count;
    const char *duplicate_name;
} QttBindingSummary;

/*
 * Assign stable lexical identities in deterministic preorder and resolve each
 * local symbol occurrence to its declaration. Re-running resolution replaces
 * prior IDs, so cloned/transformed trees can establish a fresh identity space.
 *
 * Identity-bearing contexts are the implementation counterpart of QTT
 * contexts: quantities attach to declarations, never to textual names.
 * See Atkey, "The Syntax and Semantics of Quantitative Type Theory":
 * https://bentnib.org/quantitative-type-theory.pdf
 */
QttBindingSummary qtt_bindings_resolve(AST *root);
QttBindingSummary qtt_bindings_resolve_many(AST **roots, size_t count);
const Type *qtt_bindings_layout_type(const char *name);

#endif
