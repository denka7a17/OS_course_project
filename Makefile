CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2

all: hangman-server hangman-client

hangman-server: server.c game.c game.h
	$(CC) $(CFLAGS) -o hangman-server server.c game.c

hangman-client: client.c
	$(CC) $(CFLAGS) -o hangman-client client.c

clean:
	rm -f hangman-server hangman-client

.PHONY: all clean
