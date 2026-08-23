/*
:TEST-ID tests.embedding.diagnostics.immutable-input-check
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.16.diagnostics.immutable-input-check
:TEST-QUALITY stable-abi
:TEST-SUBSET diagnostics
:TEST-CATEGORY 16-immutable-input-check
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.immutable-source-unit,monadc.context.embedding.compiler-library-boundary
:TEST-PURPOSE Source checking returns immutable versioned diagnostics without printing or terminating.
:TEST-ATOM invalid UTF-8 source -> one stable ranged diagnostic -> source destruction leaves result valid
:TEST-EXPECT compile, run, exact-stdout
:TEST-COVERAGE compiler.h and compiler.c diagnostic set ownership, UTF-8 validation, session affinity
:TEST-USES-ATOM atom.embedding.category.16.immutable-input-check
:TEST-MENU branches/diagnostics/atom.embedding.16.diagnostics.immutable-input-check
:TEST-MENU-PATH embedding/branches/diagnostics/atom.embedding.16.diagnostics.immutable-input-check
:TEST-ATOM-LEAF atom.embedding.16.diagnostics.immutable-input-check
:TEST-CLI-TARGET embedding branches diagnostics
*/
#include <monad/compiler.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static monad_compiler_source_t *source(const char *name, const char *bytes,
                                       size_t count, uint64_t version) {
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_t *result = NULL;
    monad_error_t error;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = name;
    descriptor.bytes = bytes;
    descriptor.byte_count = count;
    descriptor.version = version;
    assert(monad_compiler_source_create(&descriptor, &result, &error) == MONAD_OK);
    return result;
}

int main(void) {
    monad_compiler_session_t *session;
    monad_compiler_diagnostic_set_t *diagnostics;
    monad_error_t error;
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);

    const char invalid[] = {'d','e','f','\n',(char)0xc3,'(','\n'};
    monad_compiler_source_t *bad = source("editor://bad.mon", invalid,
                                          sizeof(invalid), 91);
    assert(monad_compiler_check_source(session, bad, &diagnostics, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 1);
    const monad_compiler_diagnostic_t *item =
        monad_compiler_diagnostic_at(diagnostics, 0);
    assert(item && item->severity == MONAD_COMPILER_DIAGNOSTIC_ERROR);
    assert(item->phase == MONAD_COMPILER_PHASE_INPUT);
    assert(strcmp(item->code, "MONAD-C0001") == 0);
    assert(strcmp(item->source_name, "editor://bad.mon") == 0);
    assert(item->source_version == 91);
    assert(item->byte_start == 4 && item->byte_end == 5);
    assert(item->line == 2 && item->column == 1);
    assert(strstr(item->message, "UTF-8") != NULL);
    monad_compiler_source_destroy(bad);
    assert(strcmp(item->source_name, "editor://bad.mon") == 0);
    assert(item->source_version == 91);
    monad_compiler_diagnostic_set_destroy(diagnostics);

    const char overlong[] = {(char)0xe0, (char)0x80, (char)0x80};
    const char surrogate[] = {(char)0xed, (char)0xa0, (char)0x80};
    const char too_high[] = {(char)0xf4, (char)0x90, (char)0x80, (char)0x80};
    const char truncated[] = {(char)0xf0, (char)0x90};
    struct invalid_case { const char *bytes; size_t count; } cases[] = {
        {overlong, sizeof(overlong)}, {surrogate, sizeof(surrogate)},
        {too_high, sizeof(too_high)}, {truncated, sizeof(truncated)},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bad = source("unicode.mon", cases[i].bytes, cases[i].count, 91);
        assert(monad_compiler_check_source(session, bad, &diagnostics, &error) == MONAD_OK);
        assert(monad_compiler_diagnostic_count(diagnostics) == 1);
        monad_compiler_diagnostic_set_destroy(diagnostics);
        monad_compiler_source_destroy(bad);
    }

    const char scalar_max[] = {(char)0xf4, (char)0x8f, (char)0xbf, (char)0xbf};
    monad_compiler_source_t *unicode = source(
        "unicode.mon", scalar_max, sizeof(scalar_max), 92);
    assert(monad_compiler_check_source(session, unicode, &diagnostics, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 0);
    monad_compiler_diagnostic_set_destroy(diagnostics);
    monad_compiler_source_destroy(unicode);

    monad_compiler_source_t *good = source("good.mon", "define id x -> x",
                                            strlen("define id x -> x"), 92);
    assert(monad_compiler_check_source(session, good, &diagnostics, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(diagnostics) == 0);
    assert(monad_compiler_diagnostic_at(diagnostics, 0) == NULL);
    monad_compiler_diagnostic_set_destroy(diagnostics);
    monad_compiler_source_destroy(good);
    monad_compiler_session_destroy(session);
    puts("immutable structured diagnostics ok");
    return 0;
}
