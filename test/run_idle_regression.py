"""Run the connection-idle regression on a disposable database and port."""
import argparse
import pathlib
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    tests = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="toast-idle-") as directory:
        root = pathlib.Path(directory)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (root / "test.yml").write_text(
            f"host: 127.0.0.1\nport: {port}\nverbose: false\n"
            "ownership_quota: false\n64bit: true\n"
        )
        with (root / "server.log").open("w+") as log:
            server = subprocess.Popen(
                [str(args.binary.resolve()), str(tests / "Test.db"),
                 str(root / "output.db"), "-p", str(port), "-O", "--no-ipv6"],
                cwd=root, stdout=log, stderr=log,
            )
            try:
                deadline = time.monotonic() + 10
                while True:
                    if server.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=1):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.01)
                return subprocess.run(
                    ["ruby", f"-I{tests / 'tests/lib'}",
                     str(tests / "tests/test_connection_idle.rb")],
                    cwd=root, timeout=15,
                ).returncode
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == "__main__":
    raise SystemExit(main())
