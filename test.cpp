#include "OrderBook.h"
#include "SpscRingBuffer.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

void logging_thread_func(SpscRingBuffer<Trade> &queue, std::atomic<bool> &running) {
  std::ofstream log_file("trades_output.txt");
  Trade trade;

  while (true) {
    if (queue.try_pop(trade)) {
      log_file << trade.id_transaction << "," << trade.price << "," << trade.quantity << "\n";
    } else if (!running.load(std::memory_order_relaxed)) {
      break;
    } else {
      std::this_thread::yield();
    }
  }
}

int main() {
  auto q = std::make_unique<SpscRingBuffer<Trade>>(16);
  auto book = std::make_unique<OrderBook>(*q);

  std::atomic<bool> system_running{true};
  std::thread logger(logging_thread_func, std::ref(*q), std::ref(system_running));

  book->add_order(1, 10, 100, BUY);
  book->add_order(2, 10, 100, SELL);

  system_running = false;
  logger.join();

  return 0;
}
