# NCCL over SUET/UDP: Ault experiments

For a reproducible build and checked 2-/4-GPU run, follow
[Test NCCL over SUET/UDP](TESTING.md). It specifies source revisions, paths,
transport settings, loader verification and pass criteria.

The [endpoint-addressing fix](endpoint-fix-2026-10-14.md) now passes the
2-/4-GPU ROD tests from 8 bytes through 1 MiB with zero validation errors.
Its report records the implementation, regressions, verified library loading,
and saved binaries. The records below preserve the earlier vanilla attempts.

The later [retry with Conda GCC/CUDA](retry-2026-10-14.md) built all
components and ran the 2-/4-GPU ROD tests. Both SUET tests failed at runtime;
the retry report includes successful controls and a confirmed endpoint
addressing defect. The record below describes the initial attempt only.

## First attempt result — 2026-10-14

The first vanilla-stack attempt failed during setup/build. Neither the
2-GPU nor 4-GPU NCCL test ran, and there are no correctness or bandwidth
results. No provider, NCCL, plugin, integration-patch, or cluster configuration
fixes were applied.

“Vanilla” means the clean SUET `refactor` branch and unmodified default-branch
checkouts of the requested NCCL and AWS OFI NCCL repositories. SUET's existing
`build.sh` still performs its documented libfabric integration and dependency
patching; no additional compatibility patches were introduced.

| Stage | Result | Evidence |
| --- | --- | --- |
| Allocate `ault23` | Rejected: node `IDLE+DRAIN`, reason `check fs` | `scontrol show node ault23`; immediate `salloc` attempt |
| Allocate approved replacement `ault24` | Successful: direct allocation `892393`, four V100 GPUs visible; released after diagnosis | [GPU inventory and source provenance](logs/2026-10-14-ault24/orchestration.log) |
| Build NVIDIA NCCL | Failed, `make` exit 2: inherited C++ compiler path absent; CUDA toolkit path also absent | [Corrected NCCL command and output](logs/2026-10-14-ault24/nccl-build.log) |
| Build SUET/libfabric | Failed, exit 1: entropy-extension patch rejected before autogen/configure/compilation | [SUET build log](logs/2026-10-14-ault24/suet-build.log) |
| Build AWS OFI NCCL | Autogen succeeded; configure failed, exit 77: inherited C compiler path absent | [Plugin build log](logs/2026-10-14-ault24/plugin-build.log), [config.log](logs/2026-10-14-ault24/plugin-config.log) |
| Discover `udp;ofi_suet` | Not run: no installed SUET/libfabric | [Status](logs/2026-10-14-ault24/status.txt) |
| NCCL all-reduce, 2 GPUs | Blocked by build failures; not run | [Status](logs/2026-10-14-ault24/status.txt) |
| NCCL all-reduce, 4 GPUs | Blocked by build failures; not run | [Status](logs/2026-10-14-ault24/status.txt) |

## Source revisions

Default-branch heads were resolved and recorded on the experiment date.

| Component | Repository / branch | Commit |
| --- | --- | --- |
| SUET | Local clean `refactor`, remote `miharulidze/suet-fork` | `05120e5afd5b675d9f1df97d6c60e70cb02731ef` |
| NCCL | [NVIDIA/nccl](https://github.com/NVIDIA/nccl), `master` | `3e46c089f4ae4732722a419eb6096aead7e47a30` |
| OFI plugin | [miharulidze/aws-ofi-nccl](https://github.com/miharulidze/aws-ofi-nccl), `master` | `745b951f443511bbc79beb0868cd8a967907eb42` |
| libfabric base | [ofiwg/libfabric](https://github.com/ofiwg/libfabric), `main` | `0d52314b7e1d9329f9843e46040fa4f8a74ed121` |
| NCCL tests | [NVIDIA/nccl-tests](https://github.com/NVIDIA/nccl-tests), `master`; fetched, not built | `afd59abab774a6a4b492eb344bd0c845a0b7bb4d` |

SUET's RUD selection is present from commit `03098fb`; the selected head also
contains subsequent reliability, congestion, entropy and reordering work.
Before documentation changes, the local SUET worktree was clean. The four
component source checkouts on `ault24` had no tracked source diffs after the
attempt. The dependency libfabric tree contains only the existing build
script's successful FI_SOURCE_ERR cherry-picks and SUET registration patch,
followed by the failed entropy patch check.

## Allocation and environment

The requested reservation was attempted from `ssh ault`:

```bash
scontrol show node ault23
timeout 15s salloc --immediate=5 --nodelist=ault23 --nodes=1 \
  --exclusive --time=02:00:00 --job-name=suet-nccl-baseline true
```

Slurm reported `Required node not available (down, drained or reserved)`.
`ault23` had been drained by root with `Reason=check fs` dated 2026-07-09.
After approval to use `ault24`, a batch holder (`892392`) failed immediately
without an output log. A second batch holder (`892394`) was cancelled after
obtaining the working direct allocation:

```bash
salloc --no-shell --immediate=10 --partition=intelv100 --nodelist=ault24 \
  --nodes=1 --exclusive --gres=gpu:4 --time=03:00:00 \
  --job-name=suet-nccl-baseline
# Granted allocation 892393
srun --jobid=892393 --nodes=1 --ntasks=1 --chdir=/tmp hostname
srun --jobid=892393 --nodes=1 --ntasks=1 --chdir=/tmp nvidia-smi
```

`ault24` exposed 4 × Tesla V100-PCIE-16GB, driver `570.86.15`, and 64 logical
CPUs. The NVIDIA-SMI CUDA `12.8` display describes the driver's compatibility;
it does not establish the presence of a CUDA toolkit. The compute-node OS
reported CentOS 8.4; the login node reported Rocky Linux 8.10.

The login node could access NFS-backed `/users`, `/scratch`, and the Spack
modules under `/apps`. On `ault24`, all three directories were empty, and
`df -h` showed no corresponding NFS mounts. `srun` could not chdir to
`/users/mkhalilo`. This explains why batch jobs targeting shared working/log
directories could not launch normally, although no batch log was available
to establish the exact first batch-launch error.

Sources and logs were staged in
`/users/mkhalilo/Development/suet-nccl-20261014` on the login node and streamed
with tar through `srun` into `/tmp/suet-nccl-mkhalilo-20261014` on `ault24`.
The node-local directory allowed the build attempts without changing mounts.

Inherited modules were `gcc/10.2.0`, `cuda/11.4.0`, and `rdma-core/34.0`.
Their files were unavailable on the compute node. In particular, `CC` and
`CXX` were absolute paths below the missing `/apps` mount. A direct
`gcc --version` resolved to system GCC 8.4.1, but the builds honored the
inherited absolute `CC`/`CXX` instead. `command -v nvcc` returned no executable.
Autoconf 2.69, Automake 1.16.1 and Libtool 2.4.6 were available locally.
See [filesystem and environment evidence](logs/2026-10-14-ault24/diagnosis.log).

## Build commands and observed failures

These are the executed component commands, with the corrected NCCL
architecture flag shown below. They ran inside allocation `892393` from the
isolated node-local directory, with inherited `CC`/`CXX` preserved:

```bash
ROOT=/tmp/suet-nccl-mkhalilo-20261014
CUDA_ROOT=/apps/ault/spack/opt/spack/linux-centos8-zen2/gcc-10.2.0/cuda-11.4.0-udyaakpt7oztg7gnj764dhkhdf5ory5d
HWLOC_ROOT=/apps/ault/spack/opt/spack/linux-centos8-zen2/gcc-10.2.0/hwloc-2.5.0-gwltaenwfu2fe6elj2k2img2ospfhwvm

cd "$ROOT/nccl"
make -j4 src.build CUDA_HOME="$CUDA_ROOT" \
  NVCC_GENCODE='-gencode=arch=compute_70,code=sm_70'

cd "$ROOT/suet"
export GIT_COMMITTER_NAME='SUET baseline build'
export GIT_COMMITTER_EMAIL='mkhalilo@ault24'
JOBS=8 WORKDIR="$ROOT/suet-workspace" \
  LIBFABRIC_REF=0d52314b7e1d9329f9843e46040fa4f8a74ed121 bash ./build.sh

cd "$ROOT/aws-ofi-nccl"
./autogen.sh
./configure --prefix="$ROOT/plugin-install" \
  --with-libfabric="$ROOT/suet-workspace/build" \
  --with-cuda="$CUDA_ROOT" --with-hwloc="$HWLOC_ROOT" --disable-tests
# make -j4 and make install were conditional on configure succeeding;
# neither ran.
```

The initial harness accidentally appended `+` to the NCCL `sm_70` value.
Only that invocation typo was corrected, and NCCL was rerun with unchanged
sources and toolchain. It still failed on the missing C++ compiler. The
[initial output](logs/2026-10-14-ault24/nccl-build-first.log) and
[original harness](logs/2026-10-14-ault24/run-builds-initial.sh) are retained
as historical evidence; the latter is not a recommended build script.

NCCL's corrected attempt returned `make` exit 2 after the inherited
`/apps/.../bin/g++` command returned 127. It also repeatedly reported that
`nvcc` was absent at `CUDA_ROOT`. The plugin's configure returned 77 with
`C compiler cannot create executables`; its `config.log` explicitly records
`/apps/.../bin/gcc: No such file or directory` and exit 127. This is an
environment/path failure, not evidence of a source compilation error.
The plugin did not reach its C++20, CUDA, hwloc or libfabric checks.

SUET's build successfully applied these existing dependency changes:

- `304fe27a299042685c029d03d692c54abeb44409`: UDP FI_SOURCE_ERR.
- `5ea1ba6ed6d56973b516cc24d3489d1c703b500a`: Verbs DGRAM FI_SOURCE/FI_SOURCE_ERR.
- `patches/0001-register-suet-provider.patch`.

It then stopped at `patches/0002-sendmsg-ev.patch`:

```text
sendmsg EV patch does not match 0d52314b7e1d9329f9843e46040fa4f8a74ed121
error: patch failed: include/rdma/fi_endpoint.h:133
error: include/rdma/fi_endpoint.h: patch does not apply
```

The patch context expects:

```c
ssize_t (*recv_flush)(struct fid_ep *ep, uint64_t flags);
```

The selected upstream header instead contains:

```c
int (*recv_flush)(struct fid_ep *ep, uint64_t flags);
```

Upstream commit
[`3a2484ccf5b766cf8a9b371f06dcdaf3101f8b13`](https://github.com/ofiwg/libfabric/commit/3a2484ccf5b766cf8a9b371f06dcdaf3101f8b13)
made that change. The [failure-details log](logs/2026-10-14-ault24/failure-details.log)
includes its diff and the failed read-only `git apply --check` output.
SUET therefore did not reach compiler invocation or produce `fi_info`.

The change belongs to libfabric core's **2.8.0a1 development**, after the
2.7.0 release. Version 2.7.0 had no `recv_flush` member; core added it on
2026-10-01 and changed its return type on 2026-10-08. No release tag contains
the return-type change as of this experiment. See the
[verified version history](logs/2026-10-14-ault24-retry/libfabric-version-history.log).

## Proposed remedies — not applied

1. Restore the normal `/apps`, `/users` and `/scratch` mounts on the compute
   node through cluster administration, or use a healthy equivalent node.
   Check `test -x "$CC"`, `test -x "$CXX"`, and
   `test -x "$CUDA_HOME/bin/nvcc"` inside the allocation before rebuilding.
   Verify hwloc headers/libraries as well. The selected plugin master
   requires a C++20 compiler (`configure.ac`); system GCC 8.4 is not a
   sufficient substitute. Compiler/toolkit compatibility remains untested.
2. Refresh only the entropy patch's stale `recv_flush` context against the
   selected libfabric revision, preserving upstream's `int` return type and
   the new `sendmsg_ev` operation. Then validate the full patch and build.
   Alternatively, pin a previously validated libfabric revision that matches
   all required patches. No alternate revision was built in this attempt.
3. Once builds work, test the existing SENDRECV host-buffer fallback first.
   SUET advertises tagged RDM messaging, but does not advertise `FI_HMEM` or
   RMA `FI_READ`; the plugin's SENDRECV discovery has a non-GDR fallback.
   The plugin's RDMA/GDR paths are not established as usable by this source
   assessment. No CUDA memory support should be inferred from having GPUs.
4. Treat RUD testing as a separate compatibility question. The vanilla
   plugin's `sendrecv_get_hints()` requests `FI_ORDER_SAS` in both TX and RX.
   [SUET endpoint creation](../../suet/src/suet_ep.c) selects ROD whenever
   `tx_attr->msg_order != 0`, and RUD only when it is zero. Investigate whether
   the plugin's matching protocol can safely remove that ordering requirement
   before proposing an unordered SENDRECV mode. Do not force RUD while still
   promising ordered semantics. No ordering change was made.

## Pending 2/4-GPU test procedure — not executed

After the build blockers are resolved, build `nccl-tests` against the selected
NCCL, verify the dynamic library paths, and confirm discovery of
`udp;ofi_suet`. A minimal same-node correctness matrix would be:

```bash
cd "$ROOT/nccl-tests"
make -j4 CUDA_HOME="$CUDA_ROOT" NCCL_HOME="$ROOT/nccl/build"
export LD_LIBRARY_PATH="$ROOT/plugin-install/lib:$ROOT/nccl/build/lib:$ROOT/suet-workspace/build/lib:${LD_LIBRARY_PATH:-}"
export FI_PROVIDER='udp;ofi_suet'
export OFI_NCCL_PROTOCOL=SENDRECV
export NCCL_NET=Libfabric
export NCCL_P2P_DISABLE=1 NCCL_SHM_DISABLE=1
export NCCL_NET_GDR_LEVEL=LOC NCCL_PROTO=Simple
export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH
"$ROOT/suet-workspace/build/bin/fi_info" -p 'udp;ofi_suet' -t FI_EP_RDM
timeout 180s ./build/all_reduce_perf -b 8 -e 1M -f 2 -g 2 -w 2 -n 5 -c 1
timeout 180s ./build/all_reduce_perf -b 8 -e 1M -f 2 -g 4 -w 2 -n 5 -c 1
```

Disabling P2P and SHM requires the NCCL network transport for intra-node
peers; `NCCL_NET=Libfabric` requests this plugin explicitly. Accept a result
only when logs demonstrate the OFI plugin and `udp;ofi_suet` provider,
both GPU counts complete with zero validation errors, and no fallback is
substituted. Those conditions were not reached on 2026-10-14. With the
unmodified plugin ordering hints, this procedure would exercise ROD on the
RUD-capable branch, not actual RUD delivery.

All raw build logs are in [logs/2026-10-14-ault24/](logs/2026-10-14-ault24/).
The evidence archive was also saved on the login node at
`/users/mkhalilo/Development/suet-nccl-20261014/ault24-evidence.tar.gz`.
Allocation `892393` was cancelled after evidence capture, and `squeue`
confirmed no remaining jobs for this experiment.
