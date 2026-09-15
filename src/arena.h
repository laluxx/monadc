#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

///  Arena allocator
//
//  A fast bump-pointer allocator organized as a singly-linked list of fixed-
//  size blocks.  Allocation is O(1).  Freeing the entire arena is O(blocks),
//  which is O(1) amortized per allocation.
//
//  Usage patterns:
//
//    // Scratch arena — reset after each use
//    Arena scratch;
//    arena_init(&scratch, 64 * 1024);
//    Foo *f = arena_alloc(&scratch, sizeof(Foo));
//    arena_reset(&scratch);          // reuse memory, keep first block
//    arena_free(&scratch);           // actually release OS memory
//
//    // Permanent arena — free everything at the end
//    Arena perm;
//    arena_init(&perm, 1024 * 1024);
//    char *s = arena_strdup(&perm, "hello");
//    arena_free(&perm);
//
//    // Checkpoints — restore to a prior state
//    ArenaCheckpoint cp = arena_checkpoint(&scratch);
//    ... allocate temporaries ...
//    arena_restore(&scratch, cp);
//

// Default block size: 4 MiB.  Override before including if needed.
#ifndef ARENA_DEFAULT_BLOCK
#define ARENA_DEFAULT_BLOCK (4u * 1024u * 1024u)
#endif

// Minimum alignment for all allocations (matches max scalar alignment).
#define ARENA_ALIGN 8

typedef struct ArenaBlock {
    char             *base;     // first byte of usable memory
    size_t            used;     // bytes consumed so far
    size_t            size;     // total usable bytes in this block
    struct ArenaBlock *next;    // older block (NULL = oldest)
} ArenaBlock;

typedef struct {
    ArenaBlock *current;         // newest (active) block
    size_t      block_size;      // default size for new blocks
    size_t      total_allocated; // running total for diagnostics
    size_t      total_used;      // running used bytes for diagnostics
} Arena;

// Saved position inside an arena — for cheap stack-like rollback.
typedef struct {
    ArenaBlock *block;
    size_t      used;
} ArenaCheckpoint;

///  Lifecycle

// Initialise arena.  block_size=0 uses ARENA_DEFAULT_BLOCK.
void  arena_init(Arena *a, size_t block_size);

// Free ALL memory owned by the arena.  The Arena struct itself is not freed.
// After this call arena_init may be used again.
void  arena_free(Arena *a);

// Reset the arena: all blocks are rewound to empty but OS memory is kept.
// The arena is immediately reusable with zero mallocs in the common case.
void  arena_reset(Arena *a);

///  Allocation

// Allocate `size` bytes, aligned to ARENA_ALIGN.  Never returns NULL —
// calls abort() on OOM (same contract as xmalloc in most compilers).
void *arena_alloc(Arena *a, size_t size);

// Allocate `size` bytes initialised to zero.
void *arena_calloc(Arena *a, size_t size);

// Allocate a copy of `src` of `size` bytes.
void *arena_memdup(Arena *a, const void *src, size_t size);

// Duplicate a NUL-terminated string into the arena.
char *arena_strdup(Arena *a, const char *s);

// Duplicate at most `n` bytes of a string into the arena (always NUL-terminates).
char *arena_strndup(Arena *a, const char *s, size_t n);

// printf into the arena — returns pointer to NUL-terminated result.
char *arena_sprintf(Arena *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

///  Checkpoints

// Save the current allocation position.
ArenaCheckpoint arena_checkpoint(Arena *a);

// Restore the arena to a previously saved checkpoint.
// All allocations made after the checkpoint are invalidated.
// Only safe when no blocks were freed between save and restore.
void arena_restore(Arena *a, ArenaCheckpoint cp);

///  Diagnostics

// Print a one-line summary of arena state to stderr.
void arena_print_stats(const Arena *a, const char *label);

#endif // ARENA_H
