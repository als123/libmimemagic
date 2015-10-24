
# Mime Magic

The venerable UNIX `file` command recognises a great many kinds of
files according to their content.  It can print out a description of
the content or print a MIME type.  The details of each file format are
described in a *magic* file which is typically found in `/etc/magic`.

More recently the machinery that recognises the content has been separated
out into a library called `libmagic`.  You can use this library in your
programs to detect the MIME type of a file or data buffer.

The `mimemagic` library does a similar job but is limited to only
detecting the MIME type for the data in a memory buffer.  The decision
machinery is derived from the same magic data as the `libmagic` library.

The aim is to provide something small and fast to be included in a web server.
It is oriented to identifying files for this purpose.

The `mimemagic` API is a single C function.  It does not load any data
file at run time.  It is fast and state-less and thread safe since the
decision process has been compiled from the magic file.

# Limitations

*   Some MIME types have been excluded on the grounds that they are
    obsolete or obscure or expensive and not particularly relevant to
    a web server. This also helps to speed up the execution as some
    types require searching for regular expressions over large chunks of
    the text. The MIME types that have been excluded are listed in the
    `mime.exceptions` file.

*   The handling of some Microsoft files such as Composite Documents
    is not currently implemented.

*   The only text character encodings that are looked for are ASCII
    and UTF-8.

*   The first MIME type found is returned. The weighing of different
    strengths carried out by `libmagic` is not implemented.

*   The 'use' mechanism in the magic file is not currently implemented.
    This misses some file types such as ELF and pcap and file partitions.

# Notes on Usage 

There is a pkgconfig file named `libmimemagic`

There is a shared library and an archive (`.a`) library.  The archive
library can be linked in to an executable and have no external
dependencies. It has been compiled with `-fpic` so it can be linked into
another shared library.

