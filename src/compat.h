#ifndef MONAD_COMPAT_H
#define MONAD_COMPAT_H

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#ifndef MONAD_THREAD_LOCAL
#if defined(_MSC_VER)
#define MONAD_THREAD_LOCAL __declspec(thread)
#else
#define MONAD_THREAD_LOCAL __thread
#endif
#endif
#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

#if defined(_WIN32) && !defined(strndup)
static inline char *monad_strndup(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}
#define strndup monad_strndup
#endif

#if defined(_WIN32)
static inline int monad_vasprintf(char **out, const char *format, va_list args)
{
    if (!out || !format) return -1;
    va_list measure;
    va_copy(measure, args);
    int length = _vscprintf(format, measure);
    va_end(measure);
    if (length < 0) return -1;

    char *buffer = (char *)malloc((size_t)length + 1);
    if (!buffer) return -1;
    int written = vsnprintf(buffer, (size_t)length + 1, format, args);
    if (written < 0) {
        free(buffer);
        return -1;
    }
    *out = buffer;
    return written;
}

static inline int monad_asprintf(char **out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = monad_vasprintf(out, format, args);
    va_end(args);
    return result;
}

#define asprintf monad_asprintf
#ifndef lstat
#define lstat stat
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif
#endif

static inline int monad_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static inline int monad_setenv(const char *name, const char *value)
{
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

#endif
