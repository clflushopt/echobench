CC = gcc
CFLAGS = -Wall -Wextra -g -pthread -O2
LDFLAGS = -luring -pthread

all: echobench loadgen

echobench: echobench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

loadgen: loadgen.c
	$(CC) $(CFLAGS) -o $@ $< -pthread

clean:
	rm -f echobench loadgen

test: all
	@echo "Running sanity checks ..."
	./echobench -m epoll -p 9999 &
	@sleep 1
	./loadgen -s 127.0.0.1 -p 9999 -c 10 -t 1 -m 128 -d 3
	@killall echobench

.PHONY: all clean test
