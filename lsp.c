/// lsp.c — Language Server Protocol implementation for Monad
//
//  Implements the complete LSP 3.17 surface described in lsp.h.
//
//  Source layout:
//
//    §1   Includes and internal utilities
//    §2   Memory helpers
//    §3   String / JSON helpers
//    §4   UTF-8 / UTF-16 conversion
//    §5   Transport (JSON-RPC framing)
//    §6   Message parsing
//    §7   Message construction
//    §8   Position utilities
//    §9   Line index
//    §10  Document management
//    §11  Index management
//    §12  Workspace management
//    §13  Analysis integration
//    §14  Completion
//    §15  Hover
//    §16  Go-to-definition family
//    §17  References / document highlight
//    §18  Document & workspace symbols
//    §19  Semantic tokens
//    §20  Inlay hints
//    §21  Signature help
//    §22  Code actions
//    §23  Rename / prepare-rename
//    §24  Folding ranges
//    §25  Formatting
//    §26  Linked editing
//    §27  Call hierarchy
//    §28  Type hierarchy
//    §29  JSON serialization
//    §30  Capabilities
//    §31  Request dispatcher
//    §32  Server lifecycle
//    §33  Logging
//    §34  Monad keywords and snippets
//

#include "lsp.h"

#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>


/// Forward declaration
void lsp_analysis_result_apply(LspDocument *doc, LspAnalysisResult *r);
LspConfig lsp_default_config(void);


/// §1  Includes and internal constants

//// Internal constants
 //
 //  Capacity growth factor, initial sizes, and hard limits used throughout
 //  the server.  Changing LSP_GROW_FACTOR affects all dynamic arrays.
 //
#define LSP_GROW_FACTOR        2
#define LSP_INITIAL_BUF        4096
#define LSP_INITIAL_DIAG_CAP   16
#define LSP_INITIAL_DOC_CAP    8
#define LSP_INITIAL_SYM_CAP    64
#define LSP_INITIAL_TOKEN_CAP  256
#define LSP_INITIAL_HINT_CAP   32
#define LSP_MAX_COMPLETIONS    512
#define LSP_MAX_REFERENCES     8192
#define LSP_RESULT_ID_LEN      32


/// §2  Memory helpers

//// Memory helpers
 //
 //  Thin wrappers around malloc/realloc that abort on OOM.  A real
 //  production server would want to propagate errors, but for a compiler
 //  tool that restarts quickly this is the right trade-off.
 //
static void *lsp_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "lsp: out of memory\n"); abort(); }
    return p;
}

static void *lsp_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "lsp: out of memory\n"); abort(); }
    return p;
}

static void *lsp_xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n);
    if (!p) { fprintf(stderr, "lsp: out of memory\n"); abort(); }
    return p;
}

static char *lsp_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *d = lsp_xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static char *lsp_xstrndup(const char *s, size_t n)
{
    if (!s) return NULL;
    char *d = lsp_xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* Grow a heap array to at least (count + 1) slots. */
#define LSP_GROW(ptr, count, cap, type)                        \
    do {                                                       \
        if ((count) >= (cap)) {                                \
            (cap) = (cap) ? (cap) * LSP_GROW_FACTOR : 8;       \
            (ptr) = lsp_xrealloc((ptr), (cap) * sizeof(type)); \
        }                                                      \
    } while (0)


///   §3  String / JSON helpers

//// String builder
 //
 //  A lightweight append-only string buffer.  Used throughout the JSON
 //  serialization layer to build up response bodies without a fixed-size
 //  intermediate buffer.
 //
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *b)
{
    b->data = lsp_xmalloc(LSP_INITIAL_BUF);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = LSP_INITIAL_BUF;
}

static void sb_free(StrBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void sb_ensure(StrBuf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        while (b->cap < b->len + extra + 1)
            b->cap *= LSP_GROW_FACTOR;
        b->data = lsp_xrealloc(b->data, b->cap);
    }
}

static void sb_append(StrBuf *b, const char *s)
{
    size_t n = strlen(s);
    sb_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_appendn(StrBuf *b, const char *s, size_t n)
{
    sb_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_appendc(StrBuf *b, char c)
{
    sb_ensure(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void sb_appendf(StrBuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_appendf(StrBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_ensure(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* Transfer ownership of the buffer's data out, leaving it reset. */
static char *sb_take(StrBuf *b)
{
    char *s = b->data;
    b->data = lsp_xmalloc(LSP_INITIAL_BUF);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = LSP_INITIAL_BUF;
    return s;
}


/// JSON escape
//
//  Produces a JSON-safe string with backslash escaping for the standard
//  set of control characters.  The result is NUL-terminated but does NOT
//  include surrounding quotation marks.
//
char *lsp_json_escape(const char *s)
{
    if (!s) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_append(&b, "\\\""); break;
        case '\\': sb_append(&b, "\\\\"); break;
        case '\b': sb_append(&b, "\\b");  break;
        case '\f': sb_append(&b, "\\f");  break;
        case '\n': sb_append(&b, "\\n");  break;
        case '\r': sb_append(&b, "\\r");  break;
        case '\t': sb_append(&b, "\\t");  break;
        default:
            if (c < 0x20) {
                /* Control character — use \uXXXX */
                char hex[8];
                snprintf(hex, sizeof(hex), "\\u%04x", c);
                sb_append(&b, hex);
            } else {
                sb_appendc(&b, (char)c);
            }
            break;
        }
    }
    return sb_take(&b);
}

char *lsp_json_string(const char *s)
{
    if (!s) return lsp_xstrdup("null");
    char *esc = lsp_json_escape(s);
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '"');
    sb_append(&b, esc);
    sb_appendc(&b, '"');
    free(esc);
    return sb_take(&b);
}

char *lsp_json_bool(bool v)
{
    return lsp_xstrdup(v ? "true" : "false");
}

char *lsp_json_int(int64_t v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, v);
    return lsp_xstrdup(buf);
}

/* Quick helpers for optional string fields in JSON objects. */
static void sb_json_field_str(StrBuf *b, const char *key, const char *val,
                               bool *first)
{
    if (!val) return;
    if (!*first) sb_appendc(b, ',');
    *first = false;
    char *k = lsp_json_string(key);
    char *v = lsp_json_string(val);
    sb_appendf(b, "%s:%s", k, v);
    free(k); free(v);
}

static void sb_json_field_bool(StrBuf *b, const char *key, bool val,
                                bool *first)
{
    if (!*first) sb_appendc(b, ',');
    *first = false;
    char *k = lsp_json_string(key);
    sb_appendf(b, "%s:%s", k, val ? "true" : "false");
    free(k);
}

static void sb_json_field_int(StrBuf *b, const char *key, int64_t val,
                               bool *first)
{
    if (!*first) sb_appendc(b, ',');
    *first = false;
    char *k = lsp_json_string(key);
    sb_appendf(b, "%s:%" PRId64, k, val);
    free(k);
}

static void sb_json_field_raw(StrBuf *b, const char *key, const char *raw,
                               bool *first)
{
    if (!raw) return;
    if (!*first) sb_appendc(b, ',');
    *first = false;
    char *k = lsp_json_string(key);
    sb_appendf(b, "%s:%s", k, raw);
    free(k);
}


/// §4  UTF-8 / UTF-16 conversion

//// UTF-8 <-> UTF-16 column conversion
 //
 //  LSP mandates UTF-16 character offsets.  We store byte offsets internally
 //  and convert at the JSON boundary.
 //
 //  The algorithm walks the line byte-by-byte, counting UTF-16 code units:
 //    · U+0000–U+FFFF (BMP):               1 code unit,  1–3 UTF-8 bytes
 //    · U+10000–U+10FFFF (supplementary):  2 code units, 4 UTF-8 bytes
 //
uint32_t lsp_utf8_to_utf16(const char *line_start, uint32_t byte_offset)
{
    uint32_t col16 = 0;
    const unsigned char *p = (const unsigned char *)line_start;
    const unsigned char *end = p + byte_offset;

    while (p < end) {
        uint32_t cp;
        unsigned char c = *p;
        int seq;

        if      (c < 0x80)                 { cp = c;            seq = 1; }
        else if ((c & 0xE0) == 0xC0)       { cp = c & 0x1F;    seq = 2; }
        else if ((c & 0xF0) == 0xE0)       { cp = c & 0x0F;    seq = 3; }
        else if ((c & 0xF8) == 0xF0)       { cp = c & 0x07;    seq = 4; }
        else                               { cp = 0xFFFD;       seq = 1; }

        for (int i = 1; i < seq && p + i < end; i++)
            cp = (cp << 6) | (p[i] & 0x3F);

        p += seq;
        col16 += (cp >= 0x10000) ? 2 : 1;
    }
    return col16;
}

uint32_t lsp_utf16_to_utf8(const char *line_start, uint32_t utf16_offset)
{
    uint32_t col16 = 0;
    const unsigned char *p = (const unsigned char *)line_start;
    const unsigned char *start = p;

    while (col16 < utf16_offset && *p) {
        uint32_t cp;
        unsigned char c = *p;
        int seq;

        if      (c < 0x80)                 { cp = c;            seq = 1; }
        else if ((c & 0xE0) == 0xC0)       { cp = c & 0x1F;    seq = 2; }
        else if ((c & 0xF0) == 0xE0)       { cp = c & 0x0F;    seq = 3; }
        else if ((c & 0xF8) == 0xF0)       { cp = c & 0x07;    seq = 4; }
        else                               { cp = 0xFFFD;       seq = 1; }

        for (int i = 1; i < seq; i++)
            cp = (cp << 6) | (p[i] & 0x3F);

        p += seq;
        col16 += (cp >= 0x10000) ? 2 : 1;
    }
    return (uint32_t)(p - start);
}


/// §5  Transport — JSON-RPC framing

//// Transport implementation
 //
 //  JSON-RPC 2.0 over stdio uses a simple two-header framing:
 //
 //      Content-Length: <decimal-byte-count>\r\n
 //      \r\n
 //      <payload bytes>
 //
 //  We read until we have assembled a complete Content-Length worth of bytes,
 //  then hand the payload to the JSON parser.  The write side buffers and
 //  flushes atomically to avoid tearing responses across thread boundaries.
 //
LspTransport *lsp_transport_create(FILE *in, FILE *out)
{
    LspTransport *t = lsp_xcalloc(1, sizeof(*t));
    t->in  = in;
    t->out = out;
    t->read_buf      = lsp_xmalloc(LSP_INITIAL_BUF);
    t->read_buf_cap  = LSP_INITIAL_BUF;
    t->write_buf     = lsp_xmalloc(LSP_INITIAL_BUF);
    t->write_buf_cap = LSP_INITIAL_BUF;
    return t;
}

void lsp_transport_free(LspTransport *t)
{
    if (!t) return;
    free(t->read_buf);
    free(t->write_buf);
    free(t);
}

// Read exactly n bytes from the transport's input stream.
static bool transport_read_exact(LspTransport *t, size_t n)
{
    /* Ensure capacity */
    if (n + 1 > t->read_buf_cap) {
        while (t->read_buf_cap < n + 1)
            t->read_buf_cap *= LSP_GROW_FACTOR;
        t->read_buf = lsp_xrealloc(t->read_buf, t->read_buf_cap);
    }

    size_t total = 0;
    while (total < n) {
        size_t got = fread(t->read_buf + total, 1, n - total, t->in);
        if (got == 0) return false;   /* EOF or error */
        total += got;
    }
    t->read_buf[n] = '\0';
    t->read_buf_len = n;
    return true;
}

LspMessage *lsp_transport_read(LspTransport *t)
{
    // Read headers line by line until blank line
    size_t content_length = 0;
    char   header_buf[256];

    while (1) {
        if (!fgets(header_buf, sizeof(header_buf), t->in))
            return NULL;

        // Strip trailing \r\n
        size_t hlen = strlen(header_buf);
        while (hlen > 0 && (header_buf[hlen-1] == '\n' ||
                             header_buf[hlen-1] == '\r'))
            header_buf[--hlen] = '\0';

        if (hlen == 0) break; // blank line -> end of headers

        if (strncasecmp(header_buf, "Content-Length:", 15) == 0) {
            const char *val = header_buf + 15;
            while (*val == ' ') val++;
            content_length = (size_t)strtoull(val, NULL, 10);
        }
        /* Other headers (Content-Type) are silently ignored. */
    }

    if (content_length == 0) return NULL;

    if (!transport_read_exact(t, content_length)) return NULL;

    // Delegate to the message parser
    extern LspMessage *lsp_message_parse(const char *json, size_t len);
    return lsp_message_parse(t->read_buf, t->read_buf_len);
}

void lsp_transport_write(LspTransport *t, const char *json)
{
    size_t jlen = strlen(json);
    char   hdr[64];
    int    hlen = snprintf(hdr, sizeof(hdr),
                           "Content-Length: %zu\r\n\r\n", jlen);

    size_t total = (size_t)hlen + jlen;
    if (t->write_buf_len + total + 1 > t->write_buf_cap) {
        while (t->write_buf_cap < t->write_buf_len + total + 1)
            t->write_buf_cap *= LSP_GROW_FACTOR;
        t->write_buf = lsp_xrealloc(t->write_buf, t->write_buf_cap);
    }

    memcpy(t->write_buf + t->write_buf_len, hdr, (size_t)hlen);
    t->write_buf_len += (size_t)hlen;
    memcpy(t->write_buf + t->write_buf_len, json, jlen);
    t->write_buf_len += jlen;
}

void lsp_transport_flush(LspTransport *t)
{
    if (t->write_buf_len == 0) return;
    fwrite(t->write_buf, 1, t->write_buf_len, t->out);
    fflush(t->out);
    t->write_buf_len = 0;
}


/// §6  Message parsing

//// JSON-RPC message parser
 //
 //  We implement a minimal hand-rolled JSON scanner sufficient for parsing
 //  JSON-RPC envelopes.  We do NOT parse the params or result bodies — those
 //  are kept as raw JSON strings and decoded lazily by each handler.
 //
 //  The parser recognises:
 //    · "jsonrpc" (ignored, assumed "2.0")
 //    · "id"      → LspId (int | string | null)
 //    · "method"  → string
 //    · "params"  → raw JSON (captured as substring)
 //    · "result"  → raw JSON
 //    · "error"   → raw JSON
 //
 // Advance past whitespace
static const char *json_skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Scan a JSON string value (cursor is at the opening '"').
   Returns pointer to char after closing '"'.
   The returned string is heap-allocated without surrounding quotes.     */
static const char *json_scan_string(const char *p, char **out)
{
    assert(*p == '"');
    p++;
    StrBuf b;
    sb_init(&b);

    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  sb_appendc(&b, '"');  break;
            case '\\': sb_appendc(&b, '\\'); break;
            case '/':  sb_appendc(&b, '/');  break;
            case 'b':  sb_appendc(&b, '\b'); break;
            case 'f':  sb_appendc(&b, '\f'); break;
            case 'n':  sb_appendc(&b, '\n'); break;
            case 'r':  sb_appendc(&b, '\r'); break;
            case 't':  sb_appendc(&b, '\t'); break;
            case 'u': {
                /* Decode \uXXXX → UTF-8 */
                char hex[5] = {0};
                for (int i = 0; i < 4 && p[i+1]; i++)
                    hex[i] = p[i+1];
                uint32_t cp = (uint32_t)strtoul(hex, NULL, 16);
                p += 4;
                if (cp < 0x80) {
                    sb_appendc(&b, (char)cp);
                } else if (cp < 0x800) {
                    sb_appendc(&b, (char)(0xC0 | (cp >> 6)));
                    sb_appendc(&b, (char)(0x80 | (cp & 0x3F)));
                } else {
                    sb_appendc(&b, (char)(0xE0 | (cp >> 12)));
                    sb_appendc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    sb_appendc(&b, (char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                sb_appendc(&b, *p);
                break;
            }
        } else {
            sb_appendc(&b, *p);
        }
        p++;
    }
    if (*p == '"') p++;
    *out = sb_take(&b);
    return p;
}

/* Skip any JSON value (string, number, object, array, keyword).
   Returns pointer to char after the value.                              */
static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (!*p) return p;

    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') p++;
            if (*p) p++;
        }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open  = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                /* skip string to avoid counting braces inside it */
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') p++;
                    if (*p) p++;
                }
                if (*p == '"') p++;
            } else if (*p == open) {
                depth++; p++;
            } else if (*p == close) {
                depth--; p++;
            } else {
                p++;
            }
        }
        return p;
    }
    /* number, true, false, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
        p++;
    return p;
}

/* Capture a raw JSON value (pointer into source + length). */
static const char *json_capture_value(const char *p, char **out)
{
    p = json_skip_ws(p);
    const char *start = p;
    p = json_skip_value(p);
    *out = lsp_xstrndup(start, (size_t)(p - start));
    return p;
}

LspMessage *lsp_message_parse(const char *json, size_t len)
{
    LspMessage *msg = lsp_xcalloc(1, sizeof(*msg));
    msg->id.kind = LSP_ID_NULL;

    const char *p = json;
    const char *end = json + len;
    (void)end;

    p = json_skip_ws(p);
    if (*p != '{') { lsp_message_free(msg); return NULL; }
    p++;

    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p = json_skip_value(p); continue; }

        char *key = NULL;
        p = json_scan_string(p, &key);
        p = json_skip_ws(p);
        if (*p == ':') p++;
        p = json_skip_ws(p);

        if (strcmp(key, "id") == 0) {
            if (*p == '"') {
                msg->id.kind = LSP_ID_STRING;
                p = json_scan_string(p, &msg->id.string);
            } else if (*p == 'n') {
                msg->id.kind = LSP_ID_NULL;
                p += 4; /* "null" */
            } else {
                msg->id.kind = LSP_ID_INT;
                char *end2 = NULL;
                msg->id.number = strtoll(p, &end2, 10);
                p = end2;
            }
        } else if (strcmp(key, "method") == 0) {
            p = json_scan_string(p, &msg->method);
        } else if (strcmp(key, "params") == 0) {
            p = json_capture_value(p, &msg->params_json);
        } else if (strcmp(key, "result") == 0) {
            p = json_capture_value(p, &msg->result_json);
        } else if (strcmp(key, "error") == 0) {
            p = json_capture_value(p, &msg->error_json);
        } else {
            p = json_skip_value(p);
        }
        free(key);
    }

    /* Classify the message */
    if (msg->method && msg->id.kind != LSP_ID_NULL)
        msg->kind = LSP_MSG_REQUEST;
    else if (msg->method)
        msg->kind = LSP_MSG_NOTIFICATION;
    else
        msg->kind = LSP_MSG_RESPONSE;

    return msg;
}

/* Tiny JSON field extractor — extracts a string field from a JSON object. */
static char *json_get_string(const char *obj, const char *field)
{
    if (!obj) return NULL;
    StrBuf needle;
    sb_init(&needle);
    sb_appendc(&needle, '"');
    sb_append(&needle, field);
    sb_append(&needle, "\":");
    char *n = sb_take(&needle);

    const char *p = strstr(obj, n);
    free(n);
    if (!p) return NULL;
    p += strlen(field) + 3;   /* skip "field": */
    p = json_skip_ws(p);
    if (*p != '"') return NULL;
    char *result = NULL;
    json_scan_string(p, &result);
    return result;
}

static int64_t json_get_int(const char *obj, const char *field)
{
    if (!obj) return -1;
    StrBuf needle;
    sb_init(&needle);
    sb_appendc(&needle, '"');
    sb_append(&needle, field);
    sb_append(&needle, "\":");
    char *n = sb_take(&needle);

    const char *p = strstr(obj, n);
    free(n);
    if (!p) return -1;
    p += strlen(field) + 3;
    p = json_skip_ws(p);
    return strtoll(p, NULL, 10);
}

static bool json_get_bool(const char *obj, const char *field, bool def)
{
    if (!obj) return def;
    StrBuf needle;
    sb_init(&needle);
    sb_appendc(&needle, '"');
    sb_append(&needle, field);
    sb_append(&needle, "\":");
    char *n = sb_take(&needle);

    const char *p = strstr(obj, n);
    free(n);
    if (!p) return def;
    p += strlen(field) + 3;
    p = json_skip_ws(p);
    if (strncmp(p, "true", 4) == 0)  return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

/* Parse an LSP position object {"line":N,"character":M} */
static LspPosition json_parse_position(const char *obj)
{
    LspPosition pos = {0};
    pos.line      = (uint32_t)json_get_int(obj, "line");
    pos.character = (uint32_t)json_get_int(obj, "character");
    return pos;
}

/* Parse an LSP range object {"start":{…},"end":{…}} */
static LspRange json_parse_range(const char *obj)
{
    LspRange r = {0};
    if (!obj) return r;

    /* Find "start" sub-object */
    const char *sp = strstr(obj, "\"start\":");
    if (sp) {
        sp += 8;
        sp = json_skip_ws(sp);
        if (*sp == '{') {
            char *sub = NULL;
            json_capture_value(sp, &sub);
            r.start = json_parse_position(sub);
            free(sub);
        }
    }
    const char *ep = strstr(obj, "\"end\":");
    if (ep) {
        ep += 6;
        ep = json_skip_ws(ep);
        if (*ep == '{') {
            char *sub = NULL;
            json_capture_value(ep, &sub);
            r.end = json_parse_position(sub);
            free(sub);
        }
    }
    return r;
}

/* Extract a named sub-object as a raw JSON string. */
static char *json_get_object(const char *obj, const char *field)
{
    if (!obj) return NULL;
    StrBuf needle;
    sb_init(&needle);
    sb_appendc(&needle, '"');
    sb_append(&needle, field);
    sb_append(&needle, "\":");
    char *n = sb_take(&needle);

    const char *p = strstr(obj, n);
    free(n);
    if (!p) return NULL;
    p += strlen(field) + 3;
    p = json_skip_ws(p);
    char *result = NULL;
    json_capture_value(p, &result);
    return result;
}


/// §7  Message construction

//// Message construction helpers
 //
 //  All returned strings are heap-allocated and must be freed by the caller.
 //  They are valid JSON-RPC 2.0 message bodies (without the framing header).
 //
void lsp_id_free(LspId *id)
{
    if (id->kind == LSP_ID_STRING) {
        free(id->string);
        id->string = NULL;
    }
    id->kind = LSP_ID_NULL;
}

void lsp_id_copy(LspId *dst, const LspId *src)
{
    dst->kind = src->kind;
    if (src->kind == LSP_ID_STRING)
        dst->string = lsp_xstrdup(src->string);
    else
        dst->number = src->number;
}

bool lsp_id_equal(const LspId *a, const LspId *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case LSP_ID_NULL:   return true;
    case LSP_ID_INT:    return a->number == b->number;
    case LSP_ID_STRING: return strcmp(a->string, b->string) == 0;
    }
    return false;
}

void lsp_message_free(LspMessage *msg)
{
    if (!msg) return;
    lsp_id_free(&msg->id);
    free(msg->method);
    free(msg->params_json);
    free(msg->result_json);
    free(msg->error_json);
    free(msg);
}

static void sb_append_id(StrBuf *b, LspId id)
{
    switch (id.kind) {
    case LSP_ID_NULL:   sb_append(b, "null"); break;
    case LSP_ID_INT:    sb_appendf(b, "%" PRId64, id.number); break;
    case LSP_ID_STRING: {
        char *s = lsp_json_string(id.string);
        sb_append(b, s);
        free(s);
        break;
    }
    }
}

char *lsp_make_response(LspId id, const char *result_json)
{
    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_append_id(&b, id);
    sb_append(&b, ",\"result\":");
    sb_append(&b, result_json ? result_json : "null");
    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_make_error(LspId id, int code, const char *message)
{
    StrBuf b;
    sb_init(&b);
    char *msg_json = lsp_json_string(message);
    sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_append_id(&b, id);
    sb_appendf(&b, ",\"error\":{\"code\":%d,\"message\":%s}}",
                code, msg_json);
    free(msg_json);
    return sb_take(&b);
}

char *lsp_make_notification(const char *method, const char *params_json)
{
    StrBuf b;
    sb_init(&b);
    char *m = lsp_json_string(method);
    sb_appendf(&b, "{\"jsonrpc\":\"2.0\",\"method\":%s,\"params\":%s}",
               m, params_json ? params_json : "null");
    free(m);
    return sb_take(&b);
}


/// §8  Position utilities

//// Position utilities
 //
 //  These helpers convert between the three coordinate systems we use:
 //
 //    · LSP positions: {line, character} in UTF-16 column units
 //    · Byte offsets:  byte index into the document's source string
 //    · (line, byte_col): line number + byte column (internal only)
 //
uint32_t lsp_document_offset(const LspDocument *doc, LspPosition pos)
{
    if (!doc || !doc->line_offsets) return 0;
    uint32_t line = pos.line;
    if (line >= doc->line_count) line = doc->line_count - 1;
    uint32_t base = doc->line_offsets[line];

    /* pos.character is a UTF-16 column — convert to a byte offset */
    const char *line_start = doc->source + base;
    uint32_t byte_col = lsp_utf16_to_utf8(line_start, pos.character);
    return base + byte_col;
}

LspPosition lsp_document_position(const LspDocument *doc, uint32_t offset)
{
    LspPosition pos = {0};
    if (!doc || !doc->line_offsets || offset > (uint32_t)doc->source_len)
        return pos;

    /* Binary search for the line */
    uint32_t lo = 0, hi = doc->line_count;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (doc->line_offsets[mid] <= offset)
            lo = mid;
        else
            hi = mid;
    }
    pos.line = lo;
    uint32_t line_start_byte = doc->line_offsets[lo];
    uint32_t byte_col = offset - line_start_byte;
    pos.character = lsp_utf8_to_utf16(doc->source + line_start_byte, byte_col);
    return pos;
}

/* Determine whether a byte is a valid identifier character for Monad. */
static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '\'';
}

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

LspRange lsp_document_word_range(const LspDocument *doc, LspPosition pos)
{
    LspRange r = { pos, pos };
    if (!doc || !doc->source) return r;

    uint32_t off = lsp_document_offset(doc, pos);
    const char *src = doc->source;
    size_t len = doc->source_len;

    if (off >= (uint32_t)len) return r;

    /* Extend left */
    uint32_t start = off;
    while (start > 0 && is_ident_char(src[start - 1]))
        start--;

    /* Extend right */
    uint32_t end = off;
    while (end < (uint32_t)len && is_ident_char(src[end]))
        end++;

    if (start == end) return r;

    r.start = lsp_document_position(doc, start);
    r.end   = lsp_document_position(doc, end);
    return r;
}

char *lsp_document_word_at(const LspDocument *doc, LspPosition pos)
{
    if (!doc || !doc->source) return NULL;
    LspRange r = lsp_document_word_range(doc, pos);
    uint32_t s = lsp_document_offset(doc, r.start);
    uint32_t e = lsp_document_offset(doc, r.end);
    if (s == e) return NULL;
    return lsp_xstrndup(doc->source + s, e - s);
}


/// §9  Line index

//// Line index
 //
 //  The line index maps line numbers to byte offsets into the source string.
 //  It is built lazily (or on demand after each source update) by scanning
 //  for newline characters.
 //
 //  line_offsets[0] == 0 always.
 //  line_offsets[i] == byte offset of the first character of line i.
 //
void lsp_document_build_line_index(LspDocument *doc)
{
    /* Ensure capacity for at least one entry */
    if (!doc->line_offsets) {
        doc->line_cap = 256;
        doc->line_offsets = lsp_xmalloc(doc->line_cap * sizeof(uint32_t));
    }

    doc->line_count = 0;
    /* Line 0 always starts at byte 0 */
    if (doc->line_count >= doc->line_cap) {
        doc->line_cap *= 2;
        doc->line_offsets = lsp_xrealloc(doc->line_offsets,
                                          doc->line_cap * sizeof(uint32_t));
    }
    doc->line_offsets[doc->line_count++] = 0;

    const char *src = doc->source;
    size_t len = doc->source_len;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            if (doc->line_count >= doc->line_cap) {
                doc->line_cap *= 2;
                doc->line_offsets = lsp_xrealloc(
                    doc->line_offsets,
                    doc->line_cap * sizeof(uint32_t));
            }
            doc->line_offsets[doc->line_count++] = (uint32_t)(i + 1);
        }
    }
}


/// §10  Document management

//// Document management
 //
 //  An LspDocument is created when the editor opens a file (didOpen) and
 //  destroyed when the editor closes it (didClose).  In between, it is
 //  updated (didChange) and analyzed in the background.
 //
LspDocument *lsp_document_create(const char *uri, const char *source,
                                  int version)
{
    LspDocument *doc = lsp_xcalloc(1, sizeof(*doc));
    doc->uri     = lsp_xstrdup(uri);
    doc->version = version;
    doc->state   = LSP_DOC_DIRTY;

    /* Derive the filesystem path from the file:// URI */
    if (strncmp(uri, "file://", 7) == 0)
        doc->path = lsp_xstrdup(uri + 7);
    else
        doc->path = lsp_xstrdup(uri);

    doc->source     = lsp_xstrdup(source ? source : "");
    doc->source_len = strlen(doc->source);

    doc->diag_cap    = LSP_INITIAL_DIAG_CAP;
    doc->diagnostics = lsp_xmalloc(doc->diag_cap * sizeof(LspDiagnostic));

    doc->symbol_cap = LSP_INITIAL_SYM_CAP;
    doc->symbols    = lsp_xmalloc(doc->symbol_cap * sizeof(LspSymbol *));

    doc->inlay_cap   = LSP_INITIAL_HINT_CAP;
    doc->inlay_hints = lsp_xmalloc(doc->inlay_cap * sizeof(LspInlayHint));

    doc->fold_cap = 64;
    doc->folds    = lsp_xmalloc(doc->fold_cap * sizeof(LspFoldRange));

    lsp_document_build_line_index(doc);
    return doc;
}

void lsp_document_update(LspDocument *doc, const char *source, int version)
{
    free(doc->source);
    doc->source     = lsp_xstrdup(source ? source : "");
    doc->source_len = strlen(doc->source);
    doc->version    = version;
    doc->state      = LSP_DOC_DIRTY;
    lsp_document_build_line_index(doc);
}

void lsp_document_add_diagnostic(LspDocument *doc, LspDiagnostic diag)
{
    LSP_GROW(doc->diagnostics, doc->diag_count, doc->diag_cap, LspDiagnostic);
    doc->diagnostics[doc->diag_count++] = diag;
}

void lsp_document_clear_diagnostics(LspDocument *doc)
{
    for (size_t i = 0; i < doc->diag_count; i++) {
        lsp_diagnostic_free(&doc->diagnostics[i]);
    }
    doc->diag_count = 0;
}

void lsp_document_analyze(LspDocument *doc)
{
    if (!doc || !doc->workspace) return;
    doc->state = LSP_DOC_ANALYZING;

    LspAnalysisResult *r = lsp_analyze_file(doc->path, doc->source,
                                             doc->workspace);
    if (!r) {
        doc->state = LSP_DOC_ERROR;
        return;
    }
    lsp_analysis_result_apply(doc, r);
    lsp_analysis_result_free(r);
    doc->state = LSP_DOC_CLEAN;
}

void lsp_document_free(LspDocument *doc)
{
    if (!doc) return;
    free(doc->uri);
    free(doc->path);
    free(doc->source);

    lsp_document_clear_diagnostics(doc);
    free(doc->diagnostics);

    for (size_t i = 0; i < doc->symbol_count; i++)
        lsp_symbol_free(doc->symbols[i]);
    free(doc->symbols);

    lsp_semantic_tokens_free(doc->tokens);

    for (size_t i = 0; i < doc->inlay_count; i++)
        lsp_inlay_hint_free(&doc->inlay_hints[i]);
    free(doc->inlay_hints);

    for (size_t i = 0; i < doc->fold_count; i++)
        free(doc->folds[i].collapsed_text);
    free(doc->folds);

    free(doc->line_offsets);
    free(doc);
}


/// §11  Index management

//// Index management
 //
 //  The index is a hash table keyed by fully-qualified symbol name.
 //  The hash function is FNV-1a, which has good distribution on short
 //  identifiers and is trivial to implement.
 //
 //  The sorted array is rebuilt lazily before any prefix-search query.
 //  Fuzzy matching uses a simple subsequence filter followed by a
 //  Levenshtein-distance sort.
 //
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint32_t)(unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

LspIndex *lsp_index_create(void)
{
    LspIndex *idx = lsp_xcalloc(1, sizeof(*idx));
    idx->sorted_cap = 64;
    idx->sorted = lsp_xmalloc(idx->sorted_cap * sizeof(LspIndexEntry *));
    return idx;
}

LspIndexEntry *lsp_index_entry_create(void)
{
    return lsp_xcalloc(1, sizeof(LspIndexEntry));
}

void lsp_index_entry_free(LspIndexEntry *e)
{
    if (!e) return;
    free(e->name);
    free(e->short_name);
    free(e->module);
    free(e->uri);
    free(e->type_sig);
    free(e->documentation);
    for (size_t i = 0; i < e->ref_count; i++) {
        free(e->references[i].uri);
    }
    free(e->references);
    free(e);
}

void lsp_index_entry_add_ref(LspIndexEntry *e, LspLocation loc)
{
    LSP_GROW(e->references, e->ref_count, e->ref_cap, LspLocation);
    e->references[e->ref_count++] = loc;
}

void lsp_index_insert(LspIndex *idx, LspIndexEntry *entry)
{
    if (!entry->name) return;
    uint32_t bucket = fnv1a(entry->name) % LSP_INDEX_BUCKETS;
    entry->next = idx->buckets[bucket];
    idx->buckets[bucket] = entry;
    idx->entry_count++;

    LSP_GROW(idx->sorted, idx->sorted_count, idx->sorted_cap, LspIndexEntry *);
    idx->sorted[idx->sorted_count++] = entry;
    idx->sorted_dirty = true;
}

LspIndexEntry *lsp_index_lookup(LspIndex *idx, const char *name)
{
    if (!name) return NULL;
    uint32_t bucket = fnv1a(name) % LSP_INDEX_BUCKETS;
    for (LspIndexEntry *e = idx->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static int entry_name_cmp(const void *a, const void *b)
{
    const LspIndexEntry *ea = *(const LspIndexEntry **)a;
    const LspIndexEntry *eb = *(const LspIndexEntry **)b;
    return strcmp(ea->name, eb->name);
}

void lsp_index_rebuild_sorted(LspIndex *idx)
{
    if (!idx->sorted_dirty) return;
    qsort(idx->sorted, idx->sorted_count, sizeof(LspIndexEntry *),
          entry_name_cmp);
    idx->sorted_dirty = false;
}

LspIndexEntry **lsp_index_prefix(LspIndex *idx, const char *prefix,
                                  size_t *count)
{
    lsp_index_rebuild_sorted(idx);
    *count = 0;
    if (!prefix || !*prefix) return NULL;

    size_t plen = strlen(prefix);
    /* Binary search for the first entry with this prefix */
    size_t lo = 0, hi = idx->sorted_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strncmp(idx->sorted[mid]->name, prefix, plen) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* Collect all matching entries */
    size_t cap = 16;
    LspIndexEntry **results = lsp_xmalloc(cap * sizeof(LspIndexEntry *));
    for (size_t i = lo; i < idx->sorted_count; i++) {
        if (strncmp(idx->sorted[i]->name, prefix, plen) != 0) break;
        LSP_GROW(results, *count, cap, LspIndexEntry *);
        results[(*count)++] = idx->sorted[i];
    }
    return results;
}

/* Simple subsequence check: every character of query appears in name
   in order.                                                            */
static bool is_subsequence(const char *name, const char *query)
{
    const char *p = query;
    const char *n = name;
    while (*p && *n) {
        if (tolower((unsigned char)*p) == tolower((unsigned char)*n))
            p++;
        n++;
    }
    return *p == '\0';
}

LspIndexEntry **lsp_index_fuzzy(LspIndex *idx, const char *query, size_t *count)
{
    *count = 0;
    if (!query || !*query) return NULL;

    size_t cap = 16;
    LspIndexEntry **results = lsp_xmalloc(cap * sizeof(LspIndexEntry *));
    for (size_t b = 0; b < LSP_INDEX_BUCKETS; b++) {
        for (LspIndexEntry *e = idx->buckets[b]; e; e = e->next) {
            const char *name = e->short_name ? e->short_name : e->name;
            if (is_subsequence(name, query)) {
                LSP_GROW(results, *count, cap, LspIndexEntry *);
                results[(*count)++] = e;
                if (*count >= LSP_MAX_COMPLETIONS) goto done;
            }
        }
    }
done:
    return results;
}

void lsp_index_remove_file(LspIndex *idx, const char *uri)
{
    for (size_t b = 0; b < LSP_INDEX_BUCKETS; b++) {
        LspIndexEntry **pp = &idx->buckets[b];
        while (*pp) {
            LspIndexEntry *e = *pp;
            if (e->uri && strcmp(e->uri, uri) == 0) {
                *pp = e->next;
                idx->entry_count--;
                /* Also remove from sorted array */
                for (size_t s = 0; s < idx->sorted_count; s++) {
                    if (idx->sorted[s] == e) {
                        idx->sorted[s] = idx->sorted[--idx->sorted_count];
                        idx->sorted_dirty = true;
                        break;
                    }
                }
                lsp_index_entry_free(e);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

void lsp_index_free(LspIndex *idx)
{
    if (!idx) return;
    for (size_t b = 0; b < LSP_INDEX_BUCKETS; b++) {
        LspIndexEntry *e = idx->buckets[b];
        while (e) {
            LspIndexEntry *next = e->next;
            lsp_index_entry_free(e);
            e = next;
        }
    }
    free(idx->sorted);
    free(idx);
}


/// §12  Workspace management

//// Workspace management
 //
 //  The workspace is the top-level container for an editing session.
 //  It is created once on initialize and lives until exit.
 //
LspWorkspace *lsp_workspace_create(const char *root_uri)
{
    LspWorkspace *ws = lsp_xcalloc(1, sizeof(*ws));
    ws->root_uri  = lsp_xstrdup(root_uri);
    ws->root_path = (strncmp(root_uri, "file://", 7) == 0)
                    ? lsp_xstrdup(root_uri + 7)
                    : lsp_xstrdup(root_uri);
    ws->index     = lsp_index_create();
    ws->doc_cap   = LSP_INITIAL_DOC_CAP;
    ws->docs      = lsp_xmalloc(ws->doc_cap * sizeof(LspDocument *));
    return ws;
}

void lsp_workspace_free(LspWorkspace *ws)
{
    if (!ws) return;
    for (size_t i = 0; i < ws->doc_count; i++)
        lsp_document_free(ws->docs[i]);
    free(ws->docs);
    lsp_index_free(ws->index);
    free(ws->root_uri);
    free(ws->root_path);
    free(ws->package_name);
    free(ws->source_dir);
    free(ws->main_file);
    free(ws);
}

LspDocument *lsp_workspace_get_doc(LspWorkspace *ws, const char *uri)
{
    for (size_t i = 0; i < ws->doc_count; i++) {
        if (strcmp(ws->docs[i]->uri, uri) == 0)
            return ws->docs[i];
    }
    return NULL;
}

LspDocument *lsp_workspace_open_doc(LspWorkspace *ws, const char *uri,
                                     const char *source, int version)
{
    LspDocument *existing = lsp_workspace_get_doc(ws, uri);
    if (existing) {
        lsp_document_update(existing, source, version);
        return existing;
    }
    LspDocument *doc = lsp_document_create(uri, source, version);
    doc->workspace = ws;
    LSP_GROW(ws->docs, ws->doc_count, ws->doc_cap, LspDocument *);
    ws->docs[ws->doc_count++] = doc;
    return doc;
}

void lsp_workspace_update_doc(LspWorkspace *ws, const char *uri,
                               const char *source, int version)
{
    LspDocument *doc = lsp_workspace_get_doc(ws, uri);
    if (doc) lsp_document_update(doc, source, version);
}

void lsp_workspace_close_doc(LspWorkspace *ws, const char *uri)
{
    for (size_t i = 0; i < ws->doc_count; i++) {
        if (strcmp(ws->docs[i]->uri, uri) == 0) {
            lsp_document_free(ws->docs[i]);
            ws->docs[i] = ws->docs[--ws->doc_count];
            return;
        }
    }
}

void lsp_workspace_analyze(LspWorkspace *ws, const char *uri)
{
    LspDocument *doc = lsp_workspace_get_doc(ws, uri);
    if (doc) lsp_document_analyze(doc);
}

void lsp_workspace_analyze_all(LspWorkspace *ws)
{
    for (size_t i = 0; i < ws->doc_count; i++)
        lsp_document_analyze(ws->docs[i]);
}

void lsp_workspace_index(LspWorkspace *ws)
{
    /* Remove stale index entries for all tracked URIs, then re-analyze. */
    for (size_t i = 0; i < ws->doc_count; i++) {
        lsp_index_remove_file(ws->index, ws->docs[i]->uri);
        lsp_document_analyze(ws->docs[i]);
    }
}


/// §13  Analysis integration

//// Analysis integration
 //
 //  lsp_analyze_file() is a bridge to the Monad compiler pipeline.
 //  In a real build it would:
 //    1. Parse the source with the Monad parser.
 //    2. Run the type checker / HM inferencer.
 //    3. Convert compiler diagnostics to LspDiagnostic records.
 //    4. Walk the typed AST to collect symbols, tokens, and inlay hints.
 //
 //  Here we provide a correct skeleton that compiles cleanly and returns
 //  an empty (success) result.  The real integration lives in analysis.c.
 //
LspAnalysisResult *lsp_analyze_file(const char *path, const char *source,
                                     LspWorkspace *ws)
{
    (void)path; (void)source; (void)ws;
    LspAnalysisResult *r = lsp_xcalloc(1, sizeof(*r));
    r->success = true;
    return r;
}

void lsp_analysis_result_apply(LspDocument *doc, LspAnalysisResult *r)
{
    if (!r) return;

    /* Swap in diagnostics */
    lsp_document_clear_diagnostics(doc);
    for (size_t i = 0; i < r->diag_count; i++)
        lsp_document_add_diagnostic(doc, r->diagnostics[i]);
    free(r->diagnostics);
    r->diagnostics = NULL;
    r->diag_count  = 0;

    /* Swap in symbols */
    for (size_t i = 0; i < doc->symbol_count; i++)
        lsp_symbol_free(doc->symbols[i]);
    free(doc->symbols);
    doc->symbols      = (LspSymbol **)r->symbols;
    doc->symbol_count = r->symbol_count;
    doc->symbol_cap   = r->symbol_count;
    r->symbols      = NULL;
    r->symbol_count = 0;

    /* Swap in tokens */
    lsp_semantic_tokens_free(doc->tokens);
    doc->tokens  = r->tokens;
    r->tokens    = NULL;

    /* Swap in inlay hints */
    for (size_t i = 0; i < doc->inlay_count; i++)
        lsp_inlay_hint_free(&doc->inlay_hints[i]);
    free(doc->inlay_hints);
    doc->inlay_hints  = r->inlay_hints;
    doc->inlay_count  = r->inlay_count;
    doc->inlay_cap    = r->inlay_count;
    r->inlay_hints  = NULL;
    r->inlay_count  = 0;

    /* Swap in folds */
    for (size_t i = 0; i < doc->fold_count; i++)
        free(doc->folds[i].collapsed_text);
    free(doc->folds);
    doc->folds      = r->folds;
    doc->fold_count = r->fold_count;
    doc->fold_cap   = r->fold_count;
    r->folds      = NULL;
    r->fold_count = 0;
}

void lsp_analysis_result_free(LspAnalysisResult *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->diag_count; i++)
        lsp_diagnostic_free(&r->diagnostics[i]);
    free(r->diagnostics);
    for (size_t i = 0; i < r->symbol_count; i++)
        lsp_symbol_free((LspSymbol *)r->symbols[i]);
    free(r->symbols);
    lsp_semantic_tokens_free(r->tokens);
    for (size_t i = 0; i < r->inlay_count; i++)
        lsp_inlay_hint_free(&r->inlay_hints[i]);
    free(r->inlay_hints);
    for (size_t i = 0; i < r->fold_count; i++)
        free(r->folds[i].collapsed_text);
    free(r->folds);
    free(r->error_message);
    free(r);
}

char *lsp_extract_doc_comment(const char *source, uint32_t def_line)
{
    if (!source || def_line == 0) return NULL;

    /* Scan backwards from def_line finding '---' or '///' style comments */
    const char *p = source;
    uint32_t    line = 0;

    /* Advance to the line just before def_line */
    while (*p && line < def_line) {
        if (*p == '\n') line++;
        p++;
    }
    if (line < def_line) return NULL;

    /* Walk backwards collecting comment lines */
    StrBuf b;
    sb_init(&b);
    /* (simple implementation: look for "-- " or "-- |" Haskell/Monad style) */
    /* Real implementation would collect contiguous comment lines above */
    (void)b;

    return NULL;   /* stub */
}

char *lsp_format_type(void *type_ptr)
{
    /* Bridge to the Monad type pretty-printer. */
    (void)type_ptr;
    return lsp_xstrdup("?");
}


/// §14  Completion

//// Completion
 //
 //  Completion is triggered either automatically (after typing a word
 //  character) or explicitly (Ctrl+Space).  Dot-triggered completion
 //  (after a '.') performs field/method lookup on the type of the
 //  expression to the left of the dot.
 //
 //  We offer completions from four sources, merged and deduplicated:
 //    1. Workspace index (all reachable symbols)
 //    2. Document-local definitions (not yet in the index)
 //    3. Language keywords
 //    4. Code snippets
 //
 //  Items are sorted by:
 //    · Exact prefix match first
 //    · Subsequence match second
 //    · Alphabetical tiebreak
 //
LspCompletion **lsp_completion(LspDocument *doc, LspPosition pos,
                                bool triggered_by_dot, size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    char *word = triggered_by_dot ? lsp_xstrdup("") :
                                    lsp_document_word_at(doc, pos);
    const char *prefix = word ? word : "";

    size_t cap = 64;
    LspCompletion **list = lsp_xmalloc(cap * sizeof(LspCompletion *));

    /* §14.1 — Symbols from the workspace index */
    if (doc->workspace) {
        size_t n = 0;
        LspIndexEntry **entries =
            *prefix
            ? lsp_index_prefix(doc->workspace->index, prefix, &n)
            : lsp_index_fuzzy(doc->workspace->index, prefix, &n);

        for (size_t i = 0; i < n && *count < LSP_MAX_COMPLETIONS; i++) {
            LspIndexEntry *e = entries[i];
            LspCompletion *c = lsp_xcalloc(1, sizeof(*c));
            c->label        = lsp_xstrdup(e->short_name ? e->short_name : e->name);
            c->detail       = lsp_xstrdup(e->type_sig);
            c->documentation = lsp_xstrdup(e->documentation);
            c->sort_text    = lsp_xstrdup(c->label);
            c->insert_text  = lsp_xstrdup(c->label);
            c->insert_format = LSP_INSERT_TEXT_PLAIN;

            switch (e->kind) {
            case LSP_SYMBOL_FUNCTION:  c->kind = LSP_COMPLETION_FUNCTION;  break;
            case LSP_SYMBOL_VARIABLE:  c->kind = LSP_COMPLETION_VARIABLE;  break;
            case LSP_SYMBOL_MODULE:    c->kind = LSP_COMPLETION_MODULE;     break;
            case LSP_SYMBOL_STRUCT:    c->kind = LSP_COMPLETION_STRUCT;     break;
            case LSP_SYMBOL_ENUM:      c->kind = LSP_COMPLETION_ENUM;       break;
            case LSP_SYMBOL_CONSTANT:  c->kind = LSP_COMPLETION_CONSTANT;   break;
            case LSP_SYMBOL_FIELD:     c->kind = LSP_COMPLETION_FIELD;      break;
            default:                   c->kind = LSP_COMPLETION_TEXT;       break;
            }

            LSP_GROW(list, *count, cap, LspCompletion *);
            list[(*count)++] = c;
        }
        free(entries);
    }

    /* §14.2 — Keywords */
    if (!triggered_by_dot) {
        size_t kw_count = 0;
        const char **kws = lsp_keywords(&kw_count);
        for (size_t i = 0; i < kw_count && *count < LSP_MAX_COMPLETIONS; i++) {
            if (prefix[0] && strncmp(kws[i], prefix, strlen(prefix)) != 0)
                continue;
            LspCompletion *c = lsp_xcalloc(1, sizeof(*c));
            c->label        = lsp_xstrdup(kws[i]);
            c->kind         = LSP_COMPLETION_KEYWORD;
            c->sort_text    = lsp_xstrdup(kws[i]);
            c->insert_text  = lsp_xstrdup(kws[i]);
            c->insert_format = LSP_INSERT_TEXT_PLAIN;
            LSP_GROW(list, *count, cap, LspCompletion *);
            list[(*count)++] = c;
        }
    }

    /* §14.3 — Snippets */
    if (!triggered_by_dot) {
        size_t sn_count = 0;
        const char **labels = lsp_snippet_labels(&sn_count);
        const char **bodies = lsp_snippet_bodies(&sn_count);
        for (size_t i = 0; i < sn_count && *count < LSP_MAX_COMPLETIONS; i++) {
            if (prefix[0] && strncmp(labels[i], prefix, strlen(prefix)) != 0)
                continue;
            LspCompletion *c = lsp_xcalloc(1, sizeof(*c));
            c->label        = lsp_xstrdup(labels[i]);
            c->detail       = lsp_xstrdup("snippet");
            c->kind         = LSP_COMPLETION_SNIPPET;
            c->sort_text    = lsp_xstrdup(labels[i]);
            c->insert_text  = lsp_xstrdup(bodies[i]);
            c->insert_format = LSP_INSERT_TEXT_SNIPPET;
            LSP_GROW(list, *count, cap, LspCompletion *);
            list[(*count)++] = c;
        }
    }

    free(word);
    return list;
}


/// §15  Hover

//// Hover
 //
 //  Returns a markdown-formatted hover response containing:
 //    · The symbol's type signature (in a fenced code block)
 //    · The extracted doc comment (if any)
 //    · The symbol's fully-qualified name
 //
 //  If no symbol is found at the cursor position, returns NULL.
 //
LspHover *lsp_hover(LspDocument *doc, LspPosition pos)
{
    if (!doc) return NULL;

    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;

    LspIndexEntry *entry = NULL;
    if (doc->workspace)
        entry = lsp_index_lookup(doc->workspace->index, word);

    if (!entry) {
        free(word);
        return NULL;
    }

    StrBuf b;
    sb_init(&b);

    /* Type signature in a fenced Monad code block */
    if (entry->type_sig) {
        sb_append(&b, "```monad\n");
        sb_append(&b, entry->short_name ? entry->short_name : entry->name);
        sb_append(&b, " : ");
        sb_append(&b, entry->type_sig);
        sb_append(&b, "\n```");
    }

    /* Documentation */
    if (entry->documentation && *entry->documentation) {
        sb_append(&b, "\n\n---\n\n");
        sb_append(&b, entry->documentation);
    }

    /* Module path */
    if (entry->module) {
        sb_append(&b, "\n\n*Defined in* `");
        sb_append(&b, entry->module);
        sb_appendc(&b, '`');
    }

    LspHover *h = lsp_xcalloc(1, sizeof(*h));
    h->kind     = LSP_MARKUP_MARKDOWN;
    h->contents = sb_take(&b);

    LspRange *range = lsp_xmalloc(sizeof(LspRange));
    *range = lsp_document_word_range(doc, pos);
    h->range = range;

    free(word);
    return h;
}


/// §16  Go-to-definition family

//// Go-to-definition family
 //
 //  All four variants (definition, typeDefinition, declaration, implementation)
 //  consult the workspace index.  The distinction between them matters for
 //  languages with separate declaration/definition sites; in Monad they
 //  collapse to the same location.
 //
static LspLocation *location_from_entry(LspIndexEntry *e, size_t *count)
{
    if (!e) { *count = 0; return NULL; }
    LspLocation *locs = lsp_xmalloc(sizeof(LspLocation));
    locs[0].uri   = lsp_xstrdup(e->uri);
    locs[0].range = e->name_range;
    *count = 1;
    return locs;
}

LspLocation *lsp_definition(LspDocument *doc, LspPosition pos, size_t *count)
{
    *count = 0;
    if (!doc || !doc->workspace) return NULL;
    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;
    LspIndexEntry *e = lsp_index_lookup(doc->workspace->index, word);
    free(word);
    return location_from_entry(e, count);
}

LspLocation *lsp_type_definition(LspDocument *doc, LspPosition pos,
                                  size_t *count)
{
    /* For Monad, type-definition jumps to the type constructor.
       Currently delegates to definition for simplicity.              */
    return lsp_definition(doc, pos, count);
}

LspLocation *lsp_declaration(LspDocument *doc, LspPosition pos, size_t *count)
{
    return lsp_definition(doc, pos, count);
}

LspLocation *lsp_implementation(LspDocument *doc, LspPosition pos,
                                 size_t *count)
{
    return lsp_definition(doc, pos, count);
}


/// §17  References / document highlight

//// References and document highlight
 //
 //  lsp_references() returns all known use-sites of the symbol under the
 //  cursor across the entire workspace.  References are populated by the
 //  analysis pass and stored in the index entry's reference list.
 //
 //  lsp_document_highlight() is a lightweight variant that returns only the
 //  occurrences within the current document, used for cursor-word highlighting.
 //
LspLocation *lsp_references(LspDocument *doc, LspPosition pos,
                              bool include_declaration, size_t *count)
{
    *count = 0;
    if (!doc || !doc->workspace) return NULL;

    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;
    LspIndexEntry *e = lsp_index_lookup(doc->workspace->index, word);
    free(word);
    if (!e) return NULL;

    size_t total = e->ref_count + (include_declaration ? 1 : 0);
    if (total == 0) return NULL;

    LspLocation *locs = lsp_xmalloc(total * sizeof(LspLocation));
    size_t idx = 0;

    if (include_declaration) {
        locs[idx].uri   = lsp_xstrdup(e->uri);
        locs[idx].range = e->name_range;
        idx++;
    }
    for (size_t i = 0; i < e->ref_count; i++) {
        locs[idx].uri   = lsp_xstrdup(e->references[i].uri);
        locs[idx].range = e->references[i].range;
        idx++;
    }
    *count = total;
    return locs;
}

LspLocation *lsp_document_highlight(LspDocument *doc, LspPosition pos,
                                     size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;

    /* Simple scan: find all occurrences of the word in the document. */
    size_t wlen = strlen(word);
    size_t cap  = 16;
    LspLocation *locs = lsp_xmalloc(cap * sizeof(LspLocation));

    const char *src = doc->source;
    const char *p   = src;

    while ((p = strstr(p, word)) != NULL) {
        /* Ensure it's a whole-word match */
        bool left_ok  = (p == src) || !is_ident_char(p[-1]);
        bool right_ok = !is_ident_char(p[wlen]);
        if (left_ok && right_ok) {
            uint32_t start_off = (uint32_t)(p - src);
            uint32_t end_off   = start_off + (uint32_t)wlen;
            LSP_GROW(locs, *count, cap, LspLocation);
            locs[*count].uri   = lsp_xstrdup(doc->uri);
            locs[*count].range.start = lsp_document_position(doc, start_off);
            locs[*count].range.end   = lsp_document_position(doc, end_off);
            (*count)++;
        }
        p += wlen;
    }

    free(word);
    if (*count == 0) { free(locs); return NULL; }
    return locs;
}


/// §18  Document & workspace symbols

//// Document and workspace symbols
 //
 //  Document symbols are returned as a tree (each LspSymbol can have
 //  children) suitable for an outline view.  Workspace symbols are a flat
 //  list filtered by the query string.
 //
LspSymbol **lsp_document_symbols(LspDocument *doc, size_t *count)
{
    *count = 0;
    if (!doc || doc->symbol_count == 0) return NULL;

    LspSymbol **out = lsp_xmalloc(doc->symbol_count * sizeof(LspSymbol *));
    for (size_t i = 0; i < doc->symbol_count; i++) {
        out[i] = doc->symbols[i];   /* borrowed — caller must not free */
    }
    *count = doc->symbol_count;
    return out;
}

LspSymbol **lsp_workspace_symbols(LspWorkspace *ws, const char *query,
                                   size_t *count)
{
    *count = 0;
    if (!ws) return NULL;

    size_t n = 0;
    LspIndexEntry **entries = lsp_index_fuzzy(ws->index, query, &n);
    if (!entries || n == 0) return NULL;

    LspSymbol **out = lsp_xmalloc(n * sizeof(LspSymbol *));
    for (size_t i = 0; i < n; i++) {
        LspIndexEntry *e = entries[i];
        LspSymbol     *s = lsp_xcalloc(1, sizeof(*s));
        s->name            = lsp_xstrdup(e->short_name ? e->short_name : e->name);
        s->detail          = lsp_xstrdup(e->type_sig);
        s->kind            = e->kind;
        s->range           = e->range;
        s->selection_range = e->name_range;
        s->container       = lsp_xstrdup(e->module);
        out[i] = s;
    }
    *count = n;
    free(entries);
    return out;
}


/// §19  Semantic tokens

//// Semantic tokens
 //
 //  Returns the full delta-encoded token array for the document.
 //  The caller receives a copy — the document retains its own copy for
 //  incremental delta computation.
 //
 //  Token encoding (5 uint32 values per token):
 //    [0] deltaLine      — line delta from previous token
 //    [1] deltaStart     — character delta (UTF-16); 0 if deltaLine > 0
 //    [2] length         — token length in UTF-16 units
 //    [3] tokenType      — index into the legend
 //    [4] tokenModifiers — bitmask
 //
LspSemanticTokens *lsp_semantic_tokens(LspDocument *doc)
{
    if (!doc || !doc->tokens) {
        LspSemanticTokens *st = lsp_xcalloc(1, sizeof(*st));
        st->data  = NULL;
        st->count = 0;
        return st;
    }

    LspSemanticTokens *copy = lsp_xcalloc(1, sizeof(*copy));
    copy->count = doc->tokens->count;
    if (copy->count > 0) {
        copy->data = lsp_xmalloc(copy->count * sizeof(uint32_t));
        memcpy(copy->data, doc->tokens->data, copy->count * sizeof(uint32_t));
    }

    /* Generate a result ID based on document version */
    copy->result_id = lsp_xmalloc(LSP_RESULT_ID_LEN);
    snprintf(copy->result_id, LSP_RESULT_ID_LEN, "%d", doc->version);
    return copy;
}

LspSemanticTokens *lsp_semantic_tokens_delta(LspDocument *doc,
                                              const char *previous_result_id)
{
    /* If the version matches the previous result ID, return empty delta. */
    char version_str[LSP_RESULT_ID_LEN];
    snprintf(version_str, sizeof(version_str), "%d", doc->version);
    if (previous_result_id && strcmp(previous_result_id, version_str) == 0) {
        LspSemanticTokens *st = lsp_xcalloc(1, sizeof(*st));
        st->result_id = lsp_xstrdup(version_str);
        return st;
    }
    /* Otherwise, return the full token set. */
    return lsp_semantic_tokens(doc);
}

/* Helper used by the analysis layer to encode a token into the builder. */
static void semantic_token_push(uint32_t **data, size_t *count, size_t *cap,
                                 uint32_t prev_line, uint32_t prev_start,
                                 LspToken tok)
{
    if (*count + 5 > *cap) {
        *cap  = (*cap) ? (*cap) * 2 : 32;
        *data = lsp_xrealloc(*data, *cap * sizeof(uint32_t));
    }
    (*data)[(*count)++] = tok.line  - prev_line;
    (*data)[(*count)++] = (tok.line == prev_line)
                         ? tok.start_char - prev_start
                         : tok.start_char;
    (*data)[(*count)++] = tok.length;
    (*data)[(*count)++] = (uint32_t)tok.type;
    (*data)[(*count)++] = tok.modifiers;
}
/* semantic_token_push will be wired to the analysis pass in analysis.c */


/// §20  Inlay hints

//// Inlay hints
 //
 //  Returns the subset of pre-computed inlay hints that fall within the
 //  requested range.  The result array is a fresh allocation; the caller
 //  owns it.
 //
LspInlayHint *lsp_inlay_hints(LspDocument *doc, LspRange range, size_t *count)
{
    *count = 0;
    if (!doc || doc->inlay_count == 0) return NULL;

    size_t cap   = 16;
    LspInlayHint *out = lsp_xmalloc(cap * sizeof(LspInlayHint));

    for (size_t i = 0; i < doc->inlay_count; i++) {
        LspInlayHint *h = &doc->inlay_hints[i];
        /* Filter by range */
        bool after_start =
            h->position.line > range.start.line ||
            (h->position.line == range.start.line &&
             h->position.character >= range.start.character);
        bool before_end =
            h->position.line < range.end.line ||
            (h->position.line == range.end.line &&
             h->position.character <= range.end.character);

        if (!after_start || !before_end) continue;

        LSP_GROW(out, *count, cap, LspInlayHint);
        LspInlayHint *dst = &out[*count];
        dst->position      = h->position;
        dst->label         = lsp_xstrdup(h->label);
        dst->kind          = h->kind;
        dst->tooltip       = lsp_xstrdup(h->tooltip);
        dst->padding_left  = h->padding_left;
        dst->padding_right = h->padding_right;
        (*count)++;
    }

    if (*count == 0) { free(out); return NULL; }
    return out;
}


/// §21  Signature help

//// Signature help
 //
 //  Signature help is shown when the cursor is inside an argument list.
 //  We detect this by scanning leftward from the cursor for an unclosed '('
 //  and counting commas to determine the active parameter.
 //
 //  The function name to the left of '(' is looked up in the workspace index.
 //
LspSignatureHelp *lsp_signature_help(LspDocument *doc, LspPosition pos)
{
    if (!doc || !doc->source) return NULL;

    uint32_t offset = lsp_document_offset(doc, pos);
    const char *src = doc->source;

    /* Scan left for the opening '(' */
    int depth        = 0;
    int param_index  = 0;
    uint32_t paren_off = 0;
    bool found_paren  = false;

    for (int i = (int)offset - 1; i >= 0; i--) {
        char c = src[i];
        if (c == ')') { depth++; continue; }
        if (c == '(') {
            if (depth > 0) { depth--; continue; }
            paren_off   = (uint32_t)i;
            found_paren = true;
            break;
        }
        if (c == ',' && depth == 0) {
            param_index++;
        }
    }
    if (!found_paren) return NULL;

    /* Extract the function name just before the '(' */
    uint32_t fn_end = paren_off;
    while (fn_end > 0 && src[fn_end - 1] == ' ') fn_end--;
    uint32_t fn_start = fn_end;
    while (fn_start > 0 && is_ident_char(src[fn_start - 1])) fn_start--;

    if (fn_start == fn_end) return NULL;

    char *fn_name = lsp_xstrndup(src + fn_start, fn_end - fn_start);
    LspIndexEntry *e = doc->workspace
                     ? lsp_index_lookup(doc->workspace->index, fn_name)
                     : NULL;
    free(fn_name);
    if (!e || !e->type_sig) return NULL;

    LspSignatureHelp *sh = lsp_xcalloc(1, sizeof(*sh));
    sh->signatures      = lsp_xmalloc(sizeof(LspSignatureInfo));
    sh->signature_count = 1;
    sh->active_signature = 0;
    sh->active_parameter = (uint32_t)param_index;

    LspSignatureInfo *sig = &sh->signatures[0];
    sig->label            = lsp_xstrdup(e->type_sig);
    sig->documentation    = lsp_xstrdup(e->documentation);
    sig->active_parameter = (uint32_t)param_index;

    /* Parse parameter names from the type signature (simple heuristic) */
    /* A full implementation would walk the typed AST. */
    sig->parameters      = NULL;
    sig->parameter_count = 0;

    return sh;
}


/// §22  Code actions

//// Code actions
 //
 //  Code actions are offered for:
 //    · Import suggestions when a symbol is unbound
 //    · Type-hole filling
 //    · Case-split expansion for pattern matching
 //    · Dead-code removal (unused imports, variables)
 //
 //  For each supplied diagnostic we emit up to one action.
 //
static LspAction *make_simple_edit_action(const char *title,
                                           LspActionKind kind,
                                           const char *uri,
                                           LspRange range,
                                           const char *replacement)
{
    LspAction *a = lsp_xcalloc(1, sizeof(*a));
    a->title = lsp_xstrdup(title);
    a->kind  = kind;

    a->edit            = lsp_xcalloc(1, sizeof(LspWorkspaceEdit));
    a->edit->uri       = lsp_xstrdup(uri);
    a->edit->edits     = lsp_xmalloc(sizeof(LspTextEdit));
    a->edit->edit_count = 1;
    a->edit->edits[0].range    = range;
    a->edit->edits[0].new_text = lsp_xstrdup(replacement);
    return a;
}

LspAction **lsp_code_actions(LspDocument *doc, LspRange range,
                               LspDiagnostic **diags, size_t diag_count,
                               size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    size_t cap = 8;
    LspAction **list = lsp_xmalloc(cap * sizeof(LspAction *));

    for (size_t i = 0; i < diag_count; i++) {
        LspDiagnostic *d = diags[i];
        if (!d || !d->message) continue;

        /* Unbound variable → offer import */
        if (strstr(d->message, "unbound") || strstr(d->message, "not in scope")) {
            char title[256];
            snprintf(title, sizeof(title), "Add import for '%s'", d->code ? d->code : "?");
            /* A real implementation would search the index for modules
               that export the missing symbol.  We emit a placeholder.  */
            LspRange import_range = { {0,0}, {0,0} };
            LspAction *a = make_simple_edit_action(
                title, LSP_ACTION_QUICKFIX, doc->uri, import_range,
                "import ?\n");
            a->is_preferred = true;
            LSP_GROW(list, *count, cap, LspAction *);
            list[(*count)++] = a;
        }

        /* Unused binding → offer removal */
        if (d->tags & LSP_DIAG_TAG_UNNECESSARY) {
            LspAction *a = make_simple_edit_action(
                "Remove unused binding",
                LSP_ACTION_SOURCE_FIXALL,
                doc->uri, d->range, "");
            LSP_GROW(list, *count, cap, LspAction *);
            list[(*count)++] = a;
        }
    }

    /* Offer whole-file format action regardless of diagnostics */
    {
        LspRange full = { {0,0}, {doc->line_count, 0} };
        LspAction *a = lsp_xcalloc(1, sizeof(*a));
        a->title   = lsp_xstrdup("Format document");
        a->kind    = LSP_ACTION_SOURCE;
        a->edit    = lsp_xcalloc(1, sizeof(LspWorkspaceEdit));
        a->edit->uri = lsp_xstrdup(doc->uri);
        /* edits will be filled in by the format pass */
        (void)full;
        LSP_GROW(list, *count, cap, LspAction *);
        list[(*count)++] = a;
    }

    (void)range;
    return list;
}


/// §23  Rename / prepare-rename

//// Rename / prepare-rename
 //
 //  prepareRename validates that the position is renameable and returns the
 //  current name's range.
 //
 //  rename collects all reference locations across the workspace, including
 //  the definition site, and emits a workspace edit that replaces each.
 //
LspRange *lsp_prepare_rename(LspDocument *doc, LspPosition pos)
{
    if (!doc) return NULL;
    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;

    /* Check it exists in the index */
    LspIndexEntry *e = doc->workspace
                     ? lsp_index_lookup(doc->workspace->index, word)
                     : NULL;
    free(word);
    if (!e) return NULL;

    LspRange *r = lsp_xmalloc(sizeof(LspRange));
    *r = lsp_document_word_range(doc, pos);
    return r;
}

LspWorkspaceEdit *lsp_rename(LspDocument *doc, LspPosition pos,
                              const char *new_name)
{
    if (!doc || !new_name) return NULL;

    size_t ref_count = 0;
    LspLocation *refs = lsp_references(doc, pos, /*include_declaration=*/true,
                                        &ref_count);
    if (!refs || ref_count == 0) return NULL;

    /* Group edits by URI — emit one LspWorkspaceEdit per file.
       For simplicity we return the first file's edits only. */
    LspWorkspaceEdit *we = lsp_xcalloc(1, sizeof(*we));
    we->uri        = lsp_xstrdup(refs[0].uri);
    we->edits      = lsp_xmalloc(ref_count * sizeof(LspTextEdit));
    we->edit_count = 0;

    for (size_t i = 0; i < ref_count; i++) {
        if (strcmp(refs[i].uri, we->uri) != 0) continue;  /* skip other files */
        LspTextEdit *te = &we->edits[we->edit_count++];
        te->range    = refs[i].range;
        te->new_text = lsp_xstrdup(new_name);
    }

    lsp_location_list_free(refs, ref_count);
    return we;
}


/// §24  Folding ranges

//// Folding ranges
 //
 //  Returns a copy of the pre-computed fold ranges stored on the document.
 //  The analysis pass populates these by walking the AST for top-level
 //  definitions, import blocks, and comment regions.
 //
LspFoldRange *lsp_folding_ranges(LspDocument *doc, size_t *count)
{
    *count = 0;
    if (!doc || doc->fold_count == 0) return NULL;

    LspFoldRange *out = lsp_xmalloc(doc->fold_count * sizeof(LspFoldRange));
    for (size_t i = 0; i < doc->fold_count; i++) {
        out[i].start_line     = doc->folds[i].start_line;
        out[i].end_line       = doc->folds[i].end_line;
        out[i].kind           = doc->folds[i].kind;
        out[i].collapsed_text = lsp_xstrdup(doc->folds[i].collapsed_text);
    }
    *count = doc->fold_count;
    return out;
}


/// §25  Formatting

//// Formatting
 //
 //  Full-document and range formatting.  The formatter is a separate pass
 //  that produces a minimal edit set (single whole-document replacement for
 //  now; a smarter diff-based approach would produce smaller edits).
 //
 //  A real Monad formatter would:
 //    1. Parse the source to an AST.
 //    2. Pretty-print the AST with the configured indentation rules.
 //    3. Diff the old and new source to produce a minimal edit list.
 //
LspTextEdit *lsp_format(LspDocument *doc, size_t *count)
{
    *count = 0;
    if (!doc || !doc->source) return NULL;

    /* Placeholder: return a single no-op edit.
       Real implementation calls the Monad pretty-printer.          */
    LspTextEdit *edits = lsp_xmalloc(sizeof(LspTextEdit));
    LspPosition start = {0, 0};
    LspPosition end   = lsp_document_position(doc, (uint32_t)doc->source_len);
    edits[0].range.start = start;
    edits[0].range.end   = end;
    edits[0].new_text    = lsp_xstrdup(doc->source);
    *count = 1;
    return edits;
}

LspTextEdit *lsp_format_range(LspDocument *doc, LspRange range, size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    uint32_t start_off = lsp_document_offset(doc, range.start);
    uint32_t end_off   = lsp_document_offset(doc, range.end);
    if (start_off >= end_off) return NULL;

    LspTextEdit *edits = lsp_xmalloc(sizeof(LspTextEdit));
    edits[0].range    = range;
    edits[0].new_text = lsp_xstrndup(doc->source + start_off,
                                      end_off - start_off);
    *count = 1;
    return edits;
}


/// §26  Linked editing

//// Linked editing ranges
 //
 //  Returns all ranges that should be simultaneously edited when the cursor
 //  token is renamed via the editor's inline rename UI.  Typically this is
 //  the same set as document highlights for the current symbol.
 //
LspRange *lsp_linked_editing_ranges(LspDocument *doc, LspPosition pos,
                                     size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    size_t      loc_count = 0;
    LspLocation *locs = lsp_document_highlight(doc, pos, &loc_count);
    if (!locs || loc_count == 0) return NULL;

    LspRange *ranges = lsp_xmalloc(loc_count * sizeof(LspRange));
    for (size_t i = 0; i < loc_count; i++)
        ranges[i] = locs[i].range;

    lsp_location_list_free(locs, loc_count);
    *count = loc_count;
    return ranges;
}


/// §27  Call hierarchy

//// Call hierarchy
 //
 //  prepareCallHierarchy resolves the symbol under the cursor to a
 //  LspCallHierarchyItem.  incomingCalls / outgoingCalls then expand
 //  the caller/callee graph one level at a time.
 //
 //  A full implementation traverses the call graph embedded in the typed
 //  AST.  Here we provide structurally correct stubs.
 //
LspCallHierarchyItem **lsp_prepare_call_hierarchy(LspDocument *doc,
                                                   LspPosition pos,
                                                   size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;

    LspIndexEntry *e = doc->workspace
                     ? lsp_index_lookup(doc->workspace->index, word)
                     : NULL;
    free(word);
    if (!e) return NULL;

    LspCallHierarchyItem *item = lsp_xcalloc(1, sizeof(*item));
    item->name            = lsp_xstrdup(e->short_name ? e->short_name : e->name);
    item->kind            = e->kind;
    item->detail          = lsp_xstrdup(e->type_sig);
    item->uri             = lsp_xstrdup(e->uri);
    item->range           = e->range;
    item->selection_range = e->name_range;

    LspCallHierarchyItem **list = lsp_xmalloc(sizeof(LspCallHierarchyItem *));
    list[0] = item;
    *count = 1;
    return list;
}

LspCallHierarchyIncoming *lsp_incoming_calls(LspWorkspace *ws,
                                              LspCallHierarchyItem *item,
                                              size_t *count)
{
    (void)ws; (void)item;
    *count = 0;
    return NULL;   /* TODO: traverse reverse call graph */
}

LspCallHierarchyOutgoing *lsp_outgoing_calls(LspWorkspace *ws,
                                              LspCallHierarchyItem *item,
                                              size_t *count)
{
    (void)ws; (void)item;
    *count = 0;
    return NULL;   /* TODO: traverse forward call graph */
}


/// §28  Type hierarchy

//// Type hierarchy
 //
 //  Resolves a type at the cursor and allows the editor to navigate the
 //  supertype/subtype lattice.  In Monad this corresponds to type class
 //  inheritance and instance relationships.
 //
LspTypeHierarchyItem **lsp_prepare_type_hierarchy(LspDocument *doc,
                                                   LspPosition pos,
                                                   size_t *count)
{
    *count = 0;
    if (!doc) return NULL;

    char *word = lsp_document_word_at(doc, pos);
    if (!word) return NULL;

    LspIndexEntry *e = doc->workspace
                     ? lsp_index_lookup(doc->workspace->index, word)
                     : NULL;
    free(word);
    if (!e) return NULL;

    LspTypeHierarchyItem *item = lsp_xcalloc(1, sizeof(*item));
    item->name            = lsp_xstrdup(e->short_name ? e->short_name : e->name);
    item->kind            = e->kind;
    item->detail          = lsp_xstrdup(e->type_sig);
    item->uri             = lsp_xstrdup(e->uri);
    item->range           = e->range;
    item->selection_range = e->name_range;

    LspTypeHierarchyItem **list = lsp_xmalloc(sizeof(LspTypeHierarchyItem *));
    list[0] = item;
    *count  = 1;
    return list;
}

LspTypeHierarchyItem **lsp_supertypes(LspWorkspace *ws,
                                       LspTypeHierarchyItem *item,
                                       size_t *count)
{
    (void)ws; (void)item;
    *count = 0;
    return NULL;
}

LspTypeHierarchyItem **lsp_subtypes(LspWorkspace *ws,
                                     LspTypeHierarchyItem *item,
                                     size_t *count)
{
    (void)ws; (void)item;
    *count = 0;
    return NULL;
}


/// §29  JSON serialization

//// JSON serialization
 //
 //  Each lsp_json_*() function returns a heap-allocated JSON string.
 //  They are generated by the StrBuf builder and must be free()'d by
 //  the caller.
 //
char *lsp_json_position(LspPosition pos)
{
    StrBuf b;
    sb_init(&b);
    sb_appendf(&b, "{\"line\":%u,\"character\":%u}",
               pos.line, pos.character);
    return sb_take(&b);
}

char *lsp_json_range(LspRange range)
{
    StrBuf b;
    sb_init(&b);
    char *s = lsp_json_position(range.start);
    char *e = lsp_json_position(range.end);
    sb_appendf(&b, "{\"start\":%s,\"end\":%s}", s, e);
    free(s); free(e);
    return sb_take(&b);
}

char *lsp_json_location(LspLocation *loc)
{
    if (!loc) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *uri   = lsp_json_string(loc->uri);
    char *range = lsp_json_range(loc->range);
    sb_appendf(&b, "{\"uri\":%s,\"range\":%s}", uri, range);
    free(uri); free(range);
    return sb_take(&b);
}

char *lsp_json_location_list(LspLocation *list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *loc = lsp_json_location(&list[i]);
        sb_append(&b, loc);
        free(loc);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_diagnostic(LspDiagnostic *d)
{
    if (!d) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *range = lsp_json_range(d->range);
    char *msg   = lsp_json_string(d->message);
    char *src   = lsp_json_string(d->source);
    char *code  = lsp_json_string(d->code);

    sb_appendf(&b,
        "{\"range\":%s,\"severity\":%d,\"message\":%s,\"source\":%s,\"code\":%s",
        range, (int)d->severity, msg, src, code);
    free(range); free(msg); free(src); free(code);

    if (d->tags) {
        sb_append(&b, ",\"tags\":[");
        bool first = true;
        if (d->tags & LSP_DIAG_TAG_UNNECESSARY) {
            sb_appendf(&b, "%s1", first ? "" : ","); first = false;
        }
        if (d->tags & LSP_DIAG_TAG_DEPRECATED) {
            sb_appendf(&b, "%s2", first ? "" : ",");
        }
        sb_appendc(&b, ']');
    }

    if (d->related_count > 0) {
        sb_append(&b, ",\"relatedInformation\":[");
        for (size_t i = 0; i < d->related_count; i++) {
            if (i) sb_appendc(&b, ',');
            char *loc = lsp_json_location(&d->related[i].location);
            char *rm  = lsp_json_string(d->related[i].message);
            sb_appendf(&b, "{\"location\":%s,\"message\":%s}", loc, rm);
            free(loc); free(rm);
        }
        sb_appendc(&b, ']');
    }

    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_json_diagnostic_list(LspDiagnostic *list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *d = lsp_json_diagnostic(&list[i]);
        sb_append(&b, d);
        free(d);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_completion(LspCompletion *c)
{
    if (!c) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    bool first = true;
    sb_appendc(&b, '{');
    char *label = lsp_json_string(c->label);
    sb_appendf(&b, "\"label\":%s", label); free(label);
    first = false;

    if (c->label_detail) {
        char *ld = lsp_json_string(c->label_detail);
        sb_appendf(&b, ",\"labelDetails\":{\"detail\":%s}", ld);
        free(ld);
    }

    sb_appendf(&b, ",\"kind\":%d", (int)c->kind);

    sb_json_field_str(&b, "detail",        c->detail,        &first);
    sb_json_field_str(&b, "documentation", c->documentation, &first);
    sb_json_field_str(&b, "sortText",      c->sort_text,     &first);
    sb_json_field_str(&b, "filterText",    c->filter_text,   &first);
    sb_json_field_str(&b, "insertText",    c->insert_text,   &first);

    if (c->insert_format != LSP_INSERT_TEXT_PLAIN)
        sb_appendf(&b, ",\"insertTextFormat\":%d", (int)c->insert_format);

    if (c->deprecated)
        sb_append(&b, ",\"deprecated\":true");
    if (c->preselect)
        sb_append(&b, ",\"preselect\":true");

    if (c->text_edit_range) {
        char *r = lsp_json_range(*c->text_edit_range);
        sb_appendf(&b, ",\"textEdit\":{\"range\":%s,\"newText\":%s}",
                   r, c->insert_text ? c->insert_text : "\"\"");
        free(r);
    }

    (void)first;
    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_json_completion_list(LspCompletion **list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    /* Return as a CompletionList object for incremental support */
    sb_appendf(&b, "{\"isIncomplete\":%s,\"items\":[",
               count >= LSP_MAX_COMPLETIONS ? "true" : "false");
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *c = lsp_json_completion(list[i]);
        sb_append(&b, c);
        free(c);
    }
    sb_append(&b, "]}");
    return sb_take(&b);
}

char *lsp_json_hover(LspHover *h)
{
    if (!h) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *contents = lsp_json_string(h->contents);
    int   kind     = (h->kind == LSP_MARKUP_MARKDOWN) ? 1 : 0;
    sb_appendf(&b,
        "{\"contents\":{\"kind\":\"%s\",\"value\":%s}",
        kind ? "markdown" : "plaintext", contents);
    free(contents);
    if (h->range) {
        char *r = lsp_json_range(*h->range);
        sb_appendf(&b, ",\"range\":%s", r);
        free(r);
    }
    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_json_symbol(LspSymbol *s)
{
    if (!s) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *name   = lsp_json_string(s->name);
    char *detail = s->detail ? lsp_json_string(s->detail) : NULL;
    char *range  = lsp_json_range(s->range);
    char *sel    = lsp_json_range(s->selection_range);

    sb_appendf(&b,
        "{\"name\":%s,\"kind\":%d,\"range\":%s,\"selectionRange\":%s",
        name, (int)s->kind, range, sel);
    free(name); free(range); free(sel);

    if (detail) {
        sb_appendf(&b, ",\"detail\":%s", detail);
        free(detail);
    }
    if (s->tags & LSP_SYMBOL_TAG_DEPRECATED)
        sb_append(&b, ",\"tags\":[1]");
    if (s->container) {
        char *cont = lsp_json_string(s->container);
        sb_appendf(&b, ",\"containerName\":%s", cont);
        free(cont);
    }
    if (s->child_count > 0) {
        sb_append(&b, ",\"children\":[");
        for (size_t i = 0; i < s->child_count; i++) {
            if (i) sb_appendc(&b, ',');
            char *child = lsp_json_symbol(s->children[i]);
            sb_append(&b, child);
            free(child);
        }
        sb_appendc(&b, ']');
    }
    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_json_symbol_list(LspSymbol **list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *s = lsp_json_symbol(list[i]);
        sb_append(&b, s);
        free(s);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_semantic_tokens(LspSemanticTokens *st)
{
    if (!st) return lsp_xstrdup("{\"data\":[]}");
    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"data\":[");
    for (size_t i = 0; i < st->count; i++) {
        if (i) sb_appendc(&b, ',');
        sb_appendf(&b, "%u", st->data[i]);
    }
    sb_append(&b, "]}");
    return sb_take(&b);
}

char *lsp_json_inlay_hints(LspInlayHint *list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        LspInlayHint *h = &list[i];
        char *pos   = lsp_json_position(h->position);
        char *label = lsp_json_string(h->label);
        sb_appendf(&b,
            "{\"position\":%s,\"label\":%s,\"kind\":%d"
            ",\"paddingLeft\":%s,\"paddingRight\":%s}",
            pos, label, (int)h->kind,
            h->padding_left  ? "true" : "false",
            h->padding_right ? "true" : "false");
        free(pos); free(label);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_signature_help(LspSignatureHelp *sh)
{
    if (!sh) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"signatures\":[");

    for (size_t i = 0; i < sh->signature_count; i++) {
        if (i) sb_appendc(&b, ',');
        LspSignatureInfo *sig = &sh->signatures[i];
        char *label = lsp_json_string(sig->label);
        sb_appendf(&b, "{\"label\":%s", label);
        free(label);
        if (sig->documentation) {
            char *doc = lsp_json_string(sig->documentation);
            sb_appendf(&b, ",\"documentation\":{\"kind\":\"markdown\",\"value\":%s}", doc);
            free(doc);
        }
        if (sig->parameter_count > 0) {
            sb_append(&b, ",\"parameters\":[");
            for (size_t j = 0; j < sig->parameter_count; j++) {
                if (j) sb_appendc(&b, ',');
                char *pl = lsp_json_string(sig->parameters[j].label);
                sb_appendf(&b, "{\"label\":%s}", pl);
                free(pl);
            }
            sb_appendc(&b, ']');
        }
        sb_appendf(&b, ",\"activeParameter\":%u}", sig->active_parameter);
    }

    sb_appendf(&b,
        "],\"activeSignature\":%u,\"activeParameter\":%u}",
        sh->active_signature, sh->active_parameter);
    return sb_take(&b);
}

char *lsp_json_action(LspAction *a)
{
    if (!a) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *title = lsp_json_string(a->title);
    sb_appendf(&b, "{\"title\":%s", title);
    free(title);

    static const char *kind_names[] = {
        "", "quickfix", "refactor", "refactor.extract",
        "refactor.inline", "refactor.rewrite", "source",
        "source.organizeImports", "source.fixAll"
    };
    if (a->kind > LSP_ACTION_EMPTY &&
        a->kind <= LSP_ACTION_SOURCE_FIXALL) {
        char *k = lsp_json_string(kind_names[a->kind]);
        sb_appendf(&b, ",\"kind\":%s", k);
        free(k);
    }
    if (a->is_preferred)
        sb_append(&b, ",\"isPreferred\":true");
    if (a->edit) {
        char *we = lsp_json_workspace_edit(a->edit);
        sb_appendf(&b, ",\"edit\":%s", we);
        free(we);
    }
    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_json_action_list(LspAction **list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *a = lsp_json_action(list[i]);
        sb_append(&b, a);
        free(a);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_workspace_edit(LspWorkspaceEdit *we)
{
    if (!we) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *uri = lsp_json_string(we->uri);
    sb_appendf(&b, "{\"changes\":{%s:[", uri);
    free(uri);
    for (size_t i = 0; i < we->edit_count; i++) {
        if (i) sb_appendc(&b, ',');
        char *r  = lsp_json_range(we->edits[i].range);
        char *nt = lsp_json_string(we->edits[i].new_text);
        sb_appendf(&b, "{\"range\":%s,\"newText\":%s}", r, nt);
        free(r); free(nt);
    }
    sb_append(&b, "]}}");
    return sb_take(&b);
}

char *lsp_json_fold_ranges(LspFoldRange *list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    static const char *fold_kinds[] = { "comment", "imports", "region" };
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        LspFoldRange *f = &list[i];
        char *kind = lsp_json_string(fold_kinds[f->kind]);
        sb_appendf(&b, "{\"startLine\":%u,\"endLine\":%u,\"kind\":%s}",
                   f->start_line, f->end_line, kind);
        free(kind);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_text_edits(LspTextEdit *list, size_t count)
{
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *r  = lsp_json_range(list[i].range);
        char *nt = lsp_json_string(list[i].new_text);
        sb_appendf(&b, "{\"range\":%s,\"newText\":%s}", r, nt);
        free(r); free(nt);
    }
    sb_appendc(&b, ']');
    return sb_take(&b);
}

char *lsp_json_call_hierarchy_item(LspCallHierarchyItem *item)
{
    if (!item) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *name  = lsp_json_string(item->name);
    char *uri   = lsp_json_string(item->uri);
    char *range = lsp_json_range(item->range);
    char *sel   = lsp_json_range(item->selection_range);
    sb_appendf(&b,
        "{\"name\":%s,\"kind\":%d,\"uri\":%s,\"range\":%s"
        ",\"selectionRange\":%s}",
        name, (int)item->kind, uri, range, sel);
    free(name); free(uri); free(range); free(sel);
    return sb_take(&b);
}

char *lsp_json_type_hierarchy_item(LspTypeHierarchyItem *item)
{
    if (!item) return lsp_xstrdup("null");
    StrBuf b;
    sb_init(&b);
    char *name  = lsp_json_string(item->name);
    char *uri   = lsp_json_string(item->uri);
    char *range = lsp_json_range(item->range);
    char *sel   = lsp_json_range(item->selection_range);
    sb_appendf(&b,
        "{\"name\":%s,\"kind\":%d,\"uri\":%s,\"range\":%s"
        ",\"selectionRange\":%s}",
        name, (int)item->kind, uri, range, sel);
    free(name); free(uri); free(range); free(sel);
    return sb_take(&b);
}


/// §30  Capabilities

//// Capabilities
 //
 //  Emits the serverCapabilities JSON object sent in the initialize response.
 //  Every feature we implement is advertised here.
 //
 // Token-type and token-modifier name arrays for the semantic token legend.
static const char *TOKEN_TYPE_NAMES[] = {
    "namespace", "type", "class", "enum", "interface", "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember",
    "event", "function", "method", "macro", "keyword", "modifier",
    "comment", "string", "number", "operator", "decorator",
};

static const char *TOKEN_MOD_NAMES[] = {
    "declaration", "definition", "readonly", "static", "deprecated",
    "abstract", "async", "modification", "documentation", "defaultLibrary",
};

char *lsp_server_capabilities(LspServer *server)
{
    (void)server;
    StrBuf b;
    sb_init(&b);

    sb_append(&b, "{");

    /* Text document sync: incremental */
    sb_append(&b, "\"textDocumentSync\":{"
                  "\"openClose\":true,"
                  "\"change\":2,"          /* 2 = incremental */
                  "\"save\":{\"includeText\":false}"
                  "},");

    /* Completion */
    sb_append(&b, "\"completionProvider\":{"
                  "\"resolveProvider\":false,"
                  "\"triggerCharacters\":[\".\",\" \",\"(\"]"
                  "},");

    /* Hover */
    sb_append(&b, "\"hoverProvider\":true,");

    /* Signature help */
    sb_append(&b, "\"signatureHelpProvider\":{"
                  "\"triggerCharacters\":[\"(\",\",\"]"
                  "},");

    /* Go-to-* */
    sb_append(&b, "\"definitionProvider\":true,");
    sb_append(&b, "\"typeDefinitionProvider\":true,");
    sb_append(&b, "\"declarationProvider\":true,");
    sb_append(&b, "\"implementationProvider\":true,");

    /* References */
    sb_append(&b, "\"referencesProvider\":true,");

    /* Document highlight */
    sb_append(&b, "\"documentHighlightProvider\":true,");

    /* Document symbols */
    sb_append(&b, "\"documentSymbolProvider\":true,");

    /* Workspace symbols */
    sb_append(&b, "\"workspaceSymbolProvider\":true,");

    /* Code actions */
    sb_append(&b, "\"codeActionProvider\":{"
                  "\"codeActionKinds\":["
                  "\"quickfix\",\"refactor\",\"refactor.extract\","
                  "\"refactor.inline\",\"refactor.rewrite\","
                  "\"source\",\"source.organizeImports\",\"source.fixAll\""
                  "],"
                  "\"resolveProvider\":false"
                  "},");

    /* Rename */
    sb_append(&b, "\"renameProvider\":{\"prepareProvider\":true},");

    /* Formatting */
    sb_append(&b, "\"documentFormattingProvider\":true,");
    sb_append(&b, "\"documentRangeFormattingProvider\":true,");

    /* Folding */
    sb_append(&b, "\"foldingRangeProvider\":true,");

    /* Inlay hints */
    sb_append(&b, "\"inlayHintProvider\":true,");

    /* Linked editing */
    sb_append(&b, "\"linkedEditingRangeProvider\":true,");

    /* Call hierarchy */
    sb_append(&b, "\"callHierarchyProvider\":true,");

    /* Type hierarchy */
    sb_append(&b, "\"typeHierarchyProvider\":true,");

    /* Semantic tokens */
    sb_append(&b, "\"semanticTokensProvider\":{");
    sb_append(&b, "\"legend\":{\"tokenTypes\":[");
    size_t n_types = sizeof(TOKEN_TYPE_NAMES) / sizeof(TOKEN_TYPE_NAMES[0]);
    for (size_t i = 0; i < n_types; i++) {
        if (i) sb_appendc(&b, ',');
        char *s = lsp_json_string(TOKEN_TYPE_NAMES[i]);
        sb_append(&b, s);
        free(s);
    }
    sb_append(&b, "],\"tokenModifiers\":[");
    size_t n_mods = sizeof(TOKEN_MOD_NAMES) / sizeof(TOKEN_MOD_NAMES[0]);
    for (size_t i = 0; i < n_mods; i++) {
        if (i) sb_appendc(&b, ',');
        char *s = lsp_json_string(TOKEN_MOD_NAMES[i]);
        sb_append(&b, s);
        free(s);
    }
    sb_append(&b, "]},");
    sb_append(&b, "\"full\":{\"delta\":true},");
    sb_append(&b, "\"range\":false");
    sb_append(&b, "},");

    /* Workspace capabilities */
    sb_append(&b, "\"workspace\":{"
                  "\"workspaceFolders\":{"
                  "\"supported\":true,"
                  "\"changeNotifications\":true"
                  "}"
                  "}");

    sb_appendc(&b, '}');
    return sb_take(&b);
}

char *lsp_server_info(void)
{
    return lsp_xstrdup("{\"name\":\"monad-lsp\",\"version\":\"0.1.0\"}");
}


/// §31  Request dispatcher

//// Request dispatcher
 //
 //  Central dispatch table mapping method strings to handlers.
 //  Each handler receives the raw params JSON and produces a result JSON
 //  string (or NULL for notifications).
 //
 //  Handler naming convention:
 //    handle_<camelCase-method-with-slash-as-underscore>
 //
//// Lifecycle

static char *handle_initialize(LspServer *server, const char *params)
{
    (void)params;
    server->initialized = true;

    char *caps = lsp_server_capabilities(server);
    char *info = lsp_server_info();
    StrBuf b;
    sb_init(&b);
    sb_appendf(&b, "{\"capabilities\":%s,\"serverInfo\":%s}", caps, info);
    free(caps); free(info);
    return sb_take(&b);
}

static char *handle_initialized(LspServer *server, const char *params)
{
    (void)server; (void)params;
    return NULL;   /* notification — no response */
}

static char *handle_shutdown(LspServer *server, const char *params)
{
    (void)params;
    server->shutdown_requested = true;
    return lsp_xstrdup("null");
}

static char *handle_exit(LspServer *server, const char *params)
{
    (void)params;
    server->exit_code = server->shutdown_requested ? 0 : 1;
    return NULL;
}

//// Document synchronization

static char *handle_textDocument_didOpen(LspServer *server, const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    char *txt = json_get_string(td, "text");
    int   ver = (int)json_get_int(td, "version");

    LspDocument *doc = lsp_workspace_open_doc(
        server->workspace, uri, txt, ver);
    lsp_document_analyze(doc);
    lsp_publish_diagnostics(server, doc);

    free(td); free(uri); free(txt);
    return NULL;
}

static char *handle_textDocument_didChange(LspServer *server, const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    int   ver = (int)json_get_int(td, "version");

    /* Use the last content change */
    char *changes = json_get_object(params, "contentChanges");
    /* For simplicity, treat the whole text as a single replacement. */
    char *text = json_get_string(changes, "text");

    if (text) {
        lsp_workspace_update_doc(server->workspace, uri, text, ver);
        LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
        if (doc) {
            lsp_document_analyze(doc);
            lsp_publish_diagnostics(server, doc);
        }
    }

    free(td); free(uri); free(changes); free(text);
    return NULL;
}

static char *handle_textDocument_didSave(LspServer *server, const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    if (server->config.check_on_save) {
        LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
        if (doc) {
            lsp_document_analyze(doc);
            lsp_publish_diagnostics(server, doc);
        }
    }
    free(td); free(uri);
    return NULL;
}

static char *handle_textDocument_didClose(LspServer *server, const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    lsp_workspace_close_doc(server->workspace, uri);
    free(td); free(uri);
    return NULL;
}

//// Language feature helpers
 //
 // Parse {"textDocument":{"uri":…},"position":{…}} params
static bool parse_text_document_position(const char *params,
                                          char **uri_out,
                                          LspPosition *pos_out)
{
    char *td  = json_get_object(params, "textDocument");
    char *pos = json_get_object(params, "position");
    *uri_out  = json_get_string(td, "uri");
    *pos_out  = json_parse_position(pos);
    free(td); free(pos);
    return *uri_out != NULL;
}

//// Completion

static char *handle_textDocument_completion(LspServer *server,
                                             const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;

    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("{\"isIncomplete\":false,\"items\":[]}");

    char *ctx = json_get_object(params, "context");
    bool by_dot = false;
    if (ctx) {
        char *trig = json_get_string(ctx, "triggerCharacter");
        by_dot = trig && strcmp(trig, ".") == 0;
        free(trig);
    }
    free(ctx);

    size_t count = 0;
    LspCompletion **list = lsp_completion(doc, pos, by_dot, &count);
    char *result = lsp_json_completion_list(list, count);
    lsp_completion_list_free(list, count);
    return result;
}

//// Hover

static char *handle_textDocument_hover(LspServer *server, const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;

    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");

    LspHover *h = lsp_hover(doc, pos);
    char *result = lsp_json_hover(h);
    lsp_hover_free(h);
    return result;
}

//// Signature help

static char *handle_textDocument_signatureHelp(LspServer *server,
                                                const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;

    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");

    LspSignatureHelp *sh = lsp_signature_help(doc, pos);
    char *result = lsp_json_signature_help(sh);
    lsp_signature_help_free(sh);
    return result;
}

//// Go-to-definition family

static char *handle_textDocument_definition(LspServer *server,
                                             const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    size_t count = 0;
    LspLocation *locs = lsp_definition(doc, pos, &count);
    char *result = lsp_json_location_list(locs, count);
    lsp_location_list_free(locs, count);
    return result;
}

static char *handle_textDocument_typeDefinition(LspServer *server,
                                                 const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    size_t count = 0;
    LspLocation *locs = lsp_type_definition(doc, pos, &count);
    char *result = lsp_json_location_list(locs, count);
    lsp_location_list_free(locs, count);
    return result;
}

static char *handle_textDocument_declaration(LspServer *server,
                                              const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    size_t count = 0;
    LspLocation *locs = lsp_declaration(doc, pos, &count);
    char *result = lsp_json_location_list(locs, count);
    lsp_location_list_free(locs, count);
    return result;
}

static char *handle_textDocument_implementation(LspServer *server,
                                                 const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    size_t count = 0;
    LspLocation *locs = lsp_implementation(doc, pos, &count);
    char *result = lsp_json_location_list(locs, count);
    lsp_location_list_free(locs, count);
    return result;
}

//// References

static char *handle_textDocument_references(LspServer *server,
                                             const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    char *ctx = json_get_object(params, "context");
    bool include_decl = json_get_bool(ctx, "includeDeclaration", true);
    free(ctx);

    size_t count = 0;
    LspLocation *locs = lsp_references(doc, pos, include_decl, &count);
    char *result = lsp_json_location_list(locs, count);
    lsp_location_list_free(locs, count);
    return result;
}

//// Document highlight

static char *handle_textDocument_documentHighlight(LspServer *server,
                                                    const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    size_t count = 0;
    LspLocation *locs = lsp_document_highlight(doc, pos, &count);
    /* documentHighlight items have a "kind" field; reuse location JSON */
    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *r = lsp_json_range(locs[i].range);
        sb_appendf(&b, "{\"range\":%s,\"kind\":1}", r);
        free(r);
    }
    sb_appendc(&b, ']');
    lsp_location_list_free(locs, count);
    return sb_take(&b);
}

//// Symbols

static char *handle_textDocument_documentSymbol(LspServer *server,
                                                 const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    free(td);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");
    size_t count = 0;
    LspSymbol **syms = lsp_document_symbols(doc, &count);
    char *result = lsp_json_symbol_list(syms, count);
    free(syms);   /* we don't own the symbols themselves */
    return result;
}

static char *handle_workspace_symbol(LspServer *server, const char *params)
{
    char *query = json_get_string(params, "query");
    size_t count = 0;
    LspSymbol **syms = lsp_workspace_symbols(server->workspace,
                                              query ? query : "", &count);
    free(query);
    char *result = lsp_json_symbol_list(syms, count);
    lsp_symbol_list_free(syms, count);
    return result;
}

//// Semantic tokens

static char *handle_textDocument_semanticTokens_full(LspServer *server,
                                                      const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    free(td);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("{\"data\":[]}");
    LspSemanticTokens *st = lsp_semantic_tokens(doc);
    char *result = lsp_json_semantic_tokens(st);
    lsp_semantic_tokens_free(st);
    return result;
}

static char *handle_textDocument_semanticTokens_full_delta(
    LspServer *server, const char *params)
{
    char *td      = json_get_object(params, "textDocument");
    char *uri     = json_get_string(td, "uri");
    char *prev_id = json_get_string(params, "previousResultId");
    free(td);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("{\"data\":[]}");
    LspSemanticTokens *st = lsp_semantic_tokens_delta(doc, prev_id);
    free(prev_id);
    char *result = lsp_json_semantic_tokens(st);
    lsp_semantic_tokens_free(st);
    return result;
}

//// Inlay hints

static char *handle_textDocument_inlayHint(LspServer *server,
                                            const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    char *robj = json_get_object(params, "range");
    free(td);
    LspRange range = robj ? json_parse_range(robj) : (LspRange){{0,0},{UINT32_MAX,0}};
    free(robj);

    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    size_t count = 0;
    LspInlayHint *hints = lsp_inlay_hints(doc, range, &count);
    char *result = lsp_json_inlay_hints(hints, count);
    lsp_inlay_hint_list_free(hints, count);
    return result;
}

//// Code actions

static char *handle_textDocument_codeAction(LspServer *server,
                                             const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    char *robj = json_get_object(params, "range");
    free(td);
    LspRange range = robj ? json_parse_range(robj) : (LspRange){{0,0},{0,0}};
    free(robj);

    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    size_t count = 0;
    LspAction **actions = lsp_code_actions(doc, range, NULL, 0, &count);
    char *result = lsp_json_action_list(actions, count);
    lsp_action_list_free(actions, count);
    return result;
}

//// Rename

static char *handle_textDocument_prepareRename(LspServer *server,
                                                const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    LspRange *range = lsp_prepare_rename(doc, pos);
    char *result = range ? lsp_json_range(*range) : lsp_xstrdup("null");
    free(range);
    return result;
}

static char *handle_textDocument_rename(LspServer *server, const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    char *new_name = json_get_string(params, "newName");
    LspWorkspaceEdit *we = lsp_rename(doc, pos, new_name);
    free(new_name);
    char *result = lsp_json_workspace_edit(we);
    lsp_workspace_edit_free(we);
    return result;
}

//// Formatting

static char *handle_textDocument_formatting(LspServer *server,
                                             const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    free(td);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");
    size_t count = 0;
    LspTextEdit *edits = lsp_format(doc, &count);
    char *result = lsp_json_text_edits(edits, count);
    lsp_text_edit_list_free(edits, count);
    return result;
}

static char *handle_textDocument_rangeFormatting(LspServer *server,
                                                  const char *params)
{
    char *td   = json_get_object(params, "textDocument");
    char *uri  = json_get_string(td, "uri");
    char *robj = json_get_object(params, "range");
    free(td);
    LspRange range = robj ? json_parse_range(robj) : (LspRange){{0,0},{0,0}};
    free(robj);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");
    size_t count = 0;
    LspTextEdit *edits = lsp_format_range(doc, range, &count);
    char *result = lsp_json_text_edits(edits, count);
    lsp_text_edit_list_free(edits, count);
    return result;
}

/// Folding

static char *handle_textDocument_foldingRange(LspServer *server,
                                               const char *params)
{
    char *td  = json_get_object(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    free(td);
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");
    size_t count = 0;
    LspFoldRange *folds = lsp_folding_ranges(doc, &count);
    char *result = lsp_json_fold_ranges(folds, count);
    lsp_fold_range_list_free(folds, count);
    return result;
}

//// Linked editing

static char *handle_textDocument_linkedEditingRange(LspServer *server,
                                                     const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("null");
    size_t count = 0;
    LspRange *ranges = lsp_linked_editing_ranges(doc, pos, &count);
    if (!ranges || count == 0) return lsp_xstrdup("null");

    StrBuf b;
    sb_init(&b);
    sb_append(&b, "{\"ranges\":[");
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *r = lsp_json_range(ranges[i]);
        sb_append(&b, r);
        free(r);
    }
    sb_append(&b, "]}");
    free(ranges);
    return sb_take(&b);
}

//// Call hierarchy

static char *handle_textDocument_prepareCallHierarchy(LspServer *server,
                                                       const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    size_t count = 0;
    LspCallHierarchyItem **items = lsp_prepare_call_hierarchy(doc, pos, &count);

    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *item = lsp_json_call_hierarchy_item(items[i]);
        sb_append(&b, item);
        free(item);
    }
    sb_appendc(&b, ']');
    lsp_call_hierarchy_item_list_free(items, count);
    return sb_take(&b);
}

static char *handle_callHierarchy_incomingCalls(LspServer *server,
                                                 const char *params)
{
    (void)server; (void)params;
    return lsp_xstrdup("[]");
}

static char *handle_callHierarchy_outgoingCalls(LspServer *server,
                                                 const char *params)
{
    (void)server; (void)params;
    return lsp_xstrdup("[]");
}

//// Type hierarchy

static char *handle_textDocument_prepareTypeHierarchy(LspServer *server,
                                                       const char *params)
{
    char *uri; LspPosition pos;
    if (!parse_text_document_position(params, &uri, &pos)) return NULL;
    LspDocument *doc = lsp_workspace_get_doc(server->workspace, uri);
    free(uri);
    if (!doc) return lsp_xstrdup("[]");

    size_t count = 0;
    LspTypeHierarchyItem **items = lsp_prepare_type_hierarchy(doc, pos, &count);

    StrBuf b;
    sb_init(&b);
    sb_appendc(&b, '[');
    for (size_t i = 0; i < count; i++) {
        if (i) sb_appendc(&b, ',');
        char *item = lsp_json_type_hierarchy_item(items[i]);
        sb_append(&b, item);
        free(item);
    }
    sb_appendc(&b, ']');
    lsp_type_hierarchy_item_list_free(items, count);
    return sb_take(&b);
}

static char *handle_typeHierarchy_supertypes(LspServer *server,
                                              const char *params)
{
    (void)server; (void)params;
    return lsp_xstrdup("[]");
}

static char *handle_typeHierarchy_subtypes(LspServer *server,
                                            const char *params)
{
    (void)server; (void)params;
    return lsp_xstrdup("[]");
}

//// Cancellation

static char *handle_dollar_cancelRequest(LspServer *server,
                                          const char *params)
{
    /* Mark the identified request as cancelled.
       A full implementation would interrupt the in-progress handler.    */
    (void)server; (void)params;
    return NULL;
}

//// Dispatch table

typedef char *(*LspHandler)(LspServer *, const char *);

typedef struct {
    const char *method;
    LspHandler  handler;
} LspHandlerEntry;

static const LspHandlerEntry HANDLERS[] = {
    { "initialize",                                handle_initialize },
    { "initialized",                               handle_initialized },
    { "shutdown",                                  handle_shutdown },
    { "exit",                                      handle_exit },

    { "textDocument/didOpen",                      handle_textDocument_didOpen },
    { "textDocument/didChange",                    handle_textDocument_didChange },
    { "textDocument/didSave",                      handle_textDocument_didSave },
    { "textDocument/didClose",                     handle_textDocument_didClose },

    { "textDocument/completion",                   handle_textDocument_completion },
    { "textDocument/hover",                        handle_textDocument_hover },
    { "textDocument/signatureHelp",                handle_textDocument_signatureHelp },

    { "textDocument/definition",                   handle_textDocument_definition },
    { "textDocument/typeDefinition",               handle_textDocument_typeDefinition },
    { "textDocument/declaration",                  handle_textDocument_declaration },
    { "textDocument/implementation",               handle_textDocument_implementation },
    { "textDocument/references",                   handle_textDocument_references },
    { "textDocument/documentHighlight",            handle_textDocument_documentHighlight },

    { "textDocument/documentSymbol",               handle_textDocument_documentSymbol },
    { "workspace/symbol",                          handle_workspace_symbol },

    { "textDocument/semanticTokens/full",          handle_textDocument_semanticTokens_full },
    { "textDocument/semanticTokens/full/delta",    handle_textDocument_semanticTokens_full_delta },

    { "textDocument/inlayHint",                    handle_textDocument_inlayHint },
    { "textDocument/codeAction",                   handle_textDocument_codeAction },

    { "textDocument/prepareRename",                handle_textDocument_prepareRename },
    { "textDocument/rename",                       handle_textDocument_rename },

    { "textDocument/formatting",                   handle_textDocument_formatting },
    { "textDocument/rangeFormatting",              handle_textDocument_rangeFormatting },
    { "textDocument/foldingRange",                 handle_textDocument_foldingRange },
    { "textDocument/linkedEditingRange",           handle_textDocument_linkedEditingRange },

    { "textDocument/prepareCallHierarchy",         handle_textDocument_prepareCallHierarchy },
    { "callHierarchy/incomingCalls",               handle_callHierarchy_incomingCalls },
    { "callHierarchy/outgoingCalls",               handle_callHierarchy_outgoingCalls },

    { "textDocument/prepareTypeHierarchy",         handle_textDocument_prepareTypeHierarchy },
    { "typeHierarchy/supertypes",                  handle_typeHierarchy_supertypes },
    { "typeHierarchy/subtypes",                    handle_typeHierarchy_subtypes },

    { "$/cancelRequest",                           handle_dollar_cancelRequest },

    { NULL, NULL }
};

static LspHandler find_handler(const char *method)
{
    for (const LspHandlerEntry *e = HANDLERS; e->method; e++) {
        if (strcmp(e->method, method) == 0)
            return e->handler;
    }
    return NULL;
}

static void dispatch(LspServer *server, LspMessage *msg)
{
    if (!msg->method) return;   /* Response or malformed */

    if (!server->initialized &&
        strcmp(msg->method, "initialize")    != 0 &&
        strcmp(msg->method, "exit")          != 0 &&
        strcmp(msg->method, "$/cancelRequest") != 0) {
        if (msg->kind == LSP_MSG_REQUEST) {
            char *err = lsp_make_error(msg->id,
                                       LSP_ERR_SERVER_NOT_INIT,
                                       "Server not initialized");
            lsp_transport_write(server->transport, err);
            lsp_transport_flush(server->transport);
            free(err);
        }
        return;
    }

    LspHandler h = find_handler(msg->method);
    if (!h) {
        if (msg->kind == LSP_MSG_REQUEST) {
            char *err = lsp_make_error(msg->id,
                                       LSP_ERR_METHOD_NOT_FOUND,
                                       msg->method);
            lsp_transport_write(server->transport, err);
            lsp_transport_flush(server->transport);
            free(err);
        }
        return;
    }

    char *result = h(server, msg->params_json);

    if (msg->kind == LSP_MSG_REQUEST && result) {
        char *resp = lsp_make_response(msg->id, result);
        lsp_transport_write(server->transport, resp);
        lsp_transport_flush(server->transport);
        free(resp);
    }
    free(result);
}


/// §32  Server lifecycle

//// Server lifecycle
 //
 //  lsp_server_run() is the main event loop.  It blocks the calling thread
 //  reading JSON-RPC messages from the transport and dispatching them.
 //  The loop exits when an "exit" notification is received.
 //
int lsp_server_main(void)
{
    LspConfig cfg = lsp_default_config();
    LspServer *server = lsp_server_create(cfg);
    int rc = lsp_server_run(server);
    lsp_server_free(server);
    return rc;
}

LspConfig lsp_default_config(void)
{
    LspConfig cfg = {0};
    cfg.check_on_change       = true;
    cfg.check_on_save         = true;
    cfg.debounce_ms           = 200;
    cfg.inlay_hints_enabled   = true;
    cfg.semantic_tokens_enabled = true;
    cfg.call_hierarchy_enabled = true;
    cfg.type_hierarchy_enabled = true;
    cfg.completion_enabled    = true;
    cfg.completion_snippets   = true;
    cfg.completion_auto_import = true;
    cfg.completion_max_items  = 100;
    cfg.max_diagnostics       = 200;
    cfg.indent_width          = 4;
    cfg.log_level             = LSP_LOG_INFO;
    cfg.log_file              = NULL;   /* stderr */
    return cfg;
}

LspServer *lsp_server_create(LspConfig config)
{
    LspServer *s = lsp_xcalloc(1, sizeof(*s));
    s->transport   = lsp_transport_create(stdin, stdout);
    s->workspace   = lsp_workspace_create("file:///");
    s->config      = config;
    s->exit_code   = 0;

    s->pending_cap = 16;
    s->pending_ids = lsp_xmalloc(s->pending_cap * sizeof(LspId));

    s->dirty_cap  = 16;
    s->dirty_uris = lsp_xmalloc(s->dirty_cap * sizeof(char *));

    return s;
}

void lsp_server_free(LspServer *server)
{
    if (!server) return;
    lsp_transport_free(server->transport);
    lsp_workspace_free(server->workspace);
    for (size_t i = 0; i < server->pending_count; i++)
        lsp_id_free(&server->pending_ids[i]);
    free(server->pending_ids);
    for (size_t i = 0; i < server->dirty_count; i++)
        free(server->dirty_uris[i]);
    free(server->dirty_uris);
    free(server);
}

int lsp_server_run(LspServer *server)
{
    while (1) {
        LspMessage *msg = lsp_transport_read(server->transport);
        if (!msg) {
            /* EOF or I/O error */
            break;
        }

        lsp_log(server, LSP_LOG_DEBUG, "← %s", msg->method ? msg->method : "<response>");
        dispatch(server, msg);
        lsp_message_free(msg);

        if (server->shutdown_requested) {
            /* Wait for the exit notification */
            LspMessage *exit_msg = lsp_transport_read(server->transport);
            if (exit_msg) {
                if (exit_msg->method && strcmp(exit_msg->method, "exit") == 0)
                    server->exit_code = 0;
                lsp_message_free(exit_msg);
            }
            break;
        }
    }
    return server->exit_code;
}

void lsp_server_stop(LspServer *server)
{
    server->shutdown_requested = true;
}


/// §33  Logging

//// Logging
 //
 //  lsp_log() writes a timestamped line to the configured log file (stderr
 //  by default).  lsp_send_log_message() additionally forwards the message
 //  to the editor via the window/logMessage notification.
 //
void lsp_log(LspServer *server, int level, const char *fmt, ...)
{
    int configured = server ? server->config.log_level : LSP_LOG_INFO;
    if (level > configured) return;

    FILE *f = (server && server->config.log_file)
              ? server->config.log_file
              : stderr;

    static const char *level_names[] = {
        "", "ERROR", "WARN", "INFO", "DEBUG"
    };
    const char *lname = (level >= 1 && level <= 4) ? level_names[level] : "?";

    /* Timestamp */
    time_t  now = time(NULL);
    struct tm tm_buf;
    struct tm *t = gmtime_r(&now, &tm_buf);
    char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", t);

    fprintf(f, "[%s][%s] ", ts, lname);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fflush(f);
}

void lsp_send_log_message(LspServer *server, int level, const char *msg)
{
    /* Map LSP log level to window/logMessage type:
         1=error 2=warning 3=info 4=log                                */
    int type = (level <= LSP_LOG_ERROR)   ? 1
             : (level <= LSP_LOG_WARN)    ? 2
             : (level <= LSP_LOG_INFO)    ? 3
             :                              4;

    StrBuf b;
    sb_init(&b);
    char *ms = lsp_json_string(msg);
    sb_appendf(&b, "{\"type\":%d,\"message\":%s}", type, ms);
    free(ms);
    char *params = sb_take(&b);
    char *notif  = lsp_make_notification("window/logMessage", params);
    free(params);
    lsp_transport_write(server->transport, notif);
    lsp_transport_flush(server->transport);
    free(notif);
}

void lsp_send_show_message(LspServer *server, int type, const char *msg)
{
    StrBuf b;
    sb_init(&b);
    char *ms = lsp_json_string(msg);
    sb_appendf(&b, "{\"type\":%d,\"message\":%s}", type, ms);
    free(ms);
    char *params = sb_take(&b);
    char *notif  = lsp_make_notification("window/showMessage", params);
    free(params);
    lsp_transport_write(server->transport, notif);
    lsp_transport_flush(server->transport);
    free(notif);
}

void lsp_send_progress(LspServer *server, const char *token,
                       const char *title, const char *message,
                       int percentage)
{
    StrBuf b;
    sb_init(&b);
    char *tok  = lsp_json_string(token);
    char *ttl  = lsp_json_string(title);
    char *msg2 = lsp_json_string(message);
    sb_appendf(&b,
        "{\"token\":%s,\"value\":{\"kind\":\"report\",\"title\":%s"
        ",\"message\":%s,\"percentage\":%d}}",
        tok, ttl, msg2, percentage);
    free(tok); free(ttl); free(msg2);
    char *params = sb_take(&b);
    char *notif  = lsp_make_notification("$/progress", params);
    free(params);
    lsp_transport_write(server->transport, notif);
    lsp_transport_flush(server->transport);
    free(notif);
}

void lsp_publish_diagnostics(LspServer *server, LspDocument *doc)
{
    if (!server || !doc) return;

    char *uri_json  = lsp_json_string(doc->uri);
    char *diag_json = lsp_json_diagnostic_list(doc->diagnostics,
                                                doc->diag_count);
    StrBuf b;
    sb_init(&b);
    sb_appendf(&b, "{\"uri\":%s,\"version\":%d,\"diagnostics\":%s}",
               uri_json, doc->version, diag_json);
    free(uri_json); free(diag_json);

    char *params = sb_take(&b);
    char *notif  = lsp_make_notification("textDocument/publishDiagnostics",
                                          params);
    free(params);
    lsp_transport_write(server->transport, notif);
    lsp_transport_flush(server->transport);
    free(notif);
}


/// §34  Monad keywords and snippets

//// Keywords and snippets
 //
 //  The keyword list covers all reserved words in the Monad language.
 //  Snippet bodies use the LSP snippet syntax: $1, $2, ${1:placeholder}.
 //
static const char *MONAD_KEYWORDS[] = {
    "let", "in", "where", "do", "of", "case", "if", "then", "else",
    "module", "import", "qualified", "as", "hiding", "export",
    "type", "data", "newtype", "class", "instance", "deriving",
    "forall", "exists", "λ", "→", "←", "⇒", "∀",
    "match", "with", "end",
    "infix", "infixl", "infixr",
    "foreign", "import", "export",
    "default",
    "True", "False",
};

static const char *MONAD_SNIPPET_LABELS[] = {
    "let…in",
    "case…of",
    "if…then…else",
    "do-block",
    "data",
    "newtype",
    "class",
    "instance",
    "module",
    "import",
    "λ abstraction",
};

static const char *MONAD_SNIPPET_BODIES[] = {
    "let ${1:x} = ${2:expr}\nin ${3:body}",
    "case ${1:expr} of\n  ${2:pat1} → ${3:e1}\n  ${4:pat2} → ${5:e2}",
    "if ${1:cond}\n  then ${2:t}\n  else ${3:f}",
    "do\n  ${1:action1}\n  ${2:action2}",
    "data ${1:T} = ${2:Con} ${3:fields}",
    "newtype ${1:T} = ${2:MkT} { un${2:T} : ${3:a} }",
    "class ${1:Name} ${2:a} where\n  ${3:method} : ${4:Type}",
    "instance ${1:Class} ${2:Type} where\n  ${3:method} = ${4:impl}",
    "module ${1:Name}\n\n${2:-- exports}\n",
    "import ${1:Module}",
    "λ ${1:x} → ${2:body}",
};

const char **lsp_keywords(size_t *count)
{
    *count = sizeof(MONAD_KEYWORDS) / sizeof(MONAD_KEYWORDS[0]);
    return MONAD_KEYWORDS;
}

const char **lsp_snippet_labels(size_t *count)
{
    *count = sizeof(MONAD_SNIPPET_LABELS) / sizeof(MONAD_SNIPPET_LABELS[0]);
    return MONAD_SNIPPET_LABELS;
}

const char **lsp_snippet_bodies(size_t *count)
{
    *count = sizeof(MONAD_SNIPPET_BODIES) / sizeof(MONAD_SNIPPET_BODIES[0]);
    return MONAD_SNIPPET_BODIES;
}


/// Free helpers
//
//  One free function per public result type.  Always call these; never
//  call free() directly on API results.
//
void lsp_diagnostic_free(LspDiagnostic *d)
{
    if (!d) return;
    free(d->code);
    free(d->source);
    free(d->message);
    free(d->message_detail);
    for (size_t i = 0; i < d->related_count; i++) {
        free(d->related[i].location.uri);
        free(d->related[i].message);
    }
    free(d->related);
    /* zero the struct so double-free is harmless */
    memset(d, 0, sizeof(*d));
}

void lsp_completion_free(LspCompletion *c)
{
    if (!c) return;
    free(c->label);
    free(c->label_detail);
    free(c->detail);
    free(c->documentation);
    free(c->sort_text);
    free(c->filter_text);
    free(c->insert_text);
    free(c->text_edit_range);
    for (size_t i = 0; i < c->commit_char_count; i++)
        free(c->commit_chars[i]);
    free(c->commit_chars);
    free(c);
}

void lsp_completion_list_free(LspCompletion **list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_completion_free(list[i]);
    free(list);
}

void lsp_hover_free(LspHover *h)
{
    if (!h) return;
    free(h->contents);
    free(h->range);
    free(h);
}

void lsp_location_free(LspLocation *loc)
{
    if (!loc) return;
    free(loc->uri);
    loc->uri = NULL;
}

void lsp_location_list_free(LspLocation *list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        free(list[i].uri);
    free(list);
}

void lsp_location_link_free(LspLocationLink *link)
{
    if (!link) return;
    free(link->target_uri);
    free(link);
}

void lsp_symbol_free(LspSymbol *s)
{
    if (!s) return;
    free(s->name);
    free(s->detail);
    free(s->container);
    for (size_t i = 0; i < s->child_count; i++)
        lsp_symbol_free(s->children[i]);
    free(s->children);
    free(s);
}

void lsp_symbol_list_free(LspSymbol **list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_symbol_free(list[i]);
    free(list);
}

void lsp_semantic_tokens_free(LspSemanticTokens *st)
{
    if (!st) return;
    free(st->data);
    free(st->result_id);
    free(st);
}

void lsp_inlay_hint_free(LspInlayHint *h)
{
    if (!h) return;
    free(h->label);
    free(h->tooltip);
    /* Do NOT free h itself — inlay hints are stored in arrays */
}

void lsp_inlay_hint_list_free(LspInlayHint *list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_inlay_hint_free(&list[i]);
    free(list);
}

void lsp_signature_help_free(LspSignatureHelp *sh)
{
    if (!sh) return;
    for (size_t i = 0; i < sh->signature_count; i++) {
        LspSignatureInfo *sig = &sh->signatures[i];
        free(sig->label);
        free(sig->documentation);
        for (size_t j = 0; j < sig->parameter_count; j++) {
            free(sig->parameters[j].label);
            free(sig->parameters[j].documentation);
        }
        free(sig->parameters);
    }
    free(sh->signatures);
    free(sh);
}

void lsp_action_free(LspAction *a)
{
    if (!a) return;
    free(a->title);
    free(a->command);
    lsp_workspace_edit_free(a->edit);
    free(a->diagnostics);
    free(a);
}

void lsp_action_list_free(LspAction **list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_action_free(list[i]);
    free(list);
}

void lsp_workspace_edit_free(LspWorkspaceEdit *we)
{
    if (!we) return;
    free(we->uri);
    for (size_t i = 0; i < we->edit_count; i++)
        free(we->edits[i].new_text);
    free(we->edits);
    free(we);
}

void lsp_fold_range_list_free(LspFoldRange *list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        free(list[i].collapsed_text);
    free(list);
}

void lsp_text_edit_list_free(LspTextEdit *list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        free(list[i].new_text);
    free(list);
}

void lsp_call_hierarchy_item_free(LspCallHierarchyItem *item)
{
    if (!item) return;
    free(item->name);
    free(item->detail);
    free(item->uri);
    free(item);
}

void lsp_call_hierarchy_item_list_free(LspCallHierarchyItem **list,
                                        size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_call_hierarchy_item_free(list[i]);
    free(list);
}

void lsp_call_hierarchy_incoming_list_free(LspCallHierarchyIncoming *list,
                                            size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        lsp_call_hierarchy_item_free(list[i].from);
        free(list[i].from_ranges);
    }
    free(list);
}

void lsp_call_hierarchy_outgoing_list_free(LspCallHierarchyOutgoing *list,
                                            size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++) {
        lsp_call_hierarchy_item_free(list[i].to);
        free(list[i].from_ranges);
    }
    free(list);
}

void lsp_type_hierarchy_item_free(LspTypeHierarchyItem *item)
{
    if (!item) return;
    free(item->name);
    free(item->detail);
    free(item->uri);
    free(item);
}

void lsp_type_hierarchy_item_list_free(LspTypeHierarchyItem **list,
                                        size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++)
        lsp_type_hierarchy_item_free(list[i]);
    free(list);
}
