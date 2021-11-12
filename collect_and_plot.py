#!/bin/python3

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
g_actuals = []

def print_help():
    print('sample usage:')
    print('\tMEMKIND_DEBUG=1 LD_LIBRARY_PATH=.libs ' \
        './utils/memtier_counter_bench/memtier_counter_bench ' \
        '-p 1 -i 5000 -r 1 -t 1 -g 2>run.log')
    print('\t./collect_and_plot.py run.log')

try:
    filename = sys.argv[1]
    if '--help' in sys.argv or '--help' in filename:
        print_help()
        exit(0)
except IndexError as e:
    print('Incorrect number of arguments')
    print_help()
    exit(-1)

for line in open(filename).read().split('\n'):
    if line == 'MEMKIND_INFO: Tier 0 - memory kind memkind_default':
        state = AWAITING_TIER0
    elif line == 'MEMKIND_INFO: Tier 1 - memory kind memkind_regular':
        state = AWAITING_TIER1
    mo = re.findall('g_hotTotalActualRatio: ([0-9\.]+)', line)
    if mo:
        val = float(mo[0])
        g_actuals.append(val)
    mo = re.findall('MEMKIND_INFO: Tier allocated size ([0-9]+)', line)
    if mo:
        val = int(mo[0])
        values[state].append(val)

tier0 = values[0]
tier1 = values[1]

ratios = []

if len(values[0]) > 0:
    for t0, t1, act in zip(values[0], values[1], g_actuals):
        if t0 != 0 and t1 != 0:
            ratios.append((t0/(t0+t1), act))
else:
    ratios = g_actuals


plt.plot(ratios)
plt.grid()
#plt.ylim(0, 2)
plt.show()

plt.savefig("run.png")
