"""Strict structured input and bounded RGB/RGBA PNG integrity checks."""
import json
import math
from pathlib import Path
import struct
import zlib

from .errors import EvidenceError


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"), parse_float=lambda token: number(float(token)),
                          parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"Non-finite number: {value}")))
    except (OSError, ValueError) as error:
        raise EvidenceError(f"{path.name}: {error}") from error


def read_lines(path: Path):
    try:
        return [json.loads(line, parse_float=lambda token: number(float(token)), parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"Non-finite number: {value}")))
                for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    except (OSError, ValueError) as error:
        raise EvidenceError(f"{path.name}: {error}") from error


def bundle_path(root: Path, name: str) -> Path:
    root = Path(root)
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise EvidenceError(f"Path escapes evidence directory: {name}")
    return path


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise EvidenceError(f"Expected finite number, got {value!r}")
    return value


def vector(value):
    if not isinstance(value, list) or len(value) != 3:
        raise EvidenceError("Expected a three-component position")
    return [number(item) for item in value]


def png_dimensions(path: Path):
    """Check PNG chunk CRCs and decompressed scanline size, not just a file extension."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise EvidenceError(f"Invalid PNG: {path.name}")
    offset, compressed, dimensions, ended = 8, bytearray(), None, False
    while offset < len(data):
        if offset + 12 > len(data):
            raise EvidenceError(f"Truncated PNG: {path.name}")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        end = offset + 12 + length
        if end > len(data) or zlib.crc32(kind + body) != struct.unpack_from(">I", data, end - 4)[0]:
            raise EvidenceError(f"PNG checksum/length mismatch: {path.name}")
        if kind == b"IHDR":
            if dimensions is not None or offset != 8 or length != 13:
                raise EvidenceError("Invalid PNG header")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
            if not (0 < width <= 16384 and 0 < height <= 16384 and depth == 8
                    and color in (2, 6) and compression == filtering == interlace == 0):
                raise EvidenceError("Unsupported PNG format (expected 8-bit RGB/RGBA, non-interlaced)")
            dimensions = width, height
            stride = width * (3 if color == 2 else 4) + 1
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            ended = length == 0 and end == len(data)
            break
        offset = end
    if not dimensions or not ended:
        raise EvidenceError(f"Incomplete PNG: {path.name}")
    decoder = zlib.decompressobj()
    expected = stride * dimensions[1]
    if expected > 128 * 1024 * 1024:
        raise EvidenceError("PNG exceeds analysis size limit")
    try:
        pixels = decoder.decompress(compressed, expected + 1)
    except zlib.error as error:
        raise EvidenceError(f"Invalid PNG compression: {path.name}") from error
    if len(pixels) != expected or not decoder.eof or decoder.unused_data or any(pixels[i] > 4 for i in range(0, len(pixels), stride)):
        raise EvidenceError(f"Invalid PNG scanlines: {path.name}")
    return dimensions
