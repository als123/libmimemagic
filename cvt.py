#!/usr/bin/env python

import sys;
import re;

#======================================================================

# There may be two or four fields. The third field
# may have quoted white space. We leave the quoting in.
def splitLine(line):
    fields = line.split(None, 2)

    if len(fields) >= 3:
        test = fields[2]
        deq  = ""
        rest = ""
        inQuote = False
        inRest  = False

        # Stop at non-quoted white space
        for c in test:
            if inRest:
                rest += c;
            elif inQuote:
                deq += '\\'
                deq += c
                inQuote = False
            elif c == '\\':
                inQuote = True
            elif c == ' ' or c == '\t':
                inRest = True
            else:
                deq += c

        fields[2] = deq;
        #print "deq", deq
        #print "rest", rest

        if len(fields) >= 4:
            fields[3] = rest.strip()
        else:
            fields.append(rest.strip())

    return fields



def describeSections(selected):
    # See what the types are
    allTypes = set()
    allTests = set()
    allMsgs  = set()

    for s in selected:
        for e in s.entries:
            t = e.getType()
            if t != None:
                allTypes.add(t)

            t = e.getTest()
            if t != None:
                allTests.add(t)

            t = e.getMsg()
            if t != None:
                allMsgs.add(t)

    print "\n\nAll types"
    typeList = list(allTypes)
    typeList.sort()
    for t in typeList:
        print t

    print "\n\nAll Tests"
    testList = list(allTests)
    testList.sort()
    for t in testList:
        print t

    print "\n\nAll Messages"
    msgList = list(allMsgs)
    msgList.sort()
    for t in msgList:
        print t



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



#======================================================================

#  REVISIT recognise sections with a name. Drop sections that call them.
#  REVISIT print progress messages showing what is happening.

class Entry:
    levelRE = re.compile(r"^([>]+)")
    maskedRE = re.compile(r"^(\w+)\&([0-9a-fA-F]+)")

    def __init__(self, fields):
        self.fields   = fields
        self.level    = 0
        self.mime     = False
        self.strength = False
        self.typed    = False

        # Count the number of leading '>'
        m = self.levelRE.match(self.fields[0])
        if m != None:
            self.level = len(m.group(1))

        # See if this is a mime type declaration
        if self.fields[0] == '!:mime':
            self.mime = True
        elif self.fields[0] == '!:strength':
            self.strength = True
        elif len(self.fields) >= 2:
            self.typed = True


    def getType(self):
        if self.typed:
            return self.fields[1]
        return None

    def getTest(self):
        if len(self.fields) >= 3:
            return self.fields[2]
        return None

    def getMsg(self):
        if self.typed and self.level == 0 and len(self.fields) >= 4:
            return self.fields[3]
        return None


    def typeToC(self):
        tp       = self.getType()

        # REVISIT how do we hit this?
        if tp == None:
            return None

        tpName   = None
        unsigned = 0
        mask     = None

        if tp[0] == 'u':
            unsigned = 1
            tp = tp[1:]

        m = self.maskedRE.match(tp)
        if m != None:
            tp = m.group(1)
            mask = m.group(2)

        if   tp == "byte":      tpName = "TestType_byte"
        elif tp == "default":   tpName = "TestType_default"
        elif tp == "beshort":   tpName = "TestType_beshort"
        elif tp == "leshort":   tpName = "TestType_leshort"
        elif tp == "belong":    tpName = "TestType_belong"
        elif tp == "lelong":    tpName = "TestType_lelong"
        elif tp == "bequad":    tpName = "TestType_bequad"
        elif tp == "lequad":    tpName = "TestType_lequad"
        elif tp == "regex":     tpName = "TestType_regex"
        elif tp == "search":    tpName = "TestType_search"
        elif tp == "string":    tpName = "TestType_string"

        return (tpName, unsigned, mask)


    def dump(self):
        print "\t".join(self.fields)


class Section:
    def __init__(self, entry, exceptions):
        self.entries = [entry]
        self.exceptions = exceptions
        self.hasMime = False
        self.hasExcept = False
        self.studyEntry(entry)


    def add(self, entry):
        self.entries.append(entry)
        self.studyEntry(entry)


    def studyEntry(self, entry):
        # see if the section has any mime types
        self.hasMime = self.hasMime or entry.mime

        if entry.getMsg() in self.exceptions:
            self.hasExcept = True



    def trimToMime(self):
        # Remove all entries after the last mime entry as they
        # can't set a mime type
        while True:
            if self.entries == [] or self.entries[-1].mime:
                break
            del self.entries[-1]


    def writeToC(self, out):
        first = 1
        for e in self.entries:
            print >> out, "{"

            print >> out, ("   .newSection = %d," % first)
            print >> out, ("   .level = %d," % e.level)

            tt = e.typeToC()
            if tt != None:
                (tpName, unsigned, mask) = tt
                print >> out, ("   .testType = %s," % tpName)
                print >> out, ("   .unsignedType = %d," % unsigned)
                print >> out, ("   .testMask = %s," % mask)

            print >> out, "},\n",

            first = 0



    def dump(self):
        print "\nSection"
        for e in self.entries:
            e.dump()


def readFile(name, exceptions):
    try:
        f = open(name)
        lines = f.readlines()

        # Discard the comments and blank lines. A new section starts
        # with a level-0 first field.

        sections = []
        section  = None

        for line in lines:
            (line, _, _) = line.partition('#')
            line = line.strip()
            if len(line) == 0:
                continue

            # There may be two or four fields. The third field
            # may have quoted white space.
            fields = splitLine(line)

            if fields != []:
                # entries to discard
                if fields[0] == '!:apple':
                    continue

                entry = Entry(fields)

                if fields[0][0] != '>' and fields[0][0] != '!':
                    # Starting a new section
                    section = Section(entry, exceptions)
                    sections.append(section)
                else:
                    section.add(entry)

        f.close()
        return sections

    except IOError,exn:
        print >> sys.stderr, "Exception ", exn

    finally:
        f.close()




def Main():
    exceptions = readExceptions("exceptions")
    sections = readFile("magic", exceptions)

    # Select only those sections that have a mime type

    # If there is a message that is in the exception list
    # then drop the section.
    selected = []

    for s in sections:
        if s.hasMime and not s.hasExcept:
            selected.append(s)

    print "# all sections", len(sections)
    print "# mime sections", len(selected)

    for s in selected:
        s.trimToMime()
        s.dump()

    describeSections(selected)

    out = open("raw_entries.c", "w")

    print >> out, '#include "mimemagic_priv.h"'
    print >> out
    print >> out, 'Entry rawEntries[] = {'

    for s in selected:
        s.writeToC(out)

    print >> out, '};'
    out.close()


Main()
