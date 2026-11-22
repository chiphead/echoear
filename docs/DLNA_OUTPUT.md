# DLNA network audio output

This feature branch adds a staged DLNA output path for the Waveshare ESP32-S3-Touch-AMOLED-1.32 EchoEar build.

## Design constraints

- Keep microphone capture and the ES8311 input path intact.
- Keep the existing Opus decoder and AudioService playback queue intact.
- Add network playback as a selectable output backend; do not remove local speaker output.
- Do not block AudioOutputTask on network I/O.
- Treat DLNA control and media delivery as separate responsibilities.

## Implementation stages

1. **SSDP discovery** — send an IPv4 M-SEARCH for UPnP MediaRenderer devices and collect unique LOCATION/USN/SERVER responses. Implemented in `main/audio/dlna/dlna_discovery.*`.
2. **Renderer description** — implemented in `main/audio/dlna/dlna_device_description.*`. Fetch LOCATION over HTTP, extract FriendlyName/Manufacturer/ModelName/UDN, and resolve the AVTransport service type and control URL using UPnP `URLBase` when present. The response is bounded to 24 KiB.
3. **Media HTTP endpoint** — `DlnaMediaServer` now serves one immutable WAV buffer over HTTP, supports GET, HEAD, and single-byte-range requests, and returns `audio/wav`. HEAD responses advertise the selected representation length without sending the WAV body. The caller must keep the buffer alive and unchanged until the server stops.
4. **AVTransport control** — `DlnaControl` sends `SetAVTransportURI`, `Play`, and `Stop` SOAP actions with bounded response capture and XML-escaped DIDL-Lite metadata. Calls are blocking and must run in a worker task; renderer state/event subscription remains pending.
5. **Audio sink integration** — add a non-blocking sink interface after PCM decode; preserve LocalSink as the default.
6. **TTS lifecycle and UX** — buffer/stream decoded PCM, handle interruption, playback completion, Wi-Fi loss, and microphone/AEC policy.

## Current discovery API

`DlnaDiscovery::Discover(timeout_ms, renderers)` performs a synchronous SSDP search. Call it from a worker task, never from the audio output task or UI thread. A successful return means the SSDP exchange completed; an empty result means no renderer answered within the discovery window. Then call `DlnaDeviceDescription::FetchAndParse(renderer, timeout_ms)` for each candidate. It performs blocking HTTP I/O and fills the device identity plus AVTransport service/control URL.

## Standalone transport smoke-test flow

The control and media-server classes are deliberately independent of `AudioService`. A worker task can exercise them in this order:

```cpp
DlnaMediaServer media;
if (media.Start(board_ipv4, wav_data, wav_size)) {
    if (DlnaControl::SetUriAndPlay(renderer, media.GetMediaUri(), "EchoEar test")) {
        // Wait for the renderer to finish or call DlnaControl::Stop(renderer).
    }
    media.Stop();
}
```

The `wav_data` buffer must remain valid and unchanged until `media.Stop()` returns. The example assumes the renderer has already been discovered and its device description parsed. It is a transport smoke-test path, not yet wired to TTS playback.

## Current limitations

- SSDP discovery, UPnP device-description parsing, and a first AVTransport SOAP control client are implemented. SOAP control is not yet validated against a physical renderer; the audio sink routing remains unimplemented. The HTTP endpoint is implemented but not yet tested against the SRS-X88.
- These modules have not yet been compiled with the project's ESP-IDF 6.1.x environment or validated on hardware.
- The Sony SRS-X88 must be on the same LAN/VLAN and have network standby / network control enabled as applicable.
