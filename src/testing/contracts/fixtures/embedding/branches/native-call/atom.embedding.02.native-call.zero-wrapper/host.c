/*
:TEST-ID tests.embedding.native-call.zero-wrapper
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.02.native-call.zero-wrapper
:TEST-QUALITY integration
:TEST-SUBSET native-call
:TEST-CATEGORY 02-certified-native
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi
:TEST-PURPOSE Native lookup validates once and returns the implementation pointer with no call wrapper.
:TEST-ATOM descriptor -> lookup -> exact pointer -> direct Int -> Int call
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c native descriptor lifecycle and lookup
:TEST-USES-ATOM atom.embedding.category.02.certified-native
:TEST-MENU branches/native-call/atom.embedding.02.native-call.zero-wrapper
:TEST-MENU-PATH embedding/branches/native-call/atom.embedding.02.native-call.zero-wrapper
:TEST-ATOM-LEAF atom.embedding.02.native-call.zero-wrapper
:TEST-CLI-TARGET embedding branches native-call
*/
#include <monad/embed.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int initialize_count;
static void initialize(void) { initialize_count++; }
static int64_t increment(int64_t value) { return value + 1; }

int main(void) {
    const monad_abi_type_t int_parameter[] = {MONAD_ABI_I64};
    const monad_abi_signature_t int_to_int = {
        sizeof(int_to_int), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, int_parameter, 1
    };
    const monad_abi_type_t float_parameter[] = {MONAD_ABI_F64};
    const monad_abi_signature_t float_to_int = {
        sizeof(float_to_int), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, float_parameter, 1
    };
    const monad_native_export_descriptor_t exports[] = {
        {"increment", &int_to_int, UINT64_C(0x91),
         (monad_native_address_t)increment}
    };
    const monad_module_descriptor_t descriptor = {
        sizeof(descriptor), MONAD_MODULE_ABI_VERSION, "NativeFixture",
        UINT64_C(0x1234), initialize, exports, 1
    };
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_module_t *module;
    monad_function_t *function;
    monad_native_address_t address;
    monad_error_t error;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_module_bind(thread, &descriptor, UINT64_C(0x1234),
                             &module, &error) == MONAD_OK);
    assert(initialize_count == 1);
    assert(monad_abi_signature_equal(&int_to_int, &int_to_int));
    assert(!monad_abi_signature_equal(&int_to_int, &float_to_int));
    assert(monad_module_find_native(module, "increment", &float_to_int,
                                    &function, &address, &error) ==
           MONAD_ERROR_SIGNATURE_MISMATCH);
    assert(monad_module_find_native(module, "increment", &int_to_int,
                                    &function, &address, &error) == MONAD_OK);
    assert(address == (monad_native_address_t)increment);
    assert(((int64_t (*)(int64_t))address)(41) == 42);
    assert(monad_module_unload(module, &error) == MONAD_ERROR_BUSY);
    monad_function_release(function);
    assert(monad_module_unload(module, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("zero-wrapper native call ok");
    return 0;
}
