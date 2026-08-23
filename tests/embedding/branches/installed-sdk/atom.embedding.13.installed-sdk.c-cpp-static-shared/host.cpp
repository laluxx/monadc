/*
:TEST-ID tests.embedding.installed-sdk.c-cpp-static-shared
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.13.installed-sdk.c-cpp-static-shared
:TEST-QUALITY installed-integration
:TEST-SUBSET installed-sdk
:TEST-CATEGORY 13-c-cpp-static-shared
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.embedding.compiler-library-boundary
:TEST-PURPOSE The installed stable runtime ABI is usable from C++ without repository-private headers.
:TEST-ATOM installed headers + installed library -> create runtime -> destroy runtime
:TEST-EXPECT compile, link-static, link-shared, run
:TEST-COVERAGE installed embed.h C++ linkage and libmonad-embed archives
:TEST-USES-ATOM atom.embedding.category.13.c-cpp-static-shared
:TEST-MENU branches/installed-sdk/atom.embedding.13.installed-sdk.c-cpp-static-shared
:TEST-MENU-PATH embedding/branches/installed-sdk/atom.embedding.13.installed-sdk.c-cpp-static-shared
:TEST-ATOM-LEAF atom.embedding.13.installed-sdk.c-cpp-static-shared
:TEST-CLI-TARGET embedding branches installed-sdk
*/
#include <monad/embed.h>

#include <cassert>
#include <cstdio>

int main() {
    monad_runtime_config_t config{};
    monad_runtime_t *runtime = nullptr;
    monad_error_t error{};
    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    std::puts("installed embedding C++ ok");
}
