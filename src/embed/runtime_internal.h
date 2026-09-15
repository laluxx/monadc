#ifndef MONAD_RUNTIME_INTERNAL_H
#define MONAD_RUNTIME_INTERNAL_H

#include "include/monad/embed.h"

typedef struct monad_native_read_snapshot monad_native_read_snapshot_t;
typedef struct monad_native_owner monad_native_owner_t;
typedef void (*monad_native_owner_destroy_t)(void *value);

monad_native_owner_t *monad_native_owner_create(
    void *value, monad_native_owner_destroy_t destroy);
void monad_native_owner_retain(monad_native_owner_t *owner);
void monad_native_owner_release(monad_native_owner_t *owner);

monad_status_t monad_runtime_publish_native_batch(
    monad_thread_t *thread, const char *const *names,
    const monad_native_address_t *addresses, const char *const *docstrings,
    size_t count, const monad_abi_signature_t *signature,
    monad_native_owner_t *owner,
    monad_native_registration_t **registrations, unsigned char *created_flags,
    monad_error_t *error);
monad_status_t monad_runtime_capture_native_generation(
    monad_thread_t *thread, const monad_abi_signature_t *signature,
    monad_native_read_snapshot_t **snapshot, monad_error_t *error);
uint64_t monad_native_read_snapshot_generation(
    const monad_native_read_snapshot_t *snapshot);
monad_native_address_t monad_native_read_snapshot_find(
    const monad_native_read_snapshot_t *snapshot, const char *name);
void monad_native_read_snapshot_release(monad_native_read_snapshot_t *snapshot);

#endif
