"""Validate evidence-linked review records and render producer-neutral findings."""
from html import escape
import re
from .contracts import ClockStamp


def _text(value, label):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"Missing visual review text: {label}")


def validate_review(review, evidence, evidence_sha256):
    """Validate provenance and observations, without assessing their artistic truth."""
    if not isinstance(review, dict) or review.get("schema_version") != 1:
        raise ValueError("Unsupported visual review schema")
    if review.get("evidence_sha256") != evidence_sha256:
        raise ValueError("Visual review refers to changed or different evidence")
    if not isinstance(review.get("reviewer"), dict) or review["reviewer"].get("kind") not in ("human", "assistant_image_review", "automated_detector"):
        raise ValueError("Visual review must declare its reviewer method")
    _text(review["reviewer"].get("identity"), "reviewer identity")
    _text(review.get("title"), "title")
    _text(review.get("summary"), "summary")
    available = {frame["file"] for frame in evidence["frames"]}
    reviewed = review.get("reviewed_frames", [])
    if not isinstance(reviewed, list) or len(reviewed) != len(set(reviewed)) or not set(reviewed) <= available:
        raise ValueError("Reviewed frames must uniquely reference retained evidence")
    findings = review.get("findings", [])
    if not isinstance(findings, list) or not findings:
        raise ValueError("Visual review requires explicit findings or unreviewed dispositions")
    identifiers = set()
    for finding in findings:
        if not isinstance(finding, dict):
            raise ValueError("Visual findings must be objects")
        identifier = finding.get("id", "")
        if not isinstance(identifier, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", identifier) or identifier in identifiers:
            raise ValueError("Finding identifiers must be unique domain names")
        identifiers.add(identifier)
        assessment = finding.get("assessment")
        if assessment not in ("consistent", "concern", "indeterminate", "not_reviewed"):
            raise ValueError("Unsupported visual assessment; capture pass is not visual approval")
        if finding.get("basis") not in ("pixels", "pixels_and_telemetry", "not_reviewed"):
            raise ValueError("Visual finding must distinguish pixels from telemetry-assisted interpretation")
        for field in ("category", "observation", "interpretation", "limits", "next_action"):
            _text(finding.get(field), field)
        references = finding.get("evidence", [])
        if not isinstance(references, list) or len(references) != len(set(references)) or not set(references) <= set(reviewed):
            raise ValueError("Finding references an unreviewed or missing frame")
        if assessment != "not_reviewed" and (not references or finding["basis"] == "not_reviewed"):
            raise ValueError("Assessed findings require reviewed pixels")
        if assessment == "not_reviewed" and (references or finding["basis"] != "not_reviewed"):
            raise ValueError("Unreviewed findings cannot claim pixel observations")
    if review["reviewer"]["kind"] == "automated_detector":
        _text(review["reviewer"].get("version"), "detector version")
        _text(review["reviewer"].get("calibration_reference"), "detector calibration reference")
    return {"status": "review_recorded", "reviewed_frame_count": len(reviewed),
            "available_frame_count": len(available),
            "assessments": {name: sum(f["assessment"] == name for f in findings)
                            for name in ("consistent", "concern", "indeterminate", "not_reviewed")},
            "meaning": "Evidence-linked review recorded; this is not an automated visual-quality pass."}


def render_review(review, evidence, summary, evidence_link):
    """Render a task-independent findings report linking into a retained pixel viewer."""
    frames = {f["file"]: (index, f) for index, f in enumerate(evidence["frames"])}
    cards = []
    for finding in review["findings"]:
        references = []
        for name in finding["evidence"]:
            index, frame = frames[name]
            stamp = ClockStamp.from_mapping(frame.get("time"))
            clock, timestamp = escape(stamp.domain), stamp.seconds
            references.append(f'<a href="{escape(evidence_link, quote=True)}#frame={index}">{escape(name)} ({clock} {timestamp:.3f}s)</a>')
        cards.append('<article><h2>' + escape(finding["category"]) + ' â€” ' + escape(finding["assessment"]) + '</h2>'
                     + ''.join('<p><strong>' + label + ':</strong> ' + escape(finding[key]) + '</p>' for key, label in
                               (("observation", "Observation"), ("interpretation", "Interpretation"), ("limits", "Limits"), ("next_action", "Next action")))
                     + '<p>Basis: ' + escape(finding["basis"]) + '</p><p>' + ' Â· '.join(references) + '</p></article>')
    return ('<!doctype html><meta charset="utf-8"><title>' + escape(review["title"]) + '</title>'
            + '<style>body{font:16px/1.55 system-ui;background:#171a20;color:#eee;max-width:1100px;margin:32px auto;padding:0 24px}article{border-top:1px solid #555;padding:12px 0}a{color:#83ccff}h2{font-size:20px}</style>'
            + '<h1>' + escape(review["title"]) + '</h1><p>' + escape(review["summary"]) + '</p><p>Reviewer: '
            + escape(review["reviewer"]["identity"]) + ' / ' + escape(review["reviewer"]["kind"]) + '</p><p>'
            + escape(summary["meaning"]) + f' Reviewed {summary["reviewed_frame_count"]} of {summary["available_frame_count"]} retained frames.</p>'
            + ''.join(cards))
