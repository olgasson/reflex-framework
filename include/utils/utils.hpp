#pragma once
#include <regex>
#include <string>

namespace reflex {

class Utils {

public:

  static constexpr int MAX_LENGTH = 30;

  static bool validate_instance_name(const std::string& instance_name) {
    if (instance_name.empty()) {
      return false;
    }

    if (instance_name.length() > MAX_LENGTH) {
      return false;
    }

    static const std::regex pattern(R"(^[a-zA-Z0-9-]{1,27}~\d{1,2}[ab]$)");
    return std::regex_match(instance_name, pattern);
  }

  static std::string extract_service_name(const std::string& instance_name) {
    if (instance_name.empty()) {
      throw std::invalid_argument("Instance name cannot be empty.");
    }

    size_t tilde_index = instance_name.find('~');
    if (tilde_index == std::string::npos) {
      throw std::invalid_argument("Instance name does not contain a valid delimiter '~'.");
    }

    return instance_name.substr(0, tilde_index);
  }

  static std::string extract_routing_group(const std::string& routing_node) {
    if (routing_node.empty()) {
      throw std::invalid_argument("routingNode name cannot be null or empty.");
    }

    // Check if routing_node is blank (only whitespace)
    if (std::all_of(routing_node.begin(), routing_node.end(), ::isspace)) {
      return "";
    }

    size_t dash_index = routing_node.find('-');
    if (dash_index == std::string::npos) {
      throw std::invalid_argument("routingNode name does not contain a valid delimiter '-'.");
    }

    return routing_node.substr(0, dash_index);
  }

};


}