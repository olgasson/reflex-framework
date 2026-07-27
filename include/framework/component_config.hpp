#pragma once
#include <string>
#include <vector>

namespace reflex {

struct ComponentConfig {
  std::string component_name_;
  std::string environment_;
};

struct BlobConfig : public ComponentConfig {
  std::string url_;
  std::string api_file_path_;
  std::vector<std::string> currency_pairs_;

  BlobConfig(const ComponentConfig& base, const std::string& url, const std::string& api_file_path, std::vector<std::string> currency_pairs)
      : ComponentConfig(base), url_(url), api_file_path_(api_file_path), currency_pairs_(currency_pairs) {}
};

struct BinaryWriterConfig : public ComponentConfig {
  std::string output_file_path_;

  BinaryWriterConfig(const ComponentConfig& base, const std::string& output_file_path)
      : ComponentConfig(base), output_file_path_(output_file_path) {}
};


struct GatewayConfig : public ComponentConfig {
  std::string url_;
  std::string api_file_path_;
  uint32_t account_;

  GatewayConfig(const ComponentConfig& base, const std::string& url, const std::string& api_file_path, uint32_t account)
      : ComponentConfig(base), url_(url), api_file_path_(api_file_path), account_(account) {}
};

}