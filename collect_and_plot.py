#!/bin/python

import sys
import re
import matplotlib.pyplot as plt

AWAITING_TIER0=0
AWAITING_TIER1=1

state = -1
values = [[], []]

for line in open(sys.argv[1]).read().split('\n'):
    if line == 'MEMKIND_INFO: Tier 0 - memory kind memkind_default':
        state = AWAITING_TIER0
    elif line == 'MEMKIND_INFO: Tier 1 - memory kind memkind_regular':
        state = AWAITING_TIER1
    mo = re.findall('MEMKIND_INFO: Tier allocated size ([0-9]+)', line)
    if mo:
        val = int(mo[0])
        values[state].append(val)

tier0 = values[0]
tier1 = values[1]

ratios = []

for t0, t1 in zip(values[0], values[1]):
    if t0 != 0 and t1 != 0:
        ratios.append(t0/t1)


plt.plot(ratios)
plt.grid()
plt.xlim(0, 1)
plt.show()
