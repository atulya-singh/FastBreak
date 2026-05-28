#!/usr/bin/env bash
# Configure + build uxnet inside the Ubuntu 22.04 dev container.
#
# Usage:
#   docker/build.sh            # configure (Release) + build lib and tests
#   docker/build.sh bench      # also build all benchmark binaries
#   docker/build.sh test       # build, then run ctest
#
# Builds into build-linux/ so it never collides with a host build/ dir.
set -euo pipefail

IMAGE=uxnet-dev
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-}"

# uxnet targets x86_64 HFT hardware (RDTSC, x86 intrinsics). Build/run the
# container as linux/amd64 — under QEMU emulation on Apple Silicon hosts.
PLATFORM=linux/amd64

# Build the image if it is missing.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build --platform "$PLATFORM" -t "$IMAGE" \
    -f "$REPO_ROOT/docker/Dockerfile" "$REPO_ROOT"
fi

run() {
  docker run --rm --platform "$PLATFORM" \
    -v "$REPO_ROOT":/work -w /work "$IMAGE" bash -c "$1"
}

run "cmake -S . -B build-linux -G Ninja"
run "cmake --build build-linux -j"

case "$TARGET" in
  bench) run "cmake --build build-linux --target bench -j" ;;
  test)  run "ctest --test-dir build-linux --output-on-failure" ;;
  "")    ;;
  *)     run "cmake --build build-linux --target $TARGET -j" ;;
esac
