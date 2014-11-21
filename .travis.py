#!/usr/bin/python2

# Travis aborts if we generate either too much output (> 4 MB) or
# too little (nothing for 10 min), so wrap the build with some
# Python...

import subprocess, sys

log = []
child = subprocess.Popen(["make"], bufsize = 1, stdout = subprocess.PIPE, stderr = subprocess.STDOUT)
for i, line in enumerate(child.stdout):
    if i % 10 == 0 or 'Entering directory' in line:
        sys.stdout.write("%d: %s" % (i, line))
        sys.stdout.flush()

    log.append(line)

child.wait()
if child.returncode:
    print "Build FAILED"
    for line in log[-1000:]: print line,
else:
    print "Build PASSED"
    for line in log[-10:]: print line,
