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
2. **Renderer description** — implemented in `main/audio/dlna/dlna_device_description.*`. Fetch LOCATION over HTTP, extract FriendlyName/Manufacturer/ModelName/UDN, and resolve the AVTransport service type and control URL. The response is bounded to 24 KiB.
3. **Media HTTP endpoint** — serve a bounded WAV/PCM resource with correct content type, content length, and HTTP Range behavior required by the renderer.
4. **AVTransport control** — set the media URI and metadata, then issue Play/Stop and track renderer state.
5. **Audio sink integration** — add a non-blocking sink interface after PCM decode; preserve LocalSink as the default.
6. **TTS lifecycle and UX** — buffer/stream decoded PCM, handle interruption, playback completion, Wi-Fi loss, and microphone/AEC policy.

## Current discovery API

`DlnaDiscovery::Discover(timeout_ms, renderers)` performs a synchronous SSDP search. Call it from a worker task, never from the audio output task or UI thread. A successful return means the SSDP exchange completed; an empty result means no renderer answered within the discovery window. Then call `DlnaDeviceDescription::FetchAndParse(renderer, timeout_ms)` for each candidate. It performs blocking HTTP I/O and fills the device identity plus AVTransport service/control URL.

## Current limitations

- SSDP discovery and UPnP device-description parsing are implemented. SOAP control, HTTP media server, and audio sink routing are not implemented yet.
- No compile or hardware validation has been performed as part of this initial commit.
- The Sony SRS-X88 must be on the same LAN/VLAN and have network standby / network control enabled as applicable.
