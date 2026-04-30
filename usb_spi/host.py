#!/usr/bin/env python3
import serial
import struct
import time

PORT = "/dev/ttyACM0"
BAUD = 115200  # literally useless but sereial port requires a buad rate to open


def frame_encode(payload: bytes) -> bytes:
    return struct.pack(">H", len(payload)) + payload


def parse_frames(buf: bytearray):
    frames = []
    while len(buf) >= 2:
        length = struct.unpack(">H", buf[:2])[0]
        if len(buf) < 2 + length:
            break
        payload = bytes(buf[2 : 2 + length])
        frames.append(payload)
        del buf[: 2 + length]
    return frames


def main():
    rx_buf = bytearray()

    with serial.Serial(PORT, BAUD, timeout=0.1) as ser:
        time.sleep(2)

        while True:
            payload = b"\xaa\xbb\xcc\xdd\xee"
            frame = frame_encode(payload)
            print(f"TX: {frame.hex()}")
            ser.write(frame)

            time.sleep(0.2)

            while ser.in_waiting:
                rx_buf.extend(ser.read(ser.in_waiting))

            for p in parse_frames(rx_buf):
                print(f"RX payload: {p.hex()}")

            time.sleep(1)


if __name__ == "__main__":
    main()
