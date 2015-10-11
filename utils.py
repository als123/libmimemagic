
import string

#======================================================================

class OStream:
    def __init__(self):
        self.parts = []


    def write(self, part):
        self.parts.append(part)

    def __str__(self):
        return ''.join(self.parts)



def copyFile(path, toFile):
    # copy the file to the output
    f = open(path, "r")
    for l in f.readlines():
        toFile.write(l)
    f.close()

#======================================================================

IndentStep = '    '

def mkIndent(level):
    return IndentStep * level


def addIndent(lines, levels):
    extra = mkIndent(levels)
    strm  = OStream()
    for l in lines.splitlines(True):
        strm.write(extra + l)
    return str(strm)

#======================================================================

cEscapes = {
    'a'  : '\a',
    'b'  : '\b',
    'f'  : '\f',
    'n'  : '\n',
    'r'  : '\r',
    't'  : '\t',
    'v'  : '\v',
    '\\' : '\\',
    }

cUnescapes = {
    '\a'  : 'a',
    '\b'  : 'b',
    '\f'  : 'f',
    '\n'  : 'n',
    '\r'  : 'r',
    '\t'  : 't',
    '\v'  : 'v',
    '\\'  : '\\',
    '"'   : '"',
    }



def cFormat(charlist):
    # Convert a list of chars back to a properly quoted C string without "" delimiters.
    s = ""
    for c in charlist:
        if c >= ' ':
            if c == '"':
                s += '\\"'
            else:
                s += c
        else:
            for (k, v) in cEscapes.items():
                if c == v:
                    s += '\\' + k
                    c = None
            if c != None:
                s += '\\x%02x' % ord(c)

    return s



def splitStringBytes(input):
    # Interpret the escape sequences in a string to return a
    # list of integer bytes, one for each character.

    # The magic file strings have \xFF hex escapes where there are one
    # or two hex characters.  This is not valid C.  Recent C allows
    # more than two so "\xabcd" is treated as a single 32 bit code. 
    # We must reinterpret the escape sequences.  We convert this
    # example to two string literals "\xab" "cd"

    bytes = [] 

    l  = len(input)
    ix = 0

    while ix < l:
        c = input[ix]

        if c == '\\':
            if ix + 1 < l:
                c2 = input[ix + 1]

                if c2 in cEscapes:
                    bytes.append(ord(cEscapes[c2]))
                    ix += 2

                elif c2 == 'x':
                    # Grab up to two hex digits.
                    ix += 2
                    n = 0
                    v = ""
                    while n < 2 and ix < l and input[ix] in string.hexdigits:
                        v += input[ix]
                        ix += 1
                        n  += 1
                    bytes.append(int(v, 16))

                elif c2 in '01234567':
                    # an octal escape, digits may be missing e.g. \0 for NUL
                    v = c2
                    ix += 2
                    while ix < l and input[ix] in '01234567':
                        v += input[ix]
                        ix += 1
                    bytes.append(int(v, 8))

                else:
                    # Copy the character literally
                    bytes.append(ord(c2))
                    ix += 2

            else:
                # no character follows the backslash. Treat it as a literal backslash
                bytes.append(ord('\\'))

        else:
            bytes.append(ord(c))
            ix += 1

    #print "splitStringBytes", input, bytes
    return bytes



def bytesToC(bytes):
    # Convert the bytes from splitStringBytes() back into
    # valid C code.  Anything we quote as hex must be in a
    # separate literal in case the following characters look
    # like hex.

    parts = []
    part  = ""

    for b in bytes:
        c = chr(b)
        if c in cUnescapes:
            part += '\\' + cUnescapes[c]

        elif b >= 32 and b < 128:
            part += c

        else:
            # Flush the part and put a hex insert
            if part:
                parts.append(part)
            parts.append("\\x%02x" % b)
            part = ""

    if part:
        parts.append(part)

    parts = ['"' + p + '"' for p in parts if p]
    return " ".join(parts)


def quoteForC(text):
    bs = splitStringBytes(text)
    return bytesToC(bs)

#======================================================================


def filterList(items, func):
    yes = []
    no  = []

    for i in items:
        if func(i):
            yes.append(i)
        else:
            no.append(i)

    return (yes, no)



def partitionByKey(items, keyFunc):
    # This applies keyFunc to each item and returns a dict
    # mapping each key to a list of items.
    result = {}

    for i in items:
        key = keyFunc(i)

        if key in result:
            result[key].append(i)
        else:
            result[key] = [i]

    return result
