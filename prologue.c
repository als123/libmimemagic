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


