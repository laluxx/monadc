#ifndef LSP_H
#define LSP_H

/// LSP — Language Server Protocol implementation for Monad
//
//  This module implements the Language Server Protocol (LSP 3.17) for the
//  Monad compiler, providing real-time editor feedback including:
//
//    · Diagnostics       — type errors, unbound variables, parse failures
//    · Completion        — symbols, keywords, methods, field access
//    · Hover             — type signatures, documentation, inferred types
//    · Go-to-def         — jump to definition across modules
//    · References        — find all uses of a symbol
//    · Rename            — project-wide symbol rename
//    · Inlay hints       — inferred types shown inline
//    · Semantic tokens   — full syntax highlighting via LSP
//    · Document symbols  — outline view of definitions
//    · Workspace symbols — cross-project symbol search
//    · Code actions      — quick fixes for common errors
//    · Signature help    — function parameter hints
//    · Folding ranges    — collapsible code regions
//
//  Architecture:
//
//    LspServer  ->  LspTransport  ->  stdin/stdout (JSON-RPC 2.0)
//       │
//       ├── LspDocument   (per-file state: source, AST, env, diagnostics)
//       ├── LspWorkspace  (project-wide index: all modules, symbols)
//       ├── LspIndex      (symbol table for fast lookup)
//       └── LspAnalyzer   (drives compilation pipeline for analysis)
//
//  Threading model:
//    · Main thread: JSON-RPC I/O loop (read → dispatch → write)
//    · Analysis thread: background reanalysis on document change
//    · Index thread: workspace-wide symbol indexing
//
//  All public functions are thread-safe unless noted otherwise.
//

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Forward declarations

typedef struct LspServer         LspServer;
typedef struct LspDocument       LspDocument;
typedef struct LspWorkspace      LspWorkspace;
typedef struct LspIndex          LspIndex;
typedef struct LspCompletion     LspCompletion;
typedef struct LspDiagnostic     LspDiagnostic;
typedef struct LspLocation       LspLocation;
typedef struct LspRange          LspRange;
typedef struct LspPosition       LspPosition;
typedef struct LspSymbol         LspSymbol;
typedef struct LspHover          LspHover;
typedef struct LspEdit           LspEdit;
typedef struct LspAction         LspAction;
typedef struct LspInlayHint      LspInlayHint;
typedef struct LspSemanticTokens LspSemanticTokens;
typedef struct LspSignatureHelp  LspSignatureHelp;


/// Position and Range
//
//  LSP uses 0-based line and character (UTF-16 code unit) offsets.
//  We store them as 0-based and convert at the boundary.
//
typedef struct LspPosition {
    uint32_t line;       // 0-based line index
    uint32_t character;  // 0-based UTF-16 code units
} LspPosition;

typedef struct LspRange {
    LspPosition start;
    LspPosition end;
} LspRange;

typedef struct LspLocation {
    char    *uri;    // file:// URI, owned
    LspRange range;
} LspLocation;


/// Diagnostic severity and tags

typedef enum {
    LSP_SEVERITY_ERROR       = 1,
    LSP_SEVERITY_WARNING     = 2,
    LSP_SEVERITY_INFORMATION = 3,
    LSP_SEVERITY_HINT        = 4,
} LspSeverity;

typedef enum {
    LSP_DIAG_TAG_UNNECESSARY = 1,  /* unused variable, dead code   */
    LSP_DIAG_TAG_DEPRECATED  = 2,  /* deprecated symbol            */
} LspDiagnosticTag;

typedef enum {
    LSP_SYMBOL_TAG_DEPRECATED = 1,
} LspSymbolTag;

typedef struct LspDiagnosticRelated {
    LspLocation location;
    char       *message;   /* owned */
} LspDiagnosticRelated;

typedef struct LspDiagnostic {
    LspRange   range;
    LspSeverity severity;
    uint32_t    tags;            // bitmask of LspDiagnosticTag
    char       *code;            // e.g. "E001", owned
    char       *source;          // "monad", owned
    char       *message;         // human-readable, owned
    char       *message_detail;  // extended detail, owned

    LspDiagnosticRelated *related;
    size_t                related_count;
} LspDiagnostic;


/// Completion item kinds (LSP spec §3.18.1)

typedef enum {
    LSP_COMPLETION_TEXT          =  1,
    LSP_COMPLETION_METHOD        =  2,
    LSP_COMPLETION_FUNCTION      =  3,
    LSP_COMPLETION_CONSTRUCTOR   =  4,
    LSP_COMPLETION_FIELD         =  5,
    LSP_COMPLETION_VARIABLE      =  6,
    LSP_COMPLETION_CLASS         =  7,
    LSP_COMPLETION_INTERFACE     =  8,
    LSP_COMPLETION_MODULE        =  9,
    LSP_COMPLETION_PROPERTY      = 10,
    LSP_COMPLETION_UNIT          = 11,
    LSP_COMPLETION_VALUE         = 12,
    LSP_COMPLETION_ENUM          = 13,
    LSP_COMPLETION_KEYWORD       = 14,
    LSP_COMPLETION_SNIPPET       = 15,
    LSP_COMPLETION_COLOR         = 16,
    LSP_COMPLETION_FILE          = 17,
    LSP_COMPLETION_REFERENCE     = 18,
    LSP_COMPLETION_FOLDER        = 19,
    LSP_COMPLETION_ENUM_MEMBER   = 20,
    LSP_COMPLETION_CONSTANT      = 21,
    LSP_COMPLETION_STRUCT        = 22,
    LSP_COMPLETION_EVENT         = 23,
    LSP_COMPLETION_OPERATOR      = 24,
    LSP_COMPLETION_TYPE_PARAM    = 25,
} LspCompletionKind;

typedef enum {
    LSP_INSERT_TEXT_PLAIN   = 1,
    LSP_INSERT_TEXT_SNIPPET = 2,         // $1, $2, ${1:placeholder}
} LspInsertTextFormat;

typedef struct LspCompletion {
    char               *label;           // shown in menu, owned
    char               *label_detail;    // type signature detail
    LspCompletionKind   kind;
    char               *detail;          // type signature, owned
    char               *documentation;   // markdown doc, owned
    bool                preselect;
    char               *sort_text;       // for ordering, owned
    char               *filter_text;     // for fuzzy match, owned
    char               *insert_text;     // what to actually insert
    LspInsertTextFormat insert_format;
    LspRange           *text_edit_range; // if non-null, replace range
    char              **commit_chars;    // e.g. {".", "("}
    size_t              commit_char_count;
    bool                deprecated;
} LspCompletion;


/// Hover result

typedef enum {
    LSP_MARKUP_PLAINTEXT = 0,
    LSP_MARKUP_MARKDOWN  = 1,
} LspMarkupKind;

typedef struct LspHover {
    LspMarkupKind kind;
    char         *contents;   // markdown or plain text, owned
    LspRange     *range;      // optional highlight range
} LspHover;


/// Symbol kinds (LSP spec §3.17.2)

typedef enum {
    LSP_SYMBOL_FILE          =  1,
    LSP_SYMBOL_MODULE        =  2,
    LSP_SYMBOL_NAMESPACE     =  3,
    LSP_SYMBOL_PACKAGE       =  4,
    LSP_SYMBOL_CLASS         =  5,
    LSP_SYMBOL_METHOD        =  6,
    LSP_SYMBOL_PROPERTY      =  7,
    LSP_SYMBOL_FIELD         =  8,
    LSP_SYMBOL_CONSTRUCTOR   =  9,
    LSP_SYMBOL_ENUM          = 10,
    LSP_SYMBOL_INTERFACE     = 11,
    LSP_SYMBOL_FUNCTION      = 12,
    LSP_SYMBOL_VARIABLE      = 13,
    LSP_SYMBOL_CONSTANT      = 14,
    LSP_SYMBOL_STRING        = 15,
    LSP_SYMBOL_NUMBER        = 16,
    LSP_SYMBOL_BOOLEAN       = 17,
    LSP_SYMBOL_ARRAY         = 18,
    LSP_SYMBOL_OBJECT        = 19,
    LSP_SYMBOL_KEY           = 20,
    LSP_SYMBOL_NULL          = 21,
    LSP_SYMBOL_ENUM_MEMBER   = 22,
    LSP_SYMBOL_STRUCT        = 23,
    LSP_SYMBOL_EVENT         = 24,
    LSP_SYMBOL_OPERATOR      = 25,
    LSP_SYMBOL_TYPE_PARAM    = 26,
} LspSymbolKind;

typedef struct LspSymbol {
    char         *name;            // owned
    char         *detail;          // type sig / module path, owned
    LspSymbolKind kind;
    bool          deprecated;
    uint32_t      tags;            // bitmask of LspSymbolTag
    LspRange      range;           // full definition range
    LspRange      selection_range; // name token range
    char         *container;       // enclosing module/layout, owned

    struct LspSymbol **children;
    size_t             child_count;
} LspSymbol;


/// Semantic tokens
//
//  Full-file semantic token encoding as per LSP 3.16+.
//  Tokens are delta-encoded: each token stores (deltaLine, deltaStart,
//  length, tokenType, tokenModifiers) as 5 uint32 values.
//
typedef enum {
    LSP_TOKEN_NAMESPACE    =  0,
    LSP_TOKEN_TYPE         =  1,
    LSP_TOKEN_CLASS        =  2,
    LSP_TOKEN_ENUM         =  3,
    LSP_TOKEN_INTERFACE    =  4,
    LSP_TOKEN_STRUCT       =  5,
    LSP_TOKEN_TYPE_PARAM   =  6,
    LSP_TOKEN_PARAMETER    =  7,
    LSP_TOKEN_VARIABLE     =  8,
    LSP_TOKEN_PROPERTY     =  9,
    LSP_TOKEN_ENUM_MEMBER  = 10,
    LSP_TOKEN_EVENT        = 11,
    LSP_TOKEN_FUNCTION     = 12,
    LSP_TOKEN_METHOD       = 13,
    LSP_TOKEN_MACRO        = 14,
    LSP_TOKEN_KEYWORD      = 15,
    LSP_TOKEN_MODIFIER     = 16,
    LSP_TOKEN_COMMENT      = 17,
    LSP_TOKEN_STRING       = 18,
    LSP_TOKEN_NUMBER       = 19,
    LSP_TOKEN_OPERATOR     = 20,
    LSP_TOKEN_DECORATOR    = 21,
    LSP_TOKEN_TYPE_COUNT,        /* sentinel — keep last */
} LspTokenType;

typedef enum {
    LSP_MOD_DECLARATION   = (1 << 0),
    LSP_MOD_DEFINITION    = (1 << 1),
    LSP_MOD_READONLY      = (1 << 2),
    LSP_MOD_STATIC        = (1 << 3),
    LSP_MOD_DEPRECATED    = (1 << 4),
    LSP_MOD_ABSTRACT      = (1 << 5),
    LSP_MOD_ASYNC         = (1 << 6),
    LSP_MOD_MODIFICATION  = (1 << 7),
    LSP_MOD_DOCUMENTATION = (1 << 8),
    LSP_MOD_DEFAULT_LIB   = (1 << 9),
} LspTokenModifier;

typedef struct LspSemanticTokens {
    uint32_t *data;       // delta-encoded 5-tuples, owned
    size_t    count;      // number of uint32 values (= tokens * 5)
    char     *result_id;  // for incremental updates, owned
} LspSemanticTokens;


/// Signature help
//
//  Shown when the cursor is inside a function call's argument list.
//
typedef struct LspParameterInfo {
    char *label;          /* parameter name + type, owned */
    char *documentation;  /* markdown, owned              */
} LspParameterInfo;

typedef struct LspSignatureInfo {
    char              *label;           /* full signature string, owned */
    char              *documentation;   /* markdown, owned              */
    LspParameterInfo  *parameters;      /* owned array                  */
    size_t             parameter_count;
    uint32_t           active_parameter; /* 0-based                     */
} LspSignatureInfo;

typedef struct LspSignatureHelp {
    LspSignatureInfo *signatures;       /* owned array */
    size_t            signature_count;
    uint32_t          active_signature; /* 0-based     */
    uint32_t          active_parameter; /* 0-based     */
} LspSignatureHelp;


/// Inlay hints
//
//  Inline type annotations shown by the editor but not in the source.
//  Used to display inferred types after variable names.
//
typedef enum {
    LSP_INLAY_TYPE       = 1,  /* ": Int" after a variable    */
    LSP_INLAY_PARAMETER  = 2,  /* "x:" before an argument     */
} LspInlayHintKind;

typedef struct LspInlayHint {
    LspPosition    position;
    char          *label;       /* e.g. ": Int", owned         */
    LspInlayHintKind kind;
    char          *tooltip;     /* markdown, owned             */
    bool           padding_left;
    bool           padding_right;
} LspInlayHint;


/// Code actions
//
//  Quick fixes and refactoring suggestions attached to diagnostics.
//
typedef enum {
    LSP_ACTION_EMPTY             = 0,
    LSP_ACTION_QUICKFIX          = 1,
    LSP_ACTION_REFACTOR          = 2,
    LSP_ACTION_REFACTOR_EXTRACT  = 3,
    LSP_ACTION_REFACTOR_INLINE   = 4,
    LSP_ACTION_REFACTOR_REWRITE  = 5,
    LSP_ACTION_SOURCE            = 6,
    LSP_ACTION_SOURCE_ORGANIZE   = 7,
    LSP_ACTION_SOURCE_FIXALL     = 8,
} LspActionKind;

typedef struct LspTextEdit {
    LspRange  range;
    char     *new_text;   /* owned */
} LspTextEdit;

typedef struct LspWorkspaceEdit {
    char          *uri;        /* target file URI, owned  */
    LspTextEdit   *edits;      /* owned array             */
    size_t         edit_count;
} LspWorkspaceEdit;

typedef struct LspAction {
    char              *title;       /* shown in menu, owned       */
    LspActionKind      kind;
    LspDiagnostic    **diagnostics; /* which diagnostics this fixes */
    size_t             diag_count;
    bool               is_preferred;
    LspWorkspaceEdit  *edit;        /* owned, may be NULL         */
    char              *command;     /* fallback command, owned    */
} LspAction;


/// Folding ranges

typedef enum {
    LSP_FOLD_COMMENT = 0,
    LSP_FOLD_IMPORTS = 1,
    LSP_FOLD_REGION  = 2,
} LspFoldKind;

typedef struct LspFoldRange {
    uint32_t   start_line;
    uint32_t   end_line;
    LspFoldKind kind;
    char       *collapsed_text;  /* shown when folded, owned */
} LspFoldRange;


/// Document state
//
//  One LspDocument per open file. Tracks the live source text,
//  parsed AST, type environment, and cached analysis results.
//  All fields are owned by the document.
//
typedef enum {
    LSP_DOC_CLEAN    = 0,  /* analysis is up to date    */
    LSP_DOC_DIRTY    = 1,  /* source changed, needs reanalysis */
    LSP_DOC_ANALYZING = 2, /* reanalysis in progress    */
    LSP_DOC_ERROR    = 3,  /* last analysis failed hard */
} LspDocState;

typedef struct LspDocument {
    char         *uri;            /* file:// URI, owned                */
    char         *path;           /* filesystem path, owned            */
    char         *source;         /* current source text, owned        */
    size_t        source_len;
    int           version;        /* LSP document version counter      */
    LspDocState   state;

    /* Analysis results — rebuilt on each reanalysis */
    LspDiagnostic   *diagnostics;
    size_t           diag_count;
    size_t           diag_cap;

    LspSymbol      **symbols;     /* document symbol outline           */
    size_t           symbol_count;
    size_t           symbol_cap;

    LspSemanticTokens *tokens;    /* full semantic token encoding      */

    LspInlayHint    *inlay_hints;
    size_t           inlay_count;
    size_t           inlay_cap;

    LspFoldRange    *folds;
    size_t           fold_count;
    size_t           fold_cap;

    /* Line index for fast position <-> offset conversion */
    uint32_t        *line_offsets; /* line_offsets[i] = byte offset of line i */
    uint32_t         line_count;
    uint32_t         line_cap;

    /* Back-pointer to owning workspace */
    LspWorkspace    *workspace;
} LspDocument;


/// Workspace index
//
//  Global symbol table built from all files in the workspace.
//  Used for go-to-definition, find-references, and workspace symbol search.
//
typedef struct LspIndexEntry {
    char         *name;          /* fully-qualified symbol name, owned */
    char         *short_name;    /* unqualified name for display, owned */
    char         *module;        /* module name, owned                 */
    char         *uri;           /* source file URI, owned             */
    LspRange      range;         /* definition range                   */
    LspRange      name_range;    /* name token range only              */
    LspSymbolKind kind;
    char         *type_sig;      /* type signature string, owned       */
    char         *documentation; /* extracted doc comment, owned       */

    /* Reference locations — populated lazily */
    LspLocation  *references;
    size_t        ref_count;
    size_t        ref_cap;

    struct LspIndexEntry *next;  /* hash chain */
} LspIndexEntry;

#define LSP_INDEX_BUCKETS 4096

typedef struct LspIndex {
    LspIndexEntry  *buckets[LSP_INDEX_BUCKETS];
    size_t          entry_count;

    /* Sorted name array for prefix search */
    LspIndexEntry **sorted;
    size_t          sorted_count;
    size_t          sorted_cap;
    bool            sorted_dirty;
} LspIndex;


/// Workspace
//
//  The workspace owns all open documents and the cross-file index.
//  It drives background reanalysis and manages the compilation cache.
//
typedef struct LspWorkspace {
    char         *root_uri;       /* workspace root, owned              */
    char         *root_path;      /* filesystem path, owned             */

    /* Open documents (hash map by URI) */
    LspDocument **docs;
    size_t        doc_count;
    size_t        doc_cap;

    /* Cross-file symbol index */
    LspIndex     *index;

    /* Package config (from package.yaml) */
    char         *package_name;
    char         *source_dir;
    char         *main_file;

    /* Compilation cache: maps source path → compiled module info */
    /* (opaque to this header; managed in lsp.c)                  */
    void         *compile_cache;
} LspWorkspace;


/// Semantic token (internal helper used by analysis layer)

typedef struct LspToken {
    uint32_t      line;
    uint32_t      start_char;
    uint32_t      length;
    LspTokenType  type;
    uint32_t      modifiers;
} LspToken;


/// Location link (definition with origin range)

typedef struct LspLocationLink {
    LspRange  origin_selection_range;
    char     *target_uri;    /* owned */
    LspRange  target_range;
    LspRange  target_selection_range;
} LspLocationLink;


/// Call hierarchy

typedef struct LspCallHierarchyItem {
    char         *name;             /* owned */
    LspSymbolKind kind;
    char         *detail;           /* owned */
    char         *uri;              /* owned */
    LspRange      range;
    LspRange      selection_range;
} LspCallHierarchyItem;

typedef struct LspCallHierarchyIncoming {
    LspCallHierarchyItem *from;       /* owned */
    LspRange             *from_ranges; /* owned array */
    size_t                from_range_count;
} LspCallHierarchyIncoming;

typedef struct LspCallHierarchyOutgoing {
    LspCallHierarchyItem *to;         /* owned */
    LspRange             *from_ranges; /* owned array */
    size_t                from_range_count;
} LspCallHierarchyOutgoing;


/// Type hierarchy

typedef struct LspTypeHierarchyItem {
    char         *name;             /* owned */
    LspSymbolKind kind;
    char         *detail;           /* owned */
    char         *uri;              /* owned */
    LspRange      range;
    LspRange      selection_range;
} LspTypeHierarchyItem;


/// Server configuration

typedef struct LspConfig {
    /* Analysis */
    bool    check_on_save;         /* reanalyze on didSave (vs. didChange) */
    bool    check_on_change;       /* reanalyze on every keystroke         */
    int     debounce_ms;           /* delay before reanalysis fires        */
    bool    inlay_hints_enabled;
    bool    semantic_tokens_enabled;
    bool    call_hierarchy_enabled;
    bool    type_hierarchy_enabled;

    /* Completion */
    bool    completion_enabled;
    bool    completion_snippets;   /* emit snippet-style completions       */
    bool    completion_auto_import; /* offer to add import statements      */
    int     completion_max_items;  /* cap on completion list size          */

    /* Diagnostics */
    bool    show_dep_warnings;     /* show dependent type checker warnings */
    bool    show_hm_trace;         /* show HM unification trace            */
    int     max_diagnostics;       /* cap per file; 0 = unlimited          */

    /* Formatting */
    int     indent_width;          /* spaces per indent level              */

    /* Logging */
    int     log_level;             /* 0=off 1=error 2=warn 3=info 4=debug */
    FILE   *log_file;              /* NULL = stderr                        */
} LspConfig;


/// Transport
//
//  Handles JSON-RPC 2.0 framing over stdin/stdout.
//  Each message: "Content-Length: N\r\n\r\n<N bytes of JSON>"
//
typedef struct LspTransport {
    FILE   *in;           /* input stream  (normally stdin)  */
    FILE   *out;          /* output stream (normally stdout) */

    char   *read_buf;     /* owned, grows as needed          */
    size_t  read_buf_cap;
    size_t  read_buf_len;

    char   *write_buf;    /* owned, for batching writes      */
    size_t  write_buf_cap;
    size_t  write_buf_len;
} LspTransport;


/// Request / notification IDs

typedef enum {
    LSP_ID_NULL   = 0,
    LSP_ID_INT    = 1,
    LSP_ID_STRING = 2,
} LspIdKind;

typedef struct LspId {
    LspIdKind kind;
    union {
        int64_t  number;
        char    *string;   /* owned when kind==LSP_ID_STRING */
    };
} LspId;


/// JSON-RPC message envelope

typedef enum {
    LSP_MSG_REQUEST      = 1,
    LSP_MSG_RESPONSE     = 2,
    LSP_MSG_NOTIFICATION = 3,
} LspMsgKind;

typedef struct LspMessage {
    LspMsgKind  kind;
    LspId       id;           /* valid for REQUEST and RESPONSE */
    char       *method;       /* owned; NULL for RESPONSE       */
    char       *params_json;  /* raw JSON string, owned         */
    char       *result_json;  /* raw JSON string, owned (RESPONSE ok)    */
    char       *error_json;   /* raw JSON string, owned (RESPONSE error) */
} LspMessage;


/// Main server struct

typedef struct LspServer {
    LspTransport  *transport;
    LspWorkspace  *workspace;
    LspConfig      config;

    /* Lifecycle state */
    bool           initialized;
    bool           shutdown_requested;
    int            exit_code;

    /* Pending request tracking (for cancellation) */
    LspId         *pending_ids;
    size_t         pending_count;
    size_t         pending_cap;

    /* Background analysis queue */
    char          **dirty_uris;     /* URIs needing reanalysis */
    size_t          dirty_count;
    size_t          dirty_cap;
} LspServer;


/// Server lifecycle

LspServer    *lsp_server_create(LspConfig config);
void          lsp_server_free(LspServer *server);
int           lsp_server_run(LspServer *server);   /* blocks until exit */
void          lsp_server_stop(LspServer *server);


/// Transport

LspTransport *lsp_transport_create(FILE *in, FILE *out);
void          lsp_transport_free(LspTransport *t);
LspMessage   *lsp_transport_read(LspTransport *t);   /* blocks */
void          lsp_transport_write(LspTransport *t, const char *json);
void          lsp_transport_flush(LspTransport *t);


/// Message construction helpers

char *lsp_make_response(LspId id, const char *result_json);
char *lsp_make_error(LspId id, int code, const char *message);
char *lsp_make_notification(const char *method, const char *params_json);
void  lsp_id_free(LspId *id);
void  lsp_message_free(LspMessage *msg);


/// Standard LSP error codes

#define LSP_ERR_PARSE_ERROR       -32700
#define LSP_ERR_INVALID_REQUEST   -32600
#define LSP_ERR_METHOD_NOT_FOUND  -32601
#define LSP_ERR_INVALID_PARAMS    -32602
#define LSP_ERR_INTERNAL          -32603
#define LSP_ERR_SERVER_NOT_INIT   -32002
#define LSP_ERR_UNKNOWN_PROTOCOL  -32001
#define LSP_ERR_CONTENT_MODIFIED  -32801
#define LSP_ERR_REQUEST_CANCELLED -32800


/// Workspace management

LspWorkspace *lsp_workspace_create(const char *root_uri);
void          lsp_workspace_free(LspWorkspace *ws);
void          lsp_workspace_index(LspWorkspace *ws);   /* full re-index */
LspDocument  *lsp_workspace_get_doc(LspWorkspace *ws, const char *uri);
LspDocument  *lsp_workspace_open_doc(LspWorkspace *ws, const char *uri,
                                      const char *source, int version);
void          lsp_workspace_update_doc(LspWorkspace *ws, const char *uri,
                                        const char *source, int version);
void          lsp_workspace_close_doc(LspWorkspace *ws, const char *uri);
void          lsp_workspace_analyze(LspWorkspace *ws, const char *uri);
void          lsp_workspace_analyze_all(LspWorkspace *ws);


/// Document operations

LspDocument  *lsp_document_create(const char *uri, const char *source, int version);
void          lsp_document_free(LspDocument *doc);
void          lsp_document_update(LspDocument *doc, const char *source, int version);
void          lsp_document_analyze(LspDocument *doc);
void          lsp_document_add_diagnostic(LspDocument *doc, LspDiagnostic diag);
void          lsp_document_clear_diagnostics(LspDocument *doc);

/* Position utilities */
uint32_t      lsp_document_offset(const LspDocument *doc, LspPosition pos);
LspPosition   lsp_document_position(const LspDocument *doc, uint32_t offset);
LspRange      lsp_document_word_range(const LspDocument *doc, LspPosition pos);
char         *lsp_document_word_at(const LspDocument *doc, LspPosition pos);
void          lsp_document_build_line_index(LspDocument *doc);


/// Index operations

LspIndex      *lsp_index_create(void);
void           lsp_index_free(LspIndex *idx);
void           lsp_index_insert(LspIndex *idx, LspIndexEntry *entry);
LspIndexEntry *lsp_index_lookup(LspIndex *idx, const char *name);
LspIndexEntry **lsp_index_prefix(LspIndex *idx, const char *prefix, size_t *count);
LspIndexEntry **lsp_index_fuzzy(LspIndex *idx, const char *query, size_t *count);
void           lsp_index_remove_file(LspIndex *idx, const char *uri);
void           lsp_index_rebuild_sorted(LspIndex *idx);
LspIndexEntry *lsp_index_entry_create(void);
void           lsp_index_entry_free(LspIndexEntry *e);


/// Language features — all return heap-allocated results

/* textDocument/completion */
LspCompletion **lsp_completion(LspDocument *doc, LspPosition pos,
                                bool triggered_by_dot, size_t *count);

/* textDocument/hover */
LspHover       *lsp_hover(LspDocument *doc, LspPosition pos);

/* textDocument/definition */
LspLocation    *lsp_definition(LspDocument *doc, LspPosition pos, size_t *count);

/* textDocument/references */
LspLocation    *lsp_references(LspDocument *doc, LspPosition pos,
                                bool include_declaration, size_t *count);

/* textDocument/documentSymbol */
LspSymbol     **lsp_document_symbols(LspDocument *doc, size_t *count);

/* workspace/symbol */
LspSymbol     **lsp_workspace_symbols(LspWorkspace *ws,
                                       const char *query, size_t *count);

/* textDocument/semanticTokens/full */
LspSemanticTokens *lsp_semantic_tokens(LspDocument *doc);

/* textDocument/inlayHint */
LspInlayHint   *lsp_inlay_hints(LspDocument *doc, LspRange range, size_t *count);

/* textDocument/signatureHelp */
LspSignatureHelp *lsp_signature_help(LspDocument *doc, LspPosition pos);

/* textDocument/codeAction */
LspAction      **lsp_code_actions(LspDocument *doc, LspRange range,
                                   LspDiagnostic **diags, size_t diag_count,
                                   size_t *count);

/* textDocument/rename */
LspWorkspaceEdit *lsp_rename(LspDocument *doc, LspPosition pos,
                              const char *new_name);

/* textDocument/prepareRename */
LspRange        *lsp_prepare_rename(LspDocument *doc, LspPosition pos);

/* textDocument/foldingRange */
LspFoldRange    *lsp_folding_ranges(LspDocument *doc, size_t *count);

/* textDocument/formatting */
LspTextEdit     *lsp_format(LspDocument *doc, size_t *count);

/* textDocument/rangeFormatting */
LspTextEdit     *lsp_format_range(LspDocument *doc, LspRange range, size_t *count);


/// Free helpers for result types

void lsp_completion_free(LspCompletion *c);
void lsp_completion_list_free(LspCompletion **list, size_t count);
void lsp_hover_free(LspHover *h);
void lsp_location_free(LspLocation *loc);
void lsp_location_list_free(LspLocation *list, size_t count);
void lsp_symbol_free(LspSymbol *s);
void lsp_symbol_list_free(LspSymbol **list, size_t count);
void lsp_semantic_tokens_free(LspSemanticTokens *st);
void lsp_inlay_hint_free(LspInlayHint *h);
void lsp_inlay_hint_list_free(LspInlayHint *list, size_t count);
void lsp_signature_help_free(LspSignatureHelp *sh);
void lsp_action_free(LspAction *a);
void lsp_action_list_free(LspAction **list, size_t count);
void lsp_workspace_edit_free(LspWorkspaceEdit *we);
void lsp_fold_range_list_free(LspFoldRange *list, size_t count);
void lsp_text_edit_list_free(LspTextEdit *list, size_t count);
void lsp_diagnostic_free(LspDiagnostic *d);


/// JSON serialization helpers

char *lsp_json_position(LspPosition pos);
char *lsp_json_range(LspRange range);
char *lsp_json_location(LspLocation *loc);
char *lsp_json_diagnostic(LspDiagnostic *d);
char *lsp_json_diagnostic_list(LspDiagnostic *list, size_t count);
char *lsp_json_completion(LspCompletion *c);
char *lsp_json_completion_list(LspCompletion **list, size_t count);
char *lsp_json_hover(LspHover *h);
char *lsp_json_symbol(LspSymbol *s);
char *lsp_json_symbol_list(LspSymbol **list, size_t count);
char *lsp_json_semantic_tokens(LspSemanticTokens *st);
char *lsp_json_inlay_hints(LspInlayHint *list, size_t count);
char *lsp_json_signature_help(LspSignatureHelp *sh);
char *lsp_json_action(LspAction *a);
char *lsp_json_action_list(LspAction **list, size_t count);
char *lsp_json_workspace_edit(LspWorkspaceEdit *we);
char *lsp_json_fold_ranges(LspFoldRange *list, size_t count);
char *lsp_json_text_edits(LspTextEdit *list, size_t count);
char *lsp_json_location_list(LspLocation *list, size_t count);

/* Generic JSON builder */
char *lsp_json_escape(const char *s);   /* returns heap-allocated escaped string */


/// Logging

#define LSP_LOG_ERROR 1
#define LSP_LOG_WARN  2
#define LSP_LOG_INFO  3
#define LSP_LOG_DEBUG 4

void lsp_log(LspServer *server, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Send window/logMessage notification to the client */
void lsp_send_log_message(LspServer *server, int level, const char *msg);

/* Send window/showMessage notification */
void lsp_send_show_message(LspServer *server, int type, const char *msg);

/* Send $/progress notification */
void lsp_send_progress(LspServer *server, const char *token,
                       const char *title, const char *message,
                       int percentage);


/// Capabilities negotiation
//
//  lsp_server_capabilities() returns the JSON object we send in
//  the initialize response, advertising all supported features.
//
char *lsp_server_capabilities(LspServer *server);
char *lsp_server_info(void);   /* {"name":"monad-lsp","version":"0.1.0"} */


/// Call/type hierarchy free helpers
//

void lsp_call_hierarchy_item_free(LspCallHierarchyItem *item);
void lsp_call_hierarchy_item_list_free(LspCallHierarchyItem **list, size_t count);
void lsp_call_hierarchy_incoming_list_free(LspCallHierarchyIncoming *list, size_t count);
void lsp_call_hierarchy_outgoing_list_free(LspCallHierarchyOutgoing *list, size_t count);
void lsp_type_hierarchy_item_free(LspTypeHierarchyItem *item);
void lsp_type_hierarchy_item_list_free(LspTypeHierarchyItem **list, size_t count);
void lsp_location_link_free(LspLocationLink *link);

char *lsp_json_call_hierarchy_item(LspCallHierarchyItem *item);
char *lsp_json_type_hierarchy_item(LspTypeHierarchyItem *item);


/// Monad-specific analysis integration
//
//  These bridge the LSP layer to the Monad compiler pipeline.
//  They run inside the analysis thread and populate the document's
//  diagnostics, symbols, tokens, and inlay hints.
//
typedef struct LspAnalysisResult {
    LspDiagnostic  *diagnostics;
    size_t          diag_count;
    LspSymbol     **symbols;
    size_t          symbol_count;
    LspInlayHint   *inlay_hints;
    size_t          inlay_count;
    LspSemanticTokens *tokens;
    LspFoldRange   *folds;
    size_t          fold_count;
    char           *error_message;
    bool            success;
} LspAnalysisResult;

LspAnalysisResult *lsp_analyze_file(const char *path,
                                     const char *source,
                                     LspWorkspace *ws);
void               lsp_analysis_result_free(LspAnalysisResult *r);
void               lsp_analysis_result_apply(LspDocument *doc,
                                             LspAnalysisResult *r);
void               lsp_publish_diagnostics(LspServer *server,
                                           LspDocument *doc);

/* Extract doc comment immediately preceding a definition */
char *lsp_extract_doc_comment(const char *source, uint32_t def_line);

/* Build type signature string from compiler Type* (opaque here) */
char *lsp_format_type(void *type_ptr);

/* Keyword list for completion */
const char **lsp_keywords(size_t *count);

/* Snippet templates for common patterns */
const char **lsp_snippet_labels(size_t *count);
const char **lsp_snippet_bodies(size_t *count);


#ifdef __cplusplus
}
#endif

#endif /* LSP_H */
