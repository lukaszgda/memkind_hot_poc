#!/usr/bin/python

# this script is used to run memtier_counter_bench multiple times with different thresholds
import numpy as np
import subprocess
import re
import os
from matplotlib import pyplot as plt

class Policy:
    STATIC_RATIO='-s'
    DATA_HOTNESS='-p'

def compile_command(runs: int, iterations: int, policy: Policy):
    return ['./utils/memtier_counter_bench/memtier_counter_bench', policy, '-i', f'{iterations}', '-r', f'{runs}', '-t', '1']


def collect_data_multiple_runs(runs: [int], iterations: int, policy: Policy):
    ratios=[]
    menv = os.environ.copy()
    menv['LD_LIBRARY_PATH'] = '.libs:' + menv['LD_LIBRARY_PATH'] if 'LD_LIBRARY_PATH' in menv else ''
    for cruns in runs:
        command = compile_command(cruns, iterations, policy)
        out = subprocess.run(command, stdout=subprocess.PIPE)
        sout = out.stdout.decode()
        ratio = None
        for line in sout.split('\n'):
            mo = re.findall('actual\|desired DRAM/TOTAL ratio: ([0-9\.]*) ', line)
            if mo:
                ratio = float(mo[0])
        if ratio is not None:
            ratios.append(ratio)
        else:
            raise Exception('Ratio not found!')
    return ratios

def collect_data_multiple_iters(runs: int, iters: int, policy: Policy):
    ratios=[]
    menv = os.environ.copy()
    menv['LD_LIBRARY_PATH'] = '.libs:' + menv['LD_LIBRARY_PATH'] if 'LD_LIBRARY_PATH' in menv else ''
    for citers in iters:
        command = compile_command(runs, citers, policy)
        out = subprocess.run(command, stdout=subprocess.PIPE)
        sout = out.stdout.decode()
        ratio = None
        for line in sout.split('\n'):
            mo = re.findall('actual\|desired DRAM/TOTAL ratio: ([0-9\.]*) ', line)
            if mo:
                ratio = float(mo[0])
        if ratio is not None:
            ratios.append(ratio)
        else:
            raise Exception('Ratio not found!')
    return ratios


#runs=np.logspace(1, 3.5, 10)
runs=np.logspace(2, 5.5, 8)
results = collect_data_multiple_runs(runs, 10, Policy.DATA_HOTNESS)

plt.plot(runs, results, '*')
plt.xscale('log')
plt.grid()
plt.xlabel('runs')
plt.ylabel('ratio')
plt.show()

iterations=np.logspace(2, 5.5, 8)
results = collect_data_multiple_iters(10, iterations, Policy.DATA_HOTNESS)

plt.plot(iterations, results, '*')
plt.xscale('log')
plt.grid()
plt.xlabel('iterations')
plt.ylabel('ratio')
plt.show()
