"""Content identity and atomic publication, independent of artifact selection policy."""
import hashlib
import json
import os
from pathlib import Path
import uuid

from .errors import EvidenceError
from .integrity import bundle_path


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def identity(value):
    """Hash canonical JSON; callers supply the complete identity-bearing value."""
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def file_manifest(root, names):
    """Hash explicitly selected relative files under root; never discover inputs.

    Canonical relative paths make identity independent of the installation path.
    Escaping paths and duplicate canonical entries are rejected, including links
    that resolve outside the root. Missing/unreadable inputs remain errors.
    """
    root = Path(root).resolve()
    result = {}
    for name in names:
        if Path(name).is_absolute():
            raise EvidenceError("Manifest entries must be relative paths")
        path = bundle_path(root, name)
        key = path.relative_to(root).as_posix()
        if key in result:
            raise EvidenceError(f"Duplicate manifest entry: {key}")
        result[key] = digest(path)
    return dict(sorted(result.items()))


def implementation_manifest():
    """Conservative source identity for this installed package, excluding caches.

    External decoders and model/profile identities belong in the caller's
    provenance too; this manifest does not claim to identify those dependencies.
    """
    root = Path(__file__).resolve().parent
    return file_manifest(root, (p.relative_to(root) for p in root.rglob("*.py")))


def atomic_text(path, text):
    """Replace one UTF-8 artifact; failure preserves its previous complete bytes.

    This is per-file atomicity, not a transaction across a report and sidecar.
    Callers still publish a pending state before starting a multi-file job.
    """
    path = Path(path)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(text, encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_json(path, value):
    atomic_text(path, json.dumps(value, indent=2, allow_nan=False) + "\n")
