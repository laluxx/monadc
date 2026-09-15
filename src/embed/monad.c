#include "include/monad/monad.h"
#include "surface_internal.h"
#include "runtime_internal.h"

#include <stdlib.h>

typedef struct monad_owned_registration {
    monad_native_registration_t *value;
    struct monad_owned_registration *next;
} monad_owned_registration_t;

typedef struct monad_owned_import {
    monad_native_import_t *value;
    struct monad_owned_import *next;
} monad_owned_import_t;

struct monad {
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_owned_registration_t *registrations;
    monad_owned_import_t *imports;
    size_t active_reads;
    size_t active_handles;
    size_t live_native_images;
};

struct monad_read {
    monad_t *monad;
    monad_native_read_snapshot_t *snapshot;
};

struct monad_i64_handle {
    monad_t *monad;
    monad_native_import_t *native_import;
    monad_i64_function_t address;
};

static const monad_abi_type_t monad_i64_parameter[] = {MONAD_ABI_I64};
static const monad_abi_signature_t monad_i64_signature = {
    sizeof(monad_i64_signature), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
    MONAD_ABI_I64, monad_i64_parameter, 1
};

static monad_status_t surface_fail(
    monad_error_t *error, monad_status_t status, const char *message) {
    if (error) {
        error->status = status;
        error->message = message;
    }
    return status;
}

monad_status_t monad_open(monad_t **output, monad_error_t *error) {
    if (!output)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad output is required");
    *output = NULL;
    monad_t *monad = calloc(1, sizeof(*monad));
    if (!monad)
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not allocate Monad host");
    monad_runtime_config_t config;
    monad_runtime_config_init(&config);
    monad_status_t status = monad_runtime_create(
        &config, &monad->runtime, error);
    if (status == MONAD_OK)
        status = monad_thread_attach(monad->runtime, &monad->thread, error);
    if (status != MONAD_OK) {
        if (monad->runtime) {
            monad_error_t ignored = {0};
            monad_runtime_destroy(monad->runtime, &ignored);
        }
        free(monad);
        return status;
    }
    *output = monad;
    return MONAD_OK;
}

monad_status_t monad_close(monad_t *monad, monad_error_t *error) {
    if (!monad)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host is required");
    if (monad->active_reads || monad->active_handles)
        return surface_fail(error, MONAD_ERROR_BUSY,
                            "release native reads and handles before closing Monad");
    while (monad->imports) {
        monad_owned_import_t *owned = monad->imports;
        monad->imports = owned->next;
        monad_native_import_release(owned->value);
        free(owned);
    }
    monad_status_t first = MONAD_OK;
    while (monad->registrations) {
        monad_owned_registration_t *owned = monad->registrations;
        monad->registrations = owned->next;
        monad_error_t local = {0};
        monad_status_t status =
            monad_native_registration_release(owned->value, &local);
        if (first == MONAD_OK && status != MONAD_OK) {
            first = status;
            if (error) *error = local;
        }
        free(owned);
    }
    monad_error_t local = {0};
    monad_status_t status = monad_thread_detach(monad->thread, &local);
    if (first == MONAD_OK && status != MONAD_OK) {
        first = status;
        if (error) *error = local;
    }
    status = monad_runtime_destroy(monad->runtime, &local);
    if (first == MONAD_OK && status != MONAD_OK) {
        first = status;
        if (error) *error = local;
    }
    free(monad);
    return first;
}

monad_status_t monad_read_begin(
    monad_t *monad, monad_read_t **output, monad_error_t *error) {
    if (!monad || !output)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host and read output are required");
    *output = NULL;
    monad_read_t *read = calloc(1, sizeof(*read));
    if (!read)
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not allocate native read token");
    monad_status_t status = monad_runtime_capture_native_generation(
        monad->thread, &monad_i64_signature, &read->snapshot, error);
    if (status != MONAD_OK) { free(read); return status; }
    read->monad = monad;
    monad->active_reads++;
    *output = read;
    return MONAD_OK;
}

uint64_t monad_read_generation(const monad_read_t *read) {
    return read
        ? monad_native_read_snapshot_generation(read->snapshot) : 0;
}

monad_status_t monad_read_find_i64_function(
    const monad_read_t *read, const char *name,
    monad_i64_function_t *function, monad_error_t *error) {
    if (!read || !name || !function)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "read token, name, and function output are required");
    *function = (monad_i64_function_t)monad_native_read_snapshot_find(
        read->snapshot, name);
    return *function
        ? MONAD_OK
        : surface_fail(error, MONAD_ERROR_NOT_FOUND,
                       "native function is absent from this generation");
}

void monad_read_end(monad_read_t *read) {
    if (!read) return;
    monad_native_read_snapshot_release(read->snapshot);
    read->monad->active_reads--;
    free(read);
}

typedef struct {
    monad_t *monad;
    void *value;
    monad_owned_resource_destroy_t destroy;
} monad_surface_native_resource_t;

static void surface_native_resource_destroy(void *opaque) {
    monad_surface_native_resource_t *resource = opaque;
    resource->destroy(resource->value);
    resource->monad->live_native_images--;
    free(resource);
}

size_t monad_live_native_image_count(const monad_t *monad) {
    return monad ? monad->live_native_images : 0;
}

monad_status_t monad_surface_publish_i64_exports(
    monad_t *monad, const monad_surface_i64_export_t *exports, size_t count,
    void *resource_value, monad_owned_resource_destroy_t resource_destroy,
    monad_error_t *error) {
    if (!monad || !exports || !count || !resource_value || !resource_destroy) {
        if (resource_value && resource_destroy) resource_destroy(resource_value);
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host, exports, and native resource are required");
    }
    monad_surface_native_resource_t *resource = malloc(sizeof(*resource));
    if (!resource) {
        resource_destroy(resource_value);
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not allocate native image ownership");
    }
    *resource = (monad_surface_native_resource_t){
        monad, resource_value, resource_destroy};
    monad_native_owner_t *owner = monad_native_owner_create(
        resource, surface_native_resource_destroy);
    if (!owner) {
        resource_destroy(resource_value);
        free(resource);
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not allocate native image owner");
    }
    monad->live_native_images++;
    const char **names = calloc(count, sizeof(*names));
    const char **docstrings = calloc(count, sizeof(*docstrings));
    monad_native_address_t *addresses = calloc(count, sizeof(*addresses));
    monad_native_registration_t **registrations =
        calloc(count, sizeof(*registrations));
    unsigned char *created = calloc(count, sizeof(*created));
    monad_owned_registration_t **owners = calloc(count, sizeof(*owners));
    if (!names || !docstrings || !addresses || !registrations || !created ||
        !owners) goto allocation_failed;
    for (size_t i = 0; i < count; i++) {
        names[i] = exports[i].name;
        docstrings[i] = exports[i].docstring;
        addresses[i] = (monad_native_address_t)exports[i].address;
        owners[i] = malloc(sizeof(*owners[i]));
        if (!owners[i]) goto allocation_failed;
    }
    monad_status_t status = monad_runtime_publish_native_batch(
        monad->thread, names, addresses, docstrings, count,
        &monad_i64_signature, owner, registrations, created, error);
    if (status == MONAD_OK)
        for (size_t i = 0; i < count; i++) {
            if (!created[i]) { free(owners[i]); continue; }
            owners[i]->value = registrations[i];
            owners[i]->next = monad->registrations;
            monad->registrations = owners[i];
        }
    else
        for (size_t i = 0; i < count; i++) free(owners[i]);
    free(names); free(docstrings); free(addresses); free(registrations);
    free(created); free(owners);
    monad_native_owner_release(owner);
    return status;

allocation_failed:
    if (owners)
        for (size_t i = 0; i < count; i++) free(owners[i]);
    free(names); free(docstrings); free(addresses); free(registrations);
    free(created); free(owners);
    monad_native_owner_release(owner);
    return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                        "could not stage native export ownership");
}

monad_status_t monad_define_i64_function(
    monad_t *monad, const char *name, monad_i64_function_t function,
    const char *docstring, monad_error_t *error) {
    if (!monad || !function)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host and function are required");
    monad_native_registration_t *registration = NULL;
    monad_status_t status = monad_runtime_register_native(
        monad->thread, name, &monad_i64_signature,
        (monad_native_address_t)function, docstring, &registration, error);
    if (status != MONAD_OK) return status;
    monad_owned_registration_t *owned = malloc(sizeof(*owned));
    if (!owned) {
        monad_error_t ignored = {0};
        monad_native_registration_release(registration, &ignored);
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not retain native binding");
    }
    owned->value = registration;
    owned->next = monad->registrations;
    monad->registrations = owned;
    return MONAD_OK;
}

static monad_status_t surface_resolve_i64(
    monad_t *monad, const char *name,
    monad_native_import_t **native_import,
    monad_i64_function_t *function, monad_error_t *error) {
    if (!monad || !name || !native_import || !function)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host, name, and typed outputs are required");
    *native_import = NULL;
    *function = NULL;
    monad_native_address_t address = NULL;
    monad_status_t status = monad_runtime_resolve_native(
        monad->thread, name, &monad_i64_signature,
        native_import, &address, error);
    if (status == MONAD_OK) *function = (monad_i64_function_t)address;
    return status;
}

monad_status_t monad_find_i64_function(
    monad_t *monad, const char *name, monad_i64_function_t *function,
    monad_error_t *error) {
    monad_native_import_t *native_import = NULL;
    monad_status_t status = surface_resolve_i64(
        monad, name, &native_import, function, error);
    if (status != MONAD_OK) return status;
    monad_owned_import_t *owned = malloc(sizeof(*owned));
    if (!owned) {
        monad_native_import_release(native_import);
        *function = NULL;
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not retain native import");
    }
    owned->value = native_import;
    owned->next = monad->imports;
    monad->imports = owned;
    return MONAD_OK;
}

monad_status_t monad_find_i64_handle(
    monad_t *monad, const char *name, monad_i64_handle_t **output,
    monad_error_t *error) {
    if (!monad || !name || !output)
        return surface_fail(error, MONAD_ERROR_INVALID_ARGUMENT,
                            "Monad host, name, and handle output are required");
    *output = NULL;
    monad_native_import_t *native_import = NULL;
    monad_i64_function_t address = NULL;
    monad_status_t status = surface_resolve_i64(
        monad, name, &native_import, &address, error);
    if (status != MONAD_OK) return status;
    monad_i64_handle_t *handle = malloc(sizeof(*handle));
    if (!handle) {
        monad_native_import_release(native_import);
        return surface_fail(error, MONAD_ERROR_OUT_OF_MEMORY,
                            "could not allocate typed function handle");
    }
    *handle = (monad_i64_handle_t){
        monad, native_import, address};
    monad->active_handles++;
    *output = handle;
    return MONAD_OK;
}

monad_i64_function_t monad_i64_handle_address(
    const monad_i64_handle_t *handle) {
    return handle ? handle->address : NULL;
}

void monad_i64_handle_release(monad_i64_handle_t *handle) {
    if (!handle) return;
    monad_native_import_release(handle->native_import);
    handle->monad->active_handles--;
    free(handle);
}
