/// lsp_repl.c -- Interactive LSP test REPL for Monad
//
//  Architecture:
//
//    main thread        background reader thread
//    -----------        ------------------------
//    print prompt  <--  drains server stdout continuously
//    send request       classifies messages:
//    wait on resp_q       "id" present  -> response_queue
//    print response       "method" only -> notif_queue
//                       prints notifications inline (async)
//
//  This gives the REPL the same message model a real editor uses:
//  notifications arrive between commands, responses are matched
//  to the request that triggered them.
//
#include "lsp_repl.h"
#include "completion.h"

#include <stdio.h>

#if defined(_WIN32)
int lsp_repl_run(void)
{
    fprintf(stderr, "LSP REPL is not available on this Windows build.\n");
    return 1;
}
#else

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pthread.h>
#include <termios.h>
#include <time.h>

/// ANSI

#define RESET    "\x1b[0m"
#define BOLD     "\x1b[1m"
#define DIM      "\x1b[2m"
#define ITALIC   "\x1b[3m"
#define CYAN     "\x1b[36m"
#define YELLOW   "\x1b[33m"
#define MAGENTA  "\x1b[35m"
#define GREEN    "\x1b[32m"
#define RED      "\x1b[31m"
#define BLUE     "\x1b[34m"
#define WHITE    "\x1b[97m"
#define BG_RED   "\x1b[41m"
#define BG_GREEN "\x1b[42m"

/* Move cursor up N lines, clear to end of screen */
#define CURSOR_UP(n)    "\x1b[" #n "A"
#define CLEAR_EOL       "\x1b[K"
#define CURSOR_COL(n)   "\x1b[" #n "G"
#define SAVE_CURSOR     "\x1b[s"
#define RESTORE_CURSOR  "\x1b[u"

//  Message queue (lock-free single-producer single-consumer via      */
//  mutex + condvar — simple and correct for our throughput)          */

#define QUEUE_CAP 64

typedef struct {
    char   *msgs[QUEUE_CAP];
    int     head;
    int     tail;
    int     count;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} MsgQueue;

static void queue_init(MsgQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

static void queue_push(MsgQueue *q, char *msg)
{
    pthread_mutex_lock(&q->mu);
    if (q->count < QUEUE_CAP) {
        q->msgs[q->tail] = msg;
        q->tail = (q->tail + 1) % QUEUE_CAP;
        q->count++;
        pthread_cond_signal(&q->cv);
    } else {
        free(msg); /* drop if full — shouldn't happen in practice */
    }
    pthread_mutex_unlock(&q->mu);
}

/* Returns NULL immediately if empty (non-blocking). */
static char *queue_pop_nowait(MsgQueue *q)
{
    pthread_mutex_lock(&q->mu);
    char *msg = NULL;
    if (q->count > 0) {
        msg = q->msgs[q->head];
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return msg;
}

/* Blocks until a message arrives or timeout_ms elapses. */
static char *queue_pop_timeout(MsgQueue *q, int timeout_ms)
{
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
    }
    char *msg = NULL;
    if (q->count > 0) {
        msg = q->msgs[q->head];
        q->head = (q->head + 1) % QUEUE_CAP;
        q->count--;
    }
    pthread_mutex_unlock(&q->mu);
    return msg;
}

/// Global REPL state

typedef struct {
    /* Server process */
    pid_t   server_pid;
    int     wr;             /* parent -> server (write) */
    int     rd;             /* server -> parent (read)  */

    /* Message routing */
    MsgQueue resp_q;        /* responses (have "id") */
    MsgQueue notif_q;       /* notifications (have "method", no "id") */

    /* Request ID counter */
    int     req_id;

    /* Diagnostic store: last diag per URI */
    char   *diag_uri[32];
    char   *diag_json[32];
    int     diag_count;

    /* Server state */
    volatile int server_alive;
    volatile int initialized;

    /* Terminal */
    struct termios orig_termios;
    int    is_tty;

    /* Prompt redraw mutex (background thread prints notifs) */
    pthread_mutex_t print_mu;
} ReplState;

static ReplState G;

/// JSON-RPC framing

static void rpc_send(const char *json)
{
    char hdr[64];
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "Content-Length: %zu\r\n\r\n", strlen(json));
    write(G.wr, hdr,  (size_t)hlen);
    write(G.wr, json, strlen(json));
}

/* Read one framed message from fd. Returns heap string or NULL. */
static char *rpc_recv_raw(int fd)
{
    size_t content_length = 0;
    char   hbuf[8];
    int    hpos = 0;
    char   line[512];
    int    lpos = 0;

    /* Read headers byte by byte */
    while (1) {
        ssize_t n = read(fd, hbuf, 1);
        if (n <= 0) return NULL;
        char c = hbuf[0];

        if (lpos < (int)sizeof(line) - 1)
            line[lpos++] = c;

        if (c == '\n') {
            line[lpos] = '\0';
            /* Blank line = end of headers */
            int blank = 1;
            for (int i = 0; i < lpos; i++)
                if (line[i] != '\r' && line[i] != '\n')
                    { blank = 0; break; }
            if (blank) break;

            if (strncasecmp(line, "Content-Length:", 15) == 0) {
                const char *v = line + 15;
                while (*v == ' ') v++;
                content_length = (size_t)strtoull(v, NULL, 10);
            }
            lpos = 0;
        }
    }

    if (content_length == 0) return NULL;

    char *buf = malloc(content_length + 1);
    if (!buf) return NULL;

    size_t total = 0;
    while (total < content_length) {
        ssize_t n = read(fd, buf + total, content_length - total);
        if (n <= 0) { free(buf); return NULL; }
        total += (size_t)n;
    }
    buf[content_length] = '\0';
    return buf;
}

/// Tiny JSON field extractors (same approach as lsp.c)

static int json_has_field(const char *json, const char *field)
{
    if (!json) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    return strstr(json, needle) != NULL;
}

static char *json_get_str(const char *json, const char *field)
{
    if (!json) return NULL;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\') end++;
        if (*end) end++;
    }
    return strndup(p, (size_t)(end - p));
}

/// Pretty printer (depth-colored JSON)

static void pretty_print(const char *json)
{
    static const char *DC[] = {
        WHITE, CYAN, YELLOW, MAGENTA, GREEN, BLUE
    };
    int   NDC   = 6;
    int   depth = 0;
    int   in_str = 0;
    size_t len  = strlen(json);

    for (size_t i = 0; i < len; i++) {
        char c = json[i];

        if (in_str) {
            if (c == '\\' && i + 1 < len) {
                putchar(c); putchar(json[++i]); continue;
            }
            if (c == '"') { in_str = 0; fputs(RESET, stdout); }
            else putchar(c);
            continue;
        }

        switch (c) {
        case '"': {
            in_str = 1;
            /* Is this a key? Scan for closing quote then ':' */
            size_t j = i + 1;
            while (j < len && json[j] != '"') {
                if (json[j] == '\\') j++;
                j++;
            }
            j++;
            while (j < len && (json[j]==' '||json[j]=='\t')) j++;
            fputs(json[j] == ':' ? CYAN : GREEN, stdout);
            putchar(c);
            break;
        }
        case '{': case '[':
            fputs(DC[depth % NDC], stdout);
            putchar(c);
            fputs(RESET, stdout);
            depth++;
            putchar('\n');
            for (int d = 0; d < depth; d++) fputs("  ", stdout);
            break;
        case '}': case ']':
            depth--;
            putchar('\n');
            for (int d = 0; d < depth; d++) fputs("  ", stdout);
            fputs(DC[depth % NDC], stdout);
            putchar(c);
            fputs(RESET, stdout);
            break;
        case ',':
            putchar(c);
            putchar('\n');
            for (int d = 0; d < depth; d++) fputs("  ", stdout);
            break;
        case ':':
            fputs(RESET, stdout);
            fputs(": ", stdout);
            break;
        case ' ': case '\t': case '\n': case '\r':
            break;
        default:
            fputs(DIM, stdout);
            putchar(c);
            fputs(RESET, stdout);
            break;
        }
    }
    fputs(RESET, stdout);
    putchar('\n');
}

/// Diagnostic pretty-printer

/* Extract array items from a JSON array string (shallow, bracket-aware). */
static void print_diagnostics_json(const char *diag_json, const char *uri)
{
    /* Find "diagnostics":[ */
    const char *p = strstr(diag_json, "\"diagnostics\":");
    if (!p) { fprintf(stdout, DIM "  (no diagnostics key)\n" RESET); return; }
    p += 14;
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;

    /* Short filename from URI */
    const char *slash = strrchr(uri ? uri : "", '/');
    const char *fname = slash ? slash + 1 : (uri ? uri : "?");

    int count = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == '\n' || *p == ',') p++;
        if (*p != '{') break;

        /* Capture one object */
        int   depth = 0;
        const char *start = p;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        char *item = strndup(start, (size_t)(p - start));

        /* severity */
        char *sev_str = json_get_str(item, "severity");
        int   sev = sev_str ? atoi(sev_str) : 1;
        free(sev_str);
        const char *sev_color = (sev == 1) ? RED :
                                (sev == 2) ? YELLOW :
                                (sev == 3) ? BLUE  : DIM;
        const char *sev_name  = (sev == 1) ? "error" :
                                (sev == 2) ? "warn " :
                                (sev == 3) ? "info " : "hint ";

        /* message */
        char *msg = json_get_str(item, "message");

        /* line (from range.start.line) */
        const char *rp = strstr(item, "\"start\":");
        int line = -1, col = -1;
        if (rp) {
            const char *lp = strstr(rp, "\"line\":");
            if (lp) line = atoi(lp + 7);
            const char *cp = strstr(rp, "\"character\":");
            if (cp) col  = atoi(cp + 12);
        }

        fprintf(stdout,
            "  %s%s" RESET "  " DIM "%s" RESET ":%d:%d  %s\n",
            sev_color, sev_name,
            fname, line + 1, col + 1,
            msg ? msg : "");

        free(msg);
        free(item);
        count++;
    }

    if (count == 0)
        fprintf(stdout,
            "  " GREEN "no diagnostics" RESET " for %s\n", fname);
}

/// Notification handler (called from background thread)

static void store_diagnostics(const char *params_json)
{
    char *uri = json_get_str(params_json, "uri");
    if (!uri) return;

    /* Replace existing entry for this URI */
    for (int i = 0; i < G.diag_count; i++) {
        if (G.diag_uri[i] && strcmp(G.diag_uri[i], uri) == 0) {
            free(G.diag_json[i]);
            G.diag_json[i] = strdup(params_json);
            free(uri);
            return;
        }
    }
    if (G.diag_count < 32) {
        G.diag_uri[G.diag_count]  = uri;
        G.diag_json[G.diag_count] = strdup(params_json);
        G.diag_count++;
    } else {
        free(uri);
    }
}

static void handle_notification(const char *method, const char *json)
{
    pthread_mutex_lock(&G.print_mu);

    if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
        /* Store for later 'diag' command */
        store_diagnostics(json);

        /* Count diagnostics */
        const char *dp = strstr(json, "\"diagnostics\":");
        int nerr = 0, nwarn = 0;
        if (dp) {
            const char *p = dp + 14;
            while (*p && *p != '[') p++;
            if (*p == '[') {
                p++;
                while (*p && *p != ']') {
                    if (*p == '{') {
                        /* Find severity */
                        const char *sp = strstr(p, "\"severity\":");
                        if (sp) {
                            int s = atoi(sp + 11);
                            if (s == 1) nerr++;
                            else if (s == 2) nwarn++;
                        }
                        /* Skip to next item */
                        int d = 0;
                        while (*p) {
                            if (*p == '{') d++;
                            else if (*p == '}') { d--; if (!d) { p++; break; } }
                            p++;
                        }
                    } else p++;
                }
            }
        }

        char *uri = json_get_str(json, "uri");
        const char *slash = uri ? strrchr(uri, '/') : NULL;
        const char *fname = slash ? slash + 1 : (uri ? uri : "?");

        /* Inline notification line */
        fprintf(stdout, "\n");
        if (nerr > 0)
            fprintf(stdout,
                "  " RED "diag" RESET "  %s  "
                RED "%d error%s" RESET,
                fname, nerr, nerr == 1 ? "" : "s");
        else
            fprintf(stdout,
                "  " GREEN "diag" RESET "  %s  "
                GREEN "clean" RESET, fname);

        if (nwarn > 0)
            fprintf(stdout, "  " YELLOW "%d warning%s" RESET,
                    nwarn, nwarn == 1 ? "" : "s");

        fprintf(stdout,
            "  " DIM "(run 'diag' to expand)" RESET "\n\n");
        fflush(stdout);
        free(uri);

    } else if (strcmp(method, "window/logMessage") == 0 ||
               strcmp(method, "window/showMessage") == 0) {
        char *msg = json_get_str(json, "message");
        fprintf(stdout,
            "\n  " DIM "server:" RESET " %s\n\n",
            msg ? msg : "");
        free(msg);
        fflush(stdout);

    } else if (strcmp(method, "$/progress") == 0) {
        /* Silently ignore progress for now */
    } else {
        /* Unknown notification — show dim */
        fprintf(stdout,
            "\n  " DIM "notif: %s" RESET "\n\n", method);
        fflush(stdout);
    }

    pthread_mutex_unlock(&G.print_mu);
}

/// Background reader thread

static void *reader_thread(void *arg)
{
    (void)arg;
    while (G.server_alive) {
        char *raw = rpc_recv_raw(G.rd);
        if (!raw) {
            G.server_alive = 0;
            /* Wake main thread if it's waiting on a response */
            queue_push(&G.resp_q, NULL);
            break;
        }

        /* Route by message type */
        int has_id     = json_has_field(raw, "id");
        int has_method = json_has_field(raw, "method");

        if (has_method && !has_id) {
            /* Notification — handle inline, don't queue */
            char *method = json_get_str(raw, "method");
            if (method) {
                handle_notification(method, raw);
                free(method);
            }
            free(raw);
        } else if (has_id) {
            /* Response to a request */
            queue_push(&G.resp_q, raw);
        } else {
            free(raw);
        }
    }
    return NULL;
}

/// Send a request and wait for its response (with timeout)

static char *send_request(const char *json)
{
    pthread_mutex_lock(&G.print_mu);
    fprintf(stderr, DIM "  --> %zu bytes\n" RESET, strlen(json));
    pthread_mutex_unlock(&G.print_mu);

    rpc_send(json);

    /* Wait up to 5 seconds */
    char *resp = queue_pop_timeout(&G.resp_q, 5000);
    if (!resp) {
        pthread_mutex_lock(&G.print_mu);
        fprintf(stderr, RED "  <-- timeout (no response in 5s)\n" RESET);
        pthread_mutex_unlock(&G.print_mu);
        return NULL;
    }

    pthread_mutex_lock(&G.print_mu);
    fprintf(stderr, DIM "  <-- %zu bytes\n" RESET, strlen(resp));
    putchar('\n');
    pretty_print(resp);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&G.print_mu);

    return resp;
}

/* Send a notification (fire and forget — no response expected). */
static void send_notif(const char *json)
{
    rpc_send(json);
}

/// Command implementations

static void cmd_init(void)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\","
        "\"params\":{\"processId\":%d,"
        "\"clientInfo\":{\"name\":\"monad-lsp-repl\",\"version\":\"0.1\"},"
        "\"rootUri\":null,\"capabilities\":{"
        "\"textDocument\":{\"publishDiagnostics\":{\"relatedInformation\":true}}}}}",
        G.req_id++, (int)getpid());

    char *resp = send_request(buf);
    free(resp);

    send_notif("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

    G.initialized = 1;
    pthread_mutex_lock(&G.print_mu);
    fprintf(stdout, GREEN "  initialized" RESET
            "  server ready\n\n");
    fflush(stdout);
    pthread_mutex_unlock(&G.print_mu);
}

static void cmd_open(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, RED "  cannot open '%s': %s\n\n" RESET,
                path, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)sz + 1);
    fread(src, 1, (size_t)sz, f);
    src[sz] = '\0';
    fclose(f);

    /* JSON-escape source */
    char *esc = malloc((size_t)sz * 6 + 4);
    size_t ei = 0;
    esc[ei++] = '"';
    for (long j = 0; j < sz; j++) {
        unsigned char c = (unsigned char)src[j];
        if      (c == '"')  { esc[ei++]='\\'; esc[ei++]='"';  }
        else if (c == '\\') { esc[ei++]='\\'; esc[ei++]='\\'; }
        else if (c == '\n') { esc[ei++]='\\'; esc[ei++]='n';  }
        else if (c == '\r') { esc[ei++]='\\'; esc[ei++]='r';  }
        else if (c == '\t') { esc[ei++]='\\'; esc[ei++]='t';  }
        else                { esc[ei++]=(char)c; }
    }
    esc[ei++] = '"'; esc[ei] = '\0';
    free(src);

    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    size_t blen = strlen(uri) + (size_t)ei + 256;
    char *buf = malloc(blen);
    snprintf(buf, blen,
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
        "\"params\":{\"textDocument\":{"
        "\"uri\":\"%s\",\"languageId\":\"monad\","
        "\"version\":1,\"text\":%s}}}",
        uri, esc);
    free(esc);

    send_notif(buf);
    free(buf);

    fprintf(stdout,
        GREEN "  opened" RESET "  " DIM "%s" RESET
        "  " DIM "(waiting for diagnostics...)" RESET "\n\n",
        uri);
    fflush(stdout);

    /* Give the server up to 2s to push diagnostics */
    /* The background thread will print them automatically */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000L };
    nanosleep(&ts, NULL);
}

static void cmd_hover(const char *path, int line, int col)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/hover\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}}",
        G.req_id++, uri, line, col);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_complete(const char *path, int line, int col)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/completion\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}}",
        G.req_id++, uri, line, col);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_definition(const char *path, int line, int col)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/definition\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}}",
        G.req_id++, uri, line, col);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_references(const char *path, int line, int col)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/references\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d},"
        "\"context\":{\"includeDeclaration\":true}}}",
        G.req_id++, uri, line, col);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_symbols(const char *path)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/documentSymbol\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
        G.req_id++, uri);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_tokens(const char *path)
{
    char uri[1024];
    if (path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", path);
    else {
        char *pwd = getenv("PWD");
        snprintf(uri, sizeof(uri), "file://%s/%s", pwd ? pwd : "", path);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"textDocument/semanticTokens/full\","
        "\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}",
        G.req_id++, uri);
    char *resp = send_request(buf);
    free(resp);
}

static void cmd_diag(const char *path)
{
    if (G.diag_count == 0) {
        fprintf(stdout,
            "\n  " DIM "no diagnostics received yet"
            "  (run 'open <file>' first)" RESET "\n\n");
        return;
    }

    for (int i = 0; i < G.diag_count; i++) {
        if (path) {
            /* Match by basename */
            const char *slash = strrchr(G.diag_uri[i], '/');
            const char *fname = slash ? slash + 1 : G.diag_uri[i];
            if (strcmp(fname, path) != 0 &&
                strcmp(G.diag_uri[i], path) != 0)
                continue;
        }
        fprintf(stdout, "\n");
        print_diagnostics_json(G.diag_json[i], G.diag_uri[i]);
    }
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void cmd_raw(const char *json)
{
    if (json_has_field(json, "\"id\"")) {
        char *resp = send_request(json);
        free(resp);
    } else {
        send_notif(json);
        fprintf(stdout, DIM "  (notification sent)\n\n" RESET);
    }
}

static void cmd_shutdown(void)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,"
        "\"method\":\"shutdown\",\"params\":null}",
        G.req_id++);
    char *resp = send_request(buf);
    free(resp);

    send_notif("{\"jsonrpc\":\"2.0\","
               "\"method\":\"exit\",\"params\":null}");
    G.server_alive = 0;
}

/// Scripted test suite

static int test_pass = 0;
static int test_fail = 0;

static void test_check(const char *name, int cond)
{
    if (cond) {
        fprintf(stdout, "  " GREEN "pass" RESET "  %s\n", name);
        test_pass++;
    } else {
        fprintf(stdout, "  " RED "FAIL" RESET "  %s\n", name);
        test_fail++;
    }
}

static void cmd_test(void)
{
    fprintf(stdout,
        "\n" BOLD "  running lsp smoke tests" RESET "\n\n");

    test_pass = 0; test_fail = 0;

    /* Test 1: initialize */
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\","
            "\"params\":{\"processId\":%d,\"rootUri\":null,"
            "\"capabilities\":{}}}",
            G.req_id++, (int)getpid());
        rpc_send(buf);
        char *resp = queue_pop_timeout(&G.resp_q, 3000);
        test_check("initialize returns response",
                   resp && json_has_field(resp, "result"));
        test_check("capabilities present",
                   resp && strstr(resp, "\"capabilities\""));
        test_check("serverInfo present",
                   resp && strstr(resp, "\"serverInfo\""));
        test_check("completionProvider advertised",
                   resp && strstr(resp, "\"completionProvider\""));
        test_check("semanticTokensProvider advertised",
                   resp && strstr(resp, "\"semanticTokensProvider\""));
        free(resp);

        send_notif("{\"jsonrpc\":\"2.0\","
                   "\"method\":\"initialized\",\"params\":{}}");
        G.initialized = 1;
    }

    /* Test 2: shutdown */
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"method\":\"shutdown\",\"params\":null}",
            G.req_id++);
        rpc_send(buf);
        char *resp = queue_pop_timeout(&G.resp_q, 3000);
        test_check("shutdown returns null result",
                   resp && strstr(resp, "\"result\":null"));
        free(resp);
        send_notif("{\"jsonrpc\":\"2.0\","
                   "\"method\":\"exit\",\"params\":null}");
        G.server_alive = 0;
    }

    fprintf(stdout,
        "\n  " BOLD "%d passed" RESET "  " DIM "%d failed" RESET "\n\n",
        test_pass, test_fail);
}

/// Help

static void print_repl_help(void)
{
    fprintf(stdout,
        "\n"
        "  " BOLD "lifecycle" RESET "\n"
        "    " CYAN "init" RESET "                          "
            DIM "initialize the server (do this first)" RESET "\n"
        "    " CYAN "shutdown" RESET "                      "
            DIM "shutdown + exit the server" RESET "\n"
        "\n"
        "  " BOLD "documents" RESET "\n"
        "    " CYAN "open <file>" RESET "                   "
            DIM "send didOpen, wait for diagnostics" RESET "\n"
        "    " CYAN "diag [file]" RESET "                   "
            DIM "show stored diagnostics (all or per file)" RESET "\n"
        "\n"
        "  " BOLD "queries  " RESET DIM "(file line col are 0-based)" RESET "\n"
        "    " CYAN "hover    <file> <line> <col>" RESET "  "
            DIM "type info at position" RESET "\n"
        "    " CYAN "complete <file> <line> <col>" RESET "  "
            DIM "completion list at position" RESET "\n"
        "    " CYAN "def      <file> <line> <col>" RESET "  "
            DIM "go-to-definition" RESET "\n"
        "    " CYAN "refs     <file> <line> <col>" RESET "  "
            DIM "find all references" RESET "\n"
        "    " CYAN "symbols  <file>" RESET "               "
            DIM "document symbol outline" RESET "\n"
        "    " CYAN "tokens   <file>" RESET "               "
            DIM "semantic token data" RESET "\n"
        "\n"
        "  " BOLD "advanced" RESET "\n"
        "    " CYAN "raw <json>" RESET "                    "
            DIM "send raw JSON-RPC envelope" RESET "\n"
        "    " CYAN "test" RESET "                          "
            DIM "run built-in smoke test suite" RESET "\n"
        "    " CYAN "help" RESET "                          "
            DIM "show this message" RESET "\n"
        "    " CYAN "exit" RESET " / Ctrl-D                 "
            DIM "quit (sends shutdown first)" RESET "\n"
        "\n");
    fflush(stdout);
}

/// Prompt

static void print_prompt(void)
{
    const char *state = G.initialized
        ? GREEN "ready" RESET
        : YELLOW "uninit" RESET;
    fprintf(stderr,
        CYAN "lsp" RESET " " DIM "(%s)" RESET " " DIM ">" RESET " ",
        state);
    fflush(stderr);
}

/// REPL entry point

int lsp_repl_run(void)
{
    memset(&G, 0, sizeof(G));
    G.req_id      = 1;
    G.server_alive = 1;
    G.is_tty      = isatty(STDIN_FILENO);
    pthread_mutex_init(&G.print_mu, NULL);
    queue_init(&G.resp_q);
    queue_init(&G.notif_q);

    /* Print the structured LSP menu */
    print_subcommand_menu("lsp");
    print_repl_help();

    /* Fork the server */
    int to_server[2], from_server[2];
    if (pipe(to_server) || pipe(from_server)) {
        fprintf(stderr, RED "error: pipe: %s\n" RESET, strerror(errno));
        return 1;
    }

    G.server_pid = fork();
    if (G.server_pid < 0) {
        fprintf(stderr, RED "error: fork: %s\n" RESET, strerror(errno));
        return 1;
    }

    if (G.server_pid == 0) {
        /* Child: wire pipes to stdin/stdout */
        dup2(to_server[0],   STDIN_FILENO);
        dup2(from_server[1], STDOUT_FILENO);
        close(to_server[0]);  close(to_server[1]);
        close(from_server[0]); close(from_server[1]);
        extern int lsp_server_main(void);
        exit(lsp_server_main());
    }

    close(to_server[0]);
    close(from_server[1]);
    G.wr = to_server[1];
    G.rd = from_server[0];

    /* Start background reader thread */
    pthread_t rthread;
    pthread_create(&rthread, NULL, reader_thread, NULL);

    fprintf(stderr,
        "\n  " BOLD "Monad LSP REPL" RESET
        "  " DIM "server pid %d" RESET
        "  " DIM "type 'help' or 'init' to begin" RESET "\n\n",
        (int)G.server_pid);

    /* Main REPL loop */
    char line[4096];
    while (G.server_alive) {
        print_prompt();

        if (!fgets(line, sizeof(line), stdin)) {
            fputc('\n', stderr);
            break;
        }

        /* Strip newline */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';
        if (ll == 0) continue;

        char *tok = strtok(line, " \t");
        if (!tok) continue;

        /* ---- dispatch ---- */
        if (!strcmp(tok, "exit") || !strcmp(tok, "quit")) {
            if (G.server_alive && G.initialized) cmd_shutdown();
            break;
        }
        else if (!strcmp(tok, "help") || !strcmp(tok, "?")) {
            print_repl_help();
        }
        else if (!strcmp(tok, "init")) {
            cmd_init();
        }
        else if (!strcmp(tok, "open")) {
            char *path = strtok(NULL, " \t");
            if (!path) fprintf(stderr,
                YELLOW "  usage: open <file>\n\n" RESET);
            else cmd_open(path);
        }
        else if (!strcmp(tok, "diag")) {
            char *path = strtok(NULL, " \t");
            cmd_diag(path);
        }
        else if (!strcmp(tok, "hover")) {
            char *f = strtok(NULL, " \t");
            char *l = strtok(NULL, " \t");
            char *c = strtok(NULL, " \t");
            if (!f || !l || !c)
                fprintf(stderr,
                    YELLOW "  usage: hover <file> <line> <col>\n\n" RESET);
            else cmd_hover(f, atoi(l), atoi(c));
        }
        else if (!strcmp(tok, "complete")) {
            char *f = strtok(NULL, " \t");
            char *l = strtok(NULL, " \t");
            char *c = strtok(NULL, " \t");
            if (!f || !l || !c)
                fprintf(stderr,
                    YELLOW "  usage: complete <file> <line> <col>\n\n" RESET);
            else cmd_complete(f, atoi(l), atoi(c));
        }
        else if (!strcmp(tok, "def")) {
            char *f = strtok(NULL, " \t");
            char *l = strtok(NULL, " \t");
            char *c = strtok(NULL, " \t");
            if (!f || !l || !c)
                fprintf(stderr,
                    YELLOW "  usage: def <file> <line> <col>\n\n" RESET);
            else cmd_definition(f, atoi(l), atoi(c));
        }
        else if (!strcmp(tok, "refs")) {
            char *f = strtok(NULL, " \t");
            char *l = strtok(NULL, " \t");
            char *c = strtok(NULL, " \t");
            if (!f || !l || !c)
                fprintf(stderr,
                    YELLOW "  usage: refs <file> <line> <col>\n\n" RESET);
            else cmd_references(f, atoi(l), atoi(c));
        }
        else if (!strcmp(tok, "symbols")) {
            char *f = strtok(NULL, " \t");
            if (!f) fprintf(stderr,
                YELLOW "  usage: symbols <file>\n\n" RESET);
            else cmd_symbols(f);
        }
        else if (!strcmp(tok, "tokens")) {
            char *f = strtok(NULL, " \t");
            if (!f) fprintf(stderr,
                YELLOW "  usage: tokens <file>\n\n" RESET);
            else cmd_tokens(f);
        }
        else if (!strcmp(tok, "shutdown")) {
            cmd_shutdown();
        }
        else if (!strcmp(tok, "raw")) {
            char *rest = strtok(NULL, "");
            if (!rest) fprintf(stderr,
                YELLOW "  usage: raw <json>\n\n" RESET);
            else cmd_raw(rest);
        }
        else if (!strcmp(tok, "test")) {
            cmd_test();
        }
        else {
            fprintf(stderr,
                RED "  unknown: '%s'" RESET
                "  — type " CYAN "help" RESET "\n\n", tok);
        }
    }

    /* Teardown */
    close(G.wr);
    close(G.rd);
    pthread_join(rthread, NULL);

    int status = 0;
    waitpid(G.server_pid, &status, 0);
    fprintf(stderr,
        "\n" DIM "  server exited (status %d)\n" RESET "\n",
        WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    return 0;
}

#endif
