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

int disableLocalInterface = 0;

void
preinitLocal()
{
    CONFIG_VARIABLE(disableLocalInterface, CONFIG_BOOLEAN,
                    "Disable the local configuration pages.");
}

static void fillSpecialObject(ObjectPtr, void (*)(char*), void*);

int 
httpLocalRequest(ObjectPtr object, int method, int from, int to,
                 HTTPRequestPtr requestor, void *closure)
{
    if(object->requestor == NULL)
        object->requestor = requestor;

    if(urlIsSpecial(object->key, object->key_size))
        return httpSpecialRequest(object, method, from, to, 
                                  requestor, closure);
    /* objectFillFromDisk already did the real work but we have to
       make sure we don't get into an infinite loop. */
    if(object->flags & OBJECT_INITIAL) {
        abortObject(object, 404, internAtom("Not found"));
    }
    object->age = current_time.tv_sec;
    object->date = current_time.tv_sec;

    object->flags &= ~OBJECT_VALIDATING;
    notifyObject(object);
    return 1;
}

static void
printConfig(char *dummy)
{
    printf("<!DOCTYPE HTML PUBLIC "
           "\"-//W3C//DTD HTML 4.01 Transitional//EN\" "
           "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
           "<html><head>\n"
           "<title>Polipo configuration</title>\n"
           "</head><body>\n"
           "<h1>Polipo configuration</h1>\n");
    printConfigVariables(stdout, 1);
    printf("<p><a href=\"/polipo/\">back</a></p>");
    printf("</body></html>\n");
}

#ifndef NO_DISK_CACHE

static void
recursiveIndexDiskObjects(char *root)
{
    indexDiskObjects(root, 1);
}

static void
plainIndexDiskObjects(char *root)
{
    indexDiskObjects(root, 0);
}
#endif

static void
serversList(char *dummy)
{
    listServers();
}

static int
matchUrl(char *base, ObjectPtr object)
{
    int n = strlen(base);
    if(object->key_size < n)
        return 0;
    if(memcmp(base, object->key, n) != 0)
        return 0;
    return (object->key_size == n) || (((char*)object->key)[n] == '?');
}
    
int 
httpSpecialRequest(ObjectPtr object, int method, int from, int to,
                   HTTPRequestPtr requestor, void *closure)
{
    char buffer[1024];
    int hlen;

    if(!(object->flags & OBJECT_INITIAL)) {
        privatiseObject(object, 0);
        supersedeObject(object);
        object->flags &= ~(OBJECT_VALIDATING | OBJECT_INPROGRESS);
        notifyObject(object);
        return 1;
    }

    hlen = snnprintf(buffer, 0, 1024,
                     "\r\nServer: polipo"
                     "\r\nContent-Type: text/html");
    object->date = current_time.tv_sec;
    object->age = current_time.tv_sec;
    object->headers = internAtomN(buffer, hlen);
    object->code = 200;
    object->message = internAtom("Okay");
    object->flags &= ~OBJECT_INITIAL;
    object->flags |= OBJECT_DYNAMIC;

    if(object->key_size == 8 && memcmp(object->key, "/polipo/", 8) == 0) {
        objectPrintf(object, 0,
                     "<!DOCTYPE HTML PUBLIC "
                     "\"-//W3C//DTD HTML 4.01 Transitional//EN\" "
                     "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
                     "<html><head>\n"
                     "<title>Polipo</title>\n"
                     "</head><body>\n"
                     "<h1>Polipo</h1>\n"
                     "<p><a href=\"status?\">Status report</a>.</p>\n"
                     "<p><a href=\"config?\">Current configuration</a>.</p>\n"
                     "<p><a href=\"servers?\">Known servers</a>.</p>\n"
#ifndef NO_DISK_CACHE
                     "<p><a href=\"index?\">Disk cache index</a>.</p>\n"
#endif
                     "</body></html>\n");
        object->length = object->size;
    } else if(matchUrl("/polipo/status", object)) {
        objectPrintf(object, 0,
                     "<!DOCTYPE HTML PUBLIC "
                     "\"-//W3C//DTD HTML 4.01 Transitional//EN\" "
                     "\"http://www.w3.org/TR/html4/loose.dtd\">\n"
                     "<html><head>\n"
                     "<title>Polipo status report</title>\n"
                     "</head><body>\n"
                     "<h1>Polipo proxy on %s:%d: status report</h1>\n"
                     "<p>The %s proxy on %s:%d is %s.</p>\n"
                     "<p>There are %d public and %d private objects "
                     "currently in memory using %d KB in %d chunks.</p>\n"
                     "<p>There are %d atoms.</p>"
                     "<p><a href=\"/polipo/\">back</a></p>"
                     "</body></html>\n",
                     proxyName->string, proxyPort,
                     cacheIsShared ? "shared" : "private",
                     proxyName->string, proxyPort,
                     proxyOffline ? "off line" :
                     (relaxTransparency ? 
                      "on line (transparency relaxed)" :
                      "on line"),
                     publicObjectCount, privateObjectCount,
                     used_chunks * CHUNK_SIZE / 1024, used_chunks,
                     used_atoms);
        object->expires = current_time.tv_sec;
        object->length = object->size;
    } else if(matchUrl("/polipo/config", object)) {
        fillSpecialObject(object, printConfig, NULL);
        object->expires = current_time.tv_sec + 5;
#ifndef NO_DISK_CACHE
    } else if(matchUrl("/polipo/index", object)) {
        int len = MAX(0, object->key_size - 14);
        char *root = strdup_n((char*)object->key + 14, len);
        if(root == NULL) {
            abortObject(object, 503, internAtom("Couldn't allocate root"));
            notifyObject(object);
            return 1;
        }
        writeoutObjects(1);
        fillSpecialObject(object, plainIndexDiskObjects, root);
        free(root);
        object->expires = current_time.tv_sec + 5;
    } else if(matchUrl("/polipo/recursive-index", object)) {
        int len = MAX(0, object->key_size - 24);
        char *root = strdup_n((char*)object->key + 24, len);
        if(root == NULL) {
            abortObject(object, 503, internAtom("Couldn't allocate root"));
            notifyObject(object);
            return 1;
        }
        writeoutObjects(1);
        fillSpecialObject(object, recursiveIndexDiskObjects, root);
        free(root);
        object->expires = current_time.tv_sec + 20;
#endif
    } else if(matchUrl("/polipo/servers", object)) {
        fillSpecialObject(object, serversList, NULL);
        object->expires = current_time.tv_sec + 2;
    } else {
        abortObject(object, 404, internAtom("Not found"));
    }

    object->flags &= ~OBJECT_VALIDATING;
    notifyObject(object);
    return 1;
}

static void
fillSpecialObject(ObjectPtr object, void (*fn)(char*), void* closure)
{
    int rc;
    int filedes[2];
    pid_t pid;
    sigset_t ss, old_mask;

    if(object->flags & OBJECT_INPROGRESS)
        return;

    if(disableLocalInterface) {
        abortObject(object, 403, internAtom("Local configuration disabled"));
        return;
    }

    rc = pipe(filedes);
    if(rc < 0) {
        do_log_error(L_ERROR, errno, "Couldn't create pipe");
        abortObject(object, 503,
                    internAtomError(errno, "Couldn't create pipe"));
        return;
    }

    fflush(stdout);
    fflush(stderr);
    fflush(logF);

    /* Block signals that we handle specially until the child can
       disable the handlers. */
    interestingSignals(&ss);
    /* I'm a little confused.  POSIX doesn't allow EINTR here, but I
       think that both Linux and SVR4 do. */
    do {
        rc = sigprocmask(SIG_BLOCK, &ss, &old_mask);
    } while (rc < 0 && errno == EINTR);
    if(rc < 0) {
        do_log_error(L_ERROR, errno, "Sigprocmask failed");
        abortObject(object, 503, internAtomError(errno, "Sigprocmask failed"));
        close(filedes[0]);
        close(filedes[1]);
        return;
    }
    
    pid = fork();
    if(pid < 0) {
        do_log_error(L_ERROR, errno, "Couldn't fork");
        abortObject(object, 503, internAtomError(errno, "Couldn't fork"));
        close(filedes[0]);
        close(filedes[1]);
        do {
            rc = sigprocmask(SIG_SETMASK, &old_mask, NULL);
        } while (rc < 0 && errno == EINTR);
        if(rc < 0) {
            do_log_error(L_ERROR, errno, "Couldn't restore signal mask");
            polipoExit();
        }
        return;
    }

    if(pid > 0) {
        SpecialRequestPtr request;
        close(filedes[1]);
        do {
            rc = sigprocmask(SIG_SETMASK, &old_mask, NULL);
        } while (rc < 0 && errno == EINTR);
        if(rc < 0) {
            do_log_error(L_ERROR, errno, "Couldn't restore signal mask");
            polipoExit();
            return;
        }

        request = malloc(sizeof(SpecialRequestRec));
        if(request == NULL) {
            kill(pid, SIGTERM);
            close(filedes[0]);
            abortObject(object, 503,
                        internAtom("Couldn't allocate request\n"));
            notifyObject(object);
            /* specialRequestHandler will take care of the rest. */
        } else {
            request->buf = get_chunk();
            if(request->buf == NULL) {
                kill(pid, SIGTERM);
                close(filedes[0]);
                free(request);
                abortObject(object, 503,
                            internAtom("Couldn't allocate request\n"));
                notifyObject(object);
            }
        }
        object->flags |= OBJECT_INPROGRESS;
        retainObject(object);
        request->object = object;
        request->fd = filedes[0];
        request->pid = pid;
        request->offset = 0;
        /* Under any sensible scheduler, the child will run before the
           parent.  So no need for IO_NOTNOW. */
        do_stream(IO_READ, filedes[0], 0, request->buf, CHUNK_SIZE,
                  specialRequestHandler, request);
    } else {
        /* child */
        close(filedes[0]);
        uninitEvents();
        do {
            rc = sigprocmask(SIG_SETMASK, &old_mask, NULL);
        } while (rc < 0 && errno == EINTR);
        if(rc < 0)
            exit(1);

        if(filedes[1] != 1)
            dup2(filedes[1], 1);

        (*fn)(closure);
        exit(0);
    }
}

int
specialRequestHandler(int status, 
                      FdEventHandlerPtr event, StreamRequestPtr srequest)
{
    SpecialRequestPtr request = srequest->data;
    int rc;
    int killed = 0;

    if(status < 0) {
        kill(request->pid, SIGTERM);
        killed = 1;
        request->object->flags &= ~OBJECT_INPROGRESS;
        abortObject(request->object, 502,
                    internAtomError(-status, "Couldn't read from client"));
        goto done;
    }

    if(srequest->offset > 0) {
        rc = objectAddData(request->object, request->buf, 
                           request->offset, srequest->offset);
        if(rc < 0) {
            kill(request->pid, SIGTERM);
            killed = 1;
            request->object->flags &= ~OBJECT_INPROGRESS;
            abortObject(request->object, 503,
                        internAtom("Couldn't add data to connection"));
            goto done;
        }
        request->offset += srequest->offset;
    }
    if(status) {
        request->object->flags &= ~OBJECT_INPROGRESS;
        request->object->length = request->object->size;
        goto done;
    }

    /* If we're the only person interested in this object, let's abort
       it now. */
    if(request->object->refcount <= 1) {
        kill(request->pid, SIGTERM);
        killed = 1;
        request->object->flags &= ~OBJECT_INPROGRESS;
        abortObject(request->object, 500, internAtom("Aborted"));
        goto done;
    }
    notifyObject(request->object);
    do_stream(IO_READ | IO_NOTNOW, request->fd, 0, request->buf, CHUNK_SIZE,
              specialRequestHandler, request);
    return 1;

 done:
    close(request->fd);
    dispose_chunk(request->buf);
    releaseNotifyObject(request->object);
    /* That's a blocking wait.  It shouldn't block for long, as we've
       either already killed the child, or else we got EOF from it. */
    do {
        rc = waitpid(request->pid, &status, 0);
    } while(rc < 0 && errno == EINTR);
    if(rc < 0) {
        do_log(L_ERROR, "Wait for %d: %d\n", (int)request->pid, errno);
    } else {
        int normal = 
            (WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
            (killed && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        char *reason =
            WIFEXITED(status) ? "with status" : 
            WIFSIGNALED(status) ? "on signal" :
            "with unknown status";
        int value =
            WIFEXITED(status) ? WEXITSTATUS(status) :
            WIFSIGNALED(status) ? WTERMSIG(status) :
            status;
        do_log(normal ? D_CHILD : L_ERROR, 
               "Child %d exited %s %d.\n",
               (int)request->pid, reason, value);
    }
    free(request);
    return 1;
}
