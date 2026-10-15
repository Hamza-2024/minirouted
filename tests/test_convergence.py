#!/usr/bin/env python3
"""
End-to-end tests for minirouted.

Starts one daemon per config in topologies/five-routers, then checks every
router's routing table against an independent Dijkstra written here in
Python. Each scenario changes the network (crash a router, restart it, send
garbage) and measures how long the network takes to converge again.

  python3 tests/test_convergence.py            # simulated, no root needed
  unshare -n python3 tests/test_convergence.py --kernel
                                               # also verify kernel tables
"""
import argparse
import heapq
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "minirouted"
NLTOOL = ROOT / "tests" / "nltool"
TOPO = ROOT / "topologies" / "five-routers"
KERNEL_IF = "mrt0"  # falls back to "lo" if the kernel lacks dummy interfaces
INF = float("inf")


# ---------------------------------------------------------------- reference model

def load_topology(directory):
    routers = {}
    for cfg in sorted(directory.glob("r*.conf")):
        rid, port, prefixes, nbrs = None, None, [], {}
        for raw in cfg.read_text().splitlines():
            words = raw.split("#")[0].split()
            if not words:
                continue
            if words[0] == "router-id":
                rid = int(words[1])
            elif words[0] == "port":
                port = int(words[1])
            elif words[0] == "prefix":
                prefixes.append(words[1])
            elif words[0] == "neighbor":
                nbrs[int(words[1])] = int(words[3])
        routers[rid] = {"cfg": cfg, "port": port, "prefixes": prefixes, "nbrs": nbrs}
    return routers


def usable(routers, alive, u, v):
    """A link counts only if both ends are up and both configured it."""
    return u in alive and v in alive and v in routers[u]["nbrs"] and u in routers[v]["nbrs"]


def dijkstra(routers, alive, src):
    dist = {src: 0}
    heap = [(0, src)]
    while heap:
        d, u = heapq.heappop(heap)
        if d > dist[u]:
            continue
        for v, cost in routers[u]["nbrs"].items():
            if usable(routers, alive, u, v) and d + cost < dist.get(v, INF):
                dist[v] = d + cost
                heapq.heappush(heap, (dist[v], v))
    return dist


def expected_tables(routers, alive):
    """{router: {prefix: (next_hop, metric)}}, lowest-id next hop on ties."""
    dist = {r: dijkstra(routers, alive, r) for r in alive}
    tables = {}
    for src in alive:
        table = {}
        for dst, d in dist[src].items():
            if dst == src:
                continue
            first_hops = [n for n, c in routers[src]["nbrs"].items()
                          if usable(routers, alive, src, n)
                          and c + dist[n].get(dst, INF) == d]
            nh = min(first_hops)
            for p in routers[dst]["prefixes"]:
                if p in routers[src]["prefixes"]:
                    continue
                if p not in table or d < table[p][1]:
                    table[p] = (nh, d)
        tables[src] = table
    return tables


# ---------------------------------------------------------------- running daemons

class Network:
    def __init__(self, routers, kernel):
        self.routers = routers
        self.kernel = kernel
        self.tmp = Path(tempfile.mkdtemp(prefix="minirouted-"))
        self.procs = {}

    def start(self, rid):
        cmd = [str(BIN), "-c", str(self.routers[rid]["cfg"]), "-o", str(self.tmp)]
        if self.kernel:
            cmd += ["-k", KERNEL_IF]
        log = open(self.tmp / f"R{rid}.log", "a")
        self.procs[rid] = subprocess.Popen(cmd, stderr=log, stdout=log)

    def crash(self, rid):
        """SIGKILL: no goodbye, neighbors must notice via the dead interval."""
        self.procs[rid].kill()
        self.procs[rid].wait()
        del self.procs[rid]

    def stop_all(self):
        for p in self.procs.values():
            p.send_signal(signal.SIGTERM)
        for p in self.procs.values():
            p.wait(timeout=5)
        self.procs.clear()

    def alive(self):
        return set(self.procs)

    def read_table(self, rid):
        f = self.tmp / f"R{rid}.routes"
        table = {}
        if f.exists():
            for line in f.read_text().splitlines():
                prefix, nh, metric = line.split()
                table[prefix] = (int(nh[1:]), int(metric))
        return table

    def kernel_table(self, rid):
        out = subprocess.run([str(NLTOOL), "show", str(100 + rid)],
                             capture_output=True, text=True, check=True).stdout
        return set(out.split("\n")) - {""}

    def log_tail(self, rid, n=15):
        f = self.tmp / f"R{rid}.log"
        return "\n".join(f.read_text().splitlines()[-n:]) if f.exists() else "(no log)"

    def wait_converged(self, timeout=20.0):
        """Poll until every live router's table matches the reference."""
        want = expected_tables(self.routers, self.alive())
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            if all(self.read_table(r) == want[r] for r in want):
                elapsed = time.monotonic() - start
                if self.kernel:
                    self.check_kernel(want)
                return elapsed
            time.sleep(0.02)
        diffs = []
        for r in sorted(want):
            got = self.read_table(r)
            if got != want[r]:
                diffs.append(f"R{r}\n  want {want[r]}\n  got  {got}\n  log:\n{self.log_tail(r)}")
        raise AssertionError("did not converge:\n" + "\n".join(diffs))

    def check_kernel(self, want):
        for r, table in want.items():
            expect = {f"{p} via 10.255.0.{nh}" for p, (nh, _) in table.items()}
            got = self.kernel_table(r)
            assert got == expect, f"R{r} kernel table\n  want {sorted(expect)}\n  got  {sorted(got)}"


# ---------------------------------------------------------------- scenarios

def report(name, result):
    print(f"  {name:<52} {result}", flush=True)


def send_garbage(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    samples = [
        b"",                                          # empty
        b"\x01",                                      # truncated header
        os.urandom(64),                               # random noise
        b"\x09\x01\x00\x08\x00\x00\x00\x02",          # bad version
        b"\x01\x02\x00\x40\x00\x00\x00\x02",          # length field lies
        b"\x01\x01\x00\x0c\x00\x00\x00\x63\x00\x00\x00\x00",  # hello from unknown R99
        b"\x01\x02\x00\x14\x00\x00\x00\x02" + b"\x00\x00\x00\x02\x00\x00\x00\x09\xff\xff\x00\x00",
                                                      # LSA claiming 65535 links
    ]
    for _ in range(50):
        for pkt in samples:
            s.sendto(pkt, ("127.0.0.1", port))
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", action="store_true",
                    help="install routes in the kernel (run inside 'unshare -n' as root)")
    args = ap.parse_args()

    if not BIN.exists():
        sys.exit("build first: make")
    routers = load_topology(TOPO)

    if args.kernel:
        subprocess.run([str(NLTOOL), "up", "lo"], check=True)
        global KERNEL_IF
        if subprocess.run([str(NLTOOL), "dummy", KERNEL_IF], capture_output=True).returncode:
            KERNEL_IF = "lo"

    net = Network(routers, args.kernel)
    mode = f"kernel tables checked too (via {KERNEL_IF})" if args.kernel else "simulated"
    print(f"minirouted: {len(routers)} routers, {mode}, logs in {net.tmp}")
    try:
        for r in routers:
            net.start(r)
        report("cold start: all routers converge", f"{net.wait_converged():.2f}s")

        # Let R2 refresh its LSA first, so the copy it leaves behind in the
        # network has a higher sequence number than a fresh start will use.
        log2 = net.tmp / "R2.log"
        deadline = time.monotonic() + 15
        while "(refresh)" not in log2.read_text():
            assert time.monotonic() < deadline, "R2 never refreshed its LSA"
            time.sleep(0.1)
        net.crash(2)
        report("R2 crashes: traffic reroutes around it", f"{net.wait_converged():.2f}s")

        net.start(2)
        report("R2 restarts: original paths come back", f"{net.wait_converged():.2f}s")
        deadline = time.monotonic() + 3
        while "previous run" not in log2.read_text().split("up on port")[-1]:
            assert time.monotonic() < deadline, \
                "R2 should have recovered its sequence number from the network"
            time.sleep(0.05)
        net.wait_converged(timeout=2)
        report("R2 recovered its LSA sequence number", "ok")

        net.crash(4)
        report("R4 crashes: R5 reached via R3 instead", f"{net.wait_converged():.2f}s")
        net.start(4)
        net.wait_converged()

        send_garbage(routers[1]["port"])
        time.sleep(0.5)
        assert net.procs[1].poll() is None, "R1 died on malformed input"
        net.wait_converged(timeout=2)
        report("350 malformed packets: R1 keeps running, table intact", "ok")

        if args.kernel:
            net.stop_all()
            leftovers = {r: net.kernel_table(r) for r in routers if net.kernel_table(r)}
            assert not leftovers, f"routes left in kernel after shutdown: {leftovers}"
            report("clean shutdown withdraws all kernel routes", "ok")
    except AssertionError as e:
        print(f"\nFAIL: {e}")
        sys.exit(1)
    finally:
        net.stop_all()
    print("all tests passed")


if __name__ == "__main__":
    main()
