#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace RemoteControl {
constexpr uint8_t requestType = 0x0b;
constexpr uint8_t stateType = 0x0b;
constexpr uint8_t resultType = 0x0c;
struct Request {
    uint32_t id = 0;
    uint32_t revision = 0;
    uint32_t command = 0;
    int32_t value = 0;
};
inline bool decode(const uint8_t* bytes, size_t size, Request& request) {
    if (size != 16) return false;
    uint32_t fields[4]{};
    for (size_t field = 0; field < 4; ++field)
        for (size_t byte = 0; byte < 4; ++byte)
            fields[field] |= uint32_t(bytes[field * 4 + byte]) << (byte * 8);
    request = {fields[0], fields[1], fields[2], static_cast<int32_t>(fields[3])};
    return true;
}
inline bool valid(const Request& request) {
    const auto value = request.value;
    switch (request.command) {
    case 0: return value == 0;
    case 1: return value >= 0 && value <= 150;
    case 2: return value == 2 || value == 5 || value == 10 || value == 20;
    case 3: case 4: case 7: case 9: case 12: return value == 0 || value == 1;
    case 5: case 6: return value >= 0 && value <= 8;
    case 8: return value >= 0 && value <= 100;
    case 10: return value >= 10 && value <= 90;
    case 11: return value >= 100 && value <= 1500;
    case 16: return value >= 0x10 && value <= 0x12;
    default: return false;
    }
}
inline bool storedAdmin(const std::string& role) { return role == "admin"; }
}
