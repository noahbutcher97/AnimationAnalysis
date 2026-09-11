"""General numeric summaries and explicit evaluation status aggregation."""
import statistics

from .errors import EvidenceError
from .integrity import number

STATUSES = ("fail", "inconclusive", "pass", "not_run")


def summarize_statuses(statuses):
    """Worst executed status wins; unexecuted cases establish no verdict.

    A returned pass applies to executed cases only. Consumers must retain their
    complete case list so unexecuted cases cannot be mistaken for coverage.
    """
    states = set(statuses)
    if states - set(STATUSES):
        raise EvidenceError("Unknown evaluation status")
    return next((state for state in STATUSES if state in states), "not_run")


def distribution(values):
    values = [number(value) for value in values]
    return {"count": len(values), "min": min(values), "median": statistics.median(values), "max": max(values)} if values else {"count": 0, "min": None, "median": None, "max": None}


def numeric_deltas(current, baseline, prefix=""):
    """Compare shared numeric mapping leaves, without choosing metrics or limits.

    Missing fields, sequences and booleans are not numeric observations. Finite
    numeric leaves are required; eligibility and units belong to the consumer.
    """
    changes = {}
    def walk(a, b, path):
        if isinstance(a, dict) and isinstance(b, dict):
            for key in sorted(a.keys() & b.keys()):
                walk(a[key], b[key], f"{path}.{key}" if path else key)
        elif type(a) in (int, float) and type(b) in (int, float):
            changes[path] = {"current": number(a), "baseline": number(b), "delta": number(a - b)}
    walk(current, baseline, prefix)
    return changes
