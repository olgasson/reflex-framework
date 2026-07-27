#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <string>
#include "utils/codec_utils.hpp"

namespace reflex {
namespace test {

class CodecUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }

    // Helper function to compare doubles with tolerance
    bool isClose(double a, double b, double tolerance = 1e-10) {
        return std::abs(a - b) <= tolerance;
    }
};

// Test encode_price with double values
TEST_F(CodecUtilsTest, EncodePriceDoubleBasicValues) {
    EXPECT_EQ(CodecUtils::encode_price(1.0), 100000000LL);
    EXPECT_EQ(CodecUtils::encode_price(0.0), 0LL);
    EXPECT_EQ(CodecUtils::encode_price(100.0), 10000000000LL);
    EXPECT_EQ(CodecUtils::encode_price(0.12345678), 12345678LL);
}

TEST_F(CodecUtilsTest, EncodePriceDoubleNegativeValues) {
    EXPECT_EQ(CodecUtils::encode_price(-1.0), -100000000LL);
    EXPECT_EQ(CodecUtils::encode_price(-100.0), -10000000000LL);
    EXPECT_EQ(CodecUtils::encode_price(-0.12345678), -12345678LL);
}

TEST_F(CodecUtilsTest, EncodePriceDoubleSmallValues) {
    EXPECT_EQ(CodecUtils::encode_price(0.00000001), 1LL);
    EXPECT_EQ(CodecUtils::encode_price(0.00000005), 5LL);
    EXPECT_EQ(CodecUtils::encode_price(0.00000009), 9LL);
}

TEST_F(CodecUtilsTest, EncodePriceDoubleLargeValues) {
    EXPECT_EQ(CodecUtils::encode_price(999999.99999999), 99999999999999LL);
    EXPECT_EQ(CodecUtils::encode_price(1000000.0), 100000000000000LL);
}

// Test encode_price with string values
TEST_F(CodecUtilsTest, EncodePriceStringBasicValues) {
    EXPECT_EQ(CodecUtils::encode_price("1.0"), 100000000LL);
    EXPECT_EQ(CodecUtils::encode_price("0.0"), 0LL);
    EXPECT_EQ(CodecUtils::encode_price("100.0"), 10000000000LL);
    EXPECT_EQ(CodecUtils::encode_price("0.12345678"), 12345678LL);
}

TEST_F(CodecUtilsTest, EncodePriceStringIntegerValues) {
    EXPECT_EQ(CodecUtils::encode_price("1"), 100000000LL);
    EXPECT_EQ(CodecUtils::encode_price("100"), 10000000000LL);
    EXPECT_EQ(CodecUtils::encode_price("0"), 0LL);
}

TEST_F(CodecUtilsTest, EncodePriceStringPrecisionValues) {
    EXPECT_EQ(CodecUtils::encode_price("1.1"), 110000000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.12"), 112000000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.123"), 112300000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.1234"), 112340000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.12345"), 112345000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.123456"), 112345600LL);
    EXPECT_EQ(CodecUtils::encode_price("1.1234567"), 112345670LL);
    EXPECT_EQ(CodecUtils::encode_price("1.12345678"), 112345678LL);
}

// Test encode_quantity
TEST_F(CodecUtilsTest, EncodeQuantityBasicValues) {
    EXPECT_EQ(CodecUtils::encode_quantity(1.0), 100000000LL);
    EXPECT_EQ(CodecUtils::encode_quantity(0.0), 0LL);
    EXPECT_EQ(CodecUtils::encode_quantity(100.0), 10000000000LL);
    EXPECT_EQ(CodecUtils::encode_quantity(0.12345678), 12345678LL);
}

TEST_F(CodecUtilsTest, EncodeQuantityNegativeValues) {
    EXPECT_EQ(CodecUtils::encode_quantity(-1.0), -100000000LL);
    EXPECT_EQ(CodecUtils::encode_quantity(-100.0), -10000000000LL);
    EXPECT_EQ(CodecUtils::encode_quantity(-0.12345678), -12345678LL);
}

TEST_F(CodecUtilsTest, EncodeQuantitySmallValues) {
    EXPECT_EQ(CodecUtils::encode_quantity(0.00000001), 1LL);
    EXPECT_EQ(CodecUtils::encode_quantity(0.00000005), 5LL);
    EXPECT_EQ(CodecUtils::encode_quantity(0.00000009), 9LL);
}

// Test to_double conversion
TEST_F(CodecUtilsTest, ToDoubleBasicValues) {
    EXPECT_TRUE(isClose(CodecUtils::to_double(100000000LL), 1.0));
    EXPECT_TRUE(isClose(CodecUtils::to_double(0LL), 0.0));
    EXPECT_TRUE(isClose(CodecUtils::to_double(10000000000LL), 100.0));
    EXPECT_TRUE(isClose(CodecUtils::to_double(12345678LL), 0.12345678));
}

TEST_F(CodecUtilsTest, ToDoubleNegativeValues) {
    EXPECT_TRUE(isClose(CodecUtils::to_double(-100000000LL), -1.0));
    EXPECT_TRUE(isClose(CodecUtils::to_double(-10000000000LL), -100.0));
    EXPECT_TRUE(isClose(CodecUtils::to_double(-12345678LL), -0.12345678));
}

TEST_F(CodecUtilsTest, ToDoubleSmallValues) {
    EXPECT_TRUE(isClose(CodecUtils::to_double(1LL), 0.00000001));
    EXPECT_TRUE(isClose(CodecUtils::to_double(5LL), 0.00000005));
    EXPECT_TRUE(isClose(CodecUtils::to_double(9LL), 0.00000009));
}

// Test round-trip conversions
TEST_F(CodecUtilsTest, RoundTripConversionsPrice) {
    std::vector<double> test_values = {
        0.0, 1.0, 100.0, 0.12345678, 999.99999999,
        -1.0, -100.0, -0.12345678, 0.00000001
    };

    for (double value : test_values) {
        int64_t encoded = CodecUtils::encode_price(value);
        double decoded = CodecUtils::to_double(encoded);
        EXPECT_TRUE(isClose(value, decoded, 1e-8))
            << "Round-trip failed for value: " << value
            << " encoded: " << encoded
            << " decoded: " << decoded;
    }
}

TEST_F(CodecUtilsTest, RoundTripConversionsQuantity) {
    std::vector<double> test_values = {
        0.0, 1.0, 100.0, 0.12345678, 999.99999999,
        -1.0, -100.0, -0.12345678, 0.00000001
    };

    for (double value : test_values) {
        int64_t encoded = CodecUtils::encode_quantity(value);
        double decoded = CodecUtils::to_double(encoded);
        EXPECT_TRUE(isClose(value, decoded, 1e-8))
            << "Round-trip failed for value: " << value
            << " encoded: " << encoded
            << " decoded: " << decoded;
    }
}

// Test string vs double encoding consistency
TEST_F(CodecUtilsTest, StringVsDoubleConsistency) {
    std::vector<std::pair<std::string, double>> test_pairs = {
        {"1.0", 1.0},
        {"100.0", 100.0},
        {"0.12345678", 0.12345678},
        {"0.00000001", 0.00000001},
        {"999.99999999", 999.99999999}
    };

    for (const auto& pair : test_pairs) {
        int64_t string_encoded = CodecUtils::encode_price(pair.first.c_str());
        int64_t double_encoded = CodecUtils::encode_price(pair.second);

        EXPECT_EQ(string_encoded, double_encoded)
            << "Inconsistency for value: " << pair.first
            << " string encoded: " << string_encoded
            << " double encoded: " << double_encoded;
    }
}

// Test scale constant
TEST_F(CodecUtilsTest, ScaleConstant) {
    EXPECT_EQ(CodecUtils::SCALE, 100000000LL);
    EXPECT_EQ(CodecUtils::SCALE, 1e8);
}

// Test edge cases
TEST_F(CodecUtilsTest, EdgeCases) {
    // Test very small positive value
    EXPECT_EQ(CodecUtils::encode_price("0.00000001"), 1LL);

    // Test empty fractional part
    EXPECT_EQ(CodecUtils::encode_price("123."), 12300000000LL);

    // Test single digit
    EXPECT_EQ(CodecUtils::encode_price("5"), 500000000LL);
}

// Negative string values (e.g. OKX negative funding rates)
TEST_F(CodecUtilsTest, EncodePriceStringNegativeValues) {
    EXPECT_EQ(CodecUtils::encode_price("-1.0"), -100000000LL);
    EXPECT_EQ(CodecUtils::encode_price("-100"), -10000000000LL);
    EXPECT_EQ(CodecUtils::encode_price("-0.12345678"), -12345678LL);
    EXPECT_EQ(CodecUtils::encode_price("-0.00000001"), -1LL);
    EXPECT_EQ(CodecUtils::encode_price("-0.0001"), -10000LL);  // negative funding rate
    EXPECT_EQ(CodecUtils::encode_price("-0"), 0LL);
}

// More than 8 decimal places must truncate, not mis-scale
TEST_F(CodecUtilsTest, EncodePriceStringTruncatesBeyondEightDecimals) {
    EXPECT_EQ(CodecUtils::encode_price("1.123456789"), 112345678LL);
    EXPECT_EQ(CodecUtils::encode_price("0.0000000199"), 1LL);
    EXPECT_EQ(CodecUtils::encode_price("0.123456789012345"), 12345678LL);
    EXPECT_EQ(CodecUtils::encode_price("-1.123456789"), -112345678LL);
}

// Empty / null-ish input
TEST_F(CodecUtilsTest, EncodePriceStringEmptyInput) {
    EXPECT_EQ(CodecUtils::encode_price(""), 0LL);
    EXPECT_EQ(CodecUtils::encode_price("-"), 0LL);
    EXPECT_EQ(CodecUtils::encode_price("."), 0LL);
    EXPECT_EQ(CodecUtils::encode_price(static_cast<const char*>(nullptr)), 0LL);
}

// Garbage input: parsing stops at the first non-digit character
TEST_F(CodecUtilsTest, EncodePriceStringGarbageInput) {
    EXPECT_EQ(CodecUtils::encode_price("abc"), 0LL);
    EXPECT_EQ(CodecUtils::encode_price("1.2x3"), 120000000LL);
    EXPECT_EQ(CodecUtils::encode_price("12a"), 1200000000LL);
    EXPECT_EQ(CodecUtils::encode_price("1.5 "), 150000000LL);
    EXPECT_EQ(CodecUtils::encode_price("1e5"), 100000000LL);   // exponent not supported
    EXPECT_EQ(CodecUtils::encode_price("1.2.3"), 120000000LL); // second dot terminates
}

// Overflow saturates rather than wrapping
TEST_F(CodecUtilsTest, EncodePriceStringOverflowSaturates) {
    EXPECT_EQ(CodecUtils::encode_price("99999999999999999999"),
              std::numeric_limits<int64_t>::max());
    EXPECT_EQ(CodecUtils::encode_price("-99999999999999999999"),
              std::numeric_limits<int64_t>::min());
    // Large but representable value still parses exactly
    EXPECT_EQ(CodecUtils::encode_price("92233720368.54775807"), 9223372036854775807LL);
}

// Performance/stress test with typical trading values
TEST_F(CodecUtilsTest, TypicalTradingValues) {
    // Stock prices
    EXPECT_EQ(CodecUtils::encode_price(150.25), 15025000000LL);
    EXPECT_EQ(CodecUtils::encode_price(2500.75), 250075000000LL);

    // Crypto prices with high precision
    EXPECT_EQ(CodecUtils::encode_price(0.00123456), 123456LL);

    // Large quantities
    EXPECT_EQ(CodecUtils::encode_quantity(1000000.0), 100000000000000LL);
}

} // namespace test
} // namespace reflex