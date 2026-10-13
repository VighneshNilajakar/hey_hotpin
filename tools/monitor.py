import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(
        description="Capture the ESP-Waker serial console.")
    ap.add_argument("-p", "--port", default="COM13", help="serial port")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--seconds", type=float, default=60.0)
    ap.add_argument("-o", "--out", default="monitor_log.txt")
    ap.add_argument("--reset", action="store_true",
                    help="pulse EN before capturing (needs RTS wired to EN)")
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.2)
    # Keep the FTDI modem-control lines idle so opening the port does not
    # reset the board. DTR is IO0, RTS is EN on an ESP32-CAM-MB.
    s.dtr = False
    s.rts = False
    time.sleep(0.2)

    if args.reset:
        s.rts = True
        time.sleep(0.1)
        s.rts = False

    with open(args.out, "wb") as f:
        f.write(b"--- capture start ---\n")
        f.flush()
        t0 = time.time()
        while time.time() - t0 < args.seconds:
            chunk = s.read(1024)
            if chunk:
                f.write(chunk)
                f.flush()

    s.close()
    print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
