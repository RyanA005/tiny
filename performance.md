# Performance log

Append-only. Each entry: commit, self-described state, wrk comparison chart.

Method unless noted: `./scripts/bench.sh --duration 30s --threads 4 --peer both`
(wrk vs native nginx + apache2, same docroot, localhost).

---

## 2026-09-24 20:26 UTC

- **commit:** `8f0d2bd70cd1f70ae896d50a8fe44da54822f6b4` (`8f0d2bd benchmarks and fixes on 404 route`)
- **host:** WSL2 linux 6.6.87, 20 CPUs
- **bench:** duration 30s, threads 4, peer both

### State

Static file server with accept queue + 8 workers × 64 epoll slots. Nonblocking
read/header/sendfile state machine, HTTP/1.1 keepalive with in-place buffer reuse
(no full memset/realloc on keep). 404/403/405 keep-alive when the client allows.
INFO logs gated (`-DTINY_VERBOSE`). Deadline sweep throttled. No open-file cache yet
(planned separately). Compared against tools/nginx and system apache2.

### Comparison (Requests/sec)

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache
------------  ------------  ------------  ------------  ----------  -----------
small            276776.68     252530.83      60601.50       1.10x        4.57x
small_close       94584.63      38681.95      40758.08       2.45x        2.32x
medium           103672.37      97461.86      64793.88       1.06x        1.60x
large             20600.11      19433.25      15499.30       1.06x        1.33x
not_found         95212.57     239210.86     111409.03       0.40x        0.85x
high_c           388666.87     272645.28     103537.58       1.43x        3.75x
```

### Notes

- Wins or ties nginx on small/close/medium/large/high_c at 30s.
- `not_found` still ~0.40× nginx (open-miss path; cache not implemented).
- wrk reported some socket timeouts on all three servers under this sustained load.

---

## 2026-09-24 20:54 UTC

- **commit:** `8f0d2bd70cd1f70ae896d50a8fe44da54822f6b4` + uncommitted (eventfd wake, accept4, queue aging, dir redirect, percent-decode, SIGUSR1)
- **host:** WSL2 linux 6.6.87, 20 CPUs
- **bench:** duration 30s, threads 4, peer both

### State

Same static server baseline, plus: per-worker eventfd wake on enqueue (idle
epoll_wait -1); accept4(SOCK_NONBLOCK|SOCK_CLOEXEC); discard queued FDs older
than 5s (monotonic accept_time); `/dir` -> 301 `/dir/`; strict percent-decode
before path normalize/openat2; SIGUSR1 only sets a flag, dump on accept thread
(SIGUSR1 blocked in worker/logger threads). Still no open-file cache.

### Comparison (Requests/sec)

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache
------------  ------------  ------------  ------------  ----------  -----------
small            298653.81     260828.23      64671.63       1.15x        4.62x
small_close       89942.64      37360.56      40441.90       2.41x        2.22x
medium           129162.96     100114.17      64016.03       1.29x        2.02x
large             20139.94      19378.76      16120.86       1.04x        1.25x
not_found        355850.26     242116.71     105891.59       1.47x        3.36x
high_c           414144.27     284311.85     104016.68       1.46x        3.98x
```

### Notes

- Ahead of nginx on every case at 30s; `not_found` flipped 0.40x -> 1.47x.
- medium also up (1.06x -> 1.29x). eventfd wake is the likely driver for both.
- Socket timeouts still present under sustained load on all three servers.

---

## 2026-09-29 12:54 UTC

- **commit:** `f9662d9ef466486997907ab523dbe280a8bea2b0` (`f9662d9 accept4, SIGUSR1 handling, URL decode, wake on accept`) + uncommitted (compile-time `config.h`, 6 workers x 128 slots, header buffer at end of `http_conn`)
- **host:** WSL2 linux 6.6.87 on 13th Gen i7-13700H. lscpu: 20 logical CPUs, 10 cores, 2 threads/core, 1 socket (hypervisor flattens the 6P+8E layout). Battery, other tabs closed. Load at start 0.99 (15-min average still 13 from earlier work).
- **bench:** duration 30s, threads 4, peer both

### State

Compiled constants in `src/config.h`. CLI is `./server <port> <root>` only. Shared eventfd wakes every worker on accept. `TINY_WORKERS` 6 (the chip's performance-core count) and `TINY_CONNS_PER_WORKER` 128 (768 slots, so the 256-connection case does not fill the server). Header buffer is the tail of `http_conn` at 4096 bytes. Still no open-file cache.

### Comparison (Requests/sec)

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache
------------  ------------  ------------  ------------  ----------  -----------
small            293287.60     258089.78      64778.38       1.14x        4.53x
small_close       76852.79      34567.86      41386.54       2.22x        1.86x
medium           128161.16      98400.07      64639.86       1.30x        1.98x
large             17715.25      16843.21      14260.44       1.05x        1.24x
not_found        360807.58     240374.30     110816.68       1.50x        3.26x
high_c           401139.50     291456.70      99668.28       1.38x        4.02x
```

### Notes

- Ahead of nginx on every case. Large-file transfer was 17.30 GB / 30s (nginx 16.45 GB), closer to the wall-power runs (~19-20 GB) than the busier battery runs (~10 GB).
- 6 workers restored keepalive (`small` 1.14x, `high_c` 1.38x) after 4 workers had dropped those to 0.61x and 1.03x. `small_close` stayed ahead (2.22x) because the accept wake is 6 threads, not 8.
- Socket timeouts still present under sustained load on all three servers.

---

## 2026-09-29 13:28 UTC

- **commit:** `f9662d9ef466486997907ab523dbe280a8bea2b0` + uncommitted
- **host:** same WSL2 i7-13700H, battery
- **bench:** duration 10s, threads 4, peer nginx (apache skipped)

### State

Skip `epoll_ctl(EPOLL_CTL_MOD)` when the slot is already armed for the same interest. `tiny_slot.armed_io` stores `HTTP_IO_WANT_READ` or `HTTP_IO_WANT_WRITE`. Accept still `EPOLL_CTL_ADD`s with `EPOLLIN` and sets `armed_io` to read, so the extra MOD on a connection that stays readable is gone. A real READ to WRITE or WRITE to READ transition still mods.

### Comparison (Requests/sec)

10s baseline immediately before this change, same binary settings otherwise:

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            203839.80     255665.56       0.80x
small_close       74759.79      36317.96       2.06x
medium           129941.61     101903.94       1.28x
large             19569.22      18963.35       1.03x
not_found        343369.00     242140.44       1.42x
high_c           394486.77     285994.78       1.38x
```

After:

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            303180.66     256183.37       1.18x
small_close       75520.60      36595.64       2.06x
medium           125755.79      99399.41       1.27x
large             18662.35      18080.27       1.03x
not_found        353126.11     246239.38       1.43x
high_c           398774.56     296761.08       1.34x
```

### Notes

- `small` moved 0.80x to 1.18x. nginx held ~256k both runs, so that jump is on tiny. It also lands on the earlier 30s result (293k, 1.14x), and the 10s baseline `small` was the outlier.
- `small_close`, `medium`, `large`, and `not_found` ratios stayed within 0.01x. `high_c` 1.38x to 1.34x while absolute tiny rps rose slightly and nginx rose more.
- Kept. The MOD is still required on an actual interest change, which is once per request in each direction on the keepalive path.

---

## 2026-09-29 13:31 UTC

- **commit:** `f9662d9ef466486997907ab523dbe280a8bea2b0` + uncommitted
- **host:** same WSL2 i7-13700H, battery. This run was slower overall (nginx `small` 256k to 236k, `not_found` 246k to 237k).
- **bench:** duration 10s, threads 4, peer nginx (apache skipped)

### State

Replaced the two `TCP_CORK` setsockopt calls around a static response with `MSG_MORE` on the header `send` when a body follows, then `sendfile` in the same call. Cork was removed, including the uncork on connection cleanup. Responses still completed (200 with full body, 404, two keep-alive responses on one socket).

### Comparison (Requests/sec)

Previous entry (cork kept, epoll MOD skipped) is the before. After `MSG_MORE`:

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            237121.76     236296.92       1.00x
small_close       64756.98      33510.33       1.93x
medium           113368.44      88844.01       1.28x
large             17456.97      17726.65       0.98x
not_found        313659.29     236641.42       1.33x
high_c           382118.94     302227.88       1.26x
```

### Notes

- `small` fell 1.18x to 1.00x. `not_found` does not touch this path and also fell (1.43x to 1.33x), so part of the drop is the machine, but `small` fell further than that.
- `large` went from 1.03x to 0.98x. `medium` stayed 1.28x.
- Reverted to `TCP_CORK`. The setsockopt pair stays.

---

## 2026-09-29 13:36 UTC

- **commit:** `f9662d9ef466486997907ab523dbe280a8bea2b0` + uncommitted
- **host:** same WSL2 i7-13700H, battery. Still the slower stretch (nginx `small` 238k, versus 256k on the epoll run).
- **bench:** duration 10s, threads 4, peer nginx (apache skipped)

### State

`nactive` was already counted without scanning slots. `try_accept_one` still walked the slot array from index 0 to find a free one. Each worker now keeps a free stack of slot indexes (`uint16_t`, 128 entries because `TINY_CONNS_PER_WORKER` is 128, so one `uint64_t` bitmap does not cover it). Pop on accept, push on close. Deadline sweep still walks slots, and returns immediately when `nactive` is 0. Cork and the epoll `armed_io` skip are still in place.

### Comparison (Requests/sec)

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            264437.47     238206.62       1.11x
small_close       64503.72      33636.97       1.92x
medium           110955.06      86966.38       1.28x
large             16663.18      15849.26       1.05x
not_found        297795.06     227698.55       1.31x
high_c           362272.50     305769.25       1.18x
```

### Notes

- No clear win against the epoll+cork run (`small` 1.18x, `small_close` 2.06x, `not_found` 1.43x, `high_c` 1.34x). nginx was slower on every case in this run, and `not_found` (almost no accepts after ramp-up) moved about as much as `small_close` (an accept every request).
- Kept. Accept is O(1) in the slot table, and the ratios did not show a regression past the machine movement. `medium` stayed 1.28x and `large` was 1.05x.

---

## 2026-09-29 13:40 UTC

- **commit:** `f9662d9ef466486997907ab523dbe280a8bea2b0` + uncommitted
- **host:** same WSL2 i7-13700H, battery. nginx `small` 241k, in line with the previous 10s run (238k).
- **bench:** duration 10s, threads 4, peer nginx (apache skipped)

### State

Per-worker open-file cache: 64 slots, FNV-1a of the relative path, linear probe, no eviction and no invalidation. A hit reuses the cached fd, size, and MIME string. `sendfile` uses an explicit offset, so the fd stays open for the life of the worker. `http_conn.file_owned` stops cleanup from closing a cached fd. Misses still `openat2` + `fstat`, then insert. 404s are not cached. Header `snprintf` and `TCP_CORK` are unchanged. Epoll `armed_io` skip and the free-slot stack are still in place.

### Comparison (Requests/sec)

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            290539.10     240856.92       1.21x
small_close       65047.02      34045.40       1.91x
medium           116901.91      88375.06       1.32x
large             14925.23      15074.51       0.99x
not_found        321699.53     230097.45       1.40x
high_c           431396.12     297640.94       1.45x
```

### Notes

- Real win on the keepalive file cases. Against the previous 10s run on a similar nginx (`small` 1.11x, `high_c` 1.18x): `small` 1.21x, `high_c` 1.45x (362k to 431k while nginx stayed ~300k). `medium` 1.28x to 1.32x.
- `small_close` stayed 1.91x. A new connection every request, so accept and close dominate the open.
- `not_found` is not cached. It moved 1.31x to 1.40x anyway; do not credit the cache for that.
- `large` 1.05x to 0.99x. One open per connection, then the run is `sendfile`. Tied with nginx.
- Kept. A replaced file is served from the old inode until the worker exits.

---

## 2026-09-29 21:52 UTC

- **commit:** `4c6991b88d44529670998a65869abcf1bc039630` (`4c6991b reverse proxy implementation`)
- **host:** WSL2 linux 6.6.87, i7-13700H, 20 CPUs. nginx `small` 250k, a bit above the previous 10s run (241k).
- **bench:** duration 10s, threads 4. Static suite via `scripts/bench.sh --peer both`. Proxy suite is separate: one nginx origin serving `<html>bench</html>`, then tiny, nginx, and apache each proxying to it. Upstream keepalive off on all three. Apache `ProxyPass ... disablereuse=On`. nginx `proxy_pass` left at its default (HTTP/1.0, no upstream pool).

### State

Config file is the only CLI. `listen` plus `static / <root>` is what the static bench starts. A single primary `static /` route still skips the route table and calls `static_begin()`. Proxy is one nonblocking upstream socket per request, owned by the same worker slot. The request and the response are rewritten with `Connection: close`. No upstream pool. Open-file cache, epoll `armed_io` skip, free-slot stack, and `TCP_CORK` are unchanged.

### Static comparison (Requests/sec)

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache
------------  ------------  ------------  ------------  ----------  -----------
small            330655.44     249634.22      46307.54       1.32x        7.14x
small_close       75118.99      34811.71      40811.13       2.16x        1.84x
medium           125659.30      91522.99      64017.80       1.37x        1.96x
large             19699.40      18474.62      16145.96       1.07x        1.22x
not_found        343596.91     235957.68     108512.30       1.46x        3.17x
high_c           472946.00     321420.23     100384.20       1.47x        4.71x
```

### Proxy comparison (Requests/sec)

Origin column is nginx serving the file directly. The other three are proxies in front of that same origin.

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache     origin
------------  ------------  ------------  ------------  ----------  -----------  -----------
small             33562.00      42676.78      32479.65       0.79x        1.03x    240178.32
small_close       33171.09      31406.72      29816.69       1.06x        1.11x     37186.98
high_c            40930.91      92417.60      47185.40       0.44x        0.87x    275479.44
```

`small` and `high_c` are 64 and 256 keepalive clients. `small_close` is 64 clients with `Connection: close`. No timeouts on the proxies. Origin `high_c` had 156 timeouts.

### Notes

- Static ratios are at or above the 13:40 UTC cache run (`small` 1.21x, `high_c` 1.45x, `not_found` 1.40x, `large` 0.99x). The config-file CLI did not move the static path.
- Proxy `small` and `small_close` are the same number (34k vs 33k). nginx's are not (43k vs 31k). Tiny answers `Connection: close`, so the keepalive run never reuses the client socket. It also sends `Connection: close` upstream and `connect()`s a new socket every request.
- At 64 connections that puts tiny next to apache and a little behind nginx. At 256 connections nginx is 2.3x tiny. nginx here is `worker_processes auto` (20). Tiny is still 6 workers. The origin itself is still ~240k, so none of the proxies are close to it.
- Next: stop forcing `Connection: close` on the client when the response has `Content-Length` or chunked framing, and keep idle upstream sockets on the worker (fixed stack, no lock, same shape as the file cache). Responses with no length still have to close the upstream. Do not raise the worker count until those two are measured.

---

## 2026-09-29 22:11 UTC

- **commit:** `4c6991b88d44529670998a65869abcf1bc039630` + uncommitted
- **host:** same WSL2 i7-13700H. Origin nginx `small` 243k, in line with the 21:52 run (240k).
- **bench:** duration 10s, threads 4. Same proxy fixture: nginx origin serving `<html>bench</html>`. Two peer setups. "no pool" matches the 21:52 entry (nginx default `proxy_pass`, Apache `disablereuse=On`). "pool" turns upstream keepalive on (nginx `keepalive 128` plus `proxy_http_version 1.1`, Apache `ProxyPass` reuse left on).

### State

Proxied responses keep the client connection when `http_should_keepalive` says so (GET/HEAD, no request body, client did not send `Connection: close`). The upstream request is sent with `Connection: keep-alive`. When the response has a known end and did not say `Connection: close`, the upstream fd goes on a per-worker idle stack (`PROXY_POOL_MAX` = slots per worker, no lock). A response with no length still closes that fd. A reused fd that dies before any response byte is dropped and the request is connected once more (GET/HEAD only). The client socket is `TCP_CORK`ed while the response is written, then uncorked, so the header and body leave as one segment.

### No upstream pool on the peers (Requests/sec)

Same peer config as the 21:52 entry. Tiny now pools. They do not.

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache
------------  ------------  ------------  ------------  ----------  -----------
small            100616.88      36322.56      33893.63       2.77x        2.97x
small_close       57010.59      31420.46      30150.33       1.81x        1.89x
high_c           188522.06      97019.25      49969.36       1.94x        3.77x
```

### Upstream keepalive on the peers (Requests/sec)

```
case                  tiny         nginx        apache  tiny/nginx  tiny/apache     origin
------------  ------------  ------------  ------------  ----------  -----------  -----------
small            100616.88     118121.73      59145.21       0.85x        1.70x    242517.66
small_close       57010.59      33143.10      40795.57       1.72x        1.40x     34757.56
high_c           188522.06     194375.69      80484.70       0.97x        2.34x    299527.24
```

Average latency, tiny vs nginx-with-pool: `small` 633us vs 594us, `high_c` 1.43ms vs 1.36ms. Close-case timeouts: tiny 62, origin 60, nginx-pool 37, apache-pool 50. No timeouts on the keepalive proxy runs.

### Notes

- Against the 21:52 tiny numbers (`small` 34k, `small_close` 33k, `high_c` 41k): `small` 101k, `small_close` 57k, `high_c` 189k. `small` and `small_close` are no longer the same rate, so the client connection is actually being kept.
- `small_close` moved because the upstream fd is reused while the client still reconnects every request. It is above the origin's own close rate (35k). The origin close run is accept-bound. Tiny's close run is not opening an upstream socket per request.
- Fair peer is nginx with a keepalive pool: 0.85x at 64 connections, 0.97x at 256. nginx is still `worker_processes auto` (20) and Tiny is still 6 workers. Apache with backend reuse is well behind (1.70x and 2.34x).
- Origin direct is 243k / 300k. The extra hop is still a userspace copy. Worker count stays at 6 until that gap is the thing being measured.

---

## 2026-09-29 22:22 UTC

- **commit:** `4c6991b88d44529670998a65869abcf1bc039630` + uncommitted
- **host:** same WSL2 i7-13700H.
- **bench:** duration 10s, threads 4. Same `<html>bench</html>` origin. Peer is nginx with `keepalive 128` and `proxy_http_version 1.1`, the fair setup from the 22:11 entry.

### State

The upstream socket is added to epoll only when a read or write returns `EAGAIN`. A request that finishes inline does not `epoll_ctl` at all. A GET with no body is one `send` upstream, with no cork. When the response body is already in the shuttle buffer, header and body go out in one `sendmsg`, and the client is not corked. A response that is still arriving keeps `TCP_CORK` until `proxy_done`.

### Comparison (Requests/sec)

```
case                  tiny         nginx   tiny/nginx
------------  ------------  ------------  ----------
small            123877.37     117520.53       1.05x
small_close       57412.93      33723.18       1.70x
high_c           199266.79     194357.18       1.03x
```

Average latency: `small` 505us vs 596us, `small_close` 762us vs 1.42ms, `high_c` 1.43ms vs 1.35ms. Timeouts: tiny `high_c` 103, nginx `high_c` 210. None on the 64-connection runs.

### Notes

- Against the 22:11 tiny numbers (`small` 101k, `small_close` 57k, `high_c` 189k) and that run's nginx-with-pool (118k / 33k / 194k). `small` 101k to 124k and is now ahead of nginx (0.85x to 1.05x). `high_c` 189k to 199k (0.97x to 1.03x). `small_close` did not move. That case is still the client handshake.
- Kept. Worker count is still 6.
