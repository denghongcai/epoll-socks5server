CC=gcc
CFLAGS=-Wall

all: main

main: main.c
	$(CC) $(CFLAGS) main.c -o epoll-socks5server

clean:
	rm -rf epoll-socks5server
