#ifndef MONAD_COMPAT_H
#define MONAD_COMPAT_H

#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
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

static inline int monad_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

#endif
