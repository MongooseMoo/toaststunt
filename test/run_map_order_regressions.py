"""Run map regressions against a disposable Test.db, never the live MOO."""
import argparse
import pathlib
import socket
import subprocess
import tempfile
import time


def run_server(binary, database, output, root, port, ruby_arguments):
    test_root = pathlib.Path(__file__).resolve().parent
    with (root / "server.log").open("w") as log:
        server = subprocess.Popen([str(binary.resolve()), str(database), str(output), "-p", str(port), "-O", "--no-ipv6"], cwd=root, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                if server.poll() is not None:
                    raise RuntimeError((root / "server.log").read_text())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=1):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.05)
            result = subprocess.run(["ruby", f"-I{test_root / 'tests/lib'}", *ruby_arguments], cwd=root, timeout=90)
        finally:
            server.terminate()
            server.wait(timeout=30)
    return result.returncode


def ruby_case(code, expected):
    return ["-e", "require 'test_helper'; class TestMapReload < Test::Unit::TestCase; def test_reload; run_test_as('wizard') { assert_equal " + expected + ", simplify(command('" + code + "')) }; end; end"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--name")
    parser.add_argument("--test", default="test_map.rb")
    parser.add_argument("--reload-from", type=pathlib.Path)
    args = parser.parse_args()
    test_root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="mongoose-map-order-") as temp:
        root = pathlib.Path(temp)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (root / "test.yml").write_text(f"host: 127.0.0.1\nport: {port}\nverbose: false\nownership_quota: false\n64bit: true\n")
        database = test_root / "Test.db"
        if args.reload_from:
            prepare = '; a = $waif:new(); b = $waif:new(); x = create({}, 1); y = create({}, 1); f = ["numeric" -> [0 -> 10, 1.0 -> 20, 1 -> 30], "bool" -> [false -> 11, true -> 22, 0 -> 33], "waifs" -> {a, b}, "waifmap" -> [a -> 11, b -> 22, 0 -> 33], "anons" -> {x, y}, "anonmap" -> [x -> 11, y -> 22, 0 -> 33]]; add_property(#0, "map_order_fixture", f, {player, ""}); return {length(f["numeric"]), maphaskey(f["numeric"], 1.0), maphaskey(f["bool"], false), maphaskey(f["waifmap"], a)};'
            checkpoint = root / "old-checkpoint.db"
            result = run_server(args.reload_from, database, checkpoint, root, port, ruby_case(prepare, '[3, 0, 0, 0]'))
            if result:
                return result
            verify = '; f = #0.map_order_fixture; a = f["waifs"][1]; b = f["waifs"][2]; x = f["anons"][1]; y = f["anons"][2]; return {length(f["numeric"]), f["numeric"][0], f["numeric"][1], f["numeric"][1.0], f["bool"][false], f["bool"][true], f["waifmap"][a], f["waifmap"][b], f["anonmap"][x], f["anonmap"][y], 1 == 1.0, 1 + 1.0};'
            result = run_server(args.binary, checkpoint, root / "fixed-checkpoint.db", root, port, ruby_case(verify, '[3, 10, 30, 20, 11, 22, 11, 22, 11, 22, 1, 2.0]'))
            if result:
                return result
            # A second reload checks that the repaired ordering also persists.
            return run_server(args.binary, root / "fixed-checkpoint.db", root / "second-checkpoint.db", root, port, ruby_case(verify, '[3, 10, 30, 20, 11, 22, 11, 22, 11, 22, 1, 2.0]'))
        command = [str(test_root / "tests" / args.test)]
        if args.name:
            command += ["--name", args.name]
        return run_server(args.binary, database, root / "checkpoint.db", root, port, command)


if __name__ == "__main__":
    raise SystemExit(main())
