# DLNA playback pipeline: asynchronous resource design

## Purpose

This document defines the next playback-manager architecture needed to reduce the gap between finite WAV segments without weakening the existing resource-lifetime guarantees. It is a design boundary, not a claim that the Sony SRS-X88 supports gapless playback.

## Why the current worker cannot preload

The current worker calls `ProcessResource()`, which starts one HTTP endpoint, issues `SetAVTransportURI` + `Play`, then blocks in `WaitForPlaybackCompletion()`. While blocked, it cannot:

- inspect the resource queue and prepare the next segment;
- issue a next-resource SOAP action while the current item is playing;
- service a renderer state machine independently from resource publication.

The current `DlnaMediaServer` serves exactly one immutable WAV buffer at `/dlna/audio.wav`. Its caller owns that buffer and must retain it until `Stop()` returns. Replacing the pointer or stopping the server at a segment boundary is unsafe while the renderer may still be issuing GET or Range requests.

Three PCM accumulators only absorb temporary delay. They do not make the worker concurrent and do not provide an unlimited stream.

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

The existing implementation has SCPD capability detection but still uses a single-resource server and a blocking sequential worker. This document deliberately does not claim gapless playback or enable `SetNextAVTransportURI`. The ESP-IDF build and physical SRS-X88 behavior remain unverified.

## Implementation progress

- Added `DlnaResourceRegistry` as a bounded two-entry ownership primitive.
- `Publish()` transfers WAV-buffer ownership only on success and creates a generation/sequence URL path.
- `Acquire()/Release()` protect bytes while an HTTP handler is reading them. Each concurrent acquisition receives a distinct lease token (bounded to eight active handlers), so a duplicate release cannot consume another handler's reference.
- `Retire()` prevents new acquisitions and defers freeing until outstanding leases reach zero.
- The new source is included only in the Waveshare 1.32 DLNA board build.

This is the first isolated implementation step. The registry is not yet connected to `DlnaMediaServer` or `DlnaPlaybackManager`; the existing single-resource playback behavior remains unchanged. The server integration must ensure its HTTPD task has stopped and all handler leases have been released before the registry is destroyed.
