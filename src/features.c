#include "features.h"
#include "reader.h"
#include <string.h>
#include <stdlib.h>

// Internal feature tracking for has_feature()
static MONAD_THREAD_LOCAL char **detected_features = NULL;
static MONAD_THREAD_LOCAL int feature_count = 0;

struct FeaturePersistentState { char **items; int count; };
static MONAD_THREAD_LOCAL FeaturePersistentState *g_bound_feature_state;
static MONAD_THREAD_LOCAL FeaturePersistentState g_saved_feature_state;

FeaturePersistentState *feature_persistent_state_create(void) {
    return calloc(1, sizeof(FeaturePersistentState));
}

void feature_persistent_state_destroy(FeaturePersistentState *state) {
    if (!state || state == g_bound_feature_state) return;
    for (int i = 0; i < state->count; i++) free(state->items[i]);
    free(state->items);
    free(state);
}

bool feature_persistent_state_enter(FeaturePersistentState *state) {
    if (!state || g_bound_feature_state) return false;
    g_saved_feature_state = (FeaturePersistentState){detected_features, feature_count};
    detected_features = state->items; feature_count = state->count;
    state->items = NULL; state->count = 0;
    g_bound_feature_state = state;
    return true;
}

void feature_persistent_state_leave(FeaturePersistentState *state) {
    if (!state || g_bound_feature_state != state) return;
    state->items = detected_features; state->count = feature_count;
    detected_features = g_saved_feature_state.items;
    feature_count = g_saved_feature_state.count;
    memset(&g_saved_feature_state, 0, sizeof(g_saved_feature_state));
    g_bound_feature_state = NULL;
}

static void add_feature_internal(const char *feature) {
    detected_features = realloc(detected_features, sizeof(char*) * (feature_count + 1));
    detected_features[feature_count++] = strdup(feature);
}

bool feature_register(const char *feature) {
    if (!feature || !feature[0]) return false;
    if (has_feature(feature)) return true;
    char *copy = strdup(feature);
    if (!copy) return false;
    char **items = realloc(detected_features, sizeof(char*) * (feature_count + 1));
    if (!items) { free(copy); return false; }
    detected_features = items;
    detected_features[feature_count++] = copy;
    return true;
}

AST *detect_features(void) {
    // Create a new list to hold all feature keywords
    AST *features_list = ast_new_list();

    // Free any previously detected features
    for (int i = 0; i < feature_count; i++) {
        free(detected_features[i]);
    }
    free(detected_features);
    detected_features = NULL;
    feature_count = 0;

    // Helper macro to add both to AST list and internal tracking
    #define ADD_FEATURE(name) do { \
        ast_list_append(features_list, ast_new_keyword(name)); \
        add_feature_internal(name); \
    } while(0)

//// Operating System Detection

    #ifdef _WIN32
        ADD_FEATURE("WINDOWS");
        #ifdef _WIN64
            ADD_FEATURE("WIN64");
        #else
            ADD_FEATURE("WIN32");
        #endif
    #endif

    #ifdef __linux__
        ADD_FEATURE("LINUX");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __APPLE__
        ADD_FEATURE("MACOS");
        ADD_FEATURE("DARWIN");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __FreeBSD__
        ADD_FEATURE("BSD");
        ADD_FEATURE("FREEBSD");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __OpenBSD__
        ADD_FEATURE("BSD");
        ADD_FEATURE("OPENBSD");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __NetBSD__
        ADD_FEATURE("BSD");
        ADD_FEATURE("NETBSD");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __DragonFly__
        ADD_FEATURE("BSD");
        ADD_FEATURE("DRAGONFLY");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __sun
        ADD_FEATURE("SOLARIS");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __CYGWIN__
        ADD_FEATURE("CYGWIN");
        ADD_FEATURE("UNIX");
    #endif

    #ifdef __MINGW32__
        ADD_FEATURE("MINGW");
        ADD_FEATURE("WINDOWS");
    #endif

//// Architecture Detection

    // x86/AMD64
    #if defined(__x86_64__) || defined(_M_X64)
        ADD_FEATURE("X86-64");
        ADD_FEATURE("64BIT");
    #elif defined(__i386__) || defined(_M_IX86)
        ADD_FEATURE("X86-32");
        ADD_FEATURE("32BIT");
    #endif

    // ARM
    #if defined(__aarch64__) || defined(_M_ARM64)
        ADD_FEATURE("ARM");
        ADD_FEATURE("ARM64");
        ADD_FEATURE("64BIT");
    #elif defined(__arm__) || defined(_M_ARM)
        ADD_FEATURE("ARM");
        ADD_FEATURE("ARM32");
        ADD_FEATURE("32BIT");
    #endif

    // PowerPC
    #ifdef __powerpc64__
        ADD_FEATURE("PPC64");
        ADD_FEATURE("POWERPC");
        ADD_FEATURE("64BIT");
    #elif defined(__powerpc__)
        ADD_FEATURE("PPC");
        ADD_FEATURE("POWERPC");
        ADD_FEATURE("32BIT");
    #endif

    // RISC-V
    #ifdef __riscv
        ADD_FEATURE("RISCV");
        #if __riscv_xlen == 64
            ADD_FEATURE("RISCV64");
            ADD_FEATURE("64BIT");
        #elif __riscv_xlen == 32
            ADD_FEATURE("RISCV32");
            ADD_FEATURE("32BIT");
        #endif
    #endif

    // MIPS
    #ifdef __mips__
        ADD_FEATURE("MIPS");
        #ifdef __mips64
            ADD_FEATURE("MIPS64");
            ADD_FEATURE("64BIT");
        #else
            ADD_FEATURE("MIPS32");
            ADD_FEATURE("32BIT");
        #endif
    #endif

    // SPARC
    #ifdef __sparc__
        ADD_FEATURE("SPARC");
        #ifdef __sparc64__
            ADD_FEATURE("SPARC64");
            ADD_FEATURE("64BIT");
        #else
            ADD_FEATURE("SPARC32");
            ADD_FEATURE("32BIT");
        #endif
    #endif

    // Alpha
    #ifdef __alpha__
        ADD_FEATURE("ALPHA");
        ADD_FEATURE("64BIT");
    #endif

//// Endianness Detection

    #if defined(__BYTE_ORDER__)
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            ADD_FEATURE("LITTLE-ENDIAN");
        #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            ADD_FEATURE("BIG-ENDIAN");
        #endif
    #elif defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN)
        ADD_FEATURE("LITTLE-ENDIAN");
    #elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN)
        ADD_FEATURE("BIG-ENDIAN");
    #endif

//// Binary Format Detection

    #ifdef _WIN32
        ADD_FEATURE("PE");       // Portable Executable (Windows)
    #endif

    #if defined(__linux__)  || defined(__FreeBSD__) || defined(__OpenBSD__) || \
        defined(__NetBSD__) || defined(__DragonFly__)
        ADD_FEATURE("ELF");      // Executable and Linkable Format (Unix/Linux)
    #endif

    #ifdef __APPLE__
        ADD_FEATURE("MACH-O");   // Mach Object (macOS)
    #endif

//// Additional CPU Features (if available)

    #ifdef __SSE__
        ADD_FEATURE("SSE");
    #endif

    #ifdef __SSE2__
        ADD_FEATURE("SSE2");
    #endif

    #ifdef __SSE3__
        ADD_FEATURE("SSE3");
    #endif

    #ifdef __AVX__
        ADD_FEATURE("AVX");
    #endif

    #ifdef __AVX2__
        ADD_FEATURE("AVX2");
    #endif

    #ifdef __FMA__
        ADD_FEATURE("FMA");
    #endif

    #ifdef __ARM_NEON
        ADD_FEATURE("NEON");
    #endif

//// Build Type Detection

    #ifdef NDEBUG
        ADD_FEATURE("RELEASE");
    #else
        ADD_FEATURE("DEBUG");
    #endif

    #ifdef _REENTRANT
        ADD_FEATURE("THREAD-SAFE");
    #endif

    #undef ADD_FEATURE

    return features_list;
}

int has_feature(const char *feature_name) {
    // Strip leading ':' if present
    const char *name = feature_name;
    if (name[0] == ':') {
        name++;
    }

    for (int i = 0; i < feature_count; i++) {
        if (strcmp(detected_features[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}
