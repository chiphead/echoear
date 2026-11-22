#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct DlnaRenderer {
    std::string location;
    std::string usn;
    std::string server;
    std::string source_address;
    std::string friendly_name;
    std::string manufacturer;
    std::string model_name;
    std::string udn;
    std::string av_transport_service_type;
    std::string av_transport_control_url;
};

class DlnaDiscovery {
public:
    static bool Discover(uint32_t timeout_ms, std::vector<DlnaRenderer>& renderers);
};
