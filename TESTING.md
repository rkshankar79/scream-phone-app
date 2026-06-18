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
  firewalls/NAT add complications (see [Troubleshooting](#7-troubleshooting)).

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

## 5. What to validate

| Signal | Expected behavior |
|---|---|
| **CWND** | Ramps up, then sawtooths down on loss / queue buildup. |
| **RTT** | Tracks the netem `delay` you set (plus base RTT). |
| **Queue delay** | Rises as you tighten `rate`; SCReAM pulls target bitrate down to keep it near its delay target (~60 ms). |
| **TX / target bitrate** | Converge toward the bottleneck `rate`. TX lags during ramps (slow filter) — expected. |
| **Loss %** | Nonzero under netem `loss` or when overdriving the bottleneck. |

---

## 6. Capture and analyze

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

## 7. Troubleshooting

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
- **ECN/L4S:** the `ect` field sets the TX codepoint, but CE-mark **reception** is
  not implemented yet, so CE will read 0. Drive congestion with netem `loss` or a
  real bottleneck to see loss/queue-delay react.
