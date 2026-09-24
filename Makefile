CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I src
LDFLAGS = -pthread

DURATION ?= 5s
THREADS  ?= 4
PEER     ?= both
SERVER_SRC = src/main.c src/allocator.c src/connection.c src/http.c src/http_parser.c \
	src/static.c src/stats.c src/logger.c src/worker.c
SERVER_HDR = src/allocator.h src/connection.h src/http.h src/static.h src/stats.h \
	src/logger.h src/worker.h

.PHONY: all clean bench perf server-stats

all: server client

server: $(SERVER_SRC) $(SERVER_HDR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

server-stats: $(SERVER_SRC) $(SERVER_HDR)
	$(CC) $(CFLAGS) -DTINY_STATS -o server $(SERVER_SRC) $(LDFLAGS)

client: src/client.c
	$(CC) $(CFLAGS) -o $@ src/client.c $(LDFLAGS)

bench: server
	@chmod +x scripts/bench.sh
	./scripts/bench.sh --bin ./server --duration $(DURATION) --threads $(THREADS) --peer $(PEER)

perf: server
	@chmod +x scripts/bench.sh
	./scripts/bench.sh --bin ./server --duration $(DURATION) --threads $(THREADS) --peer $(PEER) --perf

clean:
	rm -f server client src/*.gch perf.data
