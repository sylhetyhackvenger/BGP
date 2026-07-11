# BGP
BGP is a Gateway Protocol client supporting 4‑byte ASN, multiprotocol extensions, route refresh, ADD‑PATH, and exponential backoff reconnection. Delivers JSON logs, thread‑safe output, and automatic dependency installation via Python launcher. Ideal for peering, testing, and monitoring BGP sessions in production or lab environments.

# BGP – Border Gateway Protocol Peer CLI Tool

<p align="center">
  <img src="assets/img.png" alt="BGP Banner" width="600"/>
</p>

<p align="center">
  <strong>Lightweight, high‑performance BGP speaker for peering, testing, and monitoring</strong>
</p>

---

## 📖 Description

**BGP** is a full‑featured, single‑binary Border Gateway Protocol (BGP) client designed for network engineers, developers, and security researchers. It implements the core BGP‑4 specification (RFC 4271) with modern extensions including 4‑byte AS numbers (RFC 6793), multiprotocol extensions (RFC 4760), route refresh (RFC 2918), and ADD‑PATH (RFC 7911). The tool is built in C for maximum efficiency and runs on Linux, macOS, and Android (Termux) with minimal dependencies.

With its thread‑per‑peer architecture and non‑blocking I/O, BGP can handle multiple concurrent peering sessions simultaneously. It outputs structured JSON logs, making it easy to integrate with monitoring pipelines, SIEM systems, or custom analytics. The optional exponential backoff reconnection ensures resilient connectivity in unstable network environments.

Whether you are **testing BGP implementations**, **debugging routing policies**, **monitoring BGP sessions**, or **building network automation tools**, BGP provides a reliable, scriptable, and low‑overhead solution. The included Python launcher automatically handles dependency installation (gcc, libjansson, pkg‑config) on supported platforms, reducing setup time to seconds.

---

## 🚀 Features Dashboard

| Capability | Description |
|------------|-------------|
| **Full FSM** | Implements all BGP finite state machine states: Idle, Connect, Active, OpenSent, OpenConfirm, Established |
| **4‑Byte ASN** | Full support for 32‑bit AS numbers (RFC 6793) with AS_TRANS negotiation |
| **Multiprotocol** | IPv4, IPv6, VPNv4, and EVPN address families (AFI/SAFI) |
| **Route Refresh** | Graceful route refresh capability (RFC 2918) |
| **ADD‑PATH** | Support for advertising multiple paths per prefix (RFC 7911) |
| **Hold Timer** | Configurable hold time (default 600s) with Keepalive timer |
| **Reconnection** | Exponential backoff (5s to 120s) with configurable max retries |
| **JSON Output** | Machine‑readable logs in pretty‑print or JSONL format |
| **Thread‑Safe** | Dedicated writer thread prevents stdout blocking |
| **Cross‑Platform** | Runs on Linux, macOS, and Termux (Android) |
| **Zero‑Config** | Python launcher installs dependencies automatically |

---

## 📦 Installation

### Prerequisites
- **C compiler** (gcc or clang)
- **libjansson** (JSON library)
- **pkg‑config** (for compiler flags)
- **make** (optional, for build)

### Automatic Installation (Recommended)
```bash
# Clone or download the two files: bgp.c and bgp.py
python bgp.py
```

The Python launcher will:

· Detect your operating system (Linux, Termux)
· Install missing dependencies via apt, yum, dnf, or pkg
· Compile the C program to ./bgp
· Offer interactive or command‑line modes

Manual Build

```bash
gcc -o bgp bgp.c -ljansson -lpthread -lm -Wall -O2
```

---

🛠️ Usage

Command Line Options

```
Usage: bgp [options...] <peer> [<peer> ...]
  -s, --source <ip>        IP to source BGP connection from
  -a, --asn <asn>          Local ASN (supports 4‑byte ASNs). Default: 65000
  -r, --rid <ip>           Local router ID. Default: 1.1.1.1
  -l, --logging <level>    Logging level (0‑4). Default: 3 (Info)
  -f, --format <fmt>       Output format: 'json' or 'jsonl'. Default: json
  -R, --reconnect          Enable automatic reconnection with backoff
  -m, --max-retries <n>    Max reconnection attempts (0 = infinite)
  -t, --hold-time <sec>    Hold time in seconds. Default: 600
  -h, --help               Show this help

<peer> formats: <ip>,<asn> or <ip>,<asn>,<name>
```

Automated Mode (Quick Test)

```bash
python bgp.py auto
```

This connects to 192.168.1.1 AS 65001 with a peer named TestPeer.

Manual Mode (Custom Arguments)

```bash
python bgp.py manual
```

Enter your own options when prompted, e.g.:

```
-a 65000 -R -t 180 10.0.0.1,65001,Core-Router
```

Direct Binary Execution

```bash
./bgp -a 65000 -s 192.168.1.100 -R -l 4 172.16.0.1,65002,Peer1
```

---

📊 Example Output (JSON)

```json
{
  "time": 1700000000,
  "peer_name": "Core-Router",
  "id": 42,
  "type": "OPEN",
  "length": 29,
  "message": {
    "version": 4,
    "asn": 65001,
    "hold_time": 600,
    "router_id": "10.0.0.1",
    "capabilities": [
      { "code": 2, "name": "Route Refresh" },
      { "code": 65, "name": "4‑octet ASN", "asn": 65000 }
    ]
  }
}
```

---

🧩 Dashboard – Tool Speciality & Capabilities

Category Details
Performance Written in C, non‑blocking I/O, per‑peer threads, handles hundreds of sessions
Extensibility Modular design, easy to add new capabilities or address families
Security Supports BGP Authentication (MD5) and graceful shutdown (CEASE)
Observability JSON logs, session uptime, message counters, error classification
Automation Scriptable via Python wrapper, can be integrated into CI/CD pipelines
Debugging Verbose logging levels, trace FSM transitions, packet dumps

---

🧪 Examples for Manual Usage

1. Establish a session with reconnection

```bash
./bgp -a 65000 -R -m 5 -t 300 192.168.1.1,65001,Router-A
```

2. Use a source IP and JSONL format

```bash
./bgp -s 10.0.0.2 -f jsonl 10.0.0.1,65002
```

3. Multiple peers with custom names

```bash
./bgp 172.16.0.1,65003,Core 172.16.0.2,65004,Edge
```

4. Debug mode (level 4) with no reconnection

```bash
./bgp -l 4 -t 180 192.168.2.1,65005
```

---

👤 Author

SYLHETYHACKVENGER (THE‑ERROR808)
Special thanks to the open‑source community for the BGP standards and libraries that made this tool possible.

---

📜 License

This project is licensed under the MIT License – see the LICENSE file for details.

---

🙌 Contributing

Contributions, issues, and feature requests are welcome!
Please submit a pull request or open an issue on the repository.

---

Built with ❤️ for network engineers everywhere.

```
