#!/usr/bin/env python3
"""
discover_wiz.py - Find every WiZ device on the local network and print its
capabilities (via the WiZ local UDP/JSON API on port 38899).

No third-party packages required - standard library only.

Usage:
    python3 discover_wiz.py                # scan your /24 subnet
    python3 discover_wiz.py --subnet 192.168.0.0/24
    python3 discover_wiz.py --timeout 4    # listen longer for replies

What it does (mirrors the ESP32 firmware):
    1. Sends a getPilot to a broadcast address AND to every host on the subnet.
    2. Collects the IPs that reply (only real WiZ devices answer on 38899).
    3. Asks each responder for getSystemConfig (capabilities) + getPilot (state).
    4. Prints a report.
"""

import argparse
import ipaddress
import json
import socket
import time

WIZ_PORT = 38899


def get_local_ip() -> str:
    """Best-effort local IP of the interface used for outbound traffic."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def discover(subnet: ipaddress.IPv4Network, local_ip: str, timeout: float):
    """Broadcast + unicast sweep; return {ip: raw_reply_bytes}."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))            # ephemeral local port; replies come back here
    sock.settimeout(0.2)

    get_pilot = json.dumps({"method": "getPilot", "params": {}}).encode()
    registration = json.dumps({
        "method": "registration",
        "params": {"phoneMac": "AAAAAAAAAAAA", "register": False,
                   "phoneIp": "1.2.3.4", "id": "1"},
    }).encode()

    hosts = [str(h) for h in subnet.hosts() if str(h) != local_ip]
    print(f"Local IP : {local_ip}")
    print(f"Subnet   : {subnet}  ({len(hosts)} host addresses)")
    print(f"Probing  : broadcast + unicast getPilot to every host ...")

    # 1) broadcast (works on networks that allow it)
    for addr in ("255.255.255.255", str(subnet.broadcast_address)):
        try:
            sock.sendto(registration, (addr, WIZ_PORT))
            sock.sendto(get_pilot, (addr, WIZ_PORT))
        except OSError:
            pass

    # 2) unicast sweep (works even when broadcast is filtered)
    for host in hosts:
        try:
            sock.sendto(get_pilot, (host, WIZ_PORT))
        except OSError:
            pass

    # 3) collect replies
    found = {}
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        ip = addr[0]
        if ip == local_ip:
            continue
        if ip not in found:
            found[ip] = data
            print(f"  reply from {ip}")
    sock.close()
    return found


def query(ip: str, method: str, timeout: float = 1.5):
    """Send one method to a device and return the parsed JSON (or None)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(json.dumps({"method": method, "params": {}}).encode(),
                    (ip, WIZ_PORT))
        data, _ = sock.recvfrom(2048)
        return json.loads(data.decode())
    except (socket.timeout, OSError, json.JSONDecodeError):
        return None
    finally:
        sock.close()


def classify(module_name: str):
    """Map a WiZ moduleName to a device type + capability list + HomeKit type."""
    m = (module_name or "").upper()
    if "SOCKET" in m:
        return "SOCKET", ["on/off"], "Outlet"
    caps = ["on/off", "brightness"]
    if "RGB" in m:
        caps += ["color (RGB)", "tunable white (CCT)"]
        return "RGB", caps, "Lightbulb (color)"
    if "TW" in m:
        caps += ["tunable white (CCT)"]
        return "TW", caps, "Lightbulb (white)"
    if "DW" in m:
        return "DW", caps, "Lightbulb (dimmable)"
    return "UNKNOWN", caps, "Lightbulb (dimmable)"


def report(ip: str):
    cfg = query(ip, "getSystemConfig")
    pilot = query(ip, "getPilot")

    print("-" * 64)
    print(f"Device @ {ip}")
    if not cfg or "result" not in cfg:
        print("  ! getSystemConfig: no/invalid reply")
        return

    r = cfg["result"]
    module = r.get("moduleName", "")
    mac = r.get("mac", "")
    fw = r.get("fwVersion", "")
    dtype, caps, hk = classify(module)

    print(f"  MAC         : {mac}")
    print(f"  Module      : {module}")
    print(f"  Firmware    : {fw}")
    print(f"  Type        : {dtype}")
    print(f"  Capabilities: {', '.join(caps)}")
    print(f"  HomeKit as  : {hk}")

    if pilot and "result" in pilot:
        p = pilot["result"]
        bits = [f"state={p.get('state')}"]
        if "dimming" in p:
            bits.append(f"dimming={p.get('dimming')}%")
        if "temp" in p:
            bits.append(f"temp={p.get('temp')}K")
        if "r" in p:
            bits.append(f"rgb=({p.get('r')},{p.get('g')},{p.get('b')})")
        if "sceneId" in p:
            bits.append(f"sceneId={p.get('sceneId')}")
        print(f"  Now         : {', '.join(bits)}")

    print(f"  Raw getSystemConfig: {json.dumps(r)}")


def main():
    ap = argparse.ArgumentParser(description="Discover WiZ devices and print capabilities.")
    ap.add_argument("--subnet", help="CIDR to scan, e.g. 192.168.0.0/24 (default: your /24)")
    ap.add_argument("--timeout", type=float, default=3.0, help="seconds to listen for replies")
    args = ap.parse_args()

    local_ip = get_local_ip()
    if args.subnet:
        subnet = ipaddress.ip_network(args.subnet, strict=False)
    else:
        subnet = ipaddress.ip_network(local_ip + "/24", strict=False)

    found = discover(subnet, local_ip, args.timeout)

    print()
    print(f"Discovered {len(found)} WiZ device(s).")
    for ip in sorted(found, key=lambda x: ipaddress.ip_address(x)):
        report(ip)
    print("-" * 64)
    if not found:
        print("No devices replied. If the WiZ app works, the most likely cause")
        print("is that your computer is on a different subnet/VLAN than the bulbs,")
        print("or the router blocks client-to-client traffic. Try:")
        print("  python3 discover_wiz.py --subnet <bulb_subnet>/24 --timeout 5")


if __name__ == "__main__":
    main()
