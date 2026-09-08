
#include "backtest/binary_splitter.hpp"
#include "asset_info_manager.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
void print_usage(const char* prog, std::ostream& os = std::cout) {
  os << "Usage: " << prog << " [options] <input_files...>\n"
            << "Options:\n"
            << "  -o, --output-dir <path>   Destination directory (default: ./split_by_asset)\n"
            << "  --pattern <pattern>       Base filename pattern (default: {name}_market_data.bin)\n"
            << "  --chunk-bytes <bytes>     Max bytes per chunk (default: 1073741824)\n"
            << "  --include <ids>           Comma-separated list of instrument IDs to keep\n"
            << "  --file-list <path>        Read input file paths from a text file (one per line)\n";
}

}

int main(int argc, char** argv) {
  std::vector<std::string> data_files;
  std::string output_path = "./split_by_asset";
  std::string name_pattern = "{name}_market_data.bin";
  const std::string id_pattern = "instrument_{id}.bin";
  std::vector<uint32_t> include_ids;
  uint64_t chunk_bytes = 1ull << 30;
  std::string file_list_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-o" || arg == "--output-dir") && i + 1 < argc) {
      output_path = argv[++i];
      continue;
    }
    if (arg == "--pattern" && i + 1 < argc) {
      name_pattern = argv[++i];
      continue;
    }
    if (arg == "--file-list" && i + 1 < argc) {
      file_list_path = argv[++i];
      continue;
    }
    if (arg == "--include" && i + 1 < argc) {
      std::string list = argv[++i];
      std::string current;
      try {
        for (char ch : list) {
          if (ch == ',') {
            if (!current.empty()) {
              include_ids.push_back(static_cast<uint32_t>(std::stoul(current)));
              current.clear();
            }
          } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
          }
        }
        if (!current.empty()) {
          include_ids.push_back(static_cast<uint32_t>(std::stoul(current)));
        }
      } catch (const std::exception& e) {
        std::cerr << "Invalid --include value '" << list << "': " << e.what() << std::endl;
        return 1;
      }
      continue;
    }
    if (arg == "--chunk-bytes" && i + 1 < argc) {
      try {
        chunk_bytes = std::stoull(argv[++i]);
      } catch (const std::exception& e) {
        std::cerr << "Invalid --chunk-bytes value: " << e.what() << std::endl;
        return 1;
      }
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    data_files.push_back(arg);
  }

  if (!file_list_path.empty()) {
    std::ifstream file_list(file_list_path);
    if (!file_list) {
      std::cerr << "Error: Cannot open file list: " << file_list_path << std::endl;
      return 1;
    }
    std::string line;
    while (std::getline(file_list, line)) {
      size_t start = line.find_first_not_of(" \t\r\n");
      size_t end = line.find_last_not_of(" \t\r\n");
      if (start != std::string::npos && end != std::string::npos) {
        std::string path = line.substr(start, end - start + 1);
        if (!path.empty() && path[0] != '#') {
          data_files.push_back(path);
        }
      }
    }
    std::cout << "Loaded " << data_files.size() << " files from " << file_list_path << std::endl;
  }

  if (data_files.empty()) {
    std::cerr << "Error: no input files supplied." << std::endl;
    print_usage(argv[0], std::cerr);
    return 1;
  }

  std::cout << "Current working directory: " << std::filesystem::current_path() << std::endl;
  std::cout << "Output directory: " << output_path << std::endl;
  std::cout << "Chunk size limit: " << chunk_bytes << " bytes" << std::endl;

  std::filesystem::create_directories(output_path);

  reflex::AssetInfoManager::initialize();

  reflex::BinarySplitter splitter(data_files);
  splitter.set_use_asset_names(true);
  splitter.set_filename_pattern(name_pattern);
  splitter.set_max_chunk_bytes(chunk_bytes);
  if (!include_ids.empty()) {
    splitter.set_instrument_filters(include_ids, {});
  }

  if (!splitter.split_by_instrument(output_path)) {
    std::cerr << "Failed to split files" << std::endl;
    return 1;
  }

  const auto& stats = splitter.get_stats();

  std::cout << "\nSuccess: processed " << stats.total_input_files << " input files\n";
  std::cout << "Total: " << stats.total_events
            << " events across " << stats.total_instruments
            << " instruments\n";
  std::cout << "Output directory: " << output_path << "\n\n";

  for (const auto& [inst_id, files] : stats.instrument_files) {
    size_t count = stats.events_per_instrument.at(inst_id);
    std::string asset_name = stats.instrument_names.count(inst_id) ? stats.instrument_names.at(inst_id)
                                                                   : std::to_string(inst_id);
    std::cout << asset_name << " (ID: " << inst_id << "): " << count
              << " events across " << files.size() << " chunk(s)\n";
    for (const auto& file : files) {
      std::cout << "   " << file << "\n";
    }
  }

  return 0;
}
