CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I src
LDFLAGS = -pthread

all: server client

server: src/main.c src/allocator.c src/allocator.h src/connection.c src/connection.h src/http.c src/http_parser.c src/http.h src/logger.c src/logger.h src/worker.c src/worker.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/allocator.c src/connection.c src/http.c src/http_parser.c src/logger.c src/worker.c $(LDFLAGS)

client: src/client.c
	$(CC) $(CFLAGS) -o $@ src/client.c $(LDFLAGS)

clean:
	rm -f server client test_http_parser bench_http_parser src/*.gch
