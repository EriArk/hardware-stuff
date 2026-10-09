"""Read-only framebuffer capture; convert native 4bpp to portrait PNG (stdlib)."""
import argparse
from pathlib import Path
import re
import struct
import time
import zlib

import provision_reader as reader


def png_from_native(data):
    assert len(data) == 960 * 540 // 2
    rows = bytearray()
    for y in range(960):
        rows.append(0) # PNG scanline filter: none.
        physical_x = 959-y
        for x in range(540):
            packed = data[x*480+physical_x//2]
            rows.append(((packed >> (4 if physical_x & 1 else 0)) & 15)*17)
    def chunk(tag, payload):
        return struct.pack('!I',len(payload))+tag+payload+struct.pack('!I',zlib.crc32(tag+payload))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!2I5B',540,960,8,0,0,0,0))+
            chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with reader.open_connection(args.port) as connection:
        try:
            lines = reader.transact(connection,'DISPLAY CAPTURE','DISPLAY CAPTURE BEGIN','capture',seconds=20)
            size = int(re.search(r'bytes=(\d+)', lines[-1])[1])
            assert size == 259200
            data = bytearray()
            deadline = time.monotonic()+30
            while len(data)<size and time.monotonic()<deadline:
                data.extend(connection.read(min(4096,size-len(data))))
            assert len(data)==size, 'Incomplete framebuffer'
            end = connection.read_until(b'DISPLAY CAPTURE COMPLETE\r\n')
            assert b'DISPLAY CAPTURE COMPLETE' in end
        finally:
            reader.resume_sync(connection)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(png_from_native(data))
    print('CAPTURE_SAVED', args.output, 'native_bytes=',len(data))


if __name__ == '__main__':
    main()
