"""Bounded sampled mesh evidence on an explicit, unmodified time interval."""
from dataclasses import asdict, dataclass, field
from fractions import Fraction
import math

from .contracts import ClockStamp
from .mesh_analysis import METHOD_ID, MeshPairResult
from .temporal import TimeInterval


def _rational_mapping(value):
    return None if value is None else dict(numerator=str(value.numerator), denominator=str(value.denominator))


@dataclass(frozen=True)
class MeshSampleGap:
    """Adjacent inputs in supplied order; unrelated clocks have no numeric gap.

    exact_seconds is the rational difference of the stored clock numbers. seconds
    is its rounded presentation, or None when unrelated or outside float range.
    Negative/zero gaps remain visible and make the interval insufficient.
    """
    first_sample_id: str
    second_sample_id: str
    first_acquired: ClockStamp
    second_acquired: ClockStamp
    exact_seconds: Fraction | None
    seconds: float | None
    exceeds_max_gap: bool | None

    def to_mapping(self):
        result = asdict(self)
        result['exact_seconds'] = _rational_mapping(self.exact_seconds)
        return result


@dataclass(frozen=True)
class MeshIntervalSummary:
    """Observed minima/counts, never interpolated or continuous intersection.

    complete means the supplied samples satisfy endpoint, cadence, identity and
    measurement requirements. It does not certify behavior between samples.
    When status is insufficient but aggregate_available is True, numeric fields
    describe only the measured inputs as partial evidence. Configuration changes
    suppress every aggregate measurement/count (None), retaining individual results.
    sample_count always counts all submitted results, including insufficient ones.
    """
    interval: TimeInterval
    max_gap_seconds: float
    max_samples: int
    status: str
    reasons: tuple
    results: tuple
    gaps: tuple
    aggregate_available: bool
    sample_count: int
    measured_sample_count: int | None
    sampled_intersection_count: int | None
    sampled_within_tolerance_count: int | None
    sampled_minimum_distance: float | None
    sampled_minimum_squared_distance: Fraction | None
    between_samples: str = field(default='not_evaluated', init=False)

    def to_mapping(self):
        return dict(format='mesh_interval_summary', schema_version=1, method_id=METHOD_ID,
                    interval=asdict(self.interval), max_gap_seconds=self.max_gap_seconds,
                    max_samples=self.max_samples, status=self.status, reasons=list(self.reasons),
                    results=[result.to_mapping() for result in self.results],
                    gaps=[gap.to_mapping() for gap in self.gaps],
                    aggregate_available=self.aggregate_available, sample_count=self.sample_count,
                    measured_sample_count=self.measured_sample_count,
                    sampled_intersection_count=self.sampled_intersection_count,
                    sampled_within_tolerance_count=self.sampled_within_tolerance_count,
                    sampled_minimum_distance=self.sampled_minimum_distance,
                    sampled_minimum_squared_distance=_rational_mapping(self.sampled_minimum_squared_distance),
                    between_samples=self.between_samples)


def summarize_mesh_interval(results, interval, *, max_gap_seconds, max_samples):
    """Summarize an explicit bounded list/tuple of immutable MeshPairResult values.

    Malformed arguments or exceeding max_samples raise ValueError before copying
    inputs. Missing/duplicate/reversed/unrelated/outside samples and measurement
    failures return insufficient evidence without sorting, filtering or shrinking
    the interval. The first and last acquisitions must exactly equal its endpoints.
    Gaps compare exact rational clock differences to the supplied positive bound.

    Stable configuration includes ordered subjects/streams, region content,
    requirement criteria, units, method and tolerance. Pose frame/revision, source
    positions, transforms and per-frame observation identities may change.
    """
    if type(max_samples) is not int or not 1 <= max_samples <= 2**53-1:
        raise ValueError('max_samples must be a positive bounded integer')
    if type(results) not in (list, tuple) or len(results) > max_samples:
        raise ValueError('Expected an explicit list/tuple within max_samples')
    if not isinstance(interval, TimeInterval):
        raise ValueError('Expected an explicit TimeInterval')
    try:
        valid_gap = type(max_gap_seconds) in (int, float) and math.isfinite(max_gap_seconds) and max_gap_seconds > 0
    except OverflowError:
        valid_gap = False
    if not valid_gap:
        raise ValueError('max_gap_seconds must be finite and positive')
    if any(not isinstance(result, MeshPairResult) for result in results):
        raise ValueError('Expected MeshPairResult inputs')
    results = tuple(results)
    reasons, gaps = [], []
    sample_ids, times = set(), set()
    comparable = True
    first_key = None
    bound = Fraction(max_gap_seconds)
    for index, result in enumerate(results):
        stamp = result.acquired
        key = (METHOD_ID, result.first.configuration_key(), result.second.configuration_key(), result.tolerance)
        if first_key is None:
            first_key = key
        elif key != first_key:
            comparable = False
            reasons.append(f'sample:{index}:configuration_changed')
        if result.sample_id in sample_ids:
            reasons.append(f'sample:{index}:duplicate_sample_id')
        sample_ids.add(result.sample_id)
        clock_key = (stamp.domain, stamp.seconds)
        if clock_key in times:
            reasons.append(f'sample:{index}:duplicate_acquisition_time')
        times.add(clock_key)
        if stamp.domain != interval.start.domain:
            reasons.append(f'sample:{index}:unrelated_clock')
        elif not interval.start.seconds <= stamp.seconds <= interval.end.seconds:
            reasons.append(f'sample:{index}:outside_interval')
        if result.status != 'measured':
            reasons.append(f'sample:{index}:measurement_insufficient')
        if index:
            previous = results[index-1]
            exact = seconds = exceeds = None
            if previous.acquired.domain == stamp.domain:
                exact = Fraction(stamp.seconds) - Fraction(previous.acquired.seconds)
                try:
                    seconds = float(exact)
                except OverflowError:
                    seconds = None
                exceeds = exact > bound
                if exact < 0:
                    reasons.append(f'gap:{index-1}:reversed_acquisition_time')
                if exceeds:
                    reasons.append(f'gap:{index-1}:exceeds_max_gap')
            gaps.append(MeshSampleGap(previous.sample_id, result.sample_id, previous.acquired,
                                      stamp, exact, seconds, exceeds))
    if not results or results[0].acquired != interval.start:
        reasons.append('missing_start_sample')
    if not results or results[-1].acquired != interval.end:
        reasons.append('missing_end_sample')
    measured = [result for result in results if result.status == 'measured'] if comparable else []
    minimum = min(measured, key=lambda result: result.minimum_squared_distance) if measured else None
    return MeshIntervalSummary(
        interval, max_gap_seconds, max_samples, 'insufficient' if reasons else 'complete',
        tuple(reasons), results, tuple(gaps), comparable, len(results),
        len(measured) if comparable else None,
        sum(result.surface_intersection for result in measured) if comparable else None,
        sum(result.within_tolerance for result in measured) if comparable else None,
        minimum.minimum_distance if minimum else None,
        minimum.minimum_squared_distance if minimum else None)
