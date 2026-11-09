#!/usr/bin/env python3
"""Generate a C++ resource from an OFL font; called by CMake."""
import pathlib
import sys
import zlib

source, output, symbol = sys.argv[1:]
raw = pathlib.Path(source).read_bytes()
compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
data = compressor.compress(raw) + compressor.flush()
path = pathlib.Path(output)
path.parent.mkdir(parents=True, exist_ok=True)
with path.open('w') as out:
    out.write('// Generated from Noto Sans CJK SC (SIL OFL 1.1). Do not edit.\n')
    out.write('#include <cstddef>\nnamespace cjk::data {\n')
    out.write(f'extern const unsigned char {symbol}[] =\n')
    for offset in range(0, len(data), 128):
        out.write('"' + ''.join(f'\\x{byte:02x}' for byte in data[offset:offset + 128]) + '"\n')
    out.write(f';\nextern const size_t {symbol}Size = sizeof({symbol}) - 1;\n}}\n')
