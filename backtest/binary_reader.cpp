#include "backtest/binary_reader.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace reflex::backtest {

BinaryReader::BinaryReader(const std::string& file_path)
    : file_path_(file_path) {
}

BinaryReader::~BinaryReader() {
    close();
}

bool BinaryReader::open() {
    if (fd_ != -1) {
        close(); // Already open, close first
    }

    // Open file for reading
    fd_ = ::open(file_path_.c_str(), O_RDONLY);
    if (fd_ == -1) {
        std::cerr << "Failed to open file: " << file_path_ << " - " << strerror(errno) << std::endl;
        return false;
    }

    // Get file size
    struct stat st;
    if (fstat(fd_, &st) == -1) {
        std::cerr << "Failed to get file size: " << file_path_ << " - " << strerror(errno) << std::endl;
        close();
        return false;
    }
    file_size_ = st.st_size;

    // Memory map the file
    if (!memory_map_file()) {
        close();
        return false;
    }

    // Read and validate header
    if (file_size_ < sizeof(reflex::FileHeader)) {
        std::cerr << "File too small for header: " << file_path_ << std::endl;
        close();
        return false;
    }

    const reflex::FileHeader* file_header = static_cast<const reflex::FileHeader*>(mapped_memory_);

    // Validate magic number (0x52454658 = "REFX")
    if (file_header->magic_number_ != 0x52454658) {
        std::cerr << "Invalid magic number " << std::hex << file_header->magic_number_
                  << " in file: " << file_path_ << std::endl;
        close();
        return false;
    }

    // Validate version
    if (file_header->version_ != 1) {
        std::cerr << "Unsupported file version " << file_header->version_
                  << " in file: " << file_path_ << std::endl;
        close();
        return false;
    }

    // Set up data pointers
    data_start_ = static_cast<const char*>(mapped_memory_) + sizeof(reflex::FileHeader);

    // Calculate total messages based on remaining file size
    size_t data_size = file_size_ - sizeof(reflex::FileHeader);
    if (file_header->message_slot_size_ == 0 || file_header->message_slot_size_ != sizeof(MessageSlot)) {
        std::cerr << "Invalid message slot size " << file_header->message_slot_size_
                  << " (expected " << sizeof(MessageSlot) << ") in file: " << file_path_ << std::endl;
        close();
        return false;
    }

    total_messages_ = data_size / file_header->message_slot_size_;
    current_position_ = 0;

    // Initialise pointer‐cursor bounds
    cursor_   = data_start_;
    data_end_ = data_start_ + total_messages_ * sizeof(MessageSlot);

    return true;
}

void BinaryReader::close() {
    unmap_file();

    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }

    file_size_ = 0;
    data_start_ = nullptr;
    cursor_ = data_end_ = nullptr;
    total_messages_ = 0;
    current_position_ = 0;
}

const MessageSlot* BinaryReader::read_next_message() {
    if (is_end_of_file() || !cursor_) return nullptr;

    __builtin_prefetch(cursor_ + 2 * sizeof(MessageSlot), 0, 1);

    const MessageSlot* slot = reinterpret_cast<const MessageSlot*>(cursor_);
    cursor_ += sizeof(MessageSlot);
    ++current_position_;            // keep stats

    return slot;
}

const reflex::FileHeader& BinaryReader::get_header() const {
    if (!mapped_memory_) {
        throw std::logic_error("BinaryReader::get_header() called before open(): " + file_path_);
    }
    return *static_cast<const reflex::FileHeader*>(mapped_memory_);
}

bool BinaryReader::memory_map_file() {
    mapped_memory_ = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_memory_ == MAP_FAILED) {
        std::cerr << "Failed to mmap file: " << file_path_ << " - " << strerror(errno) << std::endl;
        mapped_memory_ = nullptr;
        return false;
    }

    // Advise kernel about access pattern
    if (madvise(mapped_memory_, file_size_, MADV_SEQUENTIAL) == -1) {
        // Not fatal, just a performance hint
        std::cerr << "Warning: madvise failed for file: " << file_path_ << std::endl;
    }

    return true;
}

void BinaryReader::unmap_file() {
    if (mapped_memory_ && mapped_memory_ != MAP_FAILED) {
        munmap(mapped_memory_, file_size_);
        mapped_memory_ = nullptr;
    }
}

// MultiFileBinaryReader implementation
MultiFileBinaryReader::MultiFileBinaryReader(std::vector<std::string> file_paths,
                                             bool merge_by_timestamp)
    : file_paths_(std::move(file_paths)),
      merge_by_timestamp_(merge_by_timestamp) {
    if (merge_by_timestamp_) {
        merged_readers_.reserve(file_paths_.size());
        for (const auto& path : file_paths_) {
            auto reader = std::make_unique<BinaryReader>(path);
            if (!reader->open()) {
                std::cerr << "Failed to open file in timestamp-merged reader: " << path << std::endl;
                continue;
            }
            if (!reader->is_end_of_file()) {
                merged_readers_.push_back(std::move(reader));
            }
        }
    } else {
        open_current_reader();
    }
}

bool MultiFileBinaryReader::open_current_reader() {
    if (current_reader_) {
        return true;
    }

    while (current_reader_index_ < file_paths_.size()) {
        auto reader = std::make_unique<BinaryReader>(file_paths_[current_reader_index_]);
        if (!reader->open()) {
            std::cerr << "Failed to open file in multi-reader: " << file_paths_[current_reader_index_] << std::endl;
            ++current_reader_index_;
            continue;
        }
        // Skip empty files immediately so peek() always has a timestamp
        if (reader->is_end_of_file()) {
            reader->close();
            ++current_reader_index_;
            continue;
        }

        current_reader_ = std::move(reader);
        return true;
    }

    return false;
}

void MultiFileBinaryReader::close_current_reader() {
    if (current_reader_) {
        current_reader_->close();
        current_reader_.reset();
    }
}

void MultiFileBinaryReader::advance_to_next_file() {
    close_current_reader();
    ++current_reader_index_;
    open_current_reader();
}

const MessageSlot* MultiFileBinaryReader::read_next_message() {
    if (merge_by_timestamp_) {
        BinaryReader* selected = nullptr;
        int64_t earliest = INT64_MAX;
        for (const auto& reader : merged_readers_) {
            const int64_t timestamp = reader->peek_next_timestamp();
            if (timestamp < earliest) {
                earliest = timestamp;
                selected = reader.get();
            }
        }
        return selected == nullptr ? nullptr : selected->read_next_message();
    }
    while (open_current_reader()) {
        if (const MessageSlot* slot = current_reader_->read_next_message()) {
            return slot;
        }
        advance_to_next_file();
    }

    return nullptr; // All files exhausted
}

uint64_t MultiFileBinaryReader::total_messages() const {
    if (total_messages_cached_) {
        return cached_total_messages_;
    }

    uint64_t total = 0;
    for (const auto& path : file_paths_) {
        BinaryReader reader(path);
        if (reader.open()) {
            total += reader.total_messages();
        } else {
            std::cerr << "Failed to open file in multi-reader total count: " << path << std::endl;
        }
    }

    cached_total_messages_ = total;
    total_messages_cached_ = true;
    return total;
}

bool MultiFileBinaryReader::is_end_of_files() {
    return !has_more_data();
}

bool MultiFileBinaryReader::has_more_data() {
    if (merge_by_timestamp_) {
        return std::any_of(merged_readers_.begin(), merged_readers_.end(),
                           [](const auto& reader) { return !reader->is_end_of_file(); });
    }
    // Skip past exhausted files eagerly: open_current_reader() already discards
    // files that fail to open or contain no messages, so "true" here guarantees
    // read_next_message() will produce a slot. (The old implementation counted
    // remaining paths without opening them, which reported phantom data when the
    // trailing files were unreadable or header-only.)
    while (open_current_reader()) {
        if (!current_reader_->is_end_of_file()) {
            return true;
        }
        advance_to_next_file();
    }
    return false;
}

int64_t MultiFileBinaryReader::peek_next_timestamp() {
    if (merge_by_timestamp_) {
        int64_t earliest = INT64_MAX;
        for (const auto& reader : merged_readers_) {
            earliest = std::min(earliest, reader->peek_next_timestamp());
        }
        return earliest;
    }
    while (open_current_reader()) {
        const int64_t ts = current_reader_->peek_next_timestamp();
        if (ts != INT64_MAX) {
            return ts;
        }
        // Current file exhausted but caller hasn't consumed the EOF yet.
        advance_to_next_file();
    }
    return INT64_MAX;  // all files exhausted
}

const std::string& MultiFileBinaryReader::current_file() const {
    static const std::string kNone;
    return current_reader_index_ < file_paths_.size() ? file_paths_[current_reader_index_] : kNone;
}

} // namespace reflex::backtest
