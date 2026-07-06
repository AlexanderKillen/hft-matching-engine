# Performance & Benchmarks

This document outlines the performance characteristics of the matching engine, how to replicate the tests, and the profiling methodology.

## 1. Environment

```bash
lscpu | grep "Model name"
Model name:                              Intel(R) Core(TM) i5-10210U CPU @ 1.60GHz
```

And 8 GB LPDDR3-2133

### Lock CPU frequency

```bash
sudo sh -c "echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

### Verify effect

```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq
```

### Reset to normal

```bash
echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
sudo sh -c "echo powersave | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
```

## Memory Diagnostics (AddressSanitizer)

```bash
g++ -std=c++20 -O0 -g3 -ggdb3 -fsanitize=address -march=native benchmark.cpp OrderBook.cpp  -L/usr/local/lib -lbenchmark -lpthread -o benchmark_debug

./benchmark_debug
```

## Profiling & Hot Path Analysis

Google Benchmark is used to generate realistic (ehh) synthetic loads for performance reports and hardware sampling.

Note: -L/usr/local/lib is on device compiled library without debug.

```bash
g++ -std=c++20 -O3 -DNDEBUG -g -march=native -flto benchmark.cpp OrderBook.cpp -L/usr/local/lib -lbenchmark -lpthread -o benchmark_run

sudo perf record -F 4000 --call-graph lbr ./benchmark_run

sudo perf report -c hft_engine
```

## Benchmark results

"Throughput is vanity, latency is sanity."

Compiled with production flags:

```bash
g++ -std=c++20 -O3 -DNDEBUG -march=native -flto benchmark.cpp OrderBook.cpp -L/usr/local/lib -lbenchmark -lpthread -o benchmark_run

./benchmark_run
```

```
-------------------------------------------------------------------------------
Benchmark                     Time             CPU   Iterations UserCounters...
-------------------------------------------------------------------------------
BM_Throughput_Add          80.3 ms         80.2 ms            9 items_per_second=12.4748M/s
BM_Throughput_Cancel       1.52 ms         1.52 ms          460 items_per_second=329.943M/s
BM_Latency_Add             93.3 ms         93.3 ms            7 1.Add_P50_ns=23.75 2.Add_P99_ns=356.25 3.Add_P99.99_ns=905
BM_Latency_Cancel          3.37 ms         3.37 ms          208 1.Cancel_P50_ns=21.25 2.Cancel_P99_ns=23.75 3.Cancel_P99.99_ns=33.75
```

Throughput of adding orders is around ~12.5 million ops/sec. Cencelling orders is around ~330 million ops/sec due to O(1) direct array index lookups and relinking in the memory pool.

Latency: Median latency p50 sits at 35 ns for Add and 21 ns for Cancel.

Tail Latency p99.99: The spikes in the upper percentiles (900 ns for Add) was found through perf analysis to be because of last level cache misses due the non-spatial locality of orders due to orders being added sequentially to the memory_pool_. Use a slab allocator.
