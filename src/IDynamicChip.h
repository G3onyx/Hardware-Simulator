//
// Created by georg on 27/07/2026.
//

#pragma once
#include <cstdint>

struct HardwareState {
    bool has_screen = false;
    uint32_t screen_width = 0;
    uint32_t screen_height = 0;
    uint32_t* screen_pixels = nullptr;
    double target_hz = 0.0;
};

// The generic boundary between the Host Window and the DLL
class IDynamicChip {
public:
    virtual ~IDynamicChip() = default;
    virtual HardwareState get_info() = 0;
    virtual void emulate_frame(uint64_t cycles, uint64_t keys_low, uint64_t keys_high) = 0;
    virtual void set_pin(const char* name, uint64_t value) {}
    virtual uint64_t get_pin(const char* name) { return 0; }
};