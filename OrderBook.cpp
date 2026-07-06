#include "OrderBook.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sys/types.h>

OrderBook::OrderBook(SpscRingBuffer<Trade> &queue) : trade_queue_(queue) {
  // Linux kernel uses demand paging. Touch the addresses.
  std::memset(memory_pool_, 0, sizeof(memory_pool_));
  std::memset(free_indices_, 0, sizeof(free_indices_));

  for (int i = 0; i < MAX_ORDERS; i++) {
    free_indices_[i] = i;
  }
}

uint32_t OrderBook::memory_pool_pop() {
  assert(free_top_ < MAX_ORDERS);
  return free_indices_[free_top_--];
}

void OrderBook::memory_pool_push(uint32_t index) {
  assert(free_top_ + 1 < MAX_ORDERS);
  free_indices_[++free_top_] = index;
}

void OrderBook::queue_order(uint32_t index, uint32_t price, Side side) {
  OrderQueue &queue = (side == BUY) ? bid_curve_[price] : ask_curve_[price];
  auto &active_bitmap = (side == BUY) ? bid_bitmap_ : ask_bitmap_;

  if (queue.first == NULL_INDEX) [[unlikely]] {
    /* No in queue */
    if (side == BUY && (highest_bid_empty_ || price > highest_bid_)) {
      highest_bid_ = price;
      highest_bid_empty_ = false;
    } else if (side == SELL && (lowest_ask_empty_ || price < lowest_ask_)) {
      lowest_ask_ = price;
      lowest_ask_empty_ = false;
    }
    queue.first = index;
    queue.last = index;

    uint32_t word_index = price >> 6; // 2^6 = 64
    uint32_t bit_index = price & 63;  // %64
    active_bitmap[word_index] |= (static_cast<uint64_t>(1) << bit_index);

    /*
    #if (side == BUY && price > highest_bid_)
      highest_bid_ = price;
    else if (side == SELL && price < lowest_ask_)
      lowest_ask_ = price;
    */
    return;
  }

  uint32_t temp_index = queue.last;
  queue.last = index;
  memory_pool_[temp_index].next = index;
  memory_pool_[index].prev = temp_index;
}

void OrderBook::update_level(uint32_t word_index, Side side,
                             std::array<bitmapword_t, BITMAP_WORDS> &active_bitmap) {
  // incoming BUY order consumes ASK side (liquidity taker)
  // incoming SELL order consumes BID side (liquidity taker)
  // this function updates best price of the *opposite book side*
  if (side == SELL) {
    // side ate final order on ask -> lowest_ask_ is invalid
    while (word_index < BITMAP_WORDS && active_bitmap[word_index] == 0) {
      word_index++;
    }
    if (word_index < BITMAP_WORDS) {
      // there is a 1 here somewhere. find it, and update
      lowest_ask_ = word_index * BITMAP_WORDS_BITS + std::countr_zero(active_bitmap[word_index]);
    } else {
      lowest_ask_empty_ = true;
    }
  } else {
    // side ate final order highest_bid_ -> highest_bid_ is invalid
    while (word_index > 0 && active_bitmap[word_index] == 0) {
      word_index--;
    }
    if (word_index > 0 || active_bitmap[word_index] != 0) {
      // there is a 1 here somewhere. find it, and update
      highest_bid_ = word_index * BITMAP_WORDS_BITS +
                     (BITMAP_WORDS_BITS - 1 - std::countl_zero(active_bitmap[word_index]));
    } else {
      highest_bid_empty_ = true;
    }
  }
}

void OrderBook::dequeue_order(uint32_t index, uint32_t price, Side side) {
  // Occupy one less cpu register by not sending reference
  Order &order = memory_pool_[index];

  if (order.next != NULL_INDEX && order.prev != NULL_INDEX) [[likely]] {
    memory_pool_[order.prev].next = order.next;
    memory_pool_[order.next].prev = order.prev;
    order.id = 0;
    memory_pool_push(index);
    return;
  }

  OrderQueue &queue = (side == BUY) ? bid_curve_[price] : ask_curve_[price];
  if (queue.first == index && queue.last == index) {
    auto &bitmap = (side == BUY) ? bid_bitmap_ : ask_bitmap_;

    uint32_t word_index = price >> 6; // 2^6 = 64
    uint32_t bit_index = price & 63;  // %64
    bitmap[word_index] &= ~(static_cast<uint64_t>(1) << bit_index);
    // Update lowest_ask_ or highest_bid_
    update_level(word_index, side, bitmap);
  }

  if (queue.first == index) {
    queue.first = order.next;
    if (order.next != NULL_INDEX)
      memory_pool_[order.next].prev = NULL_INDEX;
  }
  if (queue.last == index) {
    queue.last = order.prev;
    if (order.prev != NULL_INDEX)
      memory_pool_[order.prev].next = NULL_INDEX;
  }

  order.id = 0;
  memory_pool_push(index);
}

uint32_t OrderBook::execute_order(uint64_t id, uint32_t quantity, uint32_t price, Side side) {

  if (side == BUY && (lowest_ask_empty_ || price < lowest_ask_))
    return quantity;
  if (side == SELL && (highest_bid_empty_ || price > highest_bid_))
    return quantity;

  // We want bitmap of the opposite side to match.
  auto &bitmap = (side != BUY) ? bid_bitmap_ : ask_bitmap_;
  auto &curve = (side != BUY) ? bid_curve_ : ask_curve_;
  uint32_t remaining_qty = quantity;
  // Bid äsk, köp och sälj. Ask for it. Bidding auction
  if (side == BUY) {

    while (remaining_qty > 0 && !lowest_ask_empty_ && lowest_ask_ <= price) {

      Order &match = memory_pool_[curve[lowest_ask_].first];
      Trade receipt;
      bool success;
      if (match.quantity <= remaining_qty) {
        success = trade_queue_.try_emplace(static_cast<uint32_t>(id), // 1. id_buyer
                                           match.id,                  // 2. id_seller
                                           ++id_transaction_,         // 3. id_transaction
                                           lowest_ask_,               // 4. price
                                           match.quantity             // 5. quantity
        );
        remaining_qty -= match.quantity;
        dequeue_order(curve[lowest_ask_].first, lowest_ask_, SELL);
      } else {
        success = trade_queue_.try_emplace(static_cast<uint32_t>(id), // 1. id_buyer
                                           match.id,                  // 2. id_seller
                                           ++id_transaction_,         // 3. id_transaction
                                           lowest_ask_,               // 4. price
                                           remaining_qty              // 5. quantity
        );
        match.quantity -= remaining_qty;
        remaining_qty = 0;
      }
      assert(success && "Queue full, may never happen in prod.");
      // trade_queue_.try_push(std::move(receipt));
    }
  } else {

    while (remaining_qty > 0 && !highest_bid_empty_ && highest_bid_ >= price) {
      Order &match = memory_pool_[curve[highest_bid_].first];
      Trade receipt;
      bool success;
      if (match.quantity <= remaining_qty) {
        success = trade_queue_.try_emplace(match.id,                  // 1. id_buyer
                                           static_cast<uint32_t>(id), // 2. id_seller
                                           ++id_transaction_,         // 3. id_transaction
                                           highest_bid_,              // 4. price
                                           match.quantity             // 5. quantity
        );

        remaining_qty -= match.quantity;
        dequeue_order(curve[highest_bid_].first, highest_bid_, BUY);
      } else {
        success = trade_queue_.try_emplace(match.id,                  // 1. id_buyer
                                           static_cast<uint32_t>(id), // 2. id_seller
                                           ++id_transaction_,         // 3. id_transaction
                                           highest_bid_,              // 4. price
                                           remaining_qty              // 5. quantity
        );
        match.quantity -= remaining_qty;
        remaining_qty = 0;
      }
      assert(success && "Queue full, may never happen in prod.");
      // trade_queue_.try_push(std::move(receipt));
    }
  }
  return remaining_qty;
}
// make this uint64_t, should return id_nic and id_index, not really, should write result to
// buffer for other thread.
uint64_t OrderBook::add_order(uint64_t id_nic, uint32_t quantity, uint32_t price, Side side) {
  uint32_t remaining_qty = execute_order(id_nic, quantity, price, side);

  if (remaining_qty > 0) {

    uint32_t index = memory_pool_pop();
    uint32_t lower = static_cast<uint32_t>(id_nic); // Cut off upper half of uint64_t id_nic
    Order incoming = {.id = lower,
                      .quantity = remaining_qty,
                      .price = price,
                      .next = NULL_INDEX,
                      .prev = NULL_INDEX};

    memory_pool_[index] = incoming;

    queue_order(index, price, side);

    return (id_nic & 0xFFFFFFFFULL) | (static_cast<uint64_t>(index) << 32);
  }
  return NULL_INDEX;
}

bool OrderBook::cancel_order(uint64_t id_user, uint32_t price, Side side) {
  const uint32_t index = static_cast<uint32_t>(id_user >> 32);
  Order &order = memory_pool_[index];
  // Check valid order. Weak check, vulnearable to reverse engineering.
  if (order.id == 0 || order.id != (id_user & std::numeric_limits<uint32_t>::max()))
    return false;
  dequeue_order(index, price, side);
  return true;
}
