#include "OrderBook.h"
#include "SpscRingBuffer.h"

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <fstream>
#include <memory>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <thread>
#include <vector>
#include <x86intrin.h>
inline uint64_t get_cycles() { return __rdtsc(); }

void pin_current_thread(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pthread_t current_thread = pthread_self();
  pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

constexpr std::size_t LOG_RESERVE_SIZE = 10'000'000;

struct OrderInput {
  uint64_t id;
  uint32_t quantity;
  uint32_t price;
  Side side;
};

void logging_thread_func(SpscRingBuffer<Trade> &queue, std::atomic<bool> &system_running,
                         const std::string &filename) {
  pin_current_thread(4);
  pthread_setname_np(pthread_self(), "hft_logger");

  std::vector<Trade> history;
  history.reserve(LOG_RESERVE_SIZE);
  Trade trade;

  while (true) {
    if (queue.try_pop(trade)) {
      history.push_back(trade);
    } else if (!system_running.load(std::memory_order_relaxed)) {
      break;
    } else {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#elif defined(__aarch64__)
      asm volatile("yield");
#else
      std::this_thread::yield();
#endif
    }
  }

  while (queue.try_pop(trade)) {
    history.push_back(trade);
  }

  if (!history.empty()) {
    std::ofstream log_file(filename);
    for (const auto &t : history) {
      log_file << t.id_buyer << "," << t.id_seller << "," << t.id_transaction << "," << t.price
               << "," << t.quantity << "\n";
    }
  }
}

// TESTDATA-GENERATOR

struct MarketScenario {
  std::vector<OrderInput> makers;
  std::vector<OrderInput> takers;
  std::vector<OrderInput> cancel_sequence;
};

MarketScenario generate_scenario(size_t increases = 50, size_t base = 10'000) {
  MarketScenario scenario;
  const size_t batch_size = increases * base;

  scenario.makers.reserve(batch_size);
  scenario.takers.reserve(batch_size);

  std::mt19937 rng(21);
  std::uniform_int_distribution<uint32_t> price_dist(100, 149);

  for (size_t i = 0; i < increases; ++i) {
    for (size_t j = 0; j < base; ++j) {
      uint32_t price = price_dist(rng);
      uint32_t qty = 10 + (j % 5);
      uint64_t order_id = (i * base) + j;

      scenario.makers.push_back({order_id, qty, price, BUY});
      scenario.takers.push_back({order_id + batch_size, qty, 0, SELL});
    }
  }

  scenario.cancel_sequence = scenario.makers;
  std::shuffle(scenario.cancel_sequence.begin(), scenario.cancel_sequence.end(), rng);

  return scenario;
}

// THROUGHPUT ADD

static void BM_Throughput_Add(benchmark::State &state) {
  pin_current_thread(2);
  pthread_setname_np(pthread_self(), "hft_engine");

  auto scenario = generate_scenario();
  auto trade_queue = std::make_unique<SpscRingBuffer<Trade>>(22);
  std::atomic<bool> system_running{true};

  std::thread logger(logging_thread_func, std::ref(*trade_queue), std::ref(system_running),
                     "logging/throughput_add_trades.txt");

  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(*trade_queue);
    state.ResumeTiming();

    for (const auto &order : scenario.makers)
      book->add_order(order.id, order.quantity, order.price, order.side);

    for (const auto &order : scenario.takers)
      book->add_order(order.id, order.quantity, order.price, order.side);

    benchmark::DoNotOptimize(*book);
  }

  system_running = false;
  logger.join();
  state.SetItemsProcessed(state.iterations() * (scenario.makers.size() * 2));
}

// THROUGHPUT CANCEL

static void BM_Throughput_Cancel(benchmark::State &state) {
  pin_current_thread(2);
  auto scenario = generate_scenario();
  auto trade_queue = std::make_unique<SpscRingBuffer<Trade>>(22);
  std::atomic<bool> system_running{true};
  std::thread logger(logging_thread_func, std::ref(*trade_queue), std::ref(system_running),
                     "logging/throughput_canel_trades.txt");

  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(*trade_queue);
    for (const auto &order : scenario.makers) {
      book->add_order(order.id, order.quantity, order.price, order.side);
    }
    state.ResumeTiming();

    for (const auto &order : scenario.cancel_sequence) {
      book->cancel_order(order.id, order.price, order.side);
    }

    benchmark::DoNotOptimize(*book);
  }

  system_running = false;
  logger.join();
  state.SetItemsProcessed(state.iterations() * scenario.cancel_sequence.size());
}

// 3. LATENCY

static void BM_Latency_Add(benchmark::State &state) {
  pin_current_thread(2);

  auto scenario = generate_scenario();
  auto trade_queue = std::make_unique<SpscRingBuffer<Trade>>(22);
  std::atomic<bool> system_running{true};

  std::thread logger(logging_thread_func, std::ref(*trade_queue), std::ref(system_running),
                     "logging/latency_add_trades.txt");

  size_t max_samples = state.max_iterations * (scenario.makers.size() + scenario.takers.size());
  std::vector<uint64_t> latencies(max_samples, 0);
  size_t lat_idx = 0;

  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(*trade_queue);
    state.ResumeTiming();

    for (const auto &order : scenario.makers) {
      uint64_t start = get_cycles();
      book->add_order(order.id, order.quantity, order.price, order.side);
      latencies[lat_idx++] = get_cycles() - start;
    }

    for (const auto &order : scenario.takers) {
      uint64_t start = get_cycles();
      book->add_order(order.id, order.quantity, order.price, order.side);
      latencies[lat_idx++] = get_cycles() - start;
    }

    benchmark::DoNotOptimize(*book);
  }

  system_running = false;
  logger.join();

  std::sort(latencies.begin(), latencies.end());
  const double CPU_GHZ = 1.6;
  auto set_percentile = [&](const char *name, double p) {
    size_t idx = static_cast<size_t>(latencies.size() * p);
    state.counters[name] = static_cast<double>(latencies[idx]) / CPU_GHZ;
  };

  set_percentile("1.Add_P50_ns", 0.50);
  set_percentile("2.Add_P99_ns", 0.99);
  set_percentile("3.Add_P99.99_ns", 0.9999);
}

// LATENCY CANCEL

static void BM_Latency_Cancel(benchmark::State &state) {
  pin_current_thread(2);
  auto scenario = generate_scenario(10, 10'000);
  auto trade_queue = std::make_unique<SpscRingBuffer<Trade>>(22);
  std::atomic<bool> system_running{true};

  std::thread logger(logging_thread_func, std::ref(*trade_queue), std::ref(system_running),
                     "logging/latency_cancel_trades.txt");
  size_t max_samples = state.max_iterations * (scenario.makers.size() + scenario.takers.size());
  std::vector<uint64_t> latencies(max_samples, 0);
  size_t lat_idx = 0;

  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(*trade_queue);
    for (const auto &order : scenario.makers) {
      book->add_order(order.id, order.quantity, order.price, order.side);
    }
    state.ResumeTiming();

    for (const auto &order : scenario.cancel_sequence) {
      uint64_t start = get_cycles();
      book->cancel_order(order.id, order.price, order.side);
      latencies[lat_idx++] = get_cycles() - start;
    }
  }

  system_running = false;
  logger.join();

  std::sort(latencies.begin(), latencies.end());
  const double CPU_GHZ = 1.6;
  auto set_percentile = [&](const char *name, double p) {
    size_t idx = static_cast<size_t>(latencies.size() * p);
    state.counters[name] = static_cast<double>(latencies[idx]) / CPU_GHZ;
  };

  set_percentile("1.Cancel_P50_ns", 0.50);
  set_percentile("2.Cancel_P99_ns", 0.99);
  set_percentile("3.Cancel_P99.99_ns", 0.9999);
}

// REGISTRATION

BENCHMARK(BM_Throughput_Add)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Throughput_Cancel)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Latency_Add)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Latency_Cancel)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
