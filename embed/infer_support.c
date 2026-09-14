#include <stdbool.h>
#include <stdio.h>

#include "../dep.h"

/* The embedding compiler has no dependent-checker process globals. Inference
 * tracing remains private and disabled; this preserves the inferencer's
 * existing direct branch without linking the unrelated CLI checker. */
bool g_trace_enabled;
int g_trace_depth;

void shared_trace_indent(void) {
    if (!g_trace_enabled) return;
    for (int i = 0; i < g_trace_depth; i++) fputs("  ", stderr);
}

/* The embedding frontend runs HM inference without a DepCtx.  Keep the
 * dependent-family refinement conservative instead of pulling the complete
 * CLI dependent checker into the SDK library. */
bool dep_is_indexed_family(const DepCtx *ctx, const char *name) {
    (void)ctx;
    (void)name;
    return false;
}
