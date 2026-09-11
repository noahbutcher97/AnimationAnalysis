"""Intervals on declared clocks, without producer-specific record fields."""
from dataclasses import dataclass

from .contracts import ClockStamp
from .errors import EvidenceError
from .integrity import number


@dataclass(frozen=True)
class TimeInterval:
    start: ClockStamp
    end: ClockStamp

    def __post_init__(self):
        if not isinstance(self.start, ClockStamp) or not isinstance(self.end, ClockStamp):
            raise EvidenceError("Interval endpoints must declare their clocks")
        if self.start.domain != self.end.domain:
            raise EvidenceError("Interval endpoints use unrelated clock domains")
        if self.end.seconds <= self.start.seconds:
            raise EvidenceError("Event window is empty or reversed")


def event_window(events, first, last, before=0.0, after=0.0):
    """Find unique named endpoints from (name, ClockStamp) pairs and pad outward."""
    events = list(events)
    if number(before) < 0 or number(after) < 0:
        raise EvidenceError("Window padding must be nonnegative")
    def time(name):
        found = [stamp for label, stamp in events if label == name]
        if len(found) != 1:
            raise EvidenceError(f"Expected one {name} event, found {len(found)}")
        if not isinstance(found[0], ClockStamp):
            raise EvidenceError("Event time must declare its clock")
        return found[0]
    start, end = time(first), time(last)
    return TimeInterval(ClockStamp(start.domain, start.seconds - before), ClockStamp(end.domain, end.seconds + after))


def bracket_observations(rows, interval, timestamp):
    """Select the complete window including nearest observations on both sides.

    The caller maps each record to a ClockStamp. Input must be ordered; missing
    coverage, mixed clocks and reversed observations never silently shrink the
    requested interval. Duplicate times remain available to the consumer, which
    owns sample/pose identity and cadence policy.
    """
    rows = list(rows)
    stamps = [timestamp(row) for row in rows]
    if any(not isinstance(stamp, ClockStamp) or stamp.domain != interval.start.domain for stamp in stamps):
        raise EvidenceError("Observations use unrelated clock domains")
    times = [stamp.seconds for stamp in stamps]
    if any(right < left for left, right in zip(times, times[1:])):
        raise EvidenceError("Observations are not ordered by acquisition time")
    before = [time for time in times if time <= interval.start.seconds]
    after = [time for time in times if time >= interval.end.seconds]
    if not before or not after:
        raise EvidenceError("No observations bracket the complete event window")
    return [row for row, time in zip(rows, times) if before[-1] <= time <= after[0]]
