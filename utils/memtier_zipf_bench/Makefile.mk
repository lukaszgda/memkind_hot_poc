# SPDX-License-Identifier: BSD-2-Clause
# Copyright (C) 2021 Intel Corporation.

noinst_PROGRAMS += utils/memtier_zipf_bench/memtier_zipf_bench

# EXTRA_DIST += utils/memtier_zipf_bench/run_perf.sh

utils_memtier_zipf_bench_memtier_zipf_bench_SOURCES = utils/memtier_zipf_bench/memtier_zipf_counter_bench.cpp
utils_memtier_zipf_bench_memtier_zipf_bench_LDADD = libmemkind.la
utils_memtier_zipf_bench_memtier_zipf_bench_LDFLAGS = $(PTHREAD_CFLAGS)

clean-local: utils_memtier_zipf_bench_memtier_zipf_bench-clean

utils_memtier_zipf_bench_memtier_zipf_bench-clean:
	rm -f utils/memtier_zipf_bench/*.gcno
