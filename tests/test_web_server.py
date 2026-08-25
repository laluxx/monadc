import os
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[1]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)
PORT = 39128
WORKERS = 4


def receive_response(parts):
    with socket.create_connection(("127.0.0.1", PORT), timeout=2) as client:
        client.settimeout(5)
        for part in parts:
            client.sendall(part)
            time.sleep(0.01)
        response = bytearray()
        while True:
            chunk = client.recv(4096)
            if not chunk:
                return bytes(response)
            response.extend(chunk)


class WebServerExampleTests(unittest.TestCase):
    def test_real_http_server_routes_fragmented_and_invalid_requests(self):
        source = ROOT / "how_to" / "WebServer.mon"
        source_text = source.read_text(encoding="utf-8")

        self.assertIn("import Web.Server", source_text)
        self.assertNotIn("import Network.Socket", source_text)
        self.assertNotIn("Text.Parser.Stream", source_text)
        self.assertNotIn("SocketBuffer", source_text)
        self.assertNotIn("while ", source_text)
        self.assertFalse(
            [line for line in source_text.splitlines() if len(line) > 100],
            "The webserver tutorial should read like an application",
        )

        with tempfile.TemporaryDirectory(prefix="monadc-web-server-") as td:
            temp = Path(td)
            output = temp / "WebServer"
            env = os.environ.copy()
            env["HOME"] = str(temp / "home")
            Path(env["HOME"]).mkdir()
            env["MONAD_CORE"] = str(ROOT / "core")
            env["MONAD_RUNTIME_LIB"] = str(RUNTIME)

            compiled = subprocess.run(
                [str(MONAD), str(source), "-o", str(output)],
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-5000:])

            server = subprocess.Popen(
                [str(output)], cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            try:
                for _ in range(50):
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", PORT), timeout=0.1
                        )
                        probe.close()
                        break
                    except OSError:
                        time.sleep(0.02)
                else:
                    self.fail(f"web server did not listen; status={server.poll()}")

                children_file = Path(f"/proc/{server.pid}/task/{server.pid}/children")
                for _ in range(50):
                    worker_pids = children_file.read_text().split()
                    if len(worker_pids) == WORKERS:
                        break
                    time.sleep(0.02)
                self.assertEqual(
                    len(worker_pids), WORKERS, "server did not create its bounded worker pool"
                )

                home = receive_response([
                    b"GET / HT", b"TP/1.1\r\nHost: localhost\r\n", b"\r\n"
                ])
                head, body = home.split(b"\r\n\r\n", 1)
                self.assertTrue(
                    head.startswith(b"HTTP/1.1 200 OK\r\n"), home
                )
                self.assertIn(b"Content-Type: text/html; charset=utf-8", head)
                self.assertIn(f"Content-Length: {len(body)}".encode(), head)
                self.assertIn(b"Connection: close", head)
                self.assertIn(b"<h1>Monad is serving HTTP</h1>", body)

                head_only = receive_response([
                    b"HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                ])
                head_headers, head_body = head_only.split(b"\r\n\r\n", 1)
                self.assertIn(f"Content-Length: {len(body)}".encode(), head_headers)
                self.assertEqual(head_body, b"")

                health = receive_response([
                    b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
                ])
                self.assertTrue(health.startswith(b"HTTP/1.1 200 OK\r\n"))
                self.assertTrue(health.endswith(b"healthy\n"))

                with socket.create_connection(("127.0.0.1", PORT), timeout=2) as slow:
                    slow.settimeout(4)
                    slow.sendall(b"GET / HTTP/1.1\r\nHost: slow")

                    started = time.monotonic()
                    concurrent = receive_response([
                        b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
                    ])
                    elapsed = time.monotonic() - started

                    self.assertTrue(
                        concurrent.startswith(b"HTTP/1.1 200 OK\r\n"), concurrent
                    )
                    self.assertLess(elapsed, 1.0, "one slow client blocked the listener")

                    timed_out = bytearray()
                    while True:
                        fragment = slow.recv(4096)
                        if not fragment:
                            break
                        timed_out.extend(fragment)
                    self.assertTrue(
                        bytes(timed_out).startswith(b"HTTP/1.1 408 Request Timeout\r\n"),
                        timed_out,
                    )

                slow_clients = []
                try:
                    for worker in range(WORKERS):
                        client = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                        client.settimeout(5)
                        client.sendall(b"GET / HTTP/1.1\r\nHost: bounded")
                        slow_clients.append(client)

                    started = time.monotonic()
                    queued = receive_response([
                        b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
                    ])
                    elapsed = time.monotonic() - started

                    self.assertTrue(queued.startswith(b"HTTP/1.1 200 OK\r\n"), queued)
                    self.assertGreater(
                        elapsed, 1.0, "worker saturation did not apply backpressure"
                    )
                    self.assertLess(elapsed, 4.0, "bounded server did not recover")
                finally:
                    for client in slow_clients:
                        client.close()

                echoed = receive_response([
                    b"POST /echo HTTP/1.1\r\nHost: localhost\r\n",
                    b"Content-Length: 11\r\n\r\nhello ", b"Monad",
                ])
                self.assertTrue(
                    echoed.startswith(b"HTTP/1.1 200 OK\r\n"), echoed
                )
                self.assertTrue(echoed.endswith(b"hello Monad"))

                missing_host = receive_response([b"GET / HTTP/1.1\r\n\r\n"])
                self.assertTrue(missing_host.startswith(b"HTTP/1.1 400 Bad Request\r\n"))

                unsupported = receive_response([
                    b"POST /echo HTTP/1.1\r\nHost: localhost\r\n",
                    b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                ])
                self.assertTrue(unsupported.startswith(b"HTTP/1.1 501 Not Implemented\r\n"))

                missing = receive_response([
                    b"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n"
                ])
                self.assertTrue(missing.startswith(b"HTTP/1.1 404 Not Found\r\n"))

                malformed = receive_response([
                    b"GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                ])
                self.assertTrue(malformed.startswith(b"HTTP/1.1 400 Bad Request\r\n"))
                self.assertIsNone(server.poll(), "one bad request stopped the server")

                fdinfo = Path(f"/proc/{server.pid}/fdinfo")
                descriptors = [p for p in fdinfo.iterdir() if int(p.name) > 2]
                self.assertLessEqual(len(descriptors), 1, "accepted sockets leaked")

                self.assertEqual(
                    len(children_file.read_text().split()),
                    WORKERS,
                    "worker pool leaked or lost a worker",
                )
            finally:
                if server.poll() is None:
                    child_pids = [
                        int(pid)
                        for pid in Path(
                            f"/proc/{server.pid}/task/{server.pid}/children"
                        ).read_text().split()
                    ]
                    server.terminate()
                    server.wait(timeout=5)
                    for _ in range(50):
                        if all(not Path(f"/proc/{pid}").exists() for pid in child_pids):
                            break
                        time.sleep(0.02)
                    self.assertTrue(
                        all(not Path(f"/proc/{pid}").exists() for pid in child_pids),
                        "workers survived server shutdown",
                    )
                if server.stdout is not None:
                    server.stdout.close()


if __name__ == "__main__":
    unittest.main()
