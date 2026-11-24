#pragma once

#include "dlna_discovery.h"

/**
 * Downloads and parses a UPnP device description document.
 * Call from a worker task; this performs blocking network I/O.
 */
class DlnaDeviceDescription {
public:
    static bool FetchAndParse(DlnaRenderer& renderer, uint32_t timeout_ms = 5000);
};
