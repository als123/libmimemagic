
"""
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
"""

import sys

import utils
from utils import mkIndent
from utils import OStream

PrologueFile = "prologue.c"
EpilogueFile = "epilogue.c"

#   This is a module for compile.py. It generates the C code for the
#   decision tree.

RuntimeDebug = False

def mkOvar(level):
    return "off%d" % level

#======================================================================

class Generate:
    stringFuncs = {
        '=':    'stringEqual',
        '=!':   '!stringEqual',
        '<':    'stringLess',
        '<!':   '!stringLess',
        '>':    'stringGreater',
        '>!':   '!stringGreater'
        }

    # For the unsigned version, add a 'u' prefix.
    simpleIntFuncs = {
        'byte':     'byteMatch',

        'short':    'shortMatch',
        'long':     'longMatch',
        'quad':     'quadMatch',

        'beshort':  'beShortMatch',
        'belong':   'beLongMatch',
        'bequad':   'beQuadMatch',

        'leshort':  'leShortMatch',
        'lelong':   'leLongMatch',
        'lequad':   'leQuadMatch'
        }

    compareCodes = {
        '=':    'CompareEq',
        '<':    'CompareLt',
        '>':    'CompareGt',
        '&':    'CompareSet',
        '^':    'CompareClr',
        '~':    'CompareNeg',
        '!':    'CompareNot'
        }


    def __init__(self):
        # This may throw
        self.code  = OStream()
        self.data  = OStream()
        self.decls = OStream()

        self.mapCount = 1;
         


    def putRoot(self, root):
        self.putTests(root.subtests, 1)

        # We need an 'offN' variable for each level of nesting.
        #print 'root depth', root.depth()
        decl = []
        for n in range(root.depth()):
            decl.append('off%d' % n)
        print >> self.decls, "%ssize_t %s;" % (mkIndent(1), ', '.join(decl))



    def putTests(self, tests, level):
        # Partition them by priority
        parts = utils.partitionByKey(tests, lambda t: t.priority)
        prios = parts.keys()
        prios.sort()

        for p in prios:
            self.putTests2(parts[p], level)


    def putTests2(self, tests, level):
        (beshort, rest1) = self.selectSimpleBeShortTests(tests)

        if beshort:
            self.putBeShortGroup(beshort, level)

        (strEquals, strNotEquals, strOtherOper, rest2) = self.selectSimpleStringTests(rest1)

        # Select out those that do nothing but set a MIME after testing at offset 0
        (strMime, strEquals2) = self.selectSimpleStringMime(strEquals)

        # These tests can be done in one map
        if strMime:
            self.putStringMap(strMime, level)

        for t in strEquals2:
            self.putSimpleString(t, level)

        for t in strNotEquals:
            self.putSimpleString(t, level)

        for t in strOtherOper:
            self.putSimpleString(t, level)

        for t in rest2:
            self.putGeneralTest(t, level)


    def putSimpleString(self, test, level):
        indent = mkIndent(level)

        if test.targetOper in self.stringFuncs:
            func = self.stringFuncs[test.targetOper]

            # Get a string literal which we can use sizeof on.
            targ  = utils.quoteForC(test.target)
            ovar  = mkOvar(test.level)
            inner = OStream()

            if test.level == 0:
                print >> self.code

            print >> self.code, '%s// line %s' %(indent, test.lnum)
            if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

            print >> inner, '%srslt = %s(buf, len, %s, sizeof(%s) - 1, &%s);' % \
                                        (indent, func, targ, targ, ovar)
            print >> inner, '%sif (rslt < 0) haveError = True;' % indent

            self.genOffset(test, str(inner), level)
            self.putTestBody(test, level)

        elif test.targetOper == 'x':
            # Always match
            self.putTestContent(test, level)

        else:
            print >> sys.stderr, "Unrecognised string operator in:", test



    def putStringMap(self, tests, level):
        # This handles the case of multiple strings mapping to mime names.
        # The operator must be equality.  We have to build a table of
        # test data.  These have all have a zero offset.
        
        indent = mkIndent(level)
        ind1   = mkIndent(1)

        mapName = "stringMap%d" % self.mapCount
        self.mapCount += 1

        # A list of (bytes, targ, mime). These will be sorted
        targets = []

        for t in tests:
            # Get a string literal which we can use sizeof on.
            bytes = utils.splitStringBytes(t.target)
            targ  = utils.bytesToC(bytes)
            mime  = utils.quoteForC(t.setMime)
            triple = (bytes, targ, mime)
            targets.append(triple)

        targets.sort(key = lambda triple: triple[0])

        print >> self.data, "\nstatic StringMap %s[] = {" % mapName
        for (bytes, targ, mime) in targets:
            print >> self.data, '%s{%s,    sizeof(%s) - 1,    %s},' % (ind1, targ, targ, mime)
        print >> self.data, "};"
        print >> self.data, "static const size_t %sCount = %d;" % (mapName, len(targets))

        # Presumably all tests have the same nesting level
        testLevel = tests[0].level
        testLine  = tests[0].lnum

        if testLevel == 0:
            print >> self.code

        print >> self.code, '%s// line %s' %(indent, testLine)
        if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

        print >> self.code, '%srslt = stringEqualMap(buf, len, %s, %sCount, mime);' % (indent, mapName, mapName)
        print >> self.code, '%sif (rslt < 0) haveError = True;' % indent
        print >> self.code, '%sif (rslt > 0)' % indent
        print >> self.code, '%s{' % indent
        print >> self.code, '%sreturn Match;' % mkIndent(level + 1)
        print >> self.code, '%s}' % indent



    def putBeShortGroup(self, tests, level):
        # This handles the case of multiple strings mapping to mime names.
        # The operator must be equality.  We have to build a table of
        # test data.  These have all have a zero offset.
        indent = mkIndent(level)
        ind1   = mkIndent(1)

        mapName = "beshortMap%d" % self.mapCount
        self.mapCount += 1

        # A list of (test, mask, mime).
        targets = []

        for t in tests:
            # Get a string literal which we can use sizeof on.
            mask = '0xffff' if t.testMask == None else t.testMask
            triple = (t.target, mask, utils.quoteForC(t.setMime))
            targets.append(triple)

        print >> self.data, "\nstatic ShortMap %s[] = {" % mapName
        for (targ, mask, mime) in targets:
            print >> self.data, '%s{%s,    %s,    %s},' % (ind1, targ, mask, mime)
        print >> self.data, "};"
        print >> self.data, "static const size_t %sCount = %d;" % (mapName, len(targets))

        # Presumably all tests have the same nesting level
        testLevel = tests[0].level
        testLine  = tests[0].lnum

        if testLevel == 0:
            print >> self.code

        print >> self.code, '%s// line %s' %(indent, testLine)
        if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

        print >> self.code, '%srslt = beShortGroup(buf, len, %s, %sCount, mime);' % (indent, mapName, mapName)
        print >> self.code, '%sif (rslt < 0) haveError = True;' % indent
        print >> self.code, '%sif (rslt > 0)' % indent
        print >> self.code, '%s{' % indent
        print >> self.code, '%sreturn Match;' % mkIndent(level + 1)
        print >> self.code, '%s}' % indent



    def putGeneralTest(self, test, level):
        # Write any other kind of test

        # We don't find double or float in any of the tests we perform.
        # They could be added later if necessary. The date tests don't
        # appear in the current magic database.

        indent  = mkIndent(level)
        ovar    = mkOvar(test.level)

        if test.testCode == 'default' or test.targetOper == 'x':
            # Always match. The 'clear' operation doesn't seem to
            # be relevant to generating a MIME type.
            self.putTestContent(test, level)

        elif test.testCode == 'string':
            # handle combinations of flags and comparisons
            flags = ['0']
            if 'w' in test.testFlags: flags.append('IgnoreWS')
            if 'W' in test.testFlags: flags.append('CompactWS')
            if 'c' in test.testFlags: flags.append('MatchLower')
            if 'C' in test.testFlags: flags.append('MatchUpper')
            flags = '|'.join(flags)

            oper = []
            if '=' in test.targetOper: oper.append('CompareEq')
            if '<' in test.targetOper: oper.append('CompareLt')
            if '>' in test.targetOper: oper.append('CompareGt')
            if '!' in test.targetOper: oper.append('CompareNot')
            oper = '|'.join(oper)

            targ  = utils.quoteForC(test.target)
            inner = OStream()

            if test.level == 0:
                print >> self.code

            print >> self.code, '%s// line %s' %(indent, test.lnum)
            if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

            print >> inner, '%srslt = stringMatch(buf, len, %s, sizeof(%s) - 1, &%s, %s, %s);' % \
                                        (indent, targ, targ, ovar, oper, flags)
            print >> inner, '%sif (rslt < 0) haveError = True;' % indent

            self.genOffset(test, str(inner), level)
            self.putTestBody(test, level)


        elif test.testCode == 'search':
            # handle combinations of flags and comparisons
            flags = ['0']
            if 'w' in test.testFlags: flags.append('IgnoreWS')
            if 'W' in test.testFlags: flags.append('CompactWS')
            if 'c' in test.testFlags: flags.append('MatchLower')
            if 'C' in test.testFlags: flags.append('MatchUpper')
            flags = '|'.join(flags)
            
            # find the range limit
            limit = test.testLimit

            if limit == None:
                print >> sys.stderr, "Missing range limit in", test
            else:
                targ  = utils.quoteForC(test.target)
                inner = OStream()

                if test.level == 0:
                    print >> self.code

                print >> self.code, '%s// line %s' %(indent, test.lnum)
                if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

                print >> inner, '%srslt = stringSearch(buf, len, %s, sizeof(%s) - 1, &%s, %s, %s);' % \
                                            (indent, targ, targ, ovar, limit, flags)
                print >> inner, '%sif (rslt < 0) haveError = True;' % indent

                self.genOffset(test, str(inner), level)
                self.putTestBody(test, level)


        elif test.testCode == 'regex':
            # find the range limit
            limit = test.testLimit

            if limit == None:
                limit = '0'

            # handle combinations of flags and comparisons
            flags = ['0']
            if 'c' in test.testFlags: flags.append('RegexNoCase')
            if 's' in test.testFlags: flags.append('RegexBegin')
            if 'l' in test.testFlags: limit += ' * 80'          # pretend 80 chars per line
            flags = '|'.join(flags)

            targ  = utils.quoteForC(test.target)
            inner = OStream()

            if test.level == 0:
                print >> self.code

            print >> self.code, '%s// line %s' %(indent, test.lnum)
            if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

            print >> inner, '%srslt = regexMatch(buf, len, %s, &%s, %s, %s);' % (indent, targ, ovar, limit, flags)
            print >> inner, '%sif (rslt < 0) haveError = True;' % indent

            self.genOffset(test, str(inner), level)
            self.putTestBody(test, level)

            
        elif test.testCode in self.simpleIntFuncs:
            func    = self.simpleIntFuncs[test.testCode]
            value   = test.target                 # this should be a C integer in string form
            compare = self.genCompareCodes(test)
            mask    = test.testMask if test.testMask else "0xffffffff"
            inner   = OStream()

            if test.level == 0:
                print >> self.code

            print >> self.code, '%s// line %s' %(indent, test.lnum)
            if RuntimeDebug: print >> self.code, "%s++testCount;" % indent

            print >> inner, '%srslt = %s(buf, len, %s, %s, %s, &%s);' % (indent, func, value, compare, mask, ovar)
            print >> inner, '%sif (rslt < 0) haveError = True;' % indent

            self.genOffset(test, str(inner), level)
            self.putTestBody(test, level)


        else:
            print >> sys.stderr, "Unimplemented test", test



    def putTestBody(self, test, level):
        # This is the common structure after each test
        indent = mkIndent(level)

        print >> self.code, '%sif (rslt > 0)' % indent
        print >> self.code, '%s{' % indent
        self.putTestContent(test, level + 1)
        print >> self.code, '%s}' % indent



    def putTestContent(self, test, level):
        # What to do if the test passes
        # The result is the number of bytes that were matched. We
        # must update the offset for inner tests.

        # *** If there are both subtests and a MIME we only set the
        # MIME.  See also selectSimpleStringMime().
        indent = mkIndent(level)

        if test.setMime:
            m = utils.quoteForC(test.setMime)
            print >> self.code, '%s*mime = %s;' % (indent, m)
            print >> self.code, '%sreturn Match;' % indent

        else:
            # No mime at this level, there must be subtests
            self.putTests(test.subtests, level)



    def selectSimpleStringTests(self, tests):
        # Select simple string tests that have no flags but any non-indirect offset.
        # Further separate out the equality tests as they are the most useful to check first.
        # The result is a pair of lists.
        equals     = []
        notEquals  = []
        otherOper  = []
        other      = []

        for t in tests:
            if t.testCode == 'string' and not t.testFlags and t.offset.simple:
                if t.targetOper == '=':
                    equals.append(t)
                elif t.targetOper == '=!':
                    notEquals.append(t)
                else:
                    otherOper.append(t)
            else:
                other.append(t)

        return (equals, notEquals, otherOper, other)



    def selectSimpleStringMime(self, tests):
        # Further filter out the simple string equality tests that have
        # set a mime value. See putTestContent() for how having a MIME
        # means that no subtests will be considered.
        want  = []
        other = []

        for t in tests:
            if t.setMime and t.offset.noOffset:
                want.append(t)
            else:
                other.append(t)

        return (want, other)



    def selectSimpleBeShortTests(self, tests):
        # Select all tests that use beshort at a simple offset 0 and
        # just set a Mime type.  These are fairly common MIME types. 
        # See also selectSimpleStringMime().
        want  = []
        other = []

        for t in tests:
            if t.testCode == 'beshort' and not t.unsigned and \
                    t.targetOper == '=' and t.setMime and t.offset.noOffset:
                want.append(t)
            else:
                print "selectSimpleBeShortTests skipping", t
                other.append(t)

        return (want, other)



    def genCompareCodes(self, test):
        names = []
        for c in test.targetOper:
            if c in self.compareCodes:
                names.append(self.compareCodes[c])

        if not names:
            print >> sys.stderr, "Empty compare codes for", test

        return "|".join(names)



    def genOffset(self, test, innerCode, level):
        # Generate the code for the offset.  With indirection we need
        # something of the form: 
        #   rslt = getOffset(buf, len, off1, 's', &off1);
        #   if (rslt < 0) haveError = True;
        #   else
        #   {
        #       off1 += (30);
        #       rslt = leShortMatch(buf, len, 0xcafe, CompareEq, 0xffffffff, off1);
        #       if (rslt < 0) haveError = True;
        #   }

        # For this we need the rest of the test passed in as a stream.

        indent = mkIndent(level)

        off  = test.offset
        ovar = mkOvar(test.level)

        # Start with a simple offset
        print >> self.code,  "%s%s = %s;" % (indent, ovar, off.offset)

        # It appears that (&0x7c.l+0x26) means *(0x7c + off) + 0x26

        if off.indirect:
            # Go indirect through the buffer

            if off.innerRelative:
                outer = mkOvar(test.level - 1)
                print >> self.code, "%s%s += %s;" % (indent, ovar, outer)

            # getOffset(const Byte* buf, size_t len, size_t at, char type, size_t* offset)
            print >> self.code, "%srslt = getOffset(buf, len, %s, '%s', &%s);" % \
                                        (indent, ovar, off.typeFlag, ovar)

            if off.operand:
                value = off.operand
                print >> self.code, "%s%s %s= %s;" % (indent, ovar, off.operator, off.operand)

        if off.outerRelative:
            # Add the outer offset, for the direct and indirect cases
            print >> self.code,  "%s%s += %s;" % (indent, ovar, mkOvar(test.level - 1))

        if off.indirect:
            # We have a rslt from above to 
            print >> self.code, "%sif (rslt < 0) haveError = True;" % indent
            print >> self.code, "%selse" % indent
            print >> self.code, "%s{" % indent
            innerCode = utils.addIndent(innerCode, 1)
            print >> self.code, innerCode,
            print >> self.code, "%s}" % indent
        else:
            print >> self.code, innerCode,



    def writeToFile(self, path):
        out = open(path, "w")

        utils.copyFile(PrologueFile, out)
        out.write(str(self.data))

        if RuntimeDebug:
            print >> out, "static size_t testCount;"

        print >> out, """
static Result
runTests(const Byte* buf, size_t len, const char** mime)
{
    Result rslt;
    Bool   haveError = False;
""",

        out.write(str(self.decls))
        out.write(str(self.code))

        print >> out, """

    if (haveError)
    {
        // nothing matched, perhaps because of the error
        return Error;
    }
    return Fail;
}

"""
        utils.copyFile(EpilogueFile, out)
        out.close()
