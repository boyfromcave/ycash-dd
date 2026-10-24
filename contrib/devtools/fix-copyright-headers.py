#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
'''
Run this script inside of src/ and it will look for all the files
that were changed this year that still have the last year in the
copyright headers, and it will fix the headers on that file using
a perl regex one liner.

For example: if it finds something like this and we're in 2014

// Copyright (c) 2009-2013 The Bitcoin Core developers

it will change it to

// Copyright (c) 2009-2014 The Bitcoin Core developers

It will do this for all the files in the folder and its children.

Author: @gubatron
'''
import os
import subprocess
import time

year = time.gmtime()[0]
last_year = year - 1

# Every command below is an argv list run without a shell, so a file name
# with shell metacharacters is passed as one argument and never interpreted.
perlRegex = "s/%s The Bitcoin/%s The Bitcoin/"

extensions = [".cpp",".h"]

def getLastGitModifiedDate(filePath):
  log = subprocess.run(["git", "log", "--", filePath],
                       stdout=subprocess.PIPE, universal_newlines=True).stdout
  for l in log.splitlines():
    if "Date" in l:
      return l
  return ""

def listFiles(extension):
  for root, _, files in os.walk("."):
    for name in files:
      if name.endswith(extension):
        yield os.path.join(os.getcwd(), os.path.relpath(os.path.join(root, name), "."))

n=1
for extension in extensions:
  for filePath in listFiles(extension):
    modifiedTime = getLastGitModifiedDate(filePath)
    if len(modifiedTime) > 0 and str(year) in modifiedTime:
      print(n,"Last Git Modified: ", modifiedTime, " - ", filePath)
      subprocess.run(["perl", "-pi", "-e", perlRegex % (last_year, year), filePath])
      n = n + 1
