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
