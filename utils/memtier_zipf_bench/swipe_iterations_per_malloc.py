#!/bin/python

import subprocess
import os
import re
import matplotlib.pyplot as plt

STATIC=0
HOTNESS=1

re_exec_adjusted=re.compile('Measured execution time \[millis_thread\|millis_thread_adjusted\|millis_system\]: \[[0-9]*\|([0-9]*)\|[0-9]*\]')
re_accesses_per_malloc=re.compile('Stats \[accesses_per_malloc\|average_size\]: \[([0-9\.]*)\|[0-9\.]*\]')

def get_adjusted_execution_time(iterations, static_hotness):
    '''
    Returns:
        (adjusted_time, accesses_per_allocation, average_size)
    '''
    m_env = os.environ.copy()
    m_env['MEMKIND_DEBUG']='1'
    m_env['LD_LIBRARY_PATH']='.libs'
    args=[]
    if static_hotness == STATIC:
        args.append('static')
    elif static_hotness == HOTNESS:
        args.append('hotness')
    else:
        raise Exception('Incorrect function argument!')
    args.append(str(iterations))
    ret = subprocess.run(['./utils/memtier_zipf_bench/.libs/memtier_zipf_bench'] + args, env=m_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # what do we have:
    # 1. Ratio history
    # 2. Results
    stats = ret.stdout.decode()
    adjusted_exec_time_millis = int(re_exec_adjusted.findall(stats)[0])
    accesses_per_malloc = float(re_accesses_per_malloc.findall(stats)[0])
    return adjusted_exec_time_millis, accesses_per_malloc

iterations = [20, 40, 80, 160, 320]

execution_times_static = []
execution_times_hotness = []
accesses_per_malloc_static = []
accesses_per_malloc_hotness = []

for it in iterations:
    static_time, static_acc = get_adjusted_execution_time(it, STATIC)
    hotness_time, hotness_acc = get_adjusted_execution_time(it, HOTNESS)
    execution_times_static.append(static_time)
    execution_times_hotness.append(hotness_time)
    accesses_per_malloc_static.append(static_acc)
    accesses_per_malloc_hotness.append(hotness_acc)

plt.plot(accesses_per_malloc_static, execution_times_static, label='static')
plt.plot(accesses_per_malloc_hotness, execution_times_hotness, label='hotness')
plt.grid()
plt.show()

plt.plot(accesses_per_malloc_static, execution_times_static, label='static')
plt.plot(accesses_per_malloc_hotness, execution_times_hotness, label='hotness')
plt.grid()
plt.savefig('stats.png')


