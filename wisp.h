#ifndef WISP_H
#define WISP_H
/*
 * wisp — arity-driven syntax expansion
 *
 * PHILOSOPHY
 * Arity is structure. Indentation only marks where variadic forms end.
 * Fixed-arity forms consume exactly N arguments regardless of whitespace:
 *
 *   DrawFPS       <- arity 2, grabs next 2 tokens unconditionally
 *   600
 *   600
 *
 *   until WindowShouldClose   <- variadic, owns all indented lines below
 *     BeginDrawing
 *     ClearBackground BLACK
 *     DrawFPS                 <- fixed arity 2, reaches past until's indent
 *   600
 *   600
 *     EndDrawing
 *
 * Infix calls still work:  if x `<` 10  ->  (if (< x 10) ...)
 * Normal s-expressions pass through untouched.
 *
 * ARITY SOURCES (in order)
 *   1. Language builtins from Env  <- wisp_register_arities_from_env()
 *   2. C FFI functions and structs <- wisp_register_arity() pre-pass
 *   3. User defines                <- pre-scanned from source
 */

#include "reader.h"
#include "env.h"

ASTList wisp_parse_all(const char *source, const char *filename);
void    wisp_register_arity(const char *name, int arity);
void    wisp_register_arities_from_env(Env *env);
void    wisp_clear_arities(void);

#endif
