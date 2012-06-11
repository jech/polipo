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
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef O_CLOEXEC
// Old versions of uclibc don't have O_CLOEXEC defined even if the underlying
// kernel supports it:
#define O_CLOEXEC 0
#endif

static const char* USAGE =
    "Usage: %s [options] program [args...]\n"
    "\n"
    "Spawns opens a random socket on localhost for listening and fork-execs the\n"
    "given program with the appropriate systemd environment variables set.  Then\n"
    "prints the pid and port to\n"
    "STDOUT in the form:\n"
    "    LAUNCHED_PID=1234\n"
    "    LAUNCHED_PORT=54321\n"
    "suitable for shell evaluation\n"
    "\n"
    "Options:\n"
    "    -h, --help               Display this help message.\n"
    "    -l, --logfile            The STDIN/STDERR of the spawned instance will\n"
    "                             be redirected to this file.\n"
    "    -e, --stderr             The STDERR of the spawned instance will be \n"
    "                             redirected to this file.\n"
    "\n"
    "Typical use:\n"
    "    $ export `sd-launch polipo`\n";

static const char* ADDR = "127.0.0.1";
static const char* DEFAULT_LOGFILE = "/dev/null";

struct options_t
{
    const char* logfile;
    const char* stderr;
};

char** parse_args(struct options_t* opts, int argc, char** argv);
static pid_t sd_spawn(int* port, int stdout_fd, int stderr_fd, char** argv);

enum log_level_t
{
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3,
    LOG_LEVEL_MAX = DEBUG
};

const char* enum_to_string(enum log_level_t log_level)
{
    switch(log_level)
    {
        case ERROR: return "ERROR";
        case WARN: return "WARN";
        case INFO: return "INFO";
        case DEBUG: return "DEBUG";
        default: abort();
    };
}

static enum log_level_t log_verbosity = WARN;

void report(enum log_level_t log_level, const char* fmt, ...)
{
    assert(log_level >= ERROR && log_level <= LOG_LEVEL_MAX && "Invalid value of log_level.");

    if(log_level <= log_verbosity)
    {
        fprintf(stderr, "%s: ", enum_to_string(log_level));

        va_list args;
        va_start(args, fmt);

        vfprintf(stderr, fmt, args);

        va_end(args);
    };
}

// Implementation note: This is a very short-lived program -- it either execs
// another application, or aborts. So for the sake of clarity we don't care
// at all about cleaning up resources.
int main(int argc, char** argv)
{
    struct options_t opts;
    int port;
    int logfile_fd;
    int stderr_fd;

    const char** program_argv = parse_args(&opts, argc, argv);

    // O_CLOEXEC will be removed when we dup2 these fds into position below.
    if ((logfile_fd = open(opts.logfile, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 00640)) == -1)
    {
        report(ERROR, "Opening log file \"%s\" failed: %s\n",
               opts.logfile, strerror(errno));
        exit(1);
    }
    if (opts.stderr)
    {
        if ((stderr_fd = open(opts.stderr, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 00640)) == -1)
        {
            report(ERROR, "Opening stderr log file \"%s\" failed: %s\n",
                   opts.logfile, strerror(errno));
            exit(1);
        }
    }
    else
    {
        stderr_fd = logfile_fd;
    }
    pid_t pid = sd_spawn(&port, logfile_fd, stderr_fd, program_argv);
    printf("LAUNCHED_PID=%d\n"
           "LAUNCHED_PORT=%d\n", pid, port);
    return 0;
}

char* usage(const char* argv0) {
    char* buf;
    asprintf(&buf, USAGE, argv0);
    return buf;
}

// If arguments in argv are invalid, prints error message and exits.
// Otherwise populates opts with the appropriate values.
char** parse_args(struct options_t* opts, int argc, char** argv)
{
    // Defaults:
    opts->logfile = DEFAULT_LOGFILE;
    opts->stderr = NULL;

    static struct option long_options[] =
    {
        {"help", no_argument, NULL, 'h'},
        {"logfile", required_argument, NULL, 'l'},
        {"stderr", required_argument, NULL, 'e'},
        {NULL, 0, NULL, 0}
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "+hl:e:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 'h': // -h, --help
                printf(usage(argv[0]));
                exit(0);
                break;
            case 'l': // -l, --logfile
                opts->logfile = strdup(optarg);
                break;
            case 'e': // -e, --stderr
                opts->stderr = strdup(optarg);
                break;
            default:
                fprintf(stderr, usage(argv[0]));
                exit(1);
        }
    }

    // are we missing required exec argument?
    if ((argc - optind) == 0)
    {
        fprintf(stderr, usage(argv[0]));
        exit(1);
    }

    return argv + optind;
}

void checked_dup2(oldfd, newfd) {
    if (oldfd < 0) {
        fprintf(stderr, "Bad source fd");
        exit(1);
    }
    close(newfd);
    int fd = dup2(oldfd, newfd);
    if (fd != newfd) {
        fprintf(stderr, "dup2 failed: %s", strerror(errno));
        exit(1);
    }
}

int open_listening_socket(int* port)
{
    // make sure we make space for passed file descriptor
    int sfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sfd == -1)
    {
        report(ERROR, "Failed to create socket.\n");
        abort();
    }

    int yes = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    {
        report(WARN, "Couldn't set option SO_REUSEADDR on socket.\n");
    }

    // fill sockaddr_in structure
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    if (inet_aton(ADDR, &addr.sin_addr) < 0)
    {
        assert(0 && "Failed to parse address.");
    }

    addr.sin_port = 0;

    // bind socket
    if (bind(sfd, (struct sockaddr*) &addr, sizeof(addr)) == -1)
    {
        report(ERROR, "Failed to bind polipo socket.\n");
        abort();
    }

    // get binded port number
    socklen_t addrlen = sizeof(addr);
    getsockname(sfd, (struct sockaddr*) &addr, &addrlen);

    *port = ntohs(addr.sin_port);
    report(DEBUG, "Listening socket set up on port %d.\n", *port);

    if (listen(sfd, 32) < 0)
    {
        report(ERROR, "Couldn't set up listen on socket.\n");
        abort();
    }
    return sfd;
}

// Spawn a process with stderr and stdout redirected to log_fd and fd 3 as
// a listening socket with LISTEN_FDS and LISTEN_PID set a la systemd.
//
// Aborts the whole program if an error occurs.
//
// @return Pid of process.
// @param[out] port: port number.
pid_t sd_spawn(int* port, int stdout_fd, int stderr_fd, char** launch_argv)
{
    // create listening socket
    int sfd = open_listening_socket(port);

    report(INFO, "Starting %s on %s:%d.\n", launch_argv[0], ADDR, *port);

    // spawn polipo
    pid_t pid = fork();

    // parent
    if (pid != 0) {
        return pid;
    }
    else {
        // child

        // This has to be done after fork() call because systemd requires
        // LISTEN_PID to be the same as current process id.
        char* new_pid;
        asprintf(&new_pid, "%d", getpid());
        if (setenv("LISTEN_FDS", "1", 1) == -1 || setenv("LISTEN_PID", new_pid, 1) == -1)
        {
            report(ERROR, "Failed to set LISTEN_FDS and LISTEN_PID environment variables.\n");
            abort();
        }

        // Remap the fd numbers appropriately
        checked_dup2(open("/dev/null", O_RDONLY), 0);
        checked_dup2(stdout_fd, 1);
        checked_dup2(stderr_fd, 2);
        checked_dup2(sfd, 3);
        close(4);
        close(5);

        execvp(launch_argv[0], launch_argv);

        // If we reach this point, the exec failed.
        int exec_errno = errno;
        fprintf(stderr, "Exec failed for command \"%s\" in pid %d - %s.\n",
                launch_argv[0], getpid(), strerror(exec_errno));
        abort();
    }
}

