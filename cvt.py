#!/usr/bin/env python

import sys;
import re;

fieldRE = re.compile(r"^(\S+)\s+(\S+)\s+(\S+)\s+(.*)")
levelRE = re.compile(r"^([>]+)")

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


#======================================================================


class Entry:
    def __init__(self, fields):
        self.fields   = fields
        self.level    = 0
        self.mime     = False
        self.strength = False
        self.typed    = False

        # Count the number of leading '>'
        m = levelRE.match(self.fields[0])
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


    def dump(self):
        print "\t".join(self.fields)


class Section:
    def __init__(self, entry):
        self.entries = [entry]
        self.hasMime = False
        self.studyEntry(entry)


    def add(self, entry):
        self.entries.append(entry)
        self.studyEntry(entry)


    def studyEntry(self, entry):
        # see if the section has any mime types
        self.hasMime = self.hasMime or entry.mime


    def trimToMime(self):
        # Remove all entries after the last mime entry as they
        # can't set a mime type
        while True:
            if self.entries == [] or self.entries[-1].mime:
                break
            del self.entries[-1]


    def dump(self):
        print "\nSection"
        for e in self.entries:
            e.dump()


def readFile(name):
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
                    section = Section(entry)
                    sections.append(section)
                else:
                    section.add(entry)

        f.close()
        return sections
    except IOError,exn:
        print >> sys.stderr, "Exception ", exn




def Main():
    sections = readFile("magic")

    # Select only those sections that have a mime type
    selected = []

    for s in sections:
        if s.hasMime:
            selected.append(s)

    print "# all sections", len(sections)
    print "# mime sections", len(selected)

    for s in selected:
        s.trimToMime()
        s.dump()

    describeSections(selected)

Main()
