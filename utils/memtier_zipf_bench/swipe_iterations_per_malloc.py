#!/bin/python

import subprocess
import os
import sys
import re
import matplotlib.pyplot as plt
import numpy as np

STATIC=0
HOTNESS=1

#iterations = 5*1.5**np.array(range(12))
iterations = 20*2**np.array(range(12))
#iterations = 5*1.5**np.array(range(3))

THREADS=8

re_exec_adjusted=re.compile('Measured execution time \[millis_thread\|millis_thread_adjusted\|millis_system\]: \[[0-9]*\|([0-9]*)\|[0-9]*\]')
re_accesses_per_malloc=re.compile('Stats \[accesses_per_malloc\|average_size\]: \[([0-9\.]*)\|[0-9\.\+e]*\]')

def get_adjusted_execution_time(iterations, static_hotness):
    '''
    Run and check returned values
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
    args.append(str(THREADS))
    ret = subprocess.run(['./utils/memtier_zipf_bench/.libs/memtier_zipf_bench'] + args, env=m_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stats = ret.stdout.decode()
    adjusted_exec_time_millis = int(re_exec_adjusted.findall(stats)[0])
    accesses_per_malloc = float(re_accesses_per_malloc.findall(stats)[0])
    return adjusted_exec_time_millis, accesses_per_malloc

def get_adjusted_execution_time_exec_obj(iterations, static_hotness):
    '''
    Run asynchronously; please note that a call to
    **interpret_finished_exec_obj** will be necessary after process finishes
    execution, e.g. after ret,wait() is called
    Returns:
        subprocess.Popen object
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
    ret = subprocess.Popen(['./utils/memtier_zipf_bench/.libs/memtier_zipf_bench'] + args, env=m_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return ret

def interpret_finished_stdout(stdout_val):
    stats = stdout_val.decode()
    adjusted_exec_time_millis = int(re_exec_adjusted.findall(stats)[0])
    accesses_per_malloc = float(re_accesses_per_malloc.findall(stats)[0])
    return adjusted_exec_time_millis, accesses_per_malloc

def process_all_async(iterations):
    static_exec_objs=[]
    hotness_exec_objs=[]
    for it in iterations:
        static_exec_objs.append(get_adjusted_execution_time_exec_obj(it, STATIC))
        hotness_exec_objs.append(get_adjusted_execution_time_exec_obj(it, HOTNESS))
    execution_times_static = []
    execution_times_hotness = []
    accesses_per_malloc_static = []
    accesses_per_malloc_hotness = []
    for stat_exec_obj, hot_exec_obj in zip(static_exec_objs, hotness_exec_objs):
        stat_stdout, stat_stderr = stat_exec_obj.communicate()
        hot_stdout, hot_stderr = hot_exec_obj.communicate()
        static_time, static_acc = interpret_finished_stdout(stat_stdout)
        hotness_time, hotness_acc = interpret_finished_stdout(hot_stdout)
        execution_times_static.append(static_time)
        execution_times_hotness.append(hotness_time)
        accesses_per_malloc_static.append(static_acc)
        accesses_per_malloc_hotness.append(hotness_acc)
    return (accesses_per_malloc_static, execution_times_static, accesses_per_malloc_hotness, execution_times_hotness)


def process_all_sync(iterations):
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
    return (accesses_per_malloc_static, execution_times_static, accesses_per_malloc_hotness, execution_times_hotness)


accesses_per_malloc_static, execution_times_static, accesses_per_malloc_hotness, execution_times_hotness = process_all_sync(iterations) if 'sync' in sys.argv else process_all_async(iterations)



# interpolate
maxval = np.min([np.max(accesses_per_malloc_static), np.max(accesses_per_malloc_hotness)])
minval = np.max([np.min(accesses_per_malloc_static), np.min(accesses_per_malloc_hotness)])
assert minval < maxval # might not be true in some corner cases - make sure they don't appear
check_points = np.logspace(np.log10(minval), np.log10(maxval), 10)
interp_hot = np.interp(check_points, accesses_per_malloc_hotness, execution_times_hotness)
interp_static = np.interp(check_points, accesses_per_malloc_static, execution_times_static)
hotness_overhead = (np.array(interp_hot)-np.array(interp_static))/np.array(interp_static)

fig, axs = plt.subplots(1, 2)

axs[0].plot(accesses_per_malloc_static, execution_times_static, label='static')
axs[0].plot(accesses_per_malloc_hotness, execution_times_hotness, label='hotness')
axs[0].set_xlabel('accesses per allocation')
axs[0].set_ylabel('total execution time')
axs[0].grid(True)
axs[0].legend()
axs[0].set_xscale('log')

axs[1].plot(check_points, hotness_overhead, label='hotness policy overhead')
axs[1].set_xlabel('accesses per allocation - interpolated')
axs[1].set_ylabel('data_hotness vs static policy overhead, in %')
axs[1].set_xscale('log')
axs[1].grid(True)
axs[1].legend()
fig.tight_layout()

plt.show()
plt.clf()

fig, axs = plt.subplots(1, 2)

axs[0].plot(accesses_per_malloc_static, execution_times_static, label='static')
axs[0].plot(accesses_per_malloc_hotness, execution_times_hotness, label='hotness')
axs[0].set_xlabel('accesses per allocation')
axs[0].set_ylabel('total execution time')
axs[0].grid(True)
axs[0].legend()
axs[0].set_xscale('log')

axs[1].plot(check_points, hotness_overhead, label='hotness policy overhead')
axs[1].set_xlabel('accesses per allocation - interpolated')
axs[1].set_ylabel('data_hotness vs static policy overhead, in %')
axs[1].set_xscale('log')
axs[1].grid(True)
axs[1].legend()
fig.tight_layout()

plt.savefig('stats.png')
