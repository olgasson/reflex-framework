#pragma once
#include <cstdint>
#include <atomic>

namespace reflex {

struct alignas(128) FileHeader {
    uint32_t magic_number_;        // File format identifier (4 bytes)
    uint16_t version_;             // File format version (2 bytes)
    uint16_t reserved1_;           // Reserved for future use (2 bytes)
    uint64_t created_timestamp_;   // When the file was created (nanoseconds) (8 bytes)
    uint32_t message_slot_size_;   // Size of each MessageSlot in bytes (4 bytes)
    uint32_t reserved2_;           // Reserved for future use (4 bytes)
    char component_name_[32];      // Name of the component that created this file (32 bytes)
    uint64_t write_offset_;        // Current write position (8 bytes)
    uint64_t reserved3_[8];        // Reserved for future use (64 bytes)
    // Total: 4+2+2+8+4+4+32+8+64 = 128 bytes
    
    FileHeader()
        : magic_number_(0x52454658)  // "REFX" in hex
        , version_(1)
        , reserved1_(0)
        , created_timestamp_(0)
        , message_slot_size_(0)
        , reserved2_(0)
        , component_name_{}
        , write_offset_(0)
        , reserved3_{}
    {}
};

static_assert(sizeof(FileHeader) == 128, "FileHeader must be exactly 128 bytes");

}