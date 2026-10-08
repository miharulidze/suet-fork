# Building SUET

## Automated build

```bash
./build.sh        # builds libfabric and fabtests into ./suet-workspace/build/
./build.sh --test # runs test_suet.py against the build
```

For anything beyond the default (running a subset, multi-node, custom timeout, etc.) invoke the test runner directly:

```bash
python3 test_suet.py --bin-dir suet-workspace/build/bin --timeout 60 --verbose
```

## Simulator build

```bash
./build.sh --htsim       # libfabric + fabtests + htsim DGRAM provider and runner
./build.sh --test-htsim  # provider contract and simulator integration tests
```

Use `./build.sh --htsim-only` to build the simulator integration against an
existing install (`LIBFABRIC_ROOT`, default `$WORKDIR/build`). Set
`HTSIM_SOURCE_DIR` to use an existing `uet-htsim` checkout; otherwise the build
fetches a pinned revision. See [htsim/README.md](htsim/README.md) for run commands,
build overrides and simulator limitations.

## Manual build

1. **Setup repository root**
   
   ```bash
   export SUET_REPO=$(pwd) # should point to this repo root
   ```

2. **Clone upstream libfabric**

   ```bash
   git clone https://github.com/ofiwg/libfabric.git
   export LIBFABRIC_DIR=$(pwd)/libfabric
   ```

3. **Fetch and cherry-pick `FI_SOURCE/FI_SOURCE_ERR` commits**
   
   These commits are not yet merged upstream but add necessary libfabric API-compliant features to Verbs and UDP DGRAM providers. We pull them from the fork at `https://github.com/miharulidze/libfabric.git`.

   ```bash
   cd $LIBFABRIC_DIR
   git fetch https://github.com/miharulidze/libfabric.git \
       304fe27a299042685c029d03d692c54abeb44409 \
       5ea1ba6ed6d56973b516cc24d3489d1c703b500a

   git cherry-pick 304fe27a299042685c029d03d692c54abeb44409 # prov/udp: FI_SOURCE_ERR
   git cherry-pick 5ea1ba6ed6d56973b516cc24d3489d1c703b500a # prov/verbs DGRAM: FI_SOURCE / FI_SOURCE_ERR
   ```

4. **Add the SUET provider source code**
   
   The repo's `$SUET_REPO/suet/` directory contains SUET provider source code. It is intended to sit alongside other providers in the `prov/` directory of upstream libfabric.

   ```bash
   mkdir -p "$LIBFABRIC_DIR/prov/suet"
   rsync -a --delete --exclude='.git' --exclude='build*' "$SUET_REPO/suet/" "$LIBFABRIC_DIR/prov/suet/"
   ```

5. **Apply the SUET integration patch.**

   The patch registers the SUET provider in five spots in the upstream libfabric tree to ensure that the build infrastructure and runtime detect the presence of SUET: `Makefile.am`,
   `configure.ac`, `include/ofi_prov.h`, `include/rdma/fabric.h`, and `src/fabric.c`.

   ```bash
   cd $LIBFABRIC_DIR
   git apply "$SUET_REPO/patches/0001-register-suet-provider.patch"
   ```

6. **Build libfabric**

   ```bash
   cd $LIBFABRIC_DIR
   ./autogen.sh && ./configure --prefix=$LIBFABRIC_DIR/build
   make -j$(nproc) && make install
   ```

7. **Test SUET with fabtests**

   ```bash
   cd $LIBFABRIC_DIR/fabtests
   ./autogen.sh && ./configure --prefix=$LIBFABRIC_DIR/build --with-libfabric=$LIBFABRIC_DIR/build
   make -j$(nproc) && make install
   cd $SUET_REPO
   python3 test_suet.py --bin-dir $LIBFABRIC_DIR/build/bin --timeout 60 --verbose
   ```