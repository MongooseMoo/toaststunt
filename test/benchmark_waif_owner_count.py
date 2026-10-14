"""Linux isolated before/after allocation ticks and server-process CPU control."""
import argparse
import os
import pathlib
import socket
import statistics
import subprocess
import tempfile
import time


def cpu_seconds(pid):
    # comm can contain spaces, so split after its final closing parenthesis.
    fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def measure(binary):
    test_root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="toast-waif-owner-cpu-") as temp:
        root = pathlib.Path(temp)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (root / "test.yml").write_text(f"host: 127.0.0.1\nport: {port}\nverbose: false\nownership_quota: false\n64bit: true\n")
        with (root / "server.log").open("w") as log:
            server = subprocess.Popen([str(binary.resolve()), str(test_root / "Test.db"), str(root / "output.db"), "-p", str(port), "-O", "--no-ipv6"], cwd=root, stdout=log, stderr=log)
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
                before = cpu_seconds(server.pid)
                subprocess.run(["ruby", f"-I{test_root / 'tests/lib'}", str(test_root / "tests/waif_owner_count_benchmark.rb")], cwd=root, check=True, timeout=90)
                return cpu_seconds(server.pid) - before
            finally:
                server.terminate()
                server.wait(timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("candidate", type=pathlib.Path)
    parser.add_argument("--pairs", type=int, default=6)
    args = parser.parse_args()
    if not 2 <= args.pairs <= 20:
        parser.error("--pairs must be between 2 and 20")
    baseline, candidate = [], []
    for pair in range(args.pairs):
        order = [("baseline", args.baseline, baseline), ("candidate", args.candidate, candidate)]
        if pair % 2:
            order.reverse()
        for label, binary, samples in order:
            elapsed = measure(binary)
            samples.append(elapsed)
            print(f"pair={pair + 1} runtime={label} server_cpu_seconds={elapsed:.3f}", flush=True)
    print(f"median_server_cpu baseline={statistics.median(baseline):.3f} candidate={statistics.median(candidate):.3f}")
    print("Scope: 120000 allocations/frees of one owner slot; Linux CPU counter resolution and local load limit inference. No production acceptance.")


if __name__ == "__main__":
    main()
