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

ForbiddenDomainPtr *forbiddenDomains;
int have_forbiddenDomains = 0;
int have_forbiddenRegex = 0;
static regex_t forbiddenRegex;

static char *regex;
static int rlen, rsize, dlen, dsize;

void
preinitForbidden(void)
{
    CONFIG_VARIABLE(forbiddenFile, CONFIG_ATOM,
                    "File specifying forbidden URLs.");
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
urlForbidden(char *url, int url_size)
{
    char url_copy[1024];

    if(url_size >= 1024) return 1;
    if(url_size < 9) return 0;
    if(memcmp(url, "http://", 7) != 0)
        return 0;

    if(have_forbiddenDomains) {
        int i;
        ForbiddenDomainPtr *domain;
        for(i = 8; i < url_size; i++) {
            if(url[i] == '/')
                break;
        }
        domain = forbiddenDomains;
        while(*domain) {
            if((*domain)->length <= (i - 7) &&
               (url[i - (*domain)->length - 1] == '.' ||
                url[i - (*domain)->length - 1] == '/') &&
               memcmp(url + i - (*domain)->length,
                      (*domain)->domain, 
                      (*domain)->length) == 0)
                return 1;
            domain++;
        }
    }
    if(have_forbiddenRegex) {
        memcpy(url_copy, url, url_size);
        url_copy[url_size] = '\0';
        if(!regexec(&forbiddenRegex, url_copy, 0, NULL, 0))
            return 1;
    }
    return 0;
}
