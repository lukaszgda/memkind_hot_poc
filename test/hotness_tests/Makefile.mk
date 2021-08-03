noinst_PROGRAMS += test/hotness_tests/matmul

test_hotness_tests_matmul_SOURCES = test/hotness_tests/matmul.c

clean-local: test_hotness_tests_matmul-clean

test_hotness_tests_matmul-clean:
	rm -f test/hotness_tests/matmul.*
