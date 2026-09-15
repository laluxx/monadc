#include "compiler.h"
#include <stdlib.h>
#include <string.h>

static int compiler_verbosity;
static int compiler_explicit_trace;
static bool compiler_user_module;

static bool env_enabled_by_default(const char *name) {
    const char *value = getenv(name);
    return !value || !value[0] || strcmp(value, "0") != 0;
}

bool qtt_compiler_analysis_enabled(void) {
    return env_enabled_by_default("MONAD_QTT_ANALYSIS");
}

bool qtt_compiler_memory_enabled(void) {
    return qtt_compiler_analysis_enabled() &&
           env_enabled_by_default("MONAD_QTT_MEMORY");
}

bool qtt_compiler_diagnostics_enabled(void) {
    const char *value = getenv("MONAD_QTT_SHADOW");
    return value && value[0] && strcmp(value, "0") != 0;
}

void qtt_compiler_set_trace(int verbosity, int explicit_trace) {
    compiler_verbosity = verbosity;
    compiler_explicit_trace = explicit_trace;
}

void qtt_compiler_set_user_module(bool is_user_module) {
    compiler_user_module = is_user_module;
}

static int trace_level(void) {
    if (compiler_explicit_trace) return compiler_explicit_trace;
    return compiler_verbosity > 1 ? QTT_TRACE_PROOF
         : compiler_verbosity > 0 ? QTT_TRACE_SUMMARY
                                  : QTT_TRACE_NONE;
}

bool qtt_compiler_trace_enabled(void) {
    return trace_level() >= QTT_TRACE_SUMMARY &&
           (compiler_user_module || trace_level() >= QTT_TRACE_ALL);
}

bool qtt_compiler_trace_detailed(void) {
    return trace_level() >= QTT_TRACE_PROOF &&
           (compiler_user_module || trace_level() >= QTT_TRACE_ALL);
}

bool qtt_compiler_trace_all(void) {
    return trace_level() >= QTT_TRACE_ALL;
}

uint64_t qtt_compiler_module_id(const char *identity) {
    uint64_t module_id = UINT64_C(1469598103934665603);
    const char *stable = identity ? identity : "<unknown>";
    for (const unsigned char *p = (const unsigned char *)stable;
         *p; p++) {
        module_id ^= *p;
        module_id *= UINT64_C(1099511628211);
    }
    return module_id;
}
