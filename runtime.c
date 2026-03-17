#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "runtime.h"

static inline ConsCell     *heap_cons_cell(void)    { return malloc(sizeof(ConsCell));     }
static inline RuntimeList  *heap_list_wrapper(void) { return malloc(sizeof(RuntimeList));  }
static inline RuntimeValue *heap_value(void)        { return malloc(sizeof(RuntimeValue)); }


volatile int rt_interrupted = 0;

//  Global evaluation arena
//
//  All hot-path allocations (thunks, list cells, env structs, int/float/char
//  RuntimeValues) come from here.  The REPL resets this after every expression
//  so memory is reclaimed in bulk — zero per-object free overhead.
//
//  Initialised by:
//    repl_init()          -> arena_init(&g_eval_arena, 4 * 1024 * 1024)
//    compile_one()        -> arena_init(&g_eval_arena, 4 * 1024 * 1024)
//
//  Reset by:
//    repl_eval_line()     -> arena_reset(&g_eval_arena)  (all 4 call sites)
//    repl_sigint_handler  -> arena_reset(&g_eval_arena)
Arena g_eval_arena;

//  Step 2 — Integer interning cache
//
//  For the extremely common case of small integers (0..INT_CACHE_MAX inclusive)
//  we return a pointer into a static array that is initialised once.
//  rt_value_int() for these values does zero allocations.
#define INT_CACHE_MIN 0
#define INT_CACHE_MAX 65536
#define INT_CACHE_SIZE (INT_CACHE_MAX - INT_CACHE_MIN + 1)

static RuntimeValue  g_int_cache_storage[INT_CACHE_SIZE];
static int           g_int_cache_ready = 0;

static void int_cache_init(void) {
    if (g_int_cache_ready) return;
    for (int i = 0; i < INT_CACHE_SIZE; i++) {
        g_int_cache_storage[i].type         = RT_INT;
        g_int_cache_storage[i].data.int_val = (int64_t)(i + INT_CACHE_MIN);
    }
    g_int_cache_ready = 1;
}

//  Step 3 — Fused ConsCell
//
//  Instead of:
//    RuntimeList  (2 pointers)  ->  malloc
//    RuntimeThunk (head)        ->  malloc
//    RuntimeThunk (tail)        ->  malloc
//    FromEnv / RangeEnv         ->  malloc
//    RuntimeValue (int result)  ->  malloc
//
//  We allocate ONE ConsCell per cons node (two inlined thunks) from the arena.
//  RuntimeList becomes a thin wrapper: just a ConsCell pointer.
//
//  NULL cell  =>  empty list.

// Forward declarations needed by ConsCell forcing helpers
static RuntimeValue *_force_head(ConsCell *c);
static RuntimeValue *_force_tail(ConsCell *c);

//  Empty list singleton — stack allocated, never freed, never in arena.
static RuntimeList _empty_list_val = { NULL };
static RuntimeList *_empty_list    = &_empty_list_val;

///  Internal allocation helpers

static inline ConsCell *alloc_cons_cell(void) {
    return arena_alloc(&g_eval_arena, sizeof(ConsCell));
}

static inline RuntimeList *alloc_list_wrapper(void) {
    return arena_alloc(&g_eval_arena, sizeof(RuntimeList));
}

static inline RuntimeValue *alloc_value(void) {
    return arena_alloc(&g_eval_arena, sizeof(RuntimeValue));
}

/// Closure

RuntimeValue *rt_value_closure(void *fn_ptr, void **env, int env_size, int arity) {
    RuntimeClosure *c = malloc(sizeof(RuntimeClosure));
    c->fn_ptr   = fn_ptr;
    c->env_size = env_size;
    c->arity    = arity;
    if (env_size > 0 && env) {
        c->env = malloc(sizeof(void*) * env_size);
        memcpy(c->env, env, sizeof(void*) * env_size);
    } else {
        c->env = NULL;
    }
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_CLOSURE;
    v->data.closure_val = c;
    return v;
}

RuntimeValue *rt_closure_calln(RuntimeValue *closure, int n, RuntimeValue **args) {
    if (!closure || closure->type != RT_CLOSURE) return rt_value_nil();
    RuntimeClosure *c = closure->data.closure_val;
    typedef RuntimeValue *(*Fn)(void *, int, RuntimeValue **);
    return ((Fn)c->fn_ptr)(c->env, n, args);
}

///  Thunk forcing
// two specialised versions for head and tail (Step 3)

static RuntimeValue *_force_head(ConsCell *c) {
    if (c->head_forced) return c->head_val;
    RuntimeValue *result = c->head_fn(c->head_env);
    // Unwrap nested RT_THUNK wrappers
    while (result && result->type == RT_THUNK) {
        RuntimeThunk *inner = result->data.thunk_val;
        if (inner->forced) { result = inner->value; break; }
        result        = inner->fn(inner->env);
        inner->value  = result;
        inner->forced = 1;
    }
    c->head_val    = result;
    c->head_forced = 1;
    return result;
}

static RuntimeValue *_force_tail(ConsCell *c) {
    if (c->tail_forced) return c->tail_val;
    RuntimeValue *result = c->tail_fn(c->tail_env);
    while (result && result->type == RT_THUNK) {
        RuntimeThunk *inner = result->data.thunk_val;
        if (inner->forced) { result = inner->value; break; }
        result        = inner->fn(inner->env);
        inner->value  = result;
        inner->forced = 1;
    }
    c->tail_val    = result;
    c->tail_forced = 1;
    return result;
}

//  Legacy RuntimeThunk API  (used by rt_force, rt_thunk_of_value, etc.)
//  These are kept for compatibility with generated code that calls rt_force().
RuntimeThunk *rt_thunk_of_value(RuntimeValue *val) {
    RuntimeThunk *t = arena_alloc(&g_eval_arena, sizeof(RuntimeThunk));
    t->fn     = NULL;
    t->env    = NULL;
    t->value  = val;
    t->forced = 1;
    return t;
}

RuntimeThunk *rt_thunk_create(ThunkFn fn, void *env) {
    RuntimeThunk *t = arena_alloc(&g_eval_arena, sizeof(RuntimeThunk));
    t->fn     = fn;
    t->env    = env;
    t->value  = NULL;
    t->forced = 0;
    return t;
}

RuntimeValue *rt_force(RuntimeThunk *thunk) {
    if (!thunk) return rt_value_nil();
    if (thunk->forced) return thunk->value;

    RuntimeValue *result = thunk->fn(thunk->env);

    while (result && result->type == RT_THUNK) {
        RuntimeThunk *inner = result->data.thunk_val;
        if (inner->forced) { result = inner->value; break; }
        result        = inner->fn(inner->env);
        inner->value  = result;
        inner->forced = 1;
    }

    thunk->value  = result;
    thunk->forced = 1;
    return result;
}

///  Lazy list public API

RuntimeList *rt_list_empty(void) {
    return _empty_list;
}

RuntimeList *rt_list_new(void) {
    RuntimeList *lst = heap_list_wrapper();
    lst->cell = NULL;
    return lst;
}

int rt_list_is_empty_list(RuntimeList *list) {
    return (!list || list->cell == NULL) ? 1 : 0;
}

// Low-level lazy cons: caller supplies already-constructed thunk function +
// env for both head and tail.  Used by the range generators below.
// Returns a RuntimeList* pointing at a freshly arena-allocated ConsCell.
static RuntimeList *lazy_cons_fns(ThunkFn hfn, void *henv,
                                  ThunkFn tfn, void *tenv) {
    ConsCell *c   = alloc_cons_cell();
    c->head_fn     = hfn;
    c->head_env    = henv;
    c->head_val    = NULL;
    c->head_forced = 0;
    c->tail_fn     = tfn;
    c->tail_env    = tenv;
    c->tail_val    = NULL;
    c->tail_forced = 0;

    RuntimeList *lst = alloc_list_wrapper();
    lst->cell = c;
    return lst;
}

// Public: takes pre-built legacy RuntimeThunk* for compatibility with codegen
// that still calls rt_list_lazy_cons directly.
RuntimeList *rt_list_lazy_cons(RuntimeThunk *head_thunk, RuntimeThunk *tail_thunk) {
    ConsCell *c   = alloc_cons_cell();

    // Head — if already forced, short-circuit
    if (head_thunk && head_thunk->forced) {
        c->head_fn     = NULL;
        c->head_env    = NULL;
        c->head_val    = head_thunk->value;
        c->head_forced = 1;
    } else if (head_thunk) {
        c->head_fn     = head_thunk->fn;
        c->head_env    = head_thunk->env;
        c->head_val    = NULL;
        c->head_forced = 0;
    } else {
        c->head_fn = NULL; c->head_env = NULL;
        c->head_val = NULL; c->head_forced = 0;
    }

    // Tail
    if (tail_thunk && tail_thunk->forced) {
        c->tail_fn     = NULL;
        c->tail_env    = NULL;
        c->tail_val    = tail_thunk->value;
        c->tail_forced = 1;
    } else if (tail_thunk) {
        c->tail_fn     = tail_thunk->fn;
        c->tail_env    = tail_thunk->env;
        c->tail_val    = NULL;
        c->tail_forced = 0;
    } else {
        c->tail_fn = NULL; c->tail_env = NULL;
        c->tail_val = NULL; c->tail_forced = 1; // NULL tail = end of list
    }

    RuntimeList *lst = alloc_list_wrapper();
    lst->cell = c;
    return lst;
}

// Strict cons: head is already evaluated, tail is already a RuntimeList*.
RuntimeList *rt_list_cons(RuntimeValue *head_val, RuntimeList *tail_list) {
    ConsCell *c    = heap_cons_cell();
    c->head_fn     = NULL;
    c->head_env    = NULL;
    c->head_val    = head_val;
    c->head_forced = 1;
    c->tail_fn     = NULL;
    c->tail_env    = NULL;

    RuntimeValue *tv  = heap_value();
    tv->type          = RT_LIST;
    tv->data.list_val = tail_list;
    c->tail_val    = tv;
    c->tail_forced = 1;

    RuntimeList *lst = heap_list_wrapper();
    lst->cell = c;
    return lst;
}

RuntimeValue *rt_list_car(RuntimeList *list) {
    if (rt_list_is_empty_list(list)) return rt_value_nil();
    return _force_head(list->cell);
}

RuntimeList *rt_list_cdr(RuntimeList *list) {
    if (rt_list_is_empty_list(list)) return rt_list_empty();

    RuntimeValue *tv = _force_tail(list->cell);
    if (!tv || tv->type == RT_NIL)  return rt_list_empty();
    if (tv->type == RT_LIST)        return tv->data.list_val;
    return rt_list_empty();
}

RuntimeValue *rt_list_nth(RuntimeList *list, int64_t index) {
    if (index < 0) return rt_value_nil();
    RuntimeList *cur = list;
    for (int64_t i = 0; i < index; i++) {
        if (rt_list_is_empty_list(cur)) return rt_value_nil();
        cur = rt_list_cdr(cur);
    }
    return rt_list_car(cur);
}

int64_t rt_list_length(RuntimeList *list) {
    int64_t len = 0;
    RuntimeList *cur = list;
    while (!rt_list_is_empty_list(cur)) {
        len++;
        cur = rt_list_cdr(cur);
    }
    return len;
}

// Destructive append of an already-evaluated value.
// Walks the spine and attaches a new strict cons cell at the end.
// Used by printing helpers and rt_list_take.


void rt_list_append(RuntimeList *list, RuntimeValue *value) {
    if (!list) return;

    ConsCell *new_c    = heap_cons_cell();
    new_c->head_fn     = NULL;
    new_c->head_env    = NULL;
    new_c->head_val    = value;
    new_c->head_forced = 1;
    new_c->tail_fn     = NULL;
    new_c->tail_env    = NULL;
    new_c->tail_val    = NULL;
    new_c->tail_forced = 1;

    RuntimeList *new_cell = heap_list_wrapper();
    new_cell->cell = new_c;

    // If list is empty (cell == NULL), seed it in-place.
    // IMPORTANT: callers must NEVER pass the global _empty_list singleton here —
    // they must pass their own heap_list_wrapper() with cell=NULL.
    if (list->cell == NULL) {
        list->cell = new_c;
        return;
    }

    RuntimeList *cur = list;
    while (1) {
        ConsCell *cc = cur->cell;
        if (!cc) break;

        if (!cc->tail_forced) {
            RuntimeValue *tv = _force_tail(cc);
            if (!tv || tv->type == RT_NIL ||
                (tv->type == RT_LIST && rt_list_is_empty_list(tv->data.list_val))) {
                RuntimeValue *link  = heap_value();
                link->type          = RT_LIST;
                link->data.list_val = new_cell;
                cc->tail_val    = link;
                cc->tail_forced = 1;
                return;
            }
            if (tv->type == RT_LIST) { cur = tv->data.list_val; continue; }
            RuntimeValue *link  = heap_value();
            link->type          = RT_LIST;
            link->data.list_val = new_cell;
            cc->tail_val    = link;
            cc->tail_forced = 1;
            return;
        }

        RuntimeValue *tv = cc->tail_val;
        if (!tv || tv->type == RT_NIL ||
            (tv->type == RT_LIST && rt_list_is_empty_list(tv->data.list_val))) {
            RuntimeValue *link  = heap_value();
            link->type          = RT_LIST;
            link->data.list_val = new_cell;
            cc->tail_val    = link;
            cc->tail_forced = 1;
            return;
        }
        if (tv->type == RT_LIST) { cur = tv->data.list_val; continue; }
        RuntimeValue *link  = heap_value();
        link->type          = RT_LIST;
        link->data.list_val = new_cell;
        cc->tail_val    = link;
        cc->tail_forced = 1;
        return;
    }
}

///  Pure lazy append

typedef struct { RuntimeList *rest; RuntimeList *b; } AppendEnv;
static RuntimeValue *_rt_append_tail_fn(void *e);

RuntimeList *rt_list_append_lists(RuntimeList *a, RuntimeList *b) {
    if (rt_list_is_empty_list(a)) return b;

    // Head: share directly — it's immutable after creation
    // We need to wrap the head as a thunk fn for lazy_cons_fns.
    // Simplest: build a fused cell where head is already forced.
    ConsCell *c   = alloc_cons_cell();
    // Copy head from a
    c->head_fn     = NULL;
    c->head_env    = NULL;
    c->head_val    = _force_head(a->cell);
    c->head_forced = 1;

    // Lazy tail
    AppendEnv *env = arena_alloc(&g_eval_arena, sizeof(AppendEnv));
    env->rest = rt_list_cdr(a);
    env->b    = b;
    c->tail_fn     = _rt_append_tail_fn;
    c->tail_env    = env;
    c->tail_val    = NULL;
    c->tail_forced = 0;

    RuntimeList *lst = alloc_list_wrapper();
    lst->cell = c;
    return lst;
}

static RuntimeValue *_rt_append_tail_fn(void *e) {
    AppendEnv   *env    = (AppendEnv *)e;
    // no free — arena owns env
    RuntimeList *result = rt_list_append_lists(env->rest, env->b);
    RuntimeValue *rv = alloc_value();
    rv->type           = RT_LIST;
    rv->data.list_val  = result;
    return rv;
}

RuntimeList *rt_list_copy(RuntimeList *src) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    RuntimeList *cur = src;
    while (!rt_list_is_empty_list(cur)) {
        rt_list_append(out, rt_list_car(cur));
        cur = rt_list_cdr(cur);
    }
    return out;
}

RuntimeList *rt_make_list(int64_t n, RuntimeValue *fill_val) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    for (int64_t i = 0; i < n; i++)
        rt_list_append(out, fill_val);
    return out;
}

// --- [lo .. hi] range ---
typedef struct { int64_t lo; int64_t hi; } RangeEnv;


static RuntimeValue *_rt_range_tail_fn(void *e) {
    RangeEnv *env = (RangeEnv *)e;
    RuntimeValue *rv = malloc(sizeof(RuntimeValue)); // was: alloc_value()
    rv->type          = RT_LIST;
    rv->data.list_val = rt_list_range(env->lo, env->hi);
    return rv;
}

// Head thunk function: env is a pointer to int64_t (the value itself)
static RuntimeValue *_rt_int_head_fn(void *e) {
    int64_t *p = (int64_t *)e;
    return rt_value_int(*p);
}

RuntimeList *rt_list_range(int64_t lo, int64_t hi) {
    if (lo > hi) return rt_list_empty();
    RuntimeValue *head_val = rt_value_int(lo);

    ConsCell *c    = heap_cons_cell();
    c->head_fn     = NULL;
    c->head_env    = NULL;
    c->head_val    = head_val;
    c->head_forced = 1;

    RangeEnv *env  = malloc(sizeof(RangeEnv)); // was: arena_alloc
    env->lo        = lo + 1;
    env->hi        = hi;
    c->tail_fn     = _rt_range_tail_fn;
    c->tail_env    = env;
    c->tail_val    = NULL;
    c->tail_forced = 0;

    RuntimeList *lst = heap_list_wrapper();
    lst->cell = c;
    return lst;
}

// --- [lo ..] infinite ---
typedef struct { int64_t n; } FromEnv;


static RuntimeValue *_rt_from_tail_fn(void *e) {
    FromEnv *env = (FromEnv *)e;
    RuntimeList *result = rt_list_from(env->n); // now heap-allocates recursively
    RuntimeValue *rv = malloc(sizeof(RuntimeValue)); // was: alloc_value()
    rv->type          = RT_LIST;
    rv->data.list_val = result;
    return rv;
}

// --- [lo ..] infinite ---
RuntimeList *rt_list_from(int64_t lo) {
    RuntimeValue *head_val = rt_value_int(lo);

    ConsCell *c    = heap_cons_cell();        // was: alloc_cons_cell()
    c->head_fn     = NULL;
    c->head_env    = NULL;
    c->head_val    = head_val;
    c->head_forced = 1;

    FromEnv *env   = malloc(sizeof(FromEnv)); // was: arena_alloc(...)
    env->n         = lo + 1;
    c->tail_fn     = _rt_from_tail_fn;
    c->tail_env    = env;
    c->tail_val    = NULL;
    c->tail_forced = 0;

    RuntimeList *lst = heap_list_wrapper();   // was: alloc_list_wrapper()
    lst->cell = c;
    return lst;
}

// --- [lo, next ..] arithmetic sequence ---
typedef struct { int64_t n; int64_t step; } FromStepEnv;


static RuntimeValue *_rt_from_step_tail_fn(void *e) {
    FromStepEnv *env = (FromStepEnv *)e;
    RuntimeValue *rv = malloc(sizeof(RuntimeValue)); // was: alloc_value()
    rv->type          = RT_LIST;
    rv->data.list_val = rt_list_from_step(env->n, env->step);
    return rv;
}

RuntimeList *rt_list_from_step(int64_t lo, int64_t step) {
    RuntimeValue *head_val = rt_value_int(lo);

    ConsCell *c    = heap_cons_cell();
    c->head_fn     = NULL;
    c->head_env    = NULL;
    c->head_val    = head_val;
    c->head_forced = 1;

    FromStepEnv *env = malloc(sizeof(FromStepEnv)); // was: arena_alloc
    env->n           = lo + step;
    env->step        = step;
    c->tail_fn       = _rt_from_step_tail_fn;
    c->tail_env      = env;
    c->tail_val      = NULL;
    c->tail_forced   = 0;

    RuntimeList *lst = heap_list_wrapper();
    lst->cell = c;
    return lst;
}

// --- take / drop ---

RuntimeList *rt_list_take(RuntimeList *list, int64_t n) {
    if (n <= 0 || rt_list_is_empty_list(list)) return rt_list_empty();
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    RuntimeList *cur = list;
    for (int64_t i = 0; i < n && !rt_list_is_empty_list(cur); i++) {
        rt_list_append(out, rt_list_car(cur));
        cur = rt_list_cdr(cur);
    }
    return out;
}

char *rt_string_take(const char *s, int64_t n) {
    if (!s) return strdup("");
    int64_t len = (int64_t)strlen(s);
    if (n < 0) n = 0;
    if (n > len) n = len;
    char *result = malloc(n + 1);
    memcpy(result, s, n);
    result[n] = '\0';
    return result;
}

RuntimeList *rt_list_drop(RuntimeList *list, int64_t n) {
    RuntimeList *cur = list;
    for (int64_t i = 0; i < n && !rt_list_is_empty_list(cur); i++)
        cur = rt_list_cdr(cur);
    return cur;
}


/// Map

#define MAP_INITIAL_CAP 8
#define MAP_LOAD_NUM    7
#define MAP_LOAD_DEN    10

static RuntimeMapEntry MAP_TOMBSTONE_ENTRY = {NULL, NULL};
#define MAP_TOMBSTONE (&MAP_TOMBSTONE_ENTRY)

static RuntimeMap *map_alloc(size_t cap) {
    RuntimeMap *m  = malloc(sizeof(RuntimeMap));
    m->buckets     = calloc(cap, sizeof(RuntimeMapEntry));
    m->capacity    = cap;
    m->count       = 0;
    m->tombstones  = 0;
    return m;
}

static size_t map_probe(RuntimeMap *m, RuntimeValue *key) {
    uint64_t h    = rt_hash_value(key);
    size_t   mask = m->capacity - 1;
    return (size_t)(h & mask);
}

static void map_insert_noresize(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val) {
    size_t mask       = m->capacity - 1;
    size_t idx        = map_probe(m, key);
    size_t first_tomb = SIZE_MAX;

    for (size_t i = 0; i < m->capacity; i++) {
        size_t          slot = (idx + i) & mask;
        RuntimeMapEntry *e   = &m->buckets[slot];

        if (!e->key) {
            size_t write = (first_tomb != SIZE_MAX) ? first_tomb : slot;
            m->buckets[write].key = key;
            m->buckets[write].val = val;
            m->count++;
            if (first_tomb != SIZE_MAX) m->tombstones--;
            return;
        }
        if (e == MAP_TOMBSTONE) {
            if (first_tomb == SIZE_MAX) first_tomb = slot;
            continue;
        }
        if (rt_equal_p(e->key, key)) {
            e->val = val;  /* update existing */
            return;
        }
    }
    if (first_tomb != SIZE_MAX) {
        m->buckets[first_tomb].key = key;
        m->buckets[first_tomb].val = val;
        m->count++;
        m->tombstones--;
    }
}

static void map_resize(RuntimeMap *m) {
    size_t          new_cap = m->capacity * 2;
    RuntimeMapEntry *old    = m->buckets;
    size_t          old_cap = m->capacity;

    m->buckets    = calloc(new_cap, sizeof(RuntimeMapEntry));
    m->capacity   = new_cap;
    m->count      = 0;
    m->tombstones = 0;

    for (size_t i = 0; i < old_cap; i++) {
        RuntimeMapEntry *e = &old[i];
        if (e->key && e != MAP_TOMBSTONE)
            map_insert_noresize(m, e->key, e->val);
    }
    free(old);
}

static RuntimeMap *map_insert(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val) {
    if (!key || key->type == RT_NIL) return m;
    if ((m->count + m->tombstones + 1) * MAP_LOAD_DEN
         >= m->capacity * MAP_LOAD_NUM)
        map_resize(m);
    map_insert_noresize(m, key, val);
    return m;
}

static RuntimeMap *map_remove(RuntimeMap *m, RuntimeValue *key) {
    if (!m || !key) return m;
    size_t mask = m->capacity - 1;
    size_t idx  = map_probe(m, key);

    for (size_t i = 0; i < m->capacity; i++) {
        size_t          slot = (idx + i) & mask;
        RuntimeMapEntry *e   = &m->buckets[slot];
        if (!e->key) return m;
        if (e == MAP_TOMBSTONE) continue;
        if (rt_equal_p(e->key, key)) {
            e->key = MAP_TOMBSTONE->key;
            e->val = MAP_TOMBSTONE->val;
            m->buckets[slot] = *MAP_TOMBSTONE;
            m->count--;
            m->tombstones++;
            return m;
        }
    }
    return m;
}

static RuntimeMap *map_copy(RuntimeMap *m) {
    RuntimeMap *copy = map_alloc(m->capacity);
    for (size_t i = 0; i < m->capacity; i++) {
        RuntimeMapEntry *e = &m->buckets[i];
        if (e->key && e != MAP_TOMBSTONE)
            map_insert_noresize(copy, e->key, e->val);
    }
    return copy;
}

RuntimeMap *rt_map_new(void) {
    return map_alloc(MAP_INITIAL_CAP);
}

RuntimeMap *rt_map_assoc(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val) {
    RuntimeMap *copy = map_copy(m);
    return map_insert(copy, key, val);
}

RuntimeMap *rt_map_assoc_mut(RuntimeMap *m, RuntimeValue *key, RuntimeValue *val) {
    return map_insert(m, key, val);
}

RuntimeMap *rt_map_dissoc(RuntimeMap *m, RuntimeValue *key) {
    if (!rt_map_contains(m, key)) return m;
    return map_remove(map_copy(m), key);
}

RuntimeMap *rt_map_dissoc_mut(RuntimeMap *m, RuntimeValue *key) {
    return map_remove(m, key);
}

RuntimeValue *rt_map_get(RuntimeMap *m, RuntimeValue *key, RuntimeValue *default_val) {
    if (!m || !key) return default_val ? default_val : rt_value_nil();
    size_t mask = m->capacity - 1;
    size_t idx  = map_probe(m, key);

    for (size_t i = 0; i < m->capacity; i++) {
        size_t          slot = (idx + i) & mask;
        RuntimeMapEntry *e   = &m->buckets[slot];
        if (!e->key) return default_val ? default_val : rt_value_nil();
        if (e == MAP_TOMBSTONE) continue;
        if (rt_equal_p(e->key, key)) return e->val;
    }
    return default_val ? default_val : rt_value_nil();
}

int rt_map_contains(RuntimeMap *m, RuntimeValue *key) {
    if (!m || !key) return 0;
    size_t mask = m->capacity - 1;
    size_t idx  = map_probe(m, key);

    for (size_t i = 0; i < m->capacity; i++) {
        size_t          slot = (idx + i) & mask;
        RuntimeMapEntry *e   = &m->buckets[slot];
        if (!e->key) return 0;
        if (e == MAP_TOMBSTONE) continue;
        if (rt_equal_p(e->key, key)) return 1;
    }
    return 0;
}

RuntimeValue *rt_map_find(RuntimeMap *m, RuntimeValue *key) {
    if (!m || !key) return rt_value_nil();
    size_t mask = m->capacity - 1;
    size_t idx  = map_probe(m, key);
    for (size_t i = 0; i < m->capacity; i++) {
        size_t          slot = (idx + i) & mask;
        RuntimeMapEntry *e   = &m->buckets[slot];
        if (!e->key)              return rt_value_nil();
        if (e == MAP_TOMBSTONE)   continue;
        if (rt_equal_p(e->key, key)) {
            RuntimeList *pair = heap_list_wrapper();
            pair->cell = NULL;
            rt_list_append(pair, e->key);
            rt_list_append(pair, e->val);
            RuntimeValue *v = malloc(sizeof(RuntimeValue));
            v->type          = RT_LIST;
            v->data.list_val = pair;
            return v;
        }
    }
    return rt_value_nil();
}

int64_t rt_map_count(RuntimeMap *m) {
    return m ? (int64_t)m->count : 0;
}

RuntimeList *rt_map_keys(RuntimeMap *m) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    if (!m) return out;
    for (size_t i = 0; i < m->capacity; i++) {
        RuntimeMapEntry *e = &m->buckets[i];
        if (e->key && e != MAP_TOMBSTONE)
            rt_list_append(out, e->key);
    }
    return out;
}

RuntimeList *rt_map_vals(RuntimeMap *m) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    if (!m) return out;
    for (size_t i = 0; i < m->capacity; i++) {
        RuntimeMapEntry *e = &m->buckets[i];
        if (e->key && e != MAP_TOMBSTONE)
            rt_list_append(out, e->val);
    }
    return out;
}

RuntimeMap *rt_map_merge(RuntimeMap *a, RuntimeMap *b) {
    RuntimeMap *out = map_copy(a);
    for (size_t i = 0; i < b->capacity; i++) {
        RuntimeMapEntry *e = &b->buckets[i];
        if (e->key && e != MAP_TOMBSTONE)
            map_insert(out, e->key, e->val);
    }
    return out;
}

RuntimeMap *rt_map_merge_with(RuntimeMap *a, RuntimeMap *b,
                               RuntimeValue *(*fn)(RuntimeValue *, RuntimeValue *)) {
    RuntimeMap *out = map_copy(a);
    for (size_t i = 0; i < b->capacity; i++) {
        RuntimeMapEntry *e = &b->buckets[i];
        if (!e->key || e == MAP_TOMBSTONE) continue;
        RuntimeValue *existing = rt_map_get(out, e->key, NULL);
        if (existing && existing->type != RT_NIL)
            map_insert(out, e->key, fn(existing, e->val));
        else
            map_insert(out, e->key, e->val);
    }
    return out;
}

int rt_map_equal(RuntimeMap *a, RuntimeMap *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->capacity; i++) {
        RuntimeMapEntry *e = &a->buckets[i];
        if (!e->key || e == MAP_TOMBSTONE) continue;
        RuntimeValue *bval = rt_map_get(b, e->key, NULL);
        if (!bval || bval->type == RT_NIL) return 0;
        if (!rt_equal_p(e->val, bval)) return 0;
    }
    return 1;
}

void rt_map_free(RuntimeMap *m) {
    if (!m) return;
    free(m->buckets);
    free(m);
}

RuntimeValue *rt_value_map(RuntimeMap *m) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type         = RT_MAP;
    v->data.map_val = m;
    return v;
}

RuntimeMap *rt_unbox_map(RuntimeValue *v) {
    if (!v || v->type != RT_MAP) return rt_map_new();
    return v->data.map_val;
}

/// Set — heap-allocated open-addressing hash set

// Sentinel: a static TOMBSTONE pointer marks deleted slots.
// Load factor threshold: 0.7 (count + tombstones).
// Capacity is always a power of two so slot = hash & (cap-1).

#define SET_INITIAL_CAP 8
#define SET_LOAD_NUM    7
#define SET_LOAD_DEN    10

static RuntimeValue _tombstone_val = { .type = RT_NIL };
static RuntimeValue *TOMBSTONE = &_tombstone_val;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

static uint64_t rt_hash_value(RuntimeValue *v) {
    if (!v || v->type == RT_NIL) return 0;

    switch (v->type) {

    case RT_INT:
        return (uint64_t)v->data.int_val * 2654435761ULL;

    case RT_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &v->data.float_val, sizeof(bits));
        return bits * 2654435761ULL;
    }

    case RT_CHAR:
        return (uint64_t)(unsigned char)v->data.char_val * 2654435761ULL;

    case RT_STRING:
        return fnv1a(v->data.string_val);

    case RT_SYMBOL:
        return fnv1a(v->data.symbol_val);

    case RT_KEYWORD:
        return fnv1a(v->data.keyword_val);

    case RT_RATIO:
        return (uint64_t)v->data.ratio_val.numerator   * 2654435761ULL
             ^ (uint64_t)v->data.ratio_val.denominator * 40503ULL;

    case RT_LIST: {
        uint64_t h = 0;
        RuntimeList *cur = v->data.list_val;
        int limit = 8;
        while (!rt_list_is_empty_list(cur) && limit-- > 0) {
            h = h * 31 + rt_hash_value(rt_list_car(cur));
            cur = rt_list_cdr(cur);
        }
        return h;
    }

    case RT_SET: {
        uint64_t h = 0;
        RuntimeSet *s = v->data.set_val;
        if (!s) return 0;
        for (size_t i = 0; i < s->capacity; i++) {
            RuntimeValue *elem = s->buckets[i];
            if (elem && elem != TOMBSTONE)
                h ^= rt_hash_value(elem);
        }
        return h;
    }

    case RT_MAP: {
        uint64_t h = 0;
        RuntimeMap *m = v->data.map_val;
        if (!m) return 0;
        for (size_t i = 0; i < m->capacity; i++) {
            RuntimeMapEntry *e = &m->buckets[i];
            if (e->key && e != MAP_TOMBSTONE)
                h ^= rt_hash_value(e->key) * 31 + rt_hash_value(e->val);
        }
        return h;
    }

    default:
        return (uint64_t)(uintptr_t)v;
    }
}

static RuntimeSet *set_alloc(size_t cap) {
    RuntimeSet *s   = malloc(sizeof(RuntimeSet));
    s->buckets      = calloc(cap, sizeof(RuntimeValue *));
    s->capacity     = cap;
    s->count        = 0;
    s->tombstones   = 0;
    return s;
}

static void set_insert_noresize(RuntimeSet *s, RuntimeValue *val) {
    uint64_t h    = rt_hash_value(val);
    size_t   mask = s->capacity - 1;
    size_t   idx  = (size_t)(h & mask);
    size_t   first_tomb = SIZE_MAX;

    for (size_t i = 0; i < s->capacity; i++) {
        size_t slot = (idx + i) & mask;
        RuntimeValue *cur = s->buckets[slot];

        if (!cur) {
            /* empty slot */
            size_t write = (first_tomb != SIZE_MAX) ? first_tomb : slot;
            s->buckets[write] = val;
            s->count++;
            if (first_tomb != SIZE_MAX) s->tombstones--;
            return;
        }
        if (cur == TOMBSTONE) {
            if (first_tomb == SIZE_MAX) first_tomb = slot;
            continue;
        }
        if (rt_equal_p(cur, val)) return; /* already present */
    }
    /* table full (shouldn't happen if resize works) */
    if (first_tomb != SIZE_MAX) {
        s->buckets[first_tomb] = val;
        s->count++;
        s->tombstones--;
    }
}

static void set_resize(RuntimeSet *s) {
    size_t     new_cap  = s->capacity * 2;
    RuntimeValue **old  = s->buckets;
    size_t     old_cap  = s->capacity;

    s->buckets    = calloc(new_cap, sizeof(RuntimeValue *));
    s->capacity   = new_cap;
    s->count      = 0;
    s->tombstones = 0;

    for (size_t i = 0; i < old_cap; i++) {
        RuntimeValue *v = old[i];
        if (v && v != TOMBSTONE)
            set_insert_noresize(s, v);
    }
    free(old);
}

RuntimeSet *rt_set_new(void) {
    return set_alloc(SET_INITIAL_CAP);
}


int rt_set_contains(RuntimeSet *s, RuntimeValue *val) {
    if (!s || !val) return 0;
    uint64_t h    = rt_hash_value(val);
    size_t   mask = s->capacity - 1;
    size_t   idx  = (size_t)(h & mask);

    for (size_t i = 0; i < s->capacity; i++) {
        size_t        slot = (idx + i) & mask;
        RuntimeValue *cur  = s->buckets[slot];
        if (!cur)          return 0;
        if (cur == TOMBSTONE) continue;
        if (rt_equal_p(cur, val)) return 1;
    }
    return 0;
}

RuntimeValue *rt_set_get(RuntimeSet *s, RuntimeValue *key) {
    if (!s || !key) return rt_value_nil();
    uint64_t h    = rt_hash_value(key);
    size_t   mask = s->capacity - 1;
    size_t   idx  = (size_t)(h & mask);

    for (size_t i = 0; i < s->capacity; i++) {
        size_t        slot = (idx + i) & mask;
        RuntimeValue *cur  = s->buckets[slot];
        if (!cur)          return rt_value_nil();
        if (cur == TOMBSTONE) continue;
        if (rt_equal_p(cur, key)) return cur;
    }
    return rt_value_nil();
}

/* Internal: insert val into s without copying. Resizes in place if needed.
 * Returns s. Caller owns s.                                               */
static RuntimeSet *set_insert(RuntimeSet *s, RuntimeValue *val) {
    if (!val || val->type == RT_NIL) return s;
    if ((s->count + s->tombstones + 1) * SET_LOAD_DEN
         >= s->capacity * SET_LOAD_NUM)
        set_resize(s);
    set_insert_noresize(s, val);
    return s;
}

/* Internal: remove val from s without copying.
 * Returns s. Caller owns s.                                               */
static RuntimeSet *set_remove(RuntimeSet *s, RuntimeValue *val) {
    if (!s || !val) return s;
    uint64_t h    = rt_hash_value(val);
    size_t   mask = s->capacity - 1;
    size_t   idx  = (size_t)(h & mask);
    for (size_t i = 0; i < s->capacity; i++) {
        size_t        slot = (idx + i) & mask;
        RuntimeValue *cur  = s->buckets[slot];
        if (!cur) return s;
        if (cur == TOMBSTONE) continue;
        if (rt_equal_p(cur, val)) {
            s->buckets[slot] = TOMBSTONE;
            s->count--;
            s->tombstones++;
            return s;
        }
    }
    return s;
}

/* Internal: shallow copy of s into a new set of the same capacity. */
static RuntimeSet *set_copy(RuntimeSet *s) {
    RuntimeSet *copy = set_alloc(s->capacity);
    for (size_t i = 0; i < s->capacity; i++) {
        RuntimeValue *v = s->buckets[i];
        if (v && v != TOMBSTONE)
            set_insert_noresize(copy, v);
    }
    return copy;
}

/* Immutable — return a new set */
RuntimeSet *rt_set_conj(RuntimeSet *s, RuntimeValue *val)
 {return set_insert(set_copy(s), val);}

RuntimeSet *rt_set_disj(RuntimeSet *s, RuntimeValue *val)
{if (!rt_set_contains(s, val)) return s;
    return set_remove(set_copy(s), val);}

/* Mutable — modify in place, return s */
RuntimeSet *rt_set_conj_mut(RuntimeSet *s, RuntimeValue *val)
{return set_insert(s, val);}

RuntimeSet *rt_set_disj_mut(RuntimeSet *s, RuntimeValue *val)
{return set_remove(s, val);}



int64_t rt_set_count(RuntimeSet *s) {
    return s ? (int64_t)s->count : 0;
}

RuntimeList *rt_set_seq(RuntimeSet *s) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    if (!s) return out;
    for (size_t i = 0; i < s->capacity; i++) {
        RuntimeValue *v = s->buckets[i];
        if (v && v != TOMBSTONE)
            rt_list_append(out, v);
    }
    return out;
}

int rt_set_equal(RuntimeSet *a, RuntimeSet *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->capacity; i++) {
        RuntimeValue *v = a->buckets[i];
        if (v && v != TOMBSTONE)
            if (!rt_set_contains(b, v)) return 0;
    }
    return 1;
}

RuntimeSet *rt_set_of(RuntimeValue **vals, size_t n) {
    RuntimeSet *s = rt_set_new();
    for (size_t i = 0; i < n; i++)
        rt_set_conj(s, vals[i]);
    return s;
}

RuntimeSet *rt_set_from_list(RuntimeList *list) {
    RuntimeSet  *s   = rt_set_new();
    RuntimeList *cur = list;
    while (!rt_list_is_empty_list(cur)) {
        set_insert(s, rt_list_car(cur));
        cur = rt_list_cdr(cur);
    }
    return s;
}

RuntimeSet *rt_set_from_array(RuntimeValue *array_rv) {
    RuntimeSet *s = rt_set_new();
    if (!array_rv || array_rv->type != RT_ARRAY) return s;
    for (size_t i = 0; i < array_rv->data.array_val.length; i++) {
        RuntimeValue *v = array_rv->data.array_val.elements[i];
        if (v) set_insert(s, v);
    }
    return s;
}

/// Set — higher-order operations

RuntimeValue *rt_set_foldl(RuntimeSet *s, RuntimeValue *init,
                            void *env, RT_BinaryFn fn) {
    RuntimeValue *acc = init;
    if (!s) return acc;
    for (size_t i = 0; i < s->capacity; i++) {
        RuntimeValue *v = s->buckets[i];
        if (v && v != TOMBSTONE)
            { RuntimeValue *_a[] = {acc, v}; acc = fn(env, 2, _a); }
    }
    return acc;
}

RuntimeList *rt_set_map(RuntimeSet *s, void *env, RT_UnaryFn fn) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    if (!s) return out;
    for (size_t i = 0; i < s->capacity; i++) {
        RuntimeValue *v = s->buckets[i];
        if (v && v != TOMBSTONE)
            { RuntimeValue *_a[] = {v}; rt_list_append(out, fn(env, 1, _a)); }
    }
    return out;
}

RuntimeSet *rt_set_filter(RuntimeSet *s, void *env, RT_UnaryFn pred) {
    RuntimeSet *out = rt_set_new();
    if (!s) return out;
    for (size_t i = 0; i < s->capacity; i++) {
        RuntimeValue *v = s->buckets[i];
        if (v && v != TOMBSTONE) {
            RuntimeValue *_pa[] = {v}; RuntimeValue *result = pred(env, 1, _pa);
            if (rt_unbox_int(result) != 0)
                set_insert(out, v);
        }
    }
    return out;
}

void rt_set_free(RuntimeSet *s) {
    if (!s) return;
    free(s->buckets);
    free(s);
}

RuntimeValue *rt_value_set(RuntimeSet *s) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type         = RT_SET;
    v->data.set_val = s;
    return v;
}

RuntimeSet *rt_unbox_set(RuntimeValue *v) {
    if (!v || v->type != RT_SET) return rt_set_new();
    return v->data.set_val;
}


///  Equality

int rt_equal_p(RuntimeValue *a, RuntimeValue *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type == RT_THUNK) a = rt_force(a->data.thunk_val);
    if (b->type == RT_THUNK) b = rt_force(b->data.thunk_val);
    if (a->type == RT_NIL && b->type == RT_NIL) return 1;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case RT_INT:     return a->data.int_val   == b->data.int_val;
        case RT_FLOAT:   return a->data.float_val == b->data.float_val;
        case RT_CHAR:    return a->data.char_val  == b->data.char_val;
        case RT_STRING:  return strcmp(a->data.string_val,  b->data.string_val)  == 0;
        case RT_SYMBOL:  return strcmp(a->data.symbol_val,  b->data.symbol_val)  == 0;
        case RT_KEYWORD: return strcmp(a->data.keyword_val, b->data.keyword_val) == 0;
        case RT_NIL:     return 1;
        case RT_SET:     return rt_set_equal(a->data.set_val, b->data.set_val);
        case RT_MAP:
            if (b->type != RT_MAP) return 0;
            return rt_map_equal(a->data.map_val, b->data.map_val);
        case RT_LIST: {
            RuntimeList *la = a->data.list_val;
            RuntimeList *lb = b->data.list_val;
            while (!rt_list_is_empty_list(la) && !rt_list_is_empty_list(lb)) {
                if (!rt_equal_p(rt_list_car(la), rt_list_car(lb))) return 0;
                la = rt_list_cdr(la);
                lb = rt_list_cdr(lb);
            }
            return rt_list_is_empty_list(la) && rt_list_is_empty_list(lb);
        }
        default: return 0;
    }
}

///  Unboxing

// TODO UNBOX BIGNUM
int64_t rt_unbox_int(RuntimeValue *v) {
    if (!v || v->type == RT_NIL) return 0;
    if (v->type == RT_THUNK) v = rt_force(v->data.thunk_val);
    if (v->type == RT_INT)   return v->data.int_val;
    if (v->type == RT_FLOAT) return (int64_t)v->data.float_val;
    if (v->type == RT_CHAR)  return (int64_t)v->data.char_val;
    return 0;
}

double rt_unbox_float(RuntimeValue *v) {
    if (!v || v->type == RT_NIL) return 0.0;
    if (v->type == RT_THUNK) v = rt_force(v->data.thunk_val);
    if (v->type == RT_FLOAT) return v->data.float_val;
    if (v->type == RT_INT)   return (double)v->data.int_val;
    if (v->type == RT_CHAR)  return (double)v->data.char_val;
    return 0.0;
}

char rt_unbox_char(RuntimeValue *v) {
    if (!v || v->type == RT_NIL) return 0;
    if (v->type == RT_THUNK) v = rt_force(v->data.thunk_val);
    if (v->type == RT_CHAR) return v->data.char_val;
    if (v->type == RT_INT)  return (char)v->data.int_val;
    return 0;
}

char *rt_unbox_string(RuntimeValue *v) {
    if (!v || v->type == RT_NIL) return "";
    if (v->type == RT_THUNK) v = rt_force(v->data.thunk_val);
    if (v->type == RT_STRING) return v->data.string_val;
    return "";
}

RuntimeList *rt_unbox_list(RuntimeValue *v) {
    if (!v) return rt_list_empty();
    int tag = (int)v->type;
    if (tag < 0 || tag > RT_CLOSURE) {
        /* Treat as raw RuntimeList* */
        return (RuntimeList *)v;
    }
    if (v->type == RT_NIL) return rt_list_empty();
    if (v->type == RT_THUNK) v = rt_force(v->data.thunk_val);
    if (v->type == RT_LIST) return v->data.list_val;
    if (v->type == RT_SET)  return rt_set_seq(v->data.set_val);
    return rt_list_empty();
}

int rt_value_is_nil(RuntimeValue *v) {
    return (!v || v->type == RT_NIL) ? 1 : 0;
}

void rt_print_value_newline(RuntimeValue *v) {
    rt_print_value(v);
    printf("\n");
}

///  Value construction
//
//  HOT PATH (arena):  int, float, char, list, nil, thunk
//  LONG-LIVED (heap): string, symbol, keyword, ratio, array
//
RuntimeValue *rt_value_int(int64_t val) {
    // Step 2: integer interning — zero allocations for 0..65536
    int_cache_init();
    if (val >= INT_CACHE_MIN && val <= INT_CACHE_MAX)
        return &g_int_cache_storage[val - INT_CACHE_MIN];
    RuntimeValue *v = alloc_value();
    v->type = RT_INT; v->data.int_val = val; return v;
}

RuntimeValue *rt_value_float(double val) {
    RuntimeValue *v = alloc_value();
    v->type = RT_FLOAT; v->data.float_val = val; return v;
}

RuntimeValue *rt_value_char(char val) {
    RuntimeValue *v = alloc_value();
    v->type = RT_CHAR; v->data.char_val = val; return v;
}

RuntimeValue *rt_value_list(RuntimeList *val) {
    RuntimeValue *v = alloc_value();
    v->type = RT_LIST; v->data.list_val = val; return v;
}

RuntimeValue *rt_value_nil(void) {
    RuntimeValue *v = alloc_value();
    v->type = RT_NIL; return v;
}

RuntimeValue *rt_value_thunk(RuntimeThunk *thunk) {
    RuntimeValue *v = alloc_value();
    v->type = RT_THUNK; v->data.thunk_val = thunk; return v;
}

// Heap-allocated (long-lived)
RuntimeValue *rt_value_string(const char *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_STRING; v->data.string_val = strdup(val); return v;
}
RuntimeValue *rt_value_symbol(const char *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_SYMBOL; v->data.symbol_val = strdup(val); return v;
}
RuntimeValue *rt_value_keyword(const char *val) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_KEYWORD; v->data.keyword_val = strdup(val); return v;
}

///  Printing

// Forward declarations
static void rt_print_value_indent(RuntimeValue *val, int indent);
static void rt_print_list_indent(RuntimeList *list, int indent);

// Estimate if a value prints short enough to stay inline
static bool rt_value_is_short(RuntimeValue *val, int budget) {
    if (!val || budget <= 0) return false;
    switch (val->type) {
        case RT_INT:    return true;
        case RT_FLOAT:  return true;
        case RT_SYMBOL: return (int)strlen(val->data.symbol_val) < budget;
        case RT_STRING: return (int)strlen(val->data.string_val) + 2 < budget;
        case RT_NIL:    return true;
        case RT_LIST: {
            // Short if all elements are atoms and total is under budget
            RuntimeList *cur = val->data.list_val;
            int total = 2; // parens
            while (!rt_list_is_empty_list(cur)) {
                RuntimeValue *h = rt_list_car(cur);
                if (!h) return false;
                if (h->type == RT_LIST) {
                    // Allow one level of nesting if it's short
                    if (!rt_value_is_short(h, budget / 2)) return false;
                    total += budget / 2;
                } else if (h->type == RT_SYMBOL) {
                    total += strlen(h->data.symbol_val) + 1;
                } else if (h->type == RT_INT || h->type == RT_FLOAT) {
                    total += 8;
                } else {
                    return false;
                }
                if (total >= budget) return false;
                cur = rt_list_cdr(cur);
            }
            return total < budget;
        }
        default: return false;
    }
}

static void rt_print_list_indent(RuntimeList *list, int indent) {
    if (rt_list_is_empty_list(list)) { printf("()"); return; }

    // Collect elements
    RuntimeValue *elems[4096];
    int count = 0;
    RuntimeList *cur = list;
    while (!rt_list_is_empty_list(cur) && count < 4095) {
        if (rt_interrupted) { printf(" ..."); return; }
        elems[count++] = rt_list_car(cur);
        cur = rt_list_cdr(cur);
    }
    bool truncated = !rt_list_is_empty_list(cur);

    int inner = indent + 2;

    // Check if head is a special form symbol
    bool is_lambda = (count >= 1 && elems[0] &&
                      elems[0]->type == RT_SYMBOL &&
                      strcmp(elems[0]->data.symbol_val, "lambda") == 0);
    bool is_if     = (count >= 1 && elems[0] &&
                      elems[0]->type == RT_SYMBOL &&
                      strcmp(elems[0]->data.symbol_val, "if") == 0);
    bool is_define = (count >= 1 && elems[0] &&
                      elems[0]->type == RT_SYMBOL &&
                      strcmp(elems[0]->data.symbol_val, "define") == 0);

    // Check if any element is a list
    bool has_list = false;
    for (int i = 0; i < count; i++)
        if (elems[i] && elems[i]->type == RT_LIST)
            has_list = true;

    if (!has_list) {
        // All atoms — single line
        printf("(");
        for (int i = 0; i < count; i++) {
            if (i > 0) printf(" ");
            rt_print_value_indent(elems[i], inner);
        }
        if (truncated) printf(" ...");
        printf(")");
        return;
    }

    if (is_lambda && count >= 3) {
        // (lambda (params) body...)
        // Print as: (lambda (params)
        //             body...)
        printf("(lambda ");
        rt_print_value_indent(elems[1], inner);  // params list on same line
        for (int i = 2; i < count; i++) {
            printf("\n");
            for (int s = 0; s < inner; s++) printf(" ");
            rt_print_value_indent(elems[i], inner);
        }
        if (truncated) printf(" ...");
        printf(")");
        return;
    }

    if (is_if && count >= 3) {
        // (if test then else?)
        // Print as: (if test
        //              then
        //              else)
        printf("(if ");
        rt_print_value_indent(elems[1], inner);  // test on same line
        for (int i = 2; i < count; i++) {
            printf("\n");
            for (int s = 0; s < inner; s++) printf(" ");
            rt_print_value_indent(elems[i], inner);
        }
        if (truncated) printf(" ...");
        printf(")");
        return;
    }

    if (is_define && count >= 3) {
        // (define name value)
        printf("(define ");
        rt_print_value_indent(elems[1], inner);
        for (int i = 2; i < count; i++) {
            printf("\n");
            for (int s = 0; s < inner; s++) printf(" ");
            rt_print_value_indent(elems[i], inner);
        }
        if (truncated) printf(" ...");
        printf(")");
        return;
    }

    // General case: first element on same line, nested lists indented only if long
    printf("(");
    rt_print_value_indent(elems[0], inner);
    for (int i = 1; i < count; i++) {
        if (elems[i] && elems[i]->type == RT_LIST &&
            !rt_value_is_short(elems[i], 40)) {
            printf("\n");
            for (int s = 0; s < inner; s++) printf(" ");
            rt_print_value_indent(elems[i], inner);
        } else {
            printf(" ");
            rt_print_value_indent(elems[i], inner);
        }
    }

    if (truncated) printf(" ...");
    printf(")");
}

static void rt_print_value_indent(RuntimeValue *val, int indent) {
    if (!val) { printf("nil"); return; }
    if (val->type == RT_THUNK) {
        val = rt_force(val->data.thunk_val);
        if (!val) { printf("nil"); return; }
    }
    switch (val->type) {
        case RT_INT:     printf("%ld",  val->data.int_val);   break;
        case RT_FLOAT:   printf("%g",   val->data.float_val); break;
        case RT_CHAR:    printf("'%c'", val->data.char_val);  break;
        case RT_STRING:  printf("\"%s\"", val->data.string_val); break;
        case RT_SYMBOL:  printf("%s",   val->data.symbol_val);   break;
        case RT_KEYWORD: printf("%s",  val->data.keyword_val);  break;
        case RT_NIL:     printf("nil"); break;
        case RT_LIST:    rt_print_list_indent(val->data.list_val, indent); break;
        case RT_CLOSURE: printf("<closure/%d>", val->data.closure_val->arity); break;
        case RT_RATIO:
            if (val->data.ratio_val.denominator == 1)
                printf("%ld", val->data.ratio_val.numerator);
            else
                printf("%ld/%ld", val->data.ratio_val.numerator,
                                  val->data.ratio_val.denominator);
            break;
        case RT_ARRAY:
            printf("[");
            for (size_t i = 0; i < val->data.array_val.length; i++) {
                if (i > 0) printf(" ");
                rt_print_value_indent(val->data.array_val.elements[i]
                                      ? val->data.array_val.elements[i]
                                      : rt_value_nil(), indent);
            }
            printf("]");
            break;
        case RT_SET: {
            RuntimeSet *s = val->data.set_val;
            printf("{");
            int first = 1;
            for (size_t i = 0; i < s->capacity; i++) {
                RuntimeValue *elem = s->buckets[i];
                if (elem && elem != TOMBSTONE) {
                    if (!first) printf(" ");
                    rt_print_value_indent(elem, indent);
                    first = 0;
                }
            }
            printf("}");
            break;
        }
        case RT_MAP: {
            RuntimeMap *m = val->data.map_val;
            if (!m || m->count == 0) { printf("#{}"); break; }

            /* Collect live entries */
            RuntimeMapEntry *entries[4096];
            int count = 0;
            for (size_t i = 0; i < m->capacity && count < 4095; i++) {
                RuntimeMapEntry *e = &m->buckets[i];
                if (e->key && e != MAP_TOMBSTONE)
                    entries[count++] = e;
            }

            /* Measure max key width using snprintf into a scratch buffer */
            int max_key_w = 0;
            for (int i = 0; i < count; i++) {
                char buf[256];
                int  w = 0;
                RuntimeValue *k = entries[i]->key;
                switch (k->type) {
                case RT_STRING:  w = snprintf(buf, sizeof(buf), "\"%s\"", k->data.string_val);  break;
                case RT_KEYWORD: w = snprintf(buf, sizeof(buf), "%s",     k->data.keyword_val); break;
                case RT_SYMBOL:  w = snprintf(buf, sizeof(buf), "%s",     k->data.symbol_val);  break;
                case RT_INT:     w = snprintf(buf, sizeof(buf), "%ld",    k->data.int_val);     break;
                case RT_FLOAT:   w = snprintf(buf, sizeof(buf), "%g",     k->data.float_val);   break;
                default:         w = 4; break;
                }
                if (w > max_key_w) max_key_w = w;
            }

            /* Print: first entry on same line as #{, rest indented to align */
            printf("#{");
            for (int i = 0; i < count; i++) {
                if (i > 0) {
                    printf("\n  ");
                }
                RuntimeValue *k = entries[i]->key;
                int w = 0;
                switch (k->type) {
                case RT_STRING:  w = printf("\"%s\"", k->data.string_val);  break;
                case RT_KEYWORD: w = printf("%s",     k->data.keyword_val); break;
                case RT_SYMBOL:  w = printf("%s",     k->data.symbol_val);  break;
                case RT_INT:     w = printf("%ld",    k->data.int_val);     break;
                case RT_FLOAT:   w = printf("%g",     k->data.float_val);   break;
                default:
                    rt_print_value_indent(k, indent);
                    w = 4;
                    break;
                }
                /* Pad key to max_key_w for value alignment */
                for (int s = w; s < max_key_w; s++) printf(" ");
                printf(" ");
                rt_print_value_indent(entries[i]->val, indent + max_key_w + 3);
            }
            printf("}");
            break;
        }

        case RT_BIGNUM: {
            char *s = mpz_get_str(NULL, 10, val->data.bignum_val);
            printf("%s", s);
            free(s);
            break;
        }
        case RT_THUNK: printf("<thunk>"); break;
    }
}

void rt_print_value(RuntimeValue *val) {
    rt_print_value_indent(val, 0);
}

void rt_print_list_unbounded(RuntimeList *list) {
    rt_interrupted = 0;
    rt_print_list_indent(list, 0);
    printf("\n");
}

void rt_print_list(RuntimeList *list) {
    rt_print_list_unbounded(list);
}

void rt_print_expanded(RuntimeValue *val) {
    rt_print_value_indent(val, 0);
    printf("\n");
}

///  Memory management
//
//  With the arena, hot-path objects are reclaimed wholesale on arena_reset().
//  The free() functions below only need to handle heap-allocated data that
//  lives OUTSIDE the arena (string contents, array element arrays, etc.).
//  They are kept for API compatibility but are mostly no-ops on the hot path.
//
void rt_thunk_free(RuntimeThunk *thunk) {
    // Arena-allocated — no-op. Kept for API compatibility.
    (void)thunk;
}

void rt_value_free(RuntimeValue *val) {
    if (!val) return;
    // Only free the heap-allocated payload inside the value.
    // The RuntimeValue struct itself is arena memory — do NOT free(val).
    switch (val->type) {
        case RT_STRING:  free(val->data.string_val);  break;
        case RT_SYMBOL:  free(val->data.symbol_val);  break;
        case RT_KEYWORD: free(val->data.keyword_val); break;
        case RT_ARRAY:
            if (val->data.array_val.elements) {
                for (size_t i = 0; i < val->data.array_val.length; i++)
                    rt_value_free(val->data.array_val.elements[i]);
                free(val->data.array_val.elements);
            }
            break;
        case RT_MAP:
            rt_map_free(val->data.map_val);
            break;
        case RT_BIGNUM:
            mpz_clear(val->data.bignum_val);
            free(val);  // bignum is always heap-allocated
            break;
        default: break;
    }
    // Do NOT free(val) — arena owns the struct
}

void rt_list_free(RuntimeList *list) {
    // Arena-allocated — no-op. Kept for API compatibility.
    (void)list;
}

/// Bignum

RuntimeValue *rt_value_bignum_from_i64(int64_t n) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_BIGNUM;
    mpz_init_set_si(v->data.bignum_val, n);
    return v;
}

RuntimeValue *rt_value_bignum_from_str(const char *s) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_BIGNUM;
    mpz_init_set_str(v->data.bignum_val, s, 10);
    return v;
}

static inline RuntimeValue *rt_int_add_safe(RuntimeValue *a, RuntimeValue *b) {
    // both RT_INT: check overflow via __builtin_add_overflow
    int64_t result;
    if (!__builtin_add_overflow(a->data.int_val, b->data.int_val, &result))
        return rt_value_int(result);
    // overflow — promote both to bignum and add
    RuntimeValue *ba = rt_value_bignum_from_i64(a->data.int_val);
    RuntimeValue *bb = rt_value_bignum_from_i64(b->data.int_val);
    return rt_bignum_add(ba, bb);
}

RuntimeValue *rt_bignum_add(RuntimeValue *a, RuntimeValue *b) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_BIGNUM;
    mpz_init(v->data.bignum_val);
    mpz_add(v->data.bignum_val, a->data.bignum_val, b->data.bignum_val);
    return v;
}

/// Higher-order list operations

typedef struct { RuntimeList *rest; RT_UnaryFn fn; void *env; } LazyMapEnv;
static RuntimeValue *_rt_lazy_map_tail_fn(void *e);

RuntimeList *rt_list_map(RuntimeList *list, void *env, RT_UnaryFn fn) {
    if (rt_list_is_empty_list(list)) return rt_list_empty();

    ConsCell *c    = heap_cons_cell();
    c->head_fn     = NULL;
    c->head_env    = NULL;
    { RuntimeValue *_a[] = {rt_list_car(list)}; c->head_val = fn(env, 1, _a); }
    c->head_forced = 1;

    LazyMapEnv *lenv = malloc(sizeof(LazyMapEnv));
    lenv->rest = rt_list_cdr(list);
    lenv->fn   = fn;
    lenv->env  = env;
    c->tail_fn     = _rt_lazy_map_tail_fn;
    c->tail_env    = lenv;
    c->tail_val    = NULL;
    c->tail_forced = 0;

    RuntimeList *lst = heap_list_wrapper();
    lst->cell = c;
    return lst;
}

static RuntimeValue *_rt_lazy_map_tail_fn(void *e) {
    LazyMapEnv *env = (LazyMapEnv *)e;
    RuntimeValue *rv = malloc(sizeof(RuntimeValue));
    rv->type          = RT_LIST;
    rv->data.list_val = rt_list_map(env->rest, env->env, env->fn);
    return rv;
}

RuntimeValue *rt_list_foldl(RuntimeList *list, RuntimeValue *init,
                             void *env, RT_BinaryFn fn) {
    RuntimeValue *acc = init;
    RuntimeList  *cur = list;
    while (!rt_list_is_empty_list(cur)) {
        { RuntimeValue *_a[] = {acc, rt_list_car(cur)}; acc = fn(env, 2, _a); }
        cur = rt_list_cdr(cur);
    }
    return acc;
}

RuntimeValue *rt_list_foldr(RuntimeList *list, RuntimeValue *init,
                             void *env, RT_BinaryFn fn) {
    RuntimeValue **stack = NULL;
    size_t count = 0, cap = 0;
    RuntimeList *cur = list;
    while (!rt_list_is_empty_list(cur)) {
        if (count >= cap) {
            cap = cap ? cap * 2 : 16;
            stack = realloc(stack, cap * sizeof(RuntimeValue *));
        }
        stack[count++] = rt_list_car(cur);
        cur = rt_list_cdr(cur);
    }
    RuntimeValue *acc = init;
    for (size_t i = count; i-- > 0; )
        { RuntimeValue *_a[] = {stack[i], acc}; acc = fn(env, 2, _a); }
    free(stack);
    return acc;
}

typedef struct { RuntimeList *rest; RT_UnaryFn fn; void *env; } LazyFilterEnv;
static RuntimeValue *_rt_lazy_filter_tail_fn(void *e);

RuntimeList *rt_list_filter(RuntimeList *list, void *env, RT_UnaryFn pred) {
    RuntimeList *cur = list;
    while (!rt_list_is_empty_list(cur)) {
        RuntimeValue *val    = rt_list_car(cur);
        RuntimeValue *_pa[] = {val}; RuntimeValue *result = pred(env, 1, _pa);
        if (rt_unbox_int(result) != 0) {
            ConsCell *c    = heap_cons_cell();
            c->head_fn     = NULL;
            c->head_env    = NULL;
            c->head_val    = val;
            c->head_forced = 1;

            LazyFilterEnv *fenv = malloc(sizeof(LazyFilterEnv));
            fenv->rest = rt_list_cdr(cur);
            fenv->fn   = pred;
            fenv->env  = env;
            c->tail_fn     = _rt_lazy_filter_tail_fn;
            c->tail_env    = fenv;
            c->tail_val    = NULL;
            c->tail_forced = 0;

            RuntimeList *lst = heap_list_wrapper();
            lst->cell = c;
            return lst;
        }
        cur = rt_list_cdr(cur);
    }
    return rt_list_empty();
}

static RuntimeValue *_rt_lazy_filter_tail_fn(void *e) {
    LazyFilterEnv *fenv = (LazyFilterEnv *)e;
    RuntimeValue *rv = malloc(sizeof(RuntimeValue));
    rv->type          = RT_LIST;
    rv->data.list_val = rt_list_filter(fenv->rest, fenv->env, fenv->fn);
    return rv;
}

RuntimeList *rt_list_zip(RuntimeList *a, RuntimeList *b) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    RuntimeList *ca = a, *cb = b;
    while (!rt_list_is_empty_list(ca) && !rt_list_is_empty_list(cb)) {
        // each element is a 2-element list: (a_i b_i)
        RuntimeList *pair = heap_list_wrapper();
        pair->cell = NULL;
        rt_list_append(pair, rt_list_car(ca));
        rt_list_append(pair, rt_list_car(cb));
        rt_list_append(out, rt_value_list(pair));
        ca = rt_list_cdr(ca);
        cb = rt_list_cdr(cb);
    }
    return out;
}

RuntimeList *rt_list_zipwith(RuntimeList *a, RuntimeList *b,
                              void *env, RT_BinaryFn fn) {
    RuntimeList *out = heap_list_wrapper();
    out->cell = NULL;
    RuntimeList *ca = a, *cb = b;
    while (!rt_list_is_empty_list(ca) && !rt_list_is_empty_list(cb)) {
        RuntimeValue *_a[] = {rt_list_car(ca), rt_list_car(cb)};
        RuntimeValue *val = fn(env, 2, _a);
        rt_list_append(out, val);
        ca = rt_list_cdr(ca);
        cb = rt_list_cdr(cb);
    }
    return out;
}


///  Ratio / Array
// (heap-allocated — long-lived, not on hot path)

static int64_t gcd(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = b; b = a % b; a = t; }
    return a;
}

RuntimeValue *rt_value_ratio(int64_t n, int64_t d) {
    if (d == 0) { fprintf(stderr, "Error: division by zero\n"); exit(1); }
    if (d < 0)  { n = -n; d = -d; }
    int64_t g = gcd(n, d);
    if (g > 1) { n /= g; d /= g; }
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_RATIO;
    v->data.ratio_val.numerator   = n;
    v->data.ratio_val.denominator = d;
    return v;
}

RuntimeValue *rt_value_array(size_t length) {
    RuntimeValue *v = malloc(sizeof(RuntimeValue));
    v->type = RT_ARRAY;
    v->data.array_val.length   = length;
    v->data.array_val.elements = calloc(length, sizeof(RuntimeValue *));
    return v;
}

void rt_array_set(RuntimeValue *array, size_t index, RuntimeValue *value) {
    if (!array || array->type != RT_ARRAY) return;
    if (index >= array->data.array_val.length) {
        fprintf(stderr, "Error: array index out of bounds\n"); exit(1);
    }
    array->data.array_val.elements[index] = value;
}

RuntimeValue *rt_array_get(RuntimeValue *array, size_t index) {
    if (!array || array->type != RT_ARRAY) return rt_value_nil();
    if (index >= array->data.array_val.length) return rt_value_nil();
    RuntimeValue *v = array->data.array_val.elements[index];
    return v ? v : rt_value_nil();
}

int64_t rt_array_length(RuntimeValue *array) {
    if (!array || array->type != RT_ARRAY) return 0;
    return (int64_t)array->data.array_val.length;
}

RuntimeValue *rt_ratio_add(RuntimeValue *a, RuntimeValue *b) {
    int64_t n = a->data.ratio_val.numerator   * b->data.ratio_val.denominator
              + b->data.ratio_val.numerator   * a->data.ratio_val.denominator;
    int64_t d = a->data.ratio_val.denominator * b->data.ratio_val.denominator;
    return rt_value_ratio(n, d);
}
RuntimeValue *rt_ratio_sub(RuntimeValue *a, RuntimeValue *b) {
    int64_t n = a->data.ratio_val.numerator   * b->data.ratio_val.denominator
              - b->data.ratio_val.numerator   * a->data.ratio_val.denominator;
    int64_t d = a->data.ratio_val.denominator * b->data.ratio_val.denominator;
    return rt_value_ratio(n, d);
}
RuntimeValue *rt_ratio_mul(RuntimeValue *a, RuntimeValue *b) {
    return rt_value_ratio(a->data.ratio_val.numerator   * b->data.ratio_val.numerator,
                          a->data.ratio_val.denominator * b->data.ratio_val.denominator);
}
RuntimeValue *rt_ratio_div(RuntimeValue *a, RuntimeValue *b) {
    return rt_value_ratio(a->data.ratio_val.numerator   * b->data.ratio_val.denominator,
                          a->data.ratio_val.denominator * b->data.ratio_val.numerator);
}
int64_t rt_ratio_to_int(RuntimeValue *r) {
    return r->data.ratio_val.numerator / r->data.ratio_val.denominator;
}
double rt_ratio_to_float(RuntimeValue *r) {
    return (double)r->data.ratio_val.numerator / (double)r->data.ratio_val.denominator;
}

//  Assert failure handler
__attribute__((weak))
void __monad_assert_fail(const char *label) {
    fprintf(stderr, "\x1b[31;1mAssertion failed:\x1b[0m %s\n", label);
    abort();
}


// Called directly from JIT code — builds a RuntimeValue* list from an AST*
// entirely in C so all allocations use heap (survive arena reset).

#define HEAP_VAL()      ((RuntimeValue*)malloc(sizeof(RuntimeValue)))
#define HEAP_LIST()     ({ RuntimeList *_l = malloc(sizeof(RuntimeList)); _l->cell = NULL; _l; })
#define WRAP_LIST(lst)  ({ RuntimeValue *_v = HEAP_VAL(); _v->type = RT_LIST; _v->data.list_val = (lst); _v; })
#define HEAP_SYM(s)     ({ RuntimeValue *_v = HEAP_VAL(); _v->type = RT_SYMBOL; _v->data.symbol_val = strdup(s); _v; })

static void heap_list_append(RuntimeList *list, RuntimeValue *value) {
    ConsCell *new_c    = malloc(sizeof(ConsCell));
    new_c->head_fn     = NULL;
    new_c->head_env    = NULL;
    new_c->head_val    = value;
    new_c->head_forced = 1;
    new_c->tail_fn     = NULL;
    new_c->tail_env    = NULL;
    new_c->tail_val    = NULL;
    new_c->tail_forced = 1;

    if (list->cell == NULL) {
        list->cell = new_c;
        return;
    }

    RuntimeList *cur = list;
    while (1) {
        ConsCell *cc = cur->cell;
        if (!cc) break;
        RuntimeValue *tv = cc->tail_val;
        if (!tv || tv->type == RT_NIL ||
            (tv->type == RT_LIST && rt_list_is_empty_list(tv->data.list_val))) {
            RuntimeValue *link  = malloc(sizeof(RuntimeValue));
            link->type          = RT_LIST;
            link->data.list_val = malloc(sizeof(RuntimeList));
            link->data.list_val->cell = new_c;
            cc->tail_val    = link;
            cc->tail_forced = 1;
            return;
        }
        if (tv->type == RT_LIST) { cur = tv->data.list_val; continue; }
        break;
    }
}

RuntimeValue *rt_ast_to_runtime_value(AST *ast) {
    if (!ast) {
        RuntimeValue *v = HEAP_VAL();
        v->type = RT_NIL;
        return v;
    }

    switch (ast->type) {
    case AST_NUMBER: {
            bool is_float = false;
            if (ast->literal_str) {
                const char *s = ast->literal_str;
                while (*s) {
                    if (*s == '.' || *s == 'e' || *s == 'E') { is_float = true; break; }
                    s++;
                }
            } else {
                is_float = (ast->number != (double)(int64_t)ast->number);
            }
            RuntimeValue *v = HEAP_VAL();
            if (is_float) {
                v->type = RT_FLOAT;
                v->data.float_val = ast->number;
            } else {
                v->type = RT_INT;
                v->data.int_val = (int64_t)ast->number;
            }
            return v;
        }
        case AST_SYMBOL:  return HEAP_SYM(ast->symbol);
        case AST_STRING: {
            RuntimeValue *v = HEAP_VAL();
            v->type = RT_STRING;
            v->data.string_val = strdup(ast->string);
            return v;
        }
        case AST_CHAR: {
            RuntimeValue *v = HEAP_VAL();
            v->type = RT_CHAR;
            v->data.char_val = ast->character;
            return v;
        }
        case AST_KEYWORD: {
            RuntimeValue *v = HEAP_VAL();
            v->type = RT_KEYWORD;
            v->data.keyword_val = strdup(ast->keyword);
            return v;
        }
        case AST_RATIO: {
            RuntimeValue *v = HEAP_VAL();
            v->type = RT_RATIO;
            v->data.ratio_val.numerator   = ast->ratio.numerator;
            v->data.ratio_val.denominator = ast->ratio.denominator;
            return v;
        }
        case AST_LIST: {
            RuntimeList *lst = HEAP_LIST();
            for (size_t i = 0; i < ast->list.count; i++)
                heap_list_append(lst, rt_ast_to_runtime_value(ast->list.items[i]));
            return WRAP_LIST(lst);
        }
        case AST_ARRAY: {
            RuntimeList *lst = HEAP_LIST();
            for (size_t i = 0; i < ast->array.element_count; i++)
                heap_list_append(lst, rt_ast_to_runtime_value(ast->array.elements[i]));
            return WRAP_LIST(lst);
        }
        case AST_LAMBDA: {
            RuntimeList *lst = HEAP_LIST();
            heap_list_append(lst, HEAP_SYM("lambda"));

            // params sublist
            RuntimeList *params = HEAP_LIST();
            for (int i = 0; i < ast->lambda.param_count; i++)
                heap_list_append(params, HEAP_SYM(ast->lambda.params[i].name));
            heap_list_append(lst, WRAP_LIST(params));

            // body exprs
            for (int i = 0; i < ast->lambda.body_count; i++)
                heap_list_append(lst, rt_ast_to_runtime_value(ast->lambda.body_exprs[i]));
            return WRAP_LIST(lst);
        }

        case AST_LAYOUT: {
            RuntimeList *lst = HEAP_LIST();
            heap_list_append(lst, HEAP_SYM("layout"));
            heap_list_append(lst, HEAP_SYM(ast->layout.name));
            for (int i = 0; i < ast->layout.field_count; i++) {
                ASTLayoutField *f = &ast->layout.fields[i];
                RuntimeList *field = HEAP_LIST();
                heap_list_append(field, HEAP_SYM(f->name));
                heap_list_append(field, HEAP_SYM("::"));
                if (f->is_array) {
                    RuntimeList *arr = HEAP_LIST();
                    heap_list_append(arr, HEAP_SYM(f->array_elem ? f->array_elem : "?"));
                    char size_buf[32];
                    snprintf(size_buf, sizeof(size_buf), "%d", f->array_size);
                    heap_list_append(arr, HEAP_SYM(size_buf));
                    heap_list_append(field, WRAP_LIST(arr));
                } else {
                    heap_list_append(field, HEAP_SYM(f->type_name ? f->type_name : "?"));
                }
                heap_list_append(lst, WRAP_LIST(field));
            }
            if (ast->layout.packed) {
                heap_list_append(lst, HEAP_SYM(":packed"));
                heap_list_append(lst, HEAP_SYM("True"));
            }
            if (ast->layout.align) {
                char align_buf[32];
                snprintf(align_buf, sizeof(align_buf), "%d", ast->layout.align);
                heap_list_append(lst, HEAP_SYM(":align"));
                heap_list_append(lst, HEAP_SYM(align_buf));
            }
            return WRAP_LIST(lst);
        }

        case AST_PMATCH: {
            // Represent pmatch as a list of (pattern -> body) pairs
            RuntimeList *lst = HEAP_LIST();
            for (int i = 0; i < ast->pmatch.clause_count; i++) {
                ASTPMatchClause *cl = &ast->pmatch.clauses[i];
                RuntimeList *clause = HEAP_LIST();
                // patterns
                for (int j = 0; j < cl->pattern_count; j++) {
                    ASTPattern *pat = &cl->patterns[j];
                    RuntimeValue *pv = HEAP_VAL();
                    pv->type = RT_SYMBOL;
                    switch (pat->kind) {
                    case PAT_WILDCARD: pv->data.symbol_val = strdup("_"); break;
                    case PAT_VAR:      pv->data.symbol_val = strdup(pat->var_name); break;
                    case PAT_LITERAL_INT: {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%lld", (long long)pat->lit_value);
                        pv->data.symbol_val = strdup(buf);
                        break;
                    }
                    case PAT_LIST_EMPTY:
                        pv->data.symbol_val = strdup("[]");
                        break;
                    default:
                        pv->data.symbol_val = strdup("_");
                        break;
                    }
                    heap_list_append(clause, pv);
                }
                heap_list_append(clause, HEAP_SYM("->"));
                heap_list_append(clause, rt_ast_to_runtime_value(cl->body));
                heap_list_append(lst, WRAP_LIST(clause));
            }
            return WRAP_LIST(lst);
        }

        default: {
            return HEAP_SYM("<unknown>");
        }
    }
}

#undef HEAP_VAL
#undef HEAP_LIST
#undef WRAP_LIST
#undef HEAP_SYM

///  LLVM Integration — declare_runtime_functions

void declare_runtime_functions(CodegenContext *ctx) {
    LLVMTypeRef ptr    = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
    LLVMTypeRef i64    = LLVMInt64TypeInContext(ctx->context);
    LLVMTypeRef i32    = LLVMInt32TypeInContext(ctx->context);
    LLVMTypeRef i8     = LLVMInt8TypeInContext(ctx->context);
    LLVMTypeRef dbl    = LLVMDoubleTypeInContext(ctx->context);
    LLVMTypeRef void_t = LLVMVoidTypeInContext(ctx->context);

#define DECL(name, ret, ...) \
    do { \
        LLVMTypeRef _p[] = { __VA_ARGS__ }; \
        int _n = sizeof(_p)/sizeof(_p[0]); \
        LLVMAddFunction(ctx->module, name, LLVMFunctionType(ret, _n ? _p : NULL, _n, 0)); \
    } while(0)
#define DECL0(name, ret) \
    LLVMAddFunction(ctx->module, name, LLVMFunctionType(ret, NULL, 0, 0))

    DECL("rt_ast_to_runtime_value", ptr, ptr);

    // --- Closure ---
    DECL("rt_value_closure", ptr, ptr, ptr, i32, i32);  // fn_ptr, env, env_size, arity
    DECL("rt_closure_calln", ptr, ptr, i32, ptr);       // closure, n, args_array

    // --- Thunks ---
    DECL("rt_thunk_of_value", ptr, ptr);
    DECL("rt_thunk_create", ptr, ptr, ptr);
    DECL("rt_force", ptr, ptr);

    // --- List constructors ---
    DECL0("rt_list_new", ptr);
    DECL0("rt_list_empty", ptr);
    DECL("rt_list_lazy_cons", ptr, ptr, ptr);
    DECL("rt_list_cons", ptr, ptr, ptr);
    DECL("rt_list_is_empty_list", i32, ptr);

    // --- List accessors ---
    DECL("rt_list_car",    ptr, ptr);
    DECL("rt_list_cdr",    ptr, ptr);
    DECL("rt_list_nth",    ptr, ptr, i64);
    DECL("rt_list_length", i64, ptr);

    // --- List mutation/construction ---
    DECL("rt_list_append",       void_t, ptr, ptr);
    DECL("rt_list_append_lists", ptr, ptr, ptr);
    DECL("rt_list_copy",         ptr, ptr);
    DECL("rt_make_list",         ptr, i64, ptr);

    // --- Range / infinite lists ---
    DECL("rt_list_range",     ptr, i64, i64);
    DECL("rt_list_from",      ptr, i64);
    DECL("rt_list_from_step", ptr, i64, i64);
    DECL("rt_list_take",      ptr, ptr, i64);
    DECL("rt_list_drop",      ptr, ptr, i64);

    // --- Higher-order list ops ---
    DECL("rt_list_map",     ptr, ptr, ptr, ptr);        /* list, env, fn */
    DECL("rt_list_foldl",   ptr, ptr, ptr, ptr, ptr);   /* list, init, env, fn */
    DECL("rt_list_foldr",   ptr, ptr, ptr, ptr, ptr);   /* list, init, env, fn */
    DECL("rt_list_filter",  ptr, ptr, ptr, ptr);        /* list, env, pred */
    DECL("rt_list_zipwith", ptr, ptr, ptr, ptr, ptr);   /* a, b, env, fn */
    DECL("rt_list_zip",     ptr, ptr, ptr);       // (a, b) -> list



    // --- Equality ---
    DECL("rt_equal_p", i32, ptr, ptr);

    // --- Unboxing ---
    DECL("rt_unbox_int",    i64,    ptr);
    DECL("rt_unbox_float",  dbl,    ptr);
    DECL("rt_unbox_char",   i8,     ptr);
    DECL("rt_unbox_string", ptr,    ptr);
    DECL("rt_unbox_list",   ptr,    ptr);
    DECL("rt_value_is_nil", i32,    ptr);
    DECL("rt_print_value_newline", void_t, ptr);

    // --- Value construction ---
    DECL("rt_value_int",     ptr, i64);
    DECL("rt_value_float",   ptr, dbl);
    DECL("rt_value_char",    ptr, i8);
    DECL("rt_value_string",  ptr, ptr);
    DECL("rt_value_symbol",  ptr, ptr);
    DECL("rt_value_keyword", ptr, ptr);
    DECL("rt_value_list",    ptr, ptr);
    DECL0("rt_value_nil",    ptr);
    DECL("rt_value_thunk",   ptr, ptr);

    // --- Ratio ---
    DECL("rt_value_ratio",   ptr, i64, i64);
    DECL("rt_ratio_add",     ptr, ptr, ptr);
    DECL("rt_ratio_sub",     ptr, ptr, ptr);
    DECL("rt_ratio_mul",     ptr, ptr, ptr);
    DECL("rt_ratio_div",     ptr, ptr, ptr);
    DECL("rt_ratio_to_int",  i64, ptr);
    DECL("rt_ratio_to_float", dbl, ptr);

    // --- Array ---
    DECL("rt_value_array",  ptr, i64);
    DECL("rt_array_set",    void_t, ptr, i64, ptr);
    DECL("rt_array_get",    ptr, ptr, i64);
    DECL("rt_array_length", i64, ptr);

    // --- Set ---
    DECL0("rt_set_new",        ptr);
    DECL("rt_set_of",          ptr, ptr, i64);
    DECL("rt_set_from_list",   ptr, ptr);
    DECL("rt_set_from_array",  ptr, ptr);
    DECL("rt_set_contains",    i32, ptr, ptr);
    DECL("rt_set_conj",        ptr, ptr, ptr);
    DECL("rt_set_disj",        ptr, ptr, ptr);
    DECL("rt_set_conj_mut",    ptr, ptr, ptr);
    DECL("rt_set_disj_mut",    ptr, ptr, ptr);
    DECL("rt_set_get",         ptr, ptr, ptr);
    DECL("rt_set_count",       i64, ptr);
    DECL("rt_set_seq",         ptr, ptr);
    DECL("rt_value_set",       ptr, ptr);
    DECL("rt_unbox_set",       ptr, ptr);
    DECL("rt_set_foldl",  ptr, ptr, ptr, ptr, ptr);   /* set, init, env, fn */
    DECL("rt_set_map",    ptr, ptr, ptr, ptr);         /* set, env, fn */
    DECL("rt_set_filter", ptr, ptr, ptr, ptr);         /* set, env, pred */

    // --- Map ---
    DECL0("rt_map_new",        ptr);
    DECL("rt_map_assoc",       ptr, ptr, ptr, ptr);
    DECL("rt_map_assoc_mut",   ptr, ptr, ptr, ptr);
    DECL("rt_map_dissoc",      ptr, ptr, ptr);
    DECL("rt_map_dissoc_mut",  ptr, ptr, ptr);
    DECL("rt_map_get",         ptr, ptr, ptr, ptr);
    DECL("rt_map_contains",    i32, ptr, ptr);
    DECL("rt_map_find",        ptr, ptr, ptr);
    DECL("rt_map_count",       i64, ptr);
    DECL("rt_map_keys",        ptr, ptr);
    DECL("rt_map_vals",        ptr, ptr);
    DECL("rt_map_merge",       ptr, ptr, ptr);
    DECL("rt_value_map",       ptr, ptr);
    DECL("rt_unbox_map",       ptr, ptr);

    // --- Print ---
    DECL("rt_print_value",         void_t, ptr);
    DECL("rt_print_list",          void_t, ptr);
    DECL("rt_print_list_limited",  void_t, ptr, i64);

    // --- Assert ---
    {
        LLVMTypeRef ft = LLVMFunctionType(void_t, &ptr, 1, 0);
        LLVMAddFunction(ctx->module, "__monad_assert_fail", ft);
    }

#undef DECL
#undef DECL0
}

LLVMValueRef get_rt_string_take(CodegenContext *ctx) {
    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, "rt_string_take");
    if (!fn) {
        LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx->context);
        LLVMTypeRef args[] = {ptr, i64};
        LLVMTypeRef ft = LLVMFunctionType(ptr, args, 2, 0);
        fn = LLVMAddFunction(ctx->module, "rt_string_take", ft);
    }
    return fn;
}

///  GET_RUNTIME_FUNCTION macro + definitions

#define GET_RUNTIME_FUNCTION(name) \
    LLVMValueRef get_##name(CodegenContext *ctx) { \
        LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, #name); \
        if (!fn) { fprintf(stderr, "Runtime function " #name " not found\n"); exit(1); } \
        return fn; \
    }

GET_RUNTIME_FUNCTION(rt_thunk_of_value)
GET_RUNTIME_FUNCTION(rt_thunk_create)
GET_RUNTIME_FUNCTION(rt_force)

GET_RUNTIME_FUNCTION(rt_list_new)
GET_RUNTIME_FUNCTION(rt_list_empty)
GET_RUNTIME_FUNCTION(rt_list_lazy_cons)
GET_RUNTIME_FUNCTION(rt_list_cons)
GET_RUNTIME_FUNCTION(rt_list_is_empty_list)

LLVMValueRef get_rt_list_is_empty(CodegenContext *ctx) {
    LLVMValueRef fn = LLVMGetNamedFunction(ctx->module, "rt_list_is_empty_list");
    if (!fn) { fprintf(stderr, "Runtime function rt_list_is_empty_list not found\n"); exit(1); }
    return fn;
}

GET_RUNTIME_FUNCTION(rt_ast_to_runtime_value)

GET_RUNTIME_FUNCTION(rt_value_closure)
GET_RUNTIME_FUNCTION(rt_closure_calln)

GET_RUNTIME_FUNCTION(rt_list_car)
GET_RUNTIME_FUNCTION(rt_list_cdr)
GET_RUNTIME_FUNCTION(rt_list_nth)
GET_RUNTIME_FUNCTION(rt_list_length)
GET_RUNTIME_FUNCTION(rt_list_append)
GET_RUNTIME_FUNCTION(rt_list_append_lists)
GET_RUNTIME_FUNCTION(rt_list_copy)
GET_RUNTIME_FUNCTION(rt_make_list)

GET_RUNTIME_FUNCTION(rt_list_range)
GET_RUNTIME_FUNCTION(rt_list_from)
GET_RUNTIME_FUNCTION(rt_list_from_step)
GET_RUNTIME_FUNCTION(rt_list_take)
GET_RUNTIME_FUNCTION(rt_list_drop)

GET_RUNTIME_FUNCTION(rt_equal_p)

GET_RUNTIME_FUNCTION(rt_unbox_int)
GET_RUNTIME_FUNCTION(rt_unbox_float)
GET_RUNTIME_FUNCTION(rt_unbox_char)
GET_RUNTIME_FUNCTION(rt_unbox_string)
GET_RUNTIME_FUNCTION(rt_unbox_list)
GET_RUNTIME_FUNCTION(rt_value_is_nil)
GET_RUNTIME_FUNCTION(rt_print_value_newline)

GET_RUNTIME_FUNCTION(rt_value_int)
GET_RUNTIME_FUNCTION(rt_value_float)
GET_RUNTIME_FUNCTION(rt_value_char)
GET_RUNTIME_FUNCTION(rt_value_string)
GET_RUNTIME_FUNCTION(rt_value_symbol)
GET_RUNTIME_FUNCTION(rt_value_keyword)
GET_RUNTIME_FUNCTION(rt_value_list)
GET_RUNTIME_FUNCTION(rt_value_nil)
GET_RUNTIME_FUNCTION(rt_value_thunk)

GET_RUNTIME_FUNCTION(rt_print_value)
GET_RUNTIME_FUNCTION(rt_print_list)

GET_RUNTIME_FUNCTION(rt_value_ratio)
GET_RUNTIME_FUNCTION(rt_ratio_add)
GET_RUNTIME_FUNCTION(rt_ratio_sub)
GET_RUNTIME_FUNCTION(rt_ratio_mul)
GET_RUNTIME_FUNCTION(rt_ratio_div)
GET_RUNTIME_FUNCTION(rt_ratio_to_int)
GET_RUNTIME_FUNCTION(rt_ratio_to_float)

GET_RUNTIME_FUNCTION(rt_value_array)
GET_RUNTIME_FUNCTION(rt_array_set)
GET_RUNTIME_FUNCTION(rt_array_get)
GET_RUNTIME_FUNCTION(rt_array_length)

GET_RUNTIME_FUNCTION(rt_list_map)
GET_RUNTIME_FUNCTION(rt_list_foldl)
GET_RUNTIME_FUNCTION(rt_list_foldr)
GET_RUNTIME_FUNCTION(rt_list_filter)
GET_RUNTIME_FUNCTION(rt_list_zip)
GET_RUNTIME_FUNCTION(rt_list_zipwith)

GET_RUNTIME_FUNCTION(rt_set_new)
GET_RUNTIME_FUNCTION(rt_set_of)
GET_RUNTIME_FUNCTION(rt_set_from_list)
GET_RUNTIME_FUNCTION(rt_set_from_array)
GET_RUNTIME_FUNCTION(rt_set_contains)
GET_RUNTIME_FUNCTION(rt_set_conj)
GET_RUNTIME_FUNCTION(rt_set_disj)
GET_RUNTIME_FUNCTION(rt_set_conj_mut)
GET_RUNTIME_FUNCTION(rt_set_disj_mut)
GET_RUNTIME_FUNCTION(rt_set_get)
GET_RUNTIME_FUNCTION(rt_set_count)
GET_RUNTIME_FUNCTION(rt_set_seq)
GET_RUNTIME_FUNCTION(rt_value_set)
GET_RUNTIME_FUNCTION(rt_unbox_set)
GET_RUNTIME_FUNCTION(rt_set_foldl)
GET_RUNTIME_FUNCTION(rt_set_map)
GET_RUNTIME_FUNCTION(rt_set_filter)

GET_RUNTIME_FUNCTION(rt_map_new)
GET_RUNTIME_FUNCTION(rt_map_assoc)
GET_RUNTIME_FUNCTION(rt_map_assoc_mut)
GET_RUNTIME_FUNCTION(rt_map_dissoc)
GET_RUNTIME_FUNCTION(rt_map_dissoc_mut)
GET_RUNTIME_FUNCTION(rt_map_get)
GET_RUNTIME_FUNCTION(rt_map_contains)
GET_RUNTIME_FUNCTION(rt_map_find)
GET_RUNTIME_FUNCTION(rt_map_count)
GET_RUNTIME_FUNCTION(rt_map_keys)
GET_RUNTIME_FUNCTION(rt_map_vals)
GET_RUNTIME_FUNCTION(rt_map_merge)
GET_RUNTIME_FUNCTION(rt_value_map)
GET_RUNTIME_FUNCTION(rt_unbox_map)



LLVMTypeRef get_rt_value_type(CodegenContext *ctx) {
    return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
}

LLVMTypeRef get_rt_list_type(CodegenContext *ctx) {
    return LLVMPointerType(LLVMInt8TypeInContext(ctx->context), 0);
}
