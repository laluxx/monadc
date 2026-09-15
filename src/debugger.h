/// debugger.h — Monad debugger engine public API
//
//  The debugger is deliberately split into two layers:
//
//    * DbgEngine is the reusable debugger core. It owns execution semantics,
//      logical breakpoints, target/process/thread/frame/value state, reverse
//      execution, replay, compiler-pipeline introspection and backend events.
//    * DbgSession is the terminal client. It is opaque; terminal rendering,
//      key decoding, pane state and caches are implementation details.
//
//  A backend may be a native OS debugger, an LLDB bridge, GDB-remote client,
//  deterministic replay engine, core-file reader or an in-process Monad
//  runtime/compiler bridge. The public contract is protocol-neutral and is
//  intentionally shaped so a DAP adapter can be layered on top without making
//  the terminal UI part of the debugger semantics.
//
//  Ownership: unless explicitly documented as borrowed, strings/arrays written
//  through output parameters are malloc/free compatible and released by the
//  matching dbg_*_free helper. Backend callbacks follow the same convention.
//
//  Concurrency: DbgEngine is an event-loop object, not a concurrently mutable
//  object. Call its API from one controlling thread. Backends may use worker
//  threads internally, but must serialize externally visible changes through
//  poll_event/event_fd and must not re-enter the same DbgEngine from callbacks.
//  The DbgBackendOps table itself is borrowed and must outlive the engine; the
//  backend context is destroyed only when take_backend_ownership is true.

#ifndef MONAD_DEBUGGER_H
#define MONAD_DEBUGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define DBG_PRINTF_LIKE(fmt_index, first_arg) \
       __attribute__((format(printf, fmt_index, first_arg)))
#else
#  define DBG_PRINTF_LIKE(fmt_index, first_arg)
#endif

#define DBG_BACKEND_ABI_VERSION 2u
#define DBG_INVALID_ID          UINT64_MAX
#define DBG_INVALID_ADDRESS     UINT64_MAX

/* Opaque ownership boundaries. */
typedef struct DbgEngine        DbgEngine;
typedef struct DbgSession       DbgSession;
typedef struct DbgCodeMap       DbgCodeMap;
typedef struct DbgErrorSnapshot DbgErrorSnapshot;
typedef struct DbgBreakpoint    DbgBreakpoint;
typedef struct DbgWatch         DbgWatch;
typedef struct DbgBackendOps    DbgBackendOps;
typedef struct DbgConfig        DbgConfig;

/// Capability negotiation

typedef uint64_t DbgCapabilities;
#define DBG_CAP_LAUNCH                  (UINT64_C(1) << 0)
#define DBG_CAP_ATTACH                  (UINT64_C(1) << 1)
#define DBG_CAP_RESTART                 (UINT64_C(1) << 2)
#define DBG_CAP_TERMINATE               (UINT64_C(1) << 3)
#define DBG_CAP_DETACH                  (UINT64_C(1) << 4)
#define DBG_CAP_EXECUTION_CONTROL       (UINT64_C(1) << 5)
#define DBG_CAP_PAUSE                   (UINT64_C(1) << 6)
#define DBG_CAP_NON_STOP                (UINT64_C(1) << 7)
#define DBG_CAP_SINGLE_THREAD_EXEC      (UINT64_C(1) << 8)
#define DBG_CAP_SOURCE_BREAKPOINTS      (UINT64_C(1) << 9)
#define DBG_CAP_FUNCTION_BREAKPOINTS    (UINT64_C(1) << 10)
#define DBG_CAP_INSTRUCTION_BREAKPOINTS (UINT64_C(1) << 11)
#define DBG_CAP_DATA_BREAKPOINTS        (UINT64_C(1) << 12)
#define DBG_CAP_EXCEPTION_BREAKPOINTS   (UINT64_C(1) << 13)
#define DBG_CAP_BREAKPOINT_LOCATIONS    (UINT64_C(1) << 14)
#define DBG_CAP_CONDITIONAL_BREAKPOINTS (UINT64_C(1) << 15)
#define DBG_CAP_HIT_CONDITIONS          (UINT64_C(1) << 16)
#define DBG_CAP_LOGPOINTS               (UINT64_C(1) << 17)
#define DBG_CAP_EVALUATE                (UINT64_C(1) << 18)
#define DBG_CAP_SET_EXPRESSION          (UINT64_C(1) << 19)
#define DBG_CAP_SET_VARIABLE            (UINT64_C(1) << 20)
#define DBG_CAP_VARIABLE_PAGING         (UINT64_C(1) << 21)
#define DBG_CAP_READ_MEMORY             (UINT64_C(1) << 22)
#define DBG_CAP_WRITE_MEMORY            (UINT64_C(1) << 23)
#define DBG_CAP_DISASSEMBLE             (UINT64_C(1) << 24)
#define DBG_CAP_REGISTERS               (UINT64_C(1) << 25)
#define DBG_CAP_MODULES                 (UINT64_C(1) << 26)
#define DBG_CAP_CORE_DUMP               (UINT64_C(1) << 27)
#define DBG_CAP_REMOTE                  (UINT64_C(1) << 28)
#define DBG_CAP_REVERSE_CONTINUE        (UINT64_C(1) << 29)
#define DBG_CAP_STEP_BACK               (UINT64_C(1) << 30)
#define DBG_CAP_REPLAY                  (UINT64_C(1) << 31)
#define DBG_CAP_CHECKPOINTS             (UINT64_C(1) << 32)
#define DBG_CAP_LOGICAL_TASKS           (UINT64_C(1) << 33)
#define DBG_CAP_ASYNC_STACKS            (UINT64_C(1) << 34)
#define DBG_CAP_COMPILER_PIPELINE       (UINT64_C(1) << 35)
#define DBG_CAP_RESTART_FRAME           (UINT64_C(1) << 36)
#define DBG_CAP_STEP_IN_TARGETS         (UINT64_C(1) << 37)
#define DBG_CAP_GOTO                    (UINT64_C(1) << 38)
#define DBG_CAP_TARGET_INFO             (UINT64_C(1) << 39)
#define DBG_CAP_INVALIDATED_EVENTS      (UINT64_C(1) << 40)
#define DBG_CAP_VALUE_FORMATTING        (UINT64_C(1) << 41)
#define DBG_CAP_COMPLETIONS             (UINT64_C(1) << 42)
#define DBG_CAP_LOADED_SOURCES          (UINT64_C(1) << 43)
#define DBG_CAP_SOURCE_CONTENT          (UINT64_C(1) << 44)
#define DBG_CAP_QUERY_WHILE_RUNNING     (UINT64_C(1) << 45)

/* Compatibility spellings retained for code written against ABI v1. */
#define DBG_CAP_INSTRUCTION_BREAKS   DBG_CAP_INSTRUCTION_BREAKPOINTS
#define DBG_CAP_CONDITIONAL_BREAKS   DBG_CAP_CONDITIONAL_BREAKPOINTS

/// Target, execution and location model

typedef enum {
    DBG_BYTE_ORDER_UNKNOWN = 0,
    DBG_BYTE_ORDER_LITTLE,
    DBG_BYTE_ORDER_BIG,
} DbgByteOrder;

typedef struct {
    uint64_t id;
    char *name;
    char *executable;
    char *architecture;
    char *triple;
    char *os_abi;
    uint8_t address_size;
    DbgByteOrder byte_order;
} DbgTargetInfo;

typedef enum {
    DBG_PROCESS_NONE = 0,
    DBG_PROCESS_LAUNCHING,
    DBG_PROCESS_ATTACHING,
    DBG_PROCESS_STOPPED,
    DBG_PROCESS_PARTIALLY_STOPPED, /* non-stop: some threads still run */
    DBG_PROCESS_RUNNING,
    DBG_PROCESS_EXITED,
    DBG_PROCESS_DETACHED,
    DBG_PROCESS_CRASHED,
} DbgProcessState;

typedef enum {
    DBG_THREAD_UNKNOWN = 0,
    DBG_THREAD_RUNNING,
    DBG_THREAD_STOPPED,
    DBG_THREAD_EXITED,
} DbgThreadState;

typedef enum {
    DBG_STOP_NONE = 0,
    DBG_STOP_ENTRY,
    DBG_STOP_BREAKPOINT,
    DBG_STOP_DATA_BREAKPOINT,
    DBG_STOP_STEP,
    DBG_STOP_PAUSE,
    DBG_STOP_SIGNAL,
    DBG_STOP_EXCEPTION,
    DBG_STOP_GOTO,
    DBG_STOP_REPLAY,
    DBG_STOP_COMPILER,
    DBG_STOP_INTERNAL,
} DbgStopReason;

typedef struct {
    char *file;
    char *checksum;       /* optional hex digest supplied by debug info */
    uint64_t source_id;   /* 0 when the source is path-backed */
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} DbgSourceLocation;

typedef struct {
    DbgSourceLocation source;
    char *function;
    char *ir_function;
    char *ir_value;
    uint32_t ir_instruction;
    uint32_t inline_depth;
    uint64_t compiler_stage_id;
    uint64_t semantic_node_id;
    uint64_t address;
    uint64_t address_end; /* exclusive; INVALID/0 means one instruction */
} DbgCodeLocation;

typedef struct {
    const char *program;      /* borrowed for duration of callback */
    const char *cwd;
    const char *const *argv;
    size_t argc;
    const char *const *env;
    size_t env_count;
    bool stop_on_entry;
    bool disable_aslr;
    bool record;
} DbgLaunchSpec;

typedef struct {
    uint64_t pid;
    bool stop_on_attach;
} DbgAttachSpec;

typedef struct {
    const char *program;
    const char *core_path;
} DbgCoreSpec;

typedef struct {
    const char *url;
    const char *program;
    bool extended_mode;
} DbgRemoteSpec;

typedef enum { DBG_DIR_FORWARD = 0, DBG_DIR_REVERSE } DbgExecutionDirection;
typedef enum { DBG_RUN_ALL_THREADS = 0, DBG_RUN_SINGLE_THREAD } DbgRunMode;

typedef enum {
    DBG_STEP_INTO = 0,
    DBG_STEP_OVER,
    DBG_STEP_OUT,
} DbgStepAction;

typedef enum {
    DBG_GRANULARITY_STATEMENT = 0,
    DBG_GRANULARITY_LINE,
    DBG_GRANULARITY_EXPRESSION,     /* Monad semantic expression */
    DBG_GRANULARITY_IR_INSTRUCTION,
    DBG_GRANULARITY_INSTRUCTION,
} DbgStepGranularity;

typedef struct {
    uint64_t thread_id;
    uint64_t frame_id;
    DbgStepAction action;
    DbgStepGranularity granularity;
    DbgRunMode run_mode;
    DbgExecutionDirection direction;
    uint64_t step_in_target_id; /* 0 = backend chooses */
    bool skip_no_debug;
} DbgStepPlan;

typedef struct {
    uint64_t id;
    char *label;
    DbgCodeLocation location;
} DbgStepInTarget;

typedef struct {
    uint64_t id;
    char *label;
    DbgCodeLocation location;
} DbgGotoTarget;

typedef struct {
    uint64_t process_id;
    uint64_t id;
    char *name;
    DbgThreadState state;
    DbgStopReason stop_reason;
    char *stop_description;
    bool selected;
} DbgThreadInfo;

typedef struct {
    uint64_t id;
    uint64_t thread_id;
    uint64_t snapshot_epoch;
    char *name;
    DbgCodeLocation location;
    uint64_t frame_pointer;
    uint64_t stack_pointer;
    bool is_inline;
    bool is_async;
    uint64_t async_parent_frame_id;
    bool can_restart;
} DbgStackFrameInfo;

/// Scopes, values and expression evaluation

typedef struct {
    uint64_t id;
    uint64_t frame_id;
    uint64_t snapshot_epoch;
    char *name;
    char *presentation_hint;
    bool expensive;
    uint64_t variables_reference;
    size_t named_children;
    size_t indexed_children;
} DbgScopeInfo;

typedef enum {
    DBG_VALUE_AVAILABLE = 0,
    DBG_VALUE_OPTIMIZED_OUT,
    DBG_VALUE_UNINITIALIZED,
    DBG_VALUE_NOT_IN_SCOPE,
    DBG_VALUE_UNREADABLE,
} DbgValueAvailability;

typedef enum {
    DBG_STORAGE_UNKNOWN = 0,
    DBG_STORAGE_REGISTER,
    DBG_STORAGE_MEMORY,
    DBG_STORAGE_CONSTANT,
    DBG_STORAGE_COMPOSITE,
} DbgValueStorage;

typedef enum {
    DBG_FORMAT_NATURAL = 0,
    DBG_FORMAT_HEX,
    DBG_FORMAT_DECIMAL,
    DBG_FORMAT_UNSIGNED,
    DBG_FORMAT_BINARY,
    DBG_FORMAT_CHARACTER,
    DBG_FORMAT_STRING,
    DBG_FORMAT_RAW,
} DbgValueFormat;

typedef uint32_t DbgValuePresentation;
#define DBG_VALUE_PRESENTATION_LAZY                (UINT32_C(1) << 0)
#define DBG_VALUE_PRESENTATION_SYNTHETIC           (UINT32_C(1) << 1)
#define DBG_VALUE_PRESENTATION_DYNAMIC             (UINT32_C(1) << 2)
#define DBG_VALUE_PRESENTATION_READ_ONLY           (UINT32_C(1) << 3)
#define DBG_VALUE_PRESENTATION_HAS_DATA_BREAKPOINT (UINT32_C(1) << 4)
#define DBG_VALUE_PRESENTATION_RETURN_VALUE        (UINT32_C(1) << 5)
#define DBG_VALUE_PRESENTATION_INTERNAL            (UINT32_C(1) << 6)

typedef struct {
    uint64_t id;
    uint64_t snapshot_epoch;
    char *name;
    char *type_name;
    char *value;
    char *summary;
    char *evaluate_name;
    char *memory_reference; /* opaque backend reference, optional */
    DbgSourceLocation declaration;
    DbgValueAvailability availability;
    DbgValueStorage storage;
    DbgValuePresentation presentation;
    DbgValueFormat format;
    uint64_t address;
    size_t byte_size;
    uint64_t variables_reference;
    size_t named_children;
    size_t indexed_children;
    bool writable;
    bool changed;
} DbgValueInfo;

typedef enum {
    DBG_EVAL_REPL = 0,
    DBG_EVAL_WATCH,
    DBG_EVAL_HOVER,
    DBG_EVAL_CONDITION,
    DBG_EVAL_VARIABLES,
    DBG_EVAL_CLIPBOARD,
} DbgEvaluateContext;

typedef struct {
    DbgEvaluateContext context;
    DbgValueFormat format;
    bool allow_side_effects;
    bool allow_function_calls;
    bool prefer_dynamic;
    bool prefer_synthetic;
} DbgEvaluationOptions;

typedef struct {
    char *label;
    char *text;
    char *type;
    char *detail;
    uint32_t replace_start;
    uint32_t replace_length;
} DbgCompletionItem;

/// Modules, registers, disassembly, tasks

typedef struct {
    uint64_t id;
    char *name;
    char *path;
    char *uuid;
    uint64_t base_address;
    uint64_t size;
    bool symbols_loaded;
} DbgModuleInfo;

typedef struct {
    uint64_t source_id;
    char *name;
    char *path;
    char *origin;
    char *checksum;
    char *mime_type;
} DbgSourceInfo;

typedef struct {
    uint64_t id;
    char *name;
    char *value;
    uint64_t raw_value;
    uint16_t bit_width;
    bool is_pc, is_sp, is_fp;
} DbgRegisterInfo;

typedef struct {
    uint64_t address;
    char *bytes_hex;
    char *mnemonic;
    char *operands;
    DbgCodeLocation location;
} DbgInstructionInfo;

typedef enum {
    DBG_TASK_UNKNOWN = 0,
    DBG_TASK_RUNNING,
    DBG_TASK_SUSPENDED,
    DBG_TASK_WAITING,
    DBG_TASK_COMPLETED,
    DBG_TASK_CANCELLED,
    DBG_TASK_FAILED,
} DbgTaskState;

typedef struct {
    uint64_t id;
    uint64_t parent_id;
    uint64_t thread_id;
    char *name;
    DbgTaskState state;
    DbgCodeLocation spawn_location;
} DbgTaskInfo;

/// Monad compiler-pipeline / semantic debugging

typedef enum {
    DBG_COMPILER_STAGE_PENDING = 0,
    DBG_COMPILER_STAGE_RUNNING,
    DBG_COMPILER_STAGE_COMPLETE,
    DBG_COMPILER_STAGE_FAILED,
} DbgCompilerStageState;

typedef struct {
    uint64_t id;
    uint64_t parent_id;
    uint64_t revision;
    char *name;           /* e.g. "type-check" */
    char *kind;           /* backend-defined stable kind */
    DbgCompilerStageState state;
    DbgSourceLocation location;
} DbgCompilerStageInfo;

typedef struct {
    uint64_t id;
    uint64_t stage_id;
    uint64_t parent_id;
    uint64_t children_reference;
    size_t child_count;
    char *kind;
    char *name;
    char *type_name;
    char *rendered;
    DbgSourceLocation location;
} DbgSemanticNodeInfo;

/// Breakpoints: logical request != resolved address locations

typedef enum {
    DBG_DATA_READ  = 1 << 0,
    DBG_DATA_WRITE = 1 << 1,
    DBG_DATA_READ_WRITE = DBG_DATA_READ | DBG_DATA_WRITE,
} DbgDataAccess;

typedef enum {
    DBG_BP_SOURCE = 0,
    DBG_BP_FUNCTION,
    DBG_BP_IR_VALUE,
    DBG_BP_INSTRUCTION,
    DBG_BP_DATA,
    DBG_BP_EXCEPTION,
} DbgBreakpointKind;

/* Compatibility names from the earlier implementation. */
#define DBG_BP_LINE    DBG_BP_SOURCE
#define DBG_BP_ADDRESS DBG_BP_INSTRUCTION

typedef struct {
    char *data_id;           /* opaque stable backend identity when known */
    char *description;
    uint64_t address;
    size_t size;
    DbgDataAccess supported_access;
    bool can_persist;
} DbgDataBreakpointInfo;

typedef struct {
    DbgCodeLocation location;
    bool executable;
    char *message;
} DbgBreakpointCandidate;

typedef struct {
    uint64_t id;
    uint64_t module_id;
    DbgCodeLocation location;
    bool resolved;
    bool hardware;
    uint64_t hit_count;
    char *message;
} DbgBreakpointSite;

struct DbgBreakpoint {
    uint32_t id;
    DbgBreakpointKind kind;
    bool enabled;
    bool temporary;
    bool verified;
    uint64_t thread_id;          /* 0 = any thread */
    char *condition;
    char *hit_condition;
    char *log_message;
    char *verification_message;
    uint64_t hit_count;

    union {
        struct { DbgSourceLocation location; } source;
        struct { char *symbol; } function;
        struct { char *function; char *value; } ir;
        struct { uint64_t address; int64_t offset; } instruction;
        struct {
            char *data_id;
            uint64_t address;
            size_t size;
            DbgDataAccess access;
        } data;
        struct { char *filter; } exception;
    } as;

    DbgBreakpointSite *sites;
    size_t site_count;
};

struct DbgWatch {
    uint32_t id;
    char *expression;
    char *last_value;
    char *last_error;
    bool changed_last_step;
};

/// Engine events

typedef enum {
    DBG_OUTPUT_CONSOLE = 0,
    DBG_OUTPUT_STDOUT,
    DBG_OUTPUT_STDERR,
    DBG_OUTPUT_TELEMETRY,
    DBG_OUTPUT_IMPORTANT,
} DbgOutputCategory;

typedef enum {
    DBG_BREAKPOINT_NEW = 0,
    DBG_BREAKPOINT_CHANGED,
    DBG_BREAKPOINT_REMOVED,
    DBG_BREAKPOINT_RESOLVED,
    DBG_BREAKPOINT_INVALIDATED,
} DbgBreakpointChangeReason;

typedef uint32_t DbgInvalidationMask;
#define DBG_INVALIDATE_THREADS     (UINT32_C(1) << 0)
#define DBG_INVALIDATE_STACKS      (UINT32_C(1) << 1)
#define DBG_INVALIDATE_SCOPES      (UINT32_C(1) << 2)
#define DBG_INVALIDATE_VARIABLES   (UINT32_C(1) << 3)
#define DBG_INVALIDATE_REGISTERS   (UINT32_C(1) << 4)
#define DBG_INVALIDATE_MEMORY      (UINT32_C(1) << 5)
#define DBG_INVALIDATE_BREAKPOINTS (UINT32_C(1) << 6)
#define DBG_INVALIDATE_MODULES     (UINT32_C(1) << 7)
#define DBG_INVALIDATE_SOURCES     (UINT32_C(1) << 8)
#define DBG_INVALIDATE_TASKS       (UINT32_C(1) << 9)
#define DBG_INVALIDATE_COMPILER    (UINT32_C(1) << 10)
#define DBG_INVALIDATE_ALL         UINT32_MAX

typedef enum {
    DBG_ENGINE_EVENT_NONE = 0,
    DBG_ENGINE_EVENT_PROCESS,
    DBG_ENGINE_EVENT_STOPPED,
    DBG_ENGINE_EVENT_CONTINUED,
    DBG_ENGINE_EVENT_EXITED,
    DBG_ENGINE_EVENT_THREAD_CREATED,
    DBG_ENGINE_EVENT_THREAD_EXITED,
    DBG_ENGINE_EVENT_OUTPUT,
    DBG_ENGINE_EVENT_MODULE_LOADED,
    DBG_ENGINE_EVENT_MODULE_UNLOADED,
    DBG_ENGINE_EVENT_BREAKPOINT,
    DBG_ENGINE_EVENT_MEMORY,
    DBG_ENGINE_EVENT_REPLAY_POSITION,
    DBG_ENGINE_EVENT_INVALIDATED,
    DBG_ENGINE_EVENT_CAPABILITIES,
    DBG_ENGINE_EVENT_COMPILER,
} DbgEngineEventKind;

typedef struct {
    DbgEngineEventKind kind;
    uint64_t sequence;       /* engine assigns one when backend leaves 0 */
    uint64_t timestamp_ms;   /* engine assigns one when backend leaves 0 */
    union {
        struct {
            uint64_t process_id;
            DbgProcessState state;
        } process;
        struct {
            uint64_t process_id;
            uint64_t thread_id;
            bool all_threads;
            DbgStopReason reason;
            int signal_number;
            uint32_t breakpoint_id;
            uint64_t address;
            char *description;
            DbgCodeLocation location;
        } stopped;
        struct {
            uint64_t process_id;
            uint64_t thread_id;
            bool all_threads;
        } continued;
        struct {
            uint64_t process_id;
            int exit_code;
        } exited;
        struct {
            uint64_t process_id;
            uint64_t thread_id;
        } thread;
        struct {
            DbgOutputCategory category;
            char *text;
            uint64_t variables_reference;
            DbgCodeLocation location;
        } output;
        struct {
            uint64_t module_id;
            char *path;
        } module;
        struct {
            uint32_t breakpoint_id;
            DbgBreakpointChangeReason reason;
            char *message;
        } breakpoint;
        struct {
            uint64_t address;
            size_t length;
        } memory;
        struct {
            uint64_t position;
            uint64_t checkpoint_id;
        } replay;
        struct {
            DbgInvalidationMask areas;
            uint64_t thread_id;
            uint64_t frame_id;
        } invalidated;
        struct {
            DbgCapabilities capabilities;
        } capabilities;
        struct {
            uint64_t stage_id;
            uint64_t node_id;
            char *message;
        } compiler;
    } as;
} DbgEngineEvent;

/// Backend ABI

/* All callbacks are synchronous request operations except poll_event. Long
   operations should be performed asynchronously by the backend and reported
   with events; event_fd may return -1 when no pollable descriptor exists.
   set_breakpoint is an idempotent upsert keyed by bp->id. The backend may
   replace bp->sites/verification fields; ownership then belongs to the engine. */
struct DbgBackendOps {
    uint32_t abi_version;
    uint32_t struct_size;       /* must be sizeof(DbgBackendOps) for ABI v2 */
    const char *name;
    DbgCapabilities capabilities;

    int  (*event_fd)(void *ctx);
    bool (*poll_event)(void *ctx, DbgEngineEvent *event_out, char **error_out);

    bool (*target_info)(void *ctx, DbgTargetInfo *out, char **error_out);
    bool (*launch)(void *ctx, const DbgLaunchSpec *spec, char **error_out);
    bool (*attach)(void *ctx, const DbgAttachSpec *spec, char **error_out);
    bool (*load_core)(void *ctx, const DbgCoreSpec *spec, char **error_out);
    bool (*connect_remote)(void *ctx, const DbgRemoteSpec *spec, char **error_out);
    bool (*restart)(void *ctx, char **error_out);
    bool (*detach)(void *ctx, char **error_out);
    bool (*terminate)(void *ctx, char **error_out);

    bool (*pause)(void *ctx, uint64_t thread_id, bool all_threads,
                  char **error_out);
    bool (*resume)(void *ctx, uint64_t thread_id, DbgRunMode mode,
                   DbgExecutionDirection direction, char **error_out);
    bool (*step)(void *ctx, const DbgStepPlan *plan, char **error_out);
    bool (*restart_frame)(void *ctx, uint64_t frame_id, char **error_out);
    bool (*step_in_targets)(void *ctx, uint64_t frame_id,
                            DbgStepInTarget **out, size_t *count,
                            char **error_out);
    bool (*goto_targets)(void *ctx, const DbgSourceLocation *source,
                         DbgGotoTarget **out, size_t *count, char **error_out);
    bool (*goto_target)(void *ctx, uint64_t thread_id, uint64_t target_id,
                        char **error_out);

    bool (*set_breakpoint)(void *ctx, DbgBreakpoint *bp, char **error_out);
    bool (*remove_breakpoint)(void *ctx, const DbgBreakpoint *bp,
                              char **error_out);
    bool (*breakpoint_locations)(void *ctx, const DbgSourceLocation *range,
                                 DbgBreakpointCandidate **out, size_t *count,
                                 char **error_out);
    bool (*data_breakpoint_info)(void *ctx, uint64_t frame_id,
                                 const char *expression,
                                 DbgDataBreakpointInfo *out,
                                 char **error_out);

    bool (*threads)(void *ctx, DbgThreadInfo **out, size_t *count,
                    char **error_out);
    bool (*stack_trace)(void *ctx, uint64_t thread_id, size_t start, size_t count,
                        DbgStackFrameInfo **out, size_t *out_count, size_t *total,
                        char **error_out);
    bool (*scopes)(void *ctx, uint64_t frame_id, DbgScopeInfo **out,
                   size_t *count, char **error_out);
    bool (*variables)(void *ctx, uint64_t variables_reference, size_t start,
                      size_t count, DbgValueInfo **out, size_t *out_count,
                      char **error_out);
    bool (*evaluate)(void *ctx, uint64_t frame_id, const char *expression,
                     const DbgEvaluationOptions *options, DbgValueInfo *out,
                     char **error_out);
    bool (*set_expression)(void *ctx, uint64_t frame_id, const char *expression,
                           const char *value, DbgValueInfo *out,
                           char **error_out);
    bool (*set_variable)(void *ctx, uint64_t variables_reference,
                         const char *name, const char *value,
                         DbgValueInfo *out, char **error_out);
    bool (*completions)(void *ctx, uint64_t frame_id, const char *text,
                        uint32_t cursor, DbgCompletionItem **out, size_t *count,
                        char **error_out);

    bool (*read_memory)(void *ctx, uint64_t address, size_t count,
                        uint8_t **out, size_t *out_count, char **error_out);
    bool (*write_memory)(void *ctx, uint64_t address, const uint8_t *bytes,
                         size_t count, size_t *written, char **error_out);
    bool (*registers)(void *ctx, uint64_t thread_id, DbgRegisterInfo **out,
                      size_t *count, char **error_out);
    bool (*disassemble)(void *ctx, uint64_t address, int64_t instruction_offset,
                        size_t instruction_count, DbgInstructionInfo **out,
                        size_t *count, char **error_out);
    bool (*modules)(void *ctx, DbgModuleInfo **out, size_t *count,
                    char **error_out);
    bool (*loaded_sources)(void *ctx, DbgSourceInfo **out, size_t *count,
                           char **error_out);
    bool (*source_content)(void *ctx, uint64_t source_id, const char *path,
                           char **content, size_t *content_len,
                           char **mime_type, char **error_out);
    bool (*tasks)(void *ctx, DbgTaskInfo **out, size_t *count,
                  char **error_out);

    bool (*compiler_stages)(void *ctx, DbgCompilerStageInfo **out, size_t *count,
                            char **error_out);
    bool (*semantic_node)(void *ctx, uint64_t node_id, DbgSemanticNodeInfo *out,
                          char **error_out);
    bool (*semantic_children)(void *ctx, uint64_t children_reference,
                              size_t start, size_t count,
                              DbgSemanticNodeInfo **out, size_t *out_count,
                              char **error_out);

    bool (*checkpoint)(void *ctx, uint64_t *checkpoint_id, char **error_out);
    bool (*restore_checkpoint)(void *ctx, uint64_t checkpoint_id,
                               char **error_out);

    void (*destroy)(void *ctx);
};

#define DBG_BACKEND_OPS_INIT(name_, capabilities_) \
    { .abi_version = DBG_BACKEND_ABI_VERSION, \
      .struct_size = (uint32_t)sizeof(DbgBackendOps), \
      .name = (name_), .capabilities = (capabilities_) }

/// Engine lifecycle / state

DbgEngine *dbg_engine_create(const DbgBackendOps *ops, void *backend_ctx,
                             bool take_backend_ownership);
DbgEngine *dbg_engine_create_checked(const DbgBackendOps *ops, void *backend_ctx,
                                     bool take_backend_ownership,
                                     char **error_out);
void dbg_engine_free(DbgEngine *engine);

const char *dbg_engine_backend_name(const DbgEngine *engine);
DbgCapabilities dbg_engine_capabilities(const DbgEngine *engine);
bool dbg_engine_has_capability(const DbgEngine *engine, DbgCapabilities cap);
DbgProcessState dbg_engine_process_state(const DbgEngine *engine);
uint64_t dbg_engine_process_id(const DbgEngine *engine);
uint64_t dbg_engine_selected_thread(const DbgEngine *engine);
uint64_t dbg_engine_selected_frame(const DbgEngine *engine);
DbgThreadState dbg_engine_thread_state(const DbgEngine *engine,
                                       uint64_t thread_id);
uint64_t dbg_engine_snapshot_epoch(const DbgEngine *engine);
uint64_t dbg_engine_state_epoch(const DbgEngine *engine);
void dbg_engine_select_thread(DbgEngine *engine, uint64_t thread_id);
void dbg_engine_select_frame(DbgEngine *engine, uint64_t frame_id);

bool dbg_engine_target_info(DbgEngine *engine, DbgTargetInfo *out,
                            char **error_out);
bool dbg_engine_launch(DbgEngine *engine, const DbgLaunchSpec *spec,
                       char **error_out);
bool dbg_engine_attach(DbgEngine *engine, const DbgAttachSpec *spec,
                       char **error_out);
bool dbg_engine_load_core(DbgEngine *engine, const DbgCoreSpec *spec,
                          char **error_out);
bool dbg_engine_connect_remote(DbgEngine *engine, const DbgRemoteSpec *spec,
                               char **error_out);
bool dbg_engine_restart(DbgEngine *engine, char **error_out);
bool dbg_engine_detach(DbgEngine *engine, char **error_out);
bool dbg_engine_terminate(DbgEngine *engine, char **error_out);
bool dbg_engine_pause(DbgEngine *engine, uint64_t thread_id, bool all_threads,
                      char **error_out);
bool dbg_engine_continue(DbgEngine *engine, uint64_t thread_id, DbgRunMode mode,
                         DbgExecutionDirection direction, char **error_out);
bool dbg_engine_execute_step_plan(DbgEngine *engine, const DbgStepPlan *plan,
                                  char **error_out);
bool dbg_engine_step(DbgEngine *engine, uint64_t thread_id,
                     DbgStepAction action, DbgStepGranularity granularity,
                     DbgRunMode mode, DbgExecutionDirection direction,
                     char **error_out);
bool dbg_engine_restart_frame(DbgEngine *engine, uint64_t frame_id,
                              char **error_out);
bool dbg_engine_step_in_targets(DbgEngine *engine, uint64_t frame_id,
                                DbgStepInTarget **out, size_t *count,
                                char **error_out);
bool dbg_engine_goto_targets(DbgEngine *engine,
                             const DbgSourceLocation *source,
                             DbgGotoTarget **out, size_t *count,
                             char **error_out);
bool dbg_engine_goto_target(DbgEngine *engine, uint64_t thread_id,
                            uint64_t target_id, char **error_out);

int dbg_engine_event_fd(DbgEngine *engine);
bool dbg_engine_poll_event(DbgEngine *engine, DbgEngineEvent *event_out,
                           char **error_out);
void dbg_engine_event_free(DbgEngineEvent *event);

/// Engine queries

bool dbg_engine_threads(DbgEngine *engine, DbgThreadInfo **out, size_t *count,
                        char **error_out);
bool dbg_engine_stack_trace(DbgEngine *engine, uint64_t thread_id, size_t start,
                            size_t count, DbgStackFrameInfo **out,
                            size_t *out_count, size_t *total,
                            char **error_out);
bool dbg_engine_scopes(DbgEngine *engine, uint64_t frame_id,
                       DbgScopeInfo **out, size_t *count, char **error_out);
bool dbg_engine_variables(DbgEngine *engine, uint64_t variables_reference,
                          size_t start, size_t count, DbgValueInfo **out,
                          size_t *out_count, char **error_out);
bool dbg_engine_evaluate(DbgEngine *engine, uint64_t frame_id,
                         const char *expression,
                         const DbgEvaluationOptions *options,
                         DbgValueInfo *out, char **error_out);
bool dbg_engine_set_expression(DbgEngine *engine, uint64_t frame_id,
                               const char *expression, const char *value,
                               DbgValueInfo *out, char **error_out);
bool dbg_engine_set_variable(DbgEngine *engine, uint64_t variables_reference,
                             const char *name, const char *value,
                             DbgValueInfo *out, char **error_out);
bool dbg_engine_completions(DbgEngine *engine, uint64_t frame_id,
                            const char *text, uint32_t cursor,
                            DbgCompletionItem **out, size_t *count,
                            char **error_out);
bool dbg_engine_read_memory(DbgEngine *engine, uint64_t address, size_t count,
                            uint8_t **out, size_t *out_count, char **error_out);
bool dbg_engine_write_memory(DbgEngine *engine, uint64_t address,
                             const uint8_t *bytes, size_t count,
                             size_t *written, char **error_out);
bool dbg_engine_registers(DbgEngine *engine, uint64_t thread_id,
                          DbgRegisterInfo **out, size_t *count,
                          char **error_out);
bool dbg_engine_disassemble(DbgEngine *engine, uint64_t address,
                            int64_t instruction_offset,
                            size_t instruction_count,
                            DbgInstructionInfo **out, size_t *count,
                            char **error_out);
bool dbg_engine_modules(DbgEngine *engine, DbgModuleInfo **out, size_t *count,
                        char **error_out);
bool dbg_engine_loaded_sources(DbgEngine *engine, DbgSourceInfo **out,
                               size_t *count, char **error_out);
bool dbg_engine_source_content(DbgEngine *engine, uint64_t source_id,
                               const char *path, char **content,
                               size_t *content_len, char **mime_type,
                               char **error_out);
bool dbg_engine_tasks(DbgEngine *engine, DbgTaskInfo **out, size_t *count,
                      char **error_out);

bool dbg_engine_compiler_stages(DbgEngine *engine,
                                DbgCompilerStageInfo **out, size_t *count,
                                char **error_out);
bool dbg_engine_semantic_node(DbgEngine *engine, uint64_t node_id,
                              DbgSemanticNodeInfo *out, char **error_out);
bool dbg_engine_semantic_children(DbgEngine *engine,
                                  uint64_t children_reference,
                                  size_t start, size_t count,
                                  DbgSemanticNodeInfo **out,
                                  size_t *out_count, char **error_out);

bool dbg_engine_checkpoint(DbgEngine *engine, uint64_t *checkpoint_id,
                           char **error_out);
bool dbg_engine_restore_checkpoint(DbgEngine *engine, uint64_t checkpoint_id,
                                   char **error_out);

/// Breakpoints and watches

uint32_t dbg_engine_add_source_breakpoint(DbgEngine *engine, const char *file,
                                          uint32_t line, uint32_t column);
uint32_t dbg_engine_add_function_breakpoint(DbgEngine *engine,
                                            const char *symbol);
uint32_t dbg_engine_add_ir_breakpoint(DbgEngine *engine, const char *function,
                                      const char *value);
uint32_t dbg_engine_add_instruction_breakpoint(DbgEngine *engine,
                                               uint64_t address,
                                               int64_t offset);
uint32_t dbg_engine_add_data_breakpoint(DbgEngine *engine,
                                        const DbgDataBreakpointInfo *info,
                                        DbgDataAccess access);
uint32_t dbg_engine_add_exception_breakpoint(DbgEngine *engine,
                                             const char *filter);
/* Compatibility convenience from ABI v1. */
uint32_t dbg_engine_add_address_breakpoint(DbgEngine *engine, uint64_t address);

bool dbg_engine_remove_breakpoint(DbgEngine *engine, uint32_t id,
                                  char **error_out);
bool dbg_engine_set_breakpoint_enabled(DbgEngine *engine, uint32_t id,
                                       bool enabled, char **error_out);
bool dbg_engine_set_breakpoint_condition(DbgEngine *engine, uint32_t id,
                                         const char *condition,
                                         const char *hit_condition,
                                         const char *log_message,
                                         char **error_out);
size_t dbg_engine_breakpoint_count(const DbgEngine *engine);
DbgBreakpoint *dbg_engine_breakpoint_at(DbgEngine *engine, size_t index);
const DbgBreakpoint *dbg_engine_breakpoint_at_const(const DbgEngine *engine,
                                                     size_t index);
const DbgCodeLocation *dbg_breakpoint_primary_location(const DbgBreakpoint *bp);

bool dbg_engine_breakpoint_locations(DbgEngine *engine,
                                     const DbgSourceLocation *range,
                                     DbgBreakpointCandidate **out,
                                     size_t *count, char **error_out);
bool dbg_engine_data_breakpoint_info(DbgEngine *engine, uint64_t frame_id,
                                     const char *expression,
                                     DbgDataBreakpointInfo *out,
                                     char **error_out);

uint32_t dbg_engine_add_watch(DbgEngine *engine, const char *expression);
bool dbg_engine_remove_watch(DbgEngine *engine, uint32_t id);
size_t dbg_engine_watch_count(const DbgEngine *engine);
DbgWatch *dbg_engine_watch_at(DbgEngine *engine, size_t index);

/// Cross-layer source / semantic / IR / machine provenance

DbgCodeMap *dbg_code_map_create(void);
void dbg_code_map_free(DbgCodeMap *map);
bool dbg_code_map_add(DbgCodeMap *map, const DbgCodeLocation *location);
void dbg_code_map_finalize(DbgCodeMap *map);
const DbgCodeLocation *dbg_code_map_find_address(const DbgCodeMap *map,
                                                 uint64_t address);
const DbgCodeLocation *dbg_code_map_find_source(const DbgCodeMap *map,
                                                const char *file,
                                                uint32_t line,
                                                uint32_t column);
const DbgCodeLocation *dbg_code_map_find_ir(const DbgCodeMap *map,
                                            const char *function,
                                            uint32_t instruction,
                                            const char *value);
const DbgCodeLocation *dbg_code_map_find_semantic(const DbgCodeMap *map,
                                                  uint64_t stage_id,
                                                  uint64_t node_id);

/// Matching free helpers

void dbg_source_location_free(DbgSourceLocation *location);
void dbg_code_location_free(DbgCodeLocation *location);
void dbg_target_info_free(DbgTargetInfo *target);
void dbg_thread_infos_free(DbgThreadInfo *items, size_t count);
void dbg_stack_frame_infos_free(DbgStackFrameInfo *items, size_t count);
void dbg_scope_infos_free(DbgScopeInfo *items, size_t count);
void dbg_value_info_free(DbgValueInfo *value);
void dbg_value_infos_free(DbgValueInfo *items, size_t count);
void dbg_completion_items_free(DbgCompletionItem *items, size_t count);
void dbg_module_infos_free(DbgModuleInfo *items, size_t count);
void dbg_source_infos_free(DbgSourceInfo *items, size_t count);
void dbg_register_infos_free(DbgRegisterInfo *items, size_t count);
void dbg_instruction_infos_free(DbgInstructionInfo *items, size_t count);
void dbg_task_infos_free(DbgTaskInfo *items, size_t count);
void dbg_compiler_stage_infos_free(DbgCompilerStageInfo *items, size_t count);
void dbg_semantic_node_info_free(DbgSemanticNodeInfo *node);
void dbg_semantic_node_infos_free(DbgSemanticNodeInfo *items, size_t count);
void dbg_step_in_targets_free(DbgStepInTarget *items, size_t count);
void dbg_goto_targets_free(DbgGotoTarget *items, size_t count);
void dbg_data_breakpoint_info_free(DbgDataBreakpointInfo *info);
void dbg_breakpoint_candidates_free(DbgBreakpointCandidate *items, size_t count);

/// Compiler diagnostics

typedef enum {
    DBG_SEV_NOTE = 0,
    DBG_SEV_WARNING,
    DBG_SEV_ERROR,
    DBG_SEV_FATAL,
    DBG_SEV_INTERNAL,
} DbgSeverity;

DbgErrorSnapshot *dbg_error_snapshot_create(DbgSeverity severity,
                                             const char *message,
                                             const char *code,
                                             const char *file,
                                             uint32_t line,
                                             uint32_t column);
void dbg_error_snapshot_free(DbgErrorSnapshot *snapshot);
void dbg_error_snapshot_add_frame(DbgErrorSnapshot *snapshot,
                                  const char *function,
                                  const char *file,
                                  uint32_t line,
                                  uint32_t column,
                                  const char *ir_value);
void dbg_trap_error(DbgErrorSnapshot *snapshot);
void dbg_trap_errorf(DbgSeverity severity, const char *file,
                     uint32_t line, uint32_t column,
                     const char *code, const char *fmt, ...)
    DBG_PRINTF_LIKE(6, 7);

/// Terminal client

struct DbgConfig {
    bool mouse_enabled;
    bool truecolor_force;
    uint32_t blink_period_ms;
    uint32_t blink_max_count;
    uint32_t target_fps;
    const char *emit_ir_command;

    DbgEngine *engine;            /* optional */
    bool take_engine_ownership;
    bool auto_refresh_on_stop;
    DbgRunMode default_run_mode;
    uint32_t engine_event_budget;
    uint32_t variable_page_size;
    uint32_t variable_child_limit;
    uint32_t disassembly_instruction_count;
};

DbgConfig dbg_default_config(void);
DbgSession *dbg_session_create(DbgConfig config);
void dbg_session_free(DbgSession *session);
void dbg_session_set_engine(DbgSession *session, DbgEngine *engine,
                            bool take_ownership);
int dbg_session_run(DbgSession *session);

int dbg_main(int argc, char **argv);
int dbg_main_with_config(int argc, char **argv, DbgConfig config);

#undef DBG_PRINTF_LIKE

#ifdef __cplusplus
}
#endif

#endif /* MONAD_DEBUGGER_H */
