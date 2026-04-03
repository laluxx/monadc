#ifndef MACRO_H
#define MACRO_H

/*
 * macro.h — Compile-time macro expansion pass
 *
 * Macros are ordinary defines whose lambda return type is Syntax:
 *
 *   (define (when condition . body -> Syntax)
 *     (if condition (begin . body) (begin)))
 *
 *   (define (my-and . args -> Syntax)
 *     []         -> True
 *     [x]        -> x
 *     [x . rest] -> (if x (my-and . rest) False))
 *
 *   (define (swap! a b -> Syntax)
 *     (let [tmp a]
 *       (set! a b)
 *       (set! b tmp)))
 *
 *   (define (assert condition message -> Syntax)
 *     (if debug-mode
 *       #(when (not condition) (error message))
 *       #(begin)))
 *
 * Multi-clause macros (like my-and) use the existing pmatch desugar
 * path — the parser already turns them into an if-chain body, which
 * this expander walks and substitutes into like any other body.
 *
 * Expansion runs after parse_all / wisp_parse_all, before type-checking
 * and codegen.  It repeats to a fixpoint so macros can expand into other
 * macro calls.
 *
 * Hygiene
 * -------
 * Names *introduced* inside a macro body (lambda params, let bindings,
 * inner defines) that are NOT pattern variables get renamed to:
 *
 *   macroname__varname__N
 *
 * where N is a global monotonic counter.  Pattern variables and free
 * references to outer definitions pass through unchanged.
 *
 * #(...) quasi-output
 * -------------------
 * If the body contains no #(…) the entire body is the template.
 * If #(…) appears, only the subtree inside #(…) is the macro output;
 * the surrounding code runs at expand time (full compile-time eval is
 * TODO — currently the whole body is substituted and the #() marker
 * is stripped).
 *
 * Variadic macros
 * ---------------
 * A rest parameter (is_rest) captures zero or more trailing arguments.
 * In the template, a reference to the rest name that appears as a list
 * element is *spliced* (all captured nodes inserted flat).  In non-list
 * position a single-element rest wraps the captured nodes in (begin …).
 */

#include "reader.h"

/*
 * macro_expand_all(exprs, count)
 *
 * Run the full macro expansion pipeline on a flat list of top-level ASTs
 * produced by parse_all / wisp_parse_all:
 *
 *   1. Scan for defines whose lambda has return_type "Syntax" → register
 *      them and remove them from the output (macros are compile-time only).
 *   2. Walk every remaining AST looking for macro call sites → expand.
 *   3. Repeat step 2 to fixpoint (capped at 1 024 rounds).
 *
 * Returns a new ASTList.  The caller owns the returned exprs array
 * and should free() it after use; individual AST nodes are live.
 * Macro define nodes consumed in step 1 are freed internally.
 */
ASTList macro_expand_all(AST **exprs, size_t count);

/*
 * macro_is_registered(name)
 *
 * Returns 1 if 'name' is a registered macro, 0 otherwise.
 * Useful for the type-checker / codegen to skip macro defines.
 */
int macro_is_registered(const char *name);

/*
 * macro_clear()
 *
 * Release all registered macros.  Call between compilation units if
 * macros should not persist across files.
 */
void macro_clear(void);

#endif /* MACRO_H */
