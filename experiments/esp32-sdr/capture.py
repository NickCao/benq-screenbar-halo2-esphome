#!/usr/bin/env python3
"""Run bounded receive windows and save raw packet candidates over USB."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import secrets
import time

import serial


def read_line(port, deadline):
    line = bytearray()
    while time.monotonic() < deadline:
        char = port.read(1)
        if not char:
            continue
        line += char
        if len(line) > 4096:
            raise RuntimeError("Oversized serial reply; reset the board and reconnect")
        if char == b"\n":
            return line.decode("ascii", errors="replace").strip()
    raise TimeoutError(f"Serial reply timed out (partial reply: {line[:120]!r})")


def command(port, text, expected):
    port.write((text + "\n").encode("ascii"))
    deadline = time.monotonic() + 10
    while True:
        line = read_line(port, deadline)
        if line.startswith(expected):
            return line
        if line.startswith("ERR"):
            raise RuntimeError(f"{text}: {line}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--frequency", type=int, default=2405)
    parser.add_argument("--sync", default="CE8C234F", help="32-bit address in on-air order")
    parser.add_argument("--bits", type=int, default=105)
    parser.add_argument("--min-power", type=float, default=64)
    parser.add_argument("--windows", type=int, default=16, help="Bounded windows per serial command")
    parser.add_argument("--no-reset", action="store_true", help="Skip the native USB startup reset")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        sync = int(args.sync, 16)
    except ValueError:
        parser.error("--sync must be an eight-digit hexadecimal address")
    if not (len(args.sync) == 8 and 0 < sync < 0xFFFFFFFF):
        parser.error("--sync must contain eight hex digits and both zero and one bits")
    if not (0 < args.seconds <= 3600 and 1 <= args.windows <= 1000 and 1 <= args.bits <= 256
            and 2400 <= args.frequency <= 2483 and 1 <= args.min_power <= 65535):
        parser.error("Invalid duration, window count, bit count, frequency or power threshold")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or Path(__file__).resolve().parent / "captures" / f"{stamp}.jsonl"
    output.parent.mkdir(parents=True, exist_ok=True)
    # Match idf_monitor's native USB Serial/JTAG connection sequence. A fresh
    # flash can leave the direct-FIFO transport silent until a startup reset.
    port = serial.Serial(baudrate=2000000, timeout=.25, write_timeout=5)
    port.dtr = port.rts = True
    port.port = args.port
    port.open()
    if not args.no_reset:
        port.rts = False
        port.dtr = False
        port.rts = True
        time.sleep(.2)
        port.rts = False
    time.sleep(1)  # Allow USB/firmware startup before sending the first command.
    with port, output.open("x") as journal:
        def save(kind, data):
            row = {"kind": kind, "utc": datetime.now(timezone.utc).isoformat(), **data}
            journal.write(json.dumps(row, separators=(",", ":")) + "\n")
            journal.flush()

        port.reset_input_buffer()
        nonce = secrets.randbits(32)
        command(port, f"SYNC {nonce}", f"SYNC {nonce}")
        command(port, f"FREQ {args.frequency}", "OK")
        command(port, "BANDWIDTH 13", "OK")
        command(port, f"HALOSYNC {sync:08X}", "OK")
        command(port, f"HALOBITS {args.bits}", "OK")
        command(port, f"HALOPOWER {args.min_power}", "OK")
        info = json.loads(command(port, "HALOINFO?", "HALOINFO ")[len("HALOINFO "):])
        if info.get("backend") != "esp32s3-iq" or info.get("lr1121_reset") is not True:
            raise RuntimeError(f"Unexpected firmware: {info}")
        save("info", {"port": args.port, "reset_on_connect": not args.no_reset, "firmware": info})
        print(f"Receiving at {args.frequency} MHz, sync {sync:08X}; saving {output}", flush=True)
        stop = time.monotonic() + args.seconds
        batches = candidates = complete = rf_us = elapsed_us = 0
        while time.monotonic() < stop:
            command(port, f"HALORUN {args.windows}", "HALOBEGIN")
            deadline = time.monotonic() + max(20, args.windows * .5)
            while True:
                line = read_line(port, deadline)
                if line.startswith("CANDIDATE "):
                    candidate = json.loads(line[len("CANDIDATE "):])
                    save("candidate", {"batch": batches, **candidate})
                    candidates += 1
                    complete += candidate["complete"]
                    print(f"candidate: {candidate['bits']} bits, complete={candidate['complete']}, "
                          f"clock={candidate['samples_per_bit']:.4f}, raw={candidate['raw']}", flush=True)
                elif line.startswith("HALOEND "):
                    result = json.loads(line[len("HALOEND "):])
                    save("batch", {"batch": batches, **result})
                    if result["error"]:
                        raise RuntimeError(f"Capture failed: {result}")
                    rf_us += result["rf_us"]
                    elapsed_us += result["elapsed_us"]
                    batches += 1
                    break
                elif line.startswith("ERR"):
                    raise RuntimeError(line)
        duty = 100 * rf_us / elapsed_us if elapsed_us else 0
        save("summary", {"batches": batches, "candidates": candidates, "complete": complete,
                         "rf_us": rf_us, "firmware_elapsed_us": elapsed_us, "duty_percent": duty})
        print(f"{candidates} candidates ({complete} complete), nominal capture duty {duty:.1f}%", flush=True)


if __name__ == "__main__":
    main()
