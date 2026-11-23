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
4. **AVTransport control** — `DlnaControl` sends `SetAVTransportURI`, `Play`, `Stop`, and `GetTransportInfo` SOAP actions with bounded response capture and XML-escaped DIDL-Lite metadata. Calls are blocking and must run in a worker task. The manager polls `CurrentTransportState` and retains each HTTP/WAV resource until it observes PLAYING followed by STOPPED. If the renderer does not expose a usable state sequence, it uses the WAV byte-rate/data-length duration plus a 5-second safety margin (capped at 120 seconds).
5. **PCM output abstraction** — implemented. The optional `AudioServiceCallbacks::on_pcm_output` hook runs after Opus decode and receives PCM plus the output sample rate. Returning `true` means the frame was handled; `false` preserves the existing ES8311/local codec path. The callback must not perform network I/O.
6. **Utterance buffering and publication** — implemented. `DlnaPcmBuffer` provides a bounded, mutex-protected PCM segment accumulator and WAV finalizer. The DLNA sink converts decoded PCM to 32 kHz, 16-bit PCM before buffering, matching the lowest published SRS-X88 network WAV/Linear PCM sample rate. It preserves the incoming channel count (normally mono), and preallocates the resampler output buffer before audio callbacks run. Each accumulator caps memory at 512 KiB, prefers PSRAM, and transfers each immutable WAV allocation to the media server without copying PCM. `DlnaPlaybackManager` owns two accumulators and splits a long utterance into sequential WAV segments when a segment reaches capacity. Segment publication, discovery, HTTP and SOAP control run on a worker. Queue saturation has an independent buffer-recycle path.
7. **Playback lifecycle and routing** — integrated for all decoded playback audio, including server speech, `PlaySound()` assets, and notification audio. Each utterance keeps one output choice: local output if the sink is unavailable at its first frame, or DLNA buffering otherwise. Wi-Fi loss disables the DLNA sink before taking the PCM-state lock, invalidates the current playback generation, and triggers worker cleanup. Reconnect also verifies that the codec sample rate/channel format matches the converter initialized for the worker; unexpected format changes are rejected rather than silently using a stale converter. PCM is discarded until drain only when a DLNA-routed utterance was already in progress; an idle disconnect does not leave a stale drain guard that can swallow the first utterance after reconnect. Reconnect triggers rediscovery. The microphone capture and ES8311 input path remain independent from speaker output. Renderer playback completion is tracked by AVTransport polling; queued utterances are published serially so a new utterance does not immediately stop and replace the current one.

## Integrated audio-output path

Set `CONFIG_DLNA_AUDIO_OUTPUT=y` for the Waveshare ESP32-S3-Touch-AMOLED-1.32 target to enable the integrated sink. This option is mutually exclusive with the standalone `CONFIG_DLNA_SONY_X88_SMOKE_TEST`, because both use the same HTTP media-server port.

When Wi-Fi connects, `Application` starts `DlnaPlaybackManager` with the station IP reported by `WifiManager`. A worker performs SSDP discovery and accepts only a renderer whose identity contains `SRS-X88`. Until the renderer is discovered, AudioService continues using the local ES8311 output.

The manager installs the AudioService PCM callback for the complete decoded playback path. This includes server speech, `PlaySound()` prompt assets, and notification audio; there is no TTS-only routing filter. The callback only copies PCM into one of two bounded accumulators. The playback-drained callback finalizes the WAV header and queues the immutable resource. When AudioService resets or stops with queued/in-flight playback, a separate abort callback discards the partial utterance; any final in-flight PCM frame is consumed and discarded until drain. A dedicated worker owns discovery, HTTP media publication, and SOAP control. Queue saturation uses an independent recycle bitmask, so a failed resource enqueue cannot permanently strand a PCM accumulator. Worker teardown is cooperative: the worker stops AVTransport, shuts down its HTTP server, releases the active WAV, and signals completion before its owner releases the queue.

The manager keeps the current WAV allocation alive while the renderer may fetch it. Completed utterances are handled serially by the worker. The active WAV and HTTP endpoint remain alive while the renderer is playing; after AVTransport reports STOPPED (or the duration-based safety deadline expires), the worker stops the endpoint and releases the allocation before publishing the next queued utterance. Network loss and worker shutdown still use best-effort Stop followed by endpoint/resource cleanup. Wi-Fi disconnect disables the sink and schedules server/resource cleanup; reconnect updates the host address and triggers rediscovery.

Each utterance keeps one output choice from its first PCM frame through drain: if DLNA is not ready or a buffer is unavailable at the beginning, the whole utterance stays on the local codec. Long utterances are split into consecutive WAV segments at the 512 KiB PCM limit (roughly 8.2 seconds at 32 kHz / 16-bit mono). Segment boundaries preserve PCM sample-frame alignment. The audio callback only finalizes/enqueues a segment and switches to the other preallocated buffer; it never performs network I/O or waits for the renderer. Aborting playback increments a generation counter so stale queued segments are discarded and active playback is stopped. The worker plays segments in FIFO order and retains each segment's HTTP resource until playback completion. Every segment carries an utterance generation; an AudioService abort invalidates queued segments from that utterance and causes an active DLNA segment to be stopped and cleaned up after the current bounded SOAP request returns. This remains a bounded pipeline: with two accumulators, if the renderer is slower than incoming audio long enough that both buffers are occupied, the remaining tail of that utterance is dropped rather than mixed into local speaker output. Queue saturation can also drop a segment.

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

The `wav_data` buffer must remain valid and unchanged until `media.Stop()` returns. The example assumes the renderer has already been discovered and its device description parsed. It remains a standalone transport diagnostic; the integrated AudioService path is implemented separately.

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

## Continuous-streaming investigation

The SRS-X88 Help Guide documents playback of music stored on a DLNA server and lists supported file formats, but it does not specify support for progressive/infinite HTTP resources, HTTP chunked transfer encoding, unknown-length WAV streams, or append-only resources. The existing renderer integration therefore continues to use finite WAV resources with explicit lengths. A continuous HTTP stream must remain an experimental, separately validated transport path until the X88's actual HTTP request behavior is observed; do not replace the finite-resource path based only on generic HTTP or DLNA assumptions.

The current 512 KiB segment size is a memory/latency compromise, not a claim that the X88 supports gapless transitions between separate AVTransport resources. The controller currently issues a new `SetAVTransportURI` and `Play` for every segment, so a transition gap is expected.

## Current limitations

- SSDP discovery, UPnP device-description parsing, AVTransport SOAP control, the WAV HTTP endpoint, and the integrated PCM routing path are implemented. SOAP control and media delivery are not yet validated against a physical SRS-X88.
- The DLNA modules, integrated playback manager, and optional smoke-test worker have not yet been confirmed compiled with the project's ESP-IDF 6.1.x environment or validated on hardware.
- The integrated manager routes all decoded playback sounds when enabled, while retaining local-output fallback when the renderer is not ready at the start of an utterance. AVTransport polling behavior and duration-based fallback still require validation against a physical SRS-X88.
- The worker serializes resource publication and waits for playback completion before serving the next queued segment. The two PCM accumulators provide bounded look-ahead while a segment is playing; if the renderer cannot keep pace and both are occupied, the remainder of the current utterance is dropped. This is bounded buffering, not an unlimited stream.
- ESP-IDF 6.1.x compilation and physical SRS-X88 playback validation are still pending.
- The Sony SRS-X88 must be on the same LAN/VLAN and have network standby / network control enabled as applicable.
- Sony's published network playback format list includes WAV/Linear PCM sample rates from 32 kHz upward; the integrated path therefore resamples the decoder output to 32 kHz PCM16 instead of emitting a possible 24 kHz stream. This choice follows published specifications and still requires confirmation with physical DLNA playback.
