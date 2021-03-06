/* vi: set ts=4 expandtab shiftwidth=4: */
/**
 * @file
 * Quantis Tool<BR>
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
 * quantistool [ -h ] [ -d ] [ -v ] [ -D ] [ -i IDENT ] [ -u UNIT | -p UNIT ] [ -r BYTES ] [ -c ] [ -o PATH ]
 *
 * EXAMPLES
 *
 * quantistool -v -U 0 -r 0
 *
 * quantistool -v -U 0 | dd of=random.dat bs=4096 count=1024 iflag=fullblock
 * 
 * mkfifo quantis.fifo
 * chmod 666 quantis.fifo
 * quantistool -D -i QUANTIS -U 0 -c -o quantis.fifo &
 *
 * ABSTRACT
 *
 * Continuously reads data from a Quantis hardware entropy generator,
 * manufactured by ID Quantique, and writes it to standard output, or to a
 * specified file system path. This latter object could be a FIFO, which could
 * allow generated entropy to be read by another program, like rngd. Optionally
 * does some other useful stuff with the Quantis. This is part of the Scattergun
 * project. This must be linked with the Quantis library.
 *
 * The Quantis software library I have restricts reads to sizes of no more
 * than 16 megabytes (16 * 1024 * 1024). But it restricts each individual
 * read from the device to the block transfer size advertised by the device.
 * The lsusb command suggests that the max packet size for my Quantis USB
 * device is 512 bytes. There doesn't seem to be any mechanism to just "read
 * what you got" so that we get all available random bits without possibly
 * blocking to wait for more or leaving some behind.
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
#include <sys/types.h>
#include <sys/stat.h>
#include "Quantis.h"

static const QuantisDeviceType TYPES[] = { QUANTIS_DEVICE_PCI, QUANTIS_DEVICE_USB };
static const char * NAMES[] = { "PCI", "USB" };

static const char * program = "quantistool";
static const char * ident = "quantistool";
static int debug = 0;
static int verbose = 0;
static int done = 0;
static int report = 0;
static int daemonize = 0;

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
 * Emit a formatting string to either the system log or to standard error
 * if verbosity is enabled.
 * @param format is the printf format.
 */
static void lverbosef(const char * format, ...)
{
    if (verbose) {
        va_list ap;
        va_start(ap, format);
        if (daemonize) {
            vsyslog(LOG_DEBUG, format, ap);
        } else {
            vfprintf(stderr, format, ap);
        }
        va_end(ap);
    }
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
        syslog(LOG_ERR, "%s: %s\n", string, strerror(errno));
    } else {
        fprintf(stderr, "%s: %s\n", string, strerror(errno));
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
 * @param nomenu if true suppresses the printing of the menu.
 */
static void usage(int nomenu)
{
    lprintf("usage: %s [ -h ] [ -d ] [ -v ] [ -D ] [ -i IDENT ] [ -u UNIT | -p UNIT ] [ -r BYTES ] [ -c ] [ -o PATH ]\n", program);
    if (nomenu) { return; }
    lprintf("       -d            Enable debug mode\n");
    lprintf("       -v            Enable verbose mode\n");
    lprintf("       -D            Run as a daemon\n");
    lprintf("       -i IDENT      Use IDENT as the syslog identifier\n");
    lprintf("       -u UNIT       Use USB card UNIT\n");
    lprintf("       -p UNIT       Use PCI card UNIT\n");
    lprintf("       -r BYTES      Read at most BYTES bytes at a time (0 to exit)\n");
    lprintf("       -c            Check for the requested device\n");
    lprintf("       -o PATH       Write to PATH (which may be a fifo) instead of stdout\n");
    lprintf("       -h            Print help menu\n");
}

/**
 * Query the Quantis API for the kinds of ID Quantique hardware it finds on
 * the PCI or the USB busses and its nature and emit the results to standard
 * error.
 * @return true if the requested device is available, false otherwise.
 */
static int query(QuantisDeviceType want, int unit)
{
    int rc = 0;
    int ii;

    lverbosef("%s: device       detecting\n", program);

    for (ii = 0; ii < (sizeof(TYPES) / sizeof(TYPES[0])); ++ii) {
        QuantisDeviceType type = 0;
        float software = 0.0;
        int detected = 0;
        int jj;

        type = TYPES[ii];
        detected = QuantisCount(type);

        lverbosef("%s: type         %s\n", program, NAMES[ii]);
        lverbosef("%s: detected     %d\n", program, detected);

        if (detected <= 0) {
            continue;
        }

        software = QuantisGetDriverVersion(type);
        lverbosef("%s: software     %f\n", program, software);

        for (jj = 0; jj < detected; ++jj) {
            int hardware = 0;
            const char * serial = (const char *)0;
            const char * manufacturer = (const char *)0;
            int power = 0;
            int mask = 0;
            int status = 0;

            hardware = QuantisGetBoardVersion(type, jj);
            serial = QuantisGetSerialNumber(type, jj);
            manufacturer = QuantisGetManufacturer(type, jj);
            power = QuantisGetModulesPower(type, jj);
            mask = QuantisGetModulesMask(type, jj);
            status = QuantisGetModulesStatus(type, jj);

            lverbosef("%s: unit         %d\n", program, jj);
            lverbosef("%s: hardware     %d\n", program, hardware);
            lverbosef("%s: serial       \"%s\"\n", program, serial);
            lverbosef("%s: manufacturer \"%s\"\n", program, manufacturer);
            lverbosef("%s: power        %d\n", program, power);
            lverbosef("%s: modules      0x%8.8x\n", program, mask);
            lverbosef("%s: status       0x%8.8x\n", program, status);

            if (want != type) {
                /* Do nothing. */
            } else if (unit != jj) {
                /* Do nothing. */
            } else if (status == 0x00000000) {
                /* Do  nothing. */
            } else {
                rc = !0;
            }

            lverbosef("%s: device       %s\n", program, rc ? "present" : "absent");
        }
    }
    
    return rc;
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
    QuantisDeviceType type = QUANTIS_DEVICE_USB;
    unsigned int unit = 0;
    size_t size = 512;
    size_t written = 0;
    char * end = (char *)0;
    QuantisDeviceHandle * handle = (QuantisDeviceHandle *)0;
    unsigned char * buffer = (unsigned char *)0;
    int rc = 0;
    FILE * fp = stdout;
    uintptr_t offset = 0;
    struct sigaction sigpipe = { 0 };
    struct sigaction sighup = { 0 };
    struct sigaction sigint = { 0 };
    size_t opens = 0;
    size_t total = 0;
    size_t reads = 0;
    int try = 0;
    const char * path = (const char *)0;
    int opt;
    extern char * optarg;
    int ii;
    int check = 0;

    /*
     * Crack open the command line argument vector.
     */

    program = ((program = strrchr(argv[0], '/')) == (char *)0) ? argv[0] : program + 1;

    while ((opt = getopt(argc, argv, "dvDu:p:r:co:i:h")) >= 0) {

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

        case 'u':
            type = QUANTIS_DEVICE_USB;
            unit = strtoul(optarg, &end, 0);
            break;

        case 'p':
            type = QUANTIS_DEVICE_PCI;
            unit = strtoul(optarg, &end, 0);
            break;

        case 'c':
            check = !0;
            break;

        case 'r':
            size = strtoul(optarg, &end, 0);
            if ((*end != '\0') || (!((0 <= size) && (size <= QUANTIS_MAX_READ_SIZE)))) {
                errno = EINVAL;
                lerror(optarg);
                error = !0;
            }
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

        /*
         * Daemonize if so configured.
         */

        if (!daemonize) {
            /* Do nothing. */
        } else if (daemon(0, 0) < 0) {
            perror("daemon");
            break;
        } else {
            openlog(ident, LOG_CONS | LOG_PID, LOG_DAEMON);
            lverbosef("%s: pid          %d\n", program, getpid());
        }

        for (ii = 0; ii < (sizeof(TYPES) / sizeof(TYPES[0])); ++ii) {
            if (TYPES[ii] == type) {
                lverbosef("%s: type         %s\n", program, NAMES[ii]);
            }
        }
        lverbosef("%s: unit         %d\n", program, unit);
        lverbosef("%s: bytes        %zu\n", program, size);
        lverbosef("%s: maximum      %zu\n", program, (size_t)QUANTIS_MAX_READ_SIZE);

        /*
         * See what kind of hardware we have, and if it matches what
         * the user requested.
         */

        rc = query(type, unit);
        if (rc) {
            /* Do nothing. */
        } else if (!check) {
            /* Do nothing. */
        } else {
            break;
        }

        /*
         * If the user specifies a read size of zero, we just exit. This
         * allows them to use the verbose option to query the Quantis
         * devices on the USB or PCI bus without actually doing anything
         * else.
         */

        if (size == 0) {
            xc = 0;
            break;
        } else if (size > QUANTIS_MAX_READ_SIZE) {
            size = QUANTIS_MAX_READ_SIZE;
        } else {
            /* Do nothing. */
        }

        buffer = malloc(size);
        if (buffer == (unsigned char *)0) {
            lerror("malloc");
            break;
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
            lverbosef("%s: path         \"%s\"\n", program, path);
            fp = fopen(path, "a");
            if (fp == (FILE *)0) {
                lerror(path);
                break;
            }
        }

        /*
         * Enter our work loop.
         */

        while (!done) {

            rc = QuantisOpen(type, unit, &handle);
            if (rc < QUANTIS_SUCCESS) {
                lprintf("%s: QuantisOpen(%d,%d,%p)=%d=\"%s\"\n", program, type, unit, handle,  rc, QuantisStrError(rc));
                break;
            }
            ++opens;
            lverbosef("%s: handle       %p\n", program, handle);

            /*
             * Enter our input/output loop. If the read fails we try it again,
             * since sometimes the libusb data transfer seems to hiccup for no
             * obvious reason. If it fails the second time, we close the device
             * and reopen it. As long as the open succeeds, we soldier on.
             */

            while (!done) {
                if (report) {
                    lprintf("%s: opens=%zu size=%zu reads=%zu total=%zu\n", program, opens, size, reads, total);
                    report = 0;
                }
                rc = QuantisReadHandled(handle, buffer, size);
                if (rc < QUANTIS_SUCCESS) {
                    lprintf("%s: QuantisReadHandled(%p,%p,%zu)=%d=\"%s\" try=1\n", program, handle, buffer, size, rc, QuantisStrError(rc));
                    rc = QuantisReadHandled(handle, buffer, size);
                    if (rc < QUANTIS_SUCCESS) {
                        lprintf("%s: QuantisReadHandled(%p,%p,%zu)=%d=\"%s\" try=2\n", program, handle, buffer, size, rc, QuantisStrError(rc));
                        break;
                    }
                }
                if (rc < QUANTIS_SUCCESS) {
                    break;
                }
                ++reads;
                total += size;
                written = fwrite(buffer, size, 1, fp);
                if (written < 1) {
                    lerror("fwrite");
                    break;
                }
                if (debug) {
                    lprintf("%s: opens=%zu size=%zu reads=%zu total=%zu\n", program, opens, size, reads, total);
                }
            }

            if (handle != (QuantisDeviceHandle *)0) {
                QuantisClose(handle);
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

    if (buffer != (unsigned char *)0) {
        free(buffer);
    }

    lverbosef("%s: opens=%zu size=%zu reads=%zu total=%zu\n", program, opens, size, reads, total);

    return xc;
}
