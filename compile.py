#!/usr/bin/env python

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

import sys;
import re;

from generate import Generate;

OptDebug = True

#======================================================================

# This pattern doesn't match a \n at the end of a line
endCmntRE = re.compile(r"\s*#.*")
allCmntRE = re.compile(r"^\s*#.*")

# This splits out the leading > characters.
levelRE   = re.compile(r"^\s*([>]+)(.*)$")

# Offsets can be complex: (( x [.[bislBISL]][+-][ y ])
offsetRE  = re.compile(r"^(\&)?([^.]+)(\.([bislBISL]))?([+*/%&|^-].+)?$")

strengthRE = re.compile(r"^([+*/-])\s*(\w+)$")

def stripEndCmnt(line):
    # Strip comments from the end of a line
    return endCmntRE.sub("", line, count = 1)

def stripAllCmnt(line):
    # Strip a comment that is an entire line
    return allCmntRE.sub("", line, count = 1)


def mklnum(lnum):
    s = "%d:" % lnum
    if len(s) < 8:
        s += ' ' * (8 - len(s))
    return s


class Fields:
    def __init__(self):
        self.levels = ""
        self.offset = ""
        self.test   = ""
        self.target = ""
        self.msg    = ""

    def __str__(self):
        return "'%s'  '%s'  '%s'  '%s'  '%s'" % (self.levels, self.offset, self.test, self.target, self.msg)




# There may be two or four fields. The third field
# may have quoted white space and C escapes.  We keep in
# all other quoting as we want to recognise '\<?xml\ version\ "'
# as a target that doesn't start with a less-than operator.
#
# There may be white space between the leading > and offset.
# We return the leading > as a separate field.
def splitLine(line):
    Level  = 1
    Offset = 2
    Test   = 3
    Target = 4
    Msg    = 5

    expecting = Level

    fields = Fields()

    # Split out the level characters.
    m = levelRE.match(line)
    if m != None:
        fields.levels = m.group(1)
        line = m.group(2)

    # The offset is delimited by WS.
    # This will ignore WS before the WS.
    [offset, line] = line.split(None, 1)

    # The test is delimited by WS.
    parts = line.split(None, 1)
    test  = parts[0]

    if len(parts) > 1:
        line = parts[1]
    else:
        line = ""

    fields.offset = offset
    fields.test   = test;

    # We need special parsing for the target so that WS can be quoted.
    inRest   = False
    inQuote  = False

    for c in line:
        if inRest:
            fields.msg += c
        elif inQuote:
            if c != ' ':
                fields.target += '\\'
            fields.target += c
            inQuote = False
        elif c == '\\':
            inQuote = True
        elif c == ' ' or c == '\t':
            inRest = True
        else:
            fields.target += c

    fields.msg = fields.msg.strip()

    #if OptDebug: print fields
    return fields


# We expect text lie
#    # Screamtracker 2 module sound data
#    audio/x-mod

def readExceptions(name):
    try:
        f = open(name)
        exceptions = set()

        # Discard the comments and blank lines.
        for line in f.readlines():
            (line, _, _) = line.partition('#')
            line = line.strip()
            if len(line) > 0:
                exceptions.add(line)

        f.close()
        return exceptions

    except IOError,exn:
        print >> sys.stderr, "Exception ", exn

    finally:
        f.close()



# This keeps track of the nesting of tests.
class Stack:

    def __init__(self, root):
        # The stack is never empty
        self.stack = [root]


    def parentFor(self, level):
        # A new test is coming with the given level. 
        # Adjust the stack and return the test that is its parent
        # Postcondition: The top of stack has a level less than the new one.

        while self.stack[-1].level >= level:
            del self.stack[-1]
        
        return self.stack[-1]


    def newTest(self, test):
        # Add the new test.
        if test.level > self.stack[-1].level:
            self.stack.append(test)


#======================================================================

# This is a node in the test tree.  It represents one test that can be
# made.  A list of tests are treated as OR-ed together.  A test can
# contain subtests that will be run if the parent test succeeds. 

# The root of the tree is a node with testCode = 'root' and a level of
# -1 and parent = None.

# A test can contain an action.  The only action is to return a MIME
# type.  All actions are at leaves of the test tree.  Any test that
# doesn't lead to an action is pruned.

class Test:
    maskedRE = re.compile(r"^(\w+)\&([0-9a-fxA-FX]+)")
    regexRE  = re.compile(r"^(\d+)?([/csl]*)$")

    # The numeric tests we recognise
    integerTests = {
        'byte', 
        'short', 'leshort', 'beshort',
        'long',  'lelong',  'belong',
        'quad',  'lequad',  'bequad'
        }

    floatTests = {'float', 'befloat', 'lefloat', 'double', 'bedouble', 'ledouble'}

    otherTests = {'default', 'clear', 'regex'}


    def __init__(self, lnum, level, offset, testCode, testArg, parent = None):
        self.lnum     = lnum
        self.level    = level
        self.parent   = parent
        self.offset   = offset

        self.subtests = []
        self.active   = False           # will be set true if this test leads to an action
        self.setMime  = None
        self.mimex    = False           # set if it is a /x- Mime subtype
        self.strength = 20              # The file program does this REVISIT not used

        # These are the outputs of parseTest
        self.unsigned   = False
        self.testCode   = None
        self.testFlags  = []      # without the / and sorted to give a predictable order
        self.testMask   = None    # or else a C number as a string
        self.testLimit  = None    # a numeric limit e.g. /4096
        self.target     = None
        self.targetOper = ""      # operator on the test argument
        self.testID     = None    # operator on the test argument
        self.priority   = 0
        self.invalid    = False
        self.unimplemented = False

        if self.parent:
            self.parseTest(testCode, testArg)

        if parent != None:
            parent.subtests.append(self)
            self.strength = parent.strength


    def setStrength(self, oper, value):
        v = int(value)

        if oper == '+':
            self.strength += v
        elif oper == '-':
            self.strength -= v
        elif oper == '*':
            self.strength *= v
        elif oper == '/':
            self.strength /= v


    def setAction(self, mime):
        #print self.lnum, "setting mime", mime
        self.setMime = mime
        self.mimex = mime.find('/x-') >= 0


    def parseTest(self, test, target):
        self.target = target

        if test[0] == 'u':
            self.unsigned = True
            test = test[1:]

        # See if there is a mask
        m = self.maskedRE.match(test)
        if m != None:
            self.testMask = m.group(2)
            test = m.group(1)
            #print "mask is", self.testMask

        # Some tests have flags indicated by a /. The search test
        # has a numeric flag. The rest are single characters.

        # The string and search flags have slash separators.
        # The regex test has the one slash.

        # REVISIT we ignore the 'b' and 't' flags for distinguishing between
        # binary data and text.
        (test, sep, after) = test.partition('/')

        if sep and after:
            if test == 'regex':
                # e.g. regex/100l or regex/4096 or regex/c
                m = self.regexRE.match(after)
                if m != None:
                    g1 = m.group(1)
                    g2 = m.group(2)
                    if g1 != None and g1:
                        self.testLimit = g1
                    if 'c' in g2: self.testFlags.append('c')
                    if 's' in g2: self.testFlags.append('s')
                    if 'l' in g2: self.testFlags.append('l')
            else:
                for f in after.replace('/', ' ').split():
                    if f[0].isdigit():
                        self.testLimit = f
                    else:
                        # Ensure they are split apart
                        self.testFlags.extend(list(f))

                if 'b' in self.testFlags: self.testFlags.remove('b')
                if 't' in self.testFlags: self.testFlags.remove('t')

            self.testFlags.sort()

        self.testCode = test

        # See what target operators are present.
        # Beware: the target '=!<arch>\ndebian' has flags '=' and '!' but not '<'
        # It appears that we can get away with assuming '!' is always last and
        # 'x' appears by itself so we never have !x

        if self.target == 'x':
            self.targetOper = self.target
        else:
            self.targetOper = '='

            if self.testCode == 'string' or self.testCode == 'search':
                if self.target and self.target[0] in '=<>':
                    self.targetOper = self.target[0]
                    self.target = self.target[1:]

                if self.target and self.target[0] == '!':
                    self.targetOper += self.target[0]
                    self.target = self.target[1:]

            elif self.testCode in self.integerTests:
                # Integer flags. There may be more than one.
                if self.target and self.target[0] in '=<>&^~':
                    self.targetOper = self.target[0]
                    self.target = self.target[1:]

                if self.target and self.target[0] == '!':
                    self.targetOper += self.target[0]
                    self.target = self.target[1:]

            elif self.testCode in self.floatTests:
                # Float flags
                if self.target and self.target[0] in '=<>':
                    self.targetOper = self.target[0]
                    self.target = self.target[1:]

                if self.target and self.target[0] == '!':
                    self.targetOper += self.target[0]
                    self.target = self.target[1:]

            elif self.testCode in self.otherTests:
                self.unimplemented = True
            else:
                self.invalid = True

        # Set the byte order on some of these
        prefix = 'le' if sys.byteorder == 'little' else 'be'
        if self.testCode in ['short', 'long', 'quad']:
            self.testCode = prefix + self.testCode

        #print >> sys.stderr, "%d: Warning: %s is not yet implemented" %(self.lnum, self.testCode)
        #print >> sys.stderr, "%d: Warning: unrecognised test %s" %(self.lnum, self.testCode)

        # In addition construct a descriptor that distinguishes
        # different tests at run-time.
        self.testID = ""

        if self.unsigned:
            self.testID += 'u'

        self.testID += self.testCode

        for f in self.testFlags:
            self.testID += '/' + f

        if self.testMask:
            self.testID += '&' + self.testMask

        if self.targetOper:
            self.testID += ' ' + self.targetOper

        self.testID += ' ' + str(self.offset)

        self.setPriority()


    def check(self):
        # Check for validity. This is only done after we've pruned
        if self.offset:                 # the root doesn't have one
            self.offset.check()

        for t in self.subtests:
            t.check()


    def numSubtests(self):
        return len(self.subtests)


    def depth(self):
        # Return the number of test levels below this one.
        # At the root it is the height of the test tree.
        d = 0
        for t in self.subtests:
            d = max(d, t.depth())
        return d + 1


    def pruneTree(self, exceptions):
        # If any subtest has a MIME in the exception list
        # then remove it.  Recurse down the tree.
        # Set the active flag on every test that 
        newsubs = []

        for t in self.subtests:
            if t.setMime and t.setMime not in exceptions:
                t.active = True
                newsubs.append(t)

                # This might make self active which will save us some work.
                t.pruneTree(exceptions)

                if not self.active:
                    self.active = True
                    #print "setting active", self

                    # Run up to the root setting the active flag
                    p = self.parent
                    while p:
                        p.active = True
                        p = p.parent
            else:
                t.pruneTree(exceptions)
                if t.active:
                    newsubs.append(t)

        self.subtests = newsubs


    def setPriority(self):
        # Lower numbers are higher priority. 
        if self.testCode in self.integerTests:
            self.priority = 0

        elif self.testCode == 'string':
            self.priority = 5

        elif self.testCode == 'search':
            if self.testLimit != None and int(self.testLimit, 0) > 5:
                self.priority = 90
            else:
                self.priority = 20

        elif self.testCode == 'regex':  # These are very slow
            self.priority = 80

        else:
            self.priority = 10



    def partitions(self):
        # Divide the subtests into partitions with the same test ID.
        # Return a list of lists.
        # REVISIT no longer used.
        parts = []
        tests = []
        for t in self.subtests:
            if tests:
                if t.testID == tests[0].testID:
                    tests.append(t)
                else:
                    parts.append(tests)
                    tests = [t]
            else:
                tests = [t]

        return parts


    def printTree(self, level = 0):
        if self.parent:
            if level == 0:
                indent = '> '
            else:
                indent = '  ' + ('    ' * level)
            print "%s%s%s" % (mklnum(self.lnum), indent, self)
        else:
            # Fudget it for the root node
            level = -1

        for t in self.subtests:
            t.printTree(level + 1)


    def __str__(self):
        if self.parent:
            #active = '*' if self.active else ' '
            active = ''
            action = self.setMime if self.setMime else ""
            return "test%s %d %s '%s' %s" % (active, self.priority, self.testID, self.target, action)
        return "root"


# Offsets can be complex:
#   (( x [.[bislBISL]][+-][ y ])

# It appears that the operator can be one of +-*/%&|^ and that the
# intention is that it be a legal C integer operator. We use it directly.

#   >0x18       leshort <0x40   MZ executable (MS-DOS)
#   >>(0x3c.l)  string  PE\0\0  PE executable (MS-Windows)
#   >>(4.s*512)   leshort !0x014c MZ executable (MS-DOS)
#   >>>&(2.s-514) string  LE      LE executable (MS Windows VxD driver)
#   >>>&0       leshort 0x14c     for Intel 80386
#   >>>(&0x7c.l+0x26) string  UPX     \b, UPX compressed
#   >>>&(&0x54.l-3)  string  UNACE  \b, ACE self-extracting archive
#   >>(&0x10.l+(-4))

# The & means make it relative to the offset from the previous test.
# To describe all of these possibilities the offset is turned into
# a sequence of operations.


# The (-4) is yet another form but it is not found in the magic we
# look at.  Report it as unimplemente.  It might mean that the operand
# is an instance of the whole indirect syntax but in this case has
# omitted everything but the offset.

# REVISIT it appears that a previous search and string and in facty
# any test can update the file offset for later relative offsets. 

class Offset:
    def __init__(self, field):
        self.offset    = None
        self.simple    = False   # nothing more than a number
        self.noOffset  = False   # just a simple offset of 0
        self.indirect  = False
        self.typeFlag  = None    # for indirect offsets
        self.operator  = None    # for indirect offsets
        self.operand   = None    # for indirect offsets

        self.innerRelative = False
        self.outerRelative = False

        if field[0] == '&':
            self.outerRelative = True
            field = field[1:]

        if field and field[0] == '(' and field[-1] == ')':
            self.indirect = True
            #print >> sys.stderr, "found indirect", field

            # Strip off the outer () so that the () around the operator are parsable
            field = field[1:-1]

            m = offsetRE.match(field)
            if m != None:
                #print "offset parse of", field, m.group(1), m.group(2)
                self.innerRelative = m.group(1) != None and m.group(1) != ''
                self.offset = m.group(2)

                if m.group(4):
                    # There is an operator
                    self.typeFlag = m.group(4)
                else:
                    self.typeFlag = 'l'

                s = m.group(5)
                if s:
                    self.operator = s[0]
                    self.operand  = s[1:]
        else:
            self.offset = field

        if not self.indirect and not self.outerRelative:
            self.simple = True

        if self.simple and self.offset == '0':
            self.noOffset = True


    def check(self):
        # Check for validity. This is only done after we've pruned
        if self.offset == None:
            print >> sys.stderr, "Offset syntax error in:", field

        if self.typeFlag and self.typeFlag in 'iIm':
            print >> sys.stderr, "Unimplemented offset type flag:", field

        if self.operand and '(' in self.operand:
            print >> sys.stderr, "Unimplemented indirect operand in offset:", field


    def __str__(self):
        orel = ('& ' if self.outerRelative else '')
        irel = ('& ' if self.innerRelative else '')
        s = "[offset %s" % orel

        if self.indirect:
            s += "(%s%s %s" % (irel, self.offset, self.typeFlag)

            if self.operator:
                s += " %s %s" % (self.operator, self.operand)
            s += ')'
        else:
            s += self.offset

        s += ']'
        return s


#======================================================================


def parseFields(fields, lnum, lastTest, stack):
    # Some fields might just add extra information to the last test.

    result    = None
    level     = 0
    offset    = None
    test      = None
    name      = None
    use       = None

    if fields.offset == '!:mime':
        mime = fields.test
        #print lnum, "Recognised mime", mime
        if lastTest:
            lastTest.setAction(mime)

    elif fields.offset == '!:strength':
        # There may not be WS between the operator and operand
        m = strengthRE.match(fields.test + fields.target)
        if m != None:
            strenOper = m.group(1)
            strenVal  = m.group(2)
            #print lnum, "Recognised strength", strenOper, strenVal
            if lastTest:
                lastTest.setStrength(strenOper, strenVal)

    elif fields.offset == '!:apple':
        pass

    elif fields.test == 'name':
        name = fields.target
        #print lnum, "Recognised name", name

    elif fields.test == 'name':
        use = fields.target
        #print lnum, "Recognised use", use

    else:
        level  = len(fields.levels)
        offset = Offset(fields.offset)
        parent = stack.parentFor(level)
        result = Test(lnum, level, offset, fields.test, fields.target, parent)
        stack.newTest(result)
        #print lnum, "Recognised", level, offset, test, fields.msg

    return result


def readFile(name, exceptions):

    root  = Test(-1, -1, None, 'root', '')
    stack = Stack(root)

    try:
        f = open(name)
        lines = f.readlines()
        lnum  = 0
        last  = None

        for line in lines:
            lnum += 1

            # Discard the comments and blank lines. A comment
            # must have leading white space. Be careful that test strings
            # can have '#' characters.
            line = stripAllCmnt(line)
            line = line.strip()
            if len(line) == 0:
                continue

            newtest = parseFields(splitLine(line), lnum, last, stack)

            if newtest:
                last = newtest

        f.close()

    except IOError,exn:
        print >> sys.stderr, "Exception ", exn

    finally:
        f.close()

    return root



def Main():
    (major, minor, _, _, _) = sys.version_info

    if not(major == 2 and minor >= 7):
        print >> sys.stderr, "This needs at least version 2.7 of python"
        sys.exit(1)

    exceptions = readExceptions("mime.exceptions")
    root = readFile("magic", exceptions)

    root.pruneTree(exceptions)
    root.check()

    if OptDebug:
        root.printTree()

    gen = Generate()
    gen.putRoot(root)
    gen.writeToFile("mimemagic.c")

Main()
