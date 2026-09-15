#ifndef MONAD_EMBED_H
#define MONAD_EMBED_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(MONAD_EMBED_BUILD)
#    define MONAD_API __declspec(dllexport)
#  else
#    define MONAD_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define MONAD_API __attribute__((visibility("default")))
#else
#  define MONAD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MONAD_EMBED_ABI_VERSION UINT32_C(2)

typedef struct monad_runtime monad_runtime_t;
typedef struct monad_thread monad_thread_t;
typedef struct monad_module monad_module_t;
typedef struct monad_function monad_function_t;
typedef struct monad_native_registration monad_native_registration_t;
typedef struct monad_native_import monad_native_import_t;
typedef struct monad_variable monad_variable_t;
typedef struct monad_binding_snapshot monad_binding_snapshot_t;
typedef struct monad_foreign_type monad_foreign_type_t;
typedef struct monad_foreign_object monad_foreign_object_t;
typedef void (*monad_native_address_t)(void);

#define MONAD_MODULE_ABI_VERSION UINT32_C(1)
#define MONAD_NATIVE_ABI_VERSION UINT32_C(1)
#define MONAD_FOREIGN_TYPE_ABI_VERSION UINT32_C(2)

typedef enum monad_abi_calling_convention {
    MONAD_ABI_C = 1
} monad_abi_calling_convention_t;

/* Closed native ABI atoms. Composite and managed values are intentionally
 * absent until their representation and ownership judgments are complete. */
typedef enum monad_abi_type {
    MONAD_ABI_VOID = 0,
    MONAD_ABI_I8 = 1,
    MONAD_ABI_U8 = 2,
    MONAD_ABI_I16 = 3,
    MONAD_ABI_U16 = 4,
    MONAD_ABI_I32 = 5,
    MONAD_ABI_U32 = 6,
    MONAD_ABI_I64 = 7,
    MONAD_ABI_U64 = 8,
    MONAD_ABI_F32 = 9,
    MONAD_ABI_F64 = 10
} monad_abi_type_t;

typedef struct monad_abi_signature {
    uint32_t struct_size;
    uint32_t abi_version;
    monad_abi_calling_convention_t calling_convention;
    monad_abi_type_t result;
    const monad_abi_type_t *parameters;
    size_t parameter_count;
} monad_abi_signature_t;

/* Structural equality, not hash equality, authorizes an indirect call.
 * LLVM requires caller and callee prototypes to match; significant mismatch
 * can be undefined behavior: https://llvm.org/docs/LangRef.html#call-instruction
 */

typedef struct monad_native_export_descriptor {
    const char *name;
    const monad_abi_signature_t *signature;
    uint64_t qtt_fingerprint;
    monad_native_address_t address;
} monad_native_export_descriptor_t;

typedef struct monad_module_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *name;
    uint64_t artifact_fingerprint;
    void (*initialize)(void);
    const monad_native_export_descriptor_t *exports;
    size_t export_count;
} monad_module_descriptor_t;

typedef enum monad_status {
    MONAD_OK = 0,
    MONAD_ERROR_INVALID_ARGUMENT = 1,
    MONAD_ERROR_OUT_OF_MEMORY = 2,
    MONAD_ERROR_ABI_MISMATCH = 3,
    MONAD_ERROR_BUSY = 4,
    MONAD_ERROR_WRONG_THREAD = 5,
    MONAD_ERROR_NOT_FOUND = 6,
    MONAD_ERROR_SIGNATURE_MISMATCH = 7,
    MONAD_ERROR_CERTIFICATE_MISMATCH = 8,
    MONAD_ERROR_ALREADY_EXISTS = 9,
    MONAD_ERROR_TYPE_MISMATCH = 10,
    MONAD_ERROR_OWNERSHIP_VIOLATION = 11,
    MONAD_ERROR_IO = 12,
    MONAD_ERROR_INTERNAL = 255
} monad_status_t;

typedef void *(*monad_alloc_fn)(void *context, size_t size);
typedef void (*monad_free_fn)(void *context, void *pointer);

typedef struct monad_allocator {
    void *context;
    monad_alloc_fn allocate;
    monad_free_fn deallocate;
} monad_allocator_t;

typedef struct monad_runtime_config {
    uint32_t struct_size;
    uint32_t abi_version;
    monad_allocator_t allocator;
} monad_runtime_config_t;

typedef struct monad_error {
    monad_status_t status;
    const char *message;
} monad_error_t;

/* The successful create operation transfers payload ownership to the object.
 * Its destroy callback runs synchronously on final release unless an executor
 * is supplied. No collector or hidden finalizer thread is involved. */
typedef void (*monad_foreign_destroy_fn)(void *payload, void *context);
typedef void (*monad_foreign_job_fn)(void *job_context);
/* schedule takes ownership of one finalization job and must invoke run(context)
 * exactly once. It may invoke it inline. The type and payload remain pinned
 * until that invocation completes. */
typedef void (*monad_foreign_schedule_fn)(
    void *context, monad_foreign_job_fn run, void *job_context);

typedef enum monad_foreign_ownership {
    MONAD_FOREIGN_UNIQUE = 1,
    MONAD_FOREIGN_SHARED = 2
} monad_foreign_ownership_t;

/* Runtime-generative nominal authority. Both words participate in identity;
 * neither is a portable structural type fingerprint. */
typedef struct monad_foreign_type_identity {
    uint64_t runtime;
    uint64_t local;
} monad_foreign_type_identity_t;

typedef struct monad_foreign_executor {
    void *context;
    monad_foreign_schedule_fn schedule;
} monad_foreign_executor_t;

typedef struct monad_foreign_type_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *name;
    const char *docstring;
    monad_foreign_ownership_t ownership;
    monad_foreign_destroy_fn destroy;
    void *destroy_context;
    monad_foreign_executor_t executor;
} monad_foreign_type_descriptor_t;

typedef enum monad_binding_kind {
    MONAD_BINDING_FUNCTION = 1,
    MONAD_BINDING_VARIABLE = 2,
    MONAD_BINDING_FOREIGN_TYPE = 3
} monad_binding_kind_t;

typedef struct monad_binding_info {
    uint32_t struct_size;
    monad_binding_kind_t kind;
    const char *name;
    const char *docstring;
    const monad_abi_signature_t *signature;
    monad_abi_type_t value_type;
    int64_t i64_value;
    uint64_t generation;
    monad_foreign_type_identity_t foreign_type_identity;
    monad_foreign_ownership_t foreign_ownership;
    int foreign_has_executor;
} monad_binding_info_t;

/* info strings and signature are borrowed from the registration and remain
 * valid only until that binding is redefined or released. Inventory APIs will
 * return independently retained snapshots for concurrent editor tooling. */

#define MONAD_BINDING_MASK_FUNCTION (UINT32_C(1) << MONAD_BINDING_FUNCTION)
#define MONAD_BINDING_MASK_VARIABLE (UINT32_C(1) << MONAD_BINDING_VARIABLE)
#define MONAD_BINDING_MASK_FOREIGN_TYPE \
    (UINT32_C(1) << MONAD_BINDING_FOREIGN_TYPE)
#define MONAD_BINDING_MASK_ALL \
    (MONAD_BINDING_MASK_FUNCTION | MONAD_BINDING_MASK_VARIABLE | \
     MONAD_BINDING_MASK_FOREIGN_TYPE)

typedef struct monad_binding_filter {
    uint32_t struct_size;
    uint32_t kind_mask;
    monad_abi_type_t exact_type;
    const char *name_prefix;
} monad_binding_filter_t;

MONAD_API uint32_t monad_embed_abi_version(void);
MONAD_API const char *monad_embed_version_string(void);
MONAD_API const char *monad_status_name(monad_status_t status);
MONAD_API int monad_abi_signature_equal(
    const monad_abi_signature_t *left,
    const monad_abi_signature_t *right);

MONAD_API void monad_runtime_config_init(monad_runtime_config_t *config);
MONAD_API monad_status_t monad_runtime_create(
    const monad_runtime_config_t *config,
    monad_runtime_t **runtime,
    monad_error_t *error);
MONAD_API monad_status_t monad_runtime_destroy(
    monad_runtime_t *runtime,
    monad_error_t *error);

MONAD_API monad_status_t monad_thread_attach(
    monad_runtime_t *runtime,
    monad_thread_t **thread,
    monad_error_t *error);
MONAD_API monad_status_t monad_thread_detach(
    monad_thread_t *thread,
    monad_error_t *error);
MONAD_API monad_runtime_t *monad_thread_runtime(monad_thread_t *thread);

MONAD_API void monad_foreign_type_descriptor_init(
    monad_foreign_type_descriptor_t *descriptor);
MONAD_API monad_status_t monad_runtime_register_foreign_type(
    monad_thread_t *thread,
    const monad_foreign_type_descriptor_t *descriptor,
    monad_foreign_type_t **type,
    monad_error_t *error);
MONAD_API monad_status_t monad_foreign_type_release(
    monad_foreign_type_t *type,
    monad_error_t *error);
MONAD_API monad_status_t monad_foreign_type_identity(
    const monad_foreign_type_t *type,
    monad_foreign_type_identity_t *identity,
    monad_error_t *error);
MONAD_API int monad_foreign_type_identity_equal(
    monad_foreign_type_identity_t left,
    monad_foreign_type_identity_t right);
MONAD_API monad_status_t monad_foreign_object_create(
    monad_foreign_type_t *type,
    void *payload,
    monad_foreign_object_t **object,
    monad_error_t *error);
MONAD_API monad_status_t monad_foreign_object_retain(
    monad_foreign_object_t *object,
    monad_error_t *error);
/* Certified shared-DUP primitive for compiler-emitted code. The caller must
 * already hold a live shared object reference. */
MONAD_API void monad_foreign_object_retain_shared(
    monad_foreign_object_t *object);
MONAD_API void monad_foreign_object_release(monad_foreign_object_t *object);
MONAD_API monad_status_t monad_foreign_object_payload(
    const monad_foreign_object_t *object,
    const monad_foreign_type_t *expected_type,
    void **payload,
    monad_error_t *error);

MONAD_API monad_status_t monad_runtime_register_native(
    monad_thread_t *thread,
    const char *name,
    const monad_abi_signature_t *signature,
    monad_native_address_t address,
    const char *docstring,
    monad_native_registration_t **registration,
    monad_error_t *error);
MONAD_API monad_status_t monad_runtime_resolve_native(
    monad_thread_t *thread,
    const char *name,
    const monad_abi_signature_t *expected_signature,
    monad_native_import_t **native_import,
    monad_native_address_t *address,
    monad_error_t *error);
MONAD_API void monad_native_import_release(monad_native_import_t *native_import);
MONAD_API monad_native_address_t monad_native_import_address(
    const monad_native_import_t *native_import);
MONAD_API monad_status_t monad_native_registration_redefine(
    monad_native_registration_t *registration,
    const monad_abi_signature_t *signature,
    monad_native_address_t address,
    const char *docstring,
    monad_error_t *error);
MONAD_API monad_status_t monad_native_registration_info(
    const monad_native_registration_t *registration,
    monad_binding_info_t *info,
    monad_error_t *error);
MONAD_API monad_status_t monad_native_registration_release(
    monad_native_registration_t *registration,
    monad_error_t *error);

MONAD_API monad_status_t monad_runtime_define_i64(
    monad_thread_t *thread,
    const char *name,
    int64_t initial_value,
    const char *docstring,
    monad_variable_t **variable,
    monad_error_t *error);
MONAD_API monad_status_t monad_variable_get_i64(
    const monad_variable_t *variable,
    int64_t *value,
    monad_error_t *error);
MONAD_API monad_status_t monad_variable_set_i64(
    monad_variable_t *variable,
    int64_t value,
    monad_error_t *error);
MONAD_API monad_status_t monad_variable_compare_exchange_i64(
    monad_variable_t *variable,
    int64_t *expected,
    int64_t replacement,
    int *changed,
    monad_error_t *error);
MONAD_API monad_status_t monad_variable_release(
    monad_variable_t *variable,
    monad_error_t *error);

MONAD_API void monad_binding_filter_init(monad_binding_filter_t *filter);
MONAD_API monad_status_t monad_runtime_snapshot_bindings(
    monad_thread_t *thread,
    const monad_binding_filter_t *filter,
    monad_binding_snapshot_t **snapshot,
    monad_error_t *error);
MONAD_API size_t monad_binding_snapshot_count(
    const monad_binding_snapshot_t *snapshot);
MONAD_API const monad_binding_info_t *monad_binding_snapshot_at(
    const monad_binding_snapshot_t *snapshot,
    size_t index);
MONAD_API void monad_binding_snapshot_release(
    monad_binding_snapshot_t *snapshot);

MONAD_API monad_status_t monad_module_bind(
    monad_thread_t *thread,
    const monad_module_descriptor_t *descriptor,
    uint64_t expected_artifact_fingerprint,
    monad_module_t **module,
    monad_error_t *error);
MONAD_API monad_status_t monad_module_find_native(
    monad_module_t *module,
    const char *name,
    const monad_abi_signature_t *expected_signature,
    monad_function_t **function,
    monad_native_address_t *address,
    monad_error_t *error);
MONAD_API void monad_function_release(monad_function_t *function);
MONAD_API monad_status_t monad_module_unload(
    monad_module_t *module,
    monad_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
