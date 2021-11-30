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

lines = open(sys.argv[1]).read()

hotness, dram_to_total = parse_heatmap(lines)

fig, ax = plt.subplots(2)

hot_heat = create_heatmap(hotness)
dram_total_heat = create_heatmap(dram_to_total)

ax[0].imshow(hot_heat, cmap='hot', interpolation='nearest')
ax[0].set_title('Hotness heatmap')
ax[1].imshow(dram_total_heat, cmap='hot', interpolation='nearest')
ax[1].set_title('Dram to total heatmap')
plt.show()
create_heatmap(hotness)
create_heatmap(dram_to_total)
