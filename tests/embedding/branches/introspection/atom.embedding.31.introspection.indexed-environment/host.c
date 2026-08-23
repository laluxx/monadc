/*
:TEST-ID tests.embedding.introspection.indexed-environment
:TEST-CONTEXT dec.embedding.indexed-environment-queries
:TEST-PURPOSE Prove editor queries expose owned docs, schemes, and full ranges.
:TEST-ATOM atom.embedding.31.introspection.indexed-environment
:TEST-EXPECT indexed exact lookup and source-ordered filtering survive owner teardown
:TEST-MENU-PATH embedding/branches/introspection/atom.embedding.31.introspection.indexed-environment
*/
#include <monad/compiler.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char text[] =
        "define identity :: Int -> Int\n"
        "  \"Return its argument unchanged.\"\n"
        "  value -> value\n"
        "define [answer :: Int] 42\n";
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "editor://queries.mon";
    descriptor.bytes = text;
    descriptor.byte_count = strlen(text);
    descriptor.version = 31;
    monad_error_t error = {0};
    monad_compiler_source_t *source = NULL;
    monad_compiler_session_t *session = NULL;
    monad_compiler_diagnostic_set_t *diagnostics = NULL;
    monad_compiler_environment_t *environment = NULL;
    assert(monad_compiler_source_create(
        &descriptor, &source, &error) == MONAD_OK);
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);
    assert(monad_compiler_analyze_source(
        session, source, &diagnostics, &environment, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 0);

    const monad_compiler_definition_t *identity =
        monad_compiler_environment_find_name(environment, "identity");
    assert(identity);
    assert(monad_compiler_environment_find_id(environment, identity->id) == identity);
    assert(!monad_compiler_environment_find_name(environment, "missing"));
    assert(strcmp(identity->docstring, "Return its argument unchanged.") == 0);
    assert(strstr(text + identity->byte_start, "value -> value"));

    const monad_compiler_definition_t *answer =
        monad_compiler_environment_find_name(environment, "answer");
    assert(answer && answer->kind == MONAD_COMPILER_DEFINITION_VALUE);
    assert(identity->byte_end <= answer->byte_start);
    assert(answer->byte_end == strlen(text));
    assert(answer->docstring == NULL);

    monad_compiler_definition_filter_t filter;
    monad_compiler_definition_filter_init(&filter);
    filter.kind = MONAD_COMPILER_DEFINITION_FUNCTION;
    filter.name_prefix = "i";
    filter.principal_scheme = identity->principal_scheme;
    assert(monad_compiler_environment_matching_count(environment, &filter) == 1);
    assert(monad_compiler_environment_matching_at(environment, &filter, 0) == identity);
    assert(monad_compiler_environment_matching_at(environment, &filter, 1) == NULL);

    monad_compiler_source_destroy(source);
    monad_compiler_session_destroy(session);
    monad_compiler_diagnostic_set_destroy(diagnostics);
    assert(strcmp(identity->docstring, "Return its argument unchanged.") == 0);
    assert(monad_compiler_environment_find_id(environment, answer->id) == answer);
    monad_compiler_environment_destroy(environment);
    puts("indexed environment queries preserve owned metadata");
    return 0;
}
