
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include "messages.hpp"
#include "file_header.hpp"  // ADD THIS
#include "spdlog/spdlog.h"
#include "asset_info_manager.hpp"

namespace reflex {

class BinarySplitter {
public:
    // Single file constructor (backward compatibility)
    explicit BinarySplitter(const std::string& input_file);

    // Multi-file constructor - NEW!
    explicit BinarySplitter(const std::vector<std::string>& input_files);

    ~BinarySplitter() = default;

    // Split into individual files by instrument_id
    bool split_by_instrument(const std::string& output_dir = "./split_data/");

    // Get statistics about the split
    struct SplitStats {
        size_t total_events{0};
        size_t total_instruments{0};
        size_t total_input_files{0};
        std::unordered_map<uint32_t, size_t> events_per_instrument;
        std::unordered_map<uint32_t, std::vector<std::string>> instrument_files;
        std::unordered_map<uint32_t, std::string> instrument_names; // Asset names lookup
        std::vector<std::string> processed_files;
    };

    const SplitStats& get_stats() const { return stats_; }

    // Optional: Set custom filename pattern (use {name} for asset name, {id} for instrument_id)
    void set_filename_pattern(const std::string& pattern) { filename_pattern_ = pattern; }

    // Control whether to use asset names in folder structure
    void set_use_asset_names(bool use_names) { use_asset_names_ = use_names; }
    void set_max_chunk_bytes(uint64_t max_bytes);

    void set_instrument_filters(const std::vector<uint32_t>& include_ids,
                                const std::vector<uint32_t>& exclude_ids);

private:
    std::vector<std::string> input_files_;
    std::string filename_pattern_;
    bool use_asset_names_ = true;  // Default to using asset names
    SplitStats stats_;

    // Internal file writers per instrument
    std::unordered_map<uint32_t, std::unique_ptr<std::ofstream>> writers_;
    std::unordered_map<uint32_t, uint64_t> bytes_written_;
    std::unordered_map<uint32_t, uint32_t> chunk_indices_;

    std::unordered_set<uint32_t> include_filter_;
    std::unordered_set<uint32_t> exclude_filter_;
    bool has_include_filter_ = false;
    uint64_t max_chunk_bytes_ = (1ull << 30); // 1 GiB default

    bool should_process(uint32_t instrument_id) const;

    // Process a single input file
    bool process_input_file(const std::string& input_file, const std::string& output_dir);

    // CHANGE THIS METHOD - write MessageSlot instead of event
    void write_message_slot_to_instrument_file(uint32_t instrument_id,
                                              const MessageSlot& slot,
                                              const std::string& output_dir);

    void ensure_writer_capacity(uint32_t instrument_id, const std::string& output_dir);
    void open_new_chunk(uint32_t instrument_id, const std::string& output_dir);
    std::string get_output_filename(uint32_t instrument_id,
                                    const std::string& output_dir,
                                    uint32_t chunk_index) const;
    std::string get_asset_name(uint32_t instrument_id) const;
    void ensure_output_directory(const std::string& output_dir) const;
    std::unique_ptr<std::ofstream> create_writer(const std::string& filename);
    void write_file_header(std::ofstream& writer);  // ADD THIS
};

} // namespace reflex
