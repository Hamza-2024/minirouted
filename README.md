# minirouted

A small OSPF-style **link-state routing daemon in C for Linux**. Each router
discovers its neighbors, floods link-state advertisements, runs Dijkstra's
shortest-path-first algorithm, and installs the resulting routes into the
Linux kernel routing table over **rtnetlink**. Only the changes are applied,
not the whole table each time.

A Python test harness runs a 5-router network on one machine, crashes and
restarts routers, throws malformed packets at them, and checks every
router's routing table, and the kernel's, against an independent reference
implementation.

```
$ make && make test
minirouted: 5 routers, simulated
  cold start: all routers converge                     0.02s
  R2 crashes: traffic reroutes around it               3.95s
  R2 restarts: original paths come back                0.02s
  R2 recovered its LSA sequence number                 ok
  R4 crashes: R5 reached via R3 instead                4.10s
  350 malformed packets: R1 keeps running, table intact ok
all tests passed
```

Failover takes about 4 s because that is the dead interval: a crashed router
sends no goodbye, so its neighbors wait 4 missed seconds of hellos before
declaring it down. After that, the reroute is effectively instant.

## Build and run

Needs gcc, make and Python 3 on Linux. No other dependencies.

```sh
make                 # builds ./minirouted and tests/nltool, zero warnings at -Wall -Wextra -Wpedantic
make test            # simulated network on 127.0.0.1, no root needed
make test-kernel     # same, plus checks real kernel tables inside an isolated network namespace
make test-asan       # same tests built with AddressSanitizer + UBSan
```

Run one router by hand:

```sh
./minirouted -c topologies/five-routers/r1.conf -o /tmp        # routes in /tmp/R1.routes
sudo ./minirouted -c ... -k eth0                                # also install in kernel table 101
```

Config format:

```
router-id 1
port 5001                  # UDP port this router listens on (127.0.0.1)
prefix 10.0.1.0/24         # networks this router announces
neighbor 2 5002 10         # neighbor router-id, its port, link cost
```

## The topology used by the tests

Five routers, seven links (the number is the link cost):

| Link | Cost | | Link | Cost |
|---|---|---|---|---|
| R1–R2 | 10 | | R3–R4 | 20 |
| R1–R3 | 5 | | R3–R5 | 6 |
| R2–R3 | 2 | | R4–R5 | 3 |
| R2–R4 | 1 | | | |

Each router announces 10.0.N.0/24, and R4 also announces 192.168.4.0/24.

R1 reaches R4 via R3 → R2 → R4 (cost 8), not over its direct link to R2
(cost 10 + 1 = 11). That non-obvious path is exactly what SPF should find.

## How it works

The code is split by responsibility (~1,000 lines of C):

| File | What it does |
|---|---|
| `src/router.c` | Config, the `select()` event loop, HELLO handling, LSA flooding, timers |
| `src/proto.c` | Wire format: encode/decode with strict length checks |
| `src/spf.c` | Dijkstra over the link-state database |
| `src/rib.c` | Diffs wanted vs installed routes and applies only the changes |
| `src/netlink.c` | Talks to the kernel with `RTM_NEWROUTE` / `RTM_DELROUTE` |

### 1. Neighbor discovery (HELLO, 2-way check)

Every second each router sends a HELLO to its configured neighbors, listing
the neighbors it has heard from. When R1 sees its own id in R2's hello, it
knows traffic works in **both** directions and moves R2 to `2-WAY`. Only
2-way neighbors are used for routing, because a one-way link is useless.
If no hello arrives for 4 s (the dead interval), the neighbor goes `DOWN`.

### 2. Link-State Advertisements and flooding

Each router describes itself in an LSA: its 2-way links with their costs,
and the prefixes it owns. Every LSA carries a **sequence number**, and a
higher number means newer.

On receiving an LSA, a router keeps it only if it's newer than what it
has, then forwards it to every neighbor except the one it came from. Old or
duplicate copies are dropped, which is what stops flooding from looping
forever. When a new neighbor reaches 2-way, the router sends it its whole
database (a simplified version of OSPF's database exchange).

LSAs are refreshed every 10 s and expire after 40 s without a refresh, so
information from a router that vanished eventually disappears.

### 3. SPF (Dijkstra)

Each router runs Dijkstra from itself over the database. A link is used
only if **both** ends advertise it (the bidirectional check). That's how a
crashed router drops out of the topology immediately, even though its
stale LSA is still in the database: its neighbors stop listing it.

For each destination we only keep the **first hop**, since that's all a
router needs to forward a packet. On equal-cost paths, the neighbor with the
lowest router id wins, so the result is deterministic.

SPF is O(N²) with plain arrays, which is fine for 64 routers. A heap would
give O(E log N). Topology changes only set a `spf_dirty` flag, so a burst of
LSAs results in one SPF run, not one per packet.

### 4. Reconciling with the kernel (RIB → FIB)

After SPF, `rib_sync()` compares the routes it wants with the routes it
installed last time:

- new prefix → `RTM_NEWROUTE` with `NLM_F_CREATE`
- next hop or metric changed → `RTM_NEWROUTE` with `NLM_F_REPLACE` (in place, no gap)
- prefix gone → `RTM_DELROUTE`

Each request asks for an ACK, so kernel errors are logged instead of
silently lost. Routes are tagged with protocol number 99 so they can be told
apart from anyone else's (`ip route show table 101 proto 99`). On shutdown,
the daemon withdraws everything it installed.

In the simulation, router N installs into kernel table `100+N` and uses
gateway `10.255.0.M` for next-hop router M, with `onlink` set.

### 5. Restart and sequence-number recovery

This was a real bug found by the tests. If R2 crashes and restarts, it
starts counting its sequence numbers from 1 again, but its pre-crash LSA
(say seq 5) is still in everyone's database. The network would treat the
new LSAs as "older" and ignore them.

The fix is the same idea OSPF uses. When a router receives a copy of its
**own** LSA with a higher sequence number, or the same number with different
contents, it jumps its counter past it and re-advertises. The test crashes
R2 right after a refresh so this path is always exercised.

### 6. Robustness

Packets come from the network, so the decoder trusts nothing. It checks the
version, checks that the length field matches the real packet size, caps the
link and prefix counts, and rejects zero-cost links and prefix lengths over 32.
Packets from unknown routers are dropped. The test sends 350 malformed
packets (empty, truncated, random, lying length fields, an LSA claiming
65,535 links) and checks the router survives with its table unchanged.

## What the test harness checks

`tests/test_convergence.py` has its own Dijkstra in Python, written
independently from the C one. For every scenario it computes what every
router's table *should* be and polls until all five daemons match it,
timing the convergence. With `--kernel` it also reads the real kernel
tables back over netlink (using `tests/nltool`, so it doesn't depend on
`iproute2`).

## Simplifications compared with real OSPF

This is a learning project, not production OSPF. Knowing the gaps matters
as much as knowing the features:

- **Unreliable flooding.** Real OSPF acknowledges every LSA and retransmits
  until acked. Here a lost packet is fixed by the 10 s refresh.
- **No areas, no DR/BDR election.** Everything is one flat area, and links
  are point-to-point.
- **No LSA checksums or authentication.**
- **Simplified database exchange.** A full dump to the new neighbor instead
  of OSPF's Database Description / Link State Request / Update exchange.
- **No ECMP.** One next hop per prefix, lowest router id on ties.
- **Fast timers** (1 s hello, 4 s dead) to keep tests quick. OSPF defaults
  are 10 s / 40 s, and real deployments add BFD for sub-second failure detection.
- **Simulated links.** Routers talk over UDP on 127.0.0.1, not raw IP
  protocol 89 on real interfaces.

## Ideas for extending it

- LSA acknowledgements and retransmission timers
- ECMP: install multipath routes with `RTA_MULTIPATH`
- Run each router in its own network namespace joined by veth pairs, and
  forward real packets
- SPF throttling (wait briefly after a change before running SPF)
- A heap-based Dijkstra and a larger generated topology to benchmark it
