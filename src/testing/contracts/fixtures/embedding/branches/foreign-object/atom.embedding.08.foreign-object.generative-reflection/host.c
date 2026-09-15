/*
:TEST-ID tests.embedding.foreign-object.generative-reflection
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.08.foreign-object.generative-reflection
:TEST-QUALITY integration
:TEST-SUBSET foreign-object
:TEST-CATEGORY 08-generative-reflection
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.qtt.type-identity
:TEST-PURPOSE Foreign types have runtime-generative nominal authority and immutable editor reflection.
:TEST-ATOM register type -> snapshot identity/policy -> unregister/re-register same name -> identity differs -> old snapshot stable
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c foreign type identity, binding snapshots, kind filters, generation freshness
:TEST-USES-ATOM atom.embedding.category.08.generative-reflection
:TEST-MENU branches/foreign-object/atom.embedding.08.foreign-object.generative-reflection
:TEST-MENU-PATH embedding/branches/foreign-object/atom.embedding.08.foreign-object.generative-reflection
:TEST-ATOM-LEAF atom.embedding.08.foreign-object.generative-reflection
:TEST-CLI-TARGET embedding branches foreign-object
*/
#include <monad/embed.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_foreign_type_descriptor_t descriptor;
    monad_foreign_type_t *first;
    monad_foreign_type_t *second;
    monad_foreign_type_identity_t first_id;
    monad_foreign_type_identity_t second_id;
    monad_binding_filter_t filter;
    monad_binding_snapshot_t *before;
    monad_binding_snapshot_t *after;
    const monad_binding_info_t *info;
    monad_error_t error;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    monad_foreign_type_descriptor_init(&descriptor);
    descriptor.name = "EditorBuffer";
    descriptor.docstring = "A reflected editor buffer.";
    descriptor.ownership = MONAD_FOREIGN_UNIQUE;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &first, &error) == MONAD_OK);
    assert(monad_foreign_type_identity(first, &first_id, &error) == MONAD_OK);

    monad_binding_filter_init(&filter);
    filter.kind_mask = MONAD_BINDING_MASK_FOREIGN_TYPE;
    assert(monad_runtime_snapshot_bindings(
        thread, &filter, &before, &error) == MONAD_OK);
    assert(monad_binding_snapshot_count(before) == 1);
    info = monad_binding_snapshot_at(before, 0);
    assert(info->kind == MONAD_BINDING_FOREIGN_TYPE);
    assert(strcmp(info->name, "EditorBuffer") == 0);
    assert(strcmp(info->docstring, "A reflected editor buffer.") == 0);
    assert(info->foreign_ownership == MONAD_FOREIGN_UNIQUE);
    assert(monad_foreign_type_identity_equal(info->foreign_type_identity,
                                             first_id));

    assert(monad_foreign_type_release(first, &error) == MONAD_OK);
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &second, &error) == MONAD_OK);
    assert(monad_foreign_type_identity(second, &second_id, &error) == MONAD_OK);
    assert(!monad_foreign_type_identity_equal(first_id, second_id));
    assert(monad_foreign_type_identity_equal(info->foreign_type_identity,
                                             first_id));
    assert(monad_runtime_snapshot_bindings(
        thread, &filter, &after, &error) == MONAD_OK);
    assert(monad_foreign_type_identity_equal(
        monad_binding_snapshot_at(after, 0)->foreign_type_identity, second_id));

    monad_binding_snapshot_release(before);
    monad_binding_snapshot_release(after);
    assert(monad_foreign_type_release(second, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("generative foreign reflection ok");
    return 0;
}
