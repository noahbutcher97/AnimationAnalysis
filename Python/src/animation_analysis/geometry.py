"""Pure geometry with caller-declared projection conventions."""
import math
from .contracts import Projection


def project_point(point, projection):
    if not isinstance(projection, Projection) or len(point) != 3 or any(type(x) not in (int, float) or not math.isfinite(x) for x in point):
        raise ValueError("Expected finite 3D point and a declared projection")
    matrix, rect = projection.matrix, projection.viewport
    position = [*point, 1]
    if projection.vector_convention == "row":
        clip = [sum(position[r] * matrix[r * 4 + c] for r in range(4)) for c in range(4)]
    else:
        clip = [sum(matrix[r * 4 + c] * position[c] for c in range(4)) for r in range(4)]
    if clip[3] <= 1e-6:
        raise ValueError("Review geometry lies behind the camera")
    y = clip[1] / clip[3]
    if projection.ndc_y_axis == "up":
        y = -y
    return [rect[0] + (clip[0] / clip[3] + 1) * (rect[2] - rect[0]) / 2,
            rect[1] + (y + 1) * (rect[3] - rect[1]) / 2]


def segment_gap(start, end, target, radius):
    if any(len(p) != 3 or not all(math.isfinite(x) for x in p) for p in (start, end, target)) or not math.isfinite(radius) or radius < 0:
        raise ValueError("Expected finite 3D points and a nonnegative sphere radius")
    delta = [b - a for a, b in zip(start, end)]
    length2 = sum(x*x for x in delta)
    if length2 <= 1e-12:
        raise ValueError("Degenerate segment")
    fraction = max(0, min(1, sum((t-a)*d for t, a, d in zip(target, start, delta)) / length2))
    return math.dist([a + fraction*d for a, d in zip(start, delta)], target) - radius


