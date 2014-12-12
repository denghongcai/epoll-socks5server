CC=gcc
CFLAGS=-pg -g -Wall

all: main

main: main.c
	$(CC) $(CFLAGS) main.c -o epoll-socks5server

clean:
	rm -rf epoll-socks5server
