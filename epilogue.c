
static inline size_t
utf8ByteLen(const Byte* bytes)
{
    if (bytes)
    {
        Byte b = bytes[0];

        if ((b & 0x80) == 0)    return 1;
        if ((b & 0xe0) == 0xc0) return 2;
        if ((b & 0xf0) == 0xe0) return 3;
        if ((b & 0xf8) == 0xf0) return 4;
        if ((b & 0xfc) == 0xf8) return 5;       // a little too general perhaps
        return 6;
    }
    
    return 0;
}



static Result
tryPlainText(const Byte* buf, size_t len, const char** mime)
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

    if (len > Limit)
    {
        bend = bp + Limit;
    }

    for (; bp < bend; ++bp)
    {
        if (*bp >= 128)
        {
            ascii = False;
            break;
        }
    }

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

    for (bp = buf; bp < uend; )
    {
        size_t n = utf8ByteLen(buf);

        if (n == 0)
        {
            utf8 = False;
            break;
        }

        bp += n;
    }

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
            r = tryPlainText(buf, len, mime);
        }
    }

    return r;
}
