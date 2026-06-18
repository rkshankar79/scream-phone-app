# SCReAM Phone App (SPA) — SCReAM / L4S Network Test Tool (Android)

A real-time **network testing and visualization tool** built around Ericsson's
SCReAM congestion-control engine. The app runs SCReAM on Android, generates
RTP/UDP test traffic, exchanges RFC 8888 RTCP feedback, and visualizes the
congestion-control state live.

> **Primary goal:** this is a measurement/visualization tool for SCReAM and
> future **L4S** evaluation — *not* a video streaming app. Traffic is synthetic.

---

## 1. Status at a glance

| Capability | State |
|---|---|
| Android SCReAM **sender** (synthetic RTP/UDP, pacing, live metrics) | ✅ Working |
| **RTCP feedback receive** + congestion control (CWND/RTT/queue delay) | ✅ Working |
| Android SCReAM **receiver** (RFC 8888 feedback generation) | ✅ Working |
| **On-device loopback** (sender ↔ receiver in one app) | ✅ Verified on device |
| Live dashboard (custom Compose Canvas charts) | ✅ Working |
| ECN/L4S CE-mark reception (`EcnManager`) | ⏳ Deferred (ECN-ready, not yet live) |
| Role selector + receiver under foreground service | ⏳ TODO |
| Exact pacing-rate metric | ⏳ Approximated (loopback artifact) |

**Verified:** the full native layer (sender + receiver) compiles and links with
clang on host; Kotlin/C++ lint clean; APK builds in Android Studio
(`BUILD SUCCESSFUL`); CWND ramp + sawtooth observed on-device over loopback.

---

## 2. What is SCReAM (quick context)

SCReAM (**S**elf-**C**locked **R**at**e** **A**daptation for **M**ultimedia) is
an IETF congestion-control algorithm (RFC 8298 → SCReAM V2 draft) optimized for
real-time media over wireless. It is **window-based and self-clocked** (like
TCP): a congestion window limits bytes in flight, ACK feedback clocks out new
packets, and a sender-side RTP queue absorbs transient rate drops. It combines
three congestion signals — **queue delay** (primary), **loss**, and **ECN/L4S**
marking — and supports **L4S** (scalable, DCTCP-style response to ECT(1)/CE).

Upstream source: `https://github.com/EricssonResearch/scream` (cloned into
`./scream/`, reference only, **not part of the app build**).

---

## 3. Architecture

Layered so the vendored Ericsson code stays pristine and the transport/JNI code
never depends on a concrete engine.

```
Kotlin / Compose UI  ─▶  ScreamService (foreground + wakelock)
        │                      │
        └─ ScreamEngine (StateFlow metrics, poll loop) ─▶ ScreamBridge (JNI)
                                                              │
══════════════════════════════ JNI boundary ═════════════════│════════════════
                                                              ▼
   wrapper/   TestSession (sender: sockets, threads, pacing, traffic gen)
              ReceiverSession (receiver: recv loop, feedback send)
              *_jni.cpp (JNI entry points; depend ONLY on interfaces + sessions)
                                                              │
   controllers/  ScreamController  ─┐   ScreamReceiver  ─┐    │ (adapters: the
                                    │                    │    │  ONLY place that
   interfaces/  CongestionController│   FeedbackReceiver │    │  includes
                MetricsSnapshot     │                    │    │  scream_core)
                                    ▼                    ▼
   scream_core/  ScreamV2Tx / ScreamRx / RtpQueue   (UNTOUCHED Ericsson code)
```

### Layer rules (important)
- `scream_core/` — **never edit.** Vendored upstream. All metrics come through
  its existing public API. Forward-declared (not `#include`d) above the adapters.
- `interfaces/` — pure abstract contracts + POD structs. No engine types.
- `controllers/` — concrete adapters mapping interfaces onto SCReAM. The only
  translation units that include `scream_core/`.
- `wrapper/` — sockets, threads, JNI. Depends on `interfaces/` (+ `controllers/`
  for construction), never on `scream_core/` directly.

---

## 4. Directory layout

```
ScreamPhoneApp/
├── PROJECT.md                      ← this file
├── settings.gradle.kts, build.gradle.kts, gradle.properties
├── scream/                         ← upstream clone (reference only; gitignored)
└── app/
    ├── build.gradle.kts            ← AGP 8.5.2, Kotlin 1.9.24, Compose, NDK/CMake
    └── src/main/
        ├── AndroidManifest.xml     ← INTERNET, FOREGROUND_SERVICE, WAKE_LOCK…
        ├── cpp/
        │   ├── CMakeLists.txt      ← builds libscream.so (arm64-v8a, x86_64)
        │   ├── scream_core/        ← UNTOUCHED: ScreamTx/V2Tx/V2TxStream,
        │   │                          ScreamRx, RtpQueue (.h/.cpp)
        │   ├── interfaces/
        │   │   ├── CongestionController.h
        │   │   ├── MetricsSnapshot.h
        │   │   └── FeedbackReceiver.h
        │   ├── controllers/
        │   │   ├── ScreamController.{h,cpp}   ← ScreamV2Tx adapter (+ packet_free)
        │   │   └── ScreamReceiver.{h,cpp}     ← ScreamRx adapter
        │   └── wrapper/
        │       ├── TestSession.{h,cpp}        ← sender pipeline
        │       ├── ReceiverSession.{h,cpp}    ← receiver pipeline
        │       ├── scream_sender_jni.cpp
        │       └── scream_receiver_jni.cpp
        ├── java/com/spa/scream/
        │   ├── ScreamBridge.kt      ← JNI bindings + Metrics/ReceiverMetrics/Config
        │   ├── ScreamEngine.kt      ← session owner, StateFlow, poll loop, history
        │   ├── ScreamService.kt     ← foreground service + partial wakelock
        │   ├── MainActivity.kt      ← Compose UI (config + dashboard + RX card)
        │   └── ui/LineChart.kt      ← dependency-free Canvas line chart
        └── res/values/             ← strings, theme
```

---

## 5. Data flow

### Sender (`TestSession`)
1. Traffic generator (50 fps default) sizes each frame from
   `getTargetBitrate()`, builds 12-byte RTP headers + filler payload, and
   `enqueueRtp()` + `newMediaFrame()` into SCReAM's RTP queue.
2. Pacing thread: `isOkToTransmit()` → `dequeueRtp()` → UDP `sendto()` →
   `addTransmitted()`.
3. RTCP thread: `recvfrom()` → `incomingFeedback()` (RFC 8888) → triggers CWND
   update.
4. Metrics polled ~8 Hz across JNI into a `StateFlow<Metrics>`.

### Receiver (`ReceiverSession`)
1. UDP socket bound to a listen port; recv thread parses the RTP header.
2. `onRtpPacket()` → `ScreamRx::receive()`.
3. `buildFeedback()` → `ScreamRx::createStandardizedFeedback()` (self-gates
   cadence) → RTCP `sendto()` **back to the packet's source address** (so
   loopback needs no port pinning).

The sender's `incomingStandardizedFeedback` parses exactly the framing the
receiver's `createStandardizedFeedback` emits (RTCP FMT=11/PT=205, length at
`buf+2`, SSRC at `buf+4`) — they interoperate directly.

---

## 6. Metrics

Decoded from a native `double[]` (`ScreamBridge.METRICS_SIZE = 16`).

| Metric | Source (untouched core API) | Notes |
|---|---|---|
| CWND [B] | `getCwnd()` | |
| RTT [s] | `getSRtt()` | smoothed |
| TX bitrate [bps] | `getStatisticsItem(AVG_RATE)` | slow filter; lags during ramp |
| Target bitrate [bps] | `getTargetBitrate()` | tracked from generator path |
| Queue delay [s] | `getStatisticsItem(AVG_QUEUE_DELAY)` | network delay |
| RTP queue delay [s] | `RtpQueue::getDelay()` | sender-side |
| Loss [%] | `getStatisticsItem(LOSS_RATE)` | |
| CE mark [%] | `getStatisticsItem(CE_RATE)` | 0 until ECN reception lands |
| Pacing rate [bps] | **approx** `cwnd·8 / sRtt` | ⚠ inflates at sub-ms loopback RTT |
| Pkts / bytes / FB count | `TestSession` counters | |

Receiver metrics (`RX_METRICS_SIZE = 5`): RTP received, feedback sent, CE
marked, received rate, session time.

---

## 7. Build & run

### Prereqs (Android Studio)
- Android SDK Platform **34**
- **NDK** (side-by-side) + **CMake 3.22.1** (SDK Tools)
- ABIs built: `arm64-v8a` (device), `x86_64` (emulator)

### Build
Open `/Users/moose/ScreamPhoneApp` in Android Studio → Gradle Sync (creates
`local.properties` + `gradle-wrapper.jar`) → Run ▶.

### On-device loopback test (no Linux box, no NAT)
1. **Receiver** card → Listen port `30000` → **Start RX**.
2. **Sender** card → Server IP `127.0.0.1`, Port `30000`, Local port `0` →
   **Start**.
3. Expect: receiver RTP/FB counters climb; sender **FB > 0**, **CWND ramps**,
   RTT/queue-delay/loss live. (Start RX before the sender.)

### Android ↔ Linux test
Build upstream receiver: `cd scream && cmake . && make`, run `scream_receiver`,
set the app's Server IP to the host and pin **Local port** so the Linux side can
return RTCP to the device.

---

## 8. Key decisions (locked)

- **ECN:** ECN-ready, not ECN-dependent. TX ECT codepoint set via `IP_TOS`
  (config); CE reception deferred behind a future `EcnManager`. `isL4s` is wired
  to `ect == 1`.
- **JNI language:** C++ (bind the core directly).
- **Metrics:** derived from existing public core API; `scream_core` untouched
  (overrode the earlier "add getters" idea). Pacing rate therefore approximated.
- **Charts:** custom Compose Canvas, no charting dependency.
- **Module layout:** `interfaces/` + `controllers/` + `wrapper/` (+ `scream_core/`).

---

## 9. Known limitations / gotchas

- **Pacing rate** reads absurdly high on loopback (sub-ms RTT) — approximation
  artifact; sensible on real networks.
- **CE/loss = 0 on loopback** (lossless path; ECN CE reception not implemented).
- **Invalid Server IP** now fails loudly (`invalid server IP: …`) instead of
  silently sending to `0.0.0.0`. Use `127.0.0.1`, not `1027.0.0.1`.
- **Receiver** currently runs on native threads without the foreground service —
  fine while foregrounded; long unattended receiver runs need the service.
- **TX < Target during ramp** is expected (`avgRateTx` is a slow filter).

---

## 10. Roadmap

**Phase 1 (MVP) — DONE**
- [x] Sender, RTP/UDP test traffic, RTCP feedback, live dashboard.

**Phase 2 — DONE (core)**
- [x] Receiver mode, on-device loopback.
- [ ] Sender/Receiver/Both **role selector** in UI.
- [ ] Receiver under foreground service.

**Phase 3 — L4S evaluation (next priority)**
- [ ] `EcnManager`: CE-mark reception via `recvmsg`/cmsg (`IP_RECVTOS`).
- [ ] Live CE %, mark-fraction, L4S vs classic comparison.
- [ ] Switch codepoint to ECT(1) + enable `isL4s`.

**Later**
- [ ] Exact pacing-rate accessor.
- [ ] Android↔Android + Android↔Linux real-network test runs.
- [ ] Session logging / CSV export for offline analysis.
- [ ] Config presets, mDNS/QR pairing.

---

## 11. JNI surface (reference)

`com.spa.scream.ScreamBridge` native methods (handles are opaque `long`s):

- Sender: `nativeCreate`, `nativeStart(ip, serverPort, localPort, min, start,
  max, fps, mtu, ect, delayTarget, ssrc) → String? err`, `nativeStop`,
  `nativeIsRunning`, `nativeGetMetrics → double[16]`, `nativeDestroy`.
- Receiver: `nativeCreateReceiver`, `nativeStartReceiver(port, ssrc) → String?
  err`, `nativeStopReceiver`, `nativeIsReceiverRunning`,
  `nativeGetReceiverMetrics → double[5]`, `nativeDestroyReceiver`.

Integration contract: `RtpQueue::clear()` calls a global
`packet_free(void*, uint32_t)` — defined in `ScreamController.cpp` to free the
heap copies made in `enqueueRtp` (prevents leaks on RTP-queue discard).
