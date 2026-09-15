#ifndef MONAD_TOOLING_FORMAT_H
#define MONAD_TOOLING_FORMAT_H

#include <stdbool.h>

typedef enum {
    FORMAT_CONTROL_GLYPH,
    FORMAT_CONTROL_ASCII
} FormatControlStyle;

typedef enum {
    FORMAT_DOC_INLINE,
    FORMAT_DOC_GLYPH
} FormatDocStyle;

int cmd_format(const char *path, FormatControlStyle style,
               FormatDocStyle doc_style, bool write_changes,
               bool check_only);

#endif
