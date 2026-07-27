#include "components/data_offload/binary_writer_component.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstring>
#include "file_header.hpp"

namespace reflex {

constexpr disruptorplus::sequence_t INITIAL = static_cast<disruptorplus::sequence_t>(-1);

BinaryWriterComponent::BinaryWriterComponent(
    const BinaryWriterConfig& config,
    std::shared_ptr<disruptorplus::ring_buffer<reflex::MessageSlot>> log_buffer,
    std::shared_ptr<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>> writer_barrier,
    std::shared_ptr<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>> claim_strategy
) : BaseComponent(config)
  , log_buffer_(std::move(log_buffer))
  , writer_barrier_(std::move(writer_barrier))
  , claim_strategy_(std::move(claim_strategy))
  , base_file_path_(config.output_file_path_)
  , last_flush_(std::chrono::steady_clock::now())
  , last_stats_log_(std::chrono::steady_clock::now())
  , file_start_time_(std::chrono::steady_clock::now())
  , process_start_time_(std::chrono::system_clock::now())
  , write_buffer_(WRITE_BUFFER_SIZE)
{
    logger_->info("BinaryWriterComponent initialized with output path: {}", base_file_path_);
}

BinaryWriterComponent::~BinaryWriterComponent() {
    if (output_file_.is_open()) {
        flush_buffer();
        output_file_.close();
    }
}

void BinaryWriterComponent::on_start() {
    BaseComponent::on_start();
    ensure_file_open();
    logger_->info("BinaryWriterComponent started writing to: {}", current_file_path_);
}

void BinaryWriterComponent::on_close() {
    logger_->info("BinaryWriterComponent shutting down. Total messages written: {}, Total bytes: {}",
                  messages_written_.load(), total_bytes_written_.load());

    if (output_file_.is_open()) {
        flush_buffer();
        output_file_.close();
    }

    BaseComponent::on_close();
}

int BinaryWriterComponent::on_do_work() {
    disruptorplus::sequence_t available;
    try {
        available = claim_strategy_->wait_until_published(next_, std::chrono::nanoseconds(0));
    } catch (const std::exception& e) {
        logger_->error("Error waiting for published messages: {}", e.what());
        return 0;
    }

    if (available == INITIAL || available < next_) {
        // Check if we need to flush based on time
        auto now = std::chrono::steady_clock::now();
        if (buffer_pos_ > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count() > FLUSH_INTERVAL_MS) {
            flush_buffer();
        }

        // Log stats periodically
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_log_).count() > STATS_INTERVAL_MS) {
            log_stats();
        }

        return 0;
    }

    ensure_file_open();

    int work = 0;
    for (; next_ <= available; ++next_) {
        const MessageSlot& slot = (*log_buffer_)[next_];
        write_message_slot(slot);
        ++work;
    }

    // Signal progress
    if (work > 0) {
        writer_barrier_->publish(available);
    }

    // Check if we need to flush
    auto now = std::chrono::steady_clock::now();
    if (buffer_pos_ > WRITE_BUFFER_SIZE / 2 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count() > FLUSH_INTERVAL_MS) {
        flush_buffer();
    }

    // Check if we need to rotate the file
    rotate_file_if_needed();

    // Log stats periodically
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_log_).count() > STATS_INTERVAL_MS) {
        log_stats();
    }

    return work;
}

void BinaryWriterComponent::write_file_header() {
    if (file_header_written_) {
        return;
    }

    FileHeader header;
    header.magic_number_ = 0x52454658; // "REFX" in hex
    header.version_ = 1;
    header.created_timestamp_ = get_nano_clock().epoch_nanos();
    header.message_slot_size_ = sizeof(MessageSlot);
    std::strncpy(header.component_name_, component_name_.c_str(), sizeof(header.component_name_) - 1);
    header.component_name_[sizeof(header.component_name_) - 1] = '\0';

    // Write header to buffer
    if (buffer_pos_ + sizeof(FileHeader) > write_buffer_.size()) {
        flush_buffer();
    }

    std::memcpy(write_buffer_.data() + buffer_pos_, &header, sizeof(FileHeader));
    buffer_pos_ += sizeof(FileHeader);

    file_header_written_ = true;
    logger_->info("File header written for {}", current_file_path_);
}

void BinaryWriterComponent::write_message_slot(const MessageSlot& slot) {
    // Check if we need to flush buffer to make room
    if (buffer_pos_ + sizeof(MessageSlot) > write_buffer_.size()) {
        flush_buffer();
    }

    // Copy the message slot to buffer
    std::memcpy(write_buffer_.data() + buffer_pos_, &slot, sizeof(MessageSlot));
    buffer_pos_ += sizeof(MessageSlot);

    messages_written_++;
    record_message_type(slot.get_type());
}

void BinaryWriterComponent::flush_buffer() {
    if (buffer_pos_ == 0 || !output_file_.is_open()) {
        return;
    }

    output_file_.write(write_buffer_.data(), static_cast<std::streamsize>(buffer_pos_));
    output_file_.flush();

    // ofstream never throws unless exceptions are enabled, so failures (disk
    // full, I/O error) must be detected from the stream state explicitly.
    if (!output_file_.good()) {
        if (!write_failed_) {
            write_failed_ = true;
            component_state_ = ComponentState::FAILED;
            logger_->error("Write to {} failed (badbit={}, failbit={}) - data is being lost, marking component FAILED",
                           current_file_path_, output_file_.bad(), output_file_.fail());
        }
        // Drop the buffered data: retrying a wedged stream forever would just
        // grow the buffer without bound.
        buffer_pos_ = 0;
        return;
    }

    total_bytes_written_ += static_cast<int64_t>(buffer_pos_);
    current_file_bytes_ += static_cast<int64_t>(buffer_pos_);
    buffer_pos_ = 0;
    last_flush_ = std::chrono::steady_clock::now();
}

void BinaryWriterComponent::ensure_file_open() {
    if (!output_file_.is_open()) {
        current_file_path_ = generate_filename(base_file_path_, file_sequence_);

        // Create directory if it doesn't exist
        std::filesystem::path file_path(current_file_path_);
        std::filesystem::create_directories(file_path.parent_path());

        output_file_.open(current_file_path_, std::ios::binary | std::ios::out);
        if (!output_file_.is_open()) {
            logger_->error("Failed to open output file: {}", current_file_path_);
            throw std::runtime_error("Failed to open output file: " + current_file_path_);
        }

        file_header_written_ = false;
        file_start_time_ = std::chrono::steady_clock::now();
        write_file_header();

        logger_->info("Opened new binary file: {}", current_file_path_);
    }
}

void BinaryWriterComponent::rotate_file_if_needed() {
    if (!output_file_.is_open()) {
        return;
    }

    // Check file size
    auto current_size = current_file_bytes_.load();
    if (current_size > MAX_FILE_SIZE) {
        logger_->info("Rotating file due to size limit. Current size: {} bytes", current_size);

        flush_buffer();
        output_file_.close();

        file_sequence_++;
        current_file_bytes_ = 0;

        // Next call to ensure_file_open() will create a new file
        ensure_file_open();
    }
}

std::string BinaryWriterComponent::generate_filename(const std::string& base_path, int sequence) {
  // Use process start time instead of current time for consistent naming
  auto time_t = std::chrono::system_clock::to_time_t(process_start_time_);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      process_start_time_.time_since_epoch()).count() % 1000;

  std::stringstream ss;
  ss << base_path << "_"
     << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
     << "_" << std::setfill('0') << std::setw(3) << ms;

  if (sequence > 0) {
    ss << "_" << std::setfill('0') << std::setw(3) << sequence;
  }

  ss << ".bin";
  return ss.str();
}


void BinaryWriterComponent::log_stats() {
    const auto now = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_log_);

    if (duration.count() > 0) {
        const uint64_t total_messages = messages_written_.load();
        const uint64_t total_bytes = total_bytes_written_.load();

        const uint64_t delta_messages = total_messages - last_stats_messages_;
        const uint64_t delta_bytes = total_bytes - last_stats_bytes_;

        const double messages_per_sec = (delta_messages * 1000.0) / static_cast<double>(duration.count());
        const double mbytes_per_sec = (delta_bytes * 1000.0) / (static_cast<double>(duration.count()) * 1024.0 * 1024.0);

        // Convert to MB for consistency
        const double total_mb = total_bytes / (1024.0 * 1024.0);
        const double delta_mb = delta_bytes / (1024.0 * 1024.0);

        // Build type breakdown string
        std::ostringstream type_breakdown;
        static constexpr std::array<MessageType, 6> tracked_types{
            MessageType::L1UpdateEvent,
            MessageType::L2UpdateEvent,
            MessageType::TradeEvent,
            MessageType::MarkPriceEvent,
            MessageType::FundingRateEvent,
            MessageType::OpenInterestEvent};

        for (MessageType type : tracked_types) {
            const auto idx = static_cast<size_t>(type);
            if (idx >= MESSAGE_TYPE_BUCKETS) {
                continue;
            }

            const uint64_t total = message_type_counts_[idx];
            const uint64_t previous = message_type_counts_prev_[idx];
            const uint64_t delta = total - previous;

            if (total == 0 && delta == 0) {
                continue;
            }

            if (!type_breakdown.str().empty()) {
                type_breakdown << " ";
            }

            // Use abbreviated names
            const char* abbrev;
            switch (type) {
                case MessageType::L1UpdateEvent: abbrev = "L1"; break;
                case MessageType::L2UpdateEvent: abbrev = "L2"; break;
                case MessageType::TradeEvent: abbrev = "T"; break;
                case MessageType::MarkPriceEvent: abbrev = "MP"; break;
                case MessageType::FundingRateEvent: abbrev = "FR"; break;
                case MessageType::OpenInterestEvent: abbrev = "OI"; break;
                default: abbrev = "?"; break;
            }

            // Use fixed width for alignment
            type_breakdown << abbrev << "=" << std::setw(8) << total;
            if (delta != 0) {
                type_breakdown << "(+" << std::setw(5) << delta << ")";
            } else {
                type_breakdown << "        "; // 8 spaces to align when no delta
            }

            message_type_counts_prev_[idx] = total;
        }

        logger_->info(
            "BinaryWriter - #M: {:>8} (+{:>6}), D: {:>7.1f} MB (+{:>6.2f} MB), R: {:>4.0f} msg/s {:>4.2f} MB/s, Buff: {:>2}%, File: {} | {}",
            total_messages,
            delta_messages,
            total_mb,
            delta_mb,
            messages_per_sec,
            mbytes_per_sec,
            (buffer_pos_ * 100) / write_buffer_.size(),
            file_sequence_,
            type_breakdown.str());

        last_stats_messages_ = total_messages;
        last_stats_bytes_ = total_bytes;
        last_stats_log_ = now;
    }
}

void BinaryWriterComponent::record_message_type(MessageType type) {
    const int index = static_cast<int>(type);
    if (index >= 0 && index < static_cast<int>(MESSAGE_TYPE_BUCKETS)) {
        ++message_type_counts_[static_cast<size_t>(index)];
    } else {
        ++message_type_other_total_;
    }
}



}
