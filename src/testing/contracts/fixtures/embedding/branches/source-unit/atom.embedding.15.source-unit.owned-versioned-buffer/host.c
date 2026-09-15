/*
:TEST-ID tests.embedding.source-unit.owned-versioned-buffer
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.15.source-unit.owned-versioned-buffer
:TEST-QUALITY stable-abi
:TEST-SUBSET source-unit
:TEST-CATEGORY 15-owned-versioned-buffer
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.compiler-library-boundary
:TEST-PURPOSE In-memory source is immutable, length-delimited, versioned, and owned by an opaque compiler object.
:TEST-ATOM non-terminated caller bytes -> owned source snapshot -> caller mutation cannot alter snapshot
:TEST-EXPECT compile, run
:TEST-COVERAGE compiler.h and compiler.c source-unit ABI, validation, ownership, accessors
:TEST-USES-ATOM atom.embedding.category.15.owned-versioned-buffer
:TEST-MENU branches/source-unit/atom.embedding.15.source-unit.owned-versioned-buffer
:TEST-MENU-PATH embedding/branches/source-unit/atom.embedding.15.source-unit.owned-versioned-buffer
:TEST-ATOM-LEAF atom.embedding.15.source-unit.owned-versioned-buffer
:TEST-CLI-TARGET embedding branches source-unit
*/
#include <monad/compiler.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char name[] = {'e','d','i','t','o','r',':','/','/','b','u','f','f','e','r','.','m','o','n','\0'};
    char bytes[] = {'d','e','f','i','n','e',' ','f',' ','x',' ','-','>',' ','x'};
    const char expected[] = "define f x -> x";
    monad_compiler_source_descriptor_t descriptor;
    monad_compiler_source_t *source = NULL;
    monad_error_t error;
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = name;
    descriptor.bytes = bytes;
    descriptor.byte_count = sizeof(bytes);
    descriptor.version = 73;
    assert(monad_compiler_source_create(&descriptor, &source, &error) == MONAD_OK);
    memset(name, 'x', sizeof(name) - 1);
    memset(bytes, 'x', sizeof(bytes));
    assert(strcmp(monad_compiler_source_name(source), "editor://buffer.mon") == 0);
    assert(monad_compiler_source_version(source) == 73);
    assert(monad_compiler_source_byte_count(source) == strlen(expected));
    assert(memcmp(monad_compiler_source_bytes(source), expected, strlen(expected)) == 0);
    assert(monad_compiler_source_bytes(source)[strlen(expected)] == '\0');
    monad_compiler_source_destroy(source);

    char invalid[] = {'x', '\0', 'y'};
    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "invalid.mon";
    descriptor.bytes = invalid;
    descriptor.byte_count = sizeof(invalid);
    assert(monad_compiler_source_create(&descriptor, &source, &error) ==
           MONAD_ERROR_INVALID_ARGUMENT);
    assert(source == NULL && error.message != NULL);

    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "empty.mon";
    descriptor.version = 74;
    assert(monad_compiler_source_create(&descriptor, &source, &error) == MONAD_OK);
    assert(monad_compiler_source_byte_count(source) == 0);
    assert(monad_compiler_source_bytes(source)[0] == '\0');
    monad_compiler_source_destroy(source);

    monad_compiler_source_descriptor_init(&descriptor);
    descriptor.name = "future.mon";
    descriptor.abi_version++;
    assert(monad_compiler_source_create(&descriptor, &source, &error) ==
           MONAD_ERROR_ABI_MISMATCH);
    assert(source == NULL && error.status == MONAD_ERROR_ABI_MISMATCH);
    puts("owned source unit ok");
    return 0;
}
