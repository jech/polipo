/*
Copyright (c) 2003 by Juliusz Chroboczek

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

typedef struct _ForbiddenDomain {
    int length;
    char domain[1];
} ForbiddenDomainRec, *ForbiddenDomainPtr;

AtomPtr forbiddenFile = NULL;
AtomPtr forbiddenUrl = NULL;
int forbiddenRedirectCode = 302;

AtomPtr redirector = NULL;
int redirectorRedirectCode = 302;

ForbiddenDomainPtr *forbiddenDomains;
int have_forbiddenDomains = 0;
int have_forbiddenRegex = 0;
static regex_t forbiddenRegex;

static char *regex;
static int rlen, rsize, dlen, dsize;

static pid_t redirector_pid = 0;
static int redirector_read_fd = -1, redirector_write_fd = -1;
static char redirector_buffer[512];
RedirectRequestPtr redirector_request_first = NULL, 
    redirector_request_last = NULL;

static int atomSetterForbidden(ConfigVariablePtr, void*);

void
preinitForbidden(void)
{
    CONFIG_VARIABLE_SETTABLE(forbiddenUrl, CONFIG_ATOM, configAtomSetter,
                             "URL to which forbidden requests "
                             "should be redirected.");
    CONFIG_VARIABLE_SETTABLE(forbiddenRedirectCode, CONFIG_INT,
                             configIntSetter,
                             "Redirect code, 301 or 302.");
    CONFIG_VARIABLE_SETTABLE(forbiddenFile, CONFIG_ATOM, atomSetterForbidden,
                             "File specifying forbidden URLs.");
    CONFIG_VARIABLE(redirector, CONFIG_ATOM, "Squid-style redirector.");
    CONFIG_VARIABLE(redirectorRedirectCode, CONFIG_INT,
                    "Redirect code to use with redirector.");
}

static int
atomSetterForbidden(ConfigVariablePtr var, void *value)
{
    initForbidden();
    return configAtomSetter(var, value);
}

int
readForbiddenFile(char *filename)
{
    FILE *in;
    char buf[512];
    char *rs;
    int i, j, is_regex, start;

    in = fopen(filename, "r");
    if(in == NULL) {
        if(errno != ENOENT)
            do_log_error(L_ERROR, errno, "Couldn't open forbidden file");
        return -1;
    }

    while(1) {
        rs = fgets(buf, 512, in);
        if(rs == NULL)
            break;
        for(i = 0; i < 512; i++) {
            if(buf[i] != ' ' && buf[i] != '\t')
                break;
        }
        start = i;
        for(i = start; i < 512; i++) {
            if(buf[i] == '#' || buf[i] == '\r' || buf[i] == '\n')
                break;
        }
        while(i > start) {
            if(buf[i - 1] != ' ' && buf[i - 1] != '\t')
                break;
            i--;
        }

        if(i <= start)
            continue;

        /* The significant part of the line is now between start and i */

        is_regex = 0;
        for(j = start; j < i; j++) {
            if(buf[j] == '\\' || buf[j] == '*' || buf[j] == '/') {
                is_regex = 1;
                break;
            }
        }

        if(is_regex) {
            while(rlen + i - start + 8 >= rsize) {
                char *new_regex;
                new_regex = realloc(regex, rsize * 2 + 1);
                if(new_regex == NULL) {
                    do_log(L_ERROR, "Couldn't allocate forbidden regex.\n");
                    fclose(in);
                    return -1;
                }
                regex = new_regex;
                rsize = rsize * 2 + 1;
            }
            if(rlen != 0)
                rlen = snnprintf(regex, rlen, rsize, "|");
            rlen = snnprintf(regex, rlen, rsize, "(");
            rlen = snnprint_n(regex, rlen, rsize, buf + start, i - start);
            rlen = snnprintf(regex, rlen, rsize, ")");
        } else {
            ForbiddenDomainPtr new_domain;
            if(dlen >= dsize - 1) {
                ForbiddenDomainPtr *new_domains;
                new_domains = realloc(forbiddenDomains, (dsize * 2 + 1) * 
                                      sizeof(ForbiddenDomainPtr));
                if(new_domains == NULL) {
                    do_log(L_ERROR, 
                           "Couldn't reallocate forbidden domains.\n");
                    fclose(in);
                    return -1;
                }
                forbiddenDomains = new_domains;
                dsize = dsize * 2 + 1;
            }
            new_domain = malloc(sizeof(ForbiddenDomainRec) - 1 + i - start);
            if(new_domain == NULL) {
                do_log(L_ERROR, "Couldn't allocate forbidden domain.\n");
                fclose(in);
                return -1;
            }
            new_domain->length = i - start;
            memcpy(new_domain->domain, buf + start, i - start);
            forbiddenDomains[dlen++] = new_domain;
        }
    }
    fclose(in);
    return 1;
}

void
initForbidden(void)
{
    int rc;
    struct stat ss;

    if(forbiddenFile)
        forbiddenFile = expandTilde(forbiddenFile);

    if(forbiddenFile == NULL) {
        forbiddenFile = expandTilde(internAtom("~/.polipo-forbidden"));
        if(forbiddenFile) {
            if(access(forbiddenFile->string, F_OK) < 0) {
                releaseAtom(forbiddenFile);
                forbiddenFile = NULL;
            }
        }
    }

    if(forbiddenFile == NULL) {
        if(access("/etc/polipo/forbidden", F_OK) >= 0)
            forbiddenFile = internAtom("/etc/polipo/forbidden");
    }

    if(have_forbiddenDomains) {
        ForbiddenDomainPtr *domain = forbiddenDomains;
        while(*domain) {
            free(*domain);
            domain++;
        }
        free(forbiddenDomains);
        have_forbiddenDomains = 0;
    }

    if(have_forbiddenRegex) {
        regfree(&forbiddenRegex);
        have_forbiddenRegex = 0;
    }

    if(!forbiddenFile || forbiddenFile->length == 0)
        return;

    forbiddenDomains = malloc(64 * sizeof(ForbiddenDomainPtr));
    if(forbiddenDomains == NULL) {
        do_log(L_ERROR, "Couldn't allocate forbidden domains.\n");
        return;
    }
    dlen = 0;
    dsize = 64;

    regex = malloc(512);
    if(regex == NULL) {
        do_log(L_ERROR, "Couldn't allocate forbidden regex.\n");
        free(forbiddenDomains);
        forbiddenDomains = NULL;
        return;
    }
    rlen = 0;
    rsize = 512;

    rc = stat(forbiddenFile->string, &ss);
    if(rc < 0) {
        if(errno != ENOENT)
            do_log_error(L_WARN, errno, "Couldn't stat forbidden file");
    } else {
        if(!S_ISDIR(ss.st_mode))
            readForbiddenFile(forbiddenFile->string);
        else {
            char *fts_argv[2];
            FTS *fts;
            FTSENT *fe;
            fts_argv[0] = forbiddenFile->string;
            fts_argv[1] = NULL;
            fts = fts_open(fts_argv, FTS_LOGICAL, NULL);
            if(fts) {
                while(1) {
                    fe = fts_read(fts);
                    if(!fe) break;
                    if(fe->fts_info != FTS_D && fe->fts_info != FTS_DP &&
                       fe->fts_info != FTS_DC && fe->fts_info != FTS_DNR)
                        readForbiddenFile(fe->fts_accpath);
                }
                fts_close(fts);
            } else {
                do_log_error(L_ERROR, errno,
                             "Couldn't scan forbidden directory");
            }
        }
    }

    if(dlen > 0) {
        forbiddenDomains[dlen] = NULL;
        have_forbiddenDomains = 1;
    } else {
        free(forbiddenDomains);
        forbiddenDomains = NULL;
    }

    if(rlen > 0) {
        rc = regcomp(&forbiddenRegex, regex, REG_EXTENDED | REG_NOSUB);
        if(rc != 0) {
            do_log(L_ERROR, "Couldn't compile forbidden regex: %d.\n", rc);
        } else {
            have_forbiddenRegex = 1;
        }
    }
    free(regex);
    return;
}

int
urlIsForbidden(AtomPtr url)
{
    if(url->length < 8)
        return 0;

    if(memcmp(url->string, "http://", 7) != 0)
        return 0;

    if(have_forbiddenDomains) {
        int i;
        ForbiddenDomainPtr *domain;
        for(i = 8; i < url->length; i++) {
            if(url->string[i] == '/')
                break;
        }
        domain = forbiddenDomains;
        while(*domain) {
            if((*domain)->length <= (i - 7) &&
               (url->string[i - (*domain)->length - 1] == '.' ||
                url->string[i - (*domain)->length - 1] == '/') &&
               memcmp(url->string + i - (*domain)->length,
                      (*domain)->domain, 
                      (*domain)->length) == 0)
                return 1;
            domain++;
        }
    }
    if(have_forbiddenRegex) {
        if(!regexec(&forbiddenRegex, url->string, 0, NULL, 0))
            return 1;
    }
    return 0;
}

static char lf[1] = "\n";

int
urlForbidden(AtomPtr url,
             int (*handler)(int, AtomPtr, AtomPtr, AtomPtr, void*),
             void *closure)
{
    int forbidden = urlIsForbidden(url);
    int code = 0;
    AtomPtr message = NULL, headers = NULL;


    if(forbidden) {
        message = internAtomF("Forbidden URL %s", url->string);
        if(forbiddenUrl) {
            code = forbiddenRedirectCode;
            headers = internAtomF("\r\nLocation: %s", forbiddenUrl->string);
        } else {
            code = 403;
        }
    }

    if(code == 0 && redirector) {
        RedirectRequestPtr request;
        request = malloc(sizeof(RedirectRequestRec));
        if(request == NULL) {
            do_log(L_ERROR, "Couldn't allocate redirect request.\n");
            goto done;
        }
        request->url = url;
        request->handler = handler;
        request->data = closure;
        if(redirector_request_first == NULL)
            redirector_request_first = request;
        else
            redirector_request_last->next = request;
        redirector_request_last = request;
        request->next = NULL;
        if(request == redirector_request_first)
            redirectorTrigger();
        return 1;
    }


 done:
    handler(code, url, message, headers, closure);
    return 1;
}

void
redirectorTrigger(void)
{
    RedirectRequestPtr request = redirector_request_first;
    int rc;

    if(!request)
        return;

    if(redirector_read_fd < 0) {
        rc = runRedirector(&redirector_pid,
                           &redirector_read_fd, &redirector_write_fd);
        if(rc < 0) {
            do_log_error(L_ERROR, -rc, "Couldn't run redirector");
            request->handler(-rc, request->url, NULL, NULL, request->data);
            redirector_request_first = request->next;
            if(redirector_request_first == NULL)
                redirector_request_last = NULL;
            free(request);
            return;
        }
    }
    do_stream_2(IO_WRITE, redirector_write_fd, 0,
                request->url->string, request->url->length,
                lf, 1,
                redirectorStreamHandler1, request);
}


int
redirectorStreamHandler1(int status,
                         FdEventHandlerPtr event,
                         StreamRequestPtr srequest)
{
    RedirectRequestPtr request = (RedirectRequestPtr)srequest->data;
    int rc;

    if(status < 0) {
        do_log_error(L_ERROR, -status, "Write to redirector failed");
        request->handler(status, request->url, NULL, NULL, request->data);
        free(request);
        close(redirector_read_fd);
        redirector_read_fd = -1;
        close(redirector_write_fd);
        redirector_write_fd = -1;
        kill(redirector_pid, SIGTERM);
        rc = waitpid(redirector_pid, &status, 0);
        if(rc < 0) {
            do_log_error(L_ERROR, errno, "Couldn't wait for redirector");
        }
        redirector_pid = -1;
        return 1;
    }

    if(!streamRequestDone(srequest))
        return 0;

    do_stream(IO_READ, redirector_read_fd, 0,
              redirector_buffer, 512,
              redirectorStreamHandler2, request);
    return 1;
}

int
redirectorStreamHandler2(int status,
                         FdEventHandlerPtr event,
                         StreamRequestPtr srequest)
{
    RedirectRequestPtr request = (RedirectRequestPtr)srequest->data;
    char *c;
    AtomPtr message;
    AtomPtr headers;
    int rc, code;

    if(status < 0) {
        do_log_error(L_ERROR, -status, "Read from redirector failed");
        request->handler(status, request->url, NULL, NULL, request->data);
        goto kill;
    }
    c = memchr(redirector_buffer, '\n', srequest->offset);
    if(!c) {
        if(!status && c < redirector_buffer + 512)
            return 0;
        do_log_error(L_ERROR, errno,
                     "Redirector returned incomplete reply.\n");
        request->handler(-EUNKNOWN, request->url, NULL, NULL, request->data);
        goto kill;
    }
    *c = '\0';

    if(srequest->offset > c + 1 - redirector_buffer)
        do_log(L_WARN, "Stray bytes in redirector output.\n");

    if(c > redirector_buffer + 1) {
        code = redirectorRedirectCode;
        message = internAtom("Redirected by external redirector");
        if(message == NULL) {
            request->handler(-ENOMEM, request->url, NULL, NULL, request->data);
            goto fail;
        }

        headers = internAtomF("\r\nLocation: %s", redirector_buffer);
        if(headers == NULL) {
            releaseAtom(message);
            request->handler(-ENOMEM, request->url, NULL, NULL, request->data);
            goto fail;
        }
    } else {
        code = 0;
        message = NULL;
        headers = NULL;
    }
    request->handler(code, request->url,
                     message, headers, request->data);
    goto cont;
 kill:
    close(redirector_read_fd);
    redirector_read_fd = -1;
    close(redirector_write_fd);
    redirector_write_fd = -1;
    kill(redirector_pid, SIGTERM);
    rc = waitpid(redirector_pid, &status, 0);
    if(rc < 0)
        do_log_error(L_ERROR, errno, "Couldn't wait for redirector");
    redirector_pid = 0;
 fail:
    request->handler(-ENOMEM, request->url, NULL, NULL, request->data);
 cont:
    assert(redirector_request_first == request);
    redirector_request_first = request->next;
    if(redirector_request_first == NULL)
        redirector_request_last = NULL;
    free(request);
    redirectorTrigger();
    return 1;
}
            
int
runRedirector(pid_t *pid_return, int *read_fd_return, int *write_fd_return)
{
    int rc;
    pid_t pid;
    int filedes1[2], filedes2[2];
    sigset_t ss, old_mask;

    assert(redirector);

    rc = pipe(filedes1);
    if(rc < 0)
        return -errno;

    rc = pipe(filedes2);
    if(rc < 0) {
        close(filedes1[0]);
        close(filedes1[1]);
        return -errno;
    }

    fflush(stdout);
    fflush(stderr);
    fflush(logF);

    interestingSignals(&ss);
    do {
        rc = sigprocmask(SIG_BLOCK, &ss, &old_mask);
    } while (rc < 0 && errno == EINTR);
    if(rc < 0)
        return -errno;
    
    pid = fork();
    if(pid < 0)
        return -errno;

    if(pid > 0) {
        close(filedes1[0]);
        close(filedes2[1]);
        do {
            rc = sigprocmask(SIG_SETMASK, &old_mask, NULL);
        } while(rc < 0 && errno == EINTR);

        if(rc < 0) {
            rc = errno;
            close(filedes1[1]);
            close(filedes2[0]);
            return -rc;
        }
        rc = setNonblocking(filedes1[1], 1);
        if(rc >= 0)
            rc = setNonblocking(filedes2[0], 1);
        if(rc < 0) {
            rc = errno;
            close(filedes1[1]);
            close(filedes2[0]);
            return -rc;
        }
        *read_fd_return = filedes2[0];
        *write_fd_return = filedes1[1];
        *pid_return = pid;
    } else {
        close(filedes1[1]);
        close(filedes2[0]);
        uninitEvents();
        do {
            rc = sigprocmask(SIG_SETMASK, &old_mask, NULL);
        } while (rc < 0 && errno == EINTR);
        if(rc < 0)
            exit(1);

        if(filedes1[0] != 0)
            dup2(filedes1[0], 0);
        if(filedes2[1] != 1)
            dup2(filedes2[1], 1);

        execlp(redirector->string, redirector->string, NULL);
        exit(1);
        /* NOTREACHED */
    }
    return 1;
}
