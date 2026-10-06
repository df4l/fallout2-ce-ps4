#!/usr/bin/env python3
"""create-gp4 emits a hardcoded <rootdir>; rebuild it from the <file> entries."""
import re, sys
p = sys.argv[1]
s = open(p).read()
paths = re.findall(r'targ_path="([^"]+)"', s)
tree = {}
for path in paths:
    node = tree
    for d in path.split('/')[:-1]:
        node = node.setdefault(d, {})
def emit(n, ind):
    out = ''
    for k, v in sorted(n.items()):
        if v:
            out += f'{ind}<dir targ_name="{k}">\n{emit(v, ind + chr(9))}{ind}</dir>\n'
        else:
            out += f'{ind}<dir targ_name="{k}" />\n'
    return out
s = re.sub(r'<rootdir>.*?</rootdir>', '<rootdir>\n' + emit(tree, '\t\t') + '\t</rootdir>', s, flags=re.S)
open(p, 'w').write(s)
