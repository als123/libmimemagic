

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
