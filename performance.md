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
