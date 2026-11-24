"""Takes a screenshot of the running Katana ZERO app over FTP (debugMode=1 required).

Usage: python scripts/vita_screenshot.py [out.png] [--host HOST]   (default host: VITA_HOST, see vita_common.py)
"""
import argparse
import ftplib
import io
import struct
import time

from PIL import Image

from vita_common import FTP_PORT, HOST, ftp_connect

SHARED = "ux0:/data/gms/shared"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="work/screenshot.png")
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=FTP_PORT)
    ap.add_argument("--timeout", type=float, default=15)
    args = ap.parse_args()

    ftp = ftp_connect(args.host, args.port, timeout=10)
    ftp.cwd(SHARED)
    try:
        ftp.delete("screenshot.raw")
    except ftplib.error_perm:
        pass
    ftp.storbinary("STOR screenshot.req", io.BytesIO(b"1"))

    deadline = time.time() + args.timeout
    data = None
    while time.time() < deadline:
        time.sleep(0.5)
        if "screenshot.raw" in ftp.nlst():
            buf = io.BytesIO()
            ftp.retrbinary("RETR screenshot.raw", buf.write)
            data = buf.getvalue()
            break
    ftp.quit()
    if data is None:
        raise SystemExit("No screenshot: is the game running with debugMode=1?")

    width, height, pitch = struct.unpack("<III", data[:12])
    img = Image.frombuffer("RGBA", (pitch, height), data[12:12 + pitch * height * 4], "raw", "RGBA", 0, 1)
    img.crop((0, 0, width, height)).convert("RGB").save(args.out)
    print(f"saved {args.out} ({width}x{height})")


if __name__ == "__main__":
    main()
