#ifndef MONAD_TOOLING_FORMAT_H
#define MONAD_TOOLING_FORMAT_H

#include <stdbool.h>

typedef enum {
    FORMAT_CONTROL_GLYPH,
    FORMAT_CONTROL_ASCII
} FormatControlStyle;

int cmd_format(const char *path, FormatControlStyle style,
               bool write_changes, bool check_only);

#endif
