/*
:TEST-ID tests.embedding.introspection.environment-snapshot
:TEST-CONTEXT dec.embedding.immutable-environment-snapshot
:TEST-PURPOSE Prove committed definitions cross the installed ABI as owned data.
:TEST-ATOM atom.embedding.30.introspection.environment-snapshot
:TEST-EXPECT source order, principal schemes, stable IDs, and independent lifetime
:TEST-MENU-PATH embedding/branches/introspection/atom.embedding.30.introspection.environment-snapshot
*/
#include <monad/compiler.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static monad_compiler_source_t *source(uint64_t version) {
    static const char text[] =
        "define identity :: Int -> Int\n"
        "  value -> value\n"
        "define twice :: Int -> Int\n"
        "  value -> identity (identity value)\n";
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "editor://math.mon";
    descriptor.bytes = text;
    descriptor.byte_count = strlen(text);
    descriptor.version = version;
    monad_compiler_source_t *result = NULL;
    monad_error_t error = {0};
    assert(monad_compiler_source_create(&descriptor, &result, &error) == MONAD_OK);
    return result;
}

static monad_compiler_environment_t *analyze(
    monad_compiler_session_t *session, monad_compiler_source_t *input) {
    monad_compiler_diagnostic_set_t *diagnostics = NULL;
    monad_compiler_environment_t *environment = NULL;
    monad_error_t error = {0};
    assert(monad_compiler_analyze_source(
        session, input, &diagnostics, &environment, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 0);
    assert(environment);
    monad_compiler_diagnostic_set_destroy(diagnostics);
    return environment;
}

int main(void) {
    monad_compiler_session_t *session = NULL;
    monad_error_t error = {0};
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);
    monad_compiler_source_t *first_source = source(30);
    monad_compiler_environment_t *first = analyze(session, first_source);
    monad_compiler_source_t *second_source = source(31);
    monad_compiler_environment_t *second = analyze(session, second_source);

    assert(monad_compiler_environment_count(first) == 2);
    const monad_compiler_definition_t *identity =
        monad_compiler_environment_at(first, 0);
    const monad_compiler_definition_t *twice =
        monad_compiler_environment_at(first, 1);
    assert(identity && twice);
    assert(strcmp(identity->name, "identity") == 0);
    assert(strcmp(twice->name, "twice") == 0);
    assert(identity->kind == MONAD_COMPILER_DEFINITION_FUNCTION);
    assert(identity->principal_scheme[0]);
    assert(identity->line == 1 && twice->line == 3);
    assert(identity->byte_end > identity->byte_start);
    assert(identity->source_version == 30);
    assert(identity->id == monad_compiler_environment_at(second, 0)->id);

    monad_compiler_source_destroy(first_source);
    monad_compiler_source_destroy(second_source);
    monad_compiler_session_destroy(session);
    assert(strcmp(twice->source_name, "editor://math.mon") == 0);
    assert(strcmp(twice->name, "twice") == 0);
    assert(twice->principal_scheme[0]);
    monad_compiler_environment_destroy(second);
    monad_compiler_environment_destroy(first);
    puts("committed environment snapshot is immutable");
    return 0;
}
