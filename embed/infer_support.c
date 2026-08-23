#include <stdbool.h>
#include <stdio.h>

/* The embedding compiler has no dependent-checker process globals. Inference
 * tracing remains private and disabled; this preserves the inferencer's
 * existing direct branch without linking the unrelated CLI checker. */
bool g_trace_enabled;
int g_trace_depth;

void shared_trace_indent(void) {
    if (!g_trace_enabled) return;
    for (int i = 0; i < g_trace_depth; i++) fputs("  ", stderr);
}
