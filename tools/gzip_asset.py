#!/usr/bin/env python3
"""Gzip one web asset for embedding in the firmware.

The output is reproducible (no file name, mtime 0) so that unchanged sources give an identical
firmware image. The asset must be valid UTF-8 text without external references (no CDN): the
portal works without Internet access.

Usage: gzip_asset.py INPUT OUTPUT
"""

import gzip
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    text = data.decode("utf-8")  # raises on invalid UTF-8
    if re.search(r"(src|href)\s*=\s*[\"']?\s*(https?:)?//", text, re.IGNORECASE) or re.search(
        r"url\(\s*[\"']?\s*(https?:)?//", text, re.IGNORECASE
    ):
        print(f"{src}: external reference found, the portal must work offline", file=sys.stderr)
        return 1
    with open(dst, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as gz:
            gz.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
