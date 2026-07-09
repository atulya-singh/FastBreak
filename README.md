# uxnet

A DPDK-inspired userspace networking stack in C++20 that takes the Linux kernel
off the packet hot path. Instead of `NIC → kernel → syscall → userspace`, uxnet
reads frames directly from a memory-mapped `AF_PACKET`/`PACKET_MMAP` ring, backs
every buffer with 2 MB huge pages allocated NUMA-locally, fans packets out to
CPU-pinned busy-polling workers over lock-free SPSC rings, parses
Ethernet/IP/UDP with zero-copy typed views, and layers a minimal reliable
transport (TCP-Lite) on top.

I built it to learn — and demonstrate — the techniques real HFT/quant systems
use to hit sub-microsecond, jitter-free packet processing. The whole thing
optimizes for **latency first, throughput second, clarity third**: every
hot-path function is allocation-free and branch-minimized.

## Architecture

Five layers, each independently testable:

```
 L1  Packet I/O    AF_PACKET + PACKET_MMAP ring, zero-copy RX, busy-polled
 L2  Memory        2 MB huge pages · NUMA-local · lock-free buffer pool
 L3  Concurrency   Toeplitz RSS hash → per-core SPSC rings → pinned workers
 L4  Processing    zero-copy parse · 32-packet batches · port dispatch table
 L5  Transport     TCP-Lite: reliable/ordered datagrams over UDP
```

## Key design decisions

- **No kernel on the hot path.** `PACKET_MMAP` lets the NIC DMA frames straight
  into an mmap'd ring; workers read them by pointer dereference. Steady state is
  **0 syscalls and 0 heap allocations** — verifiable with `strace -c`.
- **Busy-poll, don't block.** Workers are pinned with `pthread_setaffinity_np`
  and spin (optionally `SCHED_FIFO`). We trade a hot core for the elimination of
  interrupt/wakeup latency and scheduler jitter — the right call for trading.
- **Lock-free everywhere it counts.** The buffer pool is a Treiber stack with a
  Michael-Scott generation counter for ABA safety; inter-core handoff is a
  Lamport SPSC ring with acquire/release ordering and cache-line-isolated
  head/tail to avoid false sharing.
- **Zero-copy parsing.** `PacketParser` returns typed *views* into the ring
  memory (`EthernetView`/`IPv4View`/`UDPView`) — no packet is ever copied to be
  inspected.
- **Huge pages for TLB pressure.** 2 MB pages mean ~256× fewer TLB entries for a
  512 MB pool; NUMA-local placement (via raw `mbind`, no libnuma) keeps DMA and
  compute on the same node.
- **Hardware-faithful RSS.** The distributor uses the exact Toeplitz hash and
  40-byte key a NIC's RSS engine uses, validated against Intel datasheet
  vectors, so software fan-out matches hardware fan-out.
- **A protocol tuned for the wrong textbook.** TCP-Lite deliberately drops flow
  control, congestion control, and Nagle — all latency poison for HFT. It keeps
  only what correctness needs: CRC32 integrity, fixed-timeout retransmit, and a
  256-packet bitmap window for out-of-order tracking.

## Metrics

Measured in-container (Ubuntu 22.04, loopback). Relative results and
correctness guarantees hold on any platform; absolute NIC throughput, hardware
TLB counters, and true wire latency are produced by `final_benchmark` on bare
metal (see below).

| Metric | Result |
|---|---|
| Zero-copy vs copy-based parse | **49% lower** p50 per-packet latency (42 ns vs 83 ns) |
| Loopback RTT, p50 | raw busy-poll UDP **1.58 µs** · kernel UDP 1.79 µs · TCP-Lite 6.9 µs |
| TCP-Lite reliability | 100 MB verified intact under **1% packet loss** and full reordering |
| Syscalls in steady-state hot path | **0** (by construction) |
| Heap allocations in hot path | **0** (pre-allocated pools) |
| Test suite | 17/17 passing |

TCP-Lite's extra ~5 µs is its CRC32 + ARQ machinery — the honest cost of
reliability over raw UDP.

**Run the full suite** (throughput, hardware TLB-miss reduction, ring→app
latency, syscall proof) on real x86-64 Linux:

```sh
echo 512 | sudo tee /proc/sys/vm/nr_hugepages       # reserve huge pages
sudo sysctl -w kernel.perf_event_paranoid=1         # allow perf counters
sudo ./build-linux/benchmarks/final_benchmark lo    # writes RESULTS.md
```

## Building

Requires CMake ≥ 3.16, a C++20 compiler (GCC 11+/Clang 14+), and Linux. The
target is x86-64 (RDTSC, cache intrinsics). GoogleTest is fetched at configure
time if not installed.

```sh
cmake -S . -B build -G Ninja
cmake --build build -j                    # library + tests
cmake --build build --target bench -j     # benchmark binaries
ctest --test-dir build --output-on-failure
```

On an Apple Silicon host, `docker/build.sh [bench|test]` builds and runs inside
an `amd64` container. Note that AF_PACKET, the PMU, and huge pages are
unavailable under emulation — run those benchmarks on a real x86-64 box.

## Layout

| Directory | Contents |
|---|---|
| `include/` | Public headers |
| `src/` | Library implementation (`uxnet_lib`) |
| `tests/` | GoogleTest unit tests (`uxnet_tests`) |
| `benchmarks/` | Latency/throughput benchmarks, incl. `final_benchmark` |
| `tools/` | Diagnostics: `perf_baseline`, `numa_info`, `pkt_gen` |
| `docs/` | Architecture notes |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full five-layer design.
