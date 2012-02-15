/*
Copyright (c) 2011 YouView TV Ltd. <william.manley@youview.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "polipo.h"
#include <errno.h>

int
get_sd_socket() {
    int fd = -1;
    struct stat buf;
    int fdflags;
    int type;
    int mask;
    const char* env;

    env = getenv("LISTEN_FDS");
    if (!env || strcmp(env, "1") != 0)
        goto fail;
    unsetenv("LISTEN_FDS");

    env = getenv("LISTEN_PID");
    if (!env || strtoul(env, NULL, 10) != getpid()) {
        do_log(L_WARN, "Socket passing error: LISTEN_FDS present but "
               "LISTEN_PID (%s) doesn't match pid (%u)", env, getpid());
        goto fail;
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");

    /* LISTEN_FDS and LISTEN_PID are fine: the socket passing is enabled */
    fd = 3;

    if (fstat(fd, &buf) != 0) {
        do_log_error(L_WARN, errno, "Socket passing error: fstating the "
                     "passed file descriptor failed");
        goto fail;
    }

    if (!S_ISSOCK(buf.st_mode)) {
        do_log(L_WARN, "Socket passing error: File descriptor 3 passed in is "
               "not a socket");
        goto fail;
    }

    socklen_t len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0) {
        do_log_error(L_WARN, errno, "Socket passing error: getsockopt failed");
        goto fail;
    }

    if (len != sizeof(type)) {
        do_log(L_WARN, "Socket passing error: Bizzare: getsockopt failed to "
               "fill provided buffer");
        goto fail;
    }

    if (type != SOCK_STREAM) {
        do_log_error(L_WARN, errno, "Socket passing error: getsockopt failed");
        goto fail;
    }

    mask = fcntl(fd, F_GETFL, 0);
    if (mask < 0 || fcntl(fd, F_SETFL, mask | O_NONBLOCK) < 0) {
        do_log_error(L_WARN, errno, "Failed to set socket in non-blocking mode");
        errno = EBADF;
        goto fail;
    }

#ifdef O_CLOEXEC
    /* It doesn't hurt to set O_CLOEXEC if possible.  Don't really mind if
       this fails */
    if ((fdflags = fcntl(fd, F_GETFD)) != -1) {
        fcntl(fd, F_SETFD, fdflags | O_CLOEXEC);
    }
#endif // O_CLOEXEC

    return fd;
fail:
    CLOSE(fd);
    return -1;
}

