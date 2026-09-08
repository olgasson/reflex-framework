
#include "backtest/binary_splitter.hpp"
#include "backtest/binary_reader.hpp"
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <sstream>
#include "spdlog/spdlog.h"

namespace reflex {

BinarySplitter::BinarySplitter(const std::string& input_file)
    : input_files_{input_file}
    , filename_pattern_("{name}_data.bin") {
}

BinarySplitter::BinarySplitter(const std::vector<std::string>& input_files)
    : input_files_(input_files)
    , filename_pattern_("{name}_data.bin") {
}

void BinarySplitter::set_max_chunk_bytes(uint64_t max_bytes) {
    const uint64_t minimum = sizeof(FileHeader) + sizeof(MessageSlot);
    if (max_bytes < minimum) {
        spdlog::warn("Requested chunk size {} is too small; using minimum {}", max_bytes, minimum);
        max_chunk_bytes_ = minimum;
    } else {
        max_chunk_bytes_ = max_bytes;
    }
}

void BinarySplitter::set_instrument_filters(const std::vector<uint32_t>& include_ids,
                                            const std::vector<uint32_t>& exclude_ids) {
    include_filter_.clear();
    exclude_filter_.clear();

    if (!include_ids.empty()) {
        include_filter_.insert(include_ids.begin(), include_ids.end());
        has_include_filter_ = true;
    } else {
        has_include_filter_ = false;
    }

    if (!exclude_ids.empty()) {
        exclude_filter_.insert(exclude_ids.begin(), exclude_ids.end());
    }
}

bool BinarySplitter::should_process(uint32_t instrument_id) const {
    if (has_include_filter_ && include_filter_.find(instrument_id) == include_filter_.end()) {
        return false;
    }
    if (!exclude_filter_.empty() && exclude_filter_.find(instrument_id) != exclude_filter_.end()) {
        return false;
    }
    return true;
}

bool BinarySplitter::split_by_instrument(const std::string& output_dir) {
    spdlog::info("Starting split of {} files into directory {}",
                 input_files_.size(), output_dir);

    if (!AssetInfoManager::is_initialized()) {
        spdlog::warn("AssetInfoManager not initialized, calling initialize()");
        AssetInfoManager::initialize();
    }

    ensure_output_directory(output_dir);

    stats_ = SplitStats{};
    writers_.clear();
    bytes_written_.clear();
    chunk_indices_.clear();
    stats_.total_input_files = input_files_.size();

    for (const auto& input_file : input_files_) {
        spdlog::info("Processing file: {}", input_file);

        if (!process_input_file(input_file, output_dir)) {
            spdlog::error("Failed to process file: {}", input_file);
            return false;
        }

        stats_.processed_files.push_back(input_file);
        spdlog::info("Completed file: {} (Total events so far: {})",
                     input_file, stats_.total_events);
    }

    writers_.clear();
    bytes_written_.clear();

    stats_.total_instruments = stats_.events_per_instrument.size();

    spdlog::info("Split complete: {} events from {} files across {} instruments",
                 stats_.total_events, stats_.total_input_files, stats_.total_instruments);

    for (const auto& [inst_id, count] : stats_.events_per_instrument) {
        std::string asset_name = stats_.instrument_names.count(inst_id) ?
                                stats_.instrument_names.at(inst_id) :
                                get_asset_name(inst_id);
        const auto& files = stats_.instrument_files[inst_id];
        if (!files.empty()) {
            spdlog::info("  {} (ID: {}): {} events -> {} chunk(s) (first: {})",
                         asset_name, inst_id, count, files.size(), files.front());
        } else {
            spdlog::info("  {} (ID: {}): {} events -> no output files?", asset_name, inst_id, count);
        }
    }

    return true;
}

bool BinarySplitter::process_input_file(const std::string& input_file, const std::string& output_dir) {
    reflex::backtest::BinaryReader reader(input_file);
    if (!reader.open()) {
        spdlog::error("Failed to open input file: {}", input_file);
        return false;
    }

    size_t events_from_this_file = 0;

    while (const MessageSlot* slot = reader.read_next_message()) {
        uint32_t instrument_id = 0;

        switch (slot->get_type()) {
            case reflex::MessageType::L1UpdateEvent: {
                const auto& event = slot->as<L1UpdateEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::L2UpdateEvent: {
                const auto& event = slot->as<L2UpdateEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::TradeEvent: {
                const auto& event = slot->as<TradeEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::MarkPriceEvent: {
                const auto& event = slot->as<MarkPriceEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::FundingRateEvent: {
                const auto& event = slot->as<FundingRateEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::OpenInterestEvent: {
                const auto& event = slot->as<OpenInterestEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            case reflex::MessageType::LiquidationEvent: {
                const auto& event = slot->as<LiquidationEvent>();
                instrument_id = event.instrument_id_;
                break;
            }
            default:
                spdlog::debug("Skipping unsupported message type: {}",
                             static_cast<int>(slot->get_type()));
                continue;
        }

        if (!should_process(instrument_id)) {
            continue;
        }

        write_message_slot_to_instrument_file(instrument_id, *slot, output_dir);

        stats_.total_events++;
        events_from_this_file++;
    }

    reader.close();
    spdlog::debug("File {} contributed {} events", input_file, events_from_this_file);
    return true;
}

void BinarySplitter::write_message_slot_to_instrument_file(uint32_t instrument_id,
                                                          const MessageSlot& slot,
                                                          const std::string& output_dir) {
    ensure_writer_capacity(instrument_id, output_dir);

    auto it = writers_.find(instrument_id);
    if (it == writers_.end() || !it->second) {
        spdlog::error("Writer unavailable for instrument {}", instrument_id);
        return;
    }

    it->second->write(reinterpret_cast<const char*>(&slot), sizeof(MessageSlot));
    if (!it->second->good()) {
        spdlog::error("Failed to write MessageSlot to file for instrument {}", instrument_id);
        return;
    }

    bytes_written_[instrument_id] += sizeof(MessageSlot);

    stats_.events_per_instrument[instrument_id]++;
    if (stats_.events_per_instrument[instrument_id] % 1000 == 0) {
        it->second->flush();
    }
}

void BinarySplitter::write_file_header(std::ofstream& writer) {
    FileHeader header;
    header.magic_number_ = 0x52454658;
    header.version_ = 1;
    header.created_timestamp_ = 0;
    header.message_slot_size_ = sizeof(MessageSlot);
    std::strncpy(header.component_name_, "BinarySplitter", sizeof(header.component_name_) - 1);
    header.component_name_[sizeof(header.component_name_) - 1] = '\0';

    writer.write(reinterpret_cast<const char*>(&header), sizeof(FileHeader));
    writer.flush();
}

void BinarySplitter::ensure_writer_capacity(uint32_t instrument_id, const std::string& output_dir) {
    const uint64_t needed = sizeof(MessageSlot);

    auto it = writers_.find(instrument_id);
    if (it == writers_.end() || !it->second) {
        open_new_chunk(instrument_id, output_dir);
        return;
    }

    const uint64_t current_bytes = bytes_written_[instrument_id];
    if (current_bytes + needed > max_chunk_bytes_) {
        it->second->flush();
        writers_.erase(it);
        open_new_chunk(instrument_id, output_dir);
    }
}

void BinarySplitter::open_new_chunk(uint32_t instrument_id, const std::string& output_dir) {
    const uint32_t chunk_index = chunk_indices_[instrument_id];
    std::string filename = get_output_filename(instrument_id, output_dir, chunk_index);
    auto writer = create_writer(filename);
    if (!writer) {
        spdlog::error("Failed to create writer for instrument {} chunk {}", instrument_id, chunk_index);
        return;
    }

    write_file_header(*writer);
    writers_[instrument_id] = std::move(writer);
    bytes_written_[instrument_id] = sizeof(FileHeader);
    chunk_indices_[instrument_id] = chunk_index + 1;

    std::string asset_name = get_asset_name(instrument_id);
    stats_.instrument_names[instrument_id] = asset_name;
    stats_.instrument_files[instrument_id].push_back(filename);

    spdlog::info("Created output file for {} (ID: {}), chunk {}: {}",
                 asset_name, instrument_id, chunk_index, filename);
}

std::string BinarySplitter::get_output_filename(uint32_t instrument_id,
                                               const std::string& output_dir,
                                               uint32_t chunk_index) const {
    std::string asset_name = get_asset_name(instrument_id);

    std::filesystem::path subfolder_path = std::filesystem::path(output_dir) / asset_name;

    std::filesystem::create_directories(subfolder_path);

    std::string filename = filename_pattern_;

    size_t pos = 0;
    while ((pos = filename.find("{name}", pos)) != std::string::npos) {
        filename.replace(pos, 6, asset_name);
        pos += asset_name.length();
    }

    pos = 0;
    while ((pos = filename.find("{id}", pos)) != std::string::npos) {
        filename.replace(pos, 4, std::to_string(instrument_id));
        pos += std::to_string(instrument_id).length();
    }

    std::filesystem::path base_path = subfolder_path / filename;
    std::string stem = base_path.stem().string();
    std::string extension = base_path.extension().string();

    std::ostringstream oss;
    oss << stem << "_chunk" << std::setfill('0') << std::setw(4) << chunk_index;
    std::filesystem::path chunk_name = oss.str() + extension;

    return (subfolder_path / chunk_name).string();
}

std::string BinarySplitter::get_asset_name(uint32_t instrument_id) const {
    if (!use_asset_names_ || !AssetInfoManager::is_initialized()) {
        return std::to_string(instrument_id);
    }

    const AssetInfo* asset_info = AssetInfoManager::get_by_instrument_id(instrument_id);
    if (asset_info && asset_info->exchange_symbol_) {
        std::string clean_symbol(asset_info->exchange_symbol_);

        std::replace_if(clean_symbol.begin(), clean_symbol.end(),
                        [](char c) {
                            return c == '/' || c == '\\' || c == ':' || c == '*' ||
                                   c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
                        },
                        '-');

        if (!clean_symbol.empty()) {
            return clean_symbol;
        }
    }

    if (asset_info) {
        spdlog::debug("Asset symbol missing for instrument {}, using ID as name", instrument_id);
    } else {
        spdlog::debug("Asset info not found for instrument {}, using ID as name", instrument_id);
    }
    return std::to_string(instrument_id);
}

void BinarySplitter::ensure_output_directory(const std::string& output_dir) const {
    std::filesystem::create_directories(output_dir);
}

std::unique_ptr<std::ofstream> BinarySplitter::create_writer(const std::string& filename) {
    std::filesystem::create_directories(std::filesystem::path(filename).parent_path());

    auto writer = std::make_unique<std::ofstream>(filename,
                                                  std::ios::binary | std::ios::out);
    if (!writer->is_open()) {
        return nullptr;
    }
    return writer;
}

}
