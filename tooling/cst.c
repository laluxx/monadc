#include "cst.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct ToolingCstDocument {
    char *path;
    char *source;
    size_t source_length;
    ToolingCstToken *tokens;
    size_t token_count, token_capacity;
    ToolingCstNode *nodes;
    size_t node_count, node_capacity;
    bool has_errors;
    uint64_t fingerprint;
};

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static bool punctuation(unsigned char byte) {
    return strchr("()[]{}#,:|", byte) != NULL ||
           byte == '.' || byte == '=' || byte == '<' || byte == '>' ||
           byte == '+' || byte == '*' || byte == '/' || byte == '%';
}

static bool push_token(ToolingCstDocument *document, ToolingCstTokenKind kind,
                       size_t offset, size_t length,
                       size_t line, size_t column) {
    if (!length) return true;
    if (document->token_count == document->token_capacity) {
        size_t capacity = document->token_capacity
            ? document->token_capacity * 2 : 64;
        void *tokens = realloc(document->tokens,
                               capacity * sizeof(*document->tokens));
        if (!tokens) return false;
        document->tokens = tokens;
        document->token_capacity = capacity;
    }
    document->tokens[document->token_count++] = (ToolingCstToken) {
        .kind = kind,
        .offset = offset,
        .length = length,
        .line = line,
        .column = column,
    };
    return true;
}

static bool push_node(ToolingCstDocument *document, ToolingCstNode node,
                      size_t *index) {
    if (document->node_count == document->node_capacity) {
        size_t capacity = document->node_capacity
            ? document->node_capacity * 2 : 16;
        void *nodes = realloc(document->nodes,
                              capacity * sizeof(*document->nodes));
        if (!nodes) return false;
        document->nodes = nodes;
        document->node_capacity = capacity;
    }
    if (index) *index = document->node_count;
    document->nodes[document->node_count++] = node;
    return true;
}

static bool token_equals(const ToolingCstDocument *document, size_t index,
                         const char *text) {
    const ToolingCstToken *token = &document->tokens[index];
    size_t length = strlen(text);
    return token->length == length &&
           memcmp(document->source + token->offset, text, length) == 0;
}

static ToolingCstNodeKind opener_kind(const ToolingCstDocument *document,
                                      size_t token_index) {
    if (token_equals(document, token_index, "(")) return TOOLING_CST_PAREN_GROUP;
    if (token_equals(document, token_index, "[")) return TOOLING_CST_BRACKET_GROUP;
    if (token_equals(document, token_index, "{")) return TOOLING_CST_BRACE_GROUP;
    return TOOLING_CST_ROOT;
}

static bool closes_kind(const ToolingCstDocument *document, size_t token_index,
                        ToolingCstNodeKind kind) {
    return (kind == TOOLING_CST_PAREN_GROUP &&
            token_equals(document, token_index, ")")) ||
           (kind == TOOLING_CST_BRACKET_GROUP &&
            token_equals(document, token_index, "]")) ||
           (kind == TOOLING_CST_BRACE_GROUP &&
            token_equals(document, token_index, "}"));
}

static bool closing_token(const ToolingCstDocument *document,
                          size_t token_index) {
    return token_equals(document, token_index, ")") ||
           token_equals(document, token_index, "]") ||
           token_equals(document, token_index, "}");
}

static bool build_nodes(ToolingCstDocument *document) {
    ToolingCstNode root = {
        .kind = TOOLING_CST_ROOT,
        .parent = SIZE_MAX,
        .first_token = 0,
        .end_token = document->token_count,
        .closed = true,
    };
    if (!push_node(document, root, NULL)) return false;
    size_t *stack = malloc((document->token_count + 1) * sizeof(*stack));
    if (!stack) return false;
    size_t depth = 1;
    stack[0] = 0;
    for (size_t i = 0; i < document->token_count; i++) {
        if (document->tokens[i].kind == TOOLING_CST_ERROR)
            document->has_errors = true;
        ToolingCstNodeKind kind = opener_kind(document, i);
        if (kind != TOOLING_CST_ROOT) {
            ToolingCstNode group = {
                .kind = kind,
                .parent = stack[depth - 1],
                .first_token = i,
                .end_token = document->token_count,
                .closed = false,
            };
            size_t node_index;
            if (!push_node(document, group, &node_index)) {
                free(stack);
                return false;
            }
            stack[depth++] = node_index;
        } else if (closing_token(document, i)) {
            if (depth > 1 &&
                closes_kind(document, i,
                            document->nodes[stack[depth - 1]].kind)) {
                ToolingCstNode *group = &document->nodes[stack[--depth]];
                group->end_token = i + 1;
                group->closed = true;
            } else {
                ToolingCstNode error = {
                    .kind = TOOLING_CST_ERROR_NODE,
                    .parent = stack[depth - 1],
                    .first_token = i,
                    .end_token = i + 1,
                    .closed = false,
                };
                if (!push_node(document, error, NULL)) {
                    free(stack);
                    return false;
                }
                document->has_errors = true;
            }
        }
    }
    if (depth > 1) document->has_errors = true;
    free(stack);
    return true;
}

static void advance_position(const char *source, size_t start, size_t end,
                             size_t *line, size_t *column) {
    for (size_t i = start; i < end; i++) {
        if (source[i] == '\n') {
            (*line)++;
            *column = 1;
        } else {
            (*column)++;
        }
    }
}

static size_t scan_quoted(const char *source, size_t length, size_t start,
                          unsigned char quote, bool *closed) {
    size_t cursor = start + 1;
    bool escaped = false;
    while (cursor < length) {
        unsigned char byte = (unsigned char)source[cursor++];
        if (!escaped && byte == quote) {
            *closed = true;
            return cursor;
        }
        if (byte == '\n' && quote == '\'') return cursor;
        if (escaped) escaped = false;
        else escaped = byte == '\\';
    }
    return cursor;
}

static uint64_t hash_byte(uint64_t hash, unsigned char byte) {
    return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t compute_fingerprint(const ToolingCstDocument *document) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < document->token_count; i++) {
        const ToolingCstToken *token = &document->tokens[i];
        hash = hash_byte(hash, (unsigned char)token->kind);
        for (size_t j = 0; j < token->length; j++)
            hash = hash_byte(hash,
                (unsigned char)document->source[token->offset + j]);
    }
    for (size_t i = 0; i < document->node_count; i++) {
        const ToolingCstNode *node = &document->nodes[i];
        hash = hash_byte(hash, (unsigned char)node->kind);
        hash = hash_byte(hash, node->closed ? 1 : 0);
    }
    return hash;
}

ToolingCstDocument *tooling_cst_parse(const char *path, const char *source) {
    if (!path || !source) return NULL;
    ToolingCstDocument *document = calloc(1, sizeof(*document));
    if (!document) return NULL;
    document->path = copy_text(path);
    document->source = copy_text(source);
    if (!document->path || !document->source) {
        tooling_cst_dispose(document);
        return NULL;
    }
    document->source_length = strlen(source);

    size_t cursor = 0, line = 1, column = 1;
    while (cursor < document->source_length) {
        size_t start = cursor, start_line = line, start_column = column;
        ToolingCstTokenKind kind;
        unsigned char byte = (unsigned char)source[cursor];
        if (byte == '\n' || (byte == '\r' && cursor + 1 < document->source_length &&
                             source[cursor + 1] == '\n')) {
            kind = TOOLING_CST_NEWLINE;
            cursor += byte == '\r' ? 2 : 1;
        } else if (byte == ' ' || byte == '\t' || byte == '\r') {
            kind = TOOLING_CST_WHITESPACE;
            while (cursor < document->source_length &&
                   (source[cursor] == ' ' || source[cursor] == '\t' ||
                    (source[cursor] == '\r' &&
                     (cursor + 1 == document->source_length ||
                      source[cursor + 1] != '\n')))) cursor++;
        } else if (byte == ';') {
            kind = TOOLING_CST_LINE_COMMENT;
            while (cursor < document->source_length && source[cursor] != '\n' &&
                   source[cursor] != '\r') cursor++;
        } else if (byte == '-' && cursor + 1 < document->source_length &&
                   source[cursor + 1] == '|') {
            kind = TOOLING_CST_BLOCK_COMMENT;
            cursor += 2;
            while (cursor < document->source_length &&
                   !(source[cursor] == '|' &&
                     cursor + 1 < document->source_length &&
                     source[cursor + 1] == '-')) cursor++;
            if (cursor < document->source_length) cursor += 2;
        } else if (byte == '"' || byte == '\'') {
            bool closed = false;
            kind = byte == '"' ? TOOLING_CST_STRING : TOOLING_CST_CHARACTER;
            cursor = scan_quoted(source, document->source_length, cursor,
                                 byte, &closed);
            if (!closed) kind = TOOLING_CST_ERROR;
        } else if (punctuation(byte) ||
                   (byte == '-' && cursor + 1 < document->source_length &&
                    source[cursor + 1] == '>')) {
            kind = TOOLING_CST_PUNCTUATION;
            cursor += byte == '-' ? 2 : 1;
        } else {
            kind = TOOLING_CST_ATOM;
            cursor++;
            while (cursor < document->source_length) {
                unsigned char next = (unsigned char)source[cursor];
                if (isspace(next) || next == ';' || next == '"' || next == '\'' ||
                    punctuation(next) ||
                    (next == '-' && cursor + 1 < document->source_length &&
                     (source[cursor + 1] == '>' || source[cursor + 1] == '|')))
                    break;
                cursor++;
            }
        }
        if (!push_token(document, kind, start, cursor - start,
                        start_line, start_column)) {
            tooling_cst_dispose(document);
            return NULL;
        }
        advance_position(source, start, cursor, &line, &column);
    }
    if (!build_nodes(document)) {
        tooling_cst_dispose(document);
        return NULL;
    }
    document->fingerprint = compute_fingerprint(document);
    return document;
}

void tooling_cst_dispose(ToolingCstDocument *document) {
    if (!document) return;
    free(document->path);
    free(document->source);
    free(document->tokens);
    free(document->nodes);
    free(document);
}

bool tooling_cst_validate(const ToolingCstDocument *document) {
    if (!document || !document->source) return false;
    if (!document->node_count || document->nodes[0].kind != TOOLING_CST_ROOT ||
        document->nodes[0].parent != SIZE_MAX ||
        document->nodes[0].first_token != 0 ||
        document->nodes[0].end_token != document->token_count)
        return false;
    size_t expected = 0;
    for (size_t i = 0; i < document->token_count; i++) {
        const ToolingCstToken *token = &document->tokens[i];
        if (!token->length || token->offset != expected ||
            token->offset + token->length > document->source_length)
            return false;
        expected += token->length;
    }
    if (expected != document->source_length) return false;
    for (size_t i = 1; i < document->node_count; i++) {
        const ToolingCstNode *node = &document->nodes[i];
        if (node->parent >= i || node->first_token >= node->end_token ||
            node->end_token > document->token_count)
            return false;
        const ToolingCstNode *parent = &document->nodes[node->parent];
        if (node->first_token < parent->first_token ||
            node->end_token > parent->end_token)
            return false;
    }
    return true;
}

size_t tooling_cst_token_count(const ToolingCstDocument *document) {
    return document ? document->token_count : 0;
}

const ToolingCstToken *tooling_cst_token(const ToolingCstDocument *document,
                                         size_t index) {
    return document && index < document->token_count
        ? &document->tokens[index] : NULL;
}

size_t tooling_cst_node_count(const ToolingCstDocument *document) {
    return document ? document->node_count : 0;
}

const ToolingCstNode *tooling_cst_node(const ToolingCstDocument *document,
                                      size_t index) {
    return document && index < document->node_count
        ? &document->nodes[index] : NULL;
}

const ToolingCstNode *tooling_cst_root(const ToolingCstDocument *document) {
    return document && document->node_count ? &document->nodes[0] : NULL;
}

bool tooling_cst_has_errors(const ToolingCstDocument *document) {
    return !document || document->has_errors;
}

const char *tooling_cst_source(const ToolingCstDocument *document) {
    return document ? document->source : NULL;
}

uint64_t tooling_cst_fingerprint(const ToolingCstDocument *document) {
    return document ? document->fingerprint : 0;
}

char *tooling_cst_render(const ToolingCstDocument *document, size_t *length) {
    if (length) *length = 0;
    if (!document || !tooling_cst_validate(document)) return NULL;
    char *rendered = malloc(document->source_length + 1);
    if (!rendered) return NULL;
    size_t cursor = 0;
    for (size_t i = 0; i < document->token_count; i++) {
        const ToolingCstToken *token = &document->tokens[i];
        memcpy(rendered + cursor, document->source + token->offset, token->length);
        cursor += token->length;
    }
    rendered[cursor] = '\0';
    if (length) *length = cursor;
    return rendered;
}

char *tooling_cst_code_projection(const ToolingCstDocument *document) {
    if (!document || !tooling_cst_validate(document)) return NULL;
    char *projection = copy_text(document->source);
    if (!projection) return NULL;
    for (size_t i = 0; i < document->token_count; i++) {
        const ToolingCstToken *token = &document->tokens[i];
        bool trivia = token->kind == TOOLING_CST_LINE_COMMENT  ||
                      token->kind == TOOLING_CST_BLOCK_COMMENT ||
                      token->kind == TOOLING_CST_STRING        ||
                      token->kind == TOOLING_CST_CHARACTER;
        if (!trivia) continue;
        for (size_t j = 0; j < token->length; j++) {
            size_t offset = token->offset + j;
            if (projection[offset] != '\n' && projection[offset] != '\r')
                projection[offset] = ' ';
        }
    }
    return projection;
}
