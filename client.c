#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define MAX_LINE 1024

static int    g_fd;
static char   g_rbuf[MAX_LINE * 2];
static size_t g_rlen = 0;

static bool take_line(char *out, size_t outsz)
{
    for (size_t i = 0; i < g_rlen; i++)
    {
        if (g_rbuf[i] == '\n')
        {
            size_t len = i;
            if (len >= outsz) len = outsz - 1;
            memcpy(out, g_rbuf, len);
            out[len] = '\0';
            if (len > 0 && out[len - 1] == '\r') out[len - 1] = '\0';

            size_t rest = g_rlen - (i + 1);
            memmove(g_rbuf, g_rbuf + i + 1, rest);
            g_rlen = rest;
            return true;
        }
    }
    return false;
}

static bool recv_line(char *out, size_t outsz)
{
    while (!take_line(out, outsz))
    {
        if (g_rlen >= sizeof g_rbuf) return false;
        ssize_t n = read(g_fd, g_rbuf + g_rlen, sizeof g_rbuf - g_rlen);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        g_rlen += (size_t) n;
    }
    return true;
}

static int send_all(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(g_fd, buf + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t) n;
    }
    return 0;
}

static void print_letters(const char *csv)
{
    if (strcmp(csv, "-") == 0) return;
    for (const char *p = csv; *p; p++)
    {
        if (*p == ',') fputs(", ", stdout);
        else           putchar(*p);
    }
}

static bool show_state(const char *masked, const char *wrong)
{
    printf("Word: %s\n", masked);
    printf("Incorrect guesses: ");
    print_letters(wrong);
    printf("\n");
    fflush(stdout);
    return strchr(masked, '_') == NULL;
}

static void show_end_and_exit(const char *code, const char *mine, const char *opp)
{
    const char *message;
    if      (strcmp(code, "WIN")  == 0) message = "YOU WIN! :)";
    else if (strcmp(code, "LOSE") == 0) message = "You Lose! :(";
    else                                message = "Tie :/";

    printf("%s\n", message);
    printf("Your incorrect guesses: ");
    print_letters(mine);
    printf("\n");
    printf("Opponent's incorrect guesses: ");
    print_letters(opp);
    printf("\n");
    fflush(stdout);
    exit(0);
}

static void handle_message(const char *line, bool *solved)
{
    if (strncmp(line, "STATE ", 6) == 0)
    {
        char masked[MAX_LINE] = "";
        char wrong[128]       = "-";
        sscanf(line + 6, "%1023s %127s", masked, wrong);
        *solved = show_state(masked, wrong);
    }
    else if (strncmp(line, "END ", 4) == 0)
    {
        char code[16] = "TIE", mine[128] = "-", opp[128] = "-";
        sscanf(line + 4, "%15s %127s %127s", code, mine, opp);
        show_end_and_exit(code, mine, opp);
    }
    else if (strncmp(line, "ERR", 3) == 0)
    {
        fprintf(stderr, "Server error: %s\n", line);
        exit(1);
    }

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <host> <port> <opponent-word>\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *word = argv[3];

    setvbuf(stdout, NULL, _IONBF, 0);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0)
    {
        fprintf(stderr, "could not resolve %s:%s\n", host, port);
        return 1;
    }

    g_fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next)
    {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            g_fd = s;
            break;
        }
        close(s);
    }
    freeaddrinfo(res);

    if (g_fd < 0)
    {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }

    {
        char buf[MAX_LINE];
        int n = snprintf(buf, sizeof buf, "%s\n", word);
        if (n < 0 || send_all(buf, (size_t) n) < 0)
        {
            fprintf(stderr, "failed to send word\n");
            return 1;
        }
    }

    char line[MAX_LINE];
    bool solved = false;

    if (!recv_line(line, sizeof line))
    {
        fprintf(stderr, "connection closed by server\n");
        return 1;
    }
    handle_message(line, &solved);

    while (!solved)
    {
        int ch = getchar();
        if (ch == EOF) break;
        if (ch == '\n') continue;

        int c2;
        while ((c2 = getchar()) != EOF && c2 != '\n')
            ;

        char out[2] = { (char) ch, '\n' };
        if (send_all(out, 2) < 0)
        {
            fprintf(stderr, "failed to send guess\n");
            return 1;
        }

        if (!recv_line(line, sizeof line))
        {
            fprintf(stderr, "connection closed by server\n");
            return 1;
        }
        handle_message(line, &solved);
    }

    while (recv_line(line, sizeof line))
        handle_message(line, &solved);

    return 0;
}
