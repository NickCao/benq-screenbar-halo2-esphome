#!/usr/bin/env python3
"""Transmit bounded Halo 2 packets with the ESP32 Wi-Fi RF front end."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import secrets
import time

from capture import command, open_port
from frame import check_crc, make_frame


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--frequency", type=int, default=2405)
    parser.add_argument("--sync", default="CE8C234F", help="Address in on-air order")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--raw", help="Following bits packed MSB first, hex; no CRC is added")
    source.add_argument("--payload", help="Ten payload bytes in hex; adds the 9-bit PCF and address-dependent CRC")
    parser.add_argument("--bits", type=int, default=105)
    parser.add_argument("--pid", type=int, default=0)
    parser.add_argument("--gain", type=int, default=96, help="Uncalibrated tone gain code: 64 strongest, 127 weakest")
    parser.add_argument("--inverted", action="store_true")
    parser.add_argument("--repeat", type=int, default=1, help="Number of packets, 1..16")
    parser.add_argument("--check", action="store_true", help="Temporarily enable the onboard LR1121 as a receive-only reference")
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        sync = int(args.sync, 16)
        if len(args.sync) != 8 or not 0 < sync < 0xFFFFFFFF:
            raise ValueError("--sync must be eight hex digits with both zero and one bits")
        data = bytes.fromhex(args.raw or args.payload)
        if args.payload is not None:
            if args.bits != 105:
                raise ValueError("--payload requires --bits 105")
            data = make_frame(sync, data, args.pid)
        if not (1 <= args.bits <= 256 and len(data) == (args.bits + 7) // 8):
            raise ValueError("Raw byte length must match --bits (1..256)")
        if args.bits % 8 and data[-1] & ((1 << (8 - args.bits % 8)) - 1):
            raise ValueError("Unused low bits must be zero")
        if not (2400 <= args.frequency <= 2483 and 64 <= args.gain <= 127 and
                1 <= args.repeat <= 16 and 0 <= args.pid <= 3):
            raise ValueError("Invalid frequency, gain, repeat count or packet ID")
    except ValueError as error:
        parser.error(str(error))
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.output or Path(__file__).resolve().parent / "captures" / f"{stamp}.tx.jsonl"
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x") as journal, open_port(args.port, args.no_reset) as port:
        def save(kind, values):
            journal.write(json.dumps({"kind": kind, "utc": datetime.now(timezone.utc).isoformat(), **values}) + "\n")
            journal.flush()

        nonce = secrets.randbits(32)
        command(port, f"SYNC {nonce}", f"SYNC {nonce}")
        info = json.loads(command(port, "HALOINFO?", "HALOINFO ")[9:])
        if info.get("tx_backend") != "esp32s3-pll" or info.get("lr1121_reset") is not True:
            raise RuntimeError(f"Expected TX-capable experiment firmware: {info}")
        command(port, f"FREQ {args.frequency}", "OK")
        command(port, f"HALOSYNC {sync:08X}", "OK")
        command(port, f"HALOTXGAIN {args.gain}", "OK")
        command(port, f"HALOTXPOLARITY {int(args.inverted)}", "OK")
        info = json.loads(command(port, "HALOINFO?", "HALOINFO ")[9:])
        save("info", {"port": args.port, "firmware": info})
        save("tx_request", {"frequency_mhz": args.frequency, "sync": f"{sync:08X}",
                            "bits": args.bits, "raw": data.hex().upper(), "repeat": args.repeat,
                            "gain_code": args.gain, "inverted": args.inverted,
                            "reference_enabled": args.check, "offline_crc": check_crc(sync, data, args.bits)})
        op = "HALOTXCHECK" if args.check else "HALOTX"
        for index in range(args.repeat):
            result = json.loads(command(port, f"{op} {args.bits} {data.hex().upper()}", "HALOTXEND ")[10:])
            if result.get("reference_raw"):
                result["offline_crc"] = check_crc(sync, bytes.fromhex(result["reference_raw"]), args.bits)
            save("tx", {"index": index, **result})
            print(json.dumps(result), flush=True)
            if not result["complete"] or not result["rx_restored"] or not result["lr1121_reset"]:
                raise RuntimeError("Transmission did not finish cleanly; see saved report")
            if args.check and not result["reference_match"]:
                raise RuntimeError("Independent receiver did not recover the transmitted bits; see saved report")
            if index + 1 < args.repeat:
                time.sleep(.1)
        print(f"Saved {output}", flush=True)


if __name__ == "__main__":
    main()
