#ifndef CONFIG_H
#define CONFIG_H

/*  config.h — User Compiler Configuration
 *
 *  Owns the small persistent configuration surface used by the CLI:
 *    · Creates ~/.config/monad/flags on first compiler startup
 *    · Reads whitespace-separated default flags from that file
 *    · Accepts comments with # until end of line
 *    · Mirrors the CLI spelling for trace and optimization defaults
 *        — --trace=all
 *        — trace all
 *        — -O, -O0, -O1, -O2
 *        — -v, -vv, --quiet
 *
 *  The file is intentionally simple: an empty file means no extra defaults.
 *  Later config keys should stay here instead of growing cli.c.
 */

#include "cli.h"

void config_apply_default_flags(CompilerFlags *flags);

#endif
