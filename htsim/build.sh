#!/usr/bin/env bash
# Called by the repository build.sh; may also use an existing libfabric install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${WORKDIR:=$PWD/suet-workspace}"
case "$WORKDIR" in /*) ;; *) WORKDIR="$PWD/$WORKDIR" ;; esac
: "${LIBFABRIC_ROOT:=$WORKDIR/build}"
: "${SUET_SRC_DIR:=$SCRIPT_DIR/../suet}"
: "${HTSIM_REPO:=https://github.com/ultraethernet/uet-htsim.git}"
: "${HTSIM_REF:=65ea315b25725df9791a5b93b61cbd6938cba0a8}"
: "${HTSIM_BUILD_DIR:=$WORKDIR/htsim-build}"
: "${SUET_SANITIZE:=OFF}"
: "${JOBS:=$( (nproc 2>/dev/null) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

case "${1:-build}" in
    test)
        if [ ! -f "$HTSIM_BUILD_DIR/CTestTestfile.cmake" ]; then
            echo "no htsim tests found under $HTSIM_BUILD_DIR; run ./build.sh --htsim first" >&2
            exit 1
        fi
        exec ctest --test-dir "$HTSIM_BUILD_DIR" --output-on-failure
        ;;
    build) ;;
    *) echo "usage: $0 [build|test]" >&2; exit 2 ;;
esac

if [ ! -f "$LIBFABRIC_ROOT/include/rdma/fabric.h" ]; then
    echo "no libfabric install found under $LIBFABRIC_ROOT; run ./build.sh --htsim first" >&2
    exit 1
fi
LIBFABRIC_ROOT="$(cd "$LIBFABRIC_ROOT" && pwd)"

# A supplied checkout is used as-is. Only a new managed checkout is initialized.
if [ -z "${HTSIM_SOURCE_DIR:-}" ]; then
    HTSIM_SOURCE_DIR="$WORKDIR/deps/uet-htsim"
    if [ ! -d "$HTSIM_SOURCE_DIR" ]; then
        mkdir -p "$(dirname "$HTSIM_SOURCE_DIR")"
        git clone --no-checkout "$HTSIM_REPO" "$HTSIM_SOURCE_DIR"
        git -C "$HTSIM_SOURCE_DIR" checkout --detach "$HTSIM_REF"
    elif [ "$(git -C "$HTSIM_SOURCE_DIR" rev-parse HEAD)" != \
           "$(git -C "$HTSIM_SOURCE_DIR" rev-parse "$HTSIM_REF^{commit}")" ]; then
        echo "managed htsim checkout differs from HTSIM_REF=$HTSIM_REF" >&2
        echo "set HTSIM_SOURCE_DIR to use an existing checkout as-is" >&2
        exit 1
    fi
fi

cmake -S "$SCRIPT_DIR" -B "$HTSIM_BUILD_DIR" \
    -DHTSIM_SOURCE_DIR="$HTSIM_SOURCE_DIR" \
    -DLIBFABRIC_ROOT="$LIBFABRIC_ROOT" \
    -DSUET_SRC_DIR="$SUET_SRC_DIR" \
    -DENABLE_SUET_TESTS=ON -DSUET_SANITIZE="$SUET_SANITIZE"
cmake --build "$HTSIM_BUILD_DIR" --target htsim_suet fabric_dgram_test --parallel "$JOBS"

printf '\nhtsim runner: %s/htsim_suet\n' "$HTSIM_BUILD_DIR"
printf 'provider path: %s\n' "$HTSIM_BUILD_DIR"
printf 'set FI_PROVIDER_PATH to the provider path when running htsim_suet\n'
