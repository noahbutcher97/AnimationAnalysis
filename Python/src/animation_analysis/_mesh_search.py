"""Deterministic exact AABB search with explicit work counters.

Tree storage is linear in triangles (2*N-1 nodes). Node-pair traversal uses a
depth-first stack, ordering children by lower bound. Budgets bound visited pairs
and exact triangle tests, not wall time or rational arithmetic bit complexity.
"""
from dataclasses import dataclass
from fractions import Fraction

from ._triangle_geometry import Point, Triangle, triangle_distance_squared


@dataclass(frozen=True)
class SearchResult:
    distance_squared: Fraction
    first_point: Point
    second_point: Point
    first_triangle: int
    second_triangle: int
    triangle_tests: int
    node_visits: int


class SearchLimitError(RuntimeError):
    """Exhausted work, deliberately carrying no partial numeric verdict."""

    def __init__(self, triangle_tests: int, node_visits: int):
        self.triangle_tests = triangle_tests
        self.node_visits = node_visits
        super().__init__('mesh search work limit exhausted')


@dataclass(frozen=True, slots=True)
class _Node:
    low: Point
    high: Point
    count: int
    triangle_id: int | None = None
    triangle: Triangle | None = None
    left: '_Node | None' = None
    right: '_Node | None' = None


def _build(records):
    low = tuple(min(p[i] for _, t in records for p in t) for i in range(3))
    high = tuple(max(p[i] for _, t in records for p in t) for i in range(3))
    if len(records) == 1:
        identity, triangle = records[0]
        return _Node(low, high, 1, identity, triangle)
    axis = max(range(3), key=lambda i: high[i] - low[i])
    records.sort(key=lambda item: (sum(p[axis] for p in item[1]), item[0]))
    middle = len(records) // 2
    # Recursive slices live along a geometric series of balanced sizes; internal
    # nodes keep only bounds and children, never descendant triangle collections.
    return _Node(low, high, len(records), left=_build(records[:middle]),
                 right=_build(records[middle:]))


def _lower_bound(a, b):
    return sum(max(Fraction(0), a.low[i] - b.high[i], b.low[i] - a.high[i])**2
               for i in range(3))


def nearest_triangles(first, second, *, max_pair_tests, max_node_visits):
    """Find a certified global minimum, or raise without publishing a partial one.

    Inputs are nonempty bounded sequences of (global ordinal, valid triangle).
    The public boundary owns admitted triangle count and selected-geometry checks.
    Tied witnesses use deterministic tree traversal; they need not be the smallest
    ordinals among every equally close pair.
    """
    for bound in (max_pair_tests, max_node_visits):
        if type(bound) is not int or bound <= 0:
            raise ValueError('search limits must be positive integers')
    if not first or not second:
        raise ValueError('search requires nonempty triangle sequences')
    root_a, root_b = _build(list(first)), _build(list(second))
    stack = [(root_a, root_b)]
    triangle_tests = node_visits = 0
    best = None
    while stack:
        if node_visits >= max_node_visits:
            raise SearchLimitError(triangle_tests, node_visits)
        node_visits += 1
        a, b = stack.pop()
        if best is not None and _lower_bound(a, b) >= best[0]:
            continue
        if a.triangle is not None and b.triangle is not None:
            if triangle_tests >= max_pair_tests:
                raise SearchLimitError(triangle_tests, node_visits)
            triangle_tests += 1
            distance, p, q = triangle_distance_squared(a.triangle, b.triangle)
            if best is None or distance < best[0]:
                best = (distance, p, q, a.triangle_id, b.triangle_id)
            if distance == 0:
                break
        else:
            # Split one side only, keeping traversal stack linear (in fact
            # logarithmic in balanced tree depths), rather than a pair heap.
            if a.triangle is None and (b.triangle is not None or a.count >= b.count):
                pairs = [(a.left, b), (a.right, b)]
            else:
                pairs = [(a, b.left), (a, b.right)]
            pairs.sort(key=lambda pair: _lower_bound(*pair), reverse=True)
            # For ties, visit the left child first, preserving build order.
            if _lower_bound(*pairs[0]) == _lower_bound(*pairs[1]):
                pairs.reverse()
            stack.extend(pairs)
    return SearchResult(*best, triangle_tests, node_visits)
