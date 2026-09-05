r"""Catch the escape sequence that keeps biting: a lone backslash in a C string.

Writing C through a shell heredoc quietly eats one backslash, so "\\FILE"
reaches the compiler as "\FILE".  Some of those are silently accepted --
\E and \e are a GNU extension for the escape character -- so a path turns
into rubbish and every open of it fails with "file not found", which looks
nothing like a quoting mistake.  This finds them before the machine does.

    python tools/check_escapes.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOWN = set('\\ntr0abfv"\'x?')          # the escapes C actually defines
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def scan(path):
    hits = []
    with open(path, encoding='latin-1') as f:
        for n, line in enumerate(f, 1):
            for m in STRING.finditer(line):
                body = m.group(1)
                for i, ch in enumerate(body):
                    if ch != '\\':
                        continue
                    if i and body[i - 1] == '\\':
                        continue                # part of an escaped backslash
                    nxt = body[i + 1] if i + 1 < len(body) else ''
                    if nxt not in KNOWN:
                        hits.append((n, nxt, line.rstrip()))
    return hits


def main():
    total = 0
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in ('build', '.git')]
        for name in files:
            if not name.endswith(('.c', '.h')):
                continue
            path = os.path.join(base, name)
            for n, ch, line in scan(path):
                rel = os.path.relpath(path, ROOT)
                print('%s:%d: suspicious \\%s   %s' % (rel, n, ch, line.strip()[:70]))
                total += 1
    if total:
        print('\n%d suspicious escape%s: a path written as "\\FILE" should be '
              '"\\\\FILE".' % (total, '' if total == 1 else 's'))
        return 1
    print('no stray escapes in any C source')
    return 0


if __name__ == '__main__':
    sys.exit(main())
