/*
:TEST-ID tests.embedding.compiler-session.transactional-foreign-import
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.10.compiler-session.transactional-foreign-import
:TEST-QUALITY integration
:TEST-SUBSET compiler-session
:TEST-CATEGORY 10-transactional-foreign-import
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.qtt-foreign-import,monadc.context.embedding.immutable-inventory
:TEST-PURPOSE Installed compiler sessions atomically import runtime foreign reflection into canonical QTT records.
:TEST-ATOM runtime snapshot -> compiler transaction -> query canonical/nominal ownership -> replace with fresh image
:TEST-EXPECT compile, run
:TEST-COVERAGE libmonad-compiler public ABI, QttForeignTypeEnv import, replacement transaction, installation
:TEST-USES-ATOM atom.embedding.category.10.transactional-foreign-import
:TEST-MENU branches/compiler-session/atom.embedding.10.compiler-session.transactional-foreign-import
:TEST-MENU-PATH embedding/branches/compiler-session/atom.embedding.10.compiler-session.transactional-foreign-import
:TEST-ATOM-LEAF atom.embedding.10.compiler-session.transactional-foreign-import
:TEST-CLI-TARGET embedding branches compiler-session
*/
#include <monad/compiler.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_runtime_config_t runtime_config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_foreign_type_descriptor_t descriptor;
    monad_foreign_type_t *type;
    monad_binding_filter_t filter;
    monad_binding_snapshot_t *snapshot;
    monad_compiler_session_t *compiler;
    const monad_compiler_foreign_type_info_t *info;
    monad_error_t error;

    monad_runtime_config_init(&runtime_config);
    assert(monad_runtime_create(&runtime_config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    monad_foreign_type_descriptor_init(&descriptor);
    descriptor.name = "EditorBuffer";
    descriptor.ownership = MONAD_FOREIGN_UNIQUE;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &type, &error) == MONAD_OK);
    monad_binding_filter_init(&filter);
    filter.kind_mask = MONAD_BINDING_MASK_FOREIGN_TYPE;
    assert(monad_runtime_snapshot_bindings(
        thread, &filter, &snapshot, &error) == MONAD_OK);

    assert(monad_compiler_session_create(&compiler, &error) == MONAD_OK);
    assert(monad_compiler_import_foreign_types(
        compiler, snapshot, &error) == MONAD_OK);
    assert(monad_compiler_foreign_type_count(compiler) == 1);
    info = monad_compiler_foreign_type_at(compiler, 0);
    assert(info && strcmp(info->name, "EditorBuffer") == 0);
    assert(info->representation == MONAD_COMPILER_REP_FOREIGN);
    assert(info->ownership == MONAD_COMPILER_OWNERSHIP_CONSUMED);
    assert(info->canonical_type_id != 0);
    assert(monad_foreign_type_identity_equal(
        info->nominal_identity,
        monad_binding_snapshot_at(snapshot, 0)->foreign_type_identity));

    monad_binding_snapshot_release(snapshot);
    assert(monad_foreign_type_release(type, &error) == MONAD_OK);
    descriptor.ownership = MONAD_FOREIGN_SHARED;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &type, &error) == MONAD_OK);
    assert(monad_runtime_snapshot_bindings(
        thread, &filter, &snapshot, &error) == MONAD_OK);
    assert(monad_compiler_import_foreign_types(
        compiler, snapshot, &error) == MONAD_OK);
    info = monad_compiler_foreign_type_at(compiler, 0);
    assert(info->ownership == MONAD_COMPILER_OWNERSHIP_SHARED);

    monad_compiler_session_destroy(compiler);
    monad_binding_snapshot_release(snapshot);
    assert(monad_foreign_type_release(type, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("transactional compiler foreign import ok");
    return 0;
}
