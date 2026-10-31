CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
INCLUDES = -Iinclude -Isrc -I.

CORE_SRCS = \
	src/util/crc32c.cpp \
	src/util/histogram.cpp \
	src/util/zipf.cpp \
	src/util/thread_pool.cpp \
	src/memtable/memtable_lockfree.cpp \
	src/memtable/memtable_global_lock.cpp \
	src/memtable/memtable_sharded.cpp \
	src/wal/wal_writer.cpp \
	src/wal/wal_reader.cpp \
	src/sstable/bloom.cpp \
	src/sstable/block.cpp \
	src/sstable/sstable_builder.cpp \
	src/sstable/sstable_reader.cpp \
	src/version/version.cpp \
	src/version/version_set.cpp \
	src/version/manifest.cpp \
	src/compaction/compaction_picker.cpp \
	src/compaction/compaction_job.cpp \
	src/compaction/compaction_pool.cpp \
	src/db/db_impl.cpp \
	src/db/flush.cpp \
	src/db/recovery.cpp \
	bench/workload_gen.cpp \
	bench/metrics.cpp

CORE_OBJS = $(CORE_SRCS:.cpp=.o)

.PHONY: all clean bench cli crash

all: lsm_bench lsm_cli crash_child crash_runner

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

liblsm_core.a: $(CORE_OBJS)
	ar rcs $@ $^

lsm_bench: bench/bench_main.o liblsm_core.a
	$(CXX) $(CXXFLAGS) $^ -o $@

lsm_cli: tools/lsm_cli.o liblsm_core.a
	$(CXX) $(CXXFLAGS) $^ -o $@

crash_child: tests/crash_child.o liblsm_core.a
	$(CXX) $(CXXFLAGS) $^ -o $@

crash_runner: tests/crash_runner.o liblsm_core.a
	$(CXX) $(CXXFLAGS) $^ -o $@

bench/bench_main.o: bench/bench_main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

tools/lsm_cli.o: tools/lsm_cli.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

tests/crash_child.o: tests/crash_child.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

tests/crash_runner.o: tests/crash_runner.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(CORE_OBJS) bench/bench_main.o tools/lsm_cli.o tests/crash_child.o tests/crash_runner.o liblsm_core.a lsm_bench lsm_cli crash_child crash_runner
