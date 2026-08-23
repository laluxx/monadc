/*
:TEST-ID tests.embedding.foreign-object.nominal-lifetime
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.06.foreign-object.nominal-lifetime
:TEST-QUALITY integration
:TEST-SUBSET foreign-object
:TEST-CATEGORY 06-nominal-lifetime
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.qtt.ownership
:TEST-PURPOSE C-defined nominal types reject confused payload access and destroy owned payloads exactly once.
:TEST-ATOM register two types -> create/retain -> checked access -> reject wrong type -> deterministic final release
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c foreign type registry, nominal identity, reference lifetime, destructor, busy removal
:TEST-USES-ATOM atom.embedding.category.06.nominal-lifetime
:TEST-MENU branches/foreign-object/atom.embedding.06.foreign-object.nominal-lifetime
:TEST-MENU-PATH embedding/branches/foreign-object/atom.embedding.06.foreign-object.nominal-lifetime
:TEST-ATOM-LEAF atom.embedding.06.foreign-object.nominal-lifetime
:TEST-CLI-TARGET embedding branches foreign-object
*/
#include <monad/embed.h>
#include <assert.h>
#include <stdio.h>

struct editor_buffer { int line_count; };
static int destroys;

static void destroy_buffer(void *payload, void *context) {
    struct editor_buffer *buffer = payload;
    int *observed_lines = context;
    *observed_lines = buffer->line_count;
    destroys++;
}

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_foreign_type_t *buffer_type;
    monad_foreign_type_t *image_type;
    monad_foreign_object_t *object;
    monad_foreign_type_descriptor_t descriptor;
    monad_error_t error;
    struct editor_buffer buffer = { 42 };
    void *payload = NULL;
    int observed_lines = 0;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);

    monad_foreign_type_descriptor_init(&descriptor);
    descriptor.name = "EditorBuffer";
    descriptor.docstring = "A host-owned editor text buffer.";
    descriptor.destroy = destroy_buffer;
    descriptor.destroy_context = &observed_lines;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &buffer_type, &error) == MONAD_OK);

    descriptor.name = "VulkanImage";
    descriptor.docstring = "A Vulkan image handle.";
    descriptor.destroy = NULL;
    descriptor.destroy_context = NULL;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &image_type, &error) == MONAD_OK);

    assert(monad_foreign_object_create(
        buffer_type, &buffer, &object, &error) == MONAD_OK);
    assert(monad_foreign_object_retain(object, &error) == MONAD_OK);
    assert(monad_foreign_object_payload(
        object, buffer_type, &payload, &error) == MONAD_OK);
    assert(payload == &buffer);
    assert(monad_foreign_object_payload(
        object, image_type, &payload, &error) == MONAD_ERROR_TYPE_MISMATCH);
    assert(monad_foreign_type_release(buffer_type, &error) == MONAD_ERROR_BUSY);
    monad_foreign_object_release(object);
    assert(destroys == 0);
    monad_foreign_object_release(object);
    assert(destroys == 1 && observed_lines == 42);

    assert(monad_foreign_type_release(buffer_type, &error) == MONAD_OK);
    assert(monad_foreign_type_release(image_type, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("nominal foreign objects ok");
    return 0;
}
