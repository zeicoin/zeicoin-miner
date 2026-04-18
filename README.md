# ZenRX

**ZenRX** is a high-performance, CPU-only miner for RandomX-based cryptocurrencies (zei available)

ZenRX can also be used as a fully standalone miner on Linux and Windows.

Dev fee - 0%

---

# Platform Integration

ZenRX providing:

- Native monitoring via HTTP API
- Simplified deployment model
- Automated optimization at startup
- MoneroOcean profitability-based auto-switching

# Changes

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

- ZeiCoin cryptocurrency
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
| `rx/zei` | ZeiCoin (zei) | RandomX default (ZeiCoin mainnet) |
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

ZenRX includes a **0% developer fee**:

---

# License

ZenRX is licensed under the **GNU General Public License v3.0**.
