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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "mimemagic.h"

//======================================================================

typedef int       Bool;
typedef uint8_t   Byte;
typedef uint64_t  Mask;
typedef int64_t   Int;
typedef uint64_t  UInt;

#define True  1
#define False 0

/*  This library doesn't distinguish between text and binary data.
*/
typedef enum StringFlags
{
    CompactWS   = 1 << 0, // Require WS and elide it
    IgnoreWS    = 1 << 1, // Ignore WS
    MatchLower  = 1 << 2, // match lower-case in the string without case
    MatchUpper  = 1 << 3, // match upper-case in the string without case
    RegexOffset = 1 << 4, // regex offset start
} StringFlags;


typedef enum Compare
{
    CompareEq   = 1,
    CompareLt   = 2,
    CompareGt   = 3,
    CompareSet  = 4,        // bits of the test value must be set
    CompareClr  = 5,        // bits of the test value must be clear
    CompareMask = 0xff,     // bits of the test value must be clear
    CompareNot  = 1 << 8,   // invert the test
    CompareNeg  = 1 << 9,   // negate the value before testing
} Compare;


typedef enum RegexFlags
{
    RegexNoCase   = 1 << 0, // case-insensitive matching
    RegexBegin    = 1 << 1, // leave the offset at the beginning of the match
} RegexFlags;


typedef struct StringMap
{
    const char* test;
    size_t      tlen;
    const char* mime;
} StringMap;


typedef struct ShortMap
{
    int16_t     test;
    uint16_t    mask;
    const char* mime;
} ShortMap;


//======================================================================

/*  The result of a match is
        >0  success, the number of bytes that matched
        0   no match
        <0  error, missing data

    If there aren't enough bytes to perform a test then -1 is returned.
    We allow true to map to Match and false to Fail.

    The offset is passed in as a pointer. Normally it is updated on a match
    to point to the next byte after the match.
*/

typedef enum Result
{
    Match = 1,
    Fail  = 0,
    Error = -1
} Result; 



static Result
stringMatch(
    const Byte* buf,
    size_t      len,
    const char* test,
    size_t      tlen,
    size_t*     offset,
    Compare     oper,
    int         flags
    )
{
    const Byte* bp   = buf + *offset;
    const Byte* bend = bp + len;
    const Byte* tp   = test;
    const Byte* tend = test + tlen;
    Bool        match;

    for (; tp < tend && bp < bend; ++bp, ++tp)
    {
        char b = *bp;
        char c = *tp;

        // If CompactWS then skip over extra spaces to the last one
        if (b == ' ' && flags & CompactWS)
        {
            for (++bp; *bp == ' ' && bp < bend; ++bp)
            {
            }

            --bp;
        }

        if (c == ' ' && flags & IgnoreWS)
        {
            // If the buffer doesn't have a space here then skip the spaces in the test.
            if (b != ' ')
            {
                for (++tp; *tp == ' ' && tp < tend; ++tp)
                {
                }

                if (tp < tend)
                {
                    c = *tp;
                }
            }
        }

        if (flags && MatchLower && c >= 'a' && c <= 'z')
        {
            // Convert the buffer char to lower case to ignore its case.
            if (b >= 'A' && b <= 'Z')
            {
                b += 32;
            }
        }

        if (flags && MatchUpper && c >= 'A' && c <= 'Z')
        {
            // Convert the buffer char to upper case to ignore its case.
            if (b >= 'a' && b <= 'z')
            {
                b -= 32;
            }
        }

        switch (oper & CompareMask)
        {
        case CompareLt: match = (c < b);        break;
        case CompareGt: match = (c > b);        break;
        default:        match = (b == c);       break;
        }

        if (oper & CompareNot)
        {
            match = !match;
        }

        if (!match)
        {
            break;
        }
    }

    if (match)
    {
        *offset += bp - buf;
        return bp - buf;
    }

    if (bp == bend && tp != tend || bp != bend && tp == tend)
    {
        // They didn't finish together so we must have run out of bytes.
        return Error;
    }

    return Fail;
}



static Result
stringSearch(
    const Byte* buf,
    size_t      len,
    const char* test,
    size_t      tlen,
    size_t*     offset,
    size_t      limit,
    int         flags
    )
{
    // Search for the string up to limit characters beyond the offset.
    // len - tlen is the last position at which a match is possible.
    // Ranges are exclusive at the right.
    Result  rslt;
    size_t  start = *offset;
    size_t  last;
    size_t  end = start + limit;

    if (start >= len || tlen > len)
    {
        return Error;
    }

    last = len - tlen + 1;

    if (end >= last)
    {
        end = last;
    }

    for (; start < end; ++start)
    {
        // stringMatch will update this to the end of the match
        *offset = start;

        rslt = stringMatch(buf, len, test, tlen, offset, CompareEq, flags);

        if (rslt != Fail)
        {
            return rslt;
        }
    }

    return Error;
}



static Result
regexMatch(
    const Byte* buf,
    size_t      len,
    const char* pattern,
    size_t*     offset,
    size_t      limit,
    int         flags
    )
{
    /*  There is only a limit if it is greater than 0.
        Either way the data must be NUL-terminated text
        so we make a copy.

        It appears that calls to regcomp() do a large number of memory
        allocations.  If would be tempting to cache compiled regexps
        but that would require a context object to preserve thread
        safety.

    */
    Result      result = Error;
    regex_t     compiled;
    regmatch_t  pmatch;
    char*       text = 0;
    int         cflags = REG_EXTENDED | REG_NEWLINE;
    int         err;
    size_t      found = 0;

    if (flags & RegexNoCase)
    {
        cflags |= REG_ICASE;
    }
    
    err = regcomp(&compiled, pattern, cflags);

    if (err == 0)
    {
        if (limit == 0 || limit > len)
        {
            limit = len;
        }

        text = malloc(limit + 1);
        memcpy(text, buf + *offset, limit);
        text[limit] = 0;

        err = regexec(&compiled, text, 1, &pmatch, 0);

        if (err == 0)
        {
            found  = pmatch.rm_eo - pmatch.rm_so;
            result = found;
        }
        else
        {
            result = Fail;
        }

        free(text);
        regfree(&compiled);
    }

    if (result > 0 && !(flags & RegexBegin))
    {
        *offset += found;
    }

    return result;
}



static Result
stringEqual(const Byte* buf, size_t len, const char* test, size_t tlen, size_t* offset)
{
    // A common case. The test string may contain NUL bytes.
    size_t n = *offset;

    if (n + tlen <= len)
    {
        if (tlen > 0 && buf[n] == test[0])
        {
            if (tlen == 1 || memcmp((const char*)buf + n + 1, test + 1, tlen - 1) == 0)
            {
                *offset += tlen;
                return tlen;
            }
        }

        return 0;
    }

    return Error;
}



static Result
stringEqualMap(const Byte* buf, size_t len, const StringMap* map, size_t mapLen, const char** mime)
{
    /*  Perform multiple equality tests and select a MIME string.

        The map entries are sorted by the test string. We can speed
        things up a bit.  Note that we never have len == 0 and the test
        strings are never empty either.

    */
    size_t i     = 0;
    char   key   = buf[0];
    Bool   error = False;

    // First a quick linear search to find the first map entry.
    for (i = 0; i < mapLen; ++i)
    {
        if (map[i].test[0] == key)
        {
            break;
        }
    }

    for (; i < mapLen; ++i)
    {
        const char* test = map[i].test;
        size_t      tlen = map[i].tlen;

        if (test[0] == key)
        {
            if (tlen > len)
            {
                // Not enough data here
                error = True;
            }
            else
            if (memcmp((const char*)buf + 1, test + 1, tlen - 1) == 0)
            {
                *mime = map[i].mime;
                return tlen;
            }
        }
        else
        {
            // Reached the end of the range
            break;
        }
    }

    return error? Error : Fail;
}



static Result
stringLess(const Byte* buf, size_t len, const char* test, size_t tlen, size_t* offset)
{
    // A common case. The test string may contain NUL bytes.
    if (*offset + tlen <= len)
    {
        if (memcmp((const char*)buf + *offset, test, tlen) < 0)
        {
            *offset += tlen;
            return tlen;
        }

        return 0;
    }

    return Error;
}



static Result
stringGreater(const Byte* buf, size_t len, const char* test, size_t tlen, size_t* offset)
{
    // A common case. The test string may contain NUL bytes.
    if (*offset + tlen <= len)
    {
        if (memcmp((const char*)buf + *offset, test, tlen) > 0)
        {
            *offset += tlen;
            return tlen;
        }

        return 0;
    }

    return Error;
}



static Result
intMatch(Int value, Int test, Compare oper, Mask mask)
{
    Bool ok = False;

    if (oper & CompareNeg)
    {
        value = -value;
    }

    switch (oper & CompareMask)
    {
    case CompareEq:     ok = test == value;     break;
    case CompareLt:     ok = test <  value;     break;
    case CompareGt:     ok = test >  value;     break;
    case CompareSet:    ok = (test & value) == test; break;
    case CompareClr:    ok = (~test & value) == 0;   break;
    }

    if (oper & CompareNot)
    {
        ok = !ok;
    }

    return ok? Match : Fail;
}



static Result
uintMatch(UInt value, UInt test, Compare oper, Mask mask)
{
    // Hopefully the CompareNeg flag is not set.
    Bool ok = False;

    switch (oper & CompareMask)
    {
    case CompareEq:     ok = test == value;     break;
    case CompareLt:     ok = test <  value;     break;
    case CompareGt:     ok = test >  value;     break;
    case CompareSet:    ok = (test & value) == test; break;
    case CompareClr:    ok = (~test & value) == 0;   break;
    }

    if (oper & CompareNot)
    {
        ok = !ok;
    }

    return ok? Match : Fail;
}



static Result
byteMatch(const Byte* buf, size_t len, char test, Compare oper, Mask mask, size_t* offset)
{
    if (len > *offset)
    {
        if (intMatch(buf[*offset], test, oper, mask))
        {
            *offset += 1;
            return 1;
        }

        return 0;
    }

    return Error;
}



static Result
leShortMatch(const Byte* buf, size_t len, int16_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 2 + n)
    {
        int16_t v = (int16_t)((buf[1 + n] << 8) + buf[0 + n]);

        if (intMatch(v, test, oper, mask))
        {
            *offset += 2;
            return 2;
        }

        return 0;
    }

    return Error;
}



static Result
beShortMatch(const Byte* buf, size_t len, int16_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 2 + n)
    {
        int16_t v = (int16_t)((buf[0 + n] << 8) + buf[1 + n]);

        if (intMatch(v, test, oper, mask))
        {
            *offset += 2;
            return 2;
        }

        return 0;
    }

    return Error;
}



static Result
leLongMatch(const Byte* buf, size_t len, int32_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 4 + n)
    {
        int32_t v = (((int32_t)buf[3 + n]) << 24) +
                    (((int32_t)buf[2 + n]) << 16) +
                    (((int32_t)buf[1 + n]) <<  8) +
                               buf[0 + n];

        if (intMatch(v, test, oper, mask))
        {
            *offset += 4;
            return 4;
        }

        return 0;
    }

    return Error;
}



static Result
beLongMatch(const Byte* buf, size_t len, int32_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 4 + n)
    {
        int32_t v = (((int32_t)buf[0 + n]) << 24) +
                    (((int32_t)buf[1 + n]) << 16) +
                    (((int32_t)buf[2 + n]) <<  8) +
                               buf[3 + n];

        if (intMatch(v, test, oper, mask))
        {
            *offset += 4;
            return 4;
        }

        return 0;
    }

    return Error;
}



static Result
leQuadMatch(const Byte* buf, size_t len, int64_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 8 + n)
    {
        int64_t v = (((int64_t)buf[7 + n]) << 56) +
                    (((int64_t)buf[6 + n]) << 48) +
                    (((int64_t)buf[5 + n]) << 40) +
                    (((int64_t)buf[4 + n]) << 32) +
                    (((int64_t)buf[3 + n]) << 24) +
                    (((int64_t)buf[2 + n]) << 16) +
                    (((int64_t)buf[1 + n]) <<  8) +
                               buf[0 + n];

        if (intMatch(v, test, oper, mask))
        {
            *offset += 8;
            return 8;
        }

        return 0;
    }

    return Error;
}



static Result
beQuadMatch(const Byte* buf, size_t len, int64_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 8 + n)
    {
        int64_t v = (((int64_t)buf[0 + n]) << 56) +
                    (((int64_t)buf[1 + n]) << 48) +
                    (((int64_t)buf[2 + n]) << 40) +
                    (((int64_t)buf[3 + n]) << 32) +
                    (((int64_t)buf[4 + n]) << 24) +
                    (((int64_t)buf[5 + n]) << 16) +
                    (((int64_t)buf[6 + n]) <<  8) +
                               buf[7 + n];

        if (intMatch(v, test, oper, mask))
        {
            *offset += 8;
            return 8;
        }

        return 0;
    }

    return Error;
}



static Result
ubyteMatch(const Byte* buf, size_t len, Byte test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len > n)
    {
        if (uintMatch(buf[n], test, oper, mask))
        {
            *offset += 1;
            return 1;
        }

        return 0;
    }

    return Error;
}



static Result
uleShortMatch(const Byte* buf, size_t len, uint16_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 2 + n)
    {
        uint16_t v = (buf[1 + n] << 8) + buf[0 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 2;
            return 2;
        }

        return 0;
    }

    return Error;
}



static Result
ubeShortMatch(const Byte* buf, size_t len, uint16_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 2 + n)
    {
        uint16_t v = (buf[0 + n] << 8) + buf[1 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 2;
            return 2;
        }

        return 0;
    }

    return Error;
}



static Result
uleLongMatch(const Byte* buf, size_t len, uint32_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 4 + n)
    {
        uint32_t v = (((uint32_t)buf[3 + n]) << 24) +
                     (((uint32_t)buf[2 + n]) << 16) +
                     (((uint32_t)buf[1 + n]) <<  8) +
                                 buf[0 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 4;
            return 4;
        }

        return 0;
    }

    return Error;
}



static Result
ubeLongMatch(const Byte* buf, size_t len, uint32_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 4 + n)
    {
        uint32_t v = (((uint32_t)buf[0 + n]) << 24) +
                     (((uint32_t)buf[1 + n]) << 16) +
                     (((uint32_t)buf[2 + n]) <<  8) +
                                 buf[3 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 4;
            return 4;
        }

        return 0;
    }

    return Error;
}



static Result
uleQuadMatch(const Byte* buf, size_t len, uint64_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 8 + n)
    {
        uint64_t v = (((uint64_t)buf[7 + n]) << 56) +
                     (((uint64_t)buf[6 + n]) << 48) +
                     (((uint64_t)buf[5 + n]) << 40) +
                     (((uint64_t)buf[4 + n]) << 32) +
                     (((uint64_t)buf[3 + n]) << 24) +
                     (((uint64_t)buf[2 + n]) << 16) +
                     (((uint64_t)buf[1 + n]) <<  8) +
                                 buf[0 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 8;
            return 8;
        }

        return 0;
    }

    return Error;
}



static Result
ubeQuadMatch(const Byte* buf, size_t len, uint64_t test, Compare oper, Mask mask, size_t* offset)
{
    size_t n = *offset;

    if (len >= 8 + n)
    {
        uint64_t v = (((uint64_t)buf[0 + n]) << 56) +
                     (((uint64_t)buf[1 + n]) << 48) +
                     (((uint64_t)buf[2 + n]) << 40) +
                     (((uint64_t)buf[3 + n]) << 32) +
                     (((uint64_t)buf[4 + n]) << 24) +
                     (((uint64_t)buf[5 + n]) << 16) +
                     (((uint64_t)buf[6 + n]) <<  8) +
                                 buf[7 + n];

        if (uintMatch(v, test, oper, mask))
        {
            *offset += 8;
            return 8;
        }

        return 0;
    }

    return Error;
}



static Result
beShortGroup(const Byte* buf, size_t len, const ShortMap* map, size_t mapLen, const char** mime)
{
    // Do multiple beshort tests at offset 0.
    if (len >= 2)
    {
        int16_t value = (int16_t)((buf[0] << 8) + buf[1]);
        size_t  i = 0;

        for (i = 0; i < mapLen; ++i)
        {
            int16_t  test = map[i].test;
            uint16_t mask = map[i].mask;

            if ((value & mask) == (test & mask))
            {
                *mime = map[i].mime;
                return Match;
            }
        }

        return Fail;
    }

    return Error;
}



static Result
getOffset(const Byte* buf, size_t len, size_t at, char type, size_t* offset)
{
    /*  Fetch an indirect offset. We only implement 'bslBSL'. These
        are the only ones in use in the magic these days. The only use of .I
        is for 'SGI SoundTrack project file' which doesn't map to a MIME that
        we use.
    */
    UInt   v = 0;

    switch (type)
    {
    case 'b':
    case 'B':
        if (at + 1 >= len) return Error;
        v = buf[at];
        break;

    case 's':
        // little endian
        if (at + 2 >= len) return Error;
        v = (buf[at + 1] << 8) + buf[at];
        break;

    case 'S':
        // big endian
        if (at + 2 >= len) return Error;
        v = (buf[at] << 8) + buf[at + 1];
        break;

    case 'l':
        // little endian
        if (at + 4 >= len) return Error;
        v = (buf[at + 3] << 24) + (buf[at + 2] << 16) + (buf[at + 1] << 8) + buf[at];
        break;

    case 'L':
        // big endian
        if (at + 4 >= len) return Error;
        v = (buf[at] << 24) + (buf[at + 1] << 16) + (buf[at + 2] << 8) + buf[at + 3];
        break;
    }

    *offset = v;
    return Match;
}



static ShortMap beshortMap1[] = {
    {0xFFFC,    0xFFFE,    "audio/mpeg"},
    {0xFFF2,    0xFFFE,    "audio/mpeg"},
    {0xFFF4,    0xFFFE,    "audio/mpeg"},
    {0xFFF6,    0xFFFE,    "audio/mpeg"},
    {0xFFE2,    0xFFFE,    "audio/mpeg"},
    {0xFFF0,    0xFFF6,    "audio/x-hx-aac-adts"},
    {0x56E0,    0xFFE0,    "audio/x-mp4a-latm"},
    {0x0b77,    0xffff,    "audio/vnd.dolby.dd-raw"},
    {0x8502,    0xffff,    "text/PGP"},
    {0x55AA,    0xffff,    "application/octet-stream"},
    {0xffd8,    0xffff,    "image/jpeg"},
    {0x9900,    0xffff,    "application/x-pgp-keyring"},
    {0x9501,    0xffff,    "application/x-pgp-keyring"},
    {0x9500,    0xffff,    "application/x-pgp-keyring"},
    {0xa600,    0xffff,    "text/PGP"},
};
static const size_t beshortMap1Count = 15;

static StringMap stringMap2[] = {
    {"\x04" "%!",    sizeof("\x04" "%!") - 1,    "application/postscript"},
    {"\x06" "\x0e" "+4" "\x02" "\x05" "\x01" "\x01" "\r" "\x01" "\x02" "\x01" "\x01" "\x02",    sizeof("\x06" "\x0e" "+4" "\x02" "\x05" "\x01" "\x01" "\r" "\x01" "\x02" "\x01" "\x01" "\x02") - 1,    "application/mxf"},
    {"\t" "\x04" "\x06" "\x00" "\x00" "\x00" "\x10" "\x00",    sizeof("\t" "\x04" "\x06" "\x00" "\x00" "\x00" "\x10" "\x00") - 1,    "application/vnd.ms-excel"},
    {"\n%PDF-",    sizeof("\n%PDF-") - 1,    "application/pdf"},
    {"\x1f" "\x1e",    sizeof("\x1f" "\x1e") - 1,    "application/octet-stream"},
    {"# KDE Config File",    sizeof("# KDE Config File") - 1,    "application/x-kdelnk"},
    {"# PaCkAgE DaTaStReAm",    sizeof("# PaCkAgE DaTaStReAm") - 1,    "application/x-svr4-package"},
    {"# abook addressbook file",    sizeof("# abook addressbook file") - 1,    "application/x-abook-addressbook"},
    {"# xmcd",    sizeof("# xmcd") - 1,    "text/x-xmcd"},
    {"#! /usr/bin/env perl",    sizeof("#! /usr/bin/env perl") - 1,    "text/x-perl"},
    {"#!/usr/bin/env perl",    sizeof("#!/usr/bin/env perl") - 1,    "text/x-perl"},
    {"%!",    sizeof("%!") - 1,    "application/postscript"},
    {"%FDF-",    sizeof("%FDF-") - 1,    "application/vnd.fdf"},
    {"%PDF-",    sizeof("%PDF-") - 1,    "application/pdf"},
    {"-----BEGIN PGP MESSAGE-",    sizeof("-----BEGIN PGP MESSAGE-") - 1,    "application/pgp"},
    {"-----BEGIN PGP SIGNATURE-",    sizeof("-----BEGIN PGP SIGNATURE-") - 1,    "application/pgp-signature"},
    {".RMF" "\x00" "\x00" "\x00",    sizeof(".RMF" "\x00" "\x00" "\x00") - 1,    "application/vnd.rn-realmedia"},
    {"8BPS",    sizeof("8BPS") - 1,    "image/vnd.adobe.photoshop"},
    {"<?xml version \"",    sizeof("<?xml version \"") - 1,    "text/xml"},
    {"<?xml version=\"",    sizeof("<?xml version=\"") - 1,    "text/xml"},
    {"<?xml version='",    sizeof("<?xml version='") - 1,    "text/xml"},
    {"<BookFile",    sizeof("<BookFile") - 1,    "application/x-mif"},
    {"<MIFFile",    sizeof("<MIFFile") - 1,    "application/x-mif"},
    {"<MML",    sizeof("<MML") - 1,    "application/x-mif"},
    {"<Maker Intermediate Print File",    sizeof("<Maker Intermediate Print File") - 1,    "application/x-mif"},
    {"<MakerFile",    sizeof("<MakerFile") - 1,    "application/x-mif"},
    {"<MakerScreenFont",    sizeof("<MakerScreenFont") - 1,    "application/x-mif"},
    {"<SCRIBUSUTF8NEW Version",    sizeof("<SCRIBUSUTF8NEW Version") - 1,    "application/x-scribus"},
    {"<svg",    sizeof("<svg") - 1,    "image/svg"},
    {"AC1.2",    sizeof("AC1.2") - 1,    "image/vnd.dwg"},
    {"AC1.3",    sizeof("AC1.3") - 1,    "image/vnd.dwg"},
    {"AC1.40",    sizeof("AC1.40") - 1,    "image/vnd.dwg"},
    {"AC1.50",    sizeof("AC1.50") - 1,    "image/vnd.dwg"},
    {"AC1001",    sizeof("AC1001") - 1,    "image/vnd.dwg"},
    {"AC1002",    sizeof("AC1002") - 1,    "image/vnd.dwg"},
    {"AC1003",    sizeof("AC1003") - 1,    "image/vnd.dwg"},
    {"AC1004",    sizeof("AC1004") - 1,    "image/vnd.dwg"},
    {"AC1006",    sizeof("AC1006") - 1,    "image/vnd.dwg"},
    {"AC1009",    sizeof("AC1009") - 1,    "image/vnd.dwg"},
    {"AC1012",    sizeof("AC1012") - 1,    "image/vnd.dwg"},
    {"AC1014",    sizeof("AC1014") - 1,    "image/vnd.dwg"},
    {"AC1015",    sizeof("AC1015") - 1,    "image/vnd.dwg"},
    {"AC1018",    sizeof("AC1018") - 1,    "image/vnd.dwg"},
    {"AC1021",    sizeof("AC1021") - 1,    "image/vnd.dwg"},
    {"AC1024",    sizeof("AC1024") - 1,    "image/vnd.dwg"},
    {"AC1027",    sizeof("AC1027") - 1,    "image/vnd.dwg"},
    {"AC2.10",    sizeof("AC2.10") - 1,    "image/vnd.dwg"},
    {"AC2.21",    sizeof("AC2.21") - 1,    "image/vnd.dwg"},
    {"AC2.22",    sizeof("AC2.22") - 1,    "image/vnd.dwg"},
    {"ADIF",    sizeof("ADIF") - 1,    "audio/x-hx-aac-adif"},
    {"BPG" "\xfb",    sizeof("BPG" "\xfb") - 1,    "image/bpg"},
    {"BZh",    sizeof("BZh") - 1,    "application/x-bzip2"},
    {"Cr24",    sizeof("Cr24") - 1,    "application/x-chrome-extension"},
    {"FLV" "\x01",    sizeof("FLV" "\x01") - 1,    "video/x-flv"},
    {"GDBM",    sizeof("GDBM") - 1,    "application/x-gdbm"},
    {"GIF8",    sizeof("GIF8") - 1,    "image/gif"},
    {"II" "\x1a" "\x00" "\x00" "\x00" "HEAPCCDR",    sizeof("II" "\x1a" "\x00" "\x00" "\x00" "HEAPCCDR") - 1,    "image/x-canon-crw"},
    {"II*" "\x00",    sizeof("II*" "\x00") - 1,    "image/tiff"},
    {"II*" "\x00" "\x10" "\x00" "\x00" "\x00" "CR",    sizeof("II*" "\x00" "\x10" "\x00" "\x00" "\x00" "CR") - 1,    "image/x-canon-cr2"},
    {"II+" "\x00",    sizeof("II+" "\x00") - 1,    "image/tiff"},
    {"IIRO",    sizeof("IIRO") - 1,    "image/x-olympus-orf"},
    {"IIRS",    sizeof("IIRS") - 1,    "image/x-olympus-orf"},
    {"MAC ",    sizeof("MAC ") - 1,    "audio/x-ape"},
    {"MC0.0",    sizeof("MC0.0") - 1,    "image/vnd.dwg"},
    {"MDMP",    sizeof("MDMP") - 1,    "application/x-dmp"},
    {"MM" "\x00" "*",    sizeof("MM" "\x00" "*") - 1,    "image/tiff"},
    {"MM" "\x00" "+",    sizeof("MM" "\x00" "+") - 1,    "image/tiff"},
    {"MMOR",    sizeof("MMOR") - 1,    "image/x-olympus-orf"},
    {"MP+",    sizeof("MP+") - 1,    "audio/x-musepack"},
    {"MPCK",    sizeof("MPCK") - 1,    "audio/x-musepack"},
    {"MSCF" "\x00" "\x00" "\x00" "\x00",    sizeof("MSCF" "\x00" "\x00" "\x00" "\x00") - 1,    "application/vnd.ms-cab-compressed"},
    {"MThd",    sizeof("MThd") - 1,    "audio/midi"},
    {"OTTO",    sizeof("OTTO") - 1,    "application/vnd.ms-opentype"},
    {"P7",    sizeof("P7") - 1,    "image/x-portable-pixmap"},
    {"PDN3",    sizeof("PDN3") - 1,    "image/x-paintnet"},
    {"PK" "\x05" "\x06",    sizeof("PK" "\x05" "\x06") - 1,    "application/zip"},
    {"PK\a\bPK" "\x03" "\x04",    sizeof("PK\a\bPK" "\x03" "\x04") - 1,    "application/zip"},
    {"PO^Q`",    sizeof("PO^Q`") - 1,    "application/msword"},
    {"RE~^",    sizeof("RE~^") - 1,    "application/x-rar"},
    {"RF64" "\xff" "\xff" "\xff" "\xff" "WAVEds64",    sizeof("RF64" "\xff" "\xff" "\xff" "\xff" "WAVEds64") - 1,    "audio/x-wav"},
    {"Rar!" "\x1a" "\a" "\x00",    sizeof("Rar!" "\x1a" "\a" "\x00") - 1,    "application/x-rar"},
    {"Rar!" "\x1a" "\a" "\x01" "\x00",    sizeof("Rar!" "\x1a" "\a" "\x01" "\x00") - 1,    "application/x-rar"},
    {"SQLite format 3",    sizeof("SQLite format 3") - 1,    "application/x-sqlite3"},
    {"Xcur",    sizeof("Xcur") - 1,    "image/x-xcursor"},
    {"[BitmapInfo2]",    sizeof("[BitmapInfo2]") - 1,    "image/x-polar-monitor-bitmap"},
    {"[KDE Desktop Entry]",    sizeof("[KDE Desktop Entry]") - 1,    "application/x-kdelnk"},
    {"d13:announce-list",    sizeof("d13:announce-list") - 1,    "application/x-bittorrent"},
    {"d8:announce",    sizeof("d8:announce") - 1,    "application/x-bittorrent"},
    {"drpm",    sizeof("drpm") - 1,    "application/x-rpm"},
    {"fLaC",    sizeof("fLaC") - 1,    "audio/x-flac"},
    {"filedesc://",    sizeof("filedesc://") - 1,    "application/x-ia-arc"},
    {"gimp xcf",    sizeof("gimp xcf") - 1,    "image/x-xcf"},
    {"icns",    sizeof("icns") - 1,    "image/x-icns"},
    {"qpress10",    sizeof("qpress10") - 1,    "application/x-qpress"},
    {"tfMR",    sizeof("tfMR") - 1,    "application/x-winhelp-fts"},
    {"xar!",    sizeof("xar!") - 1,    "application/x-xar"},
    {"{\\rtf",    sizeof("{\\rtf") - 1,    "text/rtf"},
    {"\x89" "HDF\r\n" "\x1a" "\n",    sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1,    "application/x-hdf"},
    {"\x89" "PNG\r\n" "\x1a" "\n" "\x00" "\x00" "\x00" "\rIHDR",    sizeof("\x89" "PNG\r\n" "\x1a" "\n" "\x00" "\x00" "\x00" "\rIHDR") - 1,    "image/png"},
    {"\x8a" "MNG",    sizeof("\x8a" "MNG") - 1,    "video/x-mng"},
    {"\x94" "\xa6" ".",    sizeof("\x94" "\xa6" ".") - 1,    "application/msword"},
    {"\xd9" "\xd9" "\xf7",    sizeof("\xd9" "\xd9" "\xf7") - 1,    "application/cbor"},
    {"\xdb" "\xa5" "-" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00") - 1,    "application/msword"},
    {"\xdb" "\xa5" "-" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00") - 1,    "application/msword"},
    {"\xdb" "\xa5" "-" "\x00" "\x00" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00" "\x00" "\x00") - 1,    "application/msword"},
    {"\xf7" "\x02",    sizeof("\xf7" "\x02") - 1,    "application/x-dvi"},
    {"\xfd" "7zXZ" "\x00",    sizeof("\xfd" "7zXZ" "\x00") - 1,    "application/x-xz"},
    {"\xff" "\x1f",    sizeof("\xff" "\x1f") - 1,    "application/octet-stream"},
};
static const size_t stringMap2Count = 108;

static StringMap stringMap3[] = {
    {"3",    sizeof("3") - 1,    "application/vnd.cups-raster"},
};
static const size_t stringMap3Count = 1;

static Result
runTests(const Byte* buf, size_t len, const char** mime)
{
    Result rslt;
    Bool   haveError = False;
    size_t off0, off1, off2, off3, off4, off5, off6, off7, off8, off9, off10, off11, off12, off13, off14, off15, off16, off17, off18;

    // line 1009
    rslt = beShortGroup(buf, len, beshortMap1, beshortMap1Count, mime);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        return Match;
    }

    // line 185
    off0 = 0;
    rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 186
        off1 = 0;
        rslt = byteMatch(buf, len, 9, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 187
            off2 = 16;
            rslt = beLongMatch(buf, len, 0x3030, CompareEq, 0xfe00f0f0, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 188
                off3 = 0;
                rslt = byteMatch(buf, len, 10, CompareLt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 190
                    off5 = 18;
                    rslt = regexMatch(buf, len, "[0-9][0-9][0-9][0-9][0-9][0-9]", &off5, 0, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 191
                        off6 = 0;
                        rslt = byteMatch(buf, len, 10, CompareLt, 0xffffffff, &off6);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            *mime = "application/x-zmachine";
                            return Match;
                        }
                    }
                }
            }
        }
    }

    // line 265
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x3C423FC9, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 266
        off1 = 4;
        rslt = beLongMatch(buf, len, 0x6A87C2CF, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-adrift";
            return Match;
        }
    }

    // line 747
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000100, CompareEq, 0xFFFFFF00, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 748
        off1 = 3;
        rslt = byteMatch(buf, len, 0xBA, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg";
            return Match;
        }
        // line 759
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg4-generic";
            return Match;
        }
        // line 831
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB5, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg4-generic";
            return Match;
        }
        // line 842
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg";
            return Match;
        }
    }

    // line 961
    off0 = 0;
    rslt = beShortMatch(buf, len, 0xFFFA, CompareEq, 0xFFFE, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 963
        off1 = 2;
        rslt = byteMatch(buf, len, 0x10, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 965
        off1 = 2;
        rslt = byteMatch(buf, len, 0x20, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 967
        off1 = 2;
        rslt = byteMatch(buf, len, 0x30, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 969
        off1 = 2;
        rslt = byteMatch(buf, len, 0x40, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 971
        off1 = 2;
        rslt = byteMatch(buf, len, 0x50, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 973
        off1 = 2;
        rslt = byteMatch(buf, len, 0x60, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 975
        off1 = 2;
        rslt = byteMatch(buf, len, 0x70, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 977
        off1 = 2;
        rslt = byteMatch(buf, len, 0x80, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 979
        off1 = 2;
        rslt = byteMatch(buf, len, 0x90, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 981
        off1 = 2;
        rslt = byteMatch(buf, len, 0xA0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 983
        off1 = 2;
        rslt = byteMatch(buf, len, 0xB0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 985
        off1 = 2;
        rslt = byteMatch(buf, len, 0xC0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 987
        off1 = 2;
        rslt = byteMatch(buf, len, 0xD0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 989
        off1 = 2;
        rslt = byteMatch(buf, len, 0xE0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
    }

    // line 1310
    off0 = 4;
    rslt = leShortMatch(buf, len, 0xAF11, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1312
        off1 = 8;
        rslt = leShortMatch(buf, len, 320, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 1313
            off2 = 10;
            rslt = leShortMatch(buf, len, 200, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 1314
                off3 = 12;
                rslt = leShortMatch(buf, len, 8, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "video/x-fli";
                    return Match;
                }
            }
        }
    }

    // line 1323
    off0 = 4;
    rslt = leShortMatch(buf, len, 0xAF12, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1325
        off1 = 12;
        rslt = leShortMatch(buf, len, 8, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-flc";
            return Match;
        }
    }

    // line 1380
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x3026b275, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/x-ms-asf";
        return Match;
    }

    // line 2811
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x1ee7ff00, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-eet";
        return Match;
    }

    // line 2836
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10201A7A, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "x-epoc/x-sisx-app";
        return Match;
    }

    // line 3133
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x2e7261fd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "audio/x-pn-realaudio";
        return Match;
    }

    // line 4557
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x4d4d, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4558
        off1 = 6;
        rslt = leShortMatch(buf, len, 0x2, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 4559
            off2 = 8;
            rslt = leLongMatch(buf, len, 0xa, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 4560
                off3 = 16;
                rslt = leShortMatch(buf, len, 0x3d3d, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "image/x-3ds";
                    return Match;
                }
            }
        }
    }

    // line 4585
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafebabe, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4586
        off1 = 4;
        rslt = beLongMatch(buf, len, 30, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-java-applet";
            return Match;
        }
    }

    // line 4601
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafed00d, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-java-pack200";
        return Match;
    }

    // line 4607
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafed00d, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-java-pack200";
        return Match;
    }

    // line 4620
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafebabe, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4621
        off1 = 4;
        rslt = beLongMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-mach-binary";
            return Match;
        }
        // line 4624
        off1 = 4;
        rslt = beLongMatch(buf, len, 1, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 4625
            off2 = 4;
            rslt = beLongMatch(buf, len, 20, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-mach-binary";
                return Match;
            }
        }
    }

    // line 5124
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x1f1f, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 5130
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x1fff, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 5136
    off0 = 0;
    rslt = leShortMatch(buf, len, 0145405, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 5258
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x5d, CompareEq, 0xffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 5259
        off1 = 12;
        rslt = leShortMatch(buf, len, 0xff, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-lzma";
            return Match;
        }
    }

    // line 5279
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184d2204, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 5282
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184c2103, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 5284
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184c2102, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 5315
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB522, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5317
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB523, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5319
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB524, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5321
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB525, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5323
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB526, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5325
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB527, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5328
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xFD2FB528, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd";
        return Match;
    }

    // line 5333
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xEC30A437, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-zstd-dictionary";
        return Match;
    }

    // line 6519
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x13579acd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6521
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x13579ace, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6523
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x13579acf, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6525
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x13579acd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6527
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x13579ace, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6529
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x13579acf, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 6661
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000C20, CompareLt, 0x0000FFFF, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6750
        off1 = 0;
        rslt = byteMatch(buf, len, 1, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 6753
            off2 = 0;
            rslt = byteMatch(buf, len, 0x03, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6756
            off2 = 0;
            rslt = byteMatch(buf, len, 0x04, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6759
            off2 = 0;
            rslt = byteMatch(buf, len, 0x05, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6761
            off2 = 0;
            rslt = byteMatch(buf, len, 0x30, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6763
            off2 = 0;
            rslt = byteMatch(buf, len, 0x31, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6766
            off2 = 0;
            rslt = byteMatch(buf, len, 0x32, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6769
            off2 = 0;
            rslt = byteMatch(buf, len, 0x43, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6775
            off2 = 0;
            rslt = byteMatch(buf, len, 0x7b, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6781
            off2 = 0;
            rslt = byteMatch(buf, len, 0x83, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6784
            off2 = 0;
            rslt = byteMatch(buf, len, 0x87, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6790
            off2 = 0;
            rslt = byteMatch(buf, len, 0x8B, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6793
            off2 = 0;
            rslt = byteMatch(buf, len, 0x8E, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6801
            off2 = 0;
            rslt = byteMatch(buf, len, 0xCB, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6804
            off2 = 0;
            rslt = byteMatch(buf, len, 0xE5, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 6809
            off2 = 0;
            rslt = byteMatch(buf, len, 0xF5, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            *mime = "application/x-dbf";
            return Match;
        }
    }

    // line 6843
    off0 = 16;
    rslt = byteMatch(buf, len, 4, CompareLt, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-dbt";
        return Match;
    }

    // line 6977
    off0 = 4;
    rslt = beLongMatch(buf, len, 0xefcdab89, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6979
        off1 = 132;
        rslt = beLongMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ms-ese";
            return Match;
        }
    }

    // line 7646
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x0ef1fab9, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 7700
        off1 = 16;
        rslt = leShortMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/octet-stream";
            return Match;
        }
        // line 7702
        off1 = 16;
        rslt = leShortMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-object";
            return Match;
        }
        // line 7704
        off1 = 16;
        rslt = leShortMatch(buf, len, 2, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-executable";
            return Match;
        }
        // line 7706
        off1 = 16;
        rslt = leShortMatch(buf, len, 3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-sharedlib";
            return Match;
        }
        // line 7708
        off1 = 16;
        rslt = leShortMatch(buf, len, 4, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-coredump";
            return Match;
        }
    }

    // line 8008
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10000037, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8015
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x1000006D, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8016
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x1000007D, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-epoc-sketch";
                return Match;
            }
            // line 8019
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x1000007F, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-word";
                return Match;
            }
            // line 8021
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000085, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-opl";
                return Match;
            }
            // line 8024
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000088, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-sheet";
                return Match;
            }
        }
        // line 8027
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x10000073, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-epoc-opo";
            return Match;
        }
        // line 8029
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x10000074, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-epoc-app";
            return Match;
        }
    }

    // line 8037
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10000050, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8038
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x1000006D, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8039
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000084, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-agenda";
                return Match;
            }
            // line 8041
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000086, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-data";
                return Match;
            }
            // line 8043
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000CEA, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-jotter";
                return Match;
            }
        }
    }

    // line 8257
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x0100, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8261
        off1 = 66;
        rslt = leShortMatch(buf, len, 0x0081, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8267
            off2 = 101;
            rslt = getOffset(buf, len, off2, 'l', &off2);
            if (rslt < 0) haveError = True;
            else
            {
                rslt = stringMatch(buf, len, "Postscript", sizeof("Postscript") - 1, &off2, CompareEq, 0|MatchLower);
                if (rslt < 0) haveError = True;
            }
            if (rslt > 0)
            {
                *mime = "application/x-font-pfm";
                return Match;
            }
        }
    }

    // line 8304
    off0 = 0;
    rslt = beLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-font-sfn";
        return Match;
    }

    // line 8308
    off0 = 0;
    rslt = leLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8309
        off1 = 104;
        rslt = leLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-font-sfn";
            return Match;
        }
    }

    // line 10728
    off0 = 0;
    rslt = beQuadMatch(buf, len, 0, CompareEq, 0x00FeC400000000C0, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10752
        off1 = 2;
        rslt = byteMatch(buf, len, 34, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-tga";
            return Match;
        }
    }

    // line 11417
    off0 = 0;
    rslt = beLongMatch(buf, len, 100, CompareGt, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11418
        off1 = 8;
        rslt = beLongMatch(buf, len, 3, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 11419
            off2 = 12;
            rslt = beLongMatch(buf, len, 33, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 11420
                off3 = 4;
                rslt = beLongMatch(buf, len, 7, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "image/x-xwindowdump";
                    return Match;
                }
            }
        }
    }

    // line 11485
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x0a000000, CompareEq, 0xffF8fe00, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11487
        off1 = 3;
        rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 11489
            off2 = 1;
            rslt = byteMatch(buf, len, 6, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 11490
                off3 = 1;
                rslt = byteMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "image/x-pcx";
                    return Match;
                }
            }
        }
    }

    // line 11755
    off0 = 0;
    rslt = leLongMatch(buf, len, 20000630, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-exr";
        return Match;
    }

    // line 11815
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x0e031301, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 14363
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x000000E9, CompareEq, 0x804000E9, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 14368
        off1 = 11;
        rslt = leShortMatch(buf, len, 0, CompareEq, 0x001f, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 14369
            off2 = 11;
            rslt = leShortMatch(buf, len, 32769, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 14370
                off3 = 11;
                rslt = leShortMatch(buf, len, 31, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 14371
                    off4 = 21;
                    rslt = byteMatch(buf, len, 0xF0, CompareEq, 0xf0, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 14509
                        off5 = 21;
                        rslt = byteMatch(buf, len, 0xF8, CompareEq|CompareNot, 0xffffffff, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 14511
                            off6 = 54;
                            rslt = !stringEqual(buf, len, "FAT16", sizeof("FAT16") - 1, &off6);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                // line 14513
                                off7 = 11;
                                rslt = getOffset(buf, len, off7, 's', &off7);
                                if (rslt < 0) haveError = True;
                                else
                                {
                                    rslt = leLongMatch(buf, len, 0x00ffffF0, CompareEq, 0x00ffffF0, &off7);
                                    if (rslt < 0) haveError = True;
                                }
                                if (rslt > 0)
                                {
                                    *mime = "application/x-ima";
                                    return Match;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // line 15941
    off0 = 0;
    rslt = leLongMatch(buf, len, 0xfeedface, CompareEq, 0xfffffffe, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-mach-binary";
        return Match;
    }

    // line 15946
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xfeedface, CompareEq, 0xfffffffe, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-mach-binary";
        return Match;
    }

    // line 16290
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x1a45dfa3, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16292
        off1 = 4;
        rslt = stringSearch(buf, len, "B" "\x82", sizeof("B" "\x82") - 1, &off1, 4096, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 16294
            off2 = 1;
            off2 += off1;
            rslt = stringMatch(buf, len, "webm", sizeof("webm") - 1, &off2, CompareEq, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/webm";
                return Match;
            }
            // line 16296
            off2 = 1;
            off2 += off1;
            rslt = stringMatch(buf, len, "matroska", sizeof("matroska") - 1, &off2, CompareEq, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/x-matroska";
                return Match;
            }
        }
    }

    // line 17067
    off0 = 0;
    rslt = leQuadMatch(buf, len, 0xffffffff, CompareEq, 0x07a0ffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-dosdriver";
        return Match;
    }

    // line 17215
    off0 = 0;
    rslt = byteMatch(buf, len, 0xb8, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17217
        off1 = 0;
        rslt = !stringEqual(buf, len, "\xb8" "\xc0" "\a" "\x8e", sizeof("\xb8" "\xc0" "\a" "\x8e") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17220
            off2 = 1;
            rslt = leLongMatch(buf, len, 0x21CD4CFe, CompareEq, 0xFFFFFFFe, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-c32-comboot-syslinux-exec";
                return Match;
            }
        }
    }

    // line 17311
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x31be0000, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17317
    off0 = 4;
    rslt = leLongMatch(buf, len, 0, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17318
        off1 = 0;
        rslt = beLongMatch(buf, len, 0xfe320000, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
        // line 17321
        off1 = 0;
        rslt = beLongMatch(buf, len, 0xfe340000, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
        // line 17324
        off1 = 0;
        rslt = beLongMatch(buf, len, 0xfe37001c, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
        // line 17327
        off1 = 0;
        rslt = beLongMatch(buf, len, 0xfe370023, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
    }

    // line 17364
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00001a00, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17368
        off1 = 20;
        rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17369
            off2 = 20;
            rslt = byteMatch(buf, len, 32, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.lotus-1-2-3";
                return Match;
            }
        }
    }

    // line 17432
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000200, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17437
        off1 = 7;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17439
            off2 = 6;
            rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.lotus-1-2-3";
                return Match;
            }
        }
    }

    // line 17558
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000100, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17569
        off1 = 18;
        rslt = leLongMatch(buf, len, 0x00000006, CompareSet, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17572
            off3 = 0;
            rslt = beLongMatch(buf, len, 0x00000100, CompareEq, 0xffffffff, &off3);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-icon";
                return Match;
            }
            // line 17584
            off3 = 0;
            rslt = beLongMatch(buf, len, 0x00000200, CompareEq, 0xffffffff, &off3);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-win-bitmap";
                return Match;
            }
        }
    }

    // line 17724
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xC5D0D3C6, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-eps";
        return Match;
    }

    // line 17950
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x0100, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17956
        off2 = 0;
        rslt = beLongMatch(buf, len, 0x0001a364, CompareEq|CompareNot, 0xffffffff, &off2);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17958
            off3 = 2;
            rslt = beShortMatch(buf, len, 0x0008, CompareEq|CompareNot, 0xffffffff, &off3);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 17967
                off6 = 8;
                rslt = beQuadMatch(buf, len, 0x2e01010454010203, CompareEq|CompareNot, 0xffffffff, &off6);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 17969
                    off7 = 8;
                    rslt = beQuadMatch(buf, len, 0x5dee74ad1aa56394, CompareEq|CompareNot, 0xffffffff, &off7);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "image/g3fax";
                        return Match;
                    }
                }
            }
        }
    }

    // line 17976
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x1400, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/g3fax";
        return Match;
    }

    // line 21380
    off0 = 0;
    rslt = beShortMatch(buf, len, 0x4552, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21384
        off1 = 2;
        rslt = beShortMatch(buf, len, 0, CompareEq, 0xf1FF, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-apple-diskimage";
            return Match;
        }
    }

    // line 21814
    off0 = 0x400;
    rslt = beShortMatch(buf, len, 0x4244, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21817
        off1 = 0x40e;
        rslt = beShortMatch(buf, len, 0x0003, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 21819
            off2 = 0x424;
            rslt = byteMatch(buf, len, 28, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-apple-diskimage";
                return Match;
            }
        }
    }

    // line 21907
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xedabeedb, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-rpm";
        return Match;
    }

    // line 25138
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x00035f3f, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25141
        off1 = 4;
        rslt = getOffset(buf, len, off1, 'l', &off1);
        off1 += 9;
        if (rslt < 0) haveError = True;
        else
        {
            rslt = leShortMatch(buf, len, 0x293B, CompareEq, 0xffffffff, &off1);
            if (rslt < 0) haveError = True;
        }
        if (rslt > 0)
        {
            // line 25143
            off2 = 0xD4;
            rslt = stringEqual(buf, len, "bmf" "\x01" "\x00", sizeof("bmf" "\x01" "\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-winhelp";
                return Match;
            }
            // line 25146
            off2 = 0xD4;
            rslt = !stringEqual(buf, len, "bmf" "\x01" "\x00", sizeof("bmf" "\x01" "\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25148
                off3 = 4;
                rslt = getOffset(buf, len, off3, 'l', &off3);
                off3 += 0x65;
                if (rslt < 0) haveError = True;
                else
                {
                    rslt = stringMatch(buf, len, "|Pete", sizeof("|Pete") - 1, &off3, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                }
                if (rslt > 0)
                {
                    *mime = "application/x-winhelp";
                    return Match;
                }
                // line 25153
                off3 = 4;
                rslt = getOffset(buf, len, off3, 'l', &off3);
                off3 += 0x65;
                if (rslt < 0) haveError = True;
                else
                {
                    rslt = stringMatch(buf, len, "|Pete", sizeof("|Pete") - 1, &off3, CompareEq|CompareNot, 0);
                    if (rslt < 0) haveError = True;
                }
                if (rslt > 0)
                {
                    *mime = "application/x-winhelp";
                    return Match;
                    // line 25157
                    off4 = 16;
                    rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off4, 0x49AF, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 25159
                        off5 = 4;
                        off5 += off4;
                        rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 25161
                            off6 = 0;
                            off6 += off5;
                            rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off6, 0x69AF, 0);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                // line 25163
                                off7 = 4;
                                off7 += off6;
                                rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off7);
                                if (rslt < 0) haveError = True;
                                if (rslt > 0)
                                {
                                    // line 25164
                                    off8 = 0;
                                    off8 += off7;
                                    rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off8, 0x49AF, 0);
                                    if (rslt < 0) haveError = True;
                                    if (rslt > 0)
                                    {
                                        // line 25166
                                        off9 = 4;
                                        off9 += off8;
                                        rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off9);
                                        if (rslt < 0) haveError = True;
                                        if (rslt > 0)
                                        {
                                            // line 25167
                                            off10 = 0;
                                            off10 += off9;
                                            rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off10, 0x49AF, 0);
                                            if (rslt < 0) haveError = True;
                                            if (rslt > 0)
                                            {
                                                // line 25169
                                                off11 = 4;
                                                off11 += off10;
                                                rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off11);
                                                if (rslt < 0) haveError = True;
                                                if (rslt > 0)
                                                {
                                                    // line 25170
                                                    off12 = 0;
                                                    off12 += off11;
                                                    rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off12, 0x49AF, 0);
                                                    if (rslt < 0) haveError = True;
                                                    if (rslt > 0)
                                                    {
                                                        // line 25172
                                                        off13 = 4;
                                                        off13 += off12;
                                                        rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off13);
                                                        if (rslt < 0) haveError = True;
                                                        if (rslt > 0)
                                                        {
                                                            // line 25173
                                                            off14 = 0;
                                                            off14 += off13;
                                                            rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off14, 0x49AF, 0);
                                                            if (rslt < 0) haveError = True;
                                                            if (rslt > 0)
                                                            {
                                                                // line 25175
                                                                off15 = 4;
                                                                off15 += off14;
                                                                rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off15);
                                                                if (rslt < 0) haveError = True;
                                                                if (rslt > 0)
                                                                {
                                                                    // line 25176
                                                                    off16 = 0;
                                                                    off16 += off15;
                                                                    rslt = stringSearch(buf, len, "l" "\x03", sizeof("l" "\x03") - 1, &off16, 0x49AF, 0);
                                                                    if (rslt < 0) haveError = True;
                                                                    if (rslt > 0)
                                                                    {
                                                                        // line 25180
                                                                        off17 = 4;
                                                                        off17 += off16;
                                                                        rslt = leShortMatch(buf, len, 1, CompareEq|CompareNot, 0xffffffff, &off17);
                                                                        if (rslt < 0) haveError = True;
                                                                        if (rslt > 0)
                                                                        {
                                                                            *mime = "application/winhelp";
                                                                            return Match;
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // line 25421
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x0000, CompareEq, 0xFeFe, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25424
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x00000000, CompareEq, 0xFCffFe00, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 25426
            off2 = 68;
            rslt = leLongMatch(buf, len, 0x57, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25429
                off3 = 68;
                rslt = getOffset(buf, len, off3, 'l', &off3);
                off3 -= 1;
                if (rslt < 0) haveError = True;
                else
                {
                    rslt = beLongMatch(buf, len, 0x00400018, CompareEq, 0xffE0C519, &off3);
                    if (rslt < 0) haveError = True;
                }
                if (rslt > 0)
                {
                    *mime = "application/x-pnf";
                    return Match;
                }
            }
        }
    }

    // line 25966
    off0 = 0;
    rslt = byteMatch(buf, len, 0x80, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25969
        off1 = 1;
        rslt = leShortMatch(buf, len, 1022, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 25971
            off2 = 1;
            rslt = leShortMatch(buf, len, 0, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25973
                off3 = 3;
                rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 25975
                    off4 = 4;
                    rslt = regexMatch(buf, len, "[a-zA-Z_/]{1,8}[.]", &off4, 0, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/x-object";
                        return Match;
                    }
                }
            }
        }
    }

    // line 1226
    rslt = stringEqualMap(buf, len, stringMap2, stringMap2Count, mime);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        return Match;
    }

    // line 204
    off0 = 0;
    rslt = stringEqual(buf, len, "Glul", sizeof("Glul") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 208
        off1 = 36;
        rslt = stringEqual(buf, len, "Info", sizeof("Info") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-glulx";
            return Match;
        }
    }

    // line 219
    off0 = 0;
    rslt = stringEqual(buf, len, "TADS2 bin", sizeof("TADS2 bin") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 221
        off1 = 9;
        rslt = beLongMatch(buf, len, 0x0A0D1A00, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 222
            off2 = 13;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-tads";
                return Match;
            }
        }
    }

    // line 225
    off0 = 0;
    rslt = stringEqual(buf, len, "TADS2 rsc", sizeof("TADS2 rsc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 227
        off1 = 9;
        rslt = beLongMatch(buf, len, 0x0A0D1A00, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 228
            off2 = 13;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-tads";
                return Match;
            }
        }
    }

    // line 233
    off0 = 0;
    rslt = stringEqual(buf, len, "TADS2 save/g", sizeof("TADS2 save/g") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 235
        off1 = 12;
        rslt = beLongMatch(buf, len, 0x0A0D1A00, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 236
            off2 = 16;
            rslt = getOffset(buf, len, off2, 's', &off2);
            off2 += 32;
            if (rslt < 0) haveError = True;
            else
            {
                rslt = stringMatch(buf, len, "\x00", sizeof("\x00") - 1, &off2, CompareGt, 0);
                if (rslt < 0) haveError = True;
            }
            if (rslt > 0)
            {
                *mime = "application/x-tads";
                return Match;
            }
        }
    }

    // line 240
    off0 = 0;
    rslt = stringEqual(buf, len, "TADS2 save", sizeof("TADS2 save") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 242
        off1 = 10;
        rslt = beLongMatch(buf, len, 0x0A0D1A00, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 243
            off2 = 14;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-tads";
                return Match;
            }
        }
    }

    // line 252
    off0 = 0;
    rslt = stringEqual(buf, len, "T3-state-v", sizeof("T3-state-v") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 253
        off1 = 14;
        rslt = stringEqual(buf, len, "\r\n" "\x1a", sizeof("\r\n" "\x1a") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-t3vm-image";
            return Match;
        }
    }

    // line 556
    off0 = 4;
    rslt = stringEqual(buf, len, "moov", sizeof("moov") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/quicktime";
        return Match;
    }

    // line 562
    off0 = 4;
    rslt = stringEqual(buf, len, "mdat", sizeof("mdat") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/quicktime";
        return Match;
    }

    // line 570
    off0 = 4;
    rslt = stringEqual(buf, len, "idsc", sizeof("idsc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-quicktime";
        return Match;
    }

    // line 574
    off0 = 4;
    rslt = stringEqual(buf, len, "pckg", sizeof("pckg") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-quicktime-player";
        return Match;
    }

    // line 579
    off0 = 4;
    rslt = stringEqual(buf, len, "ftyp", sizeof("ftyp") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 602
        off1 = 11;
        rslt = byteMatch(buf, len, 6, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 611
        off1 = 11;
        rslt = byteMatch(buf, len, 7, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 614
        off1 = 11;
        rslt = byteMatch(buf, len, 7, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 587
        off1 = 8;
        rslt = stringEqual(buf, len, "3g2", sizeof("3g2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp2";
            return Match;
        }
        // line 597
        off1 = 8;
        rslt = stringEqual(buf, len, "3ge", sizeof("3ge") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 616
        off1 = 8;
        rslt = stringEqual(buf, len, "avc1", sizeof("avc1") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 631
        off1 = 8;
        rslt = stringEqual(buf, len, "dash", sizeof("dash") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 642
        off1 = 8;
        rslt = stringEqual(buf, len, "dvr1", sizeof("dvr1") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/vnd.dvb.file";
            return Match;
        }
        // line 644
        off1 = 8;
        rslt = stringEqual(buf, len, "dvt1", sizeof("dvt1") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/vnd.dvb.file";
            return Match;
        }
        // line 646
        off1 = 8;
        rslt = stringEqual(buf, len, "F4V", sizeof("F4V") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 648
        off1 = 8;
        rslt = stringEqual(buf, len, "F4P", sizeof("F4P") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 650
        off1 = 8;
        rslt = stringEqual(buf, len, "F4A", sizeof("F4A") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 652
        off1 = 8;
        rslt = stringEqual(buf, len, "F4B", sizeof("F4B") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 656
        off1 = 8;
        rslt = stringEqual(buf, len, "iso2", sizeof("iso2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 658
        off1 = 8;
        rslt = stringEqual(buf, len, "isom", sizeof("isom") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 662
        off1 = 8;
        rslt = stringEqual(buf, len, "JP2", sizeof("JP2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jp2";
            return Match;
        }
        // line 665
        off1 = 8;
        rslt = stringEqual(buf, len, "jpm", sizeof("jpm") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpm";
            return Match;
        }
        // line 667
        off1 = 8;
        rslt = stringEqual(buf, len, "jpx", sizeof("jpx") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpx";
            return Match;
        }
        // line 669
        off1 = 8;
        rslt = stringEqual(buf, len, "KDDI", sizeof("KDDI") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp2";
            return Match;
        }
        // line 671
        off1 = 8;
        rslt = stringEqual(buf, len, "M4A", sizeof("M4A") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-m4a";
            return Match;
        }
        // line 673
        off1 = 8;
        rslt = stringEqual(buf, len, "M4B", sizeof("M4B") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 675
        off1 = 8;
        rslt = stringEqual(buf, len, "M4P", sizeof("M4P") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 677
        off1 = 8;
        rslt = stringEqual(buf, len, "M4V", sizeof("M4V") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-m4v";
            return Match;
        }
        // line 679
        off1 = 8;
        rslt = stringEqual(buf, len, "M4VH", sizeof("M4VH") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-m4v";
            return Match;
        }
        // line 681
        off1 = 8;
        rslt = stringEqual(buf, len, "M4VP", sizeof("M4VP") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-m4v";
            return Match;
        }
        // line 683
        off1 = 8;
        rslt = stringEqual(buf, len, "mj2s", sizeof("mj2s") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mj2";
            return Match;
        }
        // line 685
        off1 = 8;
        rslt = stringEqual(buf, len, "mjp2", sizeof("mjp2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mj2";
            return Match;
        }
        // line 687
        off1 = 8;
        rslt = stringEqual(buf, len, "mmp4", sizeof("mmp4") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 689
        off1 = 8;
        rslt = stringEqual(buf, len, "mobi", sizeof("mobi") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 692
        off1 = 8;
        rslt = stringEqual(buf, len, "mp41", sizeof("mp41") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 694
        off1 = 8;
        rslt = stringEqual(buf, len, "mp42", sizeof("mp42") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 699
        off1 = 8;
        rslt = stringEqual(buf, len, "mmp4", sizeof("mmp4") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 702
        off1 = 8;
        rslt = stringEqual(buf, len, "mqt", sizeof("mqt") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/quicktime";
            return Match;
        }
        // line 704
        off1 = 8;
        rslt = stringEqual(buf, len, "MSNV", sizeof("MSNV") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 706
        off1 = 8;
        rslt = stringEqual(buf, len, "NDAS", sizeof("NDAS") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 708
        off1 = 8;
        rslt = stringEqual(buf, len, "NDSC", sizeof("NDSC") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 710
        off1 = 8;
        rslt = stringEqual(buf, len, "NDSH", sizeof("NDSH") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 712
        off1 = 8;
        rslt = stringEqual(buf, len, "NDSM", sizeof("NDSM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 714
        off1 = 8;
        rslt = stringEqual(buf, len, "NDSP", sizeof("NDSP") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 716
        off1 = 8;
        rslt = stringEqual(buf, len, "NDSS", sizeof("NDSS") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 718
        off1 = 8;
        rslt = stringEqual(buf, len, "NDXC", sizeof("NDXC") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 720
        off1 = 8;
        rslt = stringEqual(buf, len, "NDXH", sizeof("NDXH") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 722
        off1 = 8;
        rslt = stringEqual(buf, len, "NDXM", sizeof("NDXM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 724
        off1 = 8;
        rslt = stringEqual(buf, len, "NDXP", sizeof("NDXP") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 726
        off1 = 8;
        rslt = stringEqual(buf, len, "NDXS", sizeof("NDXS") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 732
        off1 = 8;
        rslt = stringEqual(buf, len, "qt", sizeof("qt") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/quicktime";
            return Match;
        }
        // line 618
        off1 = 8;
        rslt = stringMatch(buf, len, "qt", sizeof("qt") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/quicktime";
            return Match;
        }
        // line 660
        off1 = 8;
        rslt = stringMatch(buf, len, "jp2", sizeof("jp2") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jp2";
            return Match;
        }
    }

    // line 1735
    off0 = 257;
    rslt = stringEqual(buf, len, "ustar" "\x00", sizeof("ustar" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tar";
        return Match;
    }

    // line 1737
    off0 = 257;
    rslt = stringEqual(buf, len, "ustar  " "\x00", sizeof("ustar  " "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tar";
        return Match;
    }

    // line 2272
    off0 = 0;
    rslt = stringEqual(buf, len, "\xe9" "," "\x01" "JAM ", sizeof("\xe9" "," "\x01" "JAM ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 2287
        off1 = 2;
        rslt = stringEqual(buf, len, "-", sizeof("-") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2289
            off2 = 6;
            rslt = stringEqual(buf, len, "-", sizeof("-") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2291
                off3 = 20;
                rslt = byteMatch(buf, len, 4, CompareLt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2293
                    off4 = 3;
                    rslt = regexMatch(buf, len, "^(lh[0-9a-ex]|lz[s2-8]|pm[012]|pc1)", &off4, 0, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/x-lzh-compressed";
                        return Match;
                    }
                }
            }
        }
    }

    // line 2508
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 2509
        off1 = 30;
        rslt = beLongMatch(buf, len, 0x6d696d65, CompareEq|CompareNot, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2510
            off2 = 4;
            rslt = byteMatch(buf, len, 0x00, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2513
            off2 = 4;
            rslt = byteMatch(buf, len, 0x09, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2516
            off2 = 4;
            rslt = byteMatch(buf, len, 0x0a, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2519
            off2 = 4;
            rslt = byteMatch(buf, len, 0x0b, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2525
            off2 = 4;
            rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2522
            off2 = 0x161;
            rslt = stringEqual(buf, len, "WINZIP", sizeof("WINZIP") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
        }
        // line 2640
        off1 = 26;
        rslt = getOffset(buf, len, off1, 's', &off1);
        off1 += 30;
        if (rslt < 0) haveError = True;
        else
        {
            rslt = leShortMatch(buf, len, 0xcafe, CompareEq, 0xffffffff, &off1);
            if (rslt < 0) haveError = True;
        }
        if (rslt > 0)
        {
            *mime = "application/java-archive";
            return Match;
        }
        // line 2644
        off1 = 26;
        rslt = getOffset(buf, len, off1, 's', &off1);
        off1 += 30;
        if (rslt < 0) haveError = True;
        else
        {
            rslt = leShortMatch(buf, len, 0xcafe, CompareEq|CompareNot, 0xffffffff, &off1);
            if (rslt < 0) haveError = True;
        }
        if (rslt > 0)
        {
            // line 2645
            off2 = 26;
            rslt = !stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetype", sizeof("\b" "\x00" "\x00" "\x00" "mimetype") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2646
                off3 = 30;
                rslt = stringEqual(buf, len, "Payload/", sizeof("Payload/") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2647
                    off4 = 38;
                    rslt = stringSearch(buf, len, ".app/", sizeof(".app/") - 1, &off4, 64, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/x-ios-app";
                        return Match;
                    }
                }
            }
        }
        // line 2653
        off1 = 26;
        rslt = getOffset(buf, len, off1, 's', &off1);
        off1 += 30;
        if (rslt < 0) haveError = True;
        else
        {
            rslt = leShortMatch(buf, len, 0xcafe, CompareEq|CompareNot, 0xffffffff, &off1);
            if (rslt < 0) haveError = True;
        }
        if (rslt > 0)
        {
            // line 2654
            off2 = 26;
            rslt = !stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetype", sizeof("\b" "\x00" "\x00" "\x00" "mimetype") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
        }
        // line 2533
        off1 = 26;
        rslt = stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetypeapplication/", sizeof("\b" "\x00" "\x00" "\x00" "mimetypeapplication/") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2572
            off2 = 50;
            rslt = stringEqual(buf, len, "vnd.oasis.opendocument.", sizeof("vnd.oasis.opendocument.") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2573
                off3 = 73;
                rslt = stringEqual(buf, len, "text", sizeof("text") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2574
                    off4 = 77;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text";
                        return Match;
                    }
                    // line 2576
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-template";
                        return Match;
                    }
                    // line 2578
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-web", sizeof("-web") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-web";
                        return Match;
                    }
                    // line 2580
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-master", sizeof("-master") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-master";
                        return Match;
                    }
                }
                // line 2582
                off3 = 73;
                rslt = stringEqual(buf, len, "graphics", sizeof("graphics") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2583
                    off4 = 81;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.graphics";
                        return Match;
                    }
                    // line 2585
                    off4 = 81;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.graphics-template";
                        return Match;
                    }
                }
                // line 2587
                off3 = 73;
                rslt = stringEqual(buf, len, "presentation", sizeof("presentation") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2588
                    off4 = 85;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.presentation";
                        return Match;
                    }
                    // line 2590
                    off4 = 85;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.presentation-template";
                        return Match;
                    }
                }
                // line 2592
                off3 = 73;
                rslt = stringEqual(buf, len, "spreadsheet", sizeof("spreadsheet") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2593
                    off4 = 84;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.spreadsheet";
                        return Match;
                    }
                    // line 2595
                    off4 = 84;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.spreadsheet-template";
                        return Match;
                    }
                }
                // line 2597
                off3 = 73;
                rslt = stringEqual(buf, len, "chart", sizeof("chart") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2598
                    off4 = 78;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.chart";
                        return Match;
                    }
                    // line 2600
                    off4 = 78;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.chart-template";
                        return Match;
                    }
                }
                // line 2602
                off3 = 73;
                rslt = stringEqual(buf, len, "formula", sizeof("formula") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2603
                    off4 = 80;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.formula";
                        return Match;
                    }
                    // line 2605
                    off4 = 80;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.formula-template";
                        return Match;
                    }
                }
                // line 2607
                off3 = 73;
                rslt = stringEqual(buf, len, "database", sizeof("database") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/vnd.oasis.opendocument.database";
                    return Match;
                }
                // line 2609
                off3 = 73;
                rslt = stringEqual(buf, len, "image", sizeof("image") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2610
                    off4 = 78;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.image";
                        return Match;
                    }
                    // line 2612
                    off4 = 78;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.image-template";
                        return Match;
                    }
                }
            }
            // line 2618
            off2 = 50;
            rslt = stringEqual(buf, len, "epub+zip", sizeof("epub+zip") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/epub+zip";
                return Match;
            }
            // line 2627
            off2 = 50;
            rslt = !stringEqual(buf, len, "epub+zip", sizeof("epub+zip") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2628
                off3 = 50;
                rslt = !stringEqual(buf, len, "vnd.oasis.opendocument.", sizeof("vnd.oasis.opendocument.") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2629
                    off4 = 50;
                    rslt = !stringEqual(buf, len, "vnd.sun.xml.", sizeof("vnd.sun.xml.") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 2630
                        off5 = 50;
                        rslt = !stringEqual(buf, len, "vnd.kde.", sizeof("vnd.kde.") - 1, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 2631
                            off6 = 38;
                            rslt = regexMatch(buf, len, "[!-OQ-~]+", &off6, 0, 0);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                *mime = "application/zip";
                                return Match;
                            }
                        }
                    }
                }
            }
        }
        // line 2634
        off1 = 26;
        rslt = stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetype", sizeof("\b" "\x00" "\x00" "\x00" "mimetype") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2635
            off2 = 38;
            rslt = !stringEqual(buf, len, "application/", sizeof("application/") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2636
                off3 = 38;
                rslt = regexMatch(buf, len, "[!-OQ-~]+", &off3, 0, 0);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/zip";
                    return Match;
                }
            }
        }
    }

    // line 2692
    off0 = 10;
    rslt = stringEqual(buf, len, "# This is a shell archive", sizeof("# This is a shell archive") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 3025
    off0 = 0;
    rslt = stringEqual(buf, len, ".snd", sizeof(".snd") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 3026
        off1 = 12;
        rslt = beLongMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3028
        off1 = 12;
        rslt = beLongMatch(buf, len, 2, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3030
        off1 = 12;
        rslt = beLongMatch(buf, len, 3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3032
        off1 = 12;
        rslt = beLongMatch(buf, len, 4, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3034
        off1 = 12;
        rslt = beLongMatch(buf, len, 5, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3036
        off1 = 12;
        rslt = beLongMatch(buf, len, 6, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3038
        off1 = 12;
        rslt = beLongMatch(buf, len, 7, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 3050
        off1 = 12;
        rslt = beLongMatch(buf, len, 23, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-adpcm";
            return Match;
        }
    }

    // line 3742
    off0 = 0;
    rslt = stringEqual(buf, len, "AUDIMG", sizeof("AUDIMG") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 3744
        off1 = 13;
        rslt = byteMatch(buf, len, 13, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-vpm-wav-garmin";
            return Match;
        }
    }

    // line 5025
    off0 = 0;
    rslt = stringEqual(buf, len, "<?php", sizeof("<?php") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 5026
        off1 = 5;
        rslt = regexMatch(buf, len, "[ \n]", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 5027
            off2 = 6;
            rslt = stringEqual(buf, len, "/* Smarty version", sizeof("/* Smarty version") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 5028
                off3 = 24;
                rslt = regexMatch(buf, len, "[0-9.]+", &off3, 0, 0);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "text/x-php";
                    return Match;
                }
            }
        }
    }

    // line 5251
    off0 = 0;
    rslt = stringEqual(buf, len, "7z" "\xbc" "\xaf" "'" "\x1c", sizeof("7z" "\xbc" "\xaf" "'" "\x1c") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-7z-compressed";
        return Match;
    }

    // line 5273
    off0 = 0;
    rslt = stringEqual(buf, len, "LRZI", sizeof("LRZI") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lrzip";
        return Match;
    }

    // line 6482
    off0 = 0;
    rslt = stringEqual(buf, len, "RaS", sizeof("RaS") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6485
        off1 = 3;
        rslt = stringEqual(buf, len, "3", sizeof("3") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.cups-raster";
            return Match;
        }
    }

    // line 6491
    off0 = 1;
    rslt = stringEqual(buf, len, "SaR", sizeof("SaR") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6494
        rslt = stringEqualMap(buf, len, stringMap3, stringMap3Count, mime);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            return Match;
        }
    }

    // line 6965
    off0 = 4;
    rslt = stringEqual(buf, len, "Standard Jet DB", sizeof("Standard Jet DB") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-msaccess";
        return Match;
    }

    // line 6967
    off0 = 4;
    rslt = stringEqual(buf, len, "Standard ACE DB", sizeof("Standard ACE DB") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-msaccess";
        return Match;
    }

    // line 7007
    off0 = 8;
    rslt = stringEqual(buf, len, "sdbf", sizeof("sdbf") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 7008
        off1 = 7;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 7010
            off2 = 12;
            rslt = leShortMatch(buf, len, 0x7802, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-ms-sdb";
                return Match;
            }
        }
    }

    // line 8133
    off0 = 0;
    rslt = stringEqual(buf, len, "FP2", sizeof("FP2") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8150
        off1 = 0;
        rslt = stringEqual(buf, len, "F", sizeof("F") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8151
            off2 = 8;
            rslt = byteMatch(buf, len, 0x08, CompareEq, 0xfd, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
            // line 8154
            off2 = 8;
            rslt = byteMatch(buf, len, 0x10, CompareEq, 0xfe, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
            // line 8157
            off2 = 8;
            rslt = byteMatch(buf, len, 0x18, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
            // line 8160
            off2 = 8;
            rslt = beShortMatch(buf, len, 0x2000, CompareEq, 0xff87, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
            // line 8163
            off2 = 8;
            rslt = beShortMatch(buf, len, 0x3000, CompareEq, 0xffe0, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
            // line 8166
            off2 = 8;
            rslt = byteMatch(buf, len, 0, CompareEq, 0x7, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 8167
                off3 = 8;
                rslt = byteMatch(buf, len, 0x2f, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 8168
                    off4 = 9;
                    rslt = byteMatch(buf, len, 0x20, CompareLt, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/x-shockwave-flash";
                        return Match;
                    }
                }
            }
        }
        // line 8172
        off1 = 0;
        rslt = stringEqual(buf, len, "C", sizeof("C") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8173
            off2 = 8;
            rslt = byteMatch(buf, len, 0x78, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
        }
        // line 8177
        off1 = 0;
        rslt = stringEqual(buf, len, "Z", sizeof("Z") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8178
            off2 = 8;
            rslt = byteMatch(buf, len, 0x5d, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-shockwave-flash";
                return Match;
            }
        }
    }

    // line 8320
    off0 = 0;
    rslt = stringEqual(buf, len, "FILE", sizeof("FILE") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8322
        off1 = 8;
        rslt = stringEqual(buf, len, "PFF2", sizeof("PFF2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8324
            off2 = 4;
            rslt = beLongMatch(buf, len, 4, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 8326
                off3 = 12;
                rslt = stringEqual(buf, len, "NAME", sizeof("NAME") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-font-pf2";
                    return Match;
                }
            }
        }
    }

    // line 8386
    off0 = 0;
    rslt = stringEqual(buf, len, "\x00" "\x01" "\x00" "\x00", sizeof("\x00" "\x01" "\x00" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8396
        off1 = 4;
        rslt = beShortMatch(buf, len, 47, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8400
            off2 = 12;
            rslt = regexMatch(buf, len, "^[A-Za-z][A-Za-z][A-Za-z/][A-Za-z2 ]", &off2, 4 * 80, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 8402
                off3 = 0;
                rslt = beLongMatch(buf, len, 0x4f54544f, CompareEq|CompareNot, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/font-sfnt";
                    return Match;
                }
                // line 8410
                off3 = 0;
                rslt = beLongMatch(buf, len, 0x4f54544f, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/font-sfnt";
                    return Match;
                }
            }
        }
    }

    // line 8496
    off0 = 0;
    rslt = stringEqual(buf, len, "ttcf", sizeof("ttcf") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8498
        off1 = 4;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/font-sfnt";
            return Match;
        }
    }

    // line 8536
    off0 = 0x40;
    rslt = stringEqual(buf, len, "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00", sizeof("\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8537
        off1 = 0x22;
        rslt = stringEqual(buf, len, "LP", sizeof("LP") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8540
            off2 = 0x52;
            rslt = leShortMatch(buf, len, 0, CompareEq|CompareNot, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.ms-fontobject";
                return Match;
            }
        }
    }

    // line 10639
    off0 = 0;
    rslt = stringEqual(buf, len, "FORM", sizeof("FORM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10698
        off1 = 8;
        rslt = stringEqual(buf, len, "IFZS", sizeof("IFZS") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-blorb";
            return Match;
        }
    }

    // line 10898
    off0 = 0;
    rslt = stringEqual(buf, len, "P4", sizeof("P4") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10899
        off1 = 0;
        rslt = regexMatch(buf, len, "P4[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-bitmap";
            return Match;
        }
    }

    // line 10905
    off0 = 0;
    rslt = stringEqual(buf, len, "P5", sizeof("P5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10906
        off1 = 0;
        rslt = regexMatch(buf, len, "P5[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-greymap";
            return Match;
        }
    }

    // line 10912
    off0 = 0;
    rslt = stringEqual(buf, len, "P6", sizeof("P6") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10913
        off1 = 0;
        rslt = regexMatch(buf, len, "P6[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-pixmap";
            return Match;
        }
    }

    // line 11145
    off0 = 0;
    rslt = stringEqual(buf, len, "\x89" "PNG\r\n" "\x1a" "\n" "\x00" "\x00" "\x00" "\x04" "CgBI", sizeof("\x89" "PNG\r\n" "\x1a" "\n" "\x00" "\x00" "\x00" "\x04" "CgBI") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11146
        off1 = 24;
        rslt = stringEqual(buf, len, "\x00" "\x00" "\x00" "\rIHDR", sizeof("\x00" "\x00" "\x00" "\rIHDR") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/png";
            return Match;
        }
    }

    // line 11249
    off0 = 0;
    rslt = stringEqual(buf, len, "AWBM", sizeof("AWBM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11250
        off1 = 4;
        rslt = leShortMatch(buf, len, 1981, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-award-bmp";
            return Match;
        }
    }

    // line 11269
    off0 = 0;
    rslt = stringEqual(buf, len, "BM", sizeof("BM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11270
        off1 = 14;
        rslt = leShortMatch(buf, len, 12, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 11274
        off1 = 14;
        rslt = leShortMatch(buf, len, 64, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 11278
        off1 = 14;
        rslt = leShortMatch(buf, len, 40, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 11283
        off1 = 14;
        rslt = leShortMatch(buf, len, 124, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 11288
        off1 = 14;
        rslt = leShortMatch(buf, len, 108, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 11293
        off1 = 14;
        rslt = leShortMatch(buf, len, 128, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
    }

    // line 11404
    off0 = 128;
    rslt = stringEqual(buf, len, "DICM", sizeof("DICM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/dicom";
        return Match;
    }

    // line 11571
    off0 = 16;
    rslt = stringEqual(buf, len, "TIMG" "\x00", sizeof("TIMG" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11576
        off1 = 0;
        rslt = beShortMatch(buf, len, 0x0003, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-gem";
            return Match;
        }
    }

    // line 11742
    off0 = 0;
    rslt = stringEqual(buf, len, "AT&TFORM", sizeof("AT&TFORM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11743
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVM", sizeof("DJVM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 11745
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVU", sizeof("DJVU") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 11747
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVI", sizeof("DJVI") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 11749
        off1 = 12;
        rslt = stringEqual(buf, len, "THUM", sizeof("THUM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
    }

    // line 11819
    off0 = 512;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 11821
    off0 = 1024;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 11823
    off0 = 2048;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 11825
    off0 = 4096;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 12498
    off0 = 0;
    rslt = stringEqual(buf, len, "\x00" "\x00" "\x00" "\fjP  \r\n" "\x87" "\n", sizeof("\x00" "\x00" "\x00" "\fjP  \r\n" "\x87" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12504
        off1 = 20;
        rslt = stringEqual(buf, len, "jp2 ", sizeof("jp2 ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jp2";
            return Match;
        }
        // line 12506
        off1 = 20;
        rslt = stringEqual(buf, len, "jpx ", sizeof("jpx ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpx";
            return Match;
        }
        // line 12508
        off1 = 20;
        rslt = stringEqual(buf, len, "jpm ", sizeof("jpm ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpm";
            return Match;
        }
        // line 12510
        off1 = 20;
        rslt = stringEqual(buf, len, "mjp2", sizeof("mjp2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mj2";
            return Match;
        }
    }

    // line 12519
    off0 = 0;
    rslt = stringEqual(buf, len, "II" "\xbc", sizeof("II" "\xbc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12520
        off1 = 3;
        rslt = byteMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
        }
    }

    // line 12885
    off0 = 0;
    rslt = stringEqual(buf, len, "LPKSHHRH", sizeof("LPKSHHRH") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12887
        off1 = 16;
        rslt = byteMatch(buf, len, 0, CompareEq, 252, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 12889
            off2 = 24;
            rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 12890
                off3 = 32;
                rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 12891
                    off4 = 40;
                    rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 12892
                        off5 = 48;
                        rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 12893
                            off6 = 56;
                            rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off6);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                // line 12894
                                off7 = 64;
                                rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off7);
                                if (rslt < 0) haveError = True;
                                if (rslt > 0)
                                {
                                    *mime = "application/octet-stream";
                                    return Match;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // line 14819
    off0 = 19;
    rslt = stringEqual(buf, len, "\xa0" "\x05" "\xf9" "\x05" "\x00" "\t" "\x00" "\x02" "\x00", sizeof("\xa0" "\x05" "\xf9" "\x05" "\x00" "\t" "\x00" "\x02" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 14945
        off1 = 38913;
        rslt = stringEqual(buf, len, "NSR0", sizeof("NSR0") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-iso9660-image";
            return Match;
        }
        // line 14943
        off1 = 38913;
        rslt = !stringEqual(buf, len, "NSR0", sizeof("NSR0") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-iso9660-image";
            return Match;
        }
    }

    // line 14957
    off0 = 37633;
    rslt = stringEqual(buf, len, "CD001", sizeof("CD001") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-iso9660-image";
        return Match;
    }

    // line 15507
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml", sizeof("<?xml") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15508
        off1 = 20;
        rslt = stringSearch(buf, len, " xmlns=", sizeof(" xmlns=") - 1, &off1, 400, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 15509
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "['\"]http://earth.google.com/kml", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.google-earth.kml+xml";
                return Match;
            }
            // line 15521
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "['\"]http://www.opengis.net/kml", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.google-earth.kml+xml";
                return Match;
            }
        }
    }

    // line 15529
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15530
        off1 = 4;
        rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 15531
            off2 = 30;
            rslt = stringEqual(buf, len, "doc.kml", sizeof("doc.kml") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.google-earth.kmz";
                return Match;
            }
        }
    }

    // line 16364
    off0 = 60;
    rslt = stringEqual(buf, len, "RINEX", sizeof("RINEX") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16365
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXB", sizeof("XXRINEXB") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/broadcast";
            return Match;
        }
        // line 16369
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXD", sizeof("XXRINEXD") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/observation";
            return Match;
        }
        // line 16373
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXC", sizeof("XXRINEXC") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/clock";
            return Match;
        }
        // line 16377
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXH", sizeof("XXRINEXH") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 16381
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXG", sizeof("XXRINEXG") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 16385
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXL", sizeof("XXRINEXL") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 16389
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXM", sizeof("XXRINEXM") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/meteorological";
            return Match;
        }
        // line 16393
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXN", sizeof("XXRINEXN") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 16397
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXO", sizeof("XXRINEXO") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/observation";
            return Match;
        }
    }

    // line 16416
    off0 = 0;
    rslt = stringEqual(buf, len, "0" "\x00" "\x00" "|", sizeof("0" "\x00" "\x00" "|") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16417
        off1 = 36;
        rslt = stringEqual(buf, len, "\x00" ">", sizeof("\x00" ">") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/octet-stream";
            return Match;
        }
    }

    // line 16420
    off0 = 0;
    rslt = stringEqual(buf, len, "0~" "\x00" "\x00", sizeof("0~" "\x00" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16421
        off1 = 36;
        rslt = stringEqual(buf, len, "\x00" ">", sizeof("\x00" ">") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/octet-stream";
            return Match;
        }
    }

    // line 16424
    off0 = 39;
    rslt = stringEqual(buf, len, "\x02", sizeof("\x02") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16425
        off1 = 136;
        rslt = stringEqual(buf, len, "\x02" "\x02" "\x04" "\x04", sizeof("\x02" "\x02" "\x04" "\x04") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/octet-stream";
            return Match;
        }
    }

    // line 16743
    off0 = 0;
    rslt = stringEqual(buf, len, "@", sizeof("@") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16744
        off1 = 1;
        rslt = stringMatch(buf, len, " echo off", sizeof(" echo off") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 16746
        off1 = 1;
        rslt = stringMatch(buf, len, "echo off", sizeof("echo off") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 16748
        off1 = 1;
        rslt = stringMatch(buf, len, "rem", sizeof("rem") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 16750
        off1 = 1;
        rslt = stringMatch(buf, len, "set ", sizeof("set ") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
    }

    // line 16782
    off0 = 0;
    rslt = stringEqual(buf, len, "MZ", sizeof("MZ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16991
        off1 = 0x1e;
        rslt = stringEqual(buf, len, "Copyright 1989-1990 PKWARE Inc.", sizeof("Copyright 1989-1990 PKWARE Inc.") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/zip";
            return Match;
        }
        // line 16994
        off1 = 0x1e;
        rslt = stringEqual(buf, len, "PKLITE Copr.", sizeof("PKLITE Copr.") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/zip";
            return Match;
        }
    }

    // line 17303
    off0 = 2080;
    rslt = stringEqual(buf, len, "Microsoft Word 6.0 Document", sizeof("Microsoft Word 6.0 Document") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17305
    off0 = 2080;
    rslt = stringEqual(buf, len, "Documento Microsoft Word 6", sizeof("Documento Microsoft Word 6") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17308
    off0 = 2112;
    rslt = stringEqual(buf, len, "MSWordDoc", sizeof("MSWordDoc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17334
    off0 = 512;
    rslt = stringEqual(buf, len, "\xec" "\xa5" "\xc1", sizeof("\xec" "\xa5" "\xc1") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17341
    off0 = 2080;
    rslt = stringEqual(buf, len, "Microsoft Excel 5.0 Worksheet", sizeof("Microsoft Excel 5.0 Worksheet") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 17347
    off0 = 2080;
    rslt = stringEqual(buf, len, "Foglio di lavoro Microsoft Exce", sizeof("Foglio di lavoro Microsoft Exce") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 17351
    off0 = 2114;
    rslt = stringEqual(buf, len, "Biff5", sizeof("Biff5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 17354
    off0 = 2121;
    rslt = stringEqual(buf, len, "Biff5", sizeof("Biff5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 17808
    off0 = 0;
    rslt = stringEqual(buf, len, "\xd0" "\xcf" "\x11" "\xe0" "\xa1" "\xb1" "\x1a" "\xe1", sizeof("\xd0" "\xcf" "\x11" "\xe0" "\xa1" "\xb1" "\x1a" "\xe1") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17811
        off1 = 546;
        rslt = stringEqual(buf, len, "bjbj", sizeof("bjbj") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
        // line 17813
        off1 = 546;
        rslt = stringEqual(buf, len, "jbjb", sizeof("jbjb") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
    }

    // line 17819
    off0 = 512;
    rslt = stringEqual(buf, len, "R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00" " " "\x00" "E" "\x00" "n" "\x00" "t" "\x00" "r" "\x00" "y", sizeof("R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00" " " "\x00" "E" "\x00" "n" "\x00" "t" "\x00" "r" "\x00" "y") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 17849
    off0 = 0;
    rslt = stringEqual(buf, len, "ITOLITLS", sizeof("ITOLITLS") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-ms-reader";
        return Match;
    }

    // line 18036
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 18039
        off1 = 0x1E;
        rslt = regexMatch(buf, len, "\\[Content_Types\\]\\.xml|_rels/\\.rels", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 18043
            off2 = 18;
            rslt = getOffset(buf, len, off2, 'l', &off2);
            off2 += 49;
            if (rslt < 0) haveError = True;
            else
            {
                rslt = stringSearch(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off2, 2000, 0);
                if (rslt < 0) haveError = True;
            }
            if (rslt > 0)
            {
                // line 18046
                off3 = 26;
                off3 += off2;
                rslt = stringSearch(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off3, 1000, 0);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 18050
                    off4 = 26;
                    off4 += off3;
                    rslt = stringMatch(buf, len, "word/", sizeof("word/") - 1, &off4, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
                        return Match;
                    }
                    // line 18052
                    off4 = 26;
                    off4 += off3;
                    rslt = stringMatch(buf, len, "ppt/", sizeof("ppt/") - 1, &off4, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
                        return Match;
                    }
                    // line 18054
                    off4 = 26;
                    off4 += off3;
                    rslt = stringMatch(buf, len, "xl/", sizeof("xl/") - 1, &off4, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
                        return Match;
                    }
                }
            }
        }
    }

    // line 18401
    off0 = 0;
    rslt = stringEqual(buf, len, "Microsoft C/C++ ", sizeof("Microsoft C/C++ ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 18403
        off1 = 24;
        rslt = stringSearch(buf, len, "\r\n" "\x1a", sizeof("\r\n" "\x1a") - 1, &off1, 14, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ms-pdb";
            return Match;
        }
    }

    // line 19673
    off0 = 0;
    rslt = stringEqual(buf, len, "#!", sizeof("#!") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 19674
        off1 = 0;
        rslt = regexMatch(buf, len, "^#!.*/bin/perl([[:space:]].*)*$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-perl";
            return Match;
        }
    }

    // line 19817
    off0 = 2;
    rslt = stringEqual(buf, len, "---BEGIN PGP PUBLIC KEY BLOCK-", sizeof("---BEGIN PGP PUBLIC KEY BLOCK-") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/pgp-keys";
        return Match;
    }

    // line 20849
    off0 = 0;
    rslt = stringEqual(buf, len, "RIFF", sizeof("RIFF") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20874
        off1 = 8;
        rslt = stringEqual(buf, len, "WAVE", sizeof("WAVE") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-wav";
            return Match;
        }
        // line 20879
        off1 = 8;
        rslt = stringEqual(buf, len, "CDRA", sizeof("CDRA") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-coreldraw";
            return Match;
        }
        // line 20881
        off1 = 8;
        rslt = stringEqual(buf, len, "CDR6", sizeof("CDR6") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-coreldraw";
            return Match;
        }
        // line 20885
        off1 = 8;
        rslt = stringEqual(buf, len, "AVI ", sizeof("AVI ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-msvideo";
            return Match;
        }
        // line 21010
        off1 = 8;
        rslt = stringEqual(buf, len, "WEBP", sizeof("WEBP") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/webp";
            return Match;
        }
    }

    // line 21163
    off0 = 4;
    rslt = stringEqual(buf, len, "O", sizeof("O") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21165
        off1 = 84;
        rslt = beQuadMatch(buf, len, 0x00Fe00000000Fe00, CompareClr, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 21170
            off2 = 5;
            rslt = regexMatch(buf, len, "[=.<>|!^" "\x8a" "]{79}", &off2, 0, 0|RegexBegin);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-appleworks3";
                return Match;
            }
        }
    }

    // line 22134
    off0 = 0;
    rslt = stringEqual(buf, len, "HEADER    ", sizeof("HEADER    ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22135
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^.{40}", &off1, 1 * 80, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22136
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "[0-9]{2}-[A-Z]{3}-[0-9]{2} {3}", &off2, 1 * 80, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 22137
                off3 = 0;
                off3 += off2;
                rslt = regexMatch(buf, len, "[A-Z0-9]{4}.{14}$", &off3, 1 * 80, 0|RegexBegin);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 22138
                    off4 = 0;
                    off4 += off3;
                    rslt = regexMatch(buf, len, "[A-Z0-9]{4}", &off4, 1 * 80, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "chemical/x-pdb";
                        return Match;
                    }
                }
            }
        }
    }

    // line 22440
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=", sizeof("<?xml version=") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22441
        off1 = 14;
        rslt = regexMatch(buf, len, "['\" \t]*[0-9.]+['\" \t]*", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22442
            off2 = 19;
            rslt = stringSearch(buf, len, "<svg", sizeof("<svg") - 1, &off2, 4096, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/svg+xml";
                return Match;
            }
            // line 22444
            off2 = 19;
            rslt = stringSearch(buf, len, "<gnc-v2", sizeof("<gnc-v2") - 1, &off2, 4096, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-gnucash";
                return Match;
            }
        }
    }

    // line 22450
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=", sizeof("<?xml version=") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22451
        off1 = 14;
        rslt = regexMatch(buf, len, "['\" \t]*[0-9.]+['\" \t]*", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22452
            off2 = 19;
            rslt = stringSearch(buf, len, "<urlset", sizeof("<urlset") - 1, &off2, 4096, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/xml-sitemap";
                return Match;
            }
        }
    }

    // line 22463
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22464
        off1 = 19;
        rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off1, 4096, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22465
            off2 = 15;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 22467
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version='", sizeof("<?xml version='") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22468
        off1 = 19;
        rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off1, 4096, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22469
            off2 = 15;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 22471
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22472
        off1 = 19;
        rslt = stringSearch(buf, len, "<html", sizeof("<html") - 1, &off1, 4096, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 22473
            off2 = 15;
            rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 23820
    off0 = 0;
    rslt = stringEqual(buf, len, "\x1a" "\x01", sizeof("\x1a" "\x01") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 23822
        off1 = 16;
        rslt = byteMatch(buf, len, 32, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 23824
            off2 = 12;
            rslt = regexMatch(buf, len, "^[a-zA-Z0-9][a-zA-Z0-9.][^|]*", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-terminfo";
                return Match;
            }
        }
    }

    // line 23833
    off0 = 0;
    rslt = stringEqual(buf, len, "\x1e" "\x02", sizeof("\x1e" "\x02") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 23835
        off1 = 16;
        rslt = byteMatch(buf, len, 32, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 23837
            off2 = 12;
            rslt = regexMatch(buf, len, "^[a-zA-Z0-9][a-zA-Z0-9.][^|]*", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-terminfo2";
                return Match;
            }
        }
    }

    // line 23895
    off0 = 2;
    rslt = stringEqual(buf, len, "\x00" "\x11", sizeof("\x00" "\x11") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tex-tfm";
        return Match;
    }

    // line 23898
    off0 = 2;
    rslt = stringEqual(buf, len, "\x00" "\x12", sizeof("\x00" "\x12") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tex-tfm";
        return Match;
    }

    // line 24860
    off0 = 0;
    rslt = stringEqual(buf, len, "OggS", sizeof("OggS") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 24863
        off1 = 4;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 24866
            off2 = 28;
            rslt = stringEqual(buf, len, "FLAC", sizeof("FLAC") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "audio/ogg";
                return Match;
            }
            // line 24869
            off2 = 28;
            rslt = stringEqual(buf, len, "\x80" "theora", sizeof("\x80" "theora") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/ogg";
                return Match;
            }
            // line 24872
            off2 = 28;
            rslt = stringEqual(buf, len, "\x80" "kate" "\x00" "\x00" "\x00" "\x00", sizeof("\x80" "kate" "\x00" "\x00" "\x00" "\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/ogg";
                return Match;
            }
            // line 24883
            off2 = 28;
            rslt = stringEqual(buf, len, "fishead" "\x00", sizeof("fishead" "\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/ogg";
                return Match;
            }
            // line 24888
            off2 = 28;
            rslt = stringEqual(buf, len, "Speex   ", sizeof("Speex   ") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "audio/ogg";
                return Match;
            }
            // line 24891
            off2 = 28;
            rslt = stringEqual(buf, len, "\x01" "video" "\x00" "\x00" "\x00", sizeof("\x01" "video" "\x00" "\x00" "\x00") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/ogg";
                return Match;
            }
            // line 24898
            off2 = 28;
            rslt = stringEqual(buf, len, "\x01" "vorbis", sizeof("\x01" "vorbis") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "audio/ogg";
                return Match;
            }
            // line 24962
            off2 = 28;
            rslt = stringEqual(buf, len, "OpusHead", sizeof("OpusHead") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "audio/ogg";
                return Match;
            }
        }
    }

    // line 25105
    off0 = 0;
    rslt = stringEqual(buf, len, "PMCC", sizeof("PMCC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25117
        off1 = 0;
        rslt = leShortMatch(buf, len, 0x036C, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 25119
            off2 = 4;
            rslt = leShortMatch(buf, len, 1, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/winhelp";
                return Match;
            }
        }
    }

    // line 25824
    off0 = 512;
    rslt = stringEqual(buf, len, "R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00", sizeof("R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hwp";
        return Match;
    }

    // line 25855
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25856
        off1 = 43;
        rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro4";
            return Match;
        }
    }

    // line 25860
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25861
        off1 = 43;
        rslt = byteMatch(buf, len, 0x15, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro5";
            return Match;
        }
    }

    // line 25864
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25865
        off1 = 43;
        rslt = byteMatch(buf, len, 0x16, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro6";
            return Match;
        }
    }

    // line 1819
    off0 = 0;
    rslt = !stringEqual(buf, len, "<arch>\ndebian", sizeof("<arch>\ndebian") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1820
        off1 = 8;
        rslt = stringEqual(buf, len, "debian-split", sizeof("debian-split") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.debian.binary-package";
            return Match;
        }
        // line 1822
        off1 = 8;
        rslt = stringEqual(buf, len, "debian-binary", sizeof("debian-binary") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.debian.binary-package";
            return Match;
        }
    }

    // line 576
    off0 = 4;
    rslt = stringMatch(buf, len, "jP", sizeof("jP") - 1, &off0, CompareEq, 0|CompactWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/jp2";
        return Match;
    }

    // line 1403
    off0 = 0;
    rslt = stringMatch(buf, len, "#VRML V1.0 ascii", sizeof("#VRML V1.0 ascii") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "model/vrml";
        return Match;
    }

    // line 1405
    off0 = 0;
    rslt = stringMatch(buf, len, "#VRML V2.0 utf8", sizeof("#VRML V2.0 utf8") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "model/vrml";
        return Match;
    }

    // line 1412
    off0 = 0;
    rslt = stringMatch(buf, len, "<?xml version=", sizeof("<?xml version=") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1414
        off1 = 20;
        rslt = stringSearch(buf, len, "<!DOCTYPE X3D", sizeof("<!DOCTYPE X3D") - 1, &off1, 1000, 0|IgnoreWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "model/x3d+xml";
            return Match;
        }
    }

    // line 4931
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/sh", sizeof("#! /bin/sh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4933
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/sh", sizeof("#! /bin/sh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4936
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/csh", sizeof("#! /bin/csh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4940
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/ksh", sizeof("#! /bin/ksh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4942
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/ksh", sizeof("#! /bin/ksh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4945
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/tcsh", sizeof("#! /bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4947
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/tcsh", sizeof("#! /usr/bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4949
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/tcsh", sizeof("#! /usr/local/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4951
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/tcsh", sizeof("#! /usr/local/bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4956
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/zsh", sizeof("#! /bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4958
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/zsh", sizeof("#! /usr/bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4960
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/zsh", sizeof("#! /usr/local/bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4962
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/ash", sizeof("#! /usr/local/bin/ash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4964
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/ae", sizeof("#! /usr/local/bin/ae") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4966
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/nawk", sizeof("#! /bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 4968
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/nawk", sizeof("#! /usr/bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 4970
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/nawk", sizeof("#! /usr/local/bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 4972
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/gawk", sizeof("#! /bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 4974
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/gawk", sizeof("#! /usr/bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 4976
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/gawk", sizeof("#! /usr/local/bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 4979
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/awk", sizeof("#! /bin/awk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-awk";
        return Match;
    }

    // line 4981
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/awk", sizeof("#! /usr/bin/awk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-awk";
        return Match;
    }

    // line 4989
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/bash", sizeof("#! /bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4991
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/bash", sizeof("#! /bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4993
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/bash", sizeof("#! /usr/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4995
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/bash", sizeof("#! /usr/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4997
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bash", sizeof("#! /usr/local/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 4999
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bash", sizeof("#! /usr/local/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 5001
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/bash", sizeof("#! /usr/local/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 5003
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/bash", sizeof("#! /usr/local/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 5005
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/env bash", sizeof("#! /usr/bin/env bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 16570
    off0 = 0;
    rslt = stringMatch(buf, len, "BEGIN:VCALENDAR", sizeof("BEGIN:VCALENDAR") - 1, &off0, CompareEq, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/calendar";
        return Match;
    }

    // line 16575
    off0 = 0;
    rslt = stringMatch(buf, len, "BEGIN:VCARD", sizeof("BEGIN:VCARD") - 1, &off0, CompareEq, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/vcard";
        return Match;
    }

    // line 25870
    off0 = 0;
    rslt = stringMatch(buf, len, "<map version", sizeof("<map version") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-freemind";
        return Match;
    }

    // line 25875
    off0 = 0;
    rslt = stringMatch(buf, len, "<map version=\"freeplane", sizeof("<map version=\"freeplane") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-freeplane";
        return Match;
    }

    // line 5010
    off0 = 0;
    rslt = stringSearch(buf, len, "<?php", sizeof("<?php") - 1, &off0, 1, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 5013
    off0 = 0;
    rslt = stringSearch(buf, len, "<?\n", sizeof("<?\n") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 5015
    off0 = 0;
    rslt = stringSearch(buf, len, "<?\r", sizeof("<?\r") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 5017
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/php", sizeof("#! /usr/local/bin/php") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 5020
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/php", sizeof("#! /usr/bin/php") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 8602
    off0 = 0;
    rslt = stringSearch(buf, len, "<MakerDictionary", sizeof("<MakerDictionary") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-mif";
        return Match;
    }

    // line 10877
    off0 = 0;
    rslt = stringSearch(buf, len, "P1", sizeof("P1") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10878
        off1 = 0;
        rslt = regexMatch(buf, len, "P1[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-bitmap";
            return Match;
        }
    }

    // line 10884
    off0 = 0;
    rslt = stringSearch(buf, len, "P2", sizeof("P2") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10885
        off1 = 0;
        rslt = regexMatch(buf, len, "P2[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-greymap";
            return Match;
        }
    }

    // line 10891
    off0 = 0;
    rslt = stringSearch(buf, len, "P3", sizeof("P3") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 10892
        off1 = 0;
        rslt = regexMatch(buf, len, "P3[ \t\f\r\n]", &off1, 4, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-portable-pixmap";
            return Match;
        }
    }

    // line 11307
    off0 = 0;
    rslt = stringSearch(buf, len, "/* XPM */", sizeof("/* XPM */") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-xpmi";
        return Match;
    }

    // line 12386
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/bin/node", sizeof("#!/bin/node") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12388
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/node", sizeof("#!/usr/bin/node") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12390
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/bin/nodejs", sizeof("#!/bin/nodejs") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12392
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/nodejs", sizeof("#!/usr/bin/nodejs") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12394
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env node", sizeof("#!/usr/bin/env node") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12396
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env nodejs", sizeof("#!/usr/bin/env nodejs") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 15632
    off0 = 0;
    rslt = stringSearch(buf, len, "<TeXmacs|", sizeof("<TeXmacs|") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/texmacs";
        return Match;
    }

    // line 15663
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/lua", sizeof("#! /usr/bin/lua") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 15665
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/lua", sizeof("#! /usr/local/bin/lua") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 15667
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env lua", sizeof("#!/usr/bin/env lua") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 15669
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env lua", sizeof("#! /usr/bin/env lua") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 20643
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/python", sizeof("#! /usr/bin/python") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20646
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/python", sizeof("#! /usr/local/bin/python") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20649
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env python", sizeof("#!/usr/bin/env python") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 21970
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/ruby", sizeof("#! /usr/bin/ruby") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 21973
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/ruby", sizeof("#! /usr/local/bin/ruby") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 21976
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env ruby", sizeof("#!/usr/bin/env ruby") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 21979
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env ruby", sizeof("#! /usr/bin/env ruby") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 22534
    off0 = 0;
    rslt = stringSearch(buf, len, "<?xml", sizeof("<?xml") - 1, &off0, 1, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/xml";
        return Match;
    }

    // line 22552
    off0 = 0;
    rslt = stringSearch(buf, len, "<?XML", sizeof("<?XML") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/xml";
        return Match;
    }

    // line 23779
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/tcl", sizeof("#! /usr/bin/tcl") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23781
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/tcl", sizeof("#! /usr/local/bin/tcl") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23783
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env tcl", sizeof("#!/usr/bin/env tcl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23785
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env tcl", sizeof("#! /usr/bin/env tcl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23787
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/wish", sizeof("#! /usr/bin/wish") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23789
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/wish", sizeof("#! /usr/local/bin/wish") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23791
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env wish", sizeof("#!/usr/bin/env wish") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23793
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env wish", sizeof("#! /usr/bin/env wish") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 23903
    off0 = 0;
    rslt = stringSearch(buf, len, "\\input texinfo", sizeof("\\input texinfo") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-texinfo";
        return Match;
    }

    // line 23905
    off0 = 0;
    rslt = stringSearch(buf, len, "This is Info file", sizeof("This is Info file") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-info";
        return Match;
    }

    // line 280
    off0 = 0;
    rslt = regexMatch(buf, len, "^PROC", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-Algol68";
        return Match;
    }

    // line 282
    off0 = 0;
    rslt = regexMatch(buf, len, "MODE[\t ]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-Algol68";
        return Match;
    }

    // line 284
    off0 = 0;
    rslt = regexMatch(buf, len, "REF[\t ]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-Algol68";
        return Match;
    }

    // line 286
    off0 = 0;
    rslt = regexMatch(buf, len, "FLEX[\t ]*\\[", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-Algol68";
        return Match;
    }

    // line 4344
    off0 = 0;
    rslt = regexMatch(buf, len, "^#import", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-objective-c";
        return Match;
    }

    // line 20658
    off0 = 0;
    rslt = regexMatch(buf, len, "^from[ \t\f\r\n]+([A-Za-z0-9_]|\\.)+[ \t\f\r\n]+import.*$", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20676
    off0 = 0;
    rslt = regexMatch(buf, len, "^import [_[:alpha:]]+ as [[:alpha:]][[:space:]]*$", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20699
    off0 = 0;
    rslt = regexMatch(buf, len, "^class [_[:alpha:]]+(\\(.*\\))?( )*:([ \t]+pass)?$", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20704
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]{0,50}def {1,50}[_a-zA-Z]{1,100}", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20705
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "\\(([[:alpha:]*_, ]){0,255}\\):$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
    }

    // line 21986
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*require[[:space:]]'[A-Za-z_/]+'", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21987
        off1 = 0;
        rslt = regexMatch(buf, len, "def [a-z]| do$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 21988
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^[[:space:]]*end([[:space:]]+[;#].*)?$", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/x-ruby";
                return Match;
            }
        }
    }

    // line 21991
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*(class|module)[[:space:]][A-Z]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21992
        off1 = 0;
        rslt = regexMatch(buf, len, "(modul|includ)e [A-Z]|def [a-z]", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 21993
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^[[:space:]]*end([[:space:]]+[;#].*)?$", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/x-ruby";
                return Match;
            }
        }
    }

    // line 21997
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*(class|module)[[:space:]][A-Z]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 21998
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^[[:space:]]*end([[:space:]]+[;#if].*)?$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-ruby";
            return Match;
        }
    }

    // line 22004
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*def [a-z]|def [[:alpha:]]+::[a-z]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 22005
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^[[:space:]]*end([[:space:]]+[;#].*)?$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-ruby";
            return Match;
        }
    }

    // line 22009
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*require[[:space:]]'[A-Za-z_/]+'", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 22011
    off0 = 0;
    rslt = regexMatch(buf, len, "^[[:space:]]*include ([A-Z]+[a-z]*(::))+", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 25225
    off0 = 0;
    rslt = regexMatch(buf, len, "^(:|;)", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25239
        off1 = 0;
        rslt = stringEqual(buf, len, " ", sizeof(" ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 25241
            off2 = 1;
            rslt = regexMatch(buf, len, "^([^\r>]*|.*.hlp)", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/plain";
                return Match;
            }
        }
    }

    // line 25344
    off0 = 0;
    rslt = regexMatch(buf, len, "\\`(\\r\\n|;|[[])", &off0, 0, 0|RegexBegin);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 25346
        off1 = 0;
        off1 += off0;
        rslt = stringSearch(buf, len, "[", sizeof("[") - 1, &off1, 8192, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 25394
            off2 = 0;
            off2 += off1;
            rslt = beQuadMatch(buf, len, 0x0056004500520053, CompareEq, 0xFFdfFFdfFFdfFFdf, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25396
                off3 = 0;
                off3 += off2;
                rslt = beQuadMatch(buf, len, 0x0049004f004e005d, CompareEq, 0xFFdfFFdfFFdfFFff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-setupscript";
                    return Match;
                }
            }
            // line 25399
            off2 = 0;
            off2 += off1;
            rslt = beQuadMatch(buf, len, 0x0053005400520049, CompareEq, 0xFFdfFFdfFFdfFFdf, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25401
                off3 = 0;
                off3 += off2;
                rslt = beQuadMatch(buf, len, 0x004e00470053005D, CompareEq, 0xFFdfFFdfFFdfFFff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-setupscript";
                    return Match;
                }
            }
            // line 25405
            off3 = 0;
            off3 += off2;
            rslt = stringSearch(buf, len, "[", sizeof("[") - 1, &off3, 8192, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25410
                off4 = 0;
                off4 += off3;
                rslt = beQuadMatch(buf, len, 0x0056004500520053, CompareEq, 0xFFdfFFdfFFdfFFdf, &off4);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 25412
                    off5 = 0;
                    off5 += off4;
                    rslt = beQuadMatch(buf, len, 0x0049004f004e005d, CompareEq, 0xFFdfFFdfFFdfFFff, &off5);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/x-setupscript";
                        return Match;
                    }
                }
                // line 25407
                off4 = 0;
                off4 += off3;
                rslt = stringMatch(buf, len, "version", sizeof("version") - 1, &off4, CompareEq, 0|MatchLower);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-setupscript";
                    return Match;
                }
            }
            // line 25349
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(autorun)]\r\n", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 25350
                off3 = 0;
                off3 += off2;
                rslt = byteMatch(buf, len, 0x5b, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-wine-extension-ini";
                    return Match;
                }
                // line 25354
                off3 = 0;
                off3 += off2;
                rslt = byteMatch(buf, len, 0x5b, CompareEq|CompareNot, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-setupscript";
                    return Match;
                }
            }
            // line 25358
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(version|strings)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-setupscript";
                return Match;
            }
            // line 25362
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(WinsockCRCList|OEMCPL)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/inf";
                return Match;
            }
            // line 25367
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(.ShellClassInfo|DeleteOnCopy|LocalizedFileNames)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25371
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(don't load)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25373
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(ndishlp\\$|protman\\$|NETBEUI\\$)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25377
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(windows|Compatibility|embedding)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25380
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(boot|386enh|drivers)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25383
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(SafeList)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 25386
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(boot loader)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
        }
    }

    // line 278
    off0 = 0;
    rslt = stringSearch(buf, len, "(input,", sizeof("(input,") - 1, &off0, 8192, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-Algol68";
        return Match;
    }

    // line 19598
    off0 = 0;
    rslt = stringSearch(buf, len, "%PDF-", sizeof("%PDF-") - 1, &off0, 256, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/pdf";
        return Match;
    }

    // line 19651
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec perl", sizeof("eval \"exec perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19653
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /bin/perl", sizeof("eval \"exec /bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19655
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /usr/bin/perl", sizeof("eval \"exec /usr/bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19657
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /usr/local/bin/perl", sizeof("eval \"exec /usr/local/bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19659
    off0 = 0;
    rslt = stringSearch(buf, len, "eval 'exec perl", sizeof("eval 'exec perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19661
    off0 = 0;
    rslt = stringSearch(buf, len, "eval 'exec /bin/perl", sizeof("eval 'exec /bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19663
    off0 = 0;
    rslt = stringSearch(buf, len, "eval 'exec /usr/bin/perl", sizeof("eval 'exec /usr/bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19665
    off0 = 0;
    rslt = stringSearch(buf, len, "eval 'exec /usr/local/bin/perl", sizeof("eval 'exec /usr/local/bin/perl") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 19667
    off0 = 0;
    rslt = stringSearch(buf, len, "eval '(exit $?0)' && eval 'exec", sizeof("eval '(exit $?0)' && eval 'exec") - 1, &off0, 1024, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 20652
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env python", sizeof("#! /usr/bin/env python") - 1, &off0, 10, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 20663
    off0 = 0;
    rslt = stringSearch(buf, len, "def __init__", sizeof("def __init__") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20664
        off1 = 0;
        off1 += off0;
        rslt = stringSearch(buf, len, "self", sizeof("self") - 1, &off1, 64, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
    }

    // line 20669
    off0 = 0;
    rslt = stringSearch(buf, len, "if __name__", sizeof("if __name__") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20671
        off1 = 0;
        off1 += off0;
        rslt = stringSearch(buf, len, "\"__main__\"", sizeof("\"__main__\"") - 1, &off1, 64, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
    }

    // line 20691
    off0 = 0;
    rslt = stringSearch(buf, len, "try:", sizeof("try:") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20692
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^[[:space:]]*except.*:$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
        // line 20695
        off1 = 0;
        off1 += off0;
        rslt = stringSearch(buf, len, "finally:", sizeof("finally:") - 1, &off1, 4096, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
    }

    // line 22481
    off0 = 0;
    rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22487
    off0 = 0;
    rslt = stringSearch(buf, len, "<!doctype svg", sizeof("<!doctype svg") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/svg+xml";
        return Match;
    }

    // line 22491
    off0 = 0;
    rslt = stringSearch(buf, len, "<head>", sizeof("<head>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22494
    off0 = 0;
    rslt = stringSearch(buf, len, "<head ", sizeof("<head ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22497
    off0 = 0;
    rslt = stringSearch(buf, len, "<title>", sizeof("<title>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22500
    off0 = 0;
    rslt = stringSearch(buf, len, "<title ", sizeof("<title ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22503
    off0 = 0;
    rslt = stringSearch(buf, len, "<html>", sizeof("<html>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22506
    off0 = 0;
    rslt = stringSearch(buf, len, "<html ", sizeof("<html ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22509
    off0 = 0;
    rslt = stringSearch(buf, len, "<script>", sizeof("<script>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22512
    off0 = 0;
    rslt = stringSearch(buf, len, "<script ", sizeof("<script ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22515
    off0 = 0;
    rslt = stringSearch(buf, len, "<style>", sizeof("<style>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22518
    off0 = 0;
    rslt = stringSearch(buf, len, "<style ", sizeof("<style ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22521
    off0 = 0;
    rslt = stringSearch(buf, len, "<table>", sizeof("<table>") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22524
    off0 = 0;
    rslt = stringSearch(buf, len, "<table ", sizeof("<table ") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 22528
    off0 = 0;
    rslt = stringSearch(buf, len, "<a href=", sizeof("<a href=") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 23909
    off0 = 0;
    rslt = stringSearch(buf, len, "\\input", sizeof("\\input") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23912
    off0 = 0;
    rslt = stringSearch(buf, len, "\\begin", sizeof("\\begin") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23915
    off0 = 0;
    rslt = stringSearch(buf, len, "\\section", sizeof("\\section") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23918
    off0 = 0;
    rslt = stringSearch(buf, len, "\\setlength", sizeof("\\setlength") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23921
    off0 = 0;
    rslt = stringSearch(buf, len, "\\documentstyle", sizeof("\\documentstyle") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23924
    off0 = 0;
    rslt = stringSearch(buf, len, "\\chapter", sizeof("\\chapter") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23927
    off0 = 0;
    rslt = stringSearch(buf, len, "\\documentclass", sizeof("\\documentclass") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23930
    off0 = 0;
    rslt = stringSearch(buf, len, "\\relax", sizeof("\\relax") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23933
    off0 = 0;
    rslt = stringSearch(buf, len, "\\contentsline", sizeof("\\contentsline") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 23936
    off0 = 0;
    rslt = stringSearch(buf, len, "% -*-latex-*-", sizeof("% -*-latex-*-") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }


    if (haveError)
    {
        // nothing matched, perhaps because of the error
        return Error;
    }
    return Fail;
}




static inline int
utf8Byte(const Byte** bufp)
{
    int value = -1;
    const Byte* bp = *bufp;

    if ((bp[0] & 0x80) == 0)
    {
        value = bp[0] & 0x7f;
        *bufp += 1;
    }

    if ((bp[0] & 0xe0) == 0xc0)
    {
        if ((bp[1] & 0xc0) == 0x80)
        {
            value = ((bp[0] & 0x1f) << 6) | (bp[1] & 0x3f);
            *bufp += 2;
        }
    }

    if ((bp[0] & 0xf0) == 0xe0)
    {
        if ((bp[1] & 0xc0) == 0x80 && (bp[2] & 0xc0) == 0x80)
        {
            value = ((bp[0] & 0x0f) << 12) | ((bp[1] & 0x3f) << 6) | (bp[2] & 0x3f);
            *bufp += 3;
        }
    }

    if ((bp[0] & 0xf8) == 0xf0)
    {
        if ((bp[1] & 0xc0) == 0x80 && (bp[2] & 0xc0) == 0x80 && (bp[3] & 0xc0) == 0x80)
        {
            value = ((bp[0] & 0x7) << 18) | ((bp[1] & 0x3f) << 12) | ((bp[2] & 0x3f) << 6) | (bp[3] & 0x3f);
            *bufp += 4;
        }
    }

    // UTF8 no longer does lengths 5 and 6
    return value;
}



Result
tryPlainText(const Byte* buf, size_t len, const char** mime, int flags)
{
    /*  Ensure that the mime string is a constant and doesn't
        need to be freed.
    */
    Bool        ascii = True;
    Bool        utf8  = True;
    const Byte* bp   = buf;
    const Byte* bend = buf + len;
    const Byte* uend;                   // for the utf8 search
    static const size_t Limit = 1024;   // Limit the search to 1024 bytes
    size_t      nuls    = 0;
    size_t      funnies = False;

    if (len > Limit)
    {
        bend = bp + Limit;
    }

    for (; bp < bend; ++bp)
    {
        Byte b = *bp;

        if (b == 0)
        {
            ++nuls;
        }
        else
        if (b < 32 && b != '\r' && b != '\n' && b != '\t')
        {
            ++funnies;
        }
        if (b >= 128)
        {
            ascii = False;
        }
    }

    // Note len / 100 could be zero
    ascii = ascii && nuls == 0 && funnies <= (len / 100);

    if (ascii)
    {
        *mime = "text/plain; charset=US-ASCII";
        return Match;
    }

    // UTF-8 has a BOM of EF BB BF
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
    {
        *mime = "text/plain; charset=UTF-8";
        return Match;
    }

    if (len >= 2)
    {
        if (buf[0] == 0xFE && buf[1] == 0xFF || buf[0] == 0xFF && buf[1] == 0xFE)
        {
            *mime = "text/plain; charset=UTF-16";
            return Match;
        }
    }

    /*  Try again to see if we can decode all bytes as UTF-8.
        We stop before the multi-byte char can go past the end
        of the buffer.
    */
    uend = bend - 4;
    utf8 = True;

    nuls    = 0;
    funnies = False;

    for (bp = buf; bp < uend; )
    {
        int b = utf8Byte(&bp);

        if (b < 0)
        {
            utf8 = False;
            break;
        }

        if (b == 0)
        {
            ++nuls;
        }
        else
        if (b < 32 && b != '\r' && b != '\n' && b != '\t')
        {
            ++funnies;
        }
    }

    utf8 = utf8 && nuls == 0 && funnies <= (len / 100);

    if (utf8)
    {
        *mime = "text/plain; charset=UTF-8";
        return Match;
    }

    return Error;
}



int
getMimeType(const Byte* buf, size_t len, const char** mime, int flags)
{
    *mime = NULL;

    if (len == 0)
    {
        return Error;
    }

    //testCount = 0;

    Result r = runTests(buf, len, mime);

    //printf ("test count %d\n", testCount);

    if (r < 0)
    {
        if (!(flags & MimeMagicNoTryText))
        {
            r = tryPlainText(buf, len, mime, flags);
        }
    }

    return r;
}
