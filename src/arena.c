#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

///  Internal helpers

static inline size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static ArenaBlock *block_new(size_t min_size) {
    size_t sz = min_size > ARENA_DEFAULT_BLOCK ? min_size : ARENA_DEFAULT_BLOCK;
    // Allocate the header and the usable memory in one shot.
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + sz);
    if (!b) {
        fprintf(stderr, "arena: out of memory requesting %zu bytes\n", sz);
        abort();
    }
    b->base = (char *)(b + 1);
    b->used = 0;
    b->size = sz;
    b->next = NULL;
    return b;
}

///  Lifecycle

void arena_init(Arena *a, size_t block_size) {
    a->block_size      = block_size ? block_size : ARENA_DEFAULT_BLOCK;
    a->current         = NULL;
    a->total_allocated = 0;
    a->total_used      = 0;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->current;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->current         = NULL;
    a->total_allocated = 0;
    a->total_used      = 0;
}

void arena_reset(Arena *a) {
    if (!a->current) return;

    // Walk to the oldest (largest) block — keep it, free the rest.
    // This means after a reset we have exactly one block ready to reuse,
    // which is the right trade-off for scratch arenas that grow then shrink.
    ArenaBlock *oldest = a->current;
    while (oldest->next) {
        ArenaBlock *prev = oldest->next;
        oldest->next = prev->next;
        free(oldest);
        oldest = prev;
    }
    // oldest is now the sole surviving block — rewind it
    a->current            = oldest;
    a->current->used      = 0;
    a->current->next      = NULL;
    a->total_used         = 0;
    // total_allocated reflects capacity, leave it
}

///  Allocation

void *arena_alloc(Arena *a, size_t size) {
    if (size == 0) size = 1;
    size = align_up(size, ARENA_ALIGN);

    // Fast path: bump pointer in current block.
    if (a->current && a->current->used + size <= a->current->size) {
        void *p = a->current->base + a->current->used;
        a->current->used  += size;
        a->total_used     += size;
        return p;
    }

    // Slow path: allocate a new block.
    ArenaBlock *b = block_new(size);
    b->next           = a->current;
    a->current        = b;
    a->total_allocated += b->size;

    void *p = b->base;
    b->used       = size;
    a->total_used += size;
    return p;
}

void *arena_calloc(Arena *a, size_t size) {
    void *p = arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

void *arena_memdup(Arena *a, const void *src, size_t size) {
    void *p = arena_alloc(a, size);
    memcpy(p, src, size);
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char  *p   = arena_alloc(a, len + 1);
    memcpy(p, s, len + 1);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char  *p   = arena_alloc(a, len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *arena_sprintf(Arena *a, const char *fmt, ...) {
    // Two-pass: measure then allocate.
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) return arena_strdup(a, "");

    char *buf = arena_alloc(a, (size_t)needed + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    return buf;
}

///  Checkpoints

ArenaCheckpoint arena_checkpoint(Arena *a) {
    ArenaCheckpoint cp;
    cp.block = a->current;
    cp.used  = a->current ? a->current->used : 0;
    return cp;
}

void arena_restore(Arena *a, ArenaCheckpoint cp) {
    // Free any blocks allocated after the checkpoint.
    while (a->current && a->current != cp.block) {
        ArenaBlock *b = a->current;
        a->current = b->next;
        a->total_allocated -= b->size;
        free(b);
    }
    // Rewind the checkpointed block.
    if (a->current) {
        size_t rewound    = a->current->used - cp.used;
        a->current->used  = cp.used;
        a->total_used    -= rewound;
    }
}

///  Diagnostics

void arena_print_stats(const Arena *a, const char *label) {
    size_t blocks = 0;
    ArenaBlock *b = a->current;
    while (b) { blocks++; b = b->next; }
    fprintf(stderr,
            "[arena] %s: %zu block(s), %zu KiB allocated, %zu KiB used (%.1f%%)\n",
            label ? label : "?",
            blocks,
            a->total_allocated / 1024,
            a->total_used      / 1024,
            a->total_allocated
                ? 100.0 * (double)a->total_used / (double)a->total_allocated
                : 0.0);
}
