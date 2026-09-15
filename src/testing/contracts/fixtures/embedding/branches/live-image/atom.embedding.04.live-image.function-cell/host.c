/*
:TEST-ID tests.embedding.live-image.function-cell
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.04.live-image.function-cell
:TEST-QUALITY integration
:TEST-SUBSET live-image
:TEST-CATEGORY 04-live-reflection
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.env.entry
:TEST-PURPOSE A retained live function cell observes atomic redefinition and exposes editor-grade metadata without changing identity.
:TEST-ATOM define f -> inspect -> call -> redefine f -> same cell calls new body
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c live native cell generation and metadata
:TEST-USES-ATOM atom.embedding.category.04.live-reflection
:TEST-MENU branches/live-image/atom.embedding.04.live-image.function-cell
:TEST-MENU-PATH embedding/branches/live-image/atom.embedding.04.live-image.function-cell
:TEST-ATOM-LEAF atom.embedding.04.live-image.function-cell
:TEST-CLI-TARGET embedding branches live-image
*/
#include <monad/embed.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int64_t first(int64_t value) { return value + 1; }
static int64_t second(int64_t value) { return value + 100; }

int main(void) {
    const monad_abi_type_t parameters[] = {MONAD_ABI_I64};
    const monad_abi_signature_t signature = {
        sizeof(signature), MONAD_NATIVE_ABI_VERSION, MONAD_ABI_C,
        MONAD_ABI_I64, parameters, 1
    };
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_native_registration_t *cell;
    monad_native_import_t *live;
    monad_native_address_t ignored;
    monad_binding_info_t info;
    monad_error_t error;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    assert(monad_runtime_register_native(
        thread, "editor.transform", &signature,
        (monad_native_address_t)first, "Transform the current editor value.",
        &cell, &error) == MONAD_OK);
    assert(monad_runtime_resolve_native(
        thread, "editor.transform", &signature, &live, &ignored, &error) ==
           MONAD_OK);
    assert(monad_native_registration_info(cell, &info, &error) == MONAD_OK);
    assert(strcmp(info.name, "editor.transform") == 0);
    assert(strcmp(info.docstring, "Transform the current editor value.") == 0);
    assert(info.kind == MONAD_BINDING_FUNCTION && info.generation == 1);
    assert(((int64_t (*)(int64_t))monad_native_import_address(live))(1) == 2);

    assert(monad_native_registration_redefine(
        cell, &signature, (monad_native_address_t)second,
        "Updated while the editor is running.", &error) == MONAD_OK);
    assert(((int64_t (*)(int64_t))monad_native_import_address(live))(1) == 101);
    assert(monad_native_registration_info(cell, &info, &error) == MONAD_OK);
    assert(info.generation == 2);
    assert(strcmp(info.docstring, "Updated while the editor is running.") == 0);

    monad_native_import_release(live);
    assert(monad_native_registration_release(cell, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("live function cell redefined ok");
    return 0;
}
