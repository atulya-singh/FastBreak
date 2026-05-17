# uxnet Architecture

uxnet is a userspace networking stack built to remove the Linux kernel from the
packet-processing hot path. This document is a placeholder outline of the five
layers; each section will be expanded as the corresponding layer is implemented.

**Design priority (never reordered): latency → throughput → code clarity.**
Every hot-path function is allocation-free and branch-minimized.

## Layer 1 — Packet I/O

Memory-mapped ring buffers via `AF_PACKET` + `PACKET_MMAP`. The NIC (through the
kernel driver) writes frames into a ring shared with userspace; uxnet reads them
by pointer dereference with no per-packet syscall and no copy. `SO_BUSY_POLL`
replaces interrupt-driven wakeups with busy spinning on the ring.

> Framing note: `PACKET_MMAP` is zero-copy *from userspace's perspective*, but
> the kernel NIC driver still runs in the ingest path. This is not a full kernel
> bypass like AF_XDP/DPDK — the honest claim is "zero-copy userspace RX with no
> per-packet syscall."

## Layer 2 — Memory

All packet buffers live in 2 MB huge pages to minimize TLB misses. A fixed-size
buffer pool pre-allocates everything at startup, so there is zero `malloc`/`free`
in the hot path. Allocation is NUMA-aware (implemented via raw syscalls, no
libnuma) so buffers sit on the same memory controller as the core that processes
them.

## Layer 3 — Concurrency

Single-producer/single-consumer (SPSC) lock-free ring buffers connect the I/O
thread to worker threads — one ring per worker, zero sharing. An RSS-style hash
over the 4-tuple `(src_ip, dst_ip, src_port, dst_port)` deterministically routes
each packet to a single queue. Each worker thread is pinned to a dedicated CPU
core (`pthread_setaffinity_np`) and busy-polls its own ring, keeping the OS
scheduler out of the path.

## Layer 4 — Packet Processing

Zero-copy parsing: lightweight view structs are just typed pointers into the
original buffer, so parsing Ethernet/IP/UDP is reading bytes at fixed offsets
with no deserialization. Packets are processed in batches of 32 to amortize loop
overhead and help the hardware prefetcher. Protocol dispatch uses a function-
pointer table indexed by protocol type — O(1), branch-free.

## Layer 5 — TCP-Lite Reliable Protocol

A minimal reliable transport on top of the UDP stack: sequence numbers on every
packet, cumulative ACKs, fixed-timeout retransmit (deliberately no exponential
backoff — the wrong axis for low latency), and out-of-order buffering via a
sequence-number bitmap. Reliability without kernel TCP overhead.

## Benchmarking

Every layer ships a standalone benchmark. Latency is measured with `RDTSC`
directly (no `std::chrono`, no syscalls); throughput is reported in Mpps; TLB and
cache behavior is measured with `perf_event_open` hardware counters; latency is
summarized as p50/p99/p99.9/p99.99 histograms; and `strace -c` is used to prove
the hot path issues zero syscalls.
