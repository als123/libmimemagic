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
            self