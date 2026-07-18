#include <stdio.h>
#include <stdlib.h>


/*
 * Keep the standalone failure handlers in their own archive member. The REPL
 * supplies recovery-aware versions, so its executable does not extract this
 * object. Compiled programs do extract it, giving COFF and ELF linkers the
 * same strong runtime symbols without platform-specific weak linkage.
 */
void __monad_runtime_error(const char *file, long line, long col, const char *msg)
{
    fprintf(stderr, "%s:%ld:%ld: \x1b[31;1merror:\x1b[0m %s\n",
            file ? file : "<input>", line, col, msg);
    abort();
}


void __monad_assert_fail(const char *label)
{
    fprintf(stderr,
            "<input>:0:0: \x1b[31;1merror:\x1b[0m Assertion failed: %s\n",
            label);
    abort();
}
