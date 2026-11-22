#pragma once
#include <string>
/** Optional standalone Sony SRS-X88 DLNA transport smoke test. */
class DlnaSmokeTest {
public:
    // Starts a worker task; does not use or modify AudioService/local ES8311 playback.
    static bool Start(const std::string& host_ipv4);
};
