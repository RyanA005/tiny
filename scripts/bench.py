#!/usr/bin/env python3
"""Load generator for tiny. Used by `make bench` / `make bench-stats`."""

from __future__ import annotations

import argparse
import os
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


def percentile(xs, p):
    if not xs:
        return 0.0
    ys = sorted(xs)
    i = min(len(ys) - 1, max(0, int(round((p / 100.0) * (len(ys) - 1)))))
    return ys[i]


def one_get(url, timeout=5.0):
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read()
            status = r.status
        return time.perf_counter() - t0, status, len(body), None
    except Exception as e:
        return time.perf_counter() - t0, 0, 0, str(e)


def run_load(url, n_requests, concurrency, timeout=5.0):
    lat = []
    ok = 0
    err = 0
    bytes_total = 0
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futs = [pool.submit(one_get, url, timeout) for _ in range(n_requests)]
        for f in as_completed(futs):
            dt, status, nbytes, e = f.result()
            lat.append(dt)
            if e is None and 200 <= status < 400:
                ok += 1
                bytes_total += nbytes
            else:
                err += 1
    wall = time.perf_counter() - t0
    return {
        "n": n_requests,
        "ok": ok,
        "err": err,
        "wall_s": wall,
        "rps": ok / wall if wall > 0 else 0.0,
        "Mbps": (bytes_total * 8 / wall) / 1e6 if wall > 0 else 0.0,
        "p50_ms": percentile(lat, 50) * 1000,
        "p95_ms": percentile(lat, 95) * 1000,
        "p99_ms": percentile(lat, 99) * 1000,
    }


def print_result(name, r):
    print(
        f"  {name:<18} ok={r['ok']:<6} err={r['err']:<4} "
        f"rps={r['rps']:8.1f}  p50={r['p50_ms']:7.2f}ms  "
        f"p95={r['p95_ms']:7.2f}ms  p99={r['p99_ms']:7.2f}ms  "
        f"Mbps={r['Mbps']:6.1f}"
    )


@case
def case_small(base, docroot):
    """Many GETs of a tiny HTML file (epoll + header path)."""
    r = run_load(f"{base}/", n_requests=2000, concurrency=64)
    print_result("small_html", r)
    return r


@case
def case_medium(base, docroot):
    """64 KiB file via sendfile."""
    path = docroot / "med.bin"
    path.write_bytes(os.urandom(64 * 1024))
    r = run_load(f"{base}/med.bin", n_requests=500, concurrency=32)
    print_result("medium_64k", r)
    return r


@case
def case_large(base, docroot):
    """1 MiB file via sendfile."""
    path = docroot / "large.bin"
    path.write_bytes(os.urandom(1024 * 1024))
    r = run_load(f"{base}/large.bin", n_requests=100, concurrency=16)
    print_result("large_1m", r)
    return r


@case
def case_404(base, docroot):
    """Missing paths (normalize + open miss)."""
    r = run_load(f"{base}/no/such/file", n_requests=1000, concurrency=64)
    # treat 404 as ok for this case
    r["ok"] = r["n"] - r["err"]  # urllib raises on 404
    # re-run counting 404 as success
    lat = []
    ok = err = 0
    t0 = time.perf_counter()
    def hit():
        t1 = time.perf_counter()
        try:
            urllib.request.urlopen(f"{base}/no/such/file", timeout=5)
            return time.perf_counter() - t1, False
        except urllib.error.HTTPError as e:
            return time.perf_counter() - t1, e.code == 404
        except Exception:
            return time.perf_counter() - t1, False
    with ThreadPoolExecutor(max_workers=64) as pool:
        futs = [pool.submit(hit) for _ in range(1000)]
        for f in as_completed(futs):
            dt, good = f.result()
            lat.append(dt)
            if good:
                ok += 1
            else:
                err += 1
    wall = time.perf_counter() - t0
    r = {
        "n": 1000, "ok": ok, "err": err, "wall_s": wall,
        "rps": ok / wall if wall else 0,
        "Mbps": 0.0,
        "p50_ms": percentile(lat, 50) * 1000,
        "p95_ms": percentile(lat, 95) * 1000,
        "p99_ms": percentile(lat, 99) * 1000,
    }
    print_result("not_found", r)
    return r


@case
def case_mixed(base, docroot):
    """Concurrent mix of small + medium."""
    urls = [f"{base}/", f"{base}/med.bin", f"{base}/subdir/"]
    (docroot / "subdir").mkdir(exist_ok=True)
    (docroot / "subdir" / "index.html").write_text("sub\n")
    lat = []
    ok = err = 0
    bytes_total = 0
    t0 = time.perf_counter()

    def hit(i):
        return one_get(urls[i % len(urls)])

    with ThreadPoolExecutor(max_workers=64) as pool:
        futs = [pool.submit(hit, i) for i in range(1500)]
        for f in as_completed(futs):
            dt, status, nbytes, e = f.result()
            lat.append(dt)
            if e is None and 200 <= status < 400:
                ok += 1
                bytes_total += nbytes
            else:
                err += 1
    wall = time.perf_counter() - t0
    r = {
        "n": 1500, "ok": ok, "err": err, "wall_s": wall,
        "rps": ok / wall if wall else 0,
        "Mbps": (bytes_total * 8 / wall) / 1e6 if wall else 0,
        "p50_ms": percentile(lat, 50) * 1000,
        "p95_ms": percentile(lat, 95) * 1000,
        "p99_ms": percentile(lat, 99) * 1000,
    }
    print_result("mixed", r)
    return r


@case
def case_slow_readers(base, docroot):
    """Many slow header drips while measuring a parallel healthy GET stream."""
    stop = threading.Event()

    def drip():
        import socket
        from urllib.parse import urlparse
        u = urlparse(base)
        host, port = u.hostname, u.port or 80
        while not stop.is_set():
            try:
                s = socket.create_connection((host, port), timeout=2)
                s.settimeout(1)
                # drip a few bytes then stall until stop
                s.send(b"GE")
                while not stop.wait(0.2):
                    try:
                        s.send(b"T")
                    except Exception:
                        break
                s.close()
            except Exception:
                time.sleep(0.05)

    drips = [threading.Thread(target=drip, daemon=True) for _ in range(48)]
    for t in drips:
        t.start()
    time.sleep(0.3)
    r = run_load(f"{base}/", n_requests=500, concurrency=32, timeout=8.0)
    stop.set()
    for t in drips:
        t.join(timeout=1)
    print_result("under_slowloris", r)
    return r


def wait_port(host, port, timeout=5.0):
    import socket
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection((host, port), timeout=0.2)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def pick_port():
    import socket
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default="./server")
    ap.add_argument("--port", type=int, default=0,
                    help="listen port (0 = ephemeral free port)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--stats", action="store_true",
                    help="send SIGUSR1 after each case (needs -DTINY_STATS server)")
    ap.add_argument("--perf", action="store_true",
                    help="wrap server in `perf record -g` if available")
    ap.add_argument("--case", action="append", default=None,
                    help="run only named case(s); default all")
    args = ap.parse_args()

    if args.port <= 0:
        args.port = pick_port()

    root = Path(__file__).resolve().parents[1]
    bin_path = Path(args.bin)
    if not bin_path.is_absolute():
        bin_path = root / bin_path

    with tempfile.TemporaryDirectory(prefix="tiny-bench-") as td:
        docroot = Path(td)
        (docroot / "index.html").write_text("<html>bench</html>\n")
        (docroot / "subdir").mkdir()
        (docroot / "subdir" / "index.html").write_text("sub\n")

        env = os.environ.copy()
        log_path = docroot / "server.log"
        logf = open(log_path, "w")

        perf_bin = None
        if args.perf:
            perf_bin = subprocess.getoutput("command -v perf").strip()
            if not perf_bin:
                print("perf not found; install linux-tools / perf and retry", file=sys.stderr)
                args.perf = False

        cmd = [str(bin_path), str(docroot), str(args.port)]
        if args.perf:
            out_data = root / "perf.data"
            cmd = [perf_bin, "record", "-g", "-o", str(out_data), "--"] + cmd
            print(f"perf recording -> {out_data}")

        proc = subprocess.Popen(cmd, cwd=str(root), stdout=logf, stderr=subprocess.STDOUT)
        try:
            if not wait_port(args.host, args.port):
                print("server failed to listen; log:", file=sys.stderr)
                logf.flush()
                print(log_path.read_text(), file=sys.stderr)
                return 1
            if proc.poll() is not None:
                print("server exited early; log:", file=sys.stderr)
                logf.flush()
                print(log_path.read_text(), file=sys.stderr)
                return 1
            logf.flush()
            if "listening" not in log_path.read_text():
                print("server did not log listen; log:", file=sys.stderr)
                print(log_path.read_text(), file=sys.stderr)
                return 1

            # kill target is the server; under perf it is the child
            stats_pid = proc.pid
            if args.perf:
                # best-effort: find server-bench child
                try:
                    kids = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
                    if kids:
                        stats_pid = int(kids[0])
                except Exception:
                    pass

            base = f"http://{args.host}:{args.port}"
            print(f"bench against {base}  pid={stats_pid}")
            print("-" * 88)

            selected = CASES
            if args.case:
                want = set(args.case)
                selected = [c for c in CASES if c.__name__.replace("case_", "") in want
                            or c.__name__ in want]
                if not selected:
                    print("no matching cases", file=sys.stderr)
                    return 1

            for fn in selected:
                if fn.__name__ == "case_mixed" and not (docroot / "med.bin").exists():
                    (docroot / "med.bin").write_bytes(os.urandom(64 * 1024))
                fn(base, docroot)
                if args.stats:
                    os.kill(stats_pid, signal.SIGUSR1)
                    time.sleep(0.2)

            print("-" * 88)
            if args.stats:
                print("server stats dumped to log via SIGUSR1:")
                logf.flush()
                for line in log_path.read_text().splitlines():
                    if "[STATS]" in line:
                        print(" ", line)
            if args.perf:
                print("run: perf report -i perf.data")
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
            logf.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
