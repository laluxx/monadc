/*
:TEST-ID tests.embedding.bidirectional.zero-wrapper
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.03.bidirectional.zero-wrapper
:TEST-QUALITY integration
:TEST-SUBSET bidirectional
:TEST-CATEGORY 03-native-interop
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.ffi.category-contract
:TEST-PURPOSE C and Monad native functions cross the embedding boundary through exact validated addresses in both directions.
:TEST-ATOM register C -> resolve once -> Monad direct call; export Monad -> C direct call
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c bidirectional native registry and structural ABI validation
:TEST-USES-ATOM atom.embedding.category.03.native-interop
:TEST-MENU branches/bidirectional/atom.embedding.03.bidirectional.zero-wrapper
:TEST-MENU-PATH embedding/branches/bidirectional/atom.embedding.03.bidirectional.zero-wrapper
:TEST-ATOM-LEAF atom.embedding.03.bidirectional.zero-wrapper
:TEST-CLI-TARGET embedding branches bidirectional
*/
#include <monad/embed.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int64_t c_double(int64_t value) { return value * 2; }
static int64_t (*resolved_c_double)(int64_t);

/* This stands in for the compiler-emitted body during this API slice. Its
 * foreign call is one ordinary indirect C-ABI call, with no embedding call. */
static int64_t monad_then_increment(int64_t value) {
    return resolved_c_double(value) + 1;
}

int main(void) {
    const monad_abi_type_t parameters[] = {MONAD_ABI_I64};
    const monad_abi_signature_t signature = {
        sizeof(signature), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, parameters, 1
    };
    const monad_abi_type_t float_parameters[] = {MONAD_ABI_F64};
    const monad_abi_signature_t wrong_signature = {
        sizeof(wrong_signature), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, float_parameters, 1
    };
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_native_registration_t *registration;
    monad_native_import_t *import;
    monad_native_address_t address;
    monad_error_t error;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_runtime_register_native(
        thread, "host.double", &signature,
        (monad_native_address_t)c_double, "Double an integer in the C host.",
        &registration, &error) == MONAD_OK);
    assert(monad_runtime_resolve_native(
        thread, "host.double", &wrong_signature, &import, &address, &error) ==
           MONAD_ERROR_SIGNATURE_MISMATCH);
    assert(monad_runtime_resolve_native(
        thread, "host.double", &signature, &import, &address, &error) == MONAD_OK);
    assert(address == (monad_native_address_t)c_double);
    resolved_c_double = (int64_t (*)(int64_t))address;
    assert(monad_then_increment(20) == 41);

    /* The reverse direction is equally direct. */
    address = (monad_native_address_t)monad_then_increment;
    assert(((int64_t (*)(int64_t))address)(20) == 41);

    assert(monad_native_registration_release(registration, &error) ==
           MONAD_ERROR_BUSY);
    monad_native_import_release(import);
    assert(monad_native_registration_release(registration, &error) == MONAD_OK);
    assert(monad_runtime_resolve_native(
        thread, "host.double", &signature, &import, &address, &error) ==
           MONAD_ERROR_NOT_FOUND);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("bidirectional zero-wrapper calls ok");
    return 0;
}
