#ifndef MONAD_NATIVE_COMPILE_H
#define MONAD_NATIVE_COMPILE_H

#include "frontend_transaction.h"

typedef struct MonadNativeImage MonadNativeImage;

bool monad_native_image_build(
    const MonadCompilationUnit *unit, MonadNativeImage **image,
    const char **failed_definition);
size_t monad_native_image_count(const MonadNativeImage *image);
const char *monad_native_image_name(const MonadNativeImage *image, size_t index);
const char *monad_native_image_docstring(
    const MonadNativeImage *image, size_t index);
void (*monad_native_image_address(
    const MonadNativeImage *image, size_t index))(void);
void monad_native_image_destroy(void *image);

#endif
