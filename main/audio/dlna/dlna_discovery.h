#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Minimal SSDP discovery for UPnP AV MediaRenderer devices.
 *
 * This class only discovers renderers and returns their LOCATION URLs.
 * Device description parsing and AVTransport control are intentionally kept
 * in separate modules so discovery can be tested independently.
 */
struct DlnaRenderer {
    std::string location;
    std::string usn;
    std::string server;
    std::string source_address;
};

class DlnaDiscovery {
public:
    /**
     * Search the local IPv4 network for UPnP MediaRenderer devices.
     *
     * @param timeout_ms Total discovery window. Values below 1000 ms are
     *                   clamped to 1000 ms.
     * @param renderers  Replaced with the unique renderers discovered.
     * @return true when the SSDP exchange completed, even if no renderer
     *         responded; false on socket or send/receive setup errors.
     */
    static bool Discover(uint32_t timeout_ms, std::vector<DlnaRenderer>& renderers);
};
