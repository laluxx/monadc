#include "include/monad/embed.h"
#include "runtime_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * Public embedding ABI design references (primary documentation):
 *
 * - Guile initialization and protected VM entry:
 *   https://www.gnu.org/software/guile/manual/html_node/Initialization.html
 * - Lua state, stack, and protected-call API:
 *   https://www.lua.org/manual/5.4/manual.html
 * - CPython stable/limited ABI and thread attachment:
 *   https://docs.python.org/3/c-api/stable.html
 *   https://docs.python.org/3/c-api/threads.html
 * - Julia embedding and native @cfunction entrypoints:
 *   https://docs.julialang.org/en/v1/manual/embedding/
 * - LLVM ORCv2 sessions, symbol lookup, and resource tracking:
 *   https://llvm.org/docs/ORCv2.html
 * - LLVM ORC absolute symbols used to bind host addresses into a JITDylib:
 *   https://llvm.org/doxygen/Orc_8h.html
 * - Guile module variable cells and reflective mutation:
 *   https://www.gnu.org/software/guile/manual/html_node/Variables.html
 *   https://www.gnu.org/software/guile/manual/html_node/Module-System-Reflection.html
 * - Emacs symbol function-cell indirection and runtime replacement:
 *   https://www.gnu.org/software/emacs/manual/html_node/elisp/Function-Indirection.html
 * - Guile nominal foreign object types and checked access:
 *   https://www.gnu.org/software/guile/manual/html_node/Foreign-Objects.html
 *   https://www.gnu.org/software/guile/manual/html_node/Type-Checking-of-Foreign-Objects.html
 * - Vulkan object lifetime and external host synchronization:
 *   https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-objectmodel-lifetime
 * - Dreyer, A Type System for Higher-Order Modules (generative abstract types):
 *   https://people.mpi-sws.org/~dreyer/courses/modules/dreyer03.pdf
 *
 * These references motivate opaque handles, explicit thread attachment,
 * protected error boundaries, a stable C facade over unstable internals, and
 * separate dynamic and native-fast call paths. They do not imply ABI
 * compatibility with any referenced project.
 */

#ifndef MONAD_VERSION_STRING
#define MONAD_VERSION_STRING "0.1-dev"
#endif

struct monad_runtime {
    monad_allocator_t allocator;
    pthread_mutex_t lock;
    size_t attached_threads;
    size_t loaded_modules;
    struct monad_native_registration *native_registrations;
    struct monad_variable *variables;
    struct monad_foreign_type *foreign_types;
    uint64_t binding_epoch;
    uint64_t identity;
    uint64_t next_foreign_type_identity;
};

struct monad_thread {
    monad_runtime_t *runtime;
    pthread_t owner;
};

struct monad_module {
    monad_runtime_t *runtime;
    const monad_module_descriptor_t *descriptor;
    size_t retained_functions;
};

struct monad_function {
    monad_module_t *module;
};

struct monad_native_registration {
    monad_runtime_t *runtime;
    char *name;
    char *docstring;
    monad_abi_signature_t signature;
    monad_abi_type_t *parameters;
    monad_native_address_t address;
    uint64_t generation;
    size_t consumers;
    struct monad_native_owner *owner;
    struct monad_native_registration *next;
};

struct monad_native_import {
    monad_native_registration_t *registration;
    struct monad_native_owner *owner;
};

typedef struct {
    const char *name;
    monad_native_address_t address;
    monad_native_registration_t *registration;
    struct monad_native_owner *owner;
} monad_native_read_entry_t;

struct monad_native_owner {
    size_t references;
    void *value;
    monad_native_owner_destroy_t destroy;
};

/* The final reference owns the whole LLJIT image. ORCv2 explicitly assigns
 * generated definitions to removable resource lifetimes; this owner supplies
 * the host-side old-reader grace period before destruction:
 * https://llvm.org/docs/ORCv2.html#how-to-remove-code
 */

monad_native_owner_t *monad_native_owner_create(
    void *value, monad_native_owner_destroy_t destroy) {
    if (!value || !destroy) return NULL;
    monad_native_owner_t *owner = malloc(sizeof(*owner));
    if (!owner) return NULL;
    *owner = (monad_native_owner_t){1, value, destroy};
    return owner;
}

void monad_native_owner_retain(monad_native_owner_t *owner) {
    if (owner) __atomic_add_fetch(&owner->references, 1, __ATOMIC_RELAXED);
}

void monad_native_owner_release(monad_native_owner_t *owner) {
    if (!owner) return;
    if (__atomic_sub_fetch(&owner->references, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    owner->destroy(owner->value);
    free(owner);
}

struct monad_native_read_snapshot {
    monad_allocator_t allocator;
    monad_runtime_t *runtime;
    uint64_t generation;
    size_t count;
    monad_native_read_entry_t entries[];
};

static int native_read_entry_compare(const void *left, const void *right) {
    const monad_native_read_entry_t *a = left;
    const monad_native_read_entry_t *b = right;
    return strcmp(a->name, b->name);
}

struct monad_variable {
    monad_runtime_t *runtime;
    char *name;
    char *docstring;
    int64_t value;
    uint64_t generation;
    struct monad_variable *next;
};

struct monad_binding_snapshot {
    monad_allocator_t allocator;
    size_t count;
    monad_binding_info_t *items;
};

struct monad_foreign_type {
    monad_runtime_t *runtime;
    char *name;
    char *docstring;
    monad_foreign_destroy_fn destroy;
    void *destroy_context;
    monad_foreign_ownership_t ownership;
    monad_foreign_executor_t executor;
    monad_foreign_type_identity_t identity;
    size_t live_objects;
    struct monad_foreign_type *next;
};

/* A process-local generative stamp, not a structural or persistent hash. */
static uint64_t next_runtime_identity;

struct monad_foreign_object {
    monad_foreign_type_t *type;
    void *payload;
    size_t references;
};

static void *default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void default_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

static monad_status_t fail(monad_error_t *error, monad_status_t status,
                           const char *message) {
    if (error) {
        error->status = status;
        error->message = message;
    }
    return status;
}

static void clear_error(monad_error_t *error) {
    if (error) {
        error->status = MONAD_OK;
        error->message = NULL;
    }
}

static char *allocator_copy_string(monad_runtime_t *runtime,
                                   const char *source);

uint32_t monad_embed_abi_version(void) { return MONAD_EMBED_ABI_VERSION; }
const char *monad_embed_version_string(void) { return MONAD_VERSION_STRING; }

const char *monad_status_name(monad_status_t status) {
    switch (status) {
    case MONAD_OK:                         return "ok";
    case MONAD_ERROR_INVALID_ARGUMENT:     return "invalid argument";
    case MONAD_ERROR_OUT_OF_MEMORY:        return "out of memory";
    case MONAD_ERROR_ABI_MISMATCH:         return "ABI mismatch";
    case MONAD_ERROR_BUSY:                 return "busy";
    case MONAD_ERROR_WRONG_THREAD:         return "wrong thread";
    case MONAD_ERROR_NOT_FOUND:            return "not found";
    case MONAD_ERROR_SIGNATURE_MISMATCH:   return "signature mismatch";
    case MONAD_ERROR_CERTIFICATE_MISMATCH: return "certificate mismatch";
    case MONAD_ERROR_ALREADY_EXISTS:       return "already exists";
    case MONAD_ERROR_TYPE_MISMATCH:        return "type mismatch";
    case MONAD_ERROR_OWNERSHIP_VIOLATION:  return "ownership violation";
    case MONAD_ERROR_IO:                   return "I/O error";
    case MONAD_ERROR_INTERNAL:             return "internal error";
    }
    return "unknown status";
}

static int abi_type_valid(monad_abi_type_t type, int result_position) {
    if (type == MONAD_ABI_VOID) return result_position;
    return type >= MONAD_ABI_I8 && type <= MONAD_ABI_F64;
}

static int abi_signature_valid(const monad_abi_signature_t *signature) {
    if (!signature || signature->struct_size < sizeof(*signature) ||
        signature->abi_version != MONAD_NATIVE_ABI_VERSION ||
        signature->calling_convention != MONAD_ABI_C ||
        !abi_type_valid(signature->result, 1) ||
        (signature->parameter_count && !signature->parameters))
        return 0;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (!abi_type_valid(signature->parameters[i], 0)) return 0;
    return 1;
}

int monad_abi_signature_equal(const monad_abi_signature_t *left,
                              const monad_abi_signature_t *right) {
    if (!abi_signature_valid(left) || !abi_signature_valid(right) ||
        left->abi_version != right->abi_version ||
        left->calling_convention != right->calling_convention ||
        left->result != right->result ||
        left->parameter_count != right->parameter_count)
        return 0;
    for (size_t i = 0; i < left->parameter_count; i++)
        if (left->parameters[i] != right->parameters[i]) return 0;
    return 1;
}

void monad_runtime_config_init(monad_runtime_config_t *config) {
    if (!config) return;
    config->struct_size = (uint32_t)sizeof(*config);
    config->abi_version = MONAD_EMBED_ABI_VERSION;
    config->allocator.context = NULL;
    config->allocator.allocate = default_allocate;
    config->allocator.deallocate = default_deallocate;
}

monad_status_t monad_runtime_create(const monad_runtime_config_t *config,
                                    monad_runtime_t **runtime,
                                    monad_error_t *error) {
    clear_error(error);
    if (!config || !runtime)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "config and runtime output are required");
    *runtime = NULL;
    if (config->struct_size < sizeof(monad_runtime_config_t))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "runtime config is smaller than this ABI requires");
    if (config->abi_version != MONAD_EMBED_ABI_VERSION)
        return fail(error, MONAD_ERROR_ABI_MISMATCH,
                    "requested embedding ABI is not supported");
    if (!config->allocator.allocate || !config->allocator.deallocate)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "allocator callbacks must be provided as a pair");

    monad_runtime_t *created = config->allocator.allocate(
        config->allocator.context, sizeof(*created));
    if (!created)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate the Monad runtime");
    created->allocator = config->allocator;
    created->attached_threads = 0;
    created->loaded_modules = 0;
    created->native_registrations = NULL;
    created->variables = NULL;
    created->foreign_types = NULL;
    created->binding_epoch = 0;
    created->identity = __atomic_add_fetch(
        &next_runtime_identity, UINT64_C(1), __ATOMIC_RELAXED);
    created->next_foreign_type_identity = 0;
    if (pthread_mutex_init(&created->lock, NULL) != 0) {
        created->allocator.deallocate(created->allocator.context, created);
        return fail(error, MONAD_ERROR_INTERNAL,
                    "could not initialize the runtime lock");
    }
    *runtime = created;
    return MONAD_OK;
}

monad_status_t monad_runtime_destroy(monad_runtime_t *runtime,
                                     monad_error_t *error) {
    clear_error(error);
    if (!runtime)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "runtime is required");
    pthread_mutex_lock(&runtime->lock);
    if (runtime->attached_threads != 0 || runtime->loaded_modules != 0 ||
        runtime->native_registrations != NULL || runtime->variables != NULL ||
        runtime->foreign_types != NULL) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_BUSY,
                    "detach every thread before destroying the runtime");
    }
    pthread_mutex_unlock(&runtime->lock);
    pthread_mutex_destroy(&runtime->lock);
    monad_allocator_t allocator = runtime->allocator;
    allocator.deallocate(allocator.context, runtime);
    return MONAD_OK;
}

monad_status_t monad_thread_attach(monad_runtime_t *runtime,
                                   monad_thread_t **thread,
                                   monad_error_t *error) {
    clear_error(error);
    if (!runtime || !thread)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "runtime and thread output are required");
    *thread = NULL;
    monad_thread_t *attached = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*attached));
    if (!attached)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate the thread attachment");
    attached->runtime = runtime;
    attached->owner = pthread_self();
    pthread_mutex_lock(&runtime->lock);
    runtime->attached_threads++;
    pthread_mutex_unlock(&runtime->lock);
    *thread = attached;
    return MONAD_OK;
}

monad_status_t monad_thread_detach(monad_thread_t *thread,
                                   monad_error_t *error) {
    clear_error(error);
    if (!thread)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread attachment is required");
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "a thread attachment must be detached by its owner");
    monad_runtime_t *runtime = thread->runtime;
    pthread_mutex_lock(&runtime->lock);
    runtime->attached_threads--;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, thread);
    return MONAD_OK;
}

monad_runtime_t *monad_thread_runtime(monad_thread_t *thread) {
    if (!thread || !pthread_equal(thread->owner, pthread_self())) return NULL;
    return thread->runtime;
}

void monad_foreign_type_descriptor_init(
    monad_foreign_type_descriptor_t *descriptor) {
    if (!descriptor) return;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->struct_size = (uint32_t)sizeof(*descriptor);
    descriptor->abi_version = MONAD_FOREIGN_TYPE_ABI_VERSION;
    descriptor->ownership = MONAD_FOREIGN_SHARED;
}

monad_status_t monad_runtime_register_foreign_type(
    monad_thread_t *thread, const monad_foreign_type_descriptor_t *descriptor,
    monad_foreign_type_t **type, monad_error_t *error) {
    clear_error(error);
    if (!thread || !descriptor || !type || !descriptor->name ||
        !descriptor->name[0])
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, named descriptor, and type output are required");
    *type = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "foreign type registration requires its attached thread");
    if (descriptor->struct_size < sizeof(*descriptor))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign type descriptor is smaller than required");
    if (descriptor->abi_version != MONAD_FOREIGN_TYPE_ABI_VERSION)
        return fail(error, MONAD_ERROR_ABI_MISMATCH,
                    "foreign type descriptor ABI is not supported");
    if (descriptor->ownership != MONAD_FOREIGN_UNIQUE &&
        descriptor->ownership != MONAD_FOREIGN_SHARED)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign ownership must be unique or shared");

    monad_runtime_t *runtime = thread->runtime;
    monad_foreign_type_t *created = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*created));
    if (!created)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate foreign type");
    memset(created, 0, sizeof(*created));
    created->name = allocator_copy_string(runtime, descriptor->name);
    created->docstring = allocator_copy_string(runtime, descriptor->docstring);
    if (!created->name || (descriptor->docstring && !created->docstring)) {
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->docstring);
        runtime->allocator.deallocate(runtime->allocator.context, created->name);
        runtime->allocator.deallocate(runtime->allocator.context, created);
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not copy foreign type metadata");
    }
    created->runtime = runtime;
    created->destroy = descriptor->destroy;
    created->destroy_context = descriptor->destroy_context;
    created->ownership = descriptor->ownership;
    created->executor = descriptor->executor;
    pthread_mutex_lock(&runtime->lock);
    for (monad_foreign_type_t *entry = runtime->foreign_types;
         entry; entry = entry->next) {
        if (strcmp(entry->name, descriptor->name) == 0) {
            pthread_mutex_unlock(&runtime->lock);
            runtime->allocator.deallocate(runtime->allocator.context,
                                          created->docstring);
            runtime->allocator.deallocate(runtime->allocator.context,
                                          created->name);
            runtime->allocator.deallocate(runtime->allocator.context, created);
            return fail(error, MONAD_ERROR_ALREADY_EXISTS,
                        "foreign type name is already registered");
        }
    }
    created->next = runtime->foreign_types;
    created->identity.runtime = runtime->identity;
    created->identity.local = ++runtime->next_foreign_type_identity;
    runtime->foreign_types = created;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    *type = created;
    return MONAD_OK;
}

monad_status_t monad_foreign_type_release(monad_foreign_type_t *type,
                                          monad_error_t *error) {
    clear_error(error);
    if (!type)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign type is required");
    monad_runtime_t *runtime = type->runtime;
    pthread_mutex_lock(&runtime->lock);
    if (type->live_objects != 0) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_BUSY,
                    "release every foreign object before its type");
    }
    monad_foreign_type_t **link = &runtime->foreign_types;
    while (*link && *link != type) link = &(*link)->next;
    if (!*link) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign type does not belong to its runtime");
    }
    *link = type->next;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, type->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, type->name);
    runtime->allocator.deallocate(runtime->allocator.context, type);
    return MONAD_OK;
}

monad_status_t monad_foreign_type_identity(
    const monad_foreign_type_t *type,
    monad_foreign_type_identity_t *identity, monad_error_t *error) {
    clear_error(error);
    if (!type || !identity)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign type and identity output are required");
    *identity = type->identity;
    return MONAD_OK;
}

int monad_foreign_type_identity_equal(monad_foreign_type_identity_t left,
                                      monad_foreign_type_identity_t right) {
    return left.runtime != 0 && left.local != 0 &&
           left.runtime == right.runtime && left.local == right.local;
}

monad_status_t monad_foreign_object_create(
    monad_foreign_type_t *type, void *payload,
    monad_foreign_object_t **object, monad_error_t *error) {
    clear_error(error);
    if (!type || !object)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign type and object output are required");
    *object = NULL;
    monad_runtime_t *runtime = type->runtime;
    monad_foreign_object_t *created = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*created));
    if (!created)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate foreign object");
    created->type = type;
    created->payload = payload;
    created->references = 1;
    pthread_mutex_lock(&runtime->lock);
    type->live_objects++;
    pthread_mutex_unlock(&runtime->lock);
    *object = created;
    return MONAD_OK;
}

monad_status_t monad_foreign_object_retain(monad_foreign_object_t *object,
                                           monad_error_t *error) {
    clear_error(error);
    if (!object)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "foreign object is required");
    if (object->type->ownership == MONAD_FOREIGN_UNIQUE)
        return fail(error, MONAD_ERROR_OWNERSHIP_VIOLATION,
                    "a unique foreign object cannot be retained");
    __atomic_add_fetch(&object->references, 1, __ATOMIC_RELAXED);
    return MONAD_OK;
}

void monad_foreign_object_retain_shared(monad_foreign_object_t *object) {
    __atomic_add_fetch(&object->references, 1, __ATOMIC_RELAXED);
}

static void foreign_object_finalize(void *context) {
    monad_foreign_object_t *object = context;
    monad_foreign_type_t *type = object->type;
    monad_runtime_t *runtime = type->runtime;
    /* Keep live_objects nonzero during the host callback. This pins the type,
     * its callback, and callback context without invoking host code under the
     * runtime lock. */
    if (type->destroy) type->destroy(object->payload, type->destroy_context);
    pthread_mutex_lock(&runtime->lock);
    type->live_objects--;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, object);
}

void monad_foreign_object_release(monad_foreign_object_t *object) {
    if (!object) return;
    if (__atomic_sub_fetch(&object->references, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    monad_foreign_type_t *type = object->type;
    if (type->executor.schedule) {
        type->executor.schedule(type->executor.context,
                                foreign_object_finalize, object);
        return;
    }
    foreign_object_finalize(object);
}

monad_status_t monad_foreign_object_payload(
    const monad_foreign_object_t *object,
    const monad_foreign_type_t *expected_type, void **payload,
    monad_error_t *error) {
    clear_error(error);
    if (!object || !expected_type || !payload)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "object, expected type, and payload output are required");
    *payload = NULL;
    if (object->type != expected_type)
        return fail(error, MONAD_ERROR_TYPE_MISMATCH,
                    "foreign object has a different nominal type");
    *payload = object->payload;
    return MONAD_OK;
}

static int runtime_has_binding_locked(monad_runtime_t *runtime,
                                      const char *name) {
    for (monad_native_registration_t *function = runtime->native_registrations;
         function; function = function->next)
        if (strcmp(function->name, name) == 0) return 1;
    for (monad_variable_t *variable = runtime->variables;
         variable; variable = variable->next)
        if (strcmp(variable->name, name) == 0) return 1;
    return 0;
}

static monad_native_registration_t *allocate_native_registration(
    monad_runtime_t *runtime, const char *name,
    const monad_abi_signature_t *signature, monad_native_address_t address,
    const char *docstring) {
    monad_native_registration_t *created = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*created));
    if (!created) return NULL;
    memset(created, 0, sizeof(*created));
    size_t name_size = strlen(name) + 1;
    created->name = runtime->allocator.allocate(
        runtime->allocator.context, name_size);
    if (!created->name) goto failed;
    memcpy(created->name, name, name_size);
    if (docstring) {
        size_t size = strlen(docstring) + 1;
        created->docstring = runtime->allocator.allocate(
            runtime->allocator.context, size);
        if (!created->docstring) goto failed;
        memcpy(created->docstring, docstring, size);
    }
    if (signature->parameter_count) {
        size_t bytes = signature->parameter_count * sizeof(*created->parameters);
        created->parameters = runtime->allocator.allocate(
            runtime->allocator.context, bytes);
        if (!created->parameters) goto failed;
        memcpy(created->parameters, signature->parameters, bytes);
    }
    created->runtime = runtime;
    created->signature = *signature;
    created->signature.parameters = created->parameters;
    created->address = address;
    created->generation = 1;
    return created;
failed:
    runtime->allocator.deallocate(runtime->allocator.context, created->parameters);
    runtime->allocator.deallocate(runtime->allocator.context, created->name);
    runtime->allocator.deallocate(runtime->allocator.context, created->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, created);
    return NULL;
}

static void free_native_registration(monad_native_registration_t *entry) {
    if (!entry) return;
    monad_runtime_t *runtime = entry->runtime;
    runtime->allocator.deallocate(runtime->allocator.context, entry->parameters);
    runtime->allocator.deallocate(runtime->allocator.context, entry->name);
    runtime->allocator.deallocate(runtime->allocator.context, entry->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, entry);
}

/* Internal all-or-nothing publisher. Every allocation precedes the single
 * mutex-protected validation/commit, following the prepare/commit structure
 * of transactional live programming systems:
 * https://www.kernel.org/doc/html/latest/RCU/Design/Requirements/Requirements.html#publish-subscribe-guarantee
 */
monad_status_t monad_runtime_publish_native_batch(
    monad_thread_t *thread, const char *const *names,
    const monad_native_address_t *addresses, const char *const *docstrings,
    size_t count, const monad_abi_signature_t *signature,
    monad_native_owner_t *owner,
    monad_native_registration_t **registrations, unsigned char *created_flags,
    monad_error_t *error) {
    clear_error(error);
    if (!thread || !names || !addresses || !signature || !registrations ||
        !created_flags || !count || !abi_signature_valid(signature))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "initialized native publication batch is required");
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "native publication requires the owning attached thread");
    for (size_t i = 0; i < count; i++) {
        if (!names[i] || !names[i][0] || !addresses[i])
            return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                        "every native export needs a name and address");
        for (size_t j = 0; j < i; j++)
            if (strcmp(names[i], names[j]) == 0)
                return fail(error, MONAD_ERROR_ALREADY_EXISTS,
                            "native publication contains a duplicate name");
    }
    monad_runtime_t *runtime = thread->runtime;
    monad_native_registration_t **prepared = calloc(count, sizeof(*prepared));
    char **old_docstrings = calloc(count, sizeof(*old_docstrings));
    monad_native_owner_t **old_owners = calloc(count, sizeof(*old_owners));
    if (!prepared || !old_docstrings || !old_owners) {
        free(prepared); free(old_docstrings); free(old_owners);
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate native publication transaction");
    }
    for (size_t i = 0; i < count; i++) {
        prepared[i] = allocate_native_registration(
            runtime, names[i], signature, addresses[i],
            docstrings ? docstrings[i] : NULL);
        if (!prepared[i]) {
            for (size_t j = 0; j < i; j++) free_native_registration(prepared[j]);
            free(prepared); free(old_docstrings); free(old_owners);
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not stage native publication metadata");
        }
    }
    monad_status_t status = MONAD_OK;
    pthread_mutex_lock(&runtime->lock);
    for (size_t i = 0; i < count && status == MONAD_OK; i++) {
        monad_native_registration_t *found = NULL;
        for (monad_native_registration_t *entry = runtime->native_registrations;
             entry; entry = entry->next)
            if (strcmp(entry->name, names[i]) == 0) { found = entry; break; }
        if (found && !monad_abi_signature_equal(&found->signature, signature))
            status = MONAD_ERROR_SIGNATURE_MISMATCH;
        if (!found)
            for (monad_variable_t *variable = runtime->variables;
                 variable; variable = variable->next)
                if (strcmp(variable->name, names[i]) == 0) {
                    status = MONAD_ERROR_ALREADY_EXISTS; break;
                }
        registrations[i] = found;
    }
    if (status == MONAD_OK) {
        for (size_t i = 0; i < count; i++) {
            monad_native_registration_t *entry = registrations[i];
            if (entry) {
                old_docstrings[i] = entry->docstring;
                old_owners[i] = entry->owner;
                entry->docstring = prepared[i]->docstring;
                prepared[i]->docstring = NULL;
                entry->owner = owner;
                monad_native_owner_retain(owner);
                entry->generation++;
                __atomic_store_n(&entry->address, addresses[i], __ATOMIC_RELEASE);
                created_flags[i] = 0;
            } else {
                entry = prepared[i];
                entry->owner = owner;
                monad_native_owner_retain(owner);
                entry->next = runtime->native_registrations;
                runtime->native_registrations = entry;
                registrations[i] = entry;
                prepared[i] = NULL;
                created_flags[i] = 1;
            }
        }
        runtime->binding_epoch++;
    }
    pthread_mutex_unlock(&runtime->lock);
    for (size_t i = 0; i < count; i++) {
        free_native_registration(prepared[i]);
        runtime->allocator.deallocate(runtime->allocator.context,
                                      old_docstrings[i]);
        monad_native_owner_release(old_owners[i]);
    }
    free(prepared); free(old_docstrings); free(old_owners);
    if (status == MONAD_ERROR_SIGNATURE_MISMATCH)
        return fail(error, status,
                    "native generation cannot change an existing signature");
    if (status != MONAD_OK)
        return fail(error, status,
                    "native generation conflicts with an existing binding");
    return MONAD_OK;
}

monad_status_t monad_runtime_register_native(
    monad_thread_t *thread, const char *name,
    const monad_abi_signature_t *signature, monad_native_address_t address,
    const char *docstring,
    monad_native_registration_t **registration, monad_error_t *error) {
    clear_error(error);
    if (!thread || !name || !name[0] || !signature || !address || !registration)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, name, signature, address, and output are required");
    *registration = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "native registration requires the owning attached thread");
    if (!abi_signature_valid(signature))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "native registration signature is malformed");

    monad_runtime_t *runtime = thread->runtime;
    pthread_mutex_lock(&runtime->lock);
    if (runtime_has_binding_locked(runtime, name)) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_ALREADY_EXISTS,
                    "binding name is already defined");
    }
    pthread_mutex_unlock(&runtime->lock);

    monad_native_registration_t *created = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*created));
    if (!created)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate native registration");
    memset(created, 0, sizeof(*created));
    size_t name_size = strlen(name) + 1;
    created->name = runtime->allocator.allocate(
        runtime->allocator.context, name_size);
    if (!created->name) goto allocation_failed;
    memcpy(created->name, name, name_size);
    if (docstring) {
        size_t docstring_size = strlen(docstring) + 1;
        created->docstring = runtime->allocator.allocate(
            runtime->allocator.context, docstring_size);
        if (!created->docstring) goto allocation_failed;
        memcpy(created->docstring, docstring, docstring_size);
    }
    if (signature->parameter_count) {
        size_t parameter_bytes =
            signature->parameter_count * sizeof(*created->parameters);
        created->parameters = runtime->allocator.allocate(
            runtime->allocator.context, parameter_bytes);
        if (!created->parameters) goto allocation_failed;
        memcpy(created->parameters, signature->parameters, parameter_bytes);
    }
    created->runtime = runtime;
    created->signature = *signature;
    created->signature.parameters = created->parameters;
    created->address = address;
    created->generation = 1;
    pthread_mutex_lock(&runtime->lock);
    if (runtime_has_binding_locked(runtime, name)) {
        pthread_mutex_unlock(&runtime->lock);
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->parameters);
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->name);
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->docstring);
        runtime->allocator.deallocate(runtime->allocator.context, created);
        return fail(error, MONAD_ERROR_ALREADY_EXISTS,
                    "binding name is already defined");
    }
    created->next = runtime->native_registrations;
    runtime->native_registrations = created;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    *registration = created;
    return MONAD_OK;

allocation_failed:
    runtime->allocator.deallocate(runtime->allocator.context,
                                  created->parameters);
    runtime->allocator.deallocate(runtime->allocator.context, created->name);
    runtime->allocator.deallocate(runtime->allocator.context,
                                  created->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, created);
    return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                "could not copy native registration metadata");
}

monad_status_t monad_runtime_resolve_native(
    monad_thread_t *thread, const char *name,
    const monad_abi_signature_t *expected_signature,
    monad_native_import_t **native_import,
    monad_native_address_t *address, monad_error_t *error) {
    clear_error(error);
    if (!thread || !name || !expected_signature || !native_import || !address)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, name, signature, and address output are required");
    *native_import = NULL;
    *address = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "native resolution requires the owning attached thread");
    monad_runtime_t *runtime = thread->runtime;
    pthread_mutex_lock(&runtime->lock);
    for (monad_native_registration_t *entry = runtime->native_registrations;
         entry; entry = entry->next) {
        if (strcmp(entry->name, name) != 0) continue;
        if (!monad_abi_signature_equal(&entry->signature, expected_signature)) {
            pthread_mutex_unlock(&runtime->lock);
            return fail(error, MONAD_ERROR_SIGNATURE_MISMATCH,
                        "registered native signature does not match");
        }
        entry->consumers++;
        monad_native_owner_t *owner = entry->owner;
        monad_native_owner_retain(owner);
        *address = __atomic_load_n(&entry->address, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&runtime->lock);
        monad_native_import_t *retained = runtime->allocator.allocate(
            runtime->allocator.context, sizeof(*retained));
        if (!retained) {
            pthread_mutex_lock(&runtime->lock);
            entry->consumers--;
            pthread_mutex_unlock(&runtime->lock);
            monad_native_owner_release(owner);
            *address = NULL;
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not retain the native import");
        }
        retained->registration = entry;
        retained->owner = owner;
        *native_import = retained;
        return MONAD_OK;
    }
    pthread_mutex_unlock(&runtime->lock);
    return fail(error, MONAD_ERROR_NOT_FOUND,
                "native symbol was not found");
}

void monad_native_import_release(monad_native_import_t *native_import) {
    if (!native_import) return;
    monad_native_registration_t *registration = native_import->registration;
    monad_runtime_t *runtime = registration->runtime;
    pthread_mutex_lock(&runtime->lock);
    registration->consumers--;
    pthread_mutex_unlock(&runtime->lock);
    monad_native_owner_release(native_import->owner);
    runtime->allocator.deallocate(runtime->allocator.context, native_import);
}

/* A generation token separates publication from reclamation: it copies one
 * epoch's addresses and retains their cells before releasing the registry
 * lock. This follows the old/new-version lifetime discipline of RCU:
 * https://www.kernel.org/doc/html/latest/RCU/whatisRCU.html
 */
monad_status_t monad_runtime_capture_native_generation(
    monad_thread_t *thread, const monad_abi_signature_t *signature,
    monad_native_read_snapshot_t **output, monad_error_t *error) {
    clear_error(error);
    if (!thread || !signature || !output || !abi_signature_valid(signature))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, native signature, and read output are required");
    *output = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "native generation capture requires the owning thread");
    monad_runtime_t *runtime = thread->runtime;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        size_t count = 0, name_bytes = 0;
        pthread_mutex_lock(&runtime->lock);
        uint64_t epoch = runtime->binding_epoch;
        for (monad_native_registration_t *entry = runtime->native_registrations;
             entry; entry = entry->next)
            if (monad_abi_signature_equal(&entry->signature, signature)) {
                size_t size = strlen(entry->name) + 1;
                if (size > SIZE_MAX - name_bytes) {
                    pthread_mutex_unlock(&runtime->lock);
                    return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                                "native generation names are too large");
                }
                count++;
                name_bytes += size;
            }
        pthread_mutex_unlock(&runtime->lock);
        if (count > (SIZE_MAX - sizeof(monad_native_read_snapshot_t)) /
                    sizeof(monad_native_read_entry_t))
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "native generation is too large");
        size_t bytes = sizeof(monad_native_read_snapshot_t) +
            count * sizeof(monad_native_read_entry_t);
        if (name_bytes > SIZE_MAX - bytes)
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "native generation names are too large");
        bytes += name_bytes;
        monad_native_read_snapshot_t *snapshot = runtime->allocator.allocate(
            runtime->allocator.context, bytes);
        if (!snapshot)
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not allocate native read generation");
        memset(snapshot, 0, bytes);
        snapshot->allocator = runtime->allocator;
        snapshot->runtime = runtime;
        snapshot->generation = epoch;
        char *names = (char *)(snapshot->entries + count);
        pthread_mutex_lock(&runtime->lock);
        if (runtime->binding_epoch != epoch) {
            pthread_mutex_unlock(&runtime->lock);
            runtime->allocator.deallocate(runtime->allocator.context, snapshot);
            continue;
        }
        size_t index = 0;
        for (monad_native_registration_t *entry = runtime->native_registrations;
             entry; entry = entry->next) {
            if (!monad_abi_signature_equal(&entry->signature, signature))
                continue;
            size_t size = strlen(entry->name) + 1;
            memcpy(names, entry->name, size);
            snapshot->entries[index] = (monad_native_read_entry_t){
                names,
                __atomic_load_n(&entry->address, __ATOMIC_ACQUIRE), entry,
                entry->owner};
            monad_native_owner_retain(entry->owner);
            entry->consumers++;
            names += size;
            index++;
        }
        snapshot->count = index;
        pthread_mutex_unlock(&runtime->lock);
        qsort(snapshot->entries, snapshot->count,
              sizeof(*snapshot->entries), native_read_entry_compare);
        *output = snapshot;
        return MONAD_OK;
    }
    return fail(error, MONAD_ERROR_BUSY,
                "native generation changed repeatedly during capture");
}

uint64_t monad_native_read_snapshot_generation(
    const monad_native_read_snapshot_t *snapshot) {
    return snapshot ? snapshot->generation : 0;
}

monad_native_address_t monad_native_read_snapshot_find(
    const monad_native_read_snapshot_t *snapshot, const char *name) {
    if (!snapshot || !name) return NULL;
    monad_native_read_entry_t key = {.name = name};
    const monad_native_read_entry_t *found = bsearch(
        &key, snapshot->entries, snapshot->count,
        sizeof(*snapshot->entries), native_read_entry_compare);
    return found ? found->address : NULL;
}

void monad_native_read_snapshot_release(monad_native_read_snapshot_t *snapshot) {
    if (!snapshot) return;
    monad_runtime_t *runtime = snapshot->runtime;
    pthread_mutex_lock(&runtime->lock);
    for (size_t i = 0; i < snapshot->count; i++)
        snapshot->entries[i].registration->consumers--;
    pthread_mutex_unlock(&runtime->lock);
    for (size_t i = 0; i < snapshot->count; i++)
        monad_native_owner_release(snapshot->entries[i].owner);
    snapshot->allocator.deallocate(snapshot->allocator.context, snapshot);
}

monad_native_address_t monad_native_import_address(
    const monad_native_import_t *native_import) {
    if (!native_import) return NULL;
    return __atomic_load_n(&native_import->registration->address,
                           __ATOMIC_ACQUIRE);
}

monad_status_t monad_native_registration_redefine(
    monad_native_registration_t *registration,
    const monad_abi_signature_t *signature, monad_native_address_t address,
    const char *docstring, monad_error_t *error) {
    clear_error(error);
    if (!registration || !signature || !address)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "registration, signature, and address are required");
    if (!monad_abi_signature_equal(&registration->signature, signature))
        return fail(error, MONAD_ERROR_SIGNATURE_MISMATCH,
                    "live redefinition cannot change the native signature");
    monad_runtime_t *runtime = registration->runtime;
    char *replacement_docstring = NULL;
    if (docstring) {
        size_t size = strlen(docstring) + 1;
        replacement_docstring = runtime->allocator.allocate(
            runtime->allocator.context, size);
        if (!replacement_docstring)
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not copy the updated docstring");
        memcpy(replacement_docstring, docstring, size);
    }
    pthread_mutex_lock(&runtime->lock);
    char *old_docstring = registration->docstring;
    monad_native_owner_t *old_owner = registration->owner;
    registration->docstring = replacement_docstring;
    registration->owner = NULL;
    registration->generation++;
    runtime->binding_epoch++;
    __atomic_store_n(&registration->address, address, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, old_docstring);
    monad_native_owner_release(old_owner);
    return MONAD_OK;
}

monad_status_t monad_native_registration_info(
    const monad_native_registration_t *registration,
    monad_binding_info_t *info, monad_error_t *error) {
    clear_error(error);
    if (!registration || !info)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "registration and binding info output are required");
    monad_runtime_t *runtime = registration->runtime;
    pthread_mutex_lock(&runtime->lock);
    memset(info, 0, sizeof(*info));
    info->struct_size = (uint32_t)sizeof(*info);
    info->kind = MONAD_BINDING_FUNCTION;
    info->name = registration->name;
    info->docstring = registration->docstring;
    info->signature = &registration->signature;
    info->value_type = MONAD_ABI_VOID;
    info->i64_value = 0;
    info->generation = registration->generation;
    pthread_mutex_unlock(&runtime->lock);
    return MONAD_OK;
}

monad_status_t monad_native_registration_release(
    monad_native_registration_t *registration, monad_error_t *error) {
    clear_error(error);
    if (!registration)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "native registration is required");
    monad_runtime_t *runtime = registration->runtime;
    pthread_mutex_lock(&runtime->lock);
    if (registration->consumers != 0) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_BUSY,
                    "release native imports before unregistering the symbol");
    }
    monad_native_registration_t **link = &runtime->native_registrations;
    while (*link && *link != registration) link = &(*link)->next;
    if (!*link) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "native registration does not belong to its runtime");
    }
    *link = registration->next;
    monad_native_owner_t *owner = registration->owner;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context,
                                  registration->parameters);
    runtime->allocator.deallocate(runtime->allocator.context,
                                  registration->name);
    runtime->allocator.deallocate(runtime->allocator.context,
                                  registration->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, registration);
    monad_native_owner_release(owner);
    return MONAD_OK;
}

static char *allocator_copy_string(monad_runtime_t *runtime,
                                   const char *source) {
    if (!source) return NULL;
    size_t size = strlen(source) + 1;
    char *copy = runtime->allocator.allocate(runtime->allocator.context, size);
    if (copy) memcpy(copy, source, size);
    return copy;
}

monad_status_t monad_runtime_define_i64(
    monad_thread_t *thread, const char *name, int64_t initial_value,
    const char *docstring, monad_variable_t **variable, monad_error_t *error) {
    clear_error(error);
    if (!thread || !name || !name[0] || !variable)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, variable name, and output are required");
    *variable = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "variable definition requires the owning attached thread");
    monad_runtime_t *runtime = thread->runtime;
    monad_variable_t *created = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*created));
    if (!created)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate variable cell");
    memset(created, 0, sizeof(*created));
    created->name = allocator_copy_string(runtime, name);
    created->docstring = allocator_copy_string(runtime, docstring);
    if (!created->name || (docstring && !created->docstring)) {
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->docstring);
        runtime->allocator.deallocate(runtime->allocator.context, created->name);
        runtime->allocator.deallocate(runtime->allocator.context, created);
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not copy variable metadata");
    }
    created->runtime = runtime;
    created->value = initial_value;
    created->generation = 1;
    pthread_mutex_lock(&runtime->lock);
    if (runtime_has_binding_locked(runtime, name)) {
        pthread_mutex_unlock(&runtime->lock);
        runtime->allocator.deallocate(runtime->allocator.context,
                                      created->docstring);
        runtime->allocator.deallocate(runtime->allocator.context, created->name);
        runtime->allocator.deallocate(runtime->allocator.context, created);
        return fail(error, MONAD_ERROR_ALREADY_EXISTS,
                    "binding name is already defined");
    }
    created->next = runtime->variables;
    runtime->variables = created;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    *variable = created;
    return MONAD_OK;
}

monad_status_t monad_variable_get_i64(const monad_variable_t *variable,
                                      int64_t *value, monad_error_t *error) {
    clear_error(error);
    if (!variable || !value)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "variable and value output are required");
    *value = __atomic_load_n(&variable->value, __ATOMIC_ACQUIRE);
    return MONAD_OK;
}

monad_status_t monad_variable_set_i64(monad_variable_t *variable,
                                      int64_t value, monad_error_t *error) {
    clear_error(error);
    if (!variable)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "variable is required");
    monad_runtime_t *runtime = variable->runtime;
    pthread_mutex_lock(&runtime->lock);
    __atomic_store_n(&variable->value, value, __ATOMIC_RELEASE);
    variable->generation++;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    return MONAD_OK;
}

monad_status_t monad_variable_compare_exchange_i64(
    monad_variable_t *variable, int64_t *expected, int64_t replacement,
    int *changed, monad_error_t *error) {
    clear_error(error);
    if (!variable || !expected || !changed)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "variable, expected value, and result are required");
    monad_runtime_t *runtime = variable->runtime;
    pthread_mutex_lock(&runtime->lock);
    int64_t current = __atomic_load_n(&variable->value, __ATOMIC_RELAXED);
    if (current == *expected) {
        __atomic_store_n(&variable->value, replacement, __ATOMIC_RELEASE);
        variable->generation++;
        runtime->binding_epoch++;
        *changed = 1;
    } else {
        *expected = current;
        *changed = 0;
    }
    pthread_mutex_unlock(&runtime->lock);
    return MONAD_OK;
}

monad_status_t monad_variable_release(monad_variable_t *variable,
                                      monad_error_t *error) {
    clear_error(error);
    if (!variable)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "variable is required");
    monad_runtime_t *runtime = variable->runtime;
    pthread_mutex_lock(&runtime->lock);
    monad_variable_t **link = &runtime->variables;
    while (*link && *link != variable) link = &(*link)->next;
    if (!*link) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "variable does not belong to its runtime");
    }
    *link = variable->next;
    runtime->binding_epoch++;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context,
                                  variable->docstring);
    runtime->allocator.deallocate(runtime->allocator.context, variable->name);
    runtime->allocator.deallocate(runtime->allocator.context, variable);
    return MONAD_OK;
}

void monad_binding_filter_init(monad_binding_filter_t *filter) {
    if (!filter) return;
    filter->struct_size = (uint32_t)sizeof(*filter);
    filter->kind_mask = MONAD_BINDING_MASK_ALL;
    filter->exact_type = MONAD_ABI_VOID;
    filter->name_prefix = NULL;
}

static int snapshot_name_matches(const char *name, const char *prefix) {
    if (!prefix || !prefix[0]) return 1;
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static int snapshot_function_matches(const monad_binding_filter_t *filter,
                                     const monad_native_registration_t *entry) {
    (void)entry;
    return (filter->kind_mask & MONAD_BINDING_MASK_FUNCTION) &&
           filter->exact_type == MONAD_ABI_VOID &&
           snapshot_name_matches(entry->name, filter->name_prefix);
}

static int snapshot_variable_matches(const monad_binding_filter_t *filter,
                                     const monad_variable_t *entry) {
    return (filter->kind_mask & MONAD_BINDING_MASK_VARIABLE) &&
           (filter->exact_type == MONAD_ABI_VOID ||
            filter->exact_type == MONAD_ABI_I64) &&
           snapshot_name_matches(entry->name, filter->name_prefix);
}

static int snapshot_foreign_type_matches(
    const monad_binding_filter_t *filter,
    const monad_foreign_type_t *entry) {
    return (filter->kind_mask & MONAD_BINDING_MASK_FOREIGN_TYPE) &&
           filter->exact_type == MONAD_ABI_VOID &&
           snapshot_name_matches(entry->name, filter->name_prefix);
}

static size_t align_size(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static size_t snapshot_measure_locked(monad_runtime_t *runtime,
                                      const monad_binding_filter_t *filter,
                                      size_t *count) {
    size_t bytes = align_size(sizeof(monad_binding_snapshot_t),
                              sizeof(void *));
    *count = 0;
    for (monad_native_registration_t *entry = runtime->native_registrations;
         entry; entry = entry->next) {
        if (!snapshot_function_matches(filter, entry)) continue;
        (*count)++;
        bytes += strlen(entry->name) + 1;
        if (entry->docstring) bytes += strlen(entry->docstring) + 1;
        bytes = align_size(bytes, sizeof(void *));
        bytes += sizeof(monad_abi_signature_t) +
                 entry->signature.parameter_count * sizeof(monad_abi_type_t);
    }
    for (monad_variable_t *entry = runtime->variables;
         entry; entry = entry->next) {
        if (!snapshot_variable_matches(filter, entry)) continue;
        (*count)++;
        bytes += strlen(entry->name) + 1;
        if (entry->docstring) bytes += strlen(entry->docstring) + 1;
    }
    for (monad_foreign_type_t *entry = runtime->foreign_types;
         entry; entry = entry->next) {
        if (!snapshot_foreign_type_matches(filter, entry)) continue;
        (*count)++;
        bytes += strlen(entry->name) + 1;
        if (entry->docstring) bytes += strlen(entry->docstring) + 1;
    }
    bytes = align_size(bytes, sizeof(void *));
    bytes += *count * sizeof(monad_binding_info_t);
    return bytes;
}

static char *snapshot_copy_string(char **cursor, const char *source) {
    if (!source) return NULL;
    size_t size = strlen(source) + 1;
    char *result = *cursor;
    memcpy(result, source, size);
    *cursor += size;
    return result;
}

static int compare_binding_info(const void *left, const void *right) {
    const monad_binding_info_t *a = left;
    const monad_binding_info_t *b = right;
    return strcmp(a->name, b->name);
}

monad_status_t monad_runtime_snapshot_bindings(
    monad_thread_t *thread, const monad_binding_filter_t *filter,
    monad_binding_snapshot_t **snapshot, monad_error_t *error) {
    clear_error(error);
    if (!thread || !filter || !snapshot ||
        filter->struct_size < sizeof(*filter))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, initialized filter, and snapshot output are required");
    *snapshot = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "snapshot requires the owning attached thread");
    monad_runtime_t *runtime = thread->runtime;
    for (int attempt = 0; attempt < 8; attempt++) {
        pthread_mutex_lock(&runtime->lock);
        uint64_t epoch = runtime->binding_epoch;
        size_t count = 0;
        size_t bytes = snapshot_measure_locked(runtime, filter, &count);
        pthread_mutex_unlock(&runtime->lock);
        monad_binding_snapshot_t *created = runtime->allocator.allocate(
            runtime->allocator.context, bytes);
        if (!created)
            return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not allocate binding snapshot");
        memset(created, 0, bytes);
        pthread_mutex_lock(&runtime->lock);
        if (epoch != runtime->binding_epoch) {
            pthread_mutex_unlock(&runtime->lock);
            runtime->allocator.deallocate(runtime->allocator.context, created);
            continue;
        }
        created->allocator = runtime->allocator;
        created->count = count;
        created->items = (monad_binding_info_t *)((char *)created + bytes -
                         count * sizeof(monad_binding_info_t));
        char *cursor = (char *)created +
                       align_size(sizeof(*created), sizeof(void *));
        size_t index = 0;
        for (monad_native_registration_t *entry = runtime->native_registrations;
             entry; entry = entry->next) {
            if (!snapshot_function_matches(filter, entry)) continue;
            monad_binding_info_t *info = &created->items[index++];
            info->struct_size = (uint32_t)sizeof(*info);
            info->kind = MONAD_BINDING_FUNCTION;
            info->name = snapshot_copy_string(&cursor, entry->name);
            info->docstring = snapshot_copy_string(&cursor, entry->docstring);
            cursor = (char *)align_size((size_t)cursor, sizeof(void *));
            monad_abi_signature_t *signature = (monad_abi_signature_t *)cursor;
            cursor += sizeof(*signature);
            *signature = entry->signature;
            monad_abi_type_t *parameters = (monad_abi_type_t *)cursor;
            size_t parameter_bytes = signature->parameter_count *
                                     sizeof(*parameters);
            if (parameter_bytes)
                memcpy(parameters, entry->parameters, parameter_bytes);
            cursor += parameter_bytes;
            signature->parameters = parameters;
            info->signature = signature;
            info->value_type = MONAD_ABI_VOID;
            info->generation = entry->generation;
        }
        for (monad_variable_t *entry = runtime->variables;
             entry; entry = entry->next) {
            if (!snapshot_variable_matches(filter, entry)) continue;
            monad_binding_info_t *info = &created->items[index++];
            info->struct_size = (uint32_t)sizeof(*info);
            info->kind = MONAD_BINDING_VARIABLE;
            info->name = snapshot_copy_string(&cursor, entry->name);
            info->docstring = snapshot_copy_string(&cursor, entry->docstring);
            info->value_type = MONAD_ABI_I64;
            info->i64_value = __atomic_load_n(&entry->value, __ATOMIC_RELAXED);
            info->generation = entry->generation;
        }
        for (monad_foreign_type_t *entry = runtime->foreign_types;
             entry; entry = entry->next) {
            if (!snapshot_foreign_type_matches(filter, entry)) continue;
            monad_binding_info_t *info = &created->items[index++];
            info->struct_size = (uint32_t)sizeof(*info);
            info->kind = MONAD_BINDING_FOREIGN_TYPE;
            info->name = snapshot_copy_string(&cursor, entry->name);
            info->docstring = snapshot_copy_string(&cursor, entry->docstring);
            info->value_type = MONAD_ABI_VOID;
            info->generation = 1;
            info->foreign_type_identity = entry->identity;
            info->foreign_ownership = entry->ownership;
            info->foreign_has_executor = entry->executor.schedule != NULL;
        }
        pthread_mutex_unlock(&runtime->lock);
        qsort(created->items, created->count, sizeof(*created->items),
              compare_binding_info);
        *snapshot = created;
        return MONAD_OK;
    }
    return fail(error, MONAD_ERROR_BUSY,
                "bindings changed repeatedly while creating the snapshot");
}

size_t monad_binding_snapshot_count(const monad_binding_snapshot_t *snapshot) {
    return snapshot ? snapshot->count : 0;
}

const monad_binding_info_t *monad_binding_snapshot_at(
    const monad_binding_snapshot_t *snapshot, size_t index) {
    return snapshot && index < snapshot->count ? &snapshot->items[index] : NULL;
}

void monad_binding_snapshot_release(monad_binding_snapshot_t *snapshot) {
    if (!snapshot) return;
    monad_allocator_t allocator = snapshot->allocator;
    allocator.deallocate(allocator.context, snapshot);
}

monad_status_t monad_module_bind(
    monad_thread_t *thread, const monad_module_descriptor_t *descriptor,
    uint64_t expected_artifact_fingerprint, monad_module_t **module,
    monad_error_t *error) {
    clear_error(error);
    if (!thread || !descriptor || !module)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "thread, descriptor, and module output are required");
    *module = NULL;
    if (!pthread_equal(thread->owner, pthread_self()))
        return fail(error, MONAD_ERROR_WRONG_THREAD,
                    "module binding requires the owning attached thread");
    if (descriptor->struct_size < sizeof(*descriptor) || !descriptor->name ||
        (descriptor->export_count && !descriptor->exports))
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "module descriptor is malformed");
    if (descriptor->abi_version != MONAD_MODULE_ABI_VERSION)
        return fail(error, MONAD_ERROR_ABI_MISMATCH,
                    "module ABI is not supported");
    if (descriptor->artifact_fingerprint != expected_artifact_fingerprint)
        return fail(error, MONAD_ERROR_CERTIFICATE_MISMATCH,
                    "module bytes do not match the expected certificate");

    monad_runtime_t *runtime = thread->runtime;
    monad_module_t *bound = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*bound));
    if (!bound)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not allocate the module handle");
    bound->runtime = runtime;
    bound->descriptor = descriptor;
    bound->retained_functions = 0;
    if (descriptor->initialize) descriptor->initialize();
    pthread_mutex_lock(&runtime->lock);
    runtime->loaded_modules++;
    pthread_mutex_unlock(&runtime->lock);
    *module = bound;
    return MONAD_OK;
}

monad_status_t monad_module_find_native(
    monad_module_t *module, const char *name,
    const monad_abi_signature_t *expected_signature,
    monad_function_t **function,
    monad_native_address_t *address, monad_error_t *error) {
    clear_error(error);
    if (!module || !name || !expected_signature || !function || !address)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "module, name, function, and address outputs are required");
    *function = NULL;
    *address = NULL;
    const monad_native_export_descriptor_t *found = NULL;
    for (size_t i = 0; i < module->descriptor->export_count; i++) {
        const monad_native_export_descriptor_t *candidate =
            &module->descriptor->exports[i];
        if (candidate->name && strcmp(candidate->name, name) == 0) {
            found = candidate;
            break;
        }
    }
    if (!found)
        return fail(error, MONAD_ERROR_NOT_FOUND,
                    "native export was not found");
    if (!monad_abi_signature_equal(found->signature, expected_signature))
        return fail(error, MONAD_ERROR_SIGNATURE_MISMATCH,
                    "native export signature does not match");
    if (!found->address)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "native export has no implementation address");

    monad_runtime_t *runtime = module->runtime;
    monad_function_t *retained = runtime->allocator.allocate(
        runtime->allocator.context, sizeof(*retained));
    if (!retained)
        return fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                    "could not retain the native export");
    retained->module = module;
    pthread_mutex_lock(&runtime->lock);
    module->retained_functions++;
    pthread_mutex_unlock(&runtime->lock);
    *function = retained;
    *address = found->address;
    return MONAD_OK;
}

void monad_function_release(monad_function_t *function) {
    if (!function) return;
    monad_module_t *module = function->module;
    monad_runtime_t *runtime = module->runtime;
    pthread_mutex_lock(&runtime->lock);
    module->retained_functions--;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, function);
}

monad_status_t monad_module_unload(monad_module_t *module,
                                   monad_error_t *error) {
    clear_error(error);
    if (!module)
        return fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                    "module is required");
    monad_runtime_t *runtime = module->runtime;
    pthread_mutex_lock(&runtime->lock);
    if (module->retained_functions != 0) {
        pthread_mutex_unlock(&runtime->lock);
        return fail(error, MONAD_ERROR_BUSY,
                    "release native functions before unloading the module");
    }
    runtime->loaded_modules--;
    pthread_mutex_unlock(&runtime->lock);
    runtime->allocator.deallocate(runtime->allocator.context, module);
    return MONAD_OK;
}
