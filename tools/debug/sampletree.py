#!/usr/bin/env python3
"""Inclusive time per function from a macOS `sample` call tree under a root function (counted once per stack,
recursion-safe). sampletree.py <sample.txt> <root function> [n]"""
import re, sys
L = open(sys.argv[1]).read().split('\n')
root = sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
def depth(l): return len(l) - len(l.lstrip(' +!:|'))
tot = {}
i = 0
while i < len(L):
    if re.search(r'\d+ ' + re.escape(root) + r'\s', L[i]):
        d0 = depth(L[i]); stack = []
        j = i + 1
        while j < len(L) and L[j].strip() and depth(L[j]) > d0:
            m = re.search(r'(\d+) (\S+)\s+\(in (\S+)\)', L[j])
            if m:
                d = depth(L[j])
                while stack and stack[-1][0] >= d: stack.pop()
                name = m.group(2)
                if all(s[1] != name for s in stack):
                    tot[name] = tot.get(name, 0) + int(m.group(1))
                stack.append((d, name))
            j += 1
        i = j
    else:
        i += 1
for k, v in sorted(tot.items(), key=lambda x: -x[1])[:n]: print(v, k)
