#ifndef READER_SYNTAX_H
#define READER_SYNTAX_H

/* Language-declared, scoped expression and indentation-block readers. */
char *reader_syntax_expand(const char *source, const char *filename);
void  reader_syntax_scope_push(const char *owner_file);
void  reader_syntax_scope_allow(const char *owner_file);
void  reader_syntax_scope_pop(void);
void  reader_syntax_clear(void);

#endif
