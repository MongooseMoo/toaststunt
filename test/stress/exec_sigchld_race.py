#!/usr/bin/env python3
"""Stress exec() and look for children whose exit the server never notices.

Usage: exec_sigchld_race.py MOO_BINARY TEST_DIR PORT [CONNECTIONS [EXECS [HOGS]]]

Starts MOO_BINARY on TEST_DIR/Test.db (TEST_DIR/executables/true must exist),
opens CONNECTIONS wizard connections (default 8), and has each run EXECS
exec({"true"}) calls in one task (default 1500) while HOGS busy processes
compete for the CPUs (default twice the CPU count).

The fault this looks for: the SIGCHLD handler ran on a worker thread while the
main thread was still starting the child, did not find the child in exec's
process table, and so never completed the waiting task. Its signs are a loop
that never finishes, two pipes left open in the server for each such task,
and a server that uses a whole CPU while idle because it polls those pipes at
end-of-file for ever. It does not show without CPU load.

Prints the three measurements and exits 0 when none shows the fault, 1 otherwise.
"""
import multiprocessing
import os
import signal
import socket
import subprocess
import sys
import time


def hog():
    while True:
        pass


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as handle:
        fields = handle.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def open_pipes(pid):
    count = 0
    for name in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{name}")
        except OSError:
            continue
        if target.startswith("pipe:"):
            count += 1
    return count


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    binary, test_dir, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    connections = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    execs = int(sys.argv[5]) if len(sys.argv) > 5 else 1500
    hogs = int(sys.argv[6]) if len(sys.argv) > 6 else 2 * multiprocessing.cpu_count()

    server = subprocess.Popen(
        [binary, "Test.db", "/dev/null", str(port)],
        cwd=test_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(2.0)
    baseline_pipes = open_pipes(server.pid)

    hog_processes = [multiprocessing.Process(target=hog, daemon=True) for _ in range(hogs)]
    for process in hog_processes:
        process.start()

    sockets = []
    for _ in range(connections):
        sock = socket.create_connection(("127.0.0.1", port))
        sock.sendall(b"connect wizard\r\n")
        sockets.append(sock)
    time.sleep(1.0)
    loop = '; for i in [1..%d] exec({"true"}); endfor notify(player, "LOOP-DONE"); return 0;\r\n' % execs
    for sock in sockets:
        sock.setblocking(False)
        try:
            sock.recv(65536)
        except BlockingIOError:
            pass
        sock.sendall(loop.encode())

    started = time.time()
    limit = 30 + connections * execs / 50.0
    buffers = [b""] * connections
    done = [False] * connections
    while not all(done) and time.time() - started < limit:
        for index, sock in enumerate(sockets):
            if done[index]:
                continue
            try:
                chunk = sock.recv(65536)
            except BlockingIOError:
                continue
            buffers[index] += chunk
            if b"LOOP-DONE" in buffers[index]:
                done[index] = True
        time.sleep(0.05)
    elapsed = time.time() - started

    for process in hog_processes:
        process.terminate()
    time.sleep(2.0)

    leaked = open_pipes(server.pid) - baseline_pipes
    before = cpu_ticks(server.pid)
    time.sleep(3.0)
    idle_cpu = (cpu_ticks(server.pid) - before) / 3.0
    finished = sum(done)

    print(f"loops finished    {finished} of {connections} in {elapsed:.1f} s (limit {limit:.0f} s)")
    print(f"pipes left open   {leaked}")
    print(f"idle CPU          {idle_cpu:.0f}% of one core")

    server.send_signal(signal.SIGKILL)
    server.wait()
    faulty = finished != connections or leaked != 0 or idle_cpu > 20
    print("RESULT            " + ("FAULT SEEN" if faulty else "clean"))
    return 1 if faulty else 0


if __name__ == "__main__":
    sys.exit(main())
