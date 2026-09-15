"""Cycle 4.1: linear dual sessions preserve protocol and bounded state."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConcurrencyChannelTests(unittest.TestCase):
    def test_dual_linear_endpoints_obey_bounded_protocols(self):
        source = r'''
#include "concurrency/effect.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 11, .task_id = id};
}

static ConcurrencySessionAction action(
    ConcurrencySessionActionKind kind, uint64_t payload) {
    return (ConcurrencySessionAction){
        .kind = kind,
        .payload_type_id = payload,
    };
}

static ConcurrencyChannelEndpoint endpoint(
    uint64_t endpoint_id, ConcurrencyTaskId owner,
    const ConcurrencySessionAction *protocol, size_t protocol_count) {
    return (ConcurrencyChannelEndpoint){
        .endpoint_id = endpoint_id,
        .owner = owner,
        .quantity = qtt_quantity_finite(1),
        .protocol = protocol,
        .protocol_count = protocol_count,
    };
}

static void expect(
    ConcurrencyChannelTrace trace, ConcurrencyChannelError expected) {
    ConcurrencyChannelVerification result =
        concurrency_channel_verify(&trace);
    assert(result.error == expected);
}

int main(void) {
    ConcurrencySessionAction client_protocol[] = {
        action(CONCURRENCY_SESSION_SEND, 42),
        action(CONCURRENCY_SESSION_RECEIVE, 9),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction server_protocol[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 42),
        action(CONCURRENCY_SESSION_SEND, 9),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencyChannelEndpoint client = endpoint(
        100, task(1), client_protocol, 3);
    ConcurrencyChannelEndpoint server = endpoint(
        101, task(2), server_protocol, 3);
    ConcurrencyChannelStep complete[] = {
        concurrency_channel_send(task(1), 100, 42),
        concurrency_channel_receive(task(2), 101, 42),
        concurrency_channel_send(task(2), 101, 9),
        concurrency_channel_receive(task(1), 100, 9),
        concurrency_channel_close(task(1), 100),
        concurrency_channel_close(task(2), 101),
    };
    ConcurrencyChannelTrace trace = {
        .channel_id = {.module_id = 11, .channel_id = 7},
        .left = client,
        .right = server,
        .capacity = 1,
        .steps = complete,
        .step_count = 6,
    };
    expect(trace, CONCURRENCY_CHANNEL_VALID);

    server_protocol[0].payload_type_id = 43;
    expect(trace, CONCURRENCY_CHANNEL_NOT_DUAL);
    server_protocol[0].payload_type_id = 42;

    client.quantity = qtt_quantity_omega();
    trace.left = client;
    expect(trace, CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT);
    client.quantity = qtt_quantity_finite(1);
    trace.left = client;

    complete[0] = concurrency_channel_send(task(2), 100, 42);
    expect(trace, CONCURRENCY_CHANNEL_WRONG_OWNER);
    complete[0] = concurrency_channel_send(task(1), 100, 42);

    complete[0] = concurrency_channel_send(task(1), 100, 99);
    expect(trace, CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH);
    complete[0] = concurrency_channel_send(task(1), 100, 42);

    ConcurrencySessionAction burst_left[] = {
        action(CONCURRENCY_SESSION_SEND, 1),
        action(CONCURRENCY_SESSION_SEND, 2),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction burst_right[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 1),
        action(CONCURRENCY_SESSION_RECEIVE, 2),
        action(CONCURRENCY_SESSION_END, 0),
    };
    trace.left = endpoint(100, task(1), burst_left, 3);
    trace.right = endpoint(101, task(2), burst_right, 3);
    ConcurrencyChannelStep overflow[] = {
        concurrency_channel_send(task(1), 100, 1),
        concurrency_channel_send(task(1), 100, 2),
    };
    trace.steps = overflow;
    trace.step_count = 2;
    expect(trace, CONCURRENCY_CHANNEL_FULL);

    trace.left = client;
    trace.right = server;
    trace.capacity = 0;
    trace.steps = complete;
    trace.step_count = 6;
    expect(trace, CONCURRENCY_CHANNEL_RENDEZVOUS_REQUIRED);
    ConcurrencyChannelStep rendezvous[] = {
        concurrency_channel_rendezvous(task(1), 100, task(2), 101, 42),
        concurrency_channel_rendezvous(task(2), 101, task(1), 100, 9),
        concurrency_channel_close(task(1), 100),
        concurrency_channel_close(task(2), 101),
    };
    trace.steps = rendezvous;
    trace.step_count = 4;
    expect(trace, CONCURRENCY_CHANNEL_VALID);

    ConcurrencyChannelStep early_close[] = {
        concurrency_channel_close(task(1), 100),
    };
    trace.capacity = 1;
    trace.steps = early_close;
    trace.step_count = 1;
    expect(trace, CONCURRENCY_CHANNEL_PROTOCOL_MISMATCH);

    ConcurrencyChannelStep cancelled[] = {
        concurrency_channel_cancel(task(1), 100),
    };
    trace.steps = cancelled;
    expect(trace, CONCURRENCY_CHANNEL_VALID);

    ConcurrencyChannelStep after_cancel[] = {
        concurrency_channel_cancel(task(1), 100),
        concurrency_channel_receive(task(2), 101, 42),
    };
    trace.steps = after_cancel;
    trace.step_count = 2;
    expect(trace, CONCURRENCY_CHANNEL_PEER_CANCELLED);

    ConcurrencyChannelTrace machine_definition = {
        .channel_id = {.module_id = 11, .channel_id = 9},
        .left = endpoint(100, task(1), burst_left, 3),
        .right = endpoint(101, task(2), burst_right, 3),
        .capacity = 1,
    };
    ConcurrencyChannelMachine *machine =
        concurrency_channel_machine_new(&machine_definition);
    assert(machine);
    ConcurrencyChannelMachineResult readiness =
        concurrency_channel_machine_step(
            machine, concurrency_channel_send(task(1), 100, 1));
    assert(readiness.enabled);
    readiness = concurrency_channel_machine_enabled(
        machine, concurrency_channel_send(task(1), 100, 2));
    assert(!readiness.enabled);
    assert(readiness.blocked == CONCURRENCY_CHANNEL_BLOCK_QUEUE_FULL);
    readiness = concurrency_channel_machine_step(
        machine, concurrency_channel_send(task(1), 100, 2));
    assert(!readiness.enabled);
    assert(concurrency_channel_machine_step_count(machine) == 1);
    readiness = concurrency_channel_machine_step(
        machine, concurrency_channel_receive(task(2), 101, 1));
    assert(readiness.enabled);
    readiness = concurrency_channel_machine_enabled(
        machine, concurrency_channel_send(task(1), 100, 2));
    assert(readiness.enabled);
    concurrency_channel_machine_free(machine);

    machine = concurrency_channel_machine_new(&machine_definition);
    assert(machine);
    readiness = concurrency_channel_machine_enabled(
        machine, concurrency_channel_receive(task(2), 101, 1));
    assert(!readiness.enabled);
    assert(readiness.blocked == CONCURRENCY_CHANNEL_BLOCK_QUEUE_EMPTY);
    readiness = concurrency_channel_machine_enabled(
        machine, concurrency_channel_send(task(1), 100, 99));
    assert(!readiness.enabled);
    assert(readiness.blocked == CONCURRENCY_CHANNEL_BLOCK_PROTOCOL);
    concurrency_channel_machine_free(machine);

    machine_definition.left = client;
    machine_definition.right = server;
    machine_definition.capacity = 0;
    machine = concurrency_channel_machine_new(&machine_definition);
    assert(machine);
    readiness = concurrency_channel_machine_enabled(
        machine, concurrency_channel_send(task(1), 100, 42));
    assert(!readiness.enabled);
    assert(readiness.blocked == CONCURRENCY_CHANNEL_BLOCK_AWAIT_PEER);
    readiness = concurrency_channel_machine_step(
        machine,
        concurrency_channel_rendezvous(task(1), 100, task(2), 101, 42));
    assert(readiness.enabled);
    concurrency_channel_machine_free(machine);

    machine = concurrency_channel_machine_new(&machine_definition);
    assert(machine);
    readiness = concurrency_channel_machine_transfer_owner(
        machine, 100, task(1), task(3), qtt_quantity_finite(1));
    assert(readiness.enabled);
    readiness = concurrency_channel_machine_enabled(
        machine,
        concurrency_channel_rendezvous(task(1), 100, task(2), 101, 42));
    assert(!readiness.enabled);
    assert(readiness.error == CONCURRENCY_CHANNEL_WRONG_OWNER);
    readiness = concurrency_channel_machine_step(
        machine,
        concurrency_channel_rendezvous(task(3), 100, task(2), 101, 42));
    assert(readiness.enabled);
    concurrency_channel_machine_free(machine);

    machine = concurrency_channel_machine_new(&machine_definition);
    assert(machine);
    readiness = concurrency_channel_machine_transfer_owner(
        machine, 100, task(9), task(3), qtt_quantity_finite(1));
    assert(!readiness.enabled);
    assert(readiness.error == CONCURRENCY_CHANNEL_WRONG_OWNER);
    readiness = concurrency_channel_machine_transfer_owner(
        machine, 100, task(1), task(3), qtt_quantity_omega());
    assert(!readiness.enabled);
    assert(readiness.error == CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT);
    assert(concurrency_channel_machine_step_count(machine) == 0);
    readiness = concurrency_channel_machine_step(
        machine,
        concurrency_channel_rendezvous(task(1), 100, task(2), 101, 42));
    assert(readiness.enabled);
    concurrency_channel_machine_free(machine);

    trace.capacity = 1;
    trace.steps = complete;
    trace.step_count = 6;
    QttEffectArena *arena = qtt_effect_arena_new();
    assert(arena);
    QttEffectRow *row = qtt_effect_empty(arena);
    for (int operation = CONCURRENCY_CHANNEL_EFFECT_CANCEL;
         operation >= CONCURRENCY_CHANNEL_EFFECT_SEND; operation--) {
        QttEffectAtom atom = concurrency_channel_effect_atom(
            (ConcurrencyChannelEffectOperation)operation, 77, 700);
        row = qtt_effect_extend_atom(arena, &atom, row);
        assert(row);
    }
    QttEffectSolver *solver = qtt_effect_solver_new(arena);
    assert(solver);
    assert(concurrency_verify_channel_effects(
               &trace, solver, row, 77, 700) ==
           CONCURRENCY_CHANNEL_EFFECTS_VALID);
    assert(concurrency_verify_channel_effects(
               &trace, solver, row, 78, 700) ==
           CONCURRENCY_CHANNEL_EFFECT_MISSING);
    QttEffectAtom send = concurrency_channel_effect_atom(
        CONCURRENCY_CHANNEL_EFFECT_SEND, 77, 700);
    QttEffectRow *send_only = qtt_effect_extend_atom(
        arena, &send, qtt_effect_empty(arena));
    assert(concurrency_verify_channel_effects(
               &trace, solver, send_only, 77, 700) ==
           CONCURRENCY_CHANNEL_EFFECT_MISSING);
    qtt_effect_solver_free(solver);
    qtt_effect_arena_free(arena);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "channel_test.c"
            binary_path = Path(directory) / "channel_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT / "src"), str(source_path),
                    str(ROOT / "src" / "concurrency" / "channel.c"),
                    str(ROOT / "src" / "concurrency" / "channel_machine.c"),
                    str(ROOT / "src" / "concurrency" / "channel_effect.c"),
                    str(ROOT / "src" / "concurrency" / "kernel.c"),
                    str(ROOT / "src" / "concurrency" / "effect.c"),
                    str(ROOT / "src" / "effects" / "effect.c"),
                    str(ROOT / "src" / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ],
                check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
