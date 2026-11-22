#pragma once

#include <cstdint>
#include <string>

#include "dlna_discovery.h"

/**
 * Minimal UPnP AVTransport control-point client.
 * All methods perform blocking HTTP I/O; call from a worker task.
 */
class DlnaControl {
public:
    static bool SetUriAndPlay(const DlnaRenderer& renderer,
                              const std::string& media_uri,
                              const std::string& title,
                              uint32_t timeout_ms = 5000);
    static bool Stop(const DlnaRenderer& renderer, uint32_t timeout_ms = 5000);
};
