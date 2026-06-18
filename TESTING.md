# Testing SPA in a Real-World Scenario

How to take SPA out of on-device loopback and test SCReAM over a real network.
Pick a topology, induce congestion, observe the live metrics, then export the CSV
and analyze.

> For loopback (single device, no network) see the quick steps in
> [`README.md`](README.md) / [`PROJECT.md`](PROJECT.md). This guide is about
> **real network paths**.

---

## 1. Prerequisites (real device, not emulator)

- Run on a **physical phone** (`arm64-v8a`). The emulator's `10.0.2.2` host alias
  only works in the emulator; on a real device use real LAN IPs.
- Keep the sender app **foregrounded / screen on**. The foreground service +
  wakelock keeps the pacing loop alive, but avoid Doze during long unattended runs.
- Find each node's IP:
  - **Phone:** Settings → Wi-Fi → tap the network → IP address (e.g. `192.168.1.50`).
  - **Linux:** `ip addr` (the `wlan0`/`eth0` `inet` line).
- Every node must be on a path that passes **UDP**. Same Wi-Fi LAN is easiest;
  firewalls/NAT add complications (see [Troubleshooting](#8-troubleshooting)).

---

## 2. Topology A — Android → Android over Wi-Fi (easiest, all in-app)

Two phones on the same Wi-Fi. Phone **R** = receiver, Phone **S** = sender.

1. **Phone R:** Receiver card → Listen port `30000` → **Start RX**. Note R's IP
   (e.g. `192.168.1.50`).
2. **Phone S:** Sender card → Server IP = `192.168.1.50`, Port `30000`,
   Local port `0`, Start kbps `2000`, Max kbps `50000` → **Start**.
3. Expect on **S**: `FB > 0`, CWND ramps, RTT ≈ real Wi-Fi RTT, TX bitrate climbs
   toward Max. On **R**: RTP / FB counters climb.

The receiver replies RTCP to the **source address** of the incoming RTP, so you do
**not** need to pin the sender's local port for this to work.

---

## 3. Topology B — Android → Linux (upstream receiver / reference)

Use Ericsson's upstream tool as the receiver. On the Linux box:

```bash
cd scream/code
cmake . && make            # builds scream_bw_test_tx and scream_bw_test_rx
# learn-from-source mode: give ONLY the port; it learns the sender
# address from the first incoming RTP packet and replies to source.
./scream_bw_test_rx 30000
```

Open the UDP port on the Linux firewall:

```bash
sudo ufw allow 30000/udp
```

On the **phone sender:** Server IP = Linux box IP, Port `30000`, Local port `0` →
**Start**.

Learn-from-source means the receiver sends RFC 8888 feedback back to the exact
source IP:port of your RTP — the same socket the phone listens on — so ephemeral
ports / NAT mappings are handled automatically. (Seeding it instead with
`./scream_bw_test_rx <sender_ip> 30000` requires a predictable sender address.)

---

## 4. Induce real congestion so SCReAM reacts

On a clean LAN the rate just ramps to Max and flatlines. To see the interesting
dynamics (CWND sawtooth, queue-delay tracking, rate back-off), create a bottleneck.

**Natural:** walk the phone away from the AP, switch to **cellular**, or run
competing traffic (a large download) on the same link.

**Controlled (Linux receiver, `tc` / netem).** Shape the **forward** RTP path via
ingress redirect (`ifb`). Replace `eth0` with the receiver's interface:

```bash
sudo modprobe ifb && sudo ip link set ifb0 up
sudo tc qdisc add dev eth0 handle ffff: ingress
sudo tc filter add dev eth0 parent ffff: protocol ip u32 match u32 0 0 \
  action mirred egress redirect dev ifb0
# 5 Mbit bottleneck + 20 ms delay + 0.5% loss:
sudo tc qdisc add dev ifb0 root netem rate 5mbit delay 20ms loss 0.5%
```

Vary it live and watch the app respond:

```bash
sudo tc qdisc change dev ifb0 root netem rate 2mbit delay 40ms    # tighten
sudo tc qdisc change dev ifb0 root netem rate 10mbit delay 20ms   # loosen
```

Cleanup:

```bash
sudo tc qdisc del dev ifb0 root
sudo tc qdisc del dev eth0 ingress
sudo ip link set ifb0 down
```

Simpler but only shapes the RTCP/return path (useful for RTT-on-feedback
experiments, less so for forward-path congestion):

```bash
sudo tc qdisc add dev eth0 root netem delay 50ms loss 1%
sudo tc qdisc del dev eth0 root      # cleanup
```

---

## 5. With vs without L4S comparison

This compares classic SCReAM (delay/loss-driven) against L4S (ECN-CE-driven) over
the **same** bottleneck. No app changes are required, but note the topology rule:

> **You must use the upstream Linux receiver** (`scream_bw_test_rx`). The app's own
> receiver does not read ECN CE marks yet (`ceBits` is hardcoded to 0), so an
> Android↔Android run cannot close the L4S loop — "with L4S" would look identical to
> classic. The **sender** in this app is already L4S-capable (sets ECT(1), runs
> `isL4s`, and acts on CE returned in RFC 8888 feedback).

### Get a receiver that produces CE marks

Pick **one** of:

- **Software marking (easiest, no AQM).** Build the upstream receiver with the
  built-in L4S test marker, which probabilistically turns ECT(1) into CE
  (`PMARK = 0.1` → ~10%, compile-time):

```bash
cd scream/code
cmake -DCMAKE_CXX_FLAGS="-DTEST_L4S" . && make
./scream_bw_test_rx 30000
```

- **Real ECN AQM.** Build normally (`cmake . && make`) and put an ECN-marking AQM on
  the bottleneck so it CE-marks ECT(1) traffic — e.g. `cake` / `fq_codel` with ECN,
  or DualPI2:

```bash
# example: ECN-marking fq_codel as the bottleneck qdisc
sudo tc qdisc replace dev ifb0 root fq_codel ecn
```

### First, confirm ECT(1) actually leaves the phone

Some Android / Wi-Fi / cellular paths bleach the ECN bits (and the app's `IP_TOS`
set is non-fatal). On the receiver host:

```bash
sudo tcpdump -v -n udp port 30000   # look for "ECT(1)" (and "CE" once marking is on)
```

If you only ever see `ECT(0)`/`Not-ECT`, the path is clearing ECN and L4S can't be
tested on that link — try a different network.

### Run both arms (same bottleneck both times)

1. **Without L4S (baseline):** Sender card → `ECT(-1)` → **Start**. Run ~60 s,
   **Stop**, **Export CSV** as `classic.csv`.
2. **With L4S:** Sender card → `ECT(1)` → **Start** (against the CE-producing
   receiver above). Run ~60 s, **Stop**, **Export CSV** as `l4s.csv`.

Keep `Start kbps` / `Max kbps` and the bottleneck (`rate`/`delay`) identical across
the two runs.

### What to expect

| Metric | Classic (`ECT(-1)`) | L4S (`ECT(1)` + CE) |
|---|---|---|
| `queue_delay_ms` | Higher, rises to the delay target before backing off | **Lower and more stable** (reacts to marks before a queue forms) |
| `loss_pct` | Nonzero under load | Near zero (marks replace drops) |
| `tx_kbps` | Comparable throughput | Comparable throughput, smoother |
| `cwnd_bytes` | Larger sawtooth | Smaller, tighter oscillation |

Plot `queue_delay_ms` from both CSVs on the same axis — the L4S run holding lower
latency at similar throughput is the headline result.

> Visualization caveat: the on-device dashboard and CSV do **not** surface CE % yet
> (the sender collects it internally as `ceMarkPercent`). You infer the L4S effect
> from the queue-delay/loss difference above. Surfacing CE %, plus app-receiver CE
> reading for Android↔Android L4S, is the Phase 3 work in `PROJECT.md`.

---

## 6. What to validate

| Signal | Expected behavior |
|---|---|
| **CWND** | Ramps up, then sawtooths down on loss / queue buildup. |
| **RTT** | Tracks the netem `delay` you set (plus base RTT). |
| **Queue delay** | Rises as you tighten `rate`; SCReAM pulls target bitrate down to keep it near its delay target (~60 ms). |
| **TX / target bitrate** | Converge toward the bottleneck `rate`. TX lags during ramps (slow filter) — expected. |
| **Loss %** | Nonzero under netem `loss` or when overdriving the bottleneck. |

---

## 7. Capture and analyze

1. Run for a minute or two, varying impairments mid-run.
2. **Stop** the sender, then tap **Export CSV (N samples)** → choose a location.
3. Pull and plot:

```bash
adb pull /sdcard/Download/scream_<timestamp>.csv
```

CSV columns:

```
timestamp_ms, rtt_ms, cwnd_bytes, target_kbps, tx_kbps, pacing_kbps,
queue_delay_ms, loss_pct, feedback_count, packets_sent
```

Plot `tx_kbps` and `queue_delay_ms` vs `timestamp_ms` against your netem changes to
see the control loop track the bottleneck.

---

## 8. Troubleshooting

- **FB stays 0 / CWND won't grow:** receiver unreachable. Check — receiver started
  **before** sender, correct IP, UDP port open in firewall, same subnet (or proper
  port-forwarding across NAT).
- **`invalid server IP` error:** typo in the address (it fails loudly instead of
  silently sending to `0.0.0.0`).
- **Works on Wi-Fi, not across networks:** carrier-grade NAT / firewalls block
  inbound UDP. Use a VPN / same LAN, or a publicly reachable receiver with
  port-forwarding.
- **Long runs die in background:** keep the app foregrounded; disable battery
  optimization for it. The receiver role runs on native threads without its own
  foreground service, so keep that device awake too.
- **Pacing rate looks absurd at very low RTT:** approximation artifact at sub-ms
  RTT; sensible on real networks.
- **ECN/L4S:** the `ect` field sets the TX codepoint, and the **sender** acts on CE
  returned in feedback. The **app receiver** does not read CE yet, so Android↔Android
  L4S won't close the loop — use the upstream Linux receiver for L4S (see §5). For
  classic runs, drive congestion with netem `loss` or a real bottleneck.
- **L4S "with" run looks like classic:** ECT(1) is being bleached on the path, or the
  receiver isn't marking. Verify ECT(1)/CE with `tcpdump` (§5) and confirm the
  receiver was built with `-DTEST_L4S` or sits behind an ECN-marking AQM.
