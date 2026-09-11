"""Read canonical portable visual evidence with explicit clock domains."""
import base64
import hashlib
from html.parser import HTMLParser
import json
from .contracts import ClockStamp, _name as _text


class _EvidenceParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.active = False
        self.blocks = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "script" and attrs.get("id") == "visual-evidence" and attrs.get("type") == "application/json":
            self.active = True
            self.blocks.append("")

    def handle_data(self, data):
        if self.active:
            self.blocks[-1] += data

    def handle_endtag(self, tag):
        if tag == "script":
            self.active = False


def read_document(path):
    """Read embedded evidence without executing HTML or accessing external images."""
    content = path.read_bytes()
    parser = _EvidenceParser()
    parser.feed(content.decode("utf-8-sig"))
    if len(parser.blocks) != 1:
        raise ValueError("Expected exactly one portable visual-evidence block")
    evidence = json.loads(parser.blocks[0])
    if not isinstance(evidence, dict):
        raise ValueError("Visual evidence must be an object")
    return evidence, hashlib.sha256(content).hexdigest()


def validate_evidence(evidence):
    if not isinstance(evidence, dict) or type(evidence.get("schema_version")) is not int or evidence["schema_version"] != 2:
        raise ValueError("Expected canonical visual evidence schema 2")
    frames = evidence.get("frames", [])
    if not isinstance(frames, list) or not frames:
        raise ValueError("Visual evidence has no retained frames")
    seen = set()
    for frame in frames:
        if not isinstance(frame, dict):
            raise ValueError("Visual evidence frames must be objects")
        name = frame["file"]
        _text(name, "frame identifier")
        if name in seen:
            raise ValueError("Duplicate visual evidence frame")
        seen.add(name)
        if not frame["image"].startswith("data:image/png;base64,"):
            raise ValueError("Only embedded PNG evidence is supported")
        pixels = base64.b64decode(frame["image"].partition(",")[2], validate=True)
        if not pixels.startswith(b"\x89PNG\r\n\x1a\n") or hashlib.sha256(pixels).hexdigest() != frame["image_sha256"]:
            raise ValueError("Embedded visual evidence hash mismatch")
        ClockStamp.from_mapping(frame.get("time"))
    return evidence


def load_evidence(path):
    evidence, digest = read_document(path)
    validate_evidence(evidence)
    return evidence, digest
