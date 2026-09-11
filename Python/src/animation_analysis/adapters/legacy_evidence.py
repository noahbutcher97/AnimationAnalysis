"""Normalize legacy montage/simulation clocks without changing retained evidence."""
from ..contracts import ClockStamp
from ..evidence import read_document, validate_evidence
from ..reviews import render_review as render_normalized_review, validate_review


def normalize_evidence(evidence):
    if not isinstance(evidence, dict):
        raise ValueError("Unsupported visual evidence schema")
    version = evidence.get("schema_version")
    if version is not None and (type(version) is not int or version not in (1, 2)):
        raise ValueError("Unsupported visual evidence schema")
    if evidence.get("schema_version") == 2:
        return evidence
    frames = []
    for frame in evidence.get("frames", []):
        if not isinstance(frame, dict):
            raise ValueError("Visual evidence frames must be objects")
        domain = "montage" if "montage_time_s" in frame else "simulation"
        stamp = ClockStamp(domain, frame.get(domain + "_time_s"))
        value = {"domain": stamp.domain, "seconds": stamp.seconds}
        if "time" in frame and frame["time"] != value:
            raise ValueError("Legacy and normalized evidence clocks disagree")
        frames.append(dict(frame, time=value))
    return dict(evidence, schema_version=2, frames=frames)


def load_evidence(path):
    evidence, digest = read_document(path)
    evidence = normalize_evidence(evidence)
    validate_evidence(evidence)
    return evidence, digest


def render_review(review, evidence, summary, evidence_link):
    return render_normalized_review(review, normalize_evidence(evidence), summary, evidence_link)
