/*
:TEST-ID tests.embedding.diagnostics.inference-phase
:TEST-CONTEXT dec.embedding.unit-owned-inference
:TEST-PURPOSE Prove type rejection crosses the stable C API as immutable data.
:TEST-ATOM atom.embedding.28.diagnostics.inference-phase
:TEST-EXPECT heterogeneous collection yields MONAD-C0003 in type phase
:TEST-MENU-PATH embedding/branches/diagnostics/atom.embedding.28.diagnostics.inference-phase
*/
#include <monad/compiler.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    monad_compiler_session_t *session = NULL;
    monad_error_t error = {0};
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);

    const char text[] = "[1 \"two\"]\n";
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "heterogeneous.mon";
    descriptor.bytes = text;
    descriptor.byte_count = strlen(text);
    descriptor.version = 28;
    monad_compiler_source_t *source = NULL;
    assert(monad_compiler_source_create(&descriptor, &source, &error) == MONAD_OK);

    monad_compiler_diagnostic_set_t *diagnostics = NULL;
    assert(monad_compiler_check_source(
        session, source, &diagnostics, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 1);
    const monad_compiler_diagnostic_t *item =
        monad_compiler_diagnostic_at(diagnostics, 0);
    assert(item && item->phase == MONAD_COMPILER_PHASE_TYPE);
    assert(strcmp(item->code, "MONAD-C0003") == 0);
    assert(item->source_version == 28);
    assert(item->message && item->message[0]);

    monad_compiler_source_destroy(source);
    assert(strcmp(item->source_name, "heterogeneous.mon") == 0);
    monad_compiler_diagnostic_set_destroy(diagnostics);
    monad_compiler_session_destroy(session);
    puts("type rejection is immutable structured data");
    return 0;
}
