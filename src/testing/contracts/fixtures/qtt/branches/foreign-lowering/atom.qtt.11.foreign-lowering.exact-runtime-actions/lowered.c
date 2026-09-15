#include <monad/embed.h>

monad_foreign_object_t *lowered_foreign_move(monad_foreign_object_t *object) {
    return object;
}

void lowered_foreign_drop(monad_foreign_object_t *object) {
    monad_foreign_object_release(object);
}

monad_foreign_object_t *lowered_foreign_dup(monad_foreign_object_t *object) {
    monad_foreign_object_retain_shared(object);
    return object;
}
