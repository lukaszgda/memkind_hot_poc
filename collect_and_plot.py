#!/bin/python

# This script reads a log file, parses it to extract information about hot/cold
# ratio and plots it on the screen
#
# The ratio is calculated as hot/cold, i.e.
#   1:1 should give ratio 1,
#   1:2 should give ratio 0.5,
#   1:8 should give ratio 0.125,
#
# Please note that some internal calculations use hot/total ratio, not hot/cold

import sys
import re
import matplotlib.pyplot as plt

AWAITING_TIER0=0
AWAITING_TIER1=1

state = -1
values = [[], []]

try:
    filename = sys.argv[1]
except IndexError as e:
    print('Incorrect number of arguments')
    print('sample usage:')
    print('\tMEMKIND_DEBUG=1 LD_LIBRARY_PATH=.libs ' \
        './utils/memtier_counter_bench/memtier_counter_bench ' \
        '-p -i 5000 -r 3 -t 1 -g 2>run.log')
    print('\t./collect_and_plot.py run.log')
    exit(-1)

for line in open(filename).read().split('\n'):
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
#plt.ylim(0, 2)
plt.show()

plt.savefig("run.png")
