#include "channel.h"

#include <stdint.h>
#include <stdlib.h>

struct ConcurrencyChannelMachine {
    ConcurrencyChannelTrace definition;
    ConcurrencyChannelStep *steps;
    size_t step_count;
    size_t capacity;
    ConcurrencyChannelOwnershipTransfer *ownership_transfers;
    size_t ownership_transfer_count;
};

static ConcurrencyChannelMachineResult machine_result(
    bool enabled, ConcurrencyChannelBlockReason blocked,
    ConcurrencyChannelError error) {
    return (ConcurrencyChannelMachineResult){
        .enabled = enabled,
        .blocked = blocked,
        .error = error,
    };
}

static ConcurrencyChannelBlockReason block_reason(
    ConcurrencyChannelError error) {
    switch (error) {
    case CONCURRENCY_CHANNEL_FULL:
        return CONCURRENCY_CHANNEL_BLOCK_QUEUE_FULL;
    case CONCURRENCY_CHANNEL_EMPTY:
        return CONCURRENCY_CHANNEL_BLOCK_QUEUE_EMPTY;
    case CONCURRENCY_CHANNEL_RENDEZVOUS_REQUIRED:
        return CONCURRENCY_CHANNEL_BLOCK_AWAIT_PEER;
    case CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH:
    case CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH:
        return CONCURRENCY_CHANNEL_BLOCK_PROTOCOL;
    case CONCURRENCY_CHANNEL_CLOSED:
    case CONCURRENCY_CHANNEL_PEER_CANCELLED:
        return CONCURRENCY_CHANNEL_BLOCK_TERMINAL;
    case CONCURRENCY_CHANNEL_OUT_OF_MEMORY:
        return CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY;
    default:
        return CONCURRENCY_CHANNEL_BLOCK_INVALID;
    }
}

ConcurrencyChannelMachine *concurrency_channel_machine_new(
    const ConcurrencyChannelTrace *definition) {
    if (!definition || definition->steps || definition->step_count ||
        definition->ownership_transfers || definition->ownership_transfer_count)
        return NULL;
    ConcurrencyChannelVerification verified =
        concurrency_channel_verify(definition);
    if (verified.error != CONCURRENCY_CHANNEL_INCOMPLETE &&
        verified.error != CONCURRENCY_CHANNEL_VALID)
        return NULL;
    ConcurrencyChannelMachine *machine = calloc(1, sizeof(*machine));
    if (!machine) return NULL;
    machine->definition = *definition;
    return machine;
}

ConcurrencyChannelMachineResult concurrency_channel_machine_enabled(
    const ConcurrencyChannelMachine *machine, ConcurrencyChannelStep candidate) {
    if (!machine || machine->step_count >=
            SIZE_MAX / sizeof(ConcurrencyChannelStep))
        return machine_result(
            false, CONCURRENCY_CHANNEL_BLOCK_INVALID,
            CONCURRENCY_CHANNEL_MALFORMED);
    size_t count = machine->step_count + 1;
    ConcurrencyChannelStep *steps = malloc(count * sizeof(*steps));
    if (!steps)
        return machine_result(
            false, CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY,
            CONCURRENCY_CHANNEL_OUT_OF_MEMORY);
    for (size_t i = 0; i < machine->step_count; i++)
        steps[i] = machine->steps[i];
    steps[machine->step_count] = candidate;
    ConcurrencyChannelTrace trace = machine->definition;
    trace.steps = steps;
    trace.step_count = count;
    trace.ownership_transfers = machine->ownership_transfers;
    trace.ownership_transfer_count = machine->ownership_transfer_count;
    ConcurrencyChannelError error = concurrency_channel_verify(&trace).error;
    free(steps);
    bool enabled = error == CONCURRENCY_CHANNEL_VALID ||
                   error == CONCURRENCY_CHANNEL_INCOMPLETE;
    return machine_result(
        enabled,
        enabled ? CONCURRENCY_CHANNEL_BLOCK_NONE : block_reason(error),
        error);
}

ConcurrencyChannelMachineResult concurrency_channel_machine_transfer_owner(
    ConcurrencyChannelMachine *machine, uint64_t endpoint_id,
    ConcurrencyTaskId from, ConcurrencyTaskId to, QttQuantity quantity) {
    if (!machine || machine->ownership_transfer_count >=
            SIZE_MAX / sizeof(ConcurrencyChannelOwnershipTransfer))
        return machine_result(false, CONCURRENCY_CHANNEL_BLOCK_INVALID,
                              CONCURRENCY_CHANNEL_MALFORMED);
    ConcurrencyChannelOwnershipTransfer candidate = {
        .step_index = machine->step_count,
        .endpoint_id = endpoint_id,
        .from = from,
        .to = to,
        .quantity = quantity,
    };
    size_t count = machine->ownership_transfer_count + 1;
    ConcurrencyChannelOwnershipTransfer *transfers =
        malloc(count * sizeof(*transfers));
    if (!transfers)
        return machine_result(false, CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY,
                              CONCURRENCY_CHANNEL_OUT_OF_MEMORY);
    for (size_t i = 0; i < machine->ownership_transfer_count; i++)
        transfers[i] = machine->ownership_transfers[i];
    transfers[count - 1] = candidate;
    ConcurrencyChannelTrace trace = machine->definition;
    trace.steps = machine->steps;
    trace.step_count = machine->step_count;
    trace.ownership_transfers = transfers;
    trace.ownership_transfer_count = count;
    ConcurrencyChannelError error = concurrency_channel_verify(&trace).error;
    bool enabled = error == CONCURRENCY_CHANNEL_VALID ||
                   error == CONCURRENCY_CHANNEL_INCOMPLETE;
    if (!enabled) {
        free(transfers);
        return machine_result(false, block_reason(error), error);
    }
    free(machine->ownership_transfers);
    machine->ownership_transfers = transfers;
    machine->ownership_transfer_count = count;
    return machine_result(true, CONCURRENCY_CHANNEL_BLOCK_NONE, error);
}

ConcurrencyChannelMachineResult concurrency_channel_machine_step(
    ConcurrencyChannelMachine *machine, ConcurrencyChannelStep candidate) {
    ConcurrencyChannelMachineResult result =
        concurrency_channel_machine_enabled(machine, candidate);
    if (!result.enabled) return result;
    if (machine->step_count == machine->capacity) {
        size_t capacity = machine->capacity ? machine->capacity * 2 : 8;
        if (capacity < machine->capacity ||
            capacity > SIZE_MAX / sizeof(*machine->steps))
            return machine_result(
                false, CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY,
                CONCURRENCY_CHANNEL_OUT_OF_MEMORY);
        ConcurrencyChannelStep *grown = realloc(
            machine->steps, capacity * sizeof(*grown));
        if (!grown)
            return machine_result(
                false, CONCURRENCY_CHANNEL_BLOCK_OUT_OF_MEMORY,
                CONCURRENCY_CHANNEL_OUT_OF_MEMORY);
        machine->steps = grown;
        machine->capacity = capacity;
    }
    machine->steps[machine->step_count++] = candidate;
    return result;
}

size_t concurrency_channel_machine_step_count(
    const ConcurrencyChannelMachine *machine) {
    return machine ? machine->step_count : 0;
}

bool concurrency_channel_machine_complete(
    const ConcurrencyChannelMachine *machine) {
    if (!machine) return false;
    ConcurrencyChannelTrace trace = machine->definition;
    trace.steps = machine->steps;
    trace.step_count = machine->step_count;
    trace.ownership_transfers = machine->ownership_transfers;
    trace.ownership_transfer_count = machine->ownership_transfer_count;
    return concurrency_channel_verify(&trace).error == CONCURRENCY_CHANNEL_VALID;
}

void concurrency_channel_machine_free(ConcurrencyChannelMachine *machine) {
    if (!machine) return;
    free(machine->steps);
    free(machine->ownership_transfers);
    free(machine);
}
