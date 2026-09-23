/*
 * weirdikectl -- talk to weirdiked over its local control socket.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * One request, one reply, no state. Keeping it this dumb is the point: the CGI
 * in the WebUI runs it with a fixed argument and prints what comes back, so
 * there is no place for a shell to get involved.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define WD_CTL_PATH "/var/run/weirdike.sock"

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "status";

    /* Only these three, compared exactly -- weirdikectl never forwards free
     * text to the daemon. */
    if (strcmp(cmd, "status") && strcmp(cmd, "down") && strcmp(cmd, "rekey")) {
        fprintf(stderr, "usage: weirdikectl [status|down|rekey]\n");
        return 2;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WD_CTL_PATH);

    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        /* The common case is "not running", and that is not an error worth a
         * stack trace -- the WebUI needs to tell it apart from a real fault. */
        if (errno == ENOENT || errno == ECONNREFUSED) {
            fprintf(stderr, "weirdiked is not running\n");
            close(fd);
            return 3;
        }
        perror("connect");
        close(fd);
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) { perror("write"); close(fd); return 1; }
    shutdown(fd, SHUT_WR);

    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) break;
    }
    close(fd);
    return 0;
}
