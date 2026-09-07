#pragma once
#include <cmath>
#include <cstdint>

// CodecUtils.hpp
namespace reflex {

class CodecUtils {
public:
  static constexpr int64_t SCALE = 100000000LL;         // 1e8

  // fast: parse ASCII decimal straight to a 1e-8-scaled int64.
  // Handles an optional leading '-' or '+', truncates digits beyond 8 decimal
  // places, stops at the first unexpected character, and saturates to
  // INT64_MAX/INT64_MIN instead of overflowing.
  static inline int64_t encode_price(const char* s) noexcept {
    if (s == nullptr) return 0;

    bool negative = false;
    if (*s == '-') {
      negative = true;
      ++s;
    } else if (*s == '+') {
      ++s;
    }

    int64_t v = 0;
    int      frac = 0;
    bool     after_dot = false;

    for (char c; (c = *s++) != '\0'; ) {
      if (c == '.' && !after_dot) { after_dot = true; continue; }
      if (c < '0' || c > '9') break;        // stop at first non-digit
      if (after_dot && frac >= 8) break;    // truncate beyond 1e-8 precision
      const int64_t digit = c - '0';
      if (v > (INT64_MAX - digit) / 10) {   // would overflow: saturate
        return negative ? INT64_MIN : INT64_MAX;
      }
      v = v * 10 + digit;
      if (after_dot) ++frac;
    }

    while (frac++ < 8) {                    // right-pad to 1e-8
      if (v > INT64_MAX / 10) {
        return negative ? INT64_MIN : INT64_MAX;
      }
      v *= 10;
    }
    return negative ? -v : v;
  }

  // keep the old double-based version for ad-hoc use
  static inline int64_t encode_price(double px) {
    return static_cast<int64_t>(std::llround(px * SCALE));
  }

  // keep the old double-based version for ad-hoc use
  static inline int64_t encode_quantity(double qt) {
    return static_cast<int64_t>(std::llround(qt * SCALE));
  }

  static inline double to_double(int64_t mantissa) {
    return static_cast<double>(mantissa) / SCALE;
  }
};
} // namespace reflex
