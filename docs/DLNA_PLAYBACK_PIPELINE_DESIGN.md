# DLNA playback pipeline: asynchronous resource design

## Purpose

This document records the implemented DLNA playback architecture and its remaining validation work. It is not a claim that the Sony SRS-X88 supports gapless playback; that still requires physical-device evidence.

## Why the current worker cannot preload

The current worker uses a persistent registry-backed HTTP server and a two-resource AVTransport window. `ProcessResource()` polls transport state while retaining the current WAV and preparing one look-ahead WAV. When the renderer advertises `SetNextAVTransportURI`, the worker attempts preloading and observes `CurrentURI` before retiring the previous resource. If preloading is unavailable, rejected, or not confirmed, it uses a sequential `SetAVTransportURI` + `Play` fallback. Three PCM accumulators and a bounded resource queue absorb temporary producer/consumer skew; they do not provide an unlimited stream. The legacy single-WAV media-server API remains available but is not the active playback path.

## Required capability gates

1. Parse the renderer's AVTransport SCPD and retain the advertised action capability. The current discovery path detects `SetNextAVTransportURI`; absence or failed SCPD retrieval means unsupported/unknown.
2. Do not call `SetNextAVTransportURI` unless the SCPD explicitly advertises it.
3. An advertised action is necessary but not sufficient. The X88 must be tested to establish whether it accepts the action while another resource is playing, fetches the next URI before the current track ends, and transitions correctly.
4. If the action is absent or rejected, retain the existing finite-resource sequential path. Do not silently switch to chunked, unknown-length, or append-only WAV delivery.

## Target architecture

Separate three lifetimes that are currently coupled:

- **PCM accumulator**: writable only by `WritePcm()`; becomes immutable when finalized.
- **HTTP resource**: owns one immutable WAV allocation and a stable URI; remains available until the renderer no longer needs it.
- **AVTransport item**: represents the renderer's current or queued item; advances independently of the HTTP request lifecycle.

The HTTP server should be a single long-lived server with a bounded resource registry, rather than one server instance per segment. Each published resource receives a unique path, for example `/dlna/<generation>/<sequence>.wav`. The registry maps that path to an immutable resource record. A request handler must acquire a reference to the record before reading bytes; removal marks it retired and frees it only after no handler is using it and the controller has determined that the renderer no longer needs it.

The registry must be bounded. It should have a small explicit maximum (initially two live resources: current and next), reject publication when full, and never overwrite a resource still referenced by a request or AVTransport item.

## Worker state machine

The worker becomes an event-driven loop with bounded waits. It must not wait for the entire track duration inside a single function.

Suggested states:

- `kOffline`: network unavailable; no renderer or HTTP resources are considered usable.
- `kDiscovering`: SSDP and description/SCPD discovery in progress.
- `kIdle`: renderer ready, no active AVTransport item.
- `kStartingCurrent`: publish current resource, issue `SetAVTransportURI`, then `Play`.
- `kPlayingCurrent`: poll transport state on a short cadence while also checking abort, disconnect, queue, and preload eligibility.
- `kSettingNext`: only when the action is advertised and runtime validation has enabled it; publish next resource and issue `SetNextAVTransportURI`.
- `kWaitingTransition`: retain both current and next resources while observing the renderer transition.
- `kRecovering`: stop best-effort, retire resources safely, and rediscover if necessary.
- `kStopping`: stop AVTransport, stop HTTP service, wait for in-flight HTTP handlers to leave, then free all resources.

Each iteration performs bounded work: process disconnect/abort flags, retire eligible resources, inspect queue, advance transport state, and perform at most one bounded SOAP/HTTP operation. No state may spin waiting for playback duration.

## Resource ownership and release rules

- Ownership transfers from a finalized PCM buffer to a resource record exactly once.
- A published resource is immutable. Its bytes and URI cannot change.
- A resource currently assigned as AVTransport current or next cannot be retired merely because its HTTP GET completed.
- HTTP GET completion only proves that one transfer ended; it does not prove the renderer has finished decoding or playing the file.
- The current resource may be retired only after a confirmed transition away from it (or a confirmed stop/abort), no active HTTP handler references remain, and the renderer no longer reports it as current.
- A queued next resource remains retained until it either becomes current and later completes, or the controller confirms it was not accepted and it is safe to retire.
- On disconnect, stop the HTTP server before freeing resource bytes. The server stop operation must guarantee handlers have exited; if that guarantee cannot be established, use per-resource reference counts and defer freeing.

## Queueing, overflow, and routing invariants

- Keep the existing utterance-level routing decision: one utterance is entirely DLNA or entirely local. Never send a tail to ES8311 after earlier frames were accepted for DLNA.
- Keep the PCM callback bounded and free of network I/O.
- Keep the resource queue and resource registry bounded.
- If no free accumulator exists before the first frame of an utterance, route the whole utterance locally.
- If capacity is exhausted after DLNA accepted the first frame, drop the remainder of that utterance and report it; do not mix outputs.
- A resource publication failure must return its buffer slot through an out-of-band recycle path, independent of the full resource queue.
- Generation changes invalidate queued and active resources from an aborted utterance. The worker performs network cleanup; the audio callback never waits for it.

## Implementation sequence

1. Keep SCPD capability detection and add explicit logs for missing/failed SCPD and the detected action.
2. Add a unit-testable resource registry abstraction: unique URI key, bounded capacity, immutable record, request reference acquisition/release, retire/deferred-free semantics.
3. Extend the HTTP server to serve multiple registered resource paths while preserving GET, HEAD, and single-range behavior.
4. Replace blocking `ProcessResource()/WaitForPlaybackCompletion()` with the worker state machine while initially retaining sequential `SetAVTransportURI` + `Play`. This isolates lifetime/concurrency changes from gapless behavior.
5. Add `SetNextAVTransportURI` SOAP serialization and scheduling behind the SCPD capability gate.
6. Enable next-resource scheduling only after physical X88 tests verify action acceptance, prefetch timing, transition behavior, repeated transitions, abort, disconnect, and resource release.
7. Keep the old sequential path as fallback for renderers without validated next-resource support.

## Validation matrix

| Scenario | Required result |
|---|---|
| Current item plays, next item queued | Current resource remains readable; next is prepared without stopping current |
| Renderer fetches next before current ends | Both resources remain valid concurrently |
| Next action missing/rejected | Sequential fallback; no resource is freed early |
| GET completes but item still playing | Current resource remains retained |
| Abort during current playback | Generation invalidates queued work; stop and retire only after HTTP handlers exit |
| Wi-Fi disconnect during GET | Server shutdown precedes resource free; no stale pointer |
| Queue/registry full | Bounded drop/fallback behavior; no overwrite or leak |
| Repeated segment transitions | No stale URI, premature free, or unbounded resource growth |

## Current status

The implementation has SCPD capability detection, registry-backed multi-resource HTTP serving, a current-plus-next playback window, capability-gated `SetNextAVTransportURI`, sequential fallback, HTTP request telemetry, and bounded resource cleanup. It does not claim gapless playback. A complete ESP-IDF build and physical SRS-X88 behavior remain unverified.

## Implementation progress

- Added `DlnaResourceRegistry` as a bounded two-entry ownership primitive.
- `Publish()` transfers WAV-buffer ownership only on success and creates a generation/sequence URL path.
- `Acquire()/Release()` protect bytes while an HTTP handler is reading them. Each concurrent acquisition receives a distinct lease token (bounded to eight active handlers), so a duplicate release cannot consume another handler's reference.
- `Retire()` prevents new acquisitions and defers freeing until outstanding leases reach zero.
- The new source is included only in the Waveshare 1.32 DLNA board build.

The registry is integrated with both `DlnaMediaServer` and `DlnaPlaybackManager`. HTTPD shutdown must complete before the registry is destroyed; if shutdown fails, the worker retains its context and retries rather than freeing memory that handlers may still reference.

## Multi-resource HTTP server implementation progress

- Added a registry-backed `DlnaMediaServer::Start(host, registry, port)` overload.
- The registry-backed server uses ESP-IDF HTTPD wildcard matching for `/dlna/*`, allowing generation/sequence-specific resource URLs while keeping one HTTP server alive.
- GET, HEAD, and single byte-range responses use the same immutable resource view.
- Each request acquires a unique registry lease and releases it through a scope guard on every return path.
- Unknown or retired paths return 404. Legacy single-WAV `Start(host, data, size)` remains available for the existing playback manager.
- Registered WAV sizes are bounded to the signed response length accepted by the HTTP server API.

The live playback manager uses this registry-backed overload. The server stays active across segments in one renderer/network session, enabling concurrent current/next URLs. This enables preloading but does not prove gapless playback.

## Playback manager integration progress

- `DlnaPlaybackManager` now owns a `DlnaResourceRegistry` whose lifetime extends beyond the HTTP server; member declaration order ensures the server is destroyed/stopped before the registry.
- The worker starts the registry-backed HTTP server on the first segment and reuses it for later segments in the same renderer/network session.
- Each finalized WAV is published under its generation/sequence path. SOAP `SetAVTransportURI` receives that exact URL.
- The registry takes WAV ownership only after successful Publish. On Publish failure, the manager frees the still-owned allocation. On success, Retire is the only normal release path.
- Playback completion retires the current resource without stopping the HTTP server. In-flight GET/HEAD/Range requests retain their leases, so bytes are freed only after request completion.
- Network-loss and worker-shutdown paths stop HTTPD before retiring all resources. The manager's `current_wav_` is now a non-owning pointer used only to calculate the playback duration while the registry owns the allocation.

This is persistent multi-URL serving with a current-plus-next AVTransport controller. The worker can issue `SetNextAVTransportURI` while the current item is playing when SCPD advertises support. The actual prefetch and transition behavior still requires SRS-X88 validation.

## SetNextAVTransportURI control groundwork

- Added `DlnaControl::SetNextUri()`, including DIDL-Lite metadata and SOAP invocation.
- The method refuses to send the action unless the parsed renderer SCPD explicitly advertised `SetNextAVTransportURI`.
- `SetNextUri()` is called by the playback manager only when the renderer SCPD explicitly advertises the action; rejected or unconfirmed transitions fall back to sequential playback.
- The ESP-IDF 6.1 HTTP server implementation was checked: `httpd_stop()` waits until the HTTPD task reaches `THREAD_STOPPED` before deleting the server instance. The media server now preserves its handle/context if `httpd_stop()` reports failure rather than pretending handlers have exited.

## Current-next playback pipeline implementation

The playback manager has now been advanced beyond the earlier blocking sequential worker:

- `ProcessResource()` starts the current item, then continues polling AVTransport while also draining one queued WAV segment as a look-ahead item.
- The current and next WAVs are published simultaneously in the two-entry registry. The current resource is not retired until the renderer's `CurrentURI` is observed changing to the preloaded next URI, or a sequential fallback has successfully started the next item.
- When the SCPD advertises `SetNextAVTransportURI`, the worker attempts it once after observing the current item in PLAYING state. A rejected action is not retried in a tight loop.
- Added `GetMediaInfo` / `CurrentURI` querying to confirm the actual transition rather than assuming that a successful SOAP response means playback has switched.
- If the next action is unavailable or rejected, the worker retains the next resource and uses `SetAVTransportURI` + `Play` after the current item stops.
- During a possible next-item transition, both resources remain retained for a short observation window before sequential fallback, avoiding immediate replay if the renderer's state temporarily reports STOPPED.

This is now a capability-gated preloading implementation, but it is not yet a verified gapless implementation. The X88 may accept the action but fetch the next file too late, reject it during playback, or report its CurrentURI differently. Those behaviors require hardware validation. If URI observation fails, the conservative fallback may introduce a gap; it must not free the current resource early.

The worker still handles at most one look-ahead item because the registry is deliberately bounded to two resources (current + next). Additional finalized segments remain in the bounded FreeRTOS queue until a slot becomes available. A failed Publish retains the unpublished next WAV and retries; after the current item completes, the completed URI is retired and the retry is bounded by the HTTP lease-drain window. No live resource is overwritten.

- If the registry is temporarily full because an older retired resource still has an active HTTP lease, the manager retains the unpublished next WAV and retries publication. It does not overwrite a live resource or drop the queued segment merely because the old HTTP request has not drained. The completed current resource remains retained until publication can proceed.

## SOAP transition observation and worker watchdog

- SOAP response extraction now matches XML local element names, so `CurrentURI` and `CurrentTransportState` can be read whether the renderer includes an element namespace prefix or not. Extracted text is XML-entity-decoded before URI comparison.
- Playback now has an absolute duration watchdog (`WAV duration + 15 seconds`, capped at 180 seconds). It applies even if PLAYING was observed earlier, preventing a renderer that stops answering status queries or remains stuck in PLAYING from permanently blocking the worker. The watchdog is a recovery bound, not a claim that the renderer actually completed playback.

## Host-side registry regression tests

A standalone host test is available at `scripts/tests/test_dlna_resource_registry.py`. It compiles the production `dlna_resource_registry.cc` against minimal ESP heap/log stubs, then runs ownership and lease tests under AddressSanitizer and UndefinedBehaviorSanitizer. It covers independent leases, retirement while requests are active, capacity exhaustion, duplicate paths, lease-table exhaustion, and failed-Publish ownership semantics. Run it with `python3 scripts/tests/test_dlna_resource_registry.py` on a host with a C++ compiler; the dedicated `DLNA Host Tests` workflow runs on feature-branch pushes that change DLNA source, test scripts, build configuration, or the workflow itself.

## SOAP XML helper regression coverage

AVTransport SOAP escaping and response extraction now share the header-only `dlna_xml.h` helper. Host tests cover all five predefined XML entities, namespaced and unprefixed elements, URI query ampersands, missing/malformed/empty elements, and one-pass entity decoding. `SetNextAVTransportURI` DIDL-Lite metadata was also corrected to use normal XML attribute quoting (without literal backslashes), matching the initial `SetAVTransportURI` metadata structure. Both the registry ownership suite and XML helper suite run under AddressSanitizer and UndefinedBehaviorSanitizer in the dedicated host-test workflow.

- Current and next AVTransport items now share one tested `BuildDidlMetadata()` implementation, preventing their DIDL-Lite serialization from drifting. Regression tests assert valid quoted XML attributes, no literal backslashes, and correct escaping of title and media URI.
- If the playback watchdog expires without a confirmed STOPPED state and there is no next item to replace the current one, the manager explicitly sends Stop before retiring the last resource. If Stop itself fails, it disables the sink, stops the HTTP server, and retires the registry rather than silently assuming playback ended.


## Registry-pressure recovery and renderer HTTP telemetry

- If the current AVTransport item completes while the queued look-ahead WAV is still unpublished, the worker no longer retries indefinitely while retaining the completed current resource. It retires the completed URI, gives outstanding HTTP leases a bounded 3-second drain window, and then either starts the next item sequentially or drops it with an explicit error. If the playback watchdog expires without a confirmed STOPPED state, the worker first sends Stop; it does not retire the current resource on an unconfirmed timeout.
- The registry still owns every successfully published WAV. Retiring a URI only prevents new acquisitions; an in-flight GET/HEAD/Range lease keeps the allocation alive until its handler exits. If leases do not drain within the bounded retry window, the next WAV is released by its still-owning worker path and playback exits rather than spinning forever or overwriting a live entry.
- Registry-backed HTTP requests now log method, resource path (including generation/sequence), HTTP status, requested Range header, successfully sent byte count, and handler elapsed time. These logs are intended to characterize real SRS-X88 behavior: HEAD probes, full GETs, byte-range reads, repeated fetches, and whether a next resource is requested before the current item ends.
- The telemetry is observational only. No claim is made about the X88's prefetch or transition behavior until logs from a physical device are available.


- HTTP server shutdown failure is now treated as an ownership boundary, not a log-only error. The worker checks whether the server handle remains active before calling `RetireAll()`; on failure it retries shutdown while keeping the manager and registry alive. The worker does not signal its completion semaphore until HTTPD has stopped, preventing destruction of the handler context while a request may still reference it. This deliberately prefers a delayed shutdown over a use-after-free.


- Transition handling now distinguishes a confirmed next-item URI from a confirmed PLAYING state. When `GetMediaInfo` reports the preloaded next URI but AVTransport is still `TRANSITIONING` (or temporarily reports STOPPED), the worker retains both WAV resources and allows up to 10 seconds from the accepted `SetNextAVTransportURI` action for the renderer to enter PLAYING. It no longer immediately overwrites an already-selected next URI with a second `SetAVTransportURI` merely because PLAYING has not appeared yet. If the renderer never starts the item within that bounded window, the existing sequential recovery remains available.


## PCM segmentation and producer/consumer budget

- The default PCM accumulator capacity is now 256 KiB per segment (excluding the 44-byte WAV header). At the fixed 32 kHz / 16-bit output format this represents approximately 4.096 seconds of mono audio or 2.048 seconds of stereo audio.
- The previous 512 KiB default represented approximately 8.192 seconds mono / 4.096 seconds stereo. Halving the segment size reduces the amount of PCM accumulated before the first full segment is queued and reduces each detached WAV allocation, at the cost of more frequent AVTransport transitions.
- This is a transport segmentation change only: sample rate, sample format, channel count, and PCM samples are unchanged. It does not reduce audio fidelity.
- Each queued segment carries its calculated duration as metadata. The DLNA worker logs dequeued and look-ahead segment durations plus queue depth; queue-full and callback issues are counted atomically and logged by the worker. The AudioOutputTask callback and PCM buffer finalization do not perform synchronous logging.
- The callback remains non-blocking with respect to network operations. It can still drop the remainder of an utterance if all three accumulators are occupied; the worker must consume and recycle segments quickly enough to keep pace with the real-time PCM producer.
- `AudioOutputTask` now applies media-time pacing only when the DLNA callback accepts a frame. Local codec output remains paced by I2S as before. This prevents a faster-than-real-time network/decoder producer from filling all three accumulators immediately; when DLNA queues fill, existing bounded decode/input queues provide upstream backpressure instead of silently outrunning the renderer.
- A stalled-playback watchdog now sends Stop before sequential fallback unless a recently accepted next-item transition has already been observed. This prevents a timed-out current item from being overwritten while it may still be playing, while preserving the bounded transition grace period.

- Host regression coverage for `DlnaPcmBuffer` verifies PCM payload preservation, WAV header fields, the 256 KiB default capacity, and rejection of an over-capacity append. The `DLNA Host Tests` workflow runs registry, XML, and PCM-buffer tests. Push/PR runs are path-filtered to DLNA source, DLNA tests, build configuration, and the workflow; ordinary documentation-only commits do not trigger the workflow.
