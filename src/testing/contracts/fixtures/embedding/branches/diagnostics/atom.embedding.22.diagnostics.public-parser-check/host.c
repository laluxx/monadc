/*
:TEST-ID tests.embedding.diagnostics.public-parser-check
:TEST-CONTEXT dec.embedding.public-parser-diagnostics
:TEST-PURPOSE Prove the installed SDK returns immutable real-parser diagnostics silently.
:TEST-ATOM atom.embedding.22.diagnostics.public-parser-check
:TEST-EXPECT 32 parse rejections then one valid empty result through public API
:TEST-MENU-PATH embedding/branches/diagnostics/atom.embedding.22.diagnostics.public-parser-check
*/
#include <monad/compiler.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static monad_compiler_source_t *make_source(
    const char *name, const char *bytes, uint64_t version) {
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_t *source = NULL;
    monad_error_t error;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = name;
    descriptor.bytes = bytes;
    descriptor.byte_count = strlen(bytes);
    descriptor.version = version;
    assert(monad_compiler_source_create(&descriptor, &source, &error) == MONAD_OK);
    return source;
}

static void *concurrent_checks(void *opaque) {
    const char *name = opaque;
    monad_compiler_session_t *session = NULL;
    monad_error_t error;
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);
    for (uint64_t version = 1; version <= 16; version++) {
        monad_compiler_source_t *source = make_source(
            name, "define broken :: Int -> Int\n  value -> [value\n", version);
        monad_compiler_diagnostic_set_t *set = NULL;
        assert(monad_compiler_check_source(session, source, &set, &error) == MONAD_OK);
        assert(monad_compiler_diagnostic_count(set) == 1);
        assert(monad_compiler_diagnostic_at(set, 0)->phase ==
               MONAD_COMPILER_PHASE_PARSE);
        monad_compiler_diagnostic_set_destroy(set);
        monad_compiler_source_destroy(source);
    }
    monad_compiler_session_destroy(session);
    return NULL;
}

int main(void) {
    monad_compiler_session_t *session = NULL;
    monad_error_t error;
    assert(monad_compiler_session_create(&session, &error) == MONAD_OK);

    const char *bad_text = "define broken :: Int -> Int\n  value -> [value\n";
    for (uint64_t version = 1; version <= 32; version++) {
        monad_compiler_source_t *source =
            make_source("editor://live.mon", bad_text, version);
        monad_compiler_diagnostic_set_t *set = NULL;
        assert(monad_compiler_check_source(session, source, &set, &error) == MONAD_OK);
        assert(monad_compiler_diagnostic_count(set) == 1);
        const monad_compiler_diagnostic_t *item =
            monad_compiler_diagnostic_at(set, 0);
        assert(item && item->phase == MONAD_COMPILER_PHASE_PARSE);
        assert(strcmp(item->code, "MONAD-C0002") == 0);
        assert(item->source_version == version);
        assert(item->line >= 1 && item->column >= 1);
        assert(item->byte_end >= item->byte_start);
        monad_compiler_source_destroy(source);
        assert(strcmp(item->source_name, "editor://live.mon") == 0);
        assert(item->message && item->message[0]);
        monad_compiler_diagnostic_set_destroy(set);
    }

    monad_compiler_source_t *good = make_source(
        "editor://live.mon",
        "define identity :: Int -> Int\n  value -> value\n", 33);
    monad_compiler_diagnostic_set_t *set = NULL;
    assert(monad_compiler_check_source(session, good, &set, &error) == MONAD_OK);
    assert(monad_compiler_diagnostic_count(set) == 0);
    monad_compiler_diagnostic_set_destroy(set);
    monad_compiler_source_destroy(good);
    monad_compiler_session_destroy(session);

    pthread_t first, second;
    assert(pthread_create(&first, NULL, concurrent_checks, "thread://one.mon") == 0);
    assert(pthread_create(&second, NULL, concurrent_checks, "thread://two.mon") == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    puts("public parser diagnostics are immutable and silent");
    return 0;
}
