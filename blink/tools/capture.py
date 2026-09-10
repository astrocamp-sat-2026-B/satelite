"""Capture one OV7675 photo via the Pico's USB CDC port."""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import time
import zlib


def read_exact(port, count, timeout=15):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < count:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Incomplete frame: {len(data)}/{count} bytes")
        data.extend(port.read(count - len(data)))
    return bytes(data)


def wait_line(port, prefix, timeout=10):
    deadline = time.monotonic() + timeout
    line = bytearray()
    while time.monotonic() < deadline:
        char = port.read(1)
        if not char:
            continue
        line.extend(char)
        if len(line) > 512:
            raise ValueError("Invalid response; check firmware and COM port")
        if char == b"\n":
            text = line.decode("ascii", errors="replace").strip()
            line.clear()
            if text.startswith("ERR"):
                raise RuntimeError(text)
            if text.startswith(prefix):
                return text
            if text:
                print(text)
    raise TimeoutError(f"No {prefix} response; check camera.uf2 and USB COM port")


def receive_frame(port):
    fields = wait_line(port, "FRAME ").split()
    if len(fields) != 6:
        raise ValueError("Invalid FRAME header")
    _, width, height, fmt, size, checksum = fields
    width, height, size = int(width), int(height), int(size)
    if (width, height, fmt, size) != (320, 240, "RGB565", 153600):
        raise ValueError("Unsupported dimensions or pixel format")
    data = read_exact(port, size)
    if zlib.crc32(data) != int(checksum, 16):
        raise ValueError("CRC mismatch: image was corrupted during transfer; retry")
    return width, height, data


def rgb565_to_rgb(data, swap_bytes=False, swap_rb=False):
    if len(data) % 2:
        raise ValueError("RGB565 data must contain complete pixels")
    rgb = bytearray(len(data) // 2 * 3)
    for src in range(0, len(data), 2):
        a, b = data[src:src + 2]
        # This board's OV7675 outputs the low RGB565 byte first.
        value = (a << 8 | b) if swap_bytes else (b << 8 | a)
        r, g, blue = (value >> 11) & 31, (value >> 5) & 63, value & 31
        r, g, blue = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (blue << 3) | (blue >> 2)
        if swap_rb:
            r, blue = blue, r
        dst = src // 2 * 3
        rgb[dst:dst + 3] = bytes((r, g, blue))
    return bytes(rgb)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="List USB/serial ports")
    parser.add_argument("--port", help="Pico USB CDC port, e.g. COM5")
    parser.add_argument("--test-pattern", action="store_true", help="Capture sensor color bars")
    parser.add_argument("--output", type=Path, help="PNG path (default: photos/timestamp.png)")
    parser.add_argument("--no-show", action="store_true")
    parser.add_argument("--swap-bytes", action="store_true", help="Diagnostic byte-order override")
    parser.add_argument("--swap-rb", action="store_true", help="Diagnostic red/blue override")
    args = parser.parse_args()
    import serial
    from serial.tools import list_ports
    if args.list:
        ports = list(list_ports.comports())
        for port in ports:
            print(f"{port.device}: {port.description} [{port.hwid}]")
        if not ports:
            print("No serial ports found. Connect the Pico with camera.uf2 installed.")
        return
    if not args.port:
        parser.error("Specify --port COMx (use --list to find it)")
    from PIL import Image
    with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=3) as port:
        time.sleep(0.5)
        port.reset_input_buffer()
        port.write(b"I")
        print(wait_line(port, "OK "))
        port.write(b"T" if args.test_pattern else b"C")
        width, height, data = receive_frame(port)
    image = Image.frombytes("RGB", (width, height), rgb565_to_rgb(data, args.swap_bytes, args.swap_rb))
    output = args.output or Path("photos") / (datetime.now().strftime("%Y%m%d_%H%M%S_%f") + ".png")
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, format="PNG")
    print(f"Saved {output.resolve()} ({width}x{height}, CRC OK)")
    if not args.no_show:
        image.show()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, TimeoutError, ImportError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
