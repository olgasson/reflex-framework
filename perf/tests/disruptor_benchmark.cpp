#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <iomanip>
#include <functional>
#include <algorithm>
#include <cmath>
#include <numeric>

#include <Disruptor/RingBuffer.h>
#include <Disruptor/BatchEventProcessor.h>

#include "Disruptor/BusySpinWaitStrategy.h"

#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/single_threaded_claim_strategy.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
void pin_thread_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_t current_thread = pthread_self();
  pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}
#else
void pin_thread_to_core(int) {
}
#endif

inline void cpu_relax() {
}

struct alignas(64)IntEvent {
  uint32_t value = 0;
};

class IntEventHandler : public Disruptor::IEventHandler<IntEvent> {
private:
  std::atomic<uint64_t>& counter_;

public:
  explicit IntEventHandler(std::atomic<uint64_t>& counter) : counter_(counter) {}

  void onEvent(IntEvent& event, std::int64_t sequence, bool endOfBatch) {
    counter_.fetch_add(event.value, std::memory_order_relaxed);
  }
};

class SimpleBenchmark {
public:
    void runAllBenchmarks() {
        std::cout << "======= DISRUPTOR LIBRARY COMPARISON BENCHMARK =======" << std::endl;
        std::cout << "Running on " << std::thread::hardware_concurrency() << " logical cores" << std::endl;

        const int iterations = 100000000;
        const int BUFFER_SIZE = 1024 * 64;
        const uint64_t EXPECTED_SUM = static_cast<uint64_t>(iterations) * (iterations + 1) / 2;
        const int NUM_RUNS = 1;

        std::cout << "Buffer size: " << BUFFER_SIZE << " entries" << std::endl;
        std::cout << "Operations per test: " << iterations << std::endl;
        std::cout << "Expected sum: " << EXPECTED_SUM << std::endl;
        std::cout << "Number of runs per test: " << NUM_RUNS << std::endl;
        std::cout << "Disruptor event size: " << sizeof(IntEvent) << " bytes" << std::endl;
        std::cout << "DisruptorPlus event size: " << sizeof(uint32_t) << " bytes" << std::endl;
        std::cout << "Disruptor buffer memory: " << (BUFFER_SIZE * sizeof(IntEvent)) << " bytes" << std::endl;
        std::cout << "DisruptorPlus buffer memory: " << (BUFFER_SIZE * sizeof(uint32_t)) << " bytes" << std::endl;
        std::cout << std::endl;

        std::vector<double> disruptor_times;
        std::vector<double> disruptorplus_times;

        for (int run = 1; run <= NUM_RUNS; ++run) {
            std::cout << "=== RUN " << run << " ===" << std::endl;

            double disruptor_time = runDisruptorSPSC(iterations, BUFFER_SIZE, EXPECTED_SUM);
            disruptor_times.push_back(disruptor_time);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            double disruptorplus_time = runDisruptorPlusSPSC(iterations, BUFFER_SIZE, EXPECTED_SUM);
            disruptorplus_times.push_back(disruptorplus_time);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout << std::endl;
        }

        printStatisticalSummary("DISRUPTOR", disruptor_times, iterations);
        printStatisticalSummary("DISRUPTORPLUS", disruptorplus_times, iterations);

        std::cout << "============ COMPLETE =============" << std::endl;
    }

private:
    double runDisruptorSPSC(int iterations, int buffer_size, uint64_t expected_sum) {
        std::cout << "DISRUPTOR SPSC:" << std::endl;

        std::atomic<uint64_t> counter{0};

        auto waitStrategy = std::make_shared<Disruptor::BusySpinWaitStrategy>();
        auto ringBuffer = Disruptor::RingBuffer<IntEvent>::createSingleProducer(
            []() { return IntEvent(); }, buffer_size, waitStrategy);

        auto sequenceBarrier = ringBuffer->newBarrier();
        auto handler = std::make_shared<IntEventHandler>(counter);
        auto processor = std::make_shared<Disruptor::BatchEventProcessor<IntEvent>>(
            ringBuffer, sequenceBarrier, handler);

        ringBuffer->addGatingSequences({processor->sequence()});

        std::thread consumerThread([&processor]() {
            processor->run();
        });

        auto start = std::chrono::high_resolution_clock::now();

        for (uint32_t i = 1; i <= static_cast<uint32_t>(iterations); ++i) {
            auto seq = ringBuffer->next();
            (*ringBuffer)[seq].value = i;
            ringBuffer->publish(seq);
        }

        while (counter.load(std::memory_order_relaxed) < expected_sum) {
            std::this_thread::yield();
        }

        processor->halt();
        consumerThread.join();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;

        printResults("DISRUPTOR", iterations, elapsed.count(), counter.load(), expected_sum);
        return elapsed.count();
    }


    double runDisruptorPlusSPSC(int iterations, int buffer_size, uint64_t expected_sum) {
      std::cout << "DISRUPTOR PLUS:" << std::endl;
      const size_t bufferSize = buffer_size;

      disruptorplus::ring_buffer<IntEvent> buffer(bufferSize);

      disruptorplus::spin_wait_strategy waitStrategy;
      disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy> claimStrategy(bufferSize, waitStrategy);
      disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy> consumed(waitStrategy);
      claimStrategy.add_claim_barrier(consumed);

      uint64_t sum = 0;
      std::thread consumer([&]()
      {

          disruptorplus::sequence_t nextToRead = 0;
          bool done = false;
          while (!done)
          {
              disruptorplus::sequence_t available = claimStrategy.wait_until_published(nextToRead);

              do
              {
                  auto& event = buffer[nextToRead];
                  sum += event.value;
                  if (event.value == 0)
                  {
                      done = true;
                  }
              } while (nextToRead++ != available);

              consumed.publish(available);
          }
      });

      auto start = std::chrono::high_resolution_clock::now();

      std::thread producer([&]()
      {
          for (uint32_t i = 1; i <= iterations; ++i)
          {
              disruptorplus::sequence_t seq = claimStrategy.claim_one();

              buffer[seq].value = i;

              claimStrategy.publish(seq);
          }

          disruptorplus::sequence_t seq = claimStrategy.claim_one();
          buffer[seq].value = 0;
          claimStrategy.publish(seq);
      });

      consumer.join();
      producer.join();

      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = end - start;
      printResults("DISRUPTOR PLUS", iterations, elapsed.count(), sum, expected_sum);
      return elapsed.count();
}

    void printResults(const std::string& library, int iterations, double elapsedSeconds,
                     uint64_t actual_sum, uint64_t expected_sum) {
        bool verification_passed = (actual_sum == expected_sum);

        std::cout << "Library: " << library << std::endl;
        std::cout << "Operations: " << iterations << std::endl;
        std::cout << "Time: " << std::fixed << std::setprecision(3) << elapsedSeconds << " seconds" << std::endl;
        std::cout << "Throughput: " << std::fixed << std::setprecision(2)
                  << (iterations / elapsedSeconds / 1e6) << " million ops/second" << std::endl;
        std::cout << "Expected sum: " << expected_sum << std::endl;
        std::cout << "Actual sum: " << actual_sum << std::endl;
        std::cout << "Verification: " << (verification_passed ? "PASS" : "FAIL") << std::endl;

        if (!verification_passed) {
            std::cout << "ERROR: Sum mismatch detected!" << std::endl;
        }
        std::cout << std::endl;
    }

    void printStatisticalSummary(const std::string& library, const std::vector<double>& times, int iterations) {
        if (times.empty()) return;

        std::vector<double> sorted_times = times;
        std::sort(sorted_times.begin(), sorted_times.end());

        double min_time = sorted_times.front();
        double max_time = sorted_times.back();
        double median_time = sorted_times[sorted_times.size() / 2];
        double avg_time = std::accumulate(sorted_times.begin(), sorted_times.end(), 0.0) / sorted_times.size();

        double variance = 0.0;
        for (double time : sorted_times) {
            variance += (time - avg_time) * (time - avg_time);
        }
        variance /= sorted_times.size();
        double std_dev = std::sqrt(variance);

        std::cout << "=== " << library << " STATISTICAL SUMMARY ===" << std::endl;
        std::cout << "Runs: " << times.size() << std::endl;
        std::cout << "Min time: " << std::fixed << std::setprecision(3) << min_time << " s" << std::endl;
        std::cout << "Max time: " << std::fixed << std::setprecision(3) << max_time << " s" << std::endl;
        std::cout << "Median time: " << std::fixed << std::setprecision(3) << median_time << " s" << std::endl;
        std::cout << "Average time: " << std::fixed << std::setprecision(3) << avg_time << " s" << std::endl;
        std::cout << "Std deviation: " << std::fixed << std::setprecision(3) << std_dev << " s" << std::endl;
        std::cout << "Min throughput: " << std::fixed << std::setprecision(2)
                  << (iterations / max_time / 1e6) << " million ops/sec" << std::endl;
        std::cout << "Max throughput: " << std::fixed << std::setprecision(2)
                  << (iterations / min_time / 1e6) << " million ops/sec" << std::endl;
        std::cout << "Median throughput: " << std::fixed << std::setprecision(2)
                  << (iterations / median_time / 1e6) << " million ops/sec" << std::endl;
        std::cout << "Average throughput: " << std::fixed << std::setprecision(2)
                  << (iterations / avg_time / 1e6) << " million ops/sec" << std::endl;
        std::cout << std::endl;
    }
};

int main() {
    SimpleBenchmark benchmark;
    benchmark.runAllBenchmarks();
    return 0;
}
