#include "SpscRingBuffer.h"
#include <array>
#include <cstdint>
#include <limits>

static constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

enum Side : uint8_t { BUY, SELL };

struct Order {
  uint32_t id = 0x0;
  uint32_t quantity = 0x0;
  uint32_t price = 0x0;
  uint32_t next = NULL_INDEX;
  uint32_t prev = NULL_INDEX;
};

struct OrderQueue {
  uint32_t first = NULL_INDEX;
  uint32_t last = NULL_INDEX;
};

struct Trade {
  uint32_t id_buyer;
  uint32_t id_seller;
  uint64_t id_transaction; // Data alignment... but this is ok.
  uint32_t price;
  uint32_t quantity;
};

static constexpr uint32_t next_power_of_two(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return ++v;
}

class OrderBook {
public:
  explicit OrderBook(SpscRingBuffer<Trade> &queue);
  uint64_t add_order(uint64_t id, uint32_t quantity, uint32_t price, Side side);
  bool cancel_order(uint64_t id_user, uint32_t price, Side side);

private:
  SpscRingBuffer<Trade> &trade_queue_;
  static constexpr uint32_t MAX_PRICE_SEK = 1000;
  static constexpr uint32_t TICKS_PER_SEK = 100;
  static constexpr uint32_t RAW_MAX_PRICES = MAX_PRICE_SEK * TICKS_PER_SEK; // 100 000
  static constexpr uint32_t MAX_PRICES = next_power_of_two(RAW_MAX_PRICES);
  static constexpr uint32_t BITMAP_WORDS = MAX_PRICES >> 6;
  static constexpr uint32_t MAX_ORDERS = 1U << 20; // About a million

  using bitmapword_t = uint64_t;

  static constexpr uint32_t BITMAP_WORDS_BITS = sizeof(bitmapword_t) * 8;

  uint32_t memory_pool_pop();
  void memory_pool_push(uint32_t index);
  void queue_order(uint32_t index, uint32_t price, Side side);
  void dequeue_order(uint32_t index, uint32_t price, Side side);
  uint32_t execute_order(uint64_t id, uint32_t quantity, uint32_t price, Side side);
  void update_level(uint32_t word_index, Side side,
                    std::array<bitmapword_t, BITMAP_WORDS> &active_bitmap);

  Order memory_pool_[MAX_ORDERS];
  uint32_t free_indices_[MAX_ORDERS];
  uint32_t free_top_ = MAX_ORDERS - 1;

  OrderQueue bid_curve_[MAX_PRICES];
  OrderQueue ask_curve_[MAX_PRICES];
  std::array<bitmapword_t, BITMAP_WORDS> bid_bitmap_{};
  std::array<bitmapword_t, BITMAP_WORDS> ask_bitmap_{};

  uint64_t id_transaction_ = 0;

  uint32_t highest_bid_ = 0;
  bool highest_bid_empty_ = true;

  uint32_t lowest_ask_ = 0;
  bool lowest_ask_empty_ = true;
};
