#ifndef READER_SYNTAX_H
#define READER_SYNTAX_H

/* Language-declared, scoped expression and indentation-block readers. */
char *reader_syntax_expand(const char *source, const char *filename);
/* Expand a declared one-hole type notation, returning a newly allocated
 * canonical type spelling, or NULL when no active notation matches. */
char *reader_type_syntax_expand(const char *type_text);
int   reader_type_syntax_is_active(const char *open, const char *close);
/* A collection notation can independently declare its value constructors and
 * bounded-comprehension eliminator.  This keeps brace syntax generic: the
 * reader only elaborates to the functions named by the active module. */
int   reader_term_syntax_lookup(const char *open, const char *close,
                                const char **empty_target,
                                const char **elements_target,
                                const char **filter_target);
void  reader_syntax_scope_push(const char *owner_file);
void  reader_syntax_scope_allow(const char *owner_file);
void  reader_syntax_scope_pop(void);
void  reader_syntax_clear(void);
/* Batch workers retain Core reader declarations while dropping registrations
 * owned by ordinary test/user modules. */
void  reader_syntax_clear_noncore(const char *core_dir);

#endif
