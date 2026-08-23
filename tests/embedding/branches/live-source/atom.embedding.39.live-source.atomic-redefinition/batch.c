/*
:TEST-ID tests.embedding.live-source.atomic-batch-rollback
:TEST-CONTEXT dec.embedding.live-image
:TEST-PURPOSE Prove validation failure rolls back every staged native mutation.
:TEST-ATOM atom.embedding.39.live-source.atomic-batch-rollback
:TEST-EXPECT conflicting second export preserves first address and generation
:TEST-MENU-PATH embedding/branches/live-source/atom.embedding.39.live-source.atomic-redefinition
*/
#include "embed/runtime_internal.h"

#include <assert.h>
#include <stdint.h>

static int64_t first(int64_t value) { return value + 1; }
static int64_t replacement(int64_t value) { return value + 100; }

int main(void) {
    const monad_abi_type_t parameters[] = {MONAD_ABI_I64};
    const monad_abi_signature_t signature = {
        sizeof(signature), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, parameters, 1};
    monad_runtime_config_t config;
    monad_runtime_t *runtime = NULL;
    monad_thread_t *thread = NULL;
    monad_native_registration_t *left = NULL;
    monad_native_import_t *live = NULL;
    monad_variable_t *blocked = NULL;
    monad_native_address_t address = NULL;
    monad_error_t error = {0};
    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_runtime_register_native(
        thread, "left", &signature, (monad_native_address_t)first, NULL,
        &left, &error) == MONAD_OK);
    assert(monad_runtime_define_i64(
        thread, "blocked", 0, NULL, &blocked, &error) == MONAD_OK);
    assert(monad_runtime_resolve_native(
        thread, "left", &signature, &live, &address, &error) == MONAD_OK);

    const char *names[] = {"left", "blocked"};
    const monad_native_address_t addresses[] = {
        (monad_native_address_t)replacement,
        (monad_native_address_t)replacement};
    monad_native_registration_t *registrations[2] = {0};
    unsigned char created[2] = {0};
    assert(monad_runtime_publish_native_batch(
        thread, names, addresses, NULL, 2, &signature,
        NULL, registrations, created, &error) == MONAD_ERROR_ALREADY_EXISTS);
    assert(((int64_t (*)(int64_t))monad_native_import_address(live))(1) == 2);
    monad_binding_info_t info;
    assert(monad_native_registration_info(left, &info, &error) == MONAD_OK);
    assert(info.generation == 1);

    monad_native_import_release(live);
    assert(monad_variable_release(blocked, &error) == MONAD_OK);
    assert(monad_native_registration_release(left, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    return 0;
}
