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
    {0x9901,    0xffff,    "application/x-gnupg-keyring"},
    {0xffd8,    0xffff,    "image/jpeg"},
    {0x9900,    0xffff,    "application/x-pgp-keyring"},
    {0x9501,    0xffff,    "application/x-pgp-keyring"},
    {0x9500,    0xffff,    "application/x-pgp-keyring"},
    {0xa600,    0xffff,    "text/PGP"},
};
static const size_t beshortMap1Count = 15;

static StringMap stringMap2[] = {
    {"\x00" "\x01" "\x00" "\x00" "\x00",    sizeof("\x00" "\x01" "\x00" "\x00" "\x00") - 1,    "application/x-font-ttf"},
    {"\x04" "%!",    sizeof("\x04" "%!") - 1,    "application/postscript"},
    {"\t" "\x04" "\x06" "\x00" "\x00" "\x00" "\x10" "\x00",    sizeof("\t" "\x04" "\x06" "\x00" "\x00" "\x00" "\x10" "\x00") - 1,    "application/vnd.ms-excel"},
    {"\x1f" "\x1e",    sizeof("\x1f" "\x1e") - 1,    "application/octet-stream"},
    {"# KDE Config File",    sizeof("# KDE Config File") - 1,    "application/x-kdelnk"},
    {"# PaCkAgE DaTaStReAm",    sizeof("# PaCkAgE DaTaStReAm") - 1,    "application/x-svr4-package"},
    {"# abook addressbook file",    sizeof("# abook addressbook file") - 1,    "application/x-abook-addressbook"},
    {"# xmcd",    sizeof("# xmcd") - 1,    "text/x-xmcd"},
    {"%!",    sizeof("%!") - 1,    "application/postscript"},
    {"%FDF-",    sizeof("%FDF-") - 1,    "application/vnd.fdf"},
    {"%PDF-",    sizeof("%PDF-") - 1,    "application/pdf"},
    {"-----BEGIN PGP MESSAGE-",    sizeof("-----BEGIN PGP MESSAGE-") - 1,    "application/pgp"},
    {"-----BEGIN PGP SIGNATURE-",    sizeof("-----BEGIN PGP SIGNATURE-") - 1,    "application/pgp-signature"},
    {".RMF" "\x00" "\x00" "\x00",    sizeof(".RMF" "\x00" "\x00" "\x00") - 1,    "application/vnd.rn-realmedia"},
    {"8BPS",    sizeof("8BPS") - 1,    "image/vnd.adobe.photoshop"},
    {"<?xml version \"",    sizeof("<?xml version \"") - 1,    "application/xml"},
    {"<?xml version=\"",    sizeof("<?xml version=\"") - 1,    "application/xml"},
    {"<?xml version='",    sizeof("<?xml version='") - 1,    "application/xml"},
    {"<BookFile",    sizeof("<BookFile") - 1,    "application/x-mif"},
    {"<MIFFile",    sizeof("<MIFFile") - 1,    "application/x-mif"},
    {"<MML",    sizeof("<MML") - 1,    "application/x-mif"},
    {"<Maker",    sizeof("<Maker") - 1,    "application/x-mif"},
    {"<MakerFile",    sizeof("<MakerFile") - 1,    "application/x-mif"},
    {"<MakerScreenFont",    sizeof("<MakerScreenFont") - 1,    "application/x-mif"},
    {"<SCRIBUSUTF8NEW Version",    sizeof("<SCRIBUSUTF8NEW Version") - 1,    "application/x-scribus"},
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
    {"BZh",    sizeof("BZh") - 1,    "application/x-bzip2"},
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
    {"MM" "\x00" "*",    sizeof("MM" "\x00" "*") - 1,    "image/tiff"},
    {"MM" "\x00" "+",    sizeof("MM" "\x00" "+") - 1,    "image/tiff"},
    {"MMOR",    sizeof("MMOR") - 1,    "image/x-olympus-orf"},
    {"MP+",    sizeof("MP+") - 1,    "audio/x-musepack"},
    {"MSCF" "\x00" "\x00" "\x00" "\x00",    sizeof("MSCF" "\x00" "\x00" "\x00" "\x00") - 1,    "application/vnd.ms-cab-compressed"},
    {"MThd",    sizeof("MThd") - 1,    "audio/midi"},
    {"OTTO",    sizeof("OTTO") - 1,    "application/vnd.ms-opentype"},
    {"OggS",    sizeof("OggS") - 1,    "application/ogg"},
    {"P7",    sizeof("P7") - 1,    "image/x-portable-pixmap"},
    {"PDN3",    sizeof("PDN3") - 1,    "image/x-paintnet"},
    {"PK\a\bPK" "\x03" "\x04",    sizeof("PK\a\bPK" "\x03" "\x04") - 1,    "application/zip"},
    {"PO^Q`",    sizeof("PO^Q`") - 1,    "application/msword"},
    {"RF64" "\xff" "\xff" "\xff" "\xff" "WAVEds64",    sizeof("RF64" "\xff" "\xff" "\xff" "\xff" "WAVEds64") - 1,    "audio/x-wav"},
    {"Rar!",    sizeof("Rar!") - 1,    "application/x-rar"},
    {"Xcur",    sizeof("Xcur") - 1,    "image/x-xcursor"},
    {"[BitmapInfo2]",    sizeof("[BitmapInfo2]") - 1,    "image/x-polar-monitor-bitmap"},
    {"[KDE Desktop Entry]",    sizeof("[KDE Desktop Entry]") - 1,    "application/x-kdelnk"},
    {"d8:announce",    sizeof("d8:announce") - 1,    "application/x-bittorrent"},
    {"drpm",    sizeof("drpm") - 1,    "application/x-rpm"},
    {"fLaC",    sizeof("fLaC") - 1,    "audio/x-flac"},
    {"filedesc://",    sizeof("filedesc://") - 1,    "application/x-ia-arc"},
    {"gimp xcf",    sizeof("gimp xcf") - 1,    "image/x-xcf"},
    {"{\\rtf",    sizeof("{\\rtf") - 1,    "text/rtf"},
    {"\x89" "HDF\r\n" "\x1a" "\n",    sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1,    "application/x-hdf"},
    {"\x89" "PNG\r\n" "\x1a" "\n",    sizeof("\x89" "PNG\r\n" "\x1a" "\n") - 1,    "image/png"},
    {"\x8a" "MNG",    sizeof("\x8a" "MNG") - 1,    "video/x-mng"},
    {"\x94" "\xa6" ".",    sizeof("\x94" "\xa6" ".") - 1,    "application/msword"},
    {"\xdb" "\xa5" "-" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00") - 1,    "application/msword"},
    {"\xdb" "\xa5" "-" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00") - 1,    "application/msword"},
    {"\xdb" "\xa5" "-" "\x00" "\x00" "\x00",    sizeof("\xdb" "\xa5" "-" "\x00" "\x00" "\x00") - 1,    "application/msword"},
    {"\xf7" "\x02",    sizeof("\xf7" "\x02") - 1,    "application/x-dvi"},
    {"\xfd" "7zXZ" "\x00",    sizeof("\xfd" "7zXZ" "\x00") - 1,    "application/x-xz"},
    {"\xfe" "7" "\x00" "#",    sizeof("\xfe" "7" "\x00" "#") - 1,    "application/msword"},
    {"\xff" "\x1f",    sizeof("\xff" "\x1f") - 1,    "application/octet-stream"},
};
static const size_t stringMap2Count = 92;

static StringMap stringMap3[] = {
    {"3",    sizeof("3") - 1,    "application/vnd.cups-raster"},
};
static const size_t stringMap3Count = 1;

static StringMap stringMap4[] = {
    {"C",    sizeof("C") - 1,    "application/x-shockwave-flash"},
    {"F",    sizeof("F") - 1,    "application/x-shockwave-flash"},
    {"Z",    sizeof("Z") - 1,    "application/x-shockwave-flash"},
};
static const size_t stringMap4Count = 3;

static Result
runTests(const Byte* buf, size_t len, const char** mime)
{
    Result rslt;
    Bool   haveError = False;
    size_t off0, off1, off2, off3, off4, off5, off6, off7, off8;

    // line 810
    rslt = beShortGroup(buf, len, beshortMap1, beshortMap1Count, mime);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        return Match;
    }

    // line 548
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000100, CompareEq, 0xFFFFFF00, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 549
        off1 = 3;
        rslt = byteMatch(buf, len, 0xBA, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg";
            return Match;
        }
        // line 560
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg4-generic";
            return Match;
        }
        // line 632
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB5, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg4-generic";
            return Match;
        }
        // line 643
        off1 = 3;
        rslt = byteMatch(buf, len, 0xB3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mpeg";
            return Match;
        }
    }

    // line 762
    off0 = 0;
    rslt = beShortMatch(buf, len, 0xFFFA, CompareEq, 0xFFFE, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 764
        off1 = 2;
        rslt = byteMatch(buf, len, 0x10, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 766
        off1 = 2;
        rslt = byteMatch(buf, len, 0x20, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 768
        off1 = 2;
        rslt = byteMatch(buf, len, 0x30, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 770
        off1 = 2;
        rslt = byteMatch(buf, len, 0x40, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 772
        off1 = 2;
        rslt = byteMatch(buf, len, 0x50, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 774
        off1 = 2;
        rslt = byteMatch(buf, len, 0x60, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 776
        off1 = 2;
        rslt = byteMatch(buf, len, 0x70, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 778
        off1 = 2;
        rslt = byteMatch(buf, len, 0x80, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 780
        off1 = 2;
        rslt = byteMatch(buf, len, 0x90, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 782
        off1 = 2;
        rslt = byteMatch(buf, len, 0xA0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 784
        off1 = 2;
        rslt = byteMatch(buf, len, 0xB0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 786
        off1 = 2;
        rslt = byteMatch(buf, len, 0xC0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 788
        off1 = 2;
        rslt = byteMatch(buf, len, 0xD0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
        // line 790
        off1 = 2;
        rslt = byteMatch(buf, len, 0xE0, CompareEq, 0xF0, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mpeg";
            return Match;
        }
    }

    // line 1111
    off0 = 4;
    rslt = leShortMatch(buf, len, 0xAF11, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1113
        off1 = 8;
        rslt = leShortMatch(buf, len, 320, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 1114
            off2 = 10;
            rslt = leShortMatch(buf, len, 200, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 1115
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

    // line 1124
    off0 = 4;
    rslt = leShortMatch(buf, len, 0xAF12, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1126
        off1 = 12;
        rslt = leShortMatch(buf, len, 8, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-flc";
            return Match;
        }
    }

    // line 1181
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x3026b275, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/x-ms-asf";
        return Match;
    }

    // line 2289
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x1ee7ff00, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-eet";
        return Match;
    }

    // line 2314
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10201A7A, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "x-epoc/x-sisx-app";
        return Match;
    }

    // line 2629
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x2e7261fd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "audio/x-pn-realaudio";
        return Match;
    }

    // line 3676
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafebabe, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 3677
        off1 = 4;
        rslt = beLongMatch(buf, len, 30, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-java-applet";
            return Match;
        }
    }

    // line 3690
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafed00d, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-java-pack200";
        return Match;
    }

    // line 3696
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xcafed00d, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-java-pack200";
        return Match;
    }

    // line 4099
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x1f1f, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 4105
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x1fff, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 4111
    off0 = 0;
    rslt = leShortMatch(buf, len, 0145405, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 4232
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x5d, CompareEq, 0xffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4233
        off1 = 12;
        rslt = leShortMatch(buf, len, 0xff, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-lzma";
            return Match;
        }
    }

    // line 4252
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184d2204, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 4255
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184c2103, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 4257
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x184c2102, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lz4";
        return Match;
    }

    // line 4755
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x13579acd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 4757
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x13579acd, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 4759
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x13579acf, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 4761
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x13579acf, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-gdbm";
        return Match;
    }

    // line 4893
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000C20, CompareLt, 0x0000FFFF, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4982
        off1 = 0;
        rslt = byteMatch(buf, len, 1, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 4985
            off2 = 0;
            rslt = byteMatch(buf, len, 0x03, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 4988
            off2 = 0;
            rslt = byteMatch(buf, len, 0x04, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 4991
            off2 = 0;
            rslt = byteMatch(buf, len, 0x05, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 4993
            off2 = 0;
            rslt = byteMatch(buf, len, 0x30, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 4995
            off2 = 0;
            rslt = byteMatch(buf, len, 0x31, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 4998
            off2 = 0;
            rslt = byteMatch(buf, len, 0x32, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5001
            off2 = 0;
            rslt = byteMatch(buf, len, 0x43, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5007
            off2 = 0;
            rslt = byteMatch(buf, len, 0x7b, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5013
            off2 = 0;
            rslt = byteMatch(buf, len, 0x83, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5016
            off2 = 0;
            rslt = byteMatch(buf, len, 0x87, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5022
            off2 = 0;
            rslt = byteMatch(buf, len, 0x8B, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5025
            off2 = 0;
            rslt = byteMatch(buf, len, 0x8E, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5033
            off2 = 0;
            rslt = byteMatch(buf, len, 0xCB, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5036
            off2 = 0;
            rslt = byteMatch(buf, len, 0xE5, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-dbf";
                return Match;
            }
            // line 5041
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

    // line 5598
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x0ef1fab9, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 5626
        off1 = 16;
        rslt = leShortMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/octet-stream";
            return Match;
        }
        // line 5628
        off1 = 16;
        rslt = leShortMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-object";
            return Match;
        }
        // line 5630
        off1 = 16;
        rslt = leShortMatch(buf, len, 2, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-executable";
            return Match;
        }
        // line 5632
        off1 = 16;
        rslt = leShortMatch(buf, len, 3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-sharedlib";
            return Match;
        }
        // line 5634
        off1 = 16;
        rslt = leShortMatch(buf, len, 4, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-coredump";
            return Match;
        }
    }

    // line 5961
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10000037, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 5968
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x1000006D, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 5969
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x1000007D, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-epoc-sketch";
                return Match;
            }
            // line 5972
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x1000007F, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-word";
                return Match;
            }
            // line 5974
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000085, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-opl";
                return Match;
            }
            // line 5977
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000088, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-sheet";
                return Match;
            }
        }
        // line 5980
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x10000073, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-epoc-opo";
            return Match;
        }
        // line 5982
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x10000074, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-epoc-app";
            return Match;
        }
    }

    // line 5990
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x10000050, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 5991
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x1000006D, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 5992
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000084, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-agenda";
                return Match;
            }
            // line 5994
            off2 = 8;
            rslt = leLongMatch(buf, len, 0x10000086, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-epoc-data";
                return Match;
            }
            // line 5996
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

    // line 6133
    off0 = 0;
    rslt = beLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-font-sfn";
        return Match;
    }

    // line 6137
    off0 = 0;
    rslt = leLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6138
        off1 = 104;
        rslt = leLongMatch(buf, len, 00000004, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-font-sfn";
            return Match;
        }
    }

    // line 8537
    off0 = 0;
    rslt = beLongMatch(buf, len, 100, CompareGt, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8538
        off1 = 8;
        rslt = beLongMatch(buf, len, 3, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8539
            off2 = 12;
            rslt = beLongMatch(buf, len, 33, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 8540
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

    // line 8605
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x0a000000, CompareEq, 0xffF8fe00, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8607
        off1 = 3;
        rslt = byteMatch(buf, len, 0, CompareGt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8609
            off2 = 1;
            rslt = byteMatch(buf, len, 6, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 8610
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

    // line 8819
    off0 = 0;
    rslt = leLongMatch(buf, len, 20000630, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-exr";
        return Match;
    }

    // line 8879
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x0e031301, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 11173
    off0 = 0;
    rslt = leLongMatch(buf, len, 0x000000E9, CompareEq, 0x804000E9, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 11177
        off1 = 11;
        rslt = leShortMatch(buf, len, 0, CompareEq, 0xf001f, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 11178
            off2 = 11;
            rslt = leShortMatch(buf, len, 32769, CompareLt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 11179
                off3 = 11;
                rslt = leShortMatch(buf, len, 31, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 11180
                    off4 = 21;
                    rslt = byteMatch(buf, len, 0xF0, CompareEq, 0xf0, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 11285
                        off5 = 21;
                        rslt = byteMatch(buf, len, 0xF8, CompareEq|CompareNot, 0xffffffff, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 11287
                            off6 = 54;
                            rslt = !stringEqual(buf, len, "FAT16", sizeof("FAT16") - 1, &off6);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                // line 11289
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

    // line 12822
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x1a45dfa3, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12824
        off1 = 4;
        rslt = stringSearch(buf, len, "B" "\x82", sizeof("B" "\x82") - 1, &off1, 4096, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 12826
            off2 = 1;
            off2 += off1;
            rslt = stringMatch(buf, len, "webm", sizeof("webm") - 1, &off2, CompareEq, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "video/webm";
                return Match;
            }
            // line 12828
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

    // line 13659
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x31be0000, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 13759
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000100, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 13760
        off1 = 9;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-icon";
            return Match;
        }
        // line 13764
        off1 = 9;
        rslt = byteMatch(buf, len, 0xff, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-icon";
            return Match;
        }
    }

    // line 13781
    off0 = 0;
    rslt = beLongMatch(buf, len, 0x00000200, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 13782
        off1 = 9;
        rslt = byteMatch(buf, len, 0, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-cur";
            return Match;
        }
        // line 13786
        off1 = 9;
        rslt = byteMatch(buf, len, 0xff, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-cur";
            return Match;
        }
    }

    // line 17051
    off0 = 0;
    rslt = beLongMatch(buf, len, 0xedabeedb, CompareEq, 0xffffffff, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-rpm";
        return Match;
    }

    // line 20141
    off0 = 0;
    rslt = leShortMatch(buf, len, 0x0000, CompareEq, 0xFeFe, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20143
        off1 = 4;
        rslt = leLongMatch(buf, len, 0x00000000, CompareEq, 0xFCffFe00, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 20145
            off2 = 68;
            rslt = leLongMatch(buf, len, 0x57, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 20148
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

    // line 1027
    rslt = stringEqualMap(buf, len, stringMap2, stringMap2Count, mime);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        return Match;
    }

    // line 480
    off0 = 4;
    rslt = stringEqual(buf, len, "moov", sizeof("moov") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/quicktime";
        return Match;
    }

    // line 486
    off0 = 4;
    rslt = stringEqual(buf, len, "mdat", sizeof("mdat") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "video/quicktime";
        return Match;
    }

    // line 494
    off0 = 4;
    rslt = stringEqual(buf, len, "idsc", sizeof("idsc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-quicktime";
        return Match;
    }

    // line 498
    off0 = 4;
    rslt = stringEqual(buf, len, "pckg", sizeof("pckg") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-quicktime-player";
        return Match;
    }

    // line 502
    off0 = 4;
    rslt = stringEqual(buf, len, "ftyp", sizeof("ftyp") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 503
        off1 = 8;
        rslt = stringEqual(buf, len, "isom", sizeof("isom") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 506
        off1 = 8;
        rslt = stringEqual(buf, len, "mp41", sizeof("mp41") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 508
        off1 = 8;
        rslt = stringEqual(buf, len, "mp42", sizeof("mp42") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 514
        off1 = 8;
        rslt = stringEqual(buf, len, "3ge", sizeof("3ge") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 516
        off1 = 8;
        rslt = stringEqual(buf, len, "3gg", sizeof("3gg") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 518
        off1 = 8;
        rslt = stringEqual(buf, len, "3gp", sizeof("3gp") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 520
        off1 = 8;
        rslt = stringEqual(buf, len, "3gs", sizeof("3gs") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 522
        off1 = 8;
        rslt = stringEqual(buf, len, "3g2", sizeof("3g2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp2";
            return Match;
        }
        // line 527
        off1 = 8;
        rslt = stringEqual(buf, len, "mmp4", sizeof("mmp4") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 529
        off1 = 8;
        rslt = stringEqual(buf, len, "avc1", sizeof("avc1") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/3gpp";
            return Match;
        }
        // line 512
        off1 = 8;
        rslt = stringMatch(buf, len, "jp2", sizeof("jp2") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jp2";
            return Match;
        }
        // line 531
        off1 = 8;
        rslt = stringMatch(buf, len, "M4A", sizeof("M4A") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/mp4";
            return Match;
        }
        // line 533
        off1 = 8;
        rslt = stringMatch(buf, len, "M4V", sizeof("M4V") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mp4";
            return Match;
        }
        // line 537
        off1 = 8;
        rslt = stringMatch(buf, len, "qt", sizeof("qt") - 1, &off1, CompareEq, 0|CompactWS);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/quicktime";
            return Match;
        }
    }

    // line 1211
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1213
        off1 = 20;
        rslt = stringSearch(buf, len, "<!DOCTYPE X3D", sizeof("<!DOCTYPE X3D") - 1, &off1, 1000, 0|IgnoreWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "model/x3d";
            return Match;
        }
    }

    // line 1439
    off0 = 257;
    rslt = stringEqual(buf, len, "ustar" "\x00", sizeof("ustar" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tar";
        return Match;
    }

    // line 1441
    off0 = 257;
    rslt = stringEqual(buf, len, "ustar  " "\x00", sizeof("ustar  " "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tar";
        return Match;
    }

    // line 2025
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 2026
        off1 = 30;
        rslt = beLongMatch(buf, len, 0x6d696d65, CompareEq|CompareNot, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2027
            off2 = 4;
            rslt = byteMatch(buf, len, 0x00, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2029
            off2 = 4;
            rslt = byteMatch(buf, len, 0x09, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2031
            off2 = 4;
            rslt = byteMatch(buf, len, 0x0a, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2033
            off2 = 4;
            rslt = byteMatch(buf, len, 0x0b, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2037
            off2 = 4;
            rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
            // line 2035
            off2 = 0x161;
            rslt = stringEqual(buf, len, "WINZIP", sizeof("WINZIP") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
        }
        // line 2151
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
        // line 2156
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
            // line 2157
            off2 = 26;
            rslt = !stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetype", sizeof("\b" "\x00" "\x00" "\x00" "mimetype") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/zip";
                return Match;
            }
        }
        // line 2044
        off1 = 26;
        rslt = stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetypeapplication/", sizeof("\b" "\x00" "\x00" "\x00" "mimetypeapplication/") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2083
            off2 = 50;
            rslt = stringEqual(buf, len, "vnd.oasis.opendocument.", sizeof("vnd.oasis.opendocument.") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2084
                off3 = 73;
                rslt = stringEqual(buf, len, "text", sizeof("text") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2085
                    off4 = 77;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text";
                        return Match;
                    }
                    // line 2087
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-template";
                        return Match;
                    }
                    // line 2089
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-web", sizeof("-web") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-web";
                        return Match;
                    }
                    // line 2091
                    off4 = 77;
                    rslt = stringEqual(buf, len, "-master", sizeof("-master") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.text-master";
                        return Match;
                    }
                }
                // line 2093
                off3 = 73;
                rslt = stringEqual(buf, len, "graphics", sizeof("graphics") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2094
                    off4 = 81;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.graphics";
                        return Match;
                    }
                    // line 2096
                    off4 = 81;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.graphics-template";
                        return Match;
                    }
                }
                // line 2098
                off3 = 73;
                rslt = stringEqual(buf, len, "presentation", sizeof("presentation") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2099
                    off4 = 85;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.presentation";
                        return Match;
                    }
                    // line 2101
                    off4 = 85;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.presentation-template";
                        return Match;
                    }
                }
                // line 2103
                off3 = 73;
                rslt = stringEqual(buf, len, "spreadsheet", sizeof("spreadsheet") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2104
                    off4 = 84;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.spreadsheet";
                        return Match;
                    }
                    // line 2106
                    off4 = 84;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.spreadsheet-template";
                        return Match;
                    }
                }
                // line 2108
                off3 = 73;
                rslt = stringEqual(buf, len, "chart", sizeof("chart") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2109
                    off4 = 78;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.chart";
                        return Match;
                    }
                    // line 2111
                    off4 = 78;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.chart-template";
                        return Match;
                    }
                }
                // line 2113
                off3 = 73;
                rslt = stringEqual(buf, len, "formula", sizeof("formula") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2114
                    off4 = 80;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.formula";
                        return Match;
                    }
                    // line 2116
                    off4 = 80;
                    rslt = stringEqual(buf, len, "-template", sizeof("-template") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.formula-template";
                        return Match;
                    }
                }
                // line 2118
                off3 = 73;
                rslt = stringEqual(buf, len, "database", sizeof("database") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/vnd.oasis.opendocument.database";
                    return Match;
                }
                // line 2120
                off3 = 73;
                rslt = stringEqual(buf, len, "image", sizeof("image") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2121
                    off4 = 78;
                    rslt = byteMatch(buf, len, 0x2d, CompareEq|CompareNot, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.oasis.opendocument.image";
                        return Match;
                    }
                    // line 2123
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
            // line 2129
            off2 = 50;
            rslt = stringEqual(buf, len, "epub+zip", sizeof("epub+zip") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/epub+zip";
                return Match;
            }
            // line 2138
            off2 = 50;
            rslt = !stringEqual(buf, len, "epub+zip", sizeof("epub+zip") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2139
                off3 = 50;
                rslt = !stringEqual(buf, len, "vnd.oasis.opendocument.", sizeof("vnd.oasis.opendocument.") - 1, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 2140
                    off4 = 50;
                    rslt = !stringEqual(buf, len, "vnd.sun.xml.", sizeof("vnd.sun.xml.") - 1, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 2141
                        off5 = 50;
                        rslt = !stringEqual(buf, len, "vnd.kde.", sizeof("vnd.kde.") - 1, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 2142
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
        // line 2145
        off1 = 26;
        rslt = stringEqual(buf, len, "\b" "\x00" "\x00" "\x00" "mimetype", sizeof("\b" "\x00" "\x00" "\x00" "mimetype") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 2146
            off2 = 38;
            rslt = !stringEqual(buf, len, "application/", sizeof("application/") - 1, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 2147
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

    // line 2185
    off0 = 10;
    rslt = stringEqual(buf, len, "# This is a shell archive", sizeof("# This is a shell archive") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/octet-stream";
        return Match;
    }

    // line 2521
    off0 = 0;
    rslt = stringEqual(buf, len, ".snd", sizeof(".snd") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 2522
        off1 = 12;
        rslt = beLongMatch(buf, len, 1, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2524
        off1 = 12;
        rslt = beLongMatch(buf, len, 2, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2526
        off1 = 12;
        rslt = beLongMatch(buf, len, 3, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2528
        off1 = 12;
        rslt = beLongMatch(buf, len, 4, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2530
        off1 = 12;
        rslt = beLongMatch(buf, len, 5, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2532
        off1 = 12;
        rslt = beLongMatch(buf, len, 6, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2534
        off1 = 12;
        rslt = beLongMatch(buf, len, 7, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/basic";
            return Match;
        }
        // line 2546
        off1 = 12;
        rslt = beLongMatch(buf, len, 23, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-adpcm";
            return Match;
        }
    }

    // line 4006
    off0 = 0;
    rslt = stringEqual(buf, len, "<?php /* Smarty version", sizeof("<?php /* Smarty version") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4007
        off1 = 24;
        rslt = regexMatch(buf, len, "[0-9.]+", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-php";
            return Match;
        }
    }

    // line 4226
    off0 = 0;
    rslt = stringEqual(buf, len, "7z" "\xbc" "\xaf" "'" "\x1c", sizeof("7z" "\xbc" "\xaf" "'" "\x1c") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-7z-compressed";
        return Match;
    }

    // line 4246
    off0 = 0;
    rslt = stringEqual(buf, len, "LRZI", sizeof("LRZI") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-lrzip";
        return Match;
    }

    // line 4718
    off0 = 0;
    rslt = stringEqual(buf, len, "RaS", sizeof("RaS") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4721
        off1 = 3;
        rslt = stringEqual(buf, len, "3", sizeof("3") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.cups-raster";
            return Match;
        }
    }

    // line 4727
    off0 = 1;
    rslt = stringEqual(buf, len, "SaR", sizeof("SaR") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 4730
        rslt = stringEqualMap(buf, len, stringMap3, stringMap3Count, mime);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            return Match;
        }
    }

    // line 5149
    off0 = 4;
    rslt = stringEqual(buf, len, "Standard Jet DB", sizeof("Standard Jet DB") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-msaccess";
        return Match;
    }

    // line 5151
    off0 = 4;
    rslt = stringEqual(buf, len, "Standard ACE DB", sizeof("Standard ACE DB") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-msaccess";
        return Match;
    }

    // line 6071
    off0 = 0;
    rslt = stringEqual(buf, len, "FCS3.0", sizeof("FCS3.0") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 6086
        rslt = stringEqualMap(buf, len, stringMap4, stringMap4Count, mime);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            return Match;
        }
    }

    // line 6203
    off0 = 34;
    rslt = stringEqual(buf, len, "LP", sizeof("LP") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-fontobject";
        return Match;
    }

    // line 8186
    off0 = 0;
    rslt = stringEqual(buf, len, "P4", sizeof("P4") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8188
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8189
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-bitmap";
                return Match;
            }
        }
    }

    // line 8192
    off0 = 0;
    rslt = stringEqual(buf, len, "P5", sizeof("P5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8194
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8195
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-greymap";
                return Match;
            }
        }
    }

    // line 8198
    off0 = 0;
    rslt = stringEqual(buf, len, "P6", sizeof("P6") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8200
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8201
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-pixmap";
                return Match;
            }
        }
    }

    // line 8373
    off0 = 0;
    rslt = stringEqual(buf, len, "AWBM", sizeof("AWBM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8374
        off1 = 4;
        rslt = leShortMatch(buf, len, 1981, CompareLt, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-award-bmp";
            return Match;
        }
    }

    // line 8393
    off0 = 0;
    rslt = stringEqual(buf, len, "BM", sizeof("BM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8394
        off1 = 14;
        rslt = leShortMatch(buf, len, 12, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 8398
        off1 = 14;
        rslt = leShortMatch(buf, len, 64, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 8402
        off1 = 14;
        rslt = leShortMatch(buf, len, 40, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 8407
        off1 = 14;
        rslt = leShortMatch(buf, len, 124, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 8412
        off1 = 14;
        rslt = leShortMatch(buf, len, 108, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
        // line 8417
        off1 = 14;
        rslt = leShortMatch(buf, len, 128, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-ms-bmp";
            return Match;
        }
    }

    // line 8525
    off0 = 128;
    rslt = stringEqual(buf, len, "DICM", sizeof("DICM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/dicom";
        return Match;
    }

    // line 8806
    off0 = 0;
    rslt = stringEqual(buf, len, "AT&TFORM", sizeof("AT&TFORM") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8807
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVM", sizeof("DJVM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 8809
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVU", sizeof("DJVU") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 8811
        off1 = 12;
        rslt = stringEqual(buf, len, "DJVI", sizeof("DJVI") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
        // line 8813
        off1 = 12;
        rslt = stringEqual(buf, len, "THUM", sizeof("THUM") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/vnd.djvu";
            return Match;
        }
    }

    // line 8883
    off0 = 512;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 8885
    off0 = 1024;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 8887
    off0 = 2048;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 8889
    off0 = 4096;
    rslt = stringEqual(buf, len, "\x89" "HDF\r\n" "\x1a" "\n", sizeof("\x89" "HDF\r\n" "\x1a" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hdf";
        return Match;
    }

    // line 9380
    off0 = 0;
    rslt = stringEqual(buf, len, "\x00" "\x00" "\x00" "\fjP  \r\n" "\x87" "\n", sizeof("\x00" "\x00" "\x00" "\fjP  \r\n" "\x87" "\n") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 9386
        off1 = 20;
        rslt = stringEqual(buf, len, "jp2 ", sizeof("jp2 ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jp2";
            return Match;
        }
        // line 9388
        off1 = 20;
        rslt = stringEqual(buf, len, "jpx ", sizeof("jpx ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpx";
            return Match;
        }
        // line 9390
        off1 = 20;
        rslt = stringEqual(buf, len, "jpm ", sizeof("jpm ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/jpm";
            return Match;
        }
        // line 9392
        off1 = 20;
        rslt = stringEqual(buf, len, "mjp2", sizeof("mjp2") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/mj2";
            return Match;
        }
    }

    // line 9760
    off0 = 0;
    rslt = stringEqual(buf, len, "LPKSHHRH", sizeof("LPKSHHRH") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 9762
        off1 = 16;
        rslt = byteMatch(buf, len, 0, CompareEq, 252, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 9764
            off2 = 24;
            rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 9765
                off3 = 32;
                rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 9766
                    off4 = 40;
                    rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off4);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        // line 9767
                        off5 = 48;
                        rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off5);
                        if (rslt < 0) haveError = True;
                        if (rslt > 0)
                        {
                            // line 9768
                            off6 = 56;
                            rslt = beQuadMatch(buf, len, 0, CompareGt, 0xffffffff, &off6);
                            if (rslt < 0) haveError = True;
                            if (rslt > 0)
                            {
                                // line 9769
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

    // line 11702
    off0 = 32769;
    rslt = stringEqual(buf, len, "CD001", sizeof("CD001") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-iso9660-image";
        return Match;
    }

    // line 11715
    off0 = 37633;
    rslt = stringEqual(buf, len, "CD001", sizeof("CD001") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-iso9660-image";
        return Match;
    }

    // line 12146
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml", sizeof("<?xml") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12147
        off1 = 20;
        rslt = stringSearch(buf, len, " xmlns=", sizeof(" xmlns=") - 1, &off1, 400, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 12148
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "['\"]http://earth.google.com/kml", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/vnd.google-earth.kml+xml";
                return Match;
            }
            // line 12160
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

    // line 12168
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 12169
        off1 = 4;
        rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 12170
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

    // line 13167
    off0 = 0;
    rslt = stringEqual(buf, len, "@", sizeof("@") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 13168
        off1 = 1;
        rslt = stringMatch(buf, len, " echo off", sizeof(" echo off") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 13170
        off1 = 1;
        rslt = stringMatch(buf, len, "echo off", sizeof("echo off") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 13172
        off1 = 1;
        rslt = stringMatch(buf, len, "rem", sizeof("rem") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
        // line 13174
        off1 = 1;
        rslt = stringMatch(buf, len, "set ", sizeof("set ") - 1, &off1, CompareEq, 0|CompactWS|MatchLower);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-msdos-batch";
            return Match;
        }
    }

    // line 13202
    off0 = 0;
    rslt = stringEqual(buf, len, "MZ", sizeof("MZ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 13411
        off1 = 0x1e;
        rslt = stringEqual(buf, len, "Copyright 1989-1990 PKWARE Inc.", sizeof("Copyright 1989-1990 PKWARE Inc.") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/zip";
            return Match;
        }
        // line 13414
        off1 = 0x1e;
        rslt = stringEqual(buf, len, "PKLITE Copr.", sizeof("PKLITE Copr.") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/zip";
            return Match;
        }
    }

    // line 13651
    off0 = 2080;
    rslt = stringEqual(buf, len, "Microsoft Word 6.0 Document", sizeof("Microsoft Word 6.0 Document") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 13653
    off0 = 2080;
    rslt = stringEqual(buf, len, "Documento Microsoft Word 6", sizeof("Documento Microsoft Word 6") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 13656
    off0 = 2112;
    rslt = stringEqual(buf, len, "MSWordDoc", sizeof("MSWordDoc") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 13669
    off0 = 512;
    rslt = stringEqual(buf, len, "\xec" "\xa5" "\xc1", sizeof("\xec" "\xa5" "\xc1") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 13676
    off0 = 2080;
    rslt = stringEqual(buf, len, "Microsoft Excel 5.0 Worksheet", sizeof("Microsoft Excel 5.0 Worksheet") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 13682
    off0 = 2080;
    rslt = stringEqual(buf, len, "Foglio di lavoro Microsoft Exce", sizeof("Foglio di lavoro Microsoft Exce") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 13686
    off0 = 2114;
    rslt = stringEqual(buf, len, "Biff5", sizeof("Biff5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 13689
    off0 = 2121;
    rslt = stringEqual(buf, len, "Biff5", sizeof("Biff5") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/vnd.ms-excel";
        return Match;
    }

    // line 13980
    off0 = 0;
    rslt = stringEqual(buf, len, "\xd0" "\xcf" "\x11" "\xe0" "\xa1" "\xb1" "\x1a" "\xe1", sizeof("\xd0" "\xcf" "\x11" "\xe0" "\xa1" "\xb1" "\x1a" "\xe1") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 13983
        off1 = 546;
        rslt = stringEqual(buf, len, "bjbj", sizeof("bjbj") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
        // line 13985
        off1 = 546;
        rslt = stringEqual(buf, len, "jbjb", sizeof("jbjb") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/msword";
            return Match;
        }
    }

    // line 13991
    off0 = 512;
    rslt = stringEqual(buf, len, "R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00" " " "\x00" "E" "\x00" "n" "\x00" "t" "\x00" "r" "\x00" "y", sizeof("R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00" " " "\x00" "E" "\x00" "n" "\x00" "t" "\x00" "r" "\x00" "y") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/msword";
        return Match;
    }

    // line 14020
    off0 = 0;
    rslt = stringEqual(buf, len, "ITOLITLS", sizeof("ITOLITLS") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-ms-reader";
        return Match;
    }

    // line 14092
    off0 = 0;
    rslt = stringEqual(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 14095
        off1 = 0x1E;
        rslt = regexMatch(buf, len, "[Content_Types].xml|_rels/.rels", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 14099
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
                // line 14102
                off3 = 26;
                off3 += off2;
                rslt = stringSearch(buf, len, "PK" "\x03" "\x04", sizeof("PK" "\x03" "\x04") - 1, &off3, 1000, 0);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 14106
                    off4 = 26;
                    off4 += off3;
                    rslt = stringMatch(buf, len, "word/", sizeof("word/") - 1, &off4, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
                        return Match;
                    }
                    // line 14108
                    off4 = 26;
                    off4 += off3;
                    rslt = stringMatch(buf, len, "ppt/", sizeof("ppt/") - 1, &off4, CompareEq, 0);
                    if (rslt < 0) haveError = True;
                    if (rslt > 0)
                    {
                        *mime = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
                        return Match;
                    }
                    // line 14110
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

    // line 15644
    off0 = 2;
    rslt = stringEqual(buf, len, "---BEGIN PGP PUBLIC KEY BLOCK-", sizeof("---BEGIN PGP PUBLIC KEY BLOCK-") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/pgp-keys";
        return Match;
    }

    // line 16069
    off0 = 0;
    rslt = stringEqual(buf, len, "RIFF", sizeof("RIFF") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 16094
        off1 = 8;
        rslt = stringEqual(buf, len, "WAVE", sizeof("WAVE") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "audio/x-wav";
            return Match;
        }
        // line 16099
        off1 = 8;
        rslt = stringEqual(buf, len, "CDRA", sizeof("CDRA") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-coreldraw";
            return Match;
        }
        // line 16101
        off1 = 8;
        rslt = stringEqual(buf, len, "CDR6", sizeof("CDR6") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "image/x-coreldraw";
            return Match;
        }
        // line 16105
        off1 = 8;
        rslt = stringEqual(buf, len, "AVI ", sizeof("AVI ") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "video/x-msvideo";
            return Match;
        }
    }

    // line 17008
    off0 = 60;
    rslt = stringEqual(buf, len, "RINEX", sizeof("RINEX") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17009
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXB", sizeof("XXRINEXB") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/broadcast";
            return Match;
        }
        // line 17013
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXD", sizeof("XXRINEXD") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/observation";
            return Match;
        }
        // line 17017
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXC", sizeof("XXRINEXC") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/clock";
            return Match;
        }
        // line 17021
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXH", sizeof("XXRINEXH") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 17025
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXG", sizeof("XXRINEXG") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 17029
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXL", sizeof("XXRINEXL") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 17033
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXM", sizeof("XXRINEXM") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/meteorological";
            return Match;
        }
        // line 17037
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXN", sizeof("XXRINEXN") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/navigation";
            return Match;
        }
        // line 17041
        off1 = 80;
        rslt = stringSearch(buf, len, "XXRINEXO", sizeof("XXRINEXO") - 1, &off1, 256, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "rinex/observation";
            return Match;
        }
    }

    // line 17255
    off0 = 0;
    rslt = stringEqual(buf, len, "HEADER   ", sizeof("HEADER   ") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17256
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^.{40}", &off1, 1 * 80, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17257
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "[0-9]{2}-[A-Z]{3}-[0-9]{2} {3}", &off2, 1 * 80, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 17258
                off3 = 0;
                off3 += off2;
                rslt = regexMatch(buf, len, "[A-Z0-9]{4}.{14}$", &off3, 1 * 80, 0|RegexBegin);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 17259
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

    // line 17530
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17531
        off1 = 15;
        rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17532
            off2 = 19;
            rslt = stringSearch(buf, len, "<svg", sizeof("<svg") - 1, &off2, 4096, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/svg+xml";
                return Match;
            }
            // line 17534
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

    // line 17538
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17539
        off1 = 15;
        rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17540
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

    // line 17551
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17552
        off1 = 15;
        rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17553
            off2 = 19;
            rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off2, 4096, 0|CompactWS|MatchLower);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 17555
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version='", sizeof("<?xml version='") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17556
        off1 = 15;
        rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17557
            off2 = 19;
            rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off2, 4096, 0|CompactWS|MatchLower);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 17559
    off0 = 0;
    rslt = stringEqual(buf, len, "<?xml version=\"", sizeof("<?xml version=\"") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17560
        off1 = 15;
        rslt = stringGreater(buf, len, "\x00", sizeof("\x00") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17561
            off2 = 19;
            rslt = stringSearch(buf, len, "<html", sizeof("<html") - 1, &off2, 4096, 0|CompactWS|MatchLower);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/html";
                return Match;
            }
        }
    }

    // line 18843
    off0 = 2;
    rslt = stringEqual(buf, len, "\x00" "\x11", sizeof("\x00" "\x11") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tex-tfm";
        return Match;
    }

    // line 18846
    off0 = 2;
    rslt = stringEqual(buf, len, "\x00" "\x12", sizeof("\x00" "\x12") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-tex-tfm";
        return Match;
    }

    // line 20354
    off0 = 512;
    rslt = stringEqual(buf, len, "R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00", sizeof("R" "\x00" "o" "\x00" "o" "\x00" "t" "\x00") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-hwp";
        return Match;
    }

    // line 20385
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20386
        off1 = 43;
        rslt = byteMatch(buf, len, 0x14, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro4";
            return Match;
        }
    }

    // line 20390
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20391
        off1 = 43;
        rslt = byteMatch(buf, len, 0x15, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro5";
            return Match;
        }
    }

    // line 20394
    off0 = 0;
    rslt = stringEqual(buf, len, "DOC", sizeof("DOC") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20395
        off1 = 43;
        rslt = byteMatch(buf, len, 0x16, CompareEq, 0xffffffff, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/x-ichitaro6";
            return Match;
        }
    }

    // line 1523
    off0 = 0;
    rslt = !stringEqual(buf, len, "<arch>\ndebian", sizeof("<arch>\ndebian") - 1, &off0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 1524
        off1 = 8;
        rslt = stringEqual(buf, len, "debian-split", sizeof("debian-split") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.debian.binary-package";
            return Match;
        }
        // line 1526
        off1 = 8;
        rslt = stringEqual(buf, len, "debian-binary", sizeof("debian-binary") - 1, &off1);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "application/vnd.debian.binary-package";
            return Match;
        }
    }

    // line 500
    off0 = 4;
    rslt = stringMatch(buf, len, "jP", sizeof("jP") - 1, &off0, CompareEq, 0|CompactWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/jp2";
        return Match;
    }

    // line 1204
    off0 = 0;
    rslt = stringMatch(buf, len, "#VRML V1.0 ascii", sizeof("#VRML V1.0 ascii") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "model/vrml";
        return Match;
    }

    // line 1206
    off0 = 0;
    rslt = stringMatch(buf, len, "#VRML V2.0 utf8", sizeof("#VRML V2.0 utf8") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "model/vrml";
        return Match;
    }

    // line 3914
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/sh", sizeof("#! /bin/sh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3916
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/sh", sizeof("#! /bin/sh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3919
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/csh", sizeof("#! /bin/csh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3923
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/ksh", sizeof("#! /bin/ksh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3925
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/ksh", sizeof("#! /bin/ksh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3928
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/tcsh", sizeof("#! /bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3930
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/tcsh", sizeof("#! /usr/bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3932
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/tcsh", sizeof("#! /usr/local/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3934
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/tcsh", sizeof("#! /usr/local/bin/tcsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3939
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/zsh", sizeof("#! /bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3941
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/zsh", sizeof("#! /usr/bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3943
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/zsh", sizeof("#! /usr/local/bin/zsh") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3945
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/ash", sizeof("#! /usr/local/bin/ash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3947
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/ae", sizeof("#! /usr/local/bin/ae") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3949
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/nawk", sizeof("#! /bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 3951
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/nawk", sizeof("#! /usr/bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 3953
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/nawk", sizeof("#! /usr/local/bin/nawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-nawk";
        return Match;
    }

    // line 3955
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/gawk", sizeof("#! /bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 3957
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/gawk", sizeof("#! /usr/bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 3959
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/gawk", sizeof("#! /usr/local/bin/gawk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-gawk";
        return Match;
    }

    // line 3962
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/awk", sizeof("#! /bin/awk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-awk";
        return Match;
    }

    // line 3964
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/awk", sizeof("#! /usr/bin/awk") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-awk";
        return Match;
    }

    // line 3972
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/bash", sizeof("#! /bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3974
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /bin/bash", sizeof("#! /bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3976
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/bash", sizeof("#! /usr/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3978
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/bin/bash", sizeof("#! /usr/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3980
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bash", sizeof("#! /usr/local/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3982
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bash", sizeof("#! /usr/local/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3984
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/bash", sizeof("#! /usr/local/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 3986
    off0 = 0;
    rslt = stringMatch(buf, len, "#! /usr/local/bin/bash", sizeof("#! /usr/local/bin/bash") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-shellscript";
        return Match;
    }

    // line 13032
    off0 = 0;
    rslt = stringMatch(buf, len, "BEGIN:VCALENDAR", sizeof("BEGIN:VCALENDAR") - 1, &off0, CompareEq, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/calendar";
        return Match;
    }

    // line 13034
    off0 = 0;
    rslt = stringMatch(buf, len, "BEGIN:VCARD", sizeof("BEGIN:VCARD") - 1, &off0, CompareEq, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-vcard";
        return Match;
    }

    // line 20400
    off0 = 0;
    rslt = stringMatch(buf, len, "<map version", sizeof("<map version") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-freemind";
        return Match;
    }

    // line 20405
    off0 = 0;
    rslt = stringMatch(buf, len, "<map version=\"freeplane", sizeof("<map version=\"freeplane") - 1, &off0, CompareEq, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-freeplane";
        return Match;
    }

    // line 3991
    off0 = 0;
    rslt = stringSearch(buf, len, "<?php", sizeof("<?php") - 1, &off0, 1, 0|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 3994
    off0 = 0;
    rslt = stringSearch(buf, len, "<?\n", sizeof("<?\n") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 3996
    off0 = 0;
    rslt = stringSearch(buf, len, "<?\r", sizeof("<?\r") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 3998
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/php", sizeof("#! /usr/local/bin/php") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 4001
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/php", sizeof("#! /usr/bin/php") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-php";
        return Match;
    }

    // line 6246
    off0 = 0;
    rslt = stringSearch(buf, len, "<MakerDictionary", sizeof("<MakerDictionary") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/x-mif";
        return Match;
    }

    // line 8168
    off0 = 0;
    rslt = stringSearch(buf, len, "P1", sizeof("P1") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8170
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8171
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-bitmap";
                return Match;
            }
        }
    }

    // line 8174
    off0 = 0;
    rslt = stringSearch(buf, len, "P2", sizeof("P2") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8176
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8177
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-greymap";
                return Match;
            }
        }
    }

    // line 8180
    off0 = 0;
    rslt = stringSearch(buf, len, "P3", sizeof("P3") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 8182
        off1 = 3;
        rslt = regexMatch(buf, len, "=[0-9]{1,50} ", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 8183
            off2 = 3;
            rslt = regexMatch(buf, len, "= [0-9]{1,50}", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "image/x-portable-pixmap";
                return Match;
            }
        }
    }

    // line 8431
    off0 = 0;
    rslt = stringSearch(buf, len, "/* XPM */", sizeof("/* XPM */") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "image/x-xpmi";
        return Match;
    }

    // line 9213
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/bin/node", sizeof("#!/bin/node") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 9215
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/node", sizeof("#!/usr/bin/node") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 9217
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/bin/nodejs", sizeof("#!/bin/nodejs") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 9219
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/nodejs", sizeof("#!/usr/bin/nodejs") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 9221
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env node", sizeof("#!/usr/bin/env node") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 9223
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env nodejs", sizeof("#!/usr/bin/env nodejs") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/javascript";
        return Match;
    }

    // line 12248
    off0 = 0;
    rslt = stringSearch(buf, len, "<TeXmacs|", sizeof("<TeXmacs|") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/texmacs";
        return Match;
    }

    // line 12279
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/lua", sizeof("#! /usr/bin/lua") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 12281
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/lua", sizeof("#! /usr/local/bin/lua") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 12283
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env lua", sizeof("#!/usr/bin/env lua") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 12285
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env lua", sizeof("#! /usr/bin/env lua") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-lua";
        return Match;
    }

    // line 15488
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /bin/perl", sizeof("eval \"exec /bin/perl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15490
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /usr/bin/perl", sizeof("eval \"exec /usr/bin/perl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15492
    off0 = 0;
    rslt = stringSearch(buf, len, "eval \"exec /usr/local/bin/perl", sizeof("eval \"exec /usr/local/bin/perl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15494
    off0 = 0;
    rslt = stringSearch(buf, len, "eval '(exit $?0)' && eval 'exec", sizeof("eval '(exit $?0)' && eval 'exec") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15496
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env perl", sizeof("#!/usr/bin/env perl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15498
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env perl", sizeof("#! /usr/bin/env perl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-perl";
        return Match;
    }

    // line 15500
    off0 = 0;
    rslt = stringSearch(buf, len, "#!", sizeof("#!") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15501
        off1 = 0;
        rslt = regexMatch(buf, len, "^#!.*/bin/perl$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-perl";
            return Match;
        }
    }

    // line 15926
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/python", sizeof("#! /usr/bin/python") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 15928
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/python", sizeof("#! /usr/local/bin/python") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 15930
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env python", sizeof("#!/usr/bin/env python") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 15932
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env python", sizeof("#! /usr/bin/env python") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 17114
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/ruby", sizeof("#! /usr/bin/ruby") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 17116
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/ruby", sizeof("#! /usr/local/bin/ruby") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 17118
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env ruby", sizeof("#!/usr/bin/env ruby") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 17120
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env ruby", sizeof("#! /usr/bin/env ruby") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-ruby";
        return Match;
    }

    // line 17596
    off0 = 0;
    rslt = stringSearch(buf, len, "<?xml", sizeof("<?xml") - 1, &off0, 1, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/xml";
        return Match;
    }

    // line 17614
    off0 = 0;
    rslt = stringSearch(buf, len, "<?xml", sizeof("<?xml") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/xml";
        return Match;
    }

    // line 17617
    off0 = 0;
    rslt = stringSearch(buf, len, "<?XML", sizeof("<?XML") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "application/xml";
        return Match;
    }

    // line 18779
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/tcl", sizeof("#! /usr/bin/tcl") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18781
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/tcl", sizeof("#! /usr/local/bin/tcl") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18783
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env tcl", sizeof("#!/usr/bin/env tcl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18785
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env tcl", sizeof("#! /usr/bin/env tcl") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18787
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/wish", sizeof("#! /usr/bin/wish") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18789
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/local/bin/wish", sizeof("#! /usr/local/bin/wish") - 1, &off0, 1, 0|IgnoreWS);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18791
    off0 = 0;
    rslt = stringSearch(buf, len, "#!/usr/bin/env wish", sizeof("#!/usr/bin/env wish") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18793
    off0 = 0;
    rslt = stringSearch(buf, len, "#! /usr/bin/env wish", sizeof("#! /usr/bin/env wish") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tcl";
        return Match;
    }

    // line 18851
    off0 = 0;
    rslt = stringSearch(buf, len, "\\input texinfo", sizeof("\\input texinfo") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-texinfo";
        return Match;
    }

    // line 18853
    off0 = 0;
    rslt = stringSearch(buf, len, "This is Info file", sizeof("This is Info file") - 1, &off0, 1, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-info";
        return Match;
    }

    // line 15937
    off0 = 0;
    rslt = regexMatch(buf, len, "^from\\s+(\\w|\\.)+\\s+import.*$", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-python";
        return Match;
    }

    // line 15964
    off0 = 0;
    rslt = regexMatch(buf, len, "^( |\\t){0,50}def {1,50}[a-zA-Z]{1,100}", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15965
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, " {0,50}\\(([a-zA-Z]|,| ){1,255}\\):$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
    }

    // line 17126
    off0 = 0;
    rslt = regexMatch(buf, len, "^[ \t]*require[ \t]'[A-Za-z_/]+'", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17127
        off1 = 0;
        rslt = regexMatch(buf, len, "include [A-Z]|def [a-z]| do$", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17128
            off2 = 0;
            rslt = regexMatch(buf, len, "^[ \t]*end([ \t]*[;#].*)?$", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/x-ruby";
                return Match;
            }
        }
    }

    // line 17130
    off0 = 0;
    rslt = regexMatch(buf, len, "^[ \t]*(class|module)[ \t][A-Z]", &off0, 0, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 17131
        off1 = 0;
        rslt = regexMatch(buf, len, "(modul|includ)e [A-Z]|def [a-z]", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 17132
            off2 = 0;
            rslt = regexMatch(buf, len, "^[ \t]*end([ \t]*[;#].*)?$", &off2, 0, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/x-ruby";
                return Match;
            }
        }
    }

    // line 20064
    off0 = 0;
    rslt = regexMatch(buf, len, "\\`(\r\n|;|[[]|" "\xff" "\xfe" ")", &off0, 0, 0|RegexBegin);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 20066
        off1 = 0;
        off1 += off0;
        rslt = stringSearch(buf, len, "[", sizeof("[") - 1, &off1, 8192, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            // line 20114
            off2 = 0;
            off2 += off1;
            rslt = beQuadMatch(buf, len, 0x0056004500520053, CompareEq, 0xFFdfFFdfFFdfFFdf, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 20116
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
            // line 20119
            off2 = 0;
            off2 += off1;
            rslt = beQuadMatch(buf, len, 0x0053005400520049, CompareEq, 0xFFdfFFdfFFdfFFdf, &off2);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 20121
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
            // line 20125
            off3 = 0;
            off3 += off2;
            rslt = stringSearch(buf, len, "[", sizeof("[") - 1, &off3, 8192, 0);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 20130
                off4 = 0;
                off4 += off3;
                rslt = beQuadMatch(buf, len, 0x0056004500520053, CompareEq, 0xFFdfFFdfFFdfFFdf, &off4);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    // line 20132
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
                // line 20127
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
            // line 20069
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(autorun)]\r\n", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                // line 20070
                off3 = 0;
                off3 += off2;
                rslt = byteMatch(buf, len, 0x5b, CompareEq, 0xffffffff, &off3);
                if (rslt < 0) haveError = True;
                if (rslt > 0)
                {
                    *mime = "application/x-wine-extension-ini";
                    return Match;
                }
                // line 20074
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
            // line 20078
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(version|strings)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-setupscript";
                return Match;
            }
            // line 20082
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(WinsockCRCList|OEMCPL)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "text/inf";
                return Match;
            }
            // line 20087
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(.ShellClassInfo|DeleteOnCopy|LocalizedFileNames)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20091
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(don't load)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20093
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(ndishlp\\$|protman\\$|NETBEUI\\$)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20097
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(windows|Compatibility|embedding)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20100
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(boot|386enh|drivers)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20103
            off2 = 0;
            off2 += off1;
            rslt = regexMatch(buf, len, "^(SafeList)]", &off2, 0, 0|RegexNoCase);
            if (rslt < 0) haveError = True;
            if (rslt > 0)
            {
                *mime = "application/x-wine-extension-ini";
                return Match;
            }
            // line 20106
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

    // line 15941
    off0 = 0;
    rslt = stringSearch(buf, len, "def __init__", sizeof("def __init__") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15942
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

    // line 15957
    off0 = 0;
    rslt = stringSearch(buf, len, "try:", sizeof("try:") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        // line 15958
        off1 = 0;
        off1 += off0;
        rslt = regexMatch(buf, len, "^\\s*except.*:", &off1, 0, 0);
        if (rslt < 0) haveError = True;
        if (rslt > 0)
        {
            *mime = "text/x-python";
            return Match;
        }
        // line 15960
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

    // line 17569
    off0 = 0;
    rslt = stringSearch(buf, len, "<!doctype html", sizeof("<!doctype html") - 1, &off0, 4096, 0|CompactWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17572
    off0 = 0;
    rslt = stringSearch(buf, len, "<head", sizeof("<head") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17575
    off0 = 0;
    rslt = stringSearch(buf, len, "<title", sizeof("<title") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17578
    off0 = 0;
    rslt = stringSearch(buf, len, "<html", sizeof("<html") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17581
    off0 = 0;
    rslt = stringSearch(buf, len, "<script", sizeof("<script") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17584
    off0 = 0;
    rslt = stringSearch(buf, len, "<style", sizeof("<style") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17587
    off0 = 0;
    rslt = stringSearch(buf, len, "<table", sizeof("<table") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 17590
    off0 = 0;
    rslt = stringSearch(buf, len, "<a href=", sizeof("<a href=") - 1, &off0, 4096, 0|IgnoreWS|MatchLower);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/html";
        return Match;
    }

    // line 18857
    off0 = 0;
    rslt = stringSearch(buf, len, "\\input", sizeof("\\input") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18860
    off0 = 0;
    rslt = stringSearch(buf, len, "\\begin", sizeof("\\begin") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18863
    off0 = 0;
    rslt = stringSearch(buf, len, "\\section", sizeof("\\section") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18866
    off0 = 0;
    rslt = stringSearch(buf, len, "\\setlength", sizeof("\\setlength") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18869
    off0 = 0;
    rslt = stringSearch(buf, len, "\\documentstyle", sizeof("\\documentstyle") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18872
    off0 = 0;
    rslt = stringSearch(buf, len, "\\chapter", sizeof("\\chapter") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18875
    off0 = 0;
    rslt = stringSearch(buf, len, "\\documentclass", sizeof("\\documentclass") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18878
    off0 = 0;
    rslt = stringSearch(buf, len, "\\relax", sizeof("\\relax") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18881
    off0 = 0;
    rslt = stringSearch(buf, len, "\\contentsline", sizeof("\\contentsline") - 1, &off0, 4096, 0);
    if (rslt < 0) haveError = True;
    if (rslt > 0)
    {
        *mime = "text/x-tex";
        return Match;
    }

    // line 18884
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
