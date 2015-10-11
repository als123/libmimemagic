#!/usr/bin/env python

import sys;
import re;
import utils;

#======================================================================

stringTests = [
    r"\xabcd",
    r"\xazzz",
    r"fred\xab",
    r"fred\012",
    r"fred\0wilma",
    r'\\Fred "and" Wilma',
    ]

for s in stringTests:
    print utils.quoteForC(s)


for s in stringTests:
    bs = utils.splitStringBytes(s)
    c  = utils.bytesToC(bs)
    print bs, c

print utils.bytesToC([3])
print utils.bytesToC([92])
print utils.bytesToC([10])

#======================================================================

code = "line1\nline2\n"

indented = utils.addIndent(code, 1)
print indented,
