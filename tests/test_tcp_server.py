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


class TcpServerExampleTests(unittest.TestCase):
    def test_tcp_server_accepts_one_client_and_echoes_exact_bytes(self):
        source = ROOT / "how_to/TcpServer.mon"
        source_text = source.read_text()

        self.assertIn("import Network.Socket", source_text)
        self.assertNotIn("sys-socket", source_text)
        self.assertNotIn("sys-bind", source_text)
        self.assertNotIn("sys-accept", source_text)
        self.assertNotIn("asm ", source_text)
        self.assertIn("define server-port :: Int\n  39127", source_text)
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
                with connection:
                    connection.settimeout(2)
                    message = b"Monad speaks TCP.\n"
                    connection.sendall(message)
                    self.assertEqual(connection.recv(4096), message)

                stdout, _ = server.communicate(timeout=5)
                self.assertEqual(server.returncode, 0, stdout[-4000:])
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=5)
                if server.stdout is not None:
                    server.stdout.close()


if __name__ == "__main__":
    unittest.main()
