"""Validate and publish visual findings against a portable animation evidence page."""
import argparse
import hashlib
import json
import os
from pathlib import Path

from ..artifacts import atomic_text

from ..adapters.legacy_evidence import load_evidence, render_review, validate_review


def publish(evidence_path, findings_path, output):
    if output.suffix.lower() != ".html" or output.resolve() in (evidence_path.resolve(), findings_path.resolve()):
        raise ValueError("Use a separate HTML output path")
    if output.with_suffix(".json").resolve() in (evidence_path.resolve(), findings_path.resolve()):
        raise ValueError("Review output cannot overwrite its inputs")
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_text(output, "<!doctype html><p>Visual analysis incomplete; do not reuse a previous result.</p>")
    atomic_text(output.with_suffix(".json"), json.dumps({"status": "inconclusive", "reason": "Visual analysis generation incomplete"}))
    evidence, evidence_hash = load_evidence(evidence_path)
    finding_bytes = findings_path.read_bytes()
    review = json.loads(finding_bytes.decode("utf-8-sig"))
    summary = validate_review(review, evidence, evidence_hash)
    pixel_link = Path(os.path.relpath(evidence_path, output.parent)).as_posix()
    atomic_text(output, render_review(review, evidence, summary, pixel_link))
    result = dict(summary, review=review, evidence_path=str(evidence_path.resolve()),
                  evidence_sha256=evidence_hash, findings_sha256=hashlib.sha256(finding_bytes).hexdigest(),
                  input_hashes=evidence.get("input_hashes", {}))
    atomic_text(output.with_suffix(".json"), json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--findings", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = publish(args.evidence, args.findings, args.output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Visual analysis unavailable: {error}\n")
    print(f"{result['status']}: {args.output}; {result['assessments']}")


if __name__ == "__main__":
    main()
