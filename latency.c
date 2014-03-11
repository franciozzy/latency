/*
 * ---------------------------------------
 *  Instant Disk Latency Measurement Tool
 * ---------------------------------------
 *  latency.c
 * -----------
 *  Copyright (c) 2014 Citrix
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2 of the License only.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Please read the README file.
 */

// Global definitions (don't mess with those)
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS       64

#define MT_PROGNAME             "Instant Disk Latency Measurement Tool"
#define MT_PROGNAME_LEN         strlen(MT_PROGNAME)

#define MT_OPREAD               0
#define MT_OPWRITE              1

#ifdef  CLOCK_MONOTONIC_RAW
#define MT_CLOCK                CLOCK_MONOTONIC_RAW
#else
#define MT_CLOCK                CLOCK_MONOTONIC
#endif /* CLOCK_MONOTONIC_RAW */

// Global default definitions
#define MT_BUFSIZE              4096

// Header files
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

// Global variables
uint32_t        totaltime = 0;          // Total time reading
uint32_t        count = 0;              // Number of reads
int32_t         calarm = -1;            // Alarm counter
uint8_t         falarm = 0;             // Alarm flag
uint8_t         simple = 0;             // Simple output
uint8_t         zeros = 0;              // Write zeros

// Auxiliary functions
void usage(char *argv0){
    // Local variables
    int         i;

    // Print usage
    for (i=0; i<MT_PROGNAME_LEN; i++) fprintf(stderr, "-");
    fprintf(stderr, "\n%s\n", MT_PROGNAME);
    for (i=0; i<MT_PROGNAME_LEN; i++) fprintf(stderr, "-");
    fprintf(stderr, "\nUsage: %s [ -hsw ] [ -b size ] dev_name [ "
                    "iterations ]\n", argv0);
    fprintf(stderr, "       -h               Print help message and "
                    "quit.\n");
    fprintf(stderr, "       -s               Simple output: print latency "
                    "only.\n");
    fprintf(stderr, "       -w               Write instead of read. USE "
                    "WITH CARE.\n");
    fprintf(stderr, "       -z               Write zeros instead of random "
                    "data.\n");
    fprintf(stderr, "       -b size          Use <size> bytes at a time "
                    "(default=%d).\n", MT_BUFSIZE);
    fprintf(stderr, "       dev_name         Specify block device to "
                    "operate on.\n");
    fprintf(stderr, "       iterations       Execute for so many iterations "
                    "and exit.\n");
}

void sigalarm_h(){
    falarm = 1;
}

// Main function
int main(int argc, char **argv){
    // Local variables

    // IO related
    char             *buf    = NULL;    // IO Buffer
    off_t            bufsize = -1;      // IO Buffer size
    int              optype  = MT_OPREAD;

    // Block device related
    char             *bdevfn = NULL;    // Block device file name
    int              bdevfd  = -1;      // Block device file descriptor
    int              bdflags;           // Block device open flags
    off_t            bdevsize;          // Block device size
    int              urandfd = -1;      // /dev/urandom file descriptor

    // System related
    int              psize;             // System pagesize
    int              err = 0;           // Return code

    // Summary related
    ssize_t          bytes;             // Number of bytes io'ed (each io)
    struct itimerval itv;               // iTimer setup
    struct timeval   ts, ts1, ts2;      // Read timers

    // General
    int		i;		// Temporary integer

    // Fetch arguments
    while ((i = getopt(argc, argv, "hwszb:")) != -1){
        switch (i){
        case 's': // Set output to simple
            if (simple){
                fprintf(stderr, "%s: Error, 'simple' output already set.\n",
                                argv[0]);
                goto err;
            }
            simple = 1;
            break;

        case 'w': // Set OP to write
            if (optype){
                fprintf(stderr, "%s: Error, operation type already set to "
                                "'write'.\n", argv[0]);
                goto err;
            }
            optype = MT_OPWRITE;
            break;

        case 'z': // Set output to zeros
            if (zeros){
                fprintf(stderr, "%s: Error, already set to write zeros.\n",
                        argv[0]);
                goto err;
            }
            zeros = 1;
            break;

        case 'b': // Set bufsize
            if (bufsize != -1){
                fprintf(stderr, "%s: Error, buffer size already set to %"
                                PRId64".\n", argv[0], bufsize);
                goto err;
            }
            bufsize = atoi(optarg);
            if (bufsize <= 0){
                fprintf(stderr, "%s: Invalid buffer size %"PRId64".\n",
                                argv[0], bufsize);
                goto err;
            }
            break;

        case 'h':
        default:   // Print help
            usage(argv[0]);
            goto out;
        }
    }

    // Verify number of arguments and fetch device name
    if ((argc != optind+1) && (argc != optind+2)){
        if (argc > optind+1){
            fprintf(stderr, "%s: Error, too many arguments.\n\n", argv[0]);
        }
        usage(argv[0]);
        goto err;
    }
    if (asprintf(&bdevfn, "%s", argv[optind]) <= 0){
        fprintf(stderr, "%s: Error allocating block device name.\n", argv[0]);
        bdevfn = NULL;
        goto err;
    }

    // Get number of iterations if passed as an argument
    if (argc == optind+2){
        calarm = atoi(argv[optind+1]);
        if (calarm == 0){
            fprintf(stderr, "%s: Iteration counter must be greater than 0.\n",
                            argv[0]);
            goto err;
        }
    }

    // Set defaults
    if (bufsize == -1)
        bufsize = MT_BUFSIZE;

    // Make sure a block device has been specified
    if (bdevfn == NULL){
        usage(argv[0]);
        goto err;
    }

    // Open block device
    bdflags = O_RDWR | O_DIRECT | O_LARGEFILE | O_SYNC;
    if ((bdevfd = open(bdevfn, bdflags, 0)) < 0){
        perror("open");
        fprintf(stderr, "%s: Error opening block device \"%s\".\n", argv[0],
                        bdevfn);
        goto err;
    }

    // Move the pointer to the end of the block device (gets block device size)
    if ((bdevsize = lseek(bdevfd, 0, SEEK_END)) < 0){
        perror("lseek");
        fprintf(stderr, "%s: Error repositioning offset to eof.\n", argv[0]);
        goto err;
    }

    // Move the pointer back to the beginning of the block device
    if (lseek(bdevfd, 0, SEEK_SET) < 0){
        perror("lseek");
        fprintf(stderr, "%s: Error repositioning offset to start.\n", argv[0]);
        goto err;
    }

    // Fetch system pagesize
    psize = getpagesize();

    // Allocates rbuf according to the systems page size
    if (posix_memalign((void **)&(buf), psize, bufsize) != 0){
        //perror("posix_memalign"); // posix_memalign doesn't set errno.
        fprintf(stderr, "%s: Error malloc'ing aligned buf, %"PRId64" bytes "
                        "long.\n", argv[0], bufsize);
        goto err;
    }

    // Fill buffer with random data if writing
    if (optype == MT_OPWRITE){
        if (zeros){
            memset(buf, 0, bufsize);
        }else if ((urandfd = open("/dev/urandom", O_RDONLY)) < 0){
            perror("open");
            fprintf(stderr, "%s: warning: writing zeros instead of random.\n",
                            argv[0]);
            memset(buf, 0, bufsize);
        }else{
            if (!simple)
                fprintf(stderr, "%s: Reading %"PRId64" random bytes.\n",
                                argv[0], bufsize);
            i = read(urandfd, buf, bufsize);
            (void)close(urandfd);
        }
    }

    // Setup alarm
    signal(SIGALRM, sigalarm_h);
    itv.it_interval.tv_sec = 1;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 1;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &itv, NULL);

    // Loop
    while(calarm){
        // Dump registers if alarmed
        if (falarm){
            if (simple){
                if (count)
                    fprintf(stdout, "%u\n", totaltime/count);
                else
                    fprintf(stdout, "0\n");
            }else{
                gettimeofday(&ts, NULL);
                if (count)
                    fprintf(stdout, "%ld: %u us (%u/%u)\n", ts.tv_sec,
                                    totaltime/count, totaltime, count);
                else
                    fprintf(stdout, "%ld: 0 us (%u/%u)\n", ts.tv_sec,
                                    totaltime, count);
            }
            fflush(stdout);

            count = totaltime = falarm = 0;
            if (calarm > 0)
                calarm--;
        }

        // Execute IO operation
        gettimeofday(&ts1, NULL);
        if (optype == MT_OPWRITE)
            bytes = write(bdevfd, buf, bufsize);
        else
            bytes = read(bdevfd, buf, bufsize);
        gettimeofday(&ts2, NULL);

        // Increment number of total bytes io'ed
        if (bytes == bufsize){
            // totaltime += ((ts2.tv_sec - ts1.tv_sec)*1000 + 
            //              (ts2.tv_usec - ts1.tv_usec)/1000); // ms
            totaltime += ((ts2.tv_sec - ts1.tv_sec)*1000000 + (ts2.tv_usec -
                         ts1.tv_usec)); // us
            count++;
        }else{
            // Reset the I/O position
            if ((bytes <= 0) && (lseek(bdevfd, 0, SEEK_SET) < 0)){
                perror("lseek");
                fprintf(stderr, "%s: Error offsetting to the start of the "
                                "device.\n", argv[0]);
                err = 1;
                goto out;
            }
        }
    }

    // Bypass error section
    goto out;

err:
    err = 1;

out:
    // Free resources
    if (buf)
        free(buf);
    if (bdevfn)
        free(bdevfn);

    // Close block device
    if (bdevfd != -1)
        close(bdevfd);

    // Return
    return(err);
}
