#pragma once
#include <cstdint>

namespace state {

// Interaction (UI/Audio/VAD)
enum class InteractionState : uint8_t {
    IDLE,
    LISTENING,
    THINKING,
    SPEAKING,
    SLEEPING
};

// Connectivity (WiFi / Websocket)
enum class ConnectivityState : uint8_t {
    OFFLINE,
    CONNECTING_WIFI,
    WIFI_PORTAL,
    CONNECTING_WS,
    ONLINE
};

// System level
enum class SystemState : uint8_t {
    BOOTING,
    RUNNING,
    ERROR,
    MAINTENANCE
};

// Power level
enum class PowerState : uint8_t {
    NORMAL,
    LOW_BATTERY,
    CHARGING,
    FULL_BATTERY
};

} // namespace state
