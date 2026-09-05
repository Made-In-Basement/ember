"""Write the build stamp the kernel and the desktop both include.

The number is the count of commits, which only ever goes up (plus one when
the tree has uncommitted changes, since that is what the next commit will
be); the time is when the image was made, which tells two builds of the
same commit apart.  Written as build/version.inc for NASM and
build/version.h for C.
"""
import os
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def main():
    os.makedirs(BUILD, exist_ok=True)
    try:
        n = subprocess.run(["git", "rev-list", "--count", "HEAD"], cwd=ROOT,
                           capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                               capture_output=True, text=True).stdout.strip()
        n = int(n) + (1 if dirty else 0)
    except (OSError, ValueError):
        n = 0
    stamp = "build %d, %s" % (n, time.strftime("%Y-%m-%d %H:%M"))
    with open(os.path.join(BUILD, "version.inc"), "w") as f:
        f.write('%%define BUILD_STAMP "%s"\n' % stamp)
    with open(os.path.join(BUILD, "version.h"), "w") as f:
        f.write('#define BUILD_STAMP "%s"\n' % stamp)
    print("Build stamp:", stamp)


if __name__ == "__main__":
    main()
