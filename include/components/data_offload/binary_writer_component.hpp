#pragma once
#include <fstream>
#include <string>
#include <chrono>
#include <atomic>
#include <vector>
#include <memory>
#include <array>

#include "framework/base_component.hpp"
#include "messages.hpp"
#include "file_header.hpp"
#include "disruptorplus/ring_buffer.hpp"
#include "disruptorplus/sequence_barrier.hpp"
#include "disruptorplus/single_threaded_claim_strategy.hpp"
#include "disruptorplus/spin_wait_strategy.hpp"

namespace reflex {

class BinaryWriterComponent : public BaseComponent {
public:
    explicit BinaryWriterComponent(
        const BinaryWriterConfig& config,
        std::shared_ptr<disruptorplus::ring_buffer<reflex::MessageSlot>> log_buffer,
        std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> writer_barrier,
        std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy
    );

    ~BinaryWriterComponent();

    int on_do_work() override;

protected:
    void on_start() override;
    void on_close() override;

private:
    void write_file_header();
    void write_message_slot(const MessageSlot& slot);
    void flush_buffer();
    void ensure_file_open();
    void rotate_file_if_needed();
    std::string generate_filename(const std::string& base_path, int sequence = 0);
    void record_message_type(MessageType type);

    std::shared_ptr<disruptorplus::ring_buffer<reflex::MessageSlot>> log_buffer_;
    std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> writer_barrier_;
    std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy_;

    disruptorplus::sequence_t next_{0};
    std::ofstream output_file_;
    std::string base_file_path_;
    std::string current_file_path_;

    // Performance tracking. total_bytes_written_ is the process-lifetime
    // total; current_file_bytes_ is the per-file rotation counter (reset on
    // each rotation).
    std::atomic<int64_t> messages_written_{0};
    std::atomic<int64_t> total_bytes_written_{0};
    std::atomic<int64_t> current_file_bytes_{0};
    std::chrono::steady_clock::time_point last_flush_;
    std::chrono::steady_clock::time_point last_stats_log_;
    std::chrono::steady_clock::time_point file_start_time_;

    // Stats tracking for rate calculation
    int64_t last_stats_messages_{0};
    int64_t last_stats_bytes_{0};

    std::chrono::system_clock::time_point process_start_time_;

    // Buffer for batch writes
    static constexpr size_t WRITE_BUFFER_SIZE = 256 * 1024; // 256KB buffer (4x larger)
    static constexpr int64_t FLUSH_INTERVAL_MS = 50;        // Flush every 50ms
    static constexpr size_t FLUSH_THRESHOLD_PCT = 50;       // Flush at 50% full
    static constexpr int64_t MAX_FILE_SIZE = 1024 * 1024 * 1024; // 1GB max file size
    static constexpr int64_t STATS_INTERVAL_MS = 5000;    // Log stats every 5 seconds

    std::vector<char> write_buffer_;
    size_t buffer_pos_{0};
    int file_sequence_{0};
    bool file_header_written_{false};
    bool write_failed_{false};  // sticky: set when the stream goes bad (disk full etc.)

    static constexpr size_t MESSAGE_TYPE_BUCKETS = 32;
    std::array<uint64_t, MESSAGE_TYPE_BUCKETS> message_type_counts_{};
    std::array<uint64_t, MESSAGE_TYPE_BUCKETS> message_type_counts_prev_{};
    uint64_t message_type_other_total_{0};

    void log_stats();
};

}
