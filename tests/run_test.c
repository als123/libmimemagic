/*
    Copyright (c) Anthony L. Shipman, 2015

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice immediately at the beginning of the file, without modification,
       this list of conditions, and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
     
    THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
    ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mimemagic.h"

//======================================================================

/*  We allow the special mime type of 'unrecognised'.
*/
static void
usage()
{
    fprintf(stderr, "Usage: run_test: -f FILE [-m MIME] [-p | -P int]\n");
}

//======================================================================

typedef unsigned char Byte;

static void
readfile(const char* path, Byte** buf, size_t* len)
{
    struct stat st;
    int     err;
    int     fd;

    err = stat(path, &st);

    if (err < 0)
    {
        perror(path);
        exit(1);
    }

    *buf = (Byte*)malloc(st.st_size);
    *len = st.st_size;

    fd = open(path, O_RDONLY);

    if (fd < 0)
    {
        perror(path);
        exit(1);
    }

    err = read(fd, *buf, *len);

    if (err < 0)
    {
        perror(path);
        exit(1);
    }
}



static void
reportTime(
    const char*      testFile,
    struct timespec* start,
    struct timespec* stop,
    size_t           count
    )
{
    time_t secs  = stop->tv_sec - start->tv_sec;
    long   nsecs = stop->tv_nsec - start->tv_nsec;
    double tm;

    while (nsecs < 0)
    {
        secs -= 1;
        nsecs += 1000000000;
    }

    tm = (1000000 * (secs + nsecs / 1000000000.0)) / count;

    printf("%s: time %.1f usecs\n", testFile, tm);
}


//======================================================================


int
main(int argc, char** argv)
{
    const char*     testFile = 0;
    const char*     expected = 0;
    size_t          perf     = 0;
    Byte*           buffer   = 0;
    size_t          numBytes;
    const char*     mimeType = 0;

    int opt;
    int err;

    while ((opt = getopt(argc, argv, "hpP:f:m:")) != -1)
    {
        switch (opt)
        {
        case 'f':
            testFile = optarg;
            break;

        case 'h':
        case '?':
            usage();
            exit(0);
            break;

        case 'm':
            expected = optarg;
            break;

        case 'p':
            perf = 1000;
            break;

        case 'P':
            perf = atoi(optarg);
            break;

        default:
            usage();
            exit(EXIT_FAILURE);
        }
    }

    if (!testFile)
    {
        usage();
        exit(1);
    }

    readfile(testFile, &buffer, &numBytes);

    if (perf)
    {
        // For performance run this 1000 times.
        struct timespec start;
        struct timespec stop;

        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);

        for (size_t i = 0; i < perf; ++i)
        {
            err = getMimeType(buffer, numBytes, &mimeType, MimeMagicNone);
            mimeType = 0;
        }

        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stop);
        reportTime(testFile, &start, &stop, perf);
    }
    else
    {
        err = getMimeType(buffer, numBytes, &mimeType, MimeMagicNone);

        if (expected)
        {
            if (strcmp("unrecognised", expected) == 0 && err < 0)
            {
                err = 1;
                mimeType = expected;
            }
            else
            if (err > 0)
            {
                err = (strcmp(mimeType, expected) == 0);
            }

            if (err > 0)
            {
                printf("Passed: %s: %s\n", testFile, mimeType);
            }
            else
            {
                printf("Failed: %s, not %s\n", testFile, expected);
            }
        }
        else
        {
            if (err > 0)
            {
                printf("%s\t%s\n", testFile, mimeType);
            }
            else
            {
                printf("%s\tUnrecognised MIME type\n", testFile);
            }
        }
    }

    if (buffer)
    {
        free(buffer);
        buffer = 0;
    }

    return err > 0? 0 : 1;
}
