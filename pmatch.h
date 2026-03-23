#ifndef PMATCH_H
#define PMATCH_H

#include "reader.h"

/// Pattern Match Parser
//
// Parses `pattern -> expr` clauses from the token stream inside a
// function body that does NOT start with `(`.
//
// Called from the define/lambda parsing path in reader.c after the
// optional docstring has been consumed, when the next token is not
// TOK_LPAREN.

/// Pattern Parser

// Parse a single ASTPattern from the current token stream.
// Handles: _ (wildcard), variables, numeric literals, list patterns [...]
ASTPattern parse_pattern(Parser *p);

// Parse all `pattern -> expr` clauses until TOK_RPAREN or TOK_EOF.
// param_count tells us how many patterns to expect per clause.
// Returns an AST_PMATCH node owning all parsed clauses.
AST *parse_pmatch_clauses(Parser *p, int param_count);


ASTPattern parse_single_pattern(Parser *p);

/// Desugarer

// Transform an AST_PMATCH node into an equivalent (cond ...) AST
// given the function's parameter list.
// The returned AST is freshly allocated and owned by the caller.
// Called from codegen before the lambda body is compiled.
AST *pmatch_desugar(AST *pmatch_node, ASTParam *params, int param_count);

/// Utilities

// Free a single ASTPattern and all its children.
// Thin wrapper around ast_pattern_free for stack-allocated patterns.
void pattern_free(ASTPattern *p);

#endif
