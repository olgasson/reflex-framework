#pragma once
#include <cstdint>
#include <atomic>

namespace reflex {

struct alignas(128) FileHeader {
    uint32_t magic_number_;
    uint16_t version_;
    uint16_t reserved1_;
    uint64_t created_timestamp_;
    uint32_t message_slot_size_;
    uint32_t reserved2_;
    char component_name_[32];
    uint64_t write_offset_;
    uint64_t reserved3_[8];
    
    FileHeader()
        : magic_number_(0x52454658)
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
