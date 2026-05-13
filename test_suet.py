#!/usr/bin/env python3
"""
Automated test suite for the suet provider.

Runs fabtests for RDM messaging, tagged, RMA, and atomics over udp;ofi_suet.
Each test spawns a server process, then a client, and reports pass/fail.

Usage:
    python3 test_suet.py                  # run all tests (loopback)
    python3 test_suet.py rdm              # run only RDM tests
    python3 test_suet.py rma              # run only RMA tests
    python3 test_suet.py atomic           # run only atomic tests
    python3 test_suet.py tagged           # run only tagged tests
    python3 test_suet.py -v               # verbose output
    python3 test_suet.py --bin-dir /path  # custom binary directory

    # Multi-node: server runs locally, client runs via SSH on the remote node
    python3 test_suet.py --server=node1 --client=node2 --provider="udp;ofi_suet"
"""

import argparse
import os
import shlex
import subprocess
import sys
import time

DEFAULT_PROVIDER = "udp;ofi_suet"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_TIMEOUT = 60  # seconds per test


def find_bin_dir():
    """Find the fabtests binary directory relative to this script."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    # Walk up to repo root and look for build/bin
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    candidate = os.path.join(repo_root, "build", "bin")
    if os.path.isdir(candidate):
        return candidate
    return None


def patch_test(test, provider, server_host, oob=False):
    """Replace default provider/host placeholders with actual runtime values."""
    def _patch(args):
        patched = [
            provider if a == DEFAULT_PROVIDER else
            (server_host if a == DEFAULT_HOST else a)
            for a in args
        ]
        if oob:
            patched.insert(1, "-b")
        return patched
    return {
        **test,
        "server": _patch(test["server"]),
        "client": _patch(test["client"]),
    }


def run_test(name, server_args, client_args, bin_dir, timeout, verbose,
             client_host=None):
    """
    Run a single fabtest: start server, start client, wait for both.
    Returns (passed: bool, output: str).

    If client_host is set, the client command is executed on the remote
    node via ``ssh <client_host> ...``.  The server always runs locally.
    """
    server_cmd = [os.path.join(bin_dir, server_args[0])] + server_args[1:]
    client_cmd = [os.path.join(bin_dir, client_args[0])] + client_args[1:]

    if client_host:
        remote_cmd = " ".join(shlex.quote(a) for a in client_cmd)
        client_cmd = ["ssh", client_host, remote_cmd]

    if verbose:
        print(f"\n  Server: {' '.join(shlex.quote(a) for a in server_cmd)}")
        print(f"  Client: {' '.join(shlex.quote(a) for a in client_cmd)}")

    server_proc = None
    client_proc = None
    output_lines = []

    try:
        # Start server
        server_proc = subprocess.Popen(
            server_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        time.sleep(2)  # let server bind

        if server_proc.poll() is not None:
            out = server_proc.stdout.read().decode(errors="replace")
            return False, f"Server exited early (rc={server_proc.returncode})\n{out}"

        # Start client
        client_proc = subprocess.Popen(
            client_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        # Wait for client to finish
        client_out, _ = client_proc.communicate(timeout=timeout)
        output_lines.append(client_out.decode(errors="replace"))

        # Wait for server to finish
        server_out, _ = server_proc.communicate(timeout=10)
        output_lines.append(server_out.decode(errors="replace"))

        passed = client_proc.returncode == 0 and server_proc.returncode == 0
        return passed, "\n".join(output_lines)

    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"
    except FileNotFoundError as e:
        return False, f"Binary not found: {e}"
    finally:
        for proc in (client_proc, server_proc):
            if proc and proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass


# ---------------------------------------------------------------------------
# Test definitions
# ---------------------------------------------------------------------------
# Each test uses server/client mode (no OOB -b flag, which hangs in subprocess).
# Server = no positional address arg, binds with -s DEFAULT_HOST.
# Client = positional DEFAULT_HOST arg to connect.

TESTS = [
    # ── RDM messaging ──
    {
        "name": "rdm_pingpong",
        "category": "rdm",
        "server": ["fi_rdm_pingpong", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_pingpong", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw",
        "category": "rdm",
        "server": ["fi_rdm_bw", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw_mt",
        "category": "rdm",
        "server": ["fi_rdm_bw_mt", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw_mt", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw_mt (bidir)",
        "category": "rdm",
        "server": ["fi_rdm_bw_mt", "-v", "-g", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw_mt", "-v", "-g", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw_mt (2 eps)",
        "category": "rdm",
        "server": ["fi_rdm_bw_mt", "-v", "-n", "2", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw_mt", "-v", "-n", "2", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw_mt (4 eps)",
        "category": "rdm",
        "server": ["fi_rdm_bw_mt", "-v", "-n", "4", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw_mt", "-v", "-n", "4", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
        "timeout": 120,
    },
    {
        "name": "rdm_bw_mt (4eps, bidir)",
        "category": "rdm",
        "server": ["fi_rdm_bw_mt", "-v", "-g", "-n", "4", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw_mt", "-v", "-g", "-n", "4", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm (functional)",
        "category": "rdm",
        "server": ["fi_rdm", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "unexpected_msg",
        "category": "rdm",
        "server": ["fi_unexpected_msg", "-e", "rdm", "-I", "10", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_unexpected_msg", "-e", "rdm", "-I", "10", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_cntr_pingpong",
        "category": "rdm",
        "server": ["fi_rdm_cntr_pingpong", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_cntr_pingpong", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "inject_test",
        "category": "rdm",
        "server": ["fi_inject_test", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_inject_test", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "cq_data",
        "category": "rdm",
        "server": ["fi_cq_data", "-e", "rdm", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_cq_data", "-e", "rdm", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "multi_recv",
        "category": "rdm",
        "server": ["fi_multi_recv", "-e", "rdm", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_multi_recv", "-e", "rdm", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "multi_ep",
        "category": "rdm",
        "server": ["fi_multi_ep", "-e", "rdm", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_multi_ep", "-e", "rdm", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    # ── Tagged messaging ──
    {
        "name": "rdm_tagged_pingpong",
        "category": "tagged",
        "server": ["fi_rdm_tagged_pingpong", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_tagged_pingpong", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_tagged_bw",
        "category": "tagged",
        "server": ["fi_rdm_tagged_bw", "-v", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_tagged_bw", "-v", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_tagged_peek",
        "category": "tagged",
        "server": ["fi_rdm_tagged_peek", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_tagged_peek", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "recv_cancel",
        "category": "tagged",
        "server": ["fi_recv_cancel", "-e", "rdm", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_recv_cancel", "-e", "rdm", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    # ── RMA ──
    {
        "name": "rma_bw (write)",
        "category": "rma",
        "server": ["fi_rma_bw", "-v", "-p", DEFAULT_PROVIDER, "-o", "write", "-s", DEFAULT_HOST],
        "client": ["fi_rma_bw", "-v", "-p", DEFAULT_PROVIDER, "-o", "write", DEFAULT_HOST],
    },
    {
        "name": "rma_bw (writedata)",
        "category": "rma",
        "server": ["fi_rma_bw", "-v", "-p", DEFAULT_PROVIDER, "-o", "writedata", "-s", DEFAULT_HOST],
        "client": ["fi_rma_bw", "-v", "-p", DEFAULT_PROVIDER, "-o", "writedata", DEFAULT_HOST],
    },
    {
        #fabtests:benchmarks/rma_pingpong.c:141: This test polls the last byte of an RMA buffer to check for fi_write completion on the RX side. Which doesn't guarantee that all of the data has arrived. Therefore, it can only support data verification for fi_writedata operations.
        "name": "rma_pingpong (write)",
        "category": "rma",
        "server": ["fi_rma_pingpong", "-p", DEFAULT_PROVIDER, "-o", "write", "-s", DEFAULT_HOST],
        "client": ["fi_rma_pingpong", "-p", DEFAULT_PROVIDER, "-o", "write", DEFAULT_HOST],
    },
    {
        "name": "rma_event",
        "category": "rma",
        "server": ["fi_rdm_rma_event", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_rma_event", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rma_tx_completion",
        "category": "rma",
        "server": ["fi_rma_tx_completion", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rma_tx_completion", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    # ── Atomics ──
    {
        "name": "rdm_atomic (all ops, DC)",
        "category": "atomic",
        "server": ["fi_rdm_atomic", "-v", "-o", "all", "-I", "1000", "-U", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_atomic", "-v", "-o", "all", "-I", "1000", "-U", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
        "timeout": 120,
    },
    {
        "name": "rdm_atomic (all ops)",
        "category": "atomic",
        "server": ["fi_rdm_atomic", "-v", "-o", "all", "-I", "1000", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_atomic", "-v", "-o", "all", "-I", "1000", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
        "timeout": 120,
    },
    # ── Extended RDM messaging ──
    {
        "name": "unexpected_msg (high concurrency)",
        "category": "rdm",
        "server": ["fi_unexpected_msg", "-e", "rdm", "-I", "1000", "-M", "16", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_unexpected_msg", "-e", "rdm", "-I", "1000", "-M", "16", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "unexpected_msg (DC)",
        "category": "rdm",
        "server": ["fi_unexpected_msg", "-e", "rdm", "-I", "1000", "-U", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_unexpected_msg", "-e", "rdm", "-I", "1000", "-U", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm (DC)",
        "category": "rdm",
        "server": ["fi_rdm", "-U", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm", "-U", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "multi_recv (recvmsg)",
        "category": "rdm",
        "server": ["fi_multi_recv", "-e", "rdm", "-M", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_multi_recv", "-e", "rdm", "-M", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    {
        "name": "rdm_bw (1MB)",
        "category": "rdm",
        "server": ["fi_rdm_bw", "-v", "-S", "1048576", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_bw", "-v", "-S", "1048576", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
        "timeout": 120,
    },
    {
        "name": "rdm_multi_client",
        "category": "rdm",
        "server": ["fi_rdm_multi_client", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_rdm_multi_client", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    # ── Extended tagged ──
    {
        "name": "recv_cancel (verify)",
        "category": "tagged",
        "server": ["fi_recv_cancel", "-e", "rdm", "-V", "-p", DEFAULT_PROVIDER, "-s", DEFAULT_HOST],
        "client": ["fi_recv_cancel", "-e", "rdm", "-V", "-p", DEFAULT_PROVIDER, DEFAULT_HOST],
    },
    # ── Extended RMA ──
    {
        "name": "rma_bw (write, 1MB)",
        "category": "rma",
        "server": ["fi_rma_bw", "-v", "-S", "1048576", "-p", DEFAULT_PROVIDER, "-o", "write", "-s", DEFAULT_HOST],
        "client": ["fi_rma_bw", "-v", "-S", "1048576", "-p", DEFAULT_PROVIDER, "-o", "write", DEFAULT_HOST],
        "timeout": 120,
    },
]


def main():
    parser = argparse.ArgumentParser(description="suet provider test suite")
    parser.add_argument(
        "categories", nargs="*",
        help="Test categories to run: rdm, tagged, rma, atomic (default: all)",
    )
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Show full test output")
    parser.add_argument("--bin-dir", default=None,
                        help="Path to directory containing fi_* binaries")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                        help=f"Per-test timeout in seconds (default: {DEFAULT_TIMEOUT})")
    parser.add_argument("--list", action="store_true",
                        help="List available tests without running them")
    parser.add_argument("--server", default=None,
                        help="Server hostname (runs locally). Implies multi-node mode.")
    parser.add_argument("--client", default=None,
                        help="Client hostname (runs via SSH from the server node)")
    parser.add_argument("--provider", default=None,
                        help=f"Provider name (default: {DEFAULT_PROVIDER})")
    parser.add_argument("--oob", action="store_true",
                        help="Add -b (out-of-band) flag to server and client commands")
    args = parser.parse_args()

    # Validate multi-node args
    if (args.server is None) != (args.client is None):
        print("Error: --server and --client must be specified together.",
              file=sys.stderr)
        sys.exit(1)

    provider = args.provider or DEFAULT_PROVIDER
    server_host = args.server or DEFAULT_HOST
    client_host = args.client  # None means loopback (local) mode

    # Resolve binary directory
    bin_dir = args.bin_dir or find_bin_dir()
    if not bin_dir or not os.path.isdir(bin_dir):
        print(f"Error: Cannot find fabtests binaries. Use --bin-dir.", file=sys.stderr)
        sys.exit(1)

    # Filter by category
    categories = set(args.categories) if args.categories else None
    tests = [t for t in TESTS if not categories or t["category"] in categories]

    if args.list:
        for t in tests:
            print(f"  [{t['category']:>7}] {t['name']}")
        return

    if not tests:
        print(f"No tests match categories: {args.categories}", file=sys.stderr)
        sys.exit(1)

    # Patch tests with runtime provider / host values
    tests = [patch_test(t, provider, server_host, oob=args.oob) for t in tests]

    mode = f"multi-node (server={server_host}, client={client_host})" \
           if client_host else "loopback"
    print(f"Running {len(tests)} tests with provider '{provider}' [{mode}]")
    print(f"Binary dir: {bin_dir}")
    print(f"Timeout: {args.timeout}s per test")
    print("=" * 60)

    results = []
    for t in tests:
        timeout = t.get("timeout", args.timeout)
        print(f"\n[{t['category']:>7}] {t['name']} ... ", end="", flush=True)

        passed, output = run_test(
            t["name"], t["server"], t["client"], bin_dir, timeout, args.verbose,
            client_host=client_host,
        )

        status = "PASS" if passed else "FAIL"
        print(status)

        if args.verbose or not passed:
            for line in output.strip().split("\n"):
                if line.strip():
                    print(f"    {line}")

        results.append((t["name"], t["category"], passed))

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p in results if p)
    failed = sum(1 for _, _, p in results if not p)
    print(f"Results: {passed} passed, {failed} failed, {len(results)} total")

    if failed:
        print("\nFailed tests:")
        for name, cat, p in results:
            if not p:
                print(f"  [{cat}] {name}")
        sys.exit(1)
    else:
        print("\nAll tests passed!")


if __name__ == "__main__":
    main()