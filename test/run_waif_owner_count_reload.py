"""Check owner accounting and saved builtin-name continuations in disposable DBs."""
import argparse
import pathlib
import socket
import tempfile

from run_map_order_regressions import run_server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--reload-from", type=pathlib.Path, required=True)
    args = parser.parse_args()
    test_root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="toast-waif-owner-reload-") as temp:
        root = pathlib.Path(temp)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (root / "test.yml").write_text(f"host: 127.0.0.1\nport: {port}\nverbose: false\nownership_quota: false\n64bit: true\n")
        old = root / "old.db"
        fixed = root / "fixed.db"
        # A stored alias is not another allocation. The suspended fork exercises
        # the checkpoint's native suspend continuation and compiled builtin calls.
        fixture = str(test_root / "tests/waif_owner_count_reload_fixture.rb")
        result = run_server(args.reload_from, test_root / "Test.db", old, root, port, [fixture, '--name', 'test_prepare_old_checkpoint'])
        if result:
            return result
        result = run_server(args.binary, old, fixed, root, port, [fixture, '--name', 'test_verify_first_reload'])
        if result:
            return result
        return run_server(args.binary, fixed, root / "second.db", root, port, [fixture, '--name', 'test_verify_second_reload_and_release'])


if __name__ == "__main__":
    raise SystemExit(main())
