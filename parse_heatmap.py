#!/bin/python

import re
import math
import numpy as np
import matplotlib.pyplot as plt
import sys

def parse_heatmap(lines):
    '''
    Returns:
        (hotness, dram_to_total)
    '''
    mo = re.findall('heatmap_data = \[([0-f,;]*)\]', lines)
    ret_hotness=[]
    ret_dram_to_total=[]
    if mo:
        for datum in mo[0].split(';'):
            if datum:
                hotness, dram_to_total = datum.split(',')
                hotness = int(hotness, 16)
                dram_to_total = int(dram_to_total, 16)
                ret_hotness.append(hotness)
                ret_dram_to_total.append(dram_to_total)
    return (ret_hotness, ret_dram_to_total)

def create_heatmap(values: list):
    PERFECT_RATIO=3/4
    # a = PERFECT_RATIO * b
    # a*b == len(values)
    # perfect_ratio*b**2 == len(values)
    # b = (len(values)/PERFECT_RATIO)**0.5
    columns = math.ceil((len(values)/PERFECT_RATIO)**0.5)
    rows = math.ceil(columns*PERFECT_RATIO)
    values = values + [ 0 ]*(rows*columns-len(values))
    return np.matrix(values).reshape((rows, columns))

#lines = 'heatmap_data = [ff,0;f6,0;5c,7f;d3,4c;ce,cc;]'
#lines = 'heatmap_data = [ff,cc;e5,4c;b5,7f;4c,33;33,0;19,19;5,0;]'

def print_help():
    print('Sample usage:')
    print('./parse_heatmap.py output.log # outputs regular plot')
    print('./parse_heatmap.py output.log heat # outputs heatmap')

try:
    filename = sys.argv[1]
except IndexError:
    print_help()
    exit(-1)

if filename == '--help':
    print_help()
    exit(0)

# should display nice info and exit on failure
lines = open(filename).read()

hotness, dram_to_total = parse_heatmap(lines)



def display_heat(hotness, dram_to_total):
    hot_heat = create_heatmap(hotness)
    dram_total_heat = create_heatmap(dram_to_total)
    fig, ax = plt.subplots(2)
    ax[0].imshow(hot_heat, cmap='hot', interpolation='nearest')
    ax[0].set_title('Hotness heatmap')
    ax[1].imshow(dram_total_heat, cmap='hot', interpolation='nearest')
    ax[1].set_title('Dram to total heatmap')
    ax[0].set_xlabel('type index')
    plt.show()
    plt.clf()
    fig, ax = plt.subplots(2)
    ax[0].imshow(hot_heat, cmap='hot', interpolation='nearest')
    ax[0].set_title('Hotness heatmap')
    ax[0].set_xlabel('type index')
    ax[1].imshow(dram_total_heat, cmap='hot', interpolation='nearest')
    ax[1].set_title('Dram to total heatmap')
    plt.savefig('heat_types.png')

def display_plot(hotness, dram_to_total):
    dram_to_total = np.array(dram_to_total)/255
    hotness = np.array(hotness)/255
    fig, ax = plt.subplots(2)
    ax[0].plot(hotness, '*')
    ax[0].set_title('Hotness')
    ax[0].set_xlabel('type index')
    ax[1].set_xlabel('type index')
    ax[0].set_ylabel('hotness (normalixed)')
    ax[1].plot(dram_to_total, '*')
    ax[1].set_title('Dram to total')
    ax[0].set_xlabel('type index')
    ax[0].set_ylabel('hotness (normalixed)')
    ax[1].set_ylabel('dram/(dram+pmem)')
    fig.tight_layout()
    ax[0].grid(True)
    ax[1].grid(True)
    plt.show()
    plt.clf()
    ax[0].plot(hotness, '*')
    ax[0].set_title('Hotness')
    ax[0].set_xlabel('type index')
    ax[0].set_ylabel('hotness (normalixed)')
    ax[1].plot(dram_to_total, '*')
    ax[1].set_title('Dram to total')
    ax[0].set_xlabel('type index')
    ax[0].set_ylabel('hotness (normalixed)')
    ax[1].set_ylabel('dram/(dram+pmem)')
    fig.tight_layout()
    ax[0].grid(True)
    ax[1].grid(True)
    plt.savefig('heat_types.png')

if 'heat' in sys.argv:
    display_heat(hotness, dram_to_total)
else:
    display_plot(hotness, dram_to_total)
