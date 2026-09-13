"""Exact distances between nondegenerate closed triangle surfaces.

This is a reference kernel over the stored rational coordinates, not a statement
about acquisition accuracy. No tolerance participates in intersection predicates.
"""
from fractions import Fraction

Point = tuple[Fraction, Fraction, Fraction]
Triangle = tuple[Point, Point, Point]


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])


def _along(a, direction, t):
    return tuple(x + t*y for x, y in zip(a, direction))


def _normal(triangle):
    return _cross(_sub(triangle[1], triangle[0]), _sub(triangle[2], triangle[0]))


def is_degenerate(triangle: Triangle) -> bool:
    """Whether the triangle has exactly zero area (including repeated vertices)."""
    return not any(_normal(triangle))


def _inside(point, triangle, normal):
    # For a point on the plane, all inward edge half-spaces include the boundary.
    return all(_dot(_cross(_sub(triangle[(i+1) % 3], triangle[i]),
                          _sub(point, triangle[i])), normal) >= 0 for i in range(3))


def _point_segment(point, start, end):
    direction = _sub(end, start)
    t = _dot(_sub(point, start), direction) / _dot(direction, direction)
    return _along(start, direction, max(Fraction(0), min(Fraction(1), t)))


def _segment_candidates(a, b, c, d):
    """Constrained segment minimum is interior/interior or on a box boundary."""
    yield a, _point_segment(a, c, d)
    yield b, _point_segment(b, c, d)
    yield _point_segment(c, a, b), c
    yield _point_segment(d, a, b), d
    u, v, w = _sub(b, a), _sub(d, c), _sub(a, c)
    uu, uv, vv = _dot(u, u), _dot(u, v), _dot(v, v)
    uw, vw = _dot(u, w), _dot(v, w)
    denominator = uu*vv - uv*uv
    if denominator:
        s = (uv*vw - vv*uw) / denominator
        t = (uu*vw - uv*uw) / denominator
        if 0 <= s <= 1 and 0 <= t <= 1:
            yield _along(a, u, s), _along(c, v, t)


def _piercing(start, end, triangle, normal):
    direction = _sub(end, start)
    denominator = _dot(direction, normal)
    if denominator:
        t = _dot(_sub(triangle[0], start), normal) / denominator
        if 0 <= t <= 1:
            point = _along(start, direction, t)
            if _inside(point, triangle, normal):
                return point
    return None


def triangle_distance_squared(a: Triangle, b: Triangle) -> tuple[Fraction, Point, Point]:
    """Return exact minimum squared distance and witnesses, rejecting zero area.

    Disjoint triangles attain their minimum on a vertex/face or edge/edge
    feature pair. Edge/face piercing additionally detects noncoplanar crossings;
    projected vertices and edge pairs cover coplanar overlap and boundary contact.
    """
    a = tuple(tuple(Fraction(v) for v in p) for p in a)
    b = tuple(tuple(Fraction(v) for v in p) for p in b)
    na, nb = _normal(a), _normal(b)
    if not any(na) or not any(nb):
        raise ValueError('triangle surface distance requires nondegenerate triangles')
    for first, second, normal in ((a, b, nb), (b, a, na)):
        for i in range(3):
            point = _piercing(first[i], first[(i+1) % 3], second, normal)
            if point is not None:
                return Fraction(0), point, point

    best = None

    def consider(first, second):
        nonlocal best
        delta = _sub(first, second)
        distance = _dot(delta, delta)
        if best is None or distance < best[0]:
            best = (distance, first, second)

    for first, second, normal, reverse in ((a, b, nb, False), (b, a, na, True)):
        normal_squared = _dot(normal, normal)
        for point in first:
            t = _dot(_sub(second[0], point), normal) / normal_squared
            projected = _along(point, normal, t)
            if _inside(projected, second, normal):
                consider(projected, point) if reverse else consider(point, projected)
                if best[0] == 0:
                    return best
    for i in range(3):
        for j in range(3):
            for first, second in _segment_candidates(a[i], a[(i+1) % 3],
                                                      b[j], b[(j+1) % 3]):
                consider(first, second)
                if best[0] == 0:
                    return best
    return best
