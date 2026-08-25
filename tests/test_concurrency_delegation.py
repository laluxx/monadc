"""Cycle 4.2: in-flight endpoint delegation conserves linear authority."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ConcurrencyDelegationTests(unittest.TestCase):
    def test_endpoint_authority_moves_through_messages_exactly_once(self):
        source = r'''
#include "concurrency/channel.h"

#include <assert.h>

static ConcurrencyTaskId task(uint64_t id) {
    return (ConcurrencyTaskId){.module_id = 12, .task_id = id};
}

static ConcurrencySessionAction action(
    ConcurrencySessionActionKind kind, uint64_t payload) {
    return (ConcurrencySessionAction){
        .kind = kind,
        .payload_type_id = payload,
    };
}

static ConcurrencyChannelEndpoint carrier(
    uint64_t id, ConcurrencyTaskId owner,
    const ConcurrencySessionAction *protocol, size_t count) {
    return (ConcurrencyChannelEndpoint){
        .endpoint_id = id,
        .owner = owner,
        .quantity = qtt_quantity_finite(1),
        .protocol = protocol,
        .protocol_count = count,
    };
}

static ConcurrencyEndpointAuthority authority(
    uint64_t endpoint_id, uint64_t protocol_type_id,
    ConcurrencyTaskId owner) {
    return (ConcurrencyEndpointAuthority){
        .endpoint_id = endpoint_id,
        .protocol_type_id = protocol_type_id,
        .place = qtt_place_root((QttCoreVar){
            .module_id = 12,
            .binder_id = endpoint_id,
        }),
        .owner = owner,
        .quantity = qtt_quantity_finite(1),
    };
}

static ConcurrencyChannelVerification verify(ConcurrencyChannelTrace trace) {
    return concurrency_channel_verify(&trace);
}

int main(void) {
    ConcurrencySessionAction sender_protocol[] = {
        action(CONCURRENCY_SESSION_SEND, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction receiver_protocol[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencyEndpointAuthority delegated[] = {
        authority(900, 500, task(1)),
    };
    ConcurrencyChannelStep transfer[] = {
        concurrency_channel_send_endpoint(task(1), 100, 900),
        concurrency_channel_receive_endpoint(task(2), 101, 900),
        concurrency_channel_close(task(1), 100),
        concurrency_channel_close(task(2), 101),
    };
    ConcurrencyChannelTrace trace = {
        .channel_id = {.module_id = 12, .channel_id = 8},
        .left = carrier(100, task(1), sender_protocol, 2),
        .right = carrier(101, task(2), receiver_protocol, 2),
        .capacity = 1,
        .endpoint_authorities = delegated,
        .endpoint_authority_count = 1,
        .steps = transfer,
        .step_count = 4,
    };
    ConcurrencyChannelVerification result = verify(trace);
    assert(result.error == CONCURRENCY_CHANNEL_VALID);
    assert(result.delegated_transfer_count == 1);
    assert(result.delegated_cancel_count == 0);
    assert(result.delegated_task_owned_count == 1);
    assert(result.delegated_in_flight_count == 0);

    delegated[0].protocol_type_id = 501;
    assert(verify(trace).error == CONCURRENCY_CHANNEL_DELEGATED_TYPE_MISMATCH);
    delegated[0].protocol_type_id = 500;

    delegated[0].quantity = qtt_quantity_omega();
    assert(verify(trace).error == CONCURRENCY_CHANNEL_NON_LINEAR_ENDPOINT);
    delegated[0].quantity = qtt_quantity_finite(1);

    ConcurrencyEndpointAuthority duplicate[] = {
        authority(900, 500, task(1)),
        authority(900, 500, task(1)),
    };
    trace.endpoint_authorities = duplicate;
    trace.endpoint_authority_count = 2;
    assert(verify(trace).error == CONCURRENCY_CHANNEL_DUPLICATE_AUTHORITY);
    duplicate[1].endpoint_id = 901;
    assert(verify(trace).error == CONCURRENCY_CHANNEL_DUPLICATE_AUTHORITY);
    trace.endpoint_authorities = delegated;
    trace.endpoint_authority_count = 1;

    ConcurrencySessionAction twice_sender[] = {
        action(CONCURRENCY_SESSION_SEND, 500),
        action(CONCURRENCY_SESSION_SEND, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    ConcurrencySessionAction twice_receiver[] = {
        action(CONCURRENCY_SESSION_RECEIVE, 500),
        action(CONCURRENCY_SESSION_RECEIVE, 500),
        action(CONCURRENCY_SESSION_END, 0),
    };
    trace.left = carrier(100, task(1), twice_sender, 3);
    trace.right = carrier(101, task(2), twice_receiver, 3);
    ConcurrencyChannelStep reused[] = {
        concurrency_channel_send_endpoint(task(1), 100, 900),
        concurrency_channel_send_endpoint(task(1), 100, 900),
    };
    trace.steps = reused;
    trace.step_count = 2;
    assert(verify(trace).error == CONCURRENCY_CHANNEL_DELEGATED_NOT_OWNED);

    trace.left = carrier(100, task(1), sender_protocol, 2);
    trace.right = carrier(101, task(2), receiver_protocol, 2);
    ConcurrencyChannelStep cancelled[] = {
        concurrency_channel_send_endpoint(task(1), 100, 900),
        concurrency_channel_cancel(task(1), 100),
    };
    trace.steps = cancelled;
    trace.step_count = 2;
    result = verify(trace);
    assert(result.error == CONCURRENCY_CHANNEL_VALID);
    assert(result.delegated_transfer_count == 0);
    assert(result.delegated_cancel_count == 1);
    assert(result.delegated_task_owned_count == 0);
    assert(result.delegated_in_flight_count == 0);

    trace.capacity = 0;
    ConcurrencyChannelStep rendezvous[] = {
        concurrency_channel_rendezvous_endpoint(
            task(1), 100, task(2), 101, 900),
        concurrency_channel_close(task(1), 100),
        concurrency_channel_close(task(2), 101),
    };
    trace.steps = rendezvous;
    trace.step_count = 3;
    result = verify(trace);
    assert(result.error == CONCURRENCY_CHANNEL_VALID);
    assert(result.delegated_transfer_count == 1);
    assert(result.delegated_task_owned_count == 1);
    assert(result.delegated_in_flight_count == 0);

    rendezvous[0] = concurrency_channel_rendezvous_endpoint(
        task(2), 100, task(2), 101, 900);
    assert(verify(trace).error == CONCURRENCY_CHANNEL_WRONG_OWNER);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "delegation_test.c"
            binary_path = Path(directory) / "delegation_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-iquote", str(ROOT), str(source_path),
                    str(ROOT / "concurrency" / "channel.c"),
                    str(ROOT / "qtt" / "quantity.c"),
                    "-o", str(binary_path),
                ],
                check=True,
            )
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0"
            subprocess.run([str(binary_path)], env=environment, check=True)


if __name__ == "__main__":
    unittest.main()
