# C++ matching engine

## What?

An O(1) low latency Orderbook written in C++ with focus on basic order types and making them as fast as possible.

For results and benchmarks, please see: BENCHMARK.md

## Why?

My interest in understanding the silicon and making efficient use of it led me to write this Orderbook. Rollercoaster tycoon was written with low level control and so are trading systems. Not because it could not be written in a higher level language, but because otherwise they would not be fast enough. Throwing more compute at complex problems can be substituted by using our mind and smart use of what we have.

## How?

A packet arrives at the network layer of the exchange, gets checked for correct format and is sent to the matching engine.

An attempt is made to execute the arriving order, and if not possible, added to a memory pool.

A successfully executed order has from its arrival to its completion, entered the execution stage, found the best price by accessing a 'pointer' to the best offering, and in the case of remaining quantity, done a linear search through the active levels bitmap to find the next order to match. In dense order books, L1 cache locality and hardware prefetching allows this bitmapped approach to outperform traditional node-based O(1) structures by eliminating pointer chasing.

For each partial or full order execution,  a trade receipt is generated with the IDs of buyer and seller, transaction id, price and quantity. The receipts are not returned to the users, but handed over to another logging thread managing the public transaction book.

For each order cancellation, the packet contains a 64 bit word with the following structure:

- Upper 32 bits is the location in the memory pool.
- Lower 32 bits is the id of the order.

The id of the order is derived from the 32 lower bits of arrival order counter in the NIC.

## Structure

The instrument is traded in milliSEK (no floats) and tick size specified in the header file. Separate books are kept for the buy and sell side. The book's price levels are represented by a price-level bitmap which is synced to an Orderqueue (double linked list) of order structs. The orders live in a pre-allocated memory pool for no run-time new/delete (god forbid).

```
Global State:   [Best Bid] ──┐
                [Best Ask] ──┼─► Direct pointer to hot queue (No lookup)
                             │
Fallback Scan:  Bitmap  [0][0][1][0] ──► std::countr_zero finds next '1'
                               │
                               ▼ 1:1 Index mapping
                Array   [Lvl0][Lvl1][Lvl2][Lvl3]
                                     │
                                     ▼
                              [Price Level 2]
                              ├── first_ptr ──► [Slot 1: Order A] (Head)
                              └── last_ptr  ──► [Slot 2: Order B] (Tail)

Memory Pool Slots (Zero runtime allocations):
┌─────────────────────────────────────────────────────────────┐
│ Slot 0: [ Empty Slot ]                                      │
├─────────────────────────────────────────────────────────────┤
│ Slot 1: [Order Struct A] ◄─── (Linked via next: Slot 2)     │
├─────────────────────────────────────────────────────────────┤
│ Slot 2: [Order Struct B] ◄─── (Linked via prev: Slot 1)     │
├─────────────────────────────────────────────────────────────┤
│ Slot 3: [Order Struct C] ◄─── O(1) Cancel via direct index  │
└─────────────────────────────────────────────────────────────┘
```

## Simplifications

To limit project scope, the following assumptions have been made:

- Network layer for handling packets exists. To be implemented with kernel bypass from NIC.
- Supported order types: Add (Sell/Buy) and Cancel.
- Kernel tuning to limit jitter caused by scheduling. Core isolation (isolcpus)
- Fixed price ceiling. A high max price avoids run time allocations.
- Hard capacity limit. A high capacity  avoids run time allocations.

## Features

- Zero runtime allocations using pre-allocated memory pools with startup page warming (`memset`)
- Pure integer math in milliSEK to eliminate floating-point non-determinism
- C++20 hardware bit-scanning (`std::countr_zero`) for rapid price level discovery
- Instant O(1) cancellations using direct memory slot indexes (no hash maps)
- Lock-free SPSC ring buffer (template) for asynchronous, zero-copy trade logging (`try_emplace`)
- Branch predictor tuning using C++20 `[[likely]]` and `[[unlikely]]` attributes
- Optimized hot path data flow to minimize CPU register pressure

## Current Bottlenecks & Future Optimizations

Profiling using `perf report` under benchmark (random Add/Cancel sequences) revealed a hardware bottleneck in the matching loop:

- Spatial Locality: Orders are added to the memory_pool_ sequentially which hurts price local prefetching for matching orders and causes LLC (Last Level Cache) misses. Implementing a slab allocator would resolve this.

## License

GPLv3
