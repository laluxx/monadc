#ifndef MONAD_COMPILER_H
#define MONAD_COMPILER_H

#include "embed.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MONAD_COMPILER_ABI_VERSION UINT32_C(1)

typedef struct monad_compiler_session monad_compiler_session_t;
typedef struct monad_compiler_source monad_compiler_source_t;
typedef struct monad_compiler_diagnostic_set monad_compiler_diagnostic_set_t;
typedef struct monad_compiler_environment monad_compiler_environment_t;

#define MONAD_COMPILER_SOURCE_ABI_VERSION UINT32_C(1)

typedef struct monad_compiler_source_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *name;
    const char *bytes;
    size_t byte_count;
    uint64_t version;
} monad_compiler_source_descriptor_t;

typedef enum monad_compiler_diagnostic_severity {
    MONAD_COMPILER_DIAGNOSTIC_NOTE = 1,
    MONAD_COMPILER_DIAGNOSTIC_WARNING = 2,
    MONAD_COMPILER_DIAGNOSTIC_ERROR = 3
} monad_compiler_diagnostic_severity_t;

typedef enum monad_compiler_phase {
    MONAD_COMPILER_PHASE_INPUT = 1,
    MONAD_COMPILER_PHASE_PARSE = 2,
    MONAD_COMPILER_PHASE_TYPE = 3,
    MONAD_COMPILER_PHASE_QTT = 4,
    MONAD_COMPILER_PHASE_CODEGEN = 5
} monad_compiler_phase_t;

typedef struct monad_compiler_diagnostic {
    uint32_t struct_size;
    monad_compiler_diagnostic_severity_t severity;
    monad_compiler_phase_t phase;
    const char *code;
    const char *source_name;
    uint64_t source_version;
    size_t byte_start;
    size_t byte_end;
    uint32_t line;
    uint32_t column;
    const char *message;
} monad_compiler_diagnostic_t;

typedef enum monad_compiler_definition_kind {
    MONAD_COMPILER_DEFINITION_VALUE = 1,
    MONAD_COMPILER_DEFINITION_FUNCTION = 2
} monad_compiler_definition_kind_t;

typedef struct monad_compiler_definition {
    uint32_t struct_size;
    uint64_t id;
    monad_compiler_definition_kind_t kind;
    const char *name;
    const char *principal_scheme;
    const char *source_name;
    uint64_t source_version;
    size_t byte_start;
    size_t byte_end;
    uint32_t line;
    uint32_t column;
    /* NULL when the definition has no documentation. Owned by environment. */
    const char *docstring;
} monad_compiler_definition_t;

typedef struct monad_compiler_definition_filter {
    uint32_t struct_size;
    /* Zero matches either definition kind. */
    monad_compiler_definition_kind_t kind;
    /* NULL matches every name/scheme. Strings are borrowed during each call. */
    const char *name_prefix;
    const char *principal_scheme;
} monad_compiler_definition_filter_t;

typedef enum monad_compiler_representation {
    MONAD_COMPILER_REP_FOREIGN = 1
} monad_compiler_representation_t;

typedef enum monad_compiler_ownership {
    MONAD_COMPILER_OWNERSHIP_CONSUMED = 1,
    MONAD_COMPILER_OWNERSHIP_SHARED = 2
} monad_compiler_ownership_t;

typedef struct monad_compiler_foreign_type_info {
    uint32_t struct_size;
    const char *name;
    monad_foreign_type_identity_t nominal_identity;
    uint64_t canonical_type_id;
    monad_compiler_representation_t representation;
    monad_compiler_ownership_t ownership;
} monad_compiler_foreign_type_info_t;

MONAD_API uint32_t monad_compiler_abi_version(void);
MONAD_API void monad_compiler_source_descriptor_init(
    monad_compiler_source_descriptor_t *descriptor);
/* Copies name and exactly byte_count source bytes. The source unit is immutable
 * and independent of the caller's buffer and of any compiler session. */
MONAD_API monad_status_t monad_compiler_source_create(
    const monad_compiler_source_descriptor_t *descriptor,
    monad_compiler_source_t **source,
    monad_error_t *error);
MONAD_API void monad_compiler_source_destroy(monad_compiler_source_t *source);
MONAD_API const char *monad_compiler_source_name(
    const monad_compiler_source_t *source);
MONAD_API const char *monad_compiler_source_bytes(
    const monad_compiler_source_t *source);
MONAD_API size_t monad_compiler_source_byte_count(
    const monad_compiler_source_t *source);
MONAD_API uint64_t monad_compiler_source_version(
    const monad_compiler_source_t *source);
/* A successful check may contain error diagnostics. API misuse and inability
 * to construct an immutable result are reported by the returned status.
 * MONAD-C0001 denotes invalid UTF-8 input; MONAD-C0002 denotes parser
 * rejection; MONAD-C0003 denotes type-inference rejection. Diagnostic
 * strings are owned by the returned set. */
MONAD_API monad_status_t monad_compiler_check_source(
    monad_compiler_session_t *session,
    const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_error_t *error);
/* Performs parsing and HM inference as one transaction. Environment is
 * non-NULL only when diagnostics contains no errors. Both results are owned,
 * immutable, and independent of source and session lifetime. */
MONAD_API monad_status_t monad_compiler_analyze_source(
    monad_compiler_session_t *session,
    const monad_compiler_source_t *source,
    monad_compiler_diagnostic_set_t **diagnostics,
    monad_compiler_environment_t **environment,
    monad_error_t *error);
MONAD_API void monad_compiler_diagnostic_set_destroy(
    monad_compiler_diagnostic_set_t *diagnostics);
MONAD_API size_t monad_compiler_diagnostic_count(
    const monad_compiler_diagnostic_set_t *diagnostics);
MONAD_API const monad_compiler_diagnostic_t *monad_compiler_diagnostic_at(
    const monad_compiler_diagnostic_set_t *diagnostics,
    size_t index);
MONAD_API void monad_compiler_environment_destroy(
    monad_compiler_environment_t *environment);
MONAD_API size_t monad_compiler_environment_count(
    const monad_compiler_environment_t *environment);
MONAD_API const monad_compiler_definition_t *monad_compiler_environment_at(
    const monad_compiler_environment_t *environment, size_t index);
MONAD_API const monad_compiler_definition_t *
monad_compiler_environment_find_name(
    const monad_compiler_environment_t *environment, const char *name);
MONAD_API const monad_compiler_definition_t *monad_compiler_environment_find_id(
    const monad_compiler_environment_t *environment, uint64_t id);
MONAD_API void monad_compiler_definition_filter_init(
    monad_compiler_definition_filter_t *filter);
MONAD_API size_t monad_compiler_environment_matching_count(
    const monad_compiler_environment_t *environment,
    const monad_compiler_definition_filter_t *filter);
MONAD_API const monad_compiler_definition_t *
monad_compiler_environment_matching_at(
    const monad_compiler_environment_t *environment,
    const monad_compiler_definition_filter_t *filter, size_t index);
MONAD_API monad_status_t monad_compiler_session_create(
    monad_compiler_session_t **session, monad_error_t *error);
MONAD_API void monad_compiler_session_destroy(
    monad_compiler_session_t *session);

/* Atomically replaces the session's complete foreign-type view. The snapshot
 * is only borrowed for this call. Returned info remains valid until the next
 * successful import or session destruction. */
MONAD_API monad_status_t monad_compiler_import_foreign_types(
    monad_compiler_session_t *session,
    const monad_binding_snapshot_t *snapshot,
    monad_error_t *error);
MONAD_API size_t monad_compiler_foreign_type_count(
    const monad_compiler_session_t *session);
MONAD_API const monad_compiler_foreign_type_info_t *
monad_compiler_foreign_type_at(
    const monad_compiler_session_t *session, size_t index);

#ifdef __cplusplus
}
#endif

#endif
