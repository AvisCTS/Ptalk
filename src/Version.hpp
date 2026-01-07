// File: Version.hpp
#pragma once
#include "esp_system.h"
#include <string>
#include <sstream>
#include <iomanip>
namespace app_meta {
    static constexpr const char* APP_VERSION = "1.0.1";
    static constexpr const char* DEVICE_MODEL = "PTalk-V1";
    static constexpr const char* BUILD_DATE = __DATE__; // Tự động lấy ngày biên dịch
}


uint64_t getEfuseMac()
{
    uint64_t mac;
    esp_efuse_mac_get_default((uint8_t*)&mac);
    return mac;
}

std::string getDeviceEfuseID()
{
    uint64_t mac = getEfuseMac();

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(2) << ((mac >> 40) & 0xFF)
        << std::setw(2) << ((mac >> 32) & 0xFF)
        << std::setw(2) << ((mac >> 24) & 0xFF)
        << std::setw(2) << ((mac >> 16) & 0xFF)
        << std::setw(2) << ((mac >> 8)  & 0xFF)
        << std::setw(2) << ( mac        & 0xFF);

    return oss.str(); // ví dụ: "24a160ff3b9c"
}