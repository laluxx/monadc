#ifndef MONAD_SPIRV_H
#define MONAD_SPIRV_H

/* Compile and validate one GLSL shader, then emit an ordinary Monad module
 * containing its typed U32 words and byte size. */
int spirv_write_monad_module(const char *source_path,
                             const char *output_path,
                             const char *binding_name,
                             char **error_out);

#endif
