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
#include <getopt.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <magic.h>

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


static void
usage()
{
    fprintf(stderr, "Usage: perf_old: -f FILE\n");
}


int
main(int argc, char** argv)
{
    const char*     testFile = 0;
    int             perf     = 0;
    Byte*           buffer   = 0;
    size_t          numBytes;
    const char*     mimeType = 0;
    magic_t         Magic;

    int opt;
    int err;

    while ((opt = getopt(argc, argv, "hpf:")) != -1)
    {
        switch (opt)
        {
        case 'f':
            testFile = optarg;
            break;

        case 'p':
            perf = 1;
            break;


        case 'h':
        case '?':
            usage();
            exit(0);
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

    Magic = magic_open(MAGIC_MIME_TYPE);

    magic_load(Magic, NULL);

    if (perf)
    {
        // For performance run this 1000 times.
        static const size_t Count = 1000;
        struct timespec start;
        struct timespec stop;

        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);

        for (size_t i = 0; i < Count; ++i)
        {
            mimeType = magic_buffer(Magic, (const void *)buffer, numBytes);
        }


        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stop);
        reportTime(testFile, &start, &stop, Count);
    }
    else
    {
        mimeType = magic_buffer(Magic, (const void *)buffer, numBytes);
        printf("%s\t%s\n", testFile, mimeType);
    }

    magic_close(Magic);

    free(buffer);

    return 0;
}
