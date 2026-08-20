/** @file
 *  @brief The hand-over between the reader and the parsers.
 *
 *  One thread fills, several empty, and the queue is what keeps them from
 *  treading on each other.  So the tests are threaded where the contract is:
 *  a pop waits for a push, and closing releases everyone still waiting.
 */

#include "c_api.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace {

/** A batch carrying nothing but a recognisable length. */
ParseBatch Batch(size_t len) {
  ParseBatch batch{};
  batch.buffer = nullptr;
  batch.len = len;
  return batch;
}

class ParseQueueTest : public ::testing::Test {
protected:
  void SetUp() override {
    queue = parse_queue_create();
    ASSERT_NE(queue, nullptr);
  }
  void TearDown() override {
    parse_queue_destroy(queue);
  }
  ParseQueue *queue = nullptr;
};

} // namespace

TEST_F(ParseQueueTest, WhatWasPushedComesBack) {
  parse_queue_push(queue, Batch(42));
  ParseBatch got{};
  EXPECT_EQ(parse_queue_pop(queue, &got), 1);
  EXPECT_EQ(got.len, 42u);
}

TEST_F(ParseQueueTest, KeepsTheOrderItWasGiven) {
  for (size_t i = 1; i <= PARSE_QUEUE_CAPACITY; ++i) parse_queue_push(queue, Batch(i));
  for (size_t i = 1; i <= PARSE_QUEUE_CAPACITY; ++i) {
    ParseBatch got{};
    ASSERT_EQ(parse_queue_pop(queue, &got), 1);
    EXPECT_EQ(got.len, i);
  }
}

TEST_F(ParseQueueTest, ClosingEndsThePopping) {
  parse_queue_push(queue, Batch(7));
  parse_queue_close(queue);

  ParseBatch got{};
  EXPECT_EQ(parse_queue_pop(queue, &got), 1) << "what was already in it still comes out";
  EXPECT_EQ(got.len, 7u);
  EXPECT_EQ(parse_queue_pop(queue, &got), 0) << "and then the queue says it is done";
  EXPECT_EQ(parse_queue_pop(queue, &got), 0) << "and keeps saying it";
}

TEST_F(ParseQueueTest, APopWaitsForItsPush) {
  std::atomic<bool> got_it{false};
  std::thread consumer([&] {
    ParseBatch batch{};
    if (parse_queue_pop(queue, &batch) == 1 && batch.len == 99) got_it = true;
  });

  // the consumer is now asleep on an empty queue
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(got_it) << "nothing was pushed yet, so nothing may have arrived";

  parse_queue_push(queue, Batch(99));
  consumer.join();
  EXPECT_TRUE(got_it);
}

TEST_F(ParseQueueTest, ClosingWakesAWaitingConsumer) {
  std::atomic<bool> finished{false};
  std::thread consumer([&] {
    ParseBatch batch{};
    parse_queue_pop(queue, &batch);
    finished = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(finished);

  parse_queue_close(queue);
  consumer.join();
  EXPECT_TRUE(finished) << "a closed queue must not leave anyone asleep";
}

TEST_F(ParseQueueTest, APushWaitsForRoom) {
  for (size_t i = 0; i < PARSE_QUEUE_CAPACITY; ++i) parse_queue_push(queue, Batch(i));

  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    parse_queue_push(queue, Batch(1000));
    pushed = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(pushed) << "the queue is full, so the producer waits";

  ParseBatch got{};
  parse_queue_pop(queue, &got);
  producer.join();
  EXPECT_TRUE(pushed);
}

TEST_F(ParseQueueTest, EveryBatchReachesExactlyOneConsumer) {
  constexpr size_t kBatches = 500;
  constexpr int kConsumers = 4;

  std::vector<std::vector<size_t>> taken(kConsumers);
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&, c] {
      ParseBatch batch{};
      while (parse_queue_pop(queue, &batch) == 1) taken[c].push_back(batch.len);
    });
  }

  for (size_t i = 0; i < kBatches; ++i) parse_queue_push(queue, Batch(i));
  parse_queue_close(queue);
  for (std::thread &t : consumers) t.join();

  std::set<size_t> all;
  size_t count = 0;
  for (const std::vector<size_t> &one : taken) {
    count += one.size();
    for (size_t len : one) all.insert(len);
  }
  EXPECT_EQ(count, kBatches) << "none lost, none doubled";
  EXPECT_EQ(all.size(), kBatches);
}

TEST(ParseQueueLifetime, DestroyingNullIsSafe) {
  parse_queue_destroy(nullptr);
}

TEST(ParseQueueLifetime, AnUntouchedQueueCanBeClosedAndDestroyed) {
  ParseQueue *queue = parse_queue_create();
  ASSERT_NE(queue, nullptr);
  parse_queue_close(queue);
  ParseBatch batch{};
  EXPECT_EQ(parse_queue_pop(queue, &batch), 0);
  parse_queue_destroy(queue);
}
