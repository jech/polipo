/*
Copyright (c) 2003-2006 by Juliusz Chroboczek

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

AtomPtr socksParentProxy = NULL;
AtomPtr socksProxyHost = NULL;
int socksProxyPort = -1;
AtomPtr socksProxyAddress = NULL;
int socksProxyAddressIndex = -1;

static int socksParentProxySetter();
static int do_socks_connect_common();
static int socksDnsHandler();
static int socksConnectHandler();
static int socksWriteHandler();
static int socksReadHandler();

void
preinitSocks()
{
    CONFIG_VARIABLE_SETTABLE(socksParentProxy, CONFIG_ATOM_LOWER,
                             socksParentProxySetter,
                             "SOCKS4A parent proxy (host:port)");
}

static int
socksParentProxySetter(ConfigVariablePtr var, void *value)
{
    configAtomSetter(var, value);
    initSocks();
    return 1;
}

void
initSocks()
{
    int port = -1;
    AtomPtr host = NULL, port_atom;
    int rc;

    if(socksParentProxy) {
        rc = atomSplit(socksParentProxy, ':', &host, &port_atom);
        if(rc <= 0) {
            do_log(L_ERROR, "Couldn't parse socksParentProxy");
            return;
        }
        port = atoi(port_atom->string);
        releaseAtom(port_atom);
    }

    if(socksProxyHost)
        releaseAtom(socksProxyHost);
    socksProxyHost = host;
    socksProxyPort = port;
    if(socksProxyAddress)
        releaseAtom(socksProxyAddress);
    socksProxyAddress = NULL;
    socksProxyAddressIndex = -1;
}

static void
destroySocksRequest(SocksRequestPtr request)
{
    releaseAtom(request->name);
    if(request->buf)
        free(request->buf);
    free(request);
}

int
do_socks_connect(char *name, int port,
                 int (*handler)(int, SocksRequestPtr),
                 void *data)
{
    SocksRequestPtr request = malloc(sizeof(SocksRequestRec));
    SocksRequestRec request_nomem;
    if(request == NULL)
        goto nomem;

    request->name = internAtomLowerN(name, strlen(name));
    if(request->name == NULL) {
        free(request);
        goto nomem;
    }

    request->port = port;
    request->fd = -1;
    request->handler = handler;
    request->buf = NULL;
    request->data = data;

    if(socksProxyAddress == NULL) { 
        do_gethostbyname(socksProxyHost->string, 0,
                         socksDnsHandler,
                         request);
        return 1;
    }

    return do_socks_connect_common(request);

 nomem:
    request_nomem.name = internAtomLowerN(name, strlen(name));
    request_nomem.port = port;
    request_nomem.handler = handler;
    request_nomem.buf = NULL;
    request_nomem.data = data;

    handler(-ENOMEM, &request_nomem);
    releaseAtom(request_nomem.name);
    return 1;
}

static int
do_socks_connect_common(SocksRequestPtr request)
{
    assert(socksProxyAddressIndex >= 0);

    do_connect(retainAtom(socksProxyAddress),
               socksProxyAddressIndex,
               socksProxyPort,
               socksConnectHandler, request);
    return 1;
}

static int
socksDnsHandler(int status, GethostbynameRequestPtr grequest)
{
    SocksRequestPtr request = grequest->data;
    if(status <= 0) {
        request->handler(status, request);
        destroySocksRequest(request);
        return 1;
    }

    if(grequest->addr->string[0] == DNS_CNAME) {
        if(grequest->count > 10) {
            do_log(L_ERROR, "DNS CNAME loop.\n");
            request->handler(-EDNS_CNAME_LOOP, request);
            destroySocksRequest(request);
            return 1;
        }
        do_gethostbyname(grequest->addr->string + 1, grequest->count + 1,
                         httpServerConnectionDnsHandler, request);
        return 1;
    }


    socksProxyAddress = retainAtom(grequest->addr);
    socksProxyAddressIndex = 0;

    do_socks_connect_common(request);
    return 1;
}

static int
socksConnectHandler(int status,
                    FdEventHandlerPtr event,
                    ConnectRequestPtr crequest)
{
    SocksRequestPtr request = crequest->data;
    char *buf;
    int rc;

    if(status < 0) {
        request->handler(status, request);
        destroySocksRequest(request);
        return 1;
    }

    assert(request->fd < 0);
    request->fd = crequest->fd;
    socksProxyAddressIndex = crequest->index;

    rc = setNodelay(request->fd, 1);
    if(rc < 0)
        do_log_error(L_WARN, errno, "Couldn't disable Nagle's algorithm");

    request->buf = malloc(9 + request->name->length + 1);
    if(request->buf == NULL) {
        request->handler(-ENOMEM, request);
        destroySocksRequest(request);
        return 1;
    }

    buf = request->buf;

    buf[0] = 4;        /* VN */
    buf[1] = 1;        /* CD = REQUEST */
    buf[2] = (request->port >> 8) & 0xFF;
    buf[3] = request->port & 0xFF;
    buf[4] = buf[5] = buf[6] = 0;
    buf[7] = 3;
    buf[8] = '\0';

    memcpy(buf + 9, request->name->string, request->name->length);
    buf[9 + request->name->length] = '\0';

    do_stream(IO_WRITE, request->fd, 0, buf, 9 + request->name->length + 1,
              socksWriteHandler, request);
    return 1;
}

static int
socksWriteHandler(int status,
                  FdEventHandlerPtr event,
                  StreamRequestPtr srequest)
{
    SocksRequestPtr request = srequest->data;

    if(status) {
        if(status > 0)
            status = -ESOCKS;
        request->handler(status, request);
        destroySocksRequest(request);
        return 1;
    }

    if(!streamRequestDone(srequest))
        return 0;

    do_stream(IO_READ | IO_NOTNOW, request->fd, 0, request->buf, 8,
              socksReadHandler, request);
    return 1;
}

static int
socksReadHandler(int status,
                 FdEventHandlerPtr event,
                 StreamRequestPtr srequest)
{
    SocksRequestPtr request = srequest->data;

    if(status) {
        if(status > 0)
            status = -ESOCKS;
        goto error;
    }

    if(srequest->offset < 8)
        return 0;

    if(request->buf[0] != 0 || request->buf[1] != 90) {
        status = -ESOCKS;
        goto error;
    }

    request->handler(1, request);
    destroySocksRequest(request);
    return 1;

 error:
    request->handler(status, request);
    destroySocksRequest(request);
    return 1;
}
