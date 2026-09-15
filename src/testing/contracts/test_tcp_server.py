import os
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from src.testing.monad_binary import resolve_monad_binary, resolve_runtime_archive


ROOT = Path(__file__).resolve().parents[3]
MONAD = resolve_monad_binary()
RUNTIME = resolve_runtime_archive(MONAD)


class TcpServerExampleTests(unittest.TestCase):
    def test_tcp_server_serves_sequential_clients_and_large_streams(self):
        source = ROOT / "how_to/TcpServer.mon"
        source_text = source.read_text()

        self.assertIn("import Network.Socket", source_text)
        self.assertNotIn("sys-socket", source_text)
        self.assertNotIn("sys-bind", source_text)
        self.assertNotIn("sys-accept", source_text)
        self.assertNotIn("asm ", source_text)
        self.assertIn("while running", source_text)
        self.assertNotIn("echo-once", source_text)
        self.assertIn("make-socket-buffer", source_text)
        self.assertIn("receive", source_text)
        self.assertIn("send-buffer", source_text)
        self.assertIn("shutdown-write", source_text)
        self.assertIn("release-socket-buffer", source_text)
        self.assertIn("define server-port :: Int\n  39127", source_text)

        socket_source = (ROOT / "core/Network/Socket.mon").read_text()
        self.assertIn("buffer-bytes", socket_source)
        self.assertFalse(
            [line for line in source_text.splitlines() if len(line) > 100],
            "The TCP tutorial should remain calm and readable",
        )

        with tempfile.TemporaryDirectory(prefix="monadc-tcp-server-") as td:
            temp = Path(td)
            output = temp / "TcpServer"
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
            self.assertEqual(compiled.returncode, 0, compiled.stdout[-4000:])

            server = subprocess.Popen(
                [str(output)], cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            try:
                connection = None
                for _ in range(50):
                    try:
                        connection = socket.create_connection(
                            ("127.0.0.1", 39127), timeout=0.1
                        )
                        break
                    except OSError:
                        time.sleep(0.02)
                if connection is None:
                    status = server.poll()
                    self.fail(
                        "TCP server did not begin listening "
                        f"(process status: {status})"
                    )
                def exchange(message):
                    with socket.create_connection(
                        ("127.0.0.1", 39127), timeout=2
                    ) as client:
                        client.settimeout(5)
                        client.sendall(message)
                        client.shutdown(socket.SHUT_WR)
                        echoed = bytearray()
                        while len(echoed) < len(message):
                            chunk = client.recv(16384)
                            if not chunk:
                                break
                            echoed.extend(chunk)
                        self.assertEqual(bytes(echoed), message)
                        self.assertEqual(client.recv(1), b"")

                def fragmented_exchange(parts):
                    with socket.create_connection(
                        ("127.0.0.1", 39127), timeout=2
                    ) as client:
                        client.settimeout(5)
                        for part in parts:
                            client.sendall(part)
                            time.sleep(0.01)
                        client.shutdown(socket.SHUT_WR)
                        expected = b"".join(parts)
                        received = bytearray()
                        while len(received) < len(expected):
                            chunk = client.recv(16384)
                            if not chunk:
                                break
                            received.extend(chunk)
                        self.assertEqual(bytes(received), expected)
                        self.assertEqual(client.recv(1), b"")

                with connection:
                    connection.settimeout(2)
                    first = b"Monad speaks TCP.\n"
                    connection.sendall(first)
                    connection.shutdown(socket.SHUT_WR)
                    try:
                        echoed = connection.recv(4096)
                    except ConnectionResetError as error:
                        self.fail(
                            f"first client was reset; server status={server.poll()}: {error}"
                        )
                    self.assertEqual(echoed, first)
                    self.assertEqual(connection.recv(1), b"")

                exchange(bytes(range(256)) * 800)
                fragmented_exchange([b"GET /", b"fragmented", b" HTTP/1.1\r\n\r\n"])

                reset = socket.create_connection(("127.0.0.1", 39127), timeout=2)
                reset.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_LINGER,
                    struct.pack("ii", 1, 0),
                )
                reset.sendall(b"disconnect while the server is writing" * 200)
                reset.close()

                exchange(b"still accepting after a large stream\n")
                self.assertIsNone(server.poll(), "server stopped accepting clients")

                fdinfo = Path(f"/proc/{server.pid}/fdinfo")
                descriptors = [path for path in fdinfo.iterdir() if int(path.name) > 2]
                self.assertLessEqual(len(descriptors), 1, "accepted sockets leaked")
                for descriptor in descriptors:
                    flags_line = next(
                        line for line in descriptor.read_text().splitlines()
                        if line.startswith("flags:")
                    )
                    flags = int(flags_line.split()[1], 8)
                    self.assertTrue(flags & os.O_CLOEXEC, "socket is inheritable")
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=5)
                if server.stdout is not None:
                    server.stdout.close()


if __name__ == "__main__":
    unittest.main()
