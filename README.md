# uxnet

**uxnet** is a DPDK-inspired userspace networking stack written in C++20 that
takes the Linux kernel off the packet-processing hot path. Instead of the usual
`NIC → kernel → userspace` path, uxnet reads packets directly in userspace from
a memory-mapped ring buffer (`AF_PACKET` + `PACKET_MMAP`), backs every buffer
with 2 MB huge pages and NUMA-local pre-allocation, routes packets to
CPU-pinned busy-polling worker threads over lock-free SPSC rings, parses
Ethernet/IP/UDP with zero-copy typed views, and layers a minimal reliable
transport (TCP-Lite) on top. It is built for the lowest possible latency
first, throughput second, and clarity third — every hot-path function is
allocation-free and branch-minimized.

## Building

Requires CMake ≥ 3.16, a C++20 compiler (GCC 11+/Clang 14+), and Linux
(Ubuntu 22.04 target). GoogleTest is fetched automatically at configure time
if it is not already installed.

```sh
cmake -S . -B build            # defaults to a Release build
cmake --build build -j         # build the library + tests
cmake --build build --target bench   # build all benchmark binaries
ctest --test-dir build         # run the unit tests
```

## Layout

| Directory     | Contents                                             |
|---------------|------------------------------------------------------|
| `include/`    | Public headers                                       |
| `src/`        | Library implementation (compiled into `uxnet_lib`)   |
| `tests/`      | GoogleTest unit tests (`uxnet_tests`)                |
| `benchmarks/` | Standalone latency/throughput benchmark binaries     |
| `tools/`      | Diagnostic utilities                                 |
| `docs/`       | Architecture notes                                   |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the five-layer design.
