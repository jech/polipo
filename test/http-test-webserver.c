/*
Copyright (c) 2011 YouView TV Ltd <william.manley@youview.com>

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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include "../sd.h"

#ifndef O_CLOEXEC
// Old versions of uclibc don't have O_CLOEXEC defined even if the underlying
// kernel supports it:
#define O_CLOEXEC 0
#endif

void
really_do_log(int type, const char *f, ...)
{
    va_list args;

    va_start(args, f);
    vfprintf(stderr, f, args);
    va_end(args);
}

void
really_do_log_error(int type, int e, const char *f, ...)
{
    va_list args;
    va_start(args, f);
    vfprintf(stderr, f, args);
    va_end(args);
}

/**
 * Portable (and sub-optimal) implementation of Linux sendfile written such
 * that this will also work on BSDs.
 */
ssize_t portable_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    char buf[4096];
    off_t so = 0;
    ssize_t bytes_read;

    if (count > 4096)
        count = 4096;

    if (offset)
        bytes_read = pread(in_fd, buf, count, *offset);
    else
        bytes_read = read(in_fd, buf, count);

    if (bytes_read < 0) {
        perror("pread");
        // Error
        return bytes_read;
    }

    if (offset)
        *offset += bytes_read;

    ssize_t total_written = 0;
    do {
        ssize_t bytes_written = write(out_fd, buf + total_written, bytes_read - total_written);
        if (bytes_written < 0) {
            perror("write");
            // Error
            return bytes_written;
        }
        total_written += bytes_written;
    } while (total_written < bytes_read);

    return bytes_read;
}

void check(int condition, const char* fmt, ...) {
    if (!condition) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);

        exit(1);
    }
}

const size_t bufsize = 4096;
volatile static int requests_served = 0;

void sigterm_handler(int signo __attribute__((unused)) ) {
    printf("REQUESTS_SERVED=%d\n", requests_served);
    exit(0);
}

int main(int argc, char* argv[], char* envp[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n"
                        "    %s file-to-serve\n", argv[0]);
        exit(1);
    }
    // Set program up to exit with an error if no HTTP request is made
    struct sigaction sa;
    sa.sa_handler = &sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    // Open the input file before accept to detect errors early
    int infile = open(argv[1], O_RDONLY | O_CLOEXEC);
    check(infile >= 0, "Failed to open input file\n");
    struct stat stat_buf;
    fstat(infile, &stat_buf);
    size_t file_size = stat_buf.st_size;

    // We should have had our listening socket passed in as fd 3
    int sfd = get_sd_socket();
    if (sfd == -1) {
        fprintf(stderr, "Sockets incorrectly passed in!\n");
        exit(1);
    }


    // Remove O_NONBLOCK
    int mask = fcntl(sfd, F_GETFL, 0);
    if (mask < 0 || fcntl(sfd, F_SETFL, mask & ~O_NONBLOCK) < 0) {
        fprintf(stderr, "Failed to set socket in blocking mode.\n");
        exit(1);
    }

    // Child should connect to parent shortly
    int fd;
    while ((fd = accept(sfd, NULL, NULL))) {
        check(fd >= 0, "Couldn't accept connection: %s\n", strerror(errno));

        fprintf(stderr, "Received request:\n");

        // Store the last 4 bytes as \r\n\r\n means the request has finished
        char last_bytes[5] = "\0\0\0\0";
        for (;;) {
            char buf[bufsize];
            ssize_t bytes_read = read(fd, buf, bufsize);
            fwrite(buf, bytes_read, 1, stderr);
            if (bytes_read < 0) {
                fprintf(stderr, "read failed!\n");
                abort();
            }
            else if (bytes_read == 0) {
            }
            else if (bytes_read < 4) {
                memmove(last_bytes, last_bytes + 4 - bytes_read, 4 - bytes_read);
                memcpy(last_bytes + 4 - bytes_read, buf, bytes_read);
            }
            else {
                memcpy(last_bytes, buf + bytes_read - 4, 4);
            }
            if (memcmp(last_bytes, "\r\n\r\n", 4) == 0) {
                fprintf(stderr, "All done!\n");
                // Finished receiving HTTP request
                break;
            }
        }
        off_t offset = 0;
        while (offset < file_size) {
            check(portable_sendfile(fd, infile, &offset, 1024*1024) >= 0, "Sendfile failed\n");
            fprintf(stderr, "Written %d/%d bytes\n", (int)offset, file_size);
        }
        requests_served++;
        close(fd);
    }
    return 0;
}

