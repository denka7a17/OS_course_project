#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define MAX_LINE 1024

typedef struct client
{
    int fd;
    char rbuf[MAX_LINE * 2];
    size_t rlen;
    secret_word_t word;
    char wrong_order[26];
    size_t wrong_count;
    bool solved;
} client_t;

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t) n;
    }
    return 0;
}

static int send_msg(int fd, const char *line)
{
    char buf[MAX_LINE * 2 + 2];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    if (n < 0) return -1;
    return send_all(fd, buf, (size_t) n);
}

static bool take_line(client_t *c, char *out, size_t outsz)
{
    for (size_t i = 0; i < c->rlen; i++)
    {
        if (c->rbuf[i] == '\n')
        {
            size_t len = i;
            if (len >= outsz) len = outsz - 1;
            memcpy(out, c->rbuf, len);
            out[len] = '\0';
            if (len > 0 && out[len - 1] == '\r') out[len - 1] = '\0';

            size_t rest = c->rlen - (i + 1);
            memmove(c->rbuf, c->rbuf + i + 1, rest);
            c->rlen = rest;
            return true;
        }
    }
    return false;
}

static int fill_buffer(client_t *c)
{
    if (c->rlen >= sizeof c->rbuf) return -1;
    ssize_t n = read(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen);
    if (n < 0)
    {
        if (errno == EINTR) return 1;
        return -1;
    }
    if (n == 0) return 0;
    c->rlen += (size_t) n;
    return 1;
}

static bool read_line_blocking(client_t *c, char *out, size_t outsz)
{
    while (!take_line(c, out, outsz))
    {
        if (fill_buffer(c) <= 0) return false;
    }
    return true;
}


static void build_masked(const secret_word_t *w, char *out)
{
    for (size_t i = 0; i < w->word_length; i++)
    {
        char ch;
        if (secret_word_letter_at(w, i, &ch) == SECRET_WORD_LETTER_REVEALED)
            out[i] = ch;
        else
            out[i] = '_';
    }
    out[w->word_length] = '\0';
}

static void build_wrong_csv(const client_t *c, char *out, size_t outsz)
{
    if (c->wrong_count == 0)
    {
        snprintf(out, outsz, "-");
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < c->wrong_count && pos + 2 < outsz; i++)
    {
        if (i > 0) out[pos++] = ',';
        out[pos++] = c->wrong_order[i];
    }
    out[pos] = '\0';
}

static void send_state(client_t *c)
{
    char masked[MAX_LINE];
    char wrong[64];
    char line[MAX_LINE * 2];

    build_masked(&c->word, masked);
    build_wrong_csv(c, wrong, sizeof wrong);
    snprintf(line, sizeof line, "STATE %s %s", masked, wrong);
    send_msg(c->fd, line);
}

static void send_end(client_t *me, client_t *opp, const char *code)
{
    char my_csv[64], opp_csv[64];
    char line[MAX_LINE];

    build_wrong_csv(me, my_csv, sizeof my_csv);
    build_wrong_csv(opp, opp_csv, sizeof opp_csv);
    snprintf(line, sizeof line, "END %s %s %s", code, my_csv, opp_csv);
    send_msg(me->fd, line);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t) port);

    if (bind(lsock, (struct sockaddr *) &addr, sizeof addr) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(lsock, 2) < 0)
    {
        perror("listen");
        return 1;
    }

    printf("Listening on %d...\n", port);
    fflush(stdout);

    client_t client[2];
    memset(client, 0, sizeof client);

    for (int i = 0; i < 2; i++)
    {
        int cfd = accept(lsock, NULL, NULL);
        if (cfd < 0)
        {
            perror("accept");
            return 1;
        }
        client[i].fd = cfd;
    }
    close(lsock);

    char supplied[2][MAX_LINE];
    for (int i = 0; i < 2; i++)
    {
        if (!read_line_blocking(&client[i], supplied[i], sizeof supplied[i]))
        {
            fprintf(stderr, "client %d disconnected before sending a word\n", i);
            close(client[0].fd);
            close(client[1].fd);
            return 1;
        }
    }

    secret_word_t w0, w1;
    bool ok0 = secret_word_init_from_c_string(&w0, supplied[0]);
    bool ok1 = secret_word_init_from_c_string(&w1, supplied[1]);

    if (!ok0 || !ok1)
    {
        if (!ok0) send_msg(client[0].fd, "ERR invalid word");
        if (!ok1) send_msg(client[1].fd, "ERR invalid word");
        if (ok0) secret_word_free(&w0);
        if (ok1) secret_word_free(&w1);
        close(client[0].fd);
        close(client[1].fd);
        return 1;
    }

    client[0].word = w1;
    client[1].word = w0;

    send_state(&client[0]);
    send_state(&client[1]);

    struct pollfd pfds[2];
    pfds[0].fd = client[0].fd; pfds[0].events = POLLIN;
    pfds[1].fd = client[1].fd; pfds[1].events = POLLIN;

    bool game_over = false;
    while (!game_over)
    {
        int r = poll(pfds, 2, -1);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        for (int i = 0; i < 2 && !game_over; i++)
        {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            int rc = fill_buffer(&client[i]);
            if (rc <= 0)
            {
                game_over = true;
                break;
            }

            char guess_line[MAX_LINE];
            while (take_line(&client[i], guess_line, sizeof guess_line))
            {
                if (client[i].solved) continue;

                char g = guess_line[0];
                secret_word_guess_result_t res = secret_word_guess(&client[i].word, g);
                if (res == SECRET_WORD_GUESS_INCORRECT && client[i].wrong_count < 26)
                {
                    client[i].wrong_order[client[i].wrong_count++] = normalize(g);
                }

                send_state(&client[i]);

                if (secret_word_is_solved(&client[i].word))
                    client[i].solved = true;
            }

            if (client[0].solved && client[1].solved)
            {
                size_t c0 = secret_word_incorrect_guess_count(&client[0].word);
                size_t c1 = secret_word_incorrect_guess_count(&client[1].word);

                const char *code0, *code1;
                if (c0 < c1)      { code0 = "WIN";  code1 = "LOSE"; }
                else if (c0 > c1) { code0 = "LOSE"; code1 = "WIN";  }
                else              { code0 = "TIE";  code1 = "TIE";  }

                send_end(&client[0], &client[1], code0);
                send_end(&client[1], &client[0], code1);
                game_over = true;
            }
        }
    }

    secret_word_free(&client[0].word);
    secret_word_free(&client[1].word);
    close(client[0].fd);
    close(client[1].fd);
    return 0;
}
