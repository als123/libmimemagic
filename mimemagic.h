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

#ifndef MIME_MAGIC_HH
#define MIME_MAGIC_HH

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

//======================================================================

enum MimeMagicFlags
{
    MimeMagicNone      = 0,

    /* Don't try to recognise text/plain with ASCII or Unicode character sets.
    */
    MimeMagicNoTryText = 1 << 0,
};


/*  If the match succeeded then a number greater than 0 is returned and
    *mime is set to a string value. The memory in *mime must be freed
    by the caller with free().

    If no match was found then 0 is returned.

    If no match was found and it appears that a larger chunk of data
    might help then -1 is returned.

*/

extern int
getMimeType(
    const unsigned char* buf,
    size_t          len,
    const char**    mime,
    int             flags
    );

//======================================================================

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif // MIME_MAGIC_HH
