# ZenRX

**ZenRX** is a high-performance, CPU-only miner for RandomX-based cryptocurrencies.

It is the official mining engine of the **ZenOS platform** (https://zenplatform.dev), designed for tight OS-level integration, automated deployment, and simplified large-scale management.

ZenRX can also be used as a fully standalone miner on Linux and Windows.

Performance is comparable to XMRig for supported algorithms.  
It is **not inherently faster or slower than XMRig** — the focus is architectural simplification and platform integration rather than raw performance changes.

---

# Platform Integration

ZenRX is built specifically for the **ZenOS mining platform**, providing:

- Seamless integration with ZenOS services
- Native monitoring via HTTP API
- Simplified deployment model
- Automated optimization at startup
- MoneroOcean profitability-based auto-switching

Although optimized for ZenOS, ZenRX runs perfectly as a standalone CPU miner.

---

# Acknowledgments & Attribution

ZenRX is derived from **XMRig**, https://github.com/xmrig

XMRig Donations XMR: 48edfHu7V9Z84YzzMa6fUueoELZ9ZRXq9VetWzYGzKt52XU5xvqgzYnDK9URnRoJMk1j8nLwEVsaSWJ4fhdUyZijBGUicoD

The embedded RandomX implementation and MSR tweaks originate from the XMRig codebase.

This project is released under the **GNU General Public License v3.0**, consistent with the original XMRig license.

---

# Changes Compared to XMRig

## Removed

- GPU backends (OpenCL, CUDA)
- Non-RandomX algorithms (CryptoNight, KawPow, GhostRider, etc.)
- Proxy and SOCKS5 support
- Hardware monitoring layers (NVML, ADL, DMI)
- Complex multi-backend architecture

## Simplified

- Stratum client (single raw-socket implementation with TLS)
- Flat configuration structure
- Minimal CLI
- HTTP API (single `/api` endpoint)

## Added

- MoneroOcean algorithm profitability benchmarking (`algo-perf`)
- Dual RandomX instances (user + dev) for zero-downtime dev fee switching
- Extended RandomX variants:
  - `rx/xeq`
  - `rx/xla` (Panthera)
- Background RandomX dataset reinitialization on seed hash changes

---

# Supported Algorithms

| Algorithm | Coin | Description |
|-----------|------|-------------|
| `rx/0` | Monero (XMR) | RandomX default (Monero mainnet) |
| `rx/wow` | Wownero (WOW) | RandomX variant |
| `rx/arq` | ArQmA (ARQ) | RandomX variant |
| `rx/xeq` | Equilibria (XEQ) | RandomX variant |
| `rx/graft` | Graft (GRFT) | RandomX variant |
| `rx/sfx` | Safex (SFX) | RandomX variant |
| `rx/xla` | Scala (XLA) | RandomX Panthera variant |

---

# Core Features

- **CPU-only architecture**
- **RandomX optimized**
- **MoneroOcean auto-switching**
- **Automatic L3 cache-based thread detection**
- **Automatic CPU affinity configuration**
- **Automatic MSR tweaks (Intel / AMD)**
- **Automatic huge page configuration**
- **Auto Detected TLS stratum**
- **Built-in HTTP JSON API**
- **Background dataset reinitialization**
- **Zero-downtime dev fee switching**

---

# Quick Start

## Mine on MoneroOcean (Auto-switching)

```
./zenrx -o gulf.moneroocean.stream:20128 -u YOUR_WALLET -p WORKER_NAME
```

## Mine on a specific pool

```
./zenrx -o xmr.kryptex.network:8029 -u YOUR_WALLET/WORKER_NAME
```

## Use a configuration file

```
./zenrx -c zenrx.json
```

---

# Build from Source

## Linux

```
sudo apt install build-essential cmake
./build_zenrx_linux.sh
```

## Windows (cross-compile from Linux)

```
sudo apt install build-essential cmake mingw-w64
./build_zenrx_win.sh
```

---

# Configuration

Configuration file: `zenrx.json`

```json
{
    "http": {
        "enabled": false,
        "host": "127.0.0.1",
        "port": 16000
    },
    "pools": [
        {
            "url": "gulf.moneroocean.stream:20128",
            "user": "YOUR_WALLET_ADDRESS",
            "pass": "WORKER_NAME"
        }
    ],
    "algo": null,
    "colors": true,
    "autosave": true,
    "debug": false,
    "log-file": null,
    "print-time": 60,
    "bench-algo-time": 20
}
```

---

# Dev Fee

ZenRX includes a **1% developer fee**:

- 1 minute of mining to the developer wallet
- Every 99 minutes of user mining
- Zero-downtime switching via dual RandomX instances

---

# License

ZenRX is licensed under the **GNU General Public License v3.0**.

See the original XMRig license text here:  
https://github.com/xmrig/xmrig/blob/master/LICENSE
