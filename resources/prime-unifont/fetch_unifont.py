#!/usr/bin/env python3
import gzip
from pathlib import Path
from urllib.request import urlopen, Request

VERSION = "17.0.04"
URL = f"https://unifoundry.com/pub/unifont/unifont-{VERSION}/font-builds/unifont-{VERSION}.hex.gz"
OUT = Path(__file__).resolve().parent / f"unifont-{VERSION}.hex"

OUT.parent.mkdir(parents=True, exist_ok=True)
req = Request(URL, headers={"User-Agent": "suika-prime-font-fetch/1.0"})
with urlopen(req, timeout=30) as r, gzip.GzipFile(fileobj=r) as gz, OUT.open("wb") as f:
    while True:
        chunk = gz.read(1024 * 64)
        if not chunk:
            break
        f.write(chunk)
print(OUT)
