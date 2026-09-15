#ifndef MONAD_TOOLING_CST_H
#define MONAD_TOOLING_CST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOOLING_CST_WHITESPACE,
    TOOLING_CST_NEWLINE,
    TOOLING_CST_ATOM,
    TOOLING_CST_PUNCTUATION,
    TOOLING_CST_STRING,
    TOOLING_CST_CHARACTER,
    TOOLING_CST_LINE_COMMENT,
    TOOLING_CST_BLOCK_COMMENT,
    TOOLING_CST_ERROR,
} ToolingCstTokenKind;

typedef struct {
    ToolingCstTokenKind kind;
    size_t offset, length;
    size_t line, column;
} ToolingCstToken;

typedef enum {
    TOOLING_CST_ROOT,
    TOOLING_CST_PAREN_GROUP,
    TOOLING_CST_BRACKET_GROUP,
    TOOLING_CST_BRACE_GROUP,
    TOOLING_CST_ERROR_NODE,
} ToolingCstNodeKind;

typedef struct {
    ToolingCstNodeKind kind;
    size_t parent;
    size_t first_token, end_token;
    bool closed;
} ToolingCstNode;

typedef struct ToolingCstDocument ToolingCstDocument;

ToolingCstDocument *tooling_cst_parse(const char *path, const char *source);
void tooling_cst_dispose(ToolingCstDocument *document);

bool tooling_cst_validate(const ToolingCstDocument *document);
size_t tooling_cst_token_count(const ToolingCstDocument *document);
const ToolingCstToken *tooling_cst_token(const ToolingCstDocument *document,
                                         size_t index);
size_t tooling_cst_node_count(const ToolingCstDocument *document);
const ToolingCstNode *tooling_cst_node(const ToolingCstDocument *document,
                                      size_t index);
const ToolingCstNode *tooling_cst_root(const ToolingCstDocument *document);
bool tooling_cst_has_errors(const ToolingCstDocument *document);
const char *tooling_cst_source(const ToolingCstDocument *document);
uint64_t tooling_cst_fingerprint(const ToolingCstDocument *document);
char *tooling_cst_render(const ToolingCstDocument *document, size_t *length);
char *tooling_cst_code_projection(const ToolingCstDocument *document);

#endif
