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

int logLevel = LOGGING_DEFAULT;
AtomPtr logFile = NULL;
FILE *logF;

#define STR(x) XSTR(x)
#define XSTR(x) #x

void
preinitLog()
{
    CONFIG_VARIABLE_SETTABLE(logLevel, CONFIG_HEX, configIntSetter,
                             "Logging level (max = " STR(LOGGING_MAX) ").");
    CONFIG_VARIABLE(logFile, CONFIG_ATOM, "Log file (stderr if empty).");
    logF = stderr;
}

void
initLog()
{
    if(daemonise && logFile == NULL)
        logFile = internAtom("/var/log/polipo");
        
    if(logFile != NULL && logFile->length > 0) {
        FILE *f;
        f = fopen(logFile->string, "a");
        if(f == NULL) {
            do_log_error(L_ERROR, errno, "Couldn't open log file %s",
                         logFile->string);
            exit(1);
        }
        setvbuf(f, NULL, _IOLBF, 0);
        logF = f;
    }
}

void
reopenLog()
{
    if(logFile) {
        FILE *f;
        f = fopen(logFile->string, "a");
        if(f == NULL) {
            do_log_error(L_ERROR, errno, "Couldn't reopen log file %s",
                         logFile->string);
            exit(1);
        }
        setvbuf(f, NULL, _IOLBF, 0);
        fclose(logF);
        logF = f;
    }
}

void
really_do_log(int type, const char *f, ...)
{
    va_list args;

    va_start(args, f);
    if(type & LOGGING_MAX & logLevel)
        really_do_log_v(type, f, args);
    va_end(args);
}

void
really_do_log_v(int type, const char *f, va_list args)
{
    if(type & LOGGING_MAX & logLevel)
        vfprintf(logF, f, args);
}

void 
really_do_log_error(int type, int e, const char *f, ...)
{
    va_list args;
    va_start(args, f);
    if(type & LOGGING_MAX & logLevel)
        really_do_log_error_v(type, e, f, args);
    va_end(args);
}

void
really_do_log_error_v(int type, int e, const char *f, va_list args)
{
    if((type & LOGGING_MAX & logLevel) != 0) {
        char *es = pstrerror(e);
        if(es == NULL)
            es = "Unknown error";
        vfprintf(logF, f, args);
        fprintf(logF, ": %s\n", es);
    }
}

void
really_do_log_n(int type, const char *s, int n)
{
    if((type & LOGGING_MAX & logLevel) != 0) {
        fwrite(s, n, 1, logF);
    }
}
