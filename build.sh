#!/usr/bin/env bash
#
# Knobs (set via environment variables):
#   LIBFABRIC_REPO    git URL of the libfabric base  (default: ofiwg/libfabric)
#   LIBFABRIC_REF     branch or commit to check out  (default: main)
#   CHERRY_PICK_REPO  git URL holding the FI_SOURCE_ERR commits
#                                                    (default: miharulidze/libfabric)
#   WORKDIR           where to place the workspace   (default: ./suet-workspace)
#   SUET_SRC_DIR      suet provider source dir       (default: ./suet next to this script)
#   JOBS              make -j argument               (default: nproc/sysctl)
#
# Usage:
#   ./build.sh         # full fresh build (libfabric + fabtests)
#   ./build.sh --test  # run test_suet.py against the existing build
#
# --test does NOT build. Run a build first (./build.sh) and then test
# against it (./build.sh --test).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${LIBFABRIC_REPO:=https://github.com/ofiwg/libfabric.git}"
: "${LIBFABRIC_REF:=main}"
: "${CHERRY_PICK_REPO:=https://github.com/miharulidze/libfabric.git}"
: "${WORKDIR:=$PWD/suet-workspace}"
: "${SUET_SRC_DIR:=$SCRIPT_DIR/suet}"
: "${JOBS:=$( (nproc 2>/dev/null) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}"

# Cherry-pick SHAs (live in CHERRY_PICK_REPO; not yet upstream). These
# become no-ops once the underlying PRs land in LIBFABRIC_REPO and
# LIBFABRIC_REF is bumped past them.
UDP_FI_SOURCE_ERR_SHA="304fe27a299042685c029d03d692c54abeb44409"
VRB_FI_SOURCE_ERR_SHA="5ea1ba6ed6d56973b516cc24d3489d1c703b500a"

PATCH_FILE="$SCRIPT_DIR/patches/0001-register-suet-provider.patch"

TEST_ONLY=0
case "${1:-}" in
    "")        ;;
    --test)    TEST_ONLY=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         echo "unknown flag: $1" >&2; exit 2 ;;
esac

log() { printf '\n==> %s\n' "$*"; }

INSTALL="$WORKDIR/build"

# ---- --test mode: run the suet test suite against an existing build -----
if [ "$TEST_ONLY" = 1 ]; then
    TEST_SCRIPT="$SCRIPT_DIR/test_suet.py"
    if [ ! -d "$INSTALL/bin" ] || [ ! -f "$TEST_SCRIPT" ]; then
        echo "no build found under $WORKDIR" >&2
        echo "run ./build.sh (without --test) first to produce $INSTALL" >&2
        exit 1
    fi
    log "running $TEST_SCRIPT against $INSTALL"
    exec python3 "$TEST_SCRIPT" --bin-dir "$INSTALL/bin" --timeout 60 --verbose
fi

# Accept either layout for SUET_SRC_DIR:
#   <dir>/                  -- provider root (contents of prov/suet/)
#   <dir>/prov/suet/        -- a libfabric tree with the provider inside it
# The presence of src/suet.h + configure.m4 marks the provider root
# unambiguously (libfabric's own top-level src/ does not contain suet.h).
if [ -f "$SUET_SRC_DIR/src/suet.h" ] && [ -f "$SUET_SRC_DIR/configure.m4" ]; then
    :
elif [ -f "$SUET_SRC_DIR/prov/suet/src/suet.h" ]; then
    SUET_SRC_DIR="$SUET_SRC_DIR/prov/suet"
else
    echo "SUET_SRC_DIR=$SUET_SRC_DIR does not look like a suet provider source tree" >&2
    echo "(expected <dir>/src/suet.h or <dir>/prov/suet/src/suet.h to exist)" >&2
    exit 1
fi

mkdir -p "$WORKDIR"

# ---- 1. clone libfabric --------------------------------------------------
if [ ! -d "$WORKDIR/libfabric/.git" ]; then
    log "cloning $LIBFABRIC_REPO -> $WORKDIR/libfabric"
    git clone "$LIBFABRIC_REPO" "$WORKDIR/libfabric"
fi

cd "$WORKDIR/libfabric"
log "fetching"
git fetch --all --tags --quiet
log "checking out $LIBFABRIC_REF"
git checkout "$LIBFABRIC_REF"
git reset --hard "$LIBFABRIC_REF"
git clean -fdx -e suet-workspace

# ---- 2. fetch FI_SOURCE_ERR commits from the fork ------------------------
# The cherry-pick SHAs are not in LIBFABRIC_REPO yet; make them
# reachable in the local object DB by fetching from CHERRY_PICK_REPO.
log "fetching cherry-pick source $CHERRY_PICK_REPO"
git fetch --quiet "$CHERRY_PICK_REPO" \
    "$UDP_FI_SOURCE_ERR_SHA" "$VRB_FI_SOURCE_ERR_SHA" || \
    git fetch --quiet "$CHERRY_PICK_REPO" '+refs/heads/*:refs/remotes/_cherry/*'

# ---- 3. cherry-pick FI_SOURCE_ERR support --------------------------------
cherry_pick_if_missing() {
    local sha="$1"
    if git merge-base --is-ancestor "$sha" HEAD 2>/dev/null; then
        log "cherry-pick $sha already applied, skipping"
        return
    fi
    log "cherry-pick $sha"
    git cherry-pick --keep-redundant-commits "$sha"
}

cherry_pick_if_missing "$UDP_FI_SOURCE_ERR_SHA"
cherry_pick_if_missing "$VRB_FI_SOURCE_ERR_SHA"

# ---- 4. drop in suet provider source -------------------------------------
log "syncing suet provider source from $SUET_SRC_DIR"
mkdir -p prov/suet
rsync -a --delete \
      --exclude='.git' \
      --exclude='build' \
      --exclude='suet-workspace' \
      "$SUET_SRC_DIR/" prov/suet/

# ---- 5. apply integration patch ------------------------------------------
log "applying integration patch"
if git apply --check "$PATCH_FILE" 2>/dev/null; then
    git apply "$PATCH_FILE"
elif grep -q 'prov/suet/Makefile.include' Makefile.am; then
    log "integration already present, skipping patch"
else
    echo "integration patch failed to apply cleanly against $LIBFABRIC_REF" >&2
    git apply --reject "$PATCH_FILE" || true
    exit 1
fi

# ---- 6. build libfabric --------------------------------------------------
log "building libfabric (-> $INSTALL)"
./autogen.sh
./configure --prefix="$INSTALL"
make -j"$JOBS"
make install

# ---- 7. build fabtests ---------------------------------------------------
log "building fabtests"
cd fabtests
./autogen.sh
./configure --prefix="$INSTALL" --with-libfabric="$INSTALL"
make -j"$JOBS"
make install
cd ..

log "done. install prefix: $INSTALL"
log "  fi_info:        $INSTALL/bin/fi_info -p 'udp;ofi_suet'"
log "  fabtests:       $INSTALL/bin/fi_*"
log "  run test suite: $0 --test"
