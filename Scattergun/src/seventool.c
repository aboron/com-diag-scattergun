/* vi: set ts=4 expandtab shiftwidth=4: */
/**
 * @file
 * Seven Tool<BR>
 * Copyright 2016 Digital Aggregates Corporation, Colorado, USA.<BR>
 * "Digital Aggregates Corporation" is a registered trademark.<BR>
 * Licensed under the terms of the Scattergun license.<BR>
 * author:Chip Overclock<BR>
 * mailto:coverclock@diag.com<BR>
 * http://www.diag.com/nagivation/downloads/Scattergun.html<BR>
 * http://github.com/coverclock/com-diag-scattergun<BR>
 *
 * USAGE
 *
 * seventool [ -h ] [ -d ] [ -v ] [ -D ] [ -i IDENT ] [ -o PATH ]
 *
 * EXAMPLES
 *
 * ABSTRACT
 *
 * Continuously reads random words using the rdrand or rdseed instructions
 * available on various Intel processors such as certain models of the i7 and
 * writes it to standard output, or to a specified file system path. This
 * latter object could be a FIFO, which could allow generated entropy to be
 * read by another program, like rngd. Optionally does some other useful stuff
 * regarding examining the capabilities of the host processor. This is part of
 * the Scattergun project.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#define  __RDRND__
#include <immintrin.h>

static const char * program = "seventool";
static const char * ident = "seventool";
static int debug = 0;
static int verbose = 0;
static int done = 0;
static int report = 0;
static int daemonize = 0;

enum mode { FAIL=0, RDRAND=1, RDSEED=2, };
static const char * MODE[] = { "fail", "rdrand", "rdseed", };

/**
 * Emit a formatting string to either the system log or to standard error.
 * @param format is the printf format.
 */
static void lprintf(const char * format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (daemonize) {
        vsyslog(LOG_DEBUG, format, ap);
    } else {
        vfprintf(stderr, format, ap);
    }
    va_end(ap);
}

/**
 * Emit a caller provider string and an error message string corresponding to
 * the current value of the error number (errno) to either the system log or
 * to standard error.
 * @param string is the string.
 */
static void lerror(const char * string)
{
    if (daemonize) {
        syslog(LOG_ERR, "%s: %s", string, strerror(errno));
    } else {
        fprintf(stderr, "%s: %s", string, strerror(errno));
    }
}

/**
 * Handle a signal. In the event of a SIGPIPE or a SIGINT, the program shuts
 * down in an orderly fashion. In the event of a SIGHUP, it emits some
 * statistics to standard error.
 * @param signum is the number of the incoming signal.
 */
static void handler(int signum)
{
    if (signum == SIGPIPE) {
        done = !0;
    } else if (signum == SIGINT) {
        done = !0;
    } else if (signum == SIGHUP) {
        report = !0;
    } else {
        /* Do nothing. */
    }
}

/**
 * Emit a usage message to standard error.
 */
static void usage(int nomenu)
{
    lprintf("usage: %s [ -h ] [ -d ] [ -v ] [ -D ] [ -i IDENT ] [ -R | -S ] [ -o PATH ]\n", program);
    if (nomenu) { return; }
    lprintf("       -d            Enable debug mode\n");
    lprintf("       -v            Enable verbose mode\n");
    lprintf("       -D            Run as a daemon\n");
    lprintf("       -i IDENT      Use IDENT as the syslog identifier\n");
    lprintf("       -R            Use the rdrand instruction\n");
    lprintf("       -S            Use the rdseed instruction\n");
    lprintf("       -o PATH       Write to PATH (which may be a fifo) instead of stdout\n");
    lprintf("       -h            Print help menu\n");
}

/**
 */
static void query(void)
{
}

/**
 * This is the main program.
 * @param argc is the count of command line arguments.
 * @param argv is a vector of pointers to the command line arguments.
 */
int main(int argc, char * argv[])
{
    int xc = 1;
    int error = 0;
    unsigned int unit = 0;
    size_t written = 0;
    char * end = (char *)0;
    int rc = 0;
    FILE * fp = stdout;
    uintptr_t offset = 0;
    struct sigaction sigpipe = { 0 };
    struct sigaction sighup = { 0 };
    struct sigaction sigint = { 0 };
    struct timespec request = { 0 };
    size_t tries = 0;
    size_t total = 0;
    size_t reads = 0;
    const char * path = (const char *)0;
    enum mode mode = FAIL;
    int opt;
    extern char * optarg;
    uint32_t word;
    uint8_t carry;
    static const uint32_t CAFEBEEF = 0xCAFEBEEF;
    static const uint32_t DEADCODE = 0xDEADC0DE;

    /*
     * Crack open the command line argument vector.
     */

    program = ((program = strrchr(argv[0], '/')) == (char *)0) ? argv[0] : program + 1;

    while ((opt = getopt(argc, argv, "dvDo:i:hRS")) >= 0) {

        switch (opt) {

        case 'd':
            debug = !0;
            break;

        case 'v':
            verbose = !0;
            break;

        case 'D':
            daemonize = !0;
            break;

        case 'o':
            path = optarg;
            break;

        case 'i':
            ident = optarg;
            break;

        case 'h':
            xc = 0;
            error = !0;
            break;

        case 'R':
            mode = RDRAND;
            break;

        case 'S':
            mode = RDSEED;
            break;

        default:
            error = !0;
            break;

        }

        if (error) {
            break;
        }

    }

    do {

        if (error) {
            usage(xc);
            break;
        }

        if (verbose) {  
            query();
        }

        /*
         * Daemonize if so configured. Why do we do this stuff with the
         * system log? If we daemonize, we want to direct subsequent messages
         * to the system log, since standard error will be redirected to
         * /dev/null. But it is convenient to direct all the messages to the
         * same place, so if a daemon we start off using the system log.
         * But if daemon(3) works as I think it should (it is really under-
         * documented), it should close the socket used to communicate with
         * the system log daemon. So we close it (in case it isn't already
         * closed) and (re)open it. (I am highly tempted not to use the
         * daemon(3) function and port the equivalent function from Diminuto.)
         */

        if (daemonize) {
            openlog(ident, LOG_CONS | LOG_PID, LOG_DAEMON);
            if (daemon(0, 0) < 0) {
                perror("daemon");
                break;
            }
            closelog();
            openlog(ident, LOG_CONS | LOG_PID, LOG_DAEMON);
        }

        if (verbose) {
            lprintf("%s: pid          %d\n", program, getpid());
        }

        /*
         * Install our signal handlers.
         */

        sigpipe.sa_handler = handler;
        sigpipe.sa_flags = 0;
        rc = sigaction(SIGPIPE, &sigpipe, (struct sigaction *)0);
        if (rc < 0) {
            lerror("sigaction");
            break;
        }

        sighup.sa_handler = handler;
        sighup.sa_flags = SA_RESTART;
        rc = sigaction(SIGHUP, &sighup, (struct sigaction *)0);
        if (rc < 0) {
            lerror("sigaction");
            break;
        }

        sigint.sa_handler = handler;
        sigint.sa_flags = 0;
        rc = sigaction(SIGINT, &sighup, (struct sigaction *)0);
        if (rc < 0) {
            lerror("sigaction");
            break;
        }

        /*
         * Switch from stdout to PATH if so configured.
         */

        if (path != (const char *)0) {
            if (verbose) {
                lprintf("%s: path         \"%s\"\n", program, path);
            }
            fp = fopen(path, "a");
            if (fp == (FILE *)0) {
                lerror(path);
                break;
            }
        }

        /*
         * Enter our work loop.
         */

        if (verbose) {
            lprintf("%s: mode         %s\n", program, MODE[mode]);
        }

        request.tv_sec = 0;
        request.tv_nsec = 1000000;

        word = DEADCODE;
        carry = 1;

        while (!done) {
            if (report) {
                lprintf("%s: tries=%zu size=%zu reads=%zu total=%zu\n", program, tries, sizeof(word), reads, total);
                report = 0;
            }
            ++tries;
            if (mode == RDRAND) {
#if 0
                word = CAFEBEEF;
                carry = _rdrand32_step(&word);
#elif 0
                word = CAFEBEEF;
                carry = __builtin_ia32_rdrand32_step(&word);
#elif 1
                word = CAFEBEEF;
                carry = 1;
                asm volatile ("rdrand %0; setc %1" : "=r" (word), "=qm" (carry) );
#endif
            } else if (mode == RDSEED) {
#if 0
                word = CAFEBEEF;
                carry = _rdseed32_step(&word);
#elif 0
                word = CAFEBEEF;
                carry = __builtin_ia32_rdrand32_step(&word);
#elif 1
                word = CAFEBEEF;
                carry = 1;
                asm volatile ("rdseed %0; setc %1" : "=r" (word), "=qm" (carry) );
#endif
            } else {
                /* Do nothing (fail). */
            }
            if (carry) {
                /* Do nothing (nominal). */
            } else if ((rc = nanosleep(&request, (struct timespec *)0)) >= 0) {
                continue;
            } else if (errno == EINTR) {
                continue;
            } else {
                lerror("nanosleep");
                break;
            }
            ++reads;
            total += sizeof(word);
            written = fwrite(&word, sizeof(word), 1, fp);
            if (written < 1) {
                lerror("fwrite");
                break;
            }
            if (debug) {
                lprintf("%s: tries=%zu size=%zu reads=%zu total=%zu\n", program, tries, sizeof(word), reads, total);
            }
        }

        xc = 0;

    } while (0);

    /*
     * Clean up after ourselves.
     */

    if (fp != (FILE *)0) {
        fclose(fp);
    }

    if (verbose) {
        lprintf("%s: tries=%zu reads=%zu total=%zu\n", program, tries, reads, total);
    }

    return xc;
}