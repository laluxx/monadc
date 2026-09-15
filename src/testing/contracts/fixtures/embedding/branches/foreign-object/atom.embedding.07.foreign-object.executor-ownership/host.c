/*
:TEST-ID tests.embedding.foreign-object.executor-ownership
:TEST-TIER regression
:TEST-STATUS active-2026-08-14
:TEST-EMBEDDING-ATOM atom.embedding.07.foreign-object.executor-ownership
:TEST-QUALITY integration
:TEST-SUBSET foreign-object
:TEST-CATEGORY 07-executor-ownership
:TEST-SECTION embedding
:TEST-CONTEXT monadc.context.embedding.public-abi,monadc.context.qtt.ownership
:TEST-PURPOSE Unique foreign payloads cannot be retained and destruction remains pinned until a host executor runs it.
:TEST-ATOM unique object -> reject retain -> final release enqueues -> type remains busy -> host drain destroys
:TEST-EXPECT compile, run
:TEST-COVERAGE embed/embed.c unique ownership policy, deferred destruction executor, lifetime pinning
:TEST-USES-ATOM atom.embedding.category.07.executor-ownership
:TEST-MENU branches/foreign-object/atom.embedding.07.foreign-object.executor-ownership
:TEST-MENU-PATH embedding/branches/foreign-object/atom.embedding.07.foreign-object.executor-ownership
:TEST-ATOM-LEAF atom.embedding.07.foreign-object.executor-ownership
:TEST-CLI-TARGET embedding branches foreign-object
*/
#include <monad/embed.h>
#include <assert.h>
#include <stdio.h>

struct one_job {
    monad_foreign_job_fn run;
    void *context;
};

static int destroyed;

static void enqueue(void *context, monad_foreign_job_fn run,
                    void *job_context) {
    struct one_job *queue = context;
    assert(queue->run == NULL);
    queue->run = run;
    queue->context = job_context;
}

static void destroy_resource(void *payload, void *context) {
    int *resource = payload;
    int *result = context;
    *result = *resource;
    destroyed++;
}

int main(void) {
    monad_runtime_config_t config;
    monad_runtime_t *runtime;
    monad_thread_t *thread;
    monad_foreign_type_descriptor_t descriptor;
    monad_foreign_type_t *type;
    monad_foreign_object_t *object;
    monad_error_t error;
    struct one_job queue = { 0 };
    int resource = 77;
    int observed = 0;

    monad_runtime_config_init(&config);
    assert(monad_runtime_create(&config, &runtime, &error) == MONAD_OK);
    assert(monad_thread_attach(runtime, &thread, &error) == MONAD_OK);
    monad_foreign_type_descriptor_init(&descriptor);
    descriptor.name = "RenderResource";
    descriptor.ownership = MONAD_FOREIGN_UNIQUE;
    descriptor.destroy = destroy_resource;
    descriptor.destroy_context = &observed;
    descriptor.executor.context = &queue;
    descriptor.executor.schedule = enqueue;
    assert(monad_runtime_register_foreign_type(
        thread, &descriptor, &type, &error) == MONAD_OK);
    assert(monad_foreign_object_create(
        type, &resource, &object, &error) == MONAD_OK);
    assert(monad_foreign_object_retain(object, &error) ==
           MONAD_ERROR_OWNERSHIP_VIOLATION);

    monad_foreign_object_release(object);
    assert(destroyed == 0 && queue.run != NULL);
    assert(monad_foreign_type_release(type, &error) == MONAD_ERROR_BUSY);
    queue.run(queue.context);
    assert(destroyed == 1 && observed == 77);
    assert(monad_foreign_type_release(type, &error) == MONAD_OK);
    assert(monad_thread_detach(thread, &error) == MONAD_OK);
    assert(monad_runtime_destroy(runtime, &error) == MONAD_OK);
    puts("foreign executor ownership ok");
    return 0;
}
