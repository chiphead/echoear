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
5. **PCM output abstraction** — an optional `AudioServiceCallbacks::on_pcm_output` hook is now present after Opus decode. It receives PCM plus the output sample rate; returning `true` means the frame was handled, while `false` preserves the existing ES8311/local codec path. The callback contract explicitly forbids blocking/network I/O and is intended only to enqueue/copy PCM. No DLNA callback is registered yet.
6. **Utterance buffering and publication** — `DlnaPcmBuffer` now provides a bounded, mutex-protected PCM accumulator and WAV finalizer. It validates a consistent sample rate/channel count, caps memory (default 512 KiB), prefers PSRAM, and returns an immutable WAV allocation for the media server. The lifecycle owner still needs to call `Begin`, append frames from the PCM callback, and call `FinalizeWav` from a worker after playback drains. The WAV header is reserved at the front of the allocation, so finalization writes the header and transfers buffer ownership without copying the utterance or allocating a second full-size block. Only after the buffer is immutable and the HTTP endpoint is ready should a worker issue `SetAVTransportURI` and `Play`. This deliberately favors protocol compatibility and bounded complexity over low-latency streaming; true live streaming can be evaluated later against renderer behavior.
7. **TTS lifecycle and UX** — handle interruption, playback completion, Wi-Fi loss, buffer limits/PSRAM exhaustion, output selection, and microphone/AEC policy. Keep the microphone capture and ES8311 input path independent from the selected speaker output.

## Integrated audio-output path

Set `CONFIG_DLNA_AUDIO_OUTPUT=y` for the Waveshare ESP32-S3-Touch-AMOLED-1.32 target to enable the integrated sink. This option is mutually exclusive with the standalone `CONFIG_DLNA_SONY_X88_SMOKE_TEST`, because both use the same HTTP media-server port.

When Wi-Fi connects, `Application` starts `DlnaPlaybackManager` with the station IP reported by `WifiManager`. A worker performs SSDP discovery and accepts only a renderer whose identity contains `SRS-X88`. Until the renderer is discovered, AudioService continues using the local ES8311 output.

The manager installs the AudioService PCM callback for the complete decoded playback path. This includes server speech, `PlaySound()` prompt assets, and notification audio; there is no TTS-only routing filter. The callback only copies PCM into one of two bounded accumulators. The existing playback-drained callback finalizes the WAV header and queues the immutable resource. A dedicated worker owns discovery, HTTP media publication, and SOAP control. Queue saturation uses an independent recycle bitmask, so a failed resource enqueue cannot permanently strand a PCM accumulator. Worker teardown is cooperative: the worker stops AVTransport, shuts down its HTTP server, releases the active WAV, and signals completion before its owner releases the queue.

The manager keeps the current WAV allocation alive while the renderer may fetch it. When a new utterance replaces the current one, it first sends best-effort AVTransport Stop, stops the old HTTP server, and only then releases the previous WAV allocation. Wi-Fi disconnect disables the sink and schedules server/resource cleanup; reconnect updates the host address and triggers rediscovery.

Each utterance keeps one output choice from its first PCM frame through drain: if DLNA is not ready or a buffer is unavailable at the beginning, the whole utterance stays on the local codec. If a DLNA-buffered utterance exceeds its configured limit, it is discarded rather than splitting the utterance between DLNA and local output. The current default is 512 KiB per PCM accumulator, equivalent to roughly 10.9 seconds at 24 kHz / 16-bit mono. Longer uninterrupted utterances exceed this initial safety limit and are discarded; increasing the limit or adding segmented playback is a follow-up optimization.

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

## Optional Sony SRS-X88 smoke test

Enable `CONFIG_DLNA_SONY_X88_SMOKE_TEST=y` for the
`echoear-waveshare-1.32` build to launch a one-shot test after Wi-Fi connects.
The worker searches specifically for an identity containing `SRS-X88`; it will
not send playback commands to another renderer. It generates a 2-second,
44.1 kHz / 16-bit / stereo PCM sine-wave WAV in heap memory, serves it over HTTP,
then issues AVTransport `SetAVTransportURI` and `Play`. The media server remains
available for 15 seconds before a best-effort `Stop` and cleanup.

The option defaults to disabled. It is intended only for transport diagnostics.
A successful SOAP response means the renderer accepted the control request; it
does not prove that the speaker produced audible sound. Confirm playback by
listening to the SRS-X88 and checking the serial logs. The test runs in a worker
task and does not intercept TTS PCM or alter the local ES8311 audio path.

## Current limitations

- SSDP discovery, UPnP device-description parsing, and a first AVTransport SOAP control client are implemented. SOAP control is not yet validated against a physical renderer; the audio sink routing remains unimplemented. The HTTP endpoint is implemented but not yet tested against the SRS-X88.
- The DLNA modules, integrated playback manager, and optional smoke-test worker have not yet been confirmed compiled with the project's ESP-IDF 6.1.x environment or validated on hardware.
- The integrated manager routes all decoded playback sounds when enabled, while retaining local-output fallback when the renderer is not ready at the start of an utterance. ESP-IDF 6.1.x compilation and physical SRS-X88 playback validation are still pending.
- The Sony SRS-X88 must be on the same LAN/VLAN and have network standby / network control enabled as applicable.
