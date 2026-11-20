"""Download the unmodified MiSans Regular face and its license from Xiaomi."""

import io
from pathlib import Path
import re
import sys
from urllib.parse import quote
from urllib.request import Request, urlopen
from zipfile import ZipFile

URL = "https://hyperos.mi.com/font-download/MiSans.zip"
LICENSE_URL = "https://hyperos.mi.com/font-download/" + quote("MiSans字体知识产权许可协议.pdf")


def read_range(byte_range):
    request = Request(URL, headers={"Range": f"bytes={byte_range}"})
    with urlopen(request, timeout=45) as response:
        match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", response.headers.get("Content-Range", ""))
        if response.status != 206 or not match:
            raise OSError("Xiaomi's download did not return the requested byte range")
        first, last, length = map(int, match.groups())
        data = response.read()
        if len(data) != last - first + 1:
            raise OSError("The Xiaomi download was incomplete")
        return first, length, data


class FontArchive:
    def __init__(self):
        self.tail_start, self.length, self.tail = read_range("-65536")
        self.offset = 0

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_CUR:
            offset += self.offset
        elif whence == io.SEEK_END:
            offset += self.length
        self.offset = offset
        return offset

    def tell(self):
        return self.offset

    def seekable(self):
        return True

    def read(self, size=-1):
        first = self.offset
        last = self.length if size < 0 else min(self.length, first + size)
        if last <= first:
            return b""
        if first >= self.tail_start:
            data = self.tail[first - self.tail_start:last - self.tail_start]
        else:
            start, length, data = read_range(f"{first}-{last - 1}")
            if start != first or length != self.length or len(data) != last - first:
                raise OSError("The Xiaomi archive changed while it was being read")
        self.offset = last
        return data


if __name__ == "__main__":
    output = Path(sys.argv[1])
    license_path = output.with_name("MiSans-License.pdf")
    if not license_path.exists():
        with urlopen(LICENSE_URL, timeout=45) as response:
            agreement = response.read()
        if not agreement.startswith(b"%PDF-"):
            raise OSError("Xiaomi's MiSans license download did not return a PDF")
        license_path.write_bytes(agreement)

    with ZipFile(FontArchive()) as archive:
        entry, = [name for name in archive.namelist() if name.rsplit("/", 1)[-1] == "MiSans-Regular.ttf"]
        output.write_bytes(archive.read(entry))
        print(f"Embedded MiSans Regular: {output.stat().st_size:,} bytes")
