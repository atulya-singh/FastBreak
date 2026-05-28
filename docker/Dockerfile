# Ubuntu 22.04 build/test environment for uxnet.
#
# uxnet targets Linux (AF_PACKET / PACKET_MMAP, perf_event_open, NUMA syscalls),
# so it cannot build on the macOS host. This image provides the C++20 toolchain
# used to compile, test, and run the stack.
#
# uxnet targets x86_64 (RDTSC, x86 intrinsics), so build/run this image as
# linux/amd64 — emulated via QEMU on Apple Silicon hosts.
#
# Build:  docker build --platform linux/amd64 -t uxnet-dev -f docker/Dockerfile .
# Shell:  docker run --rm -it --platform linux/amd64 -v "$PWD":/work -w /work uxnet-dev
#
# Live packet capture additionally needs raw-socket capability and host
# networking; run those with:
#   docker run --rm -it --network host --cap-add NET_RAW --cap-add NET_ADMIN \
#     -v "$PWD":/work -w /work uxnet-dev
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        pkg-config \
        linux-tools-generic \
        strace \
        gdb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

CMD ["/bin/bash"]
