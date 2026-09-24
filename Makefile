CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I src
LDFLAGS = -pthread

BENCH_PORT ?= 0
SERVER_SRC = src/main.c src/allocator.c src/connection.c src/http.c src/http_parser.c \
	src/static.c src/stats.c src/logger.c src/worker.c
SERVER_HDR = src/allocator.h src/connection.h src/http.h src/static.h src/stats.h \
	src/logger.h src/worker.h

.PHONY: all clean bench bench-stats perf server-stats

all: server client

server: $(SERVER_SRC) $(SERVER_HDR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

# Stats-enabled binary (kill -USR1 <pid> dumps phase averages).
server-stats: $(SERVER_SRC) $(SERVER_HDR)
	$(CC) $(CFLAGS) -DTINY_STATS -o server $(SERVER_SRC) $(LDFLAGS)

client: src/client.c
	$(CC) $(CFLAGS) -o $@ src/client.c $(LDFLAGS)

# Realistic HTTP cases. Port 0 = pick a free port.
# Override: make bench BENCH_PORT=21000
bench: server
	python3 scripts/bench.py --bin ./server --port $(BENCH_PORT)

bench-stats: server-stats
	python3 scripts/bench.py --bin ./server --port $(BENCH_PORT) --stats

# Requires `perf` (linux-tools). Writes ./perf.data then: perf report
perf: server
	python3 scripts/bench.py --bin ./server --port $(BENCH_PORT) --perf

clean:
	rm -f server client src/*.gch perf.data
