"""Producer-neutral observation identities and explicit coordinate conventions."""
from dataclasses import dataclass
import math


def _name(value, label):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"Expected a nonempty {label}")


def _finite(value):
    return type(value) in (int, float) and math.isfinite(value)


@dataclass(frozen=True)
class ClockStamp:
    domain: str
    seconds: float

    def __post_init__(self):
        _name(self.domain, "clock domain")
        if not _finite(self.seconds):
            raise ValueError("Clock seconds must be finite")

    @classmethod
    def from_mapping(cls, value):
        if not isinstance(value, dict) or set(value) != {"domain", "seconds"}:
            raise ValueError("Clock requires exactly domain and seconds")
        return cls(**value)

    def delta_seconds(self, earlier):
        if not isinstance(earlier, ClockStamp) or self.domain != earlier.domain:
            raise ValueError("Cannot subtract unrelated clock domains")
        return self.seconds - earlier.seconds


@dataclass(frozen=True)
class PoseKey:
    subject_id: str
    stream_id: str
    frame_id: int
    revision: int

    def __post_init__(self):
        _name(self.subject_id, "subject identity")
        _name(self.stream_id, "stream identity")
        if any(type(v) is not int or v < 0 for v in (self.frame_id, self.revision)):
            raise ValueError("Pose frame and revision must be nonnegative integers")


def require_same_pose(observed, referenced):
    if not isinstance(observed, PoseKey) or not isinstance(referenced, PoseKey) or observed != referenced:
        raise ValueError("Observation and reference pose identity differ")


@dataclass(frozen=True)
class Projection:
    """Row-major storage; multiplication and NDC vertical direction are explicit.

    Coordinates use the same caller-declared world space as the matrix. The output
    viewport uses top-left image coordinates; visible points require positive W.
    """
    matrix: tuple
    viewport: tuple
    vector_convention: str
    ndc_y_axis: str

    def __post_init__(self):
        if not isinstance(self.matrix, (tuple, list)) or len(self.matrix) != 16 or not all(_finite(v) for v in self.matrix):
            raise ValueError("Projection requires 16 finite matrix values")
        if not isinstance(self.viewport, (tuple, list)) or len(self.viewport) != 4 or not all(_finite(v) for v in self.viewport):
            raise ValueError("Projection requires a finite viewport rectangle")
        if self.viewport[2] <= self.viewport[0] or self.viewport[3] <= self.viewport[1]:
            raise ValueError("Projection viewport is empty")
        if self.vector_convention not in ("row", "column") or self.ndc_y_axis not in ("up", "down"):
            raise ValueError("Unsupported projection convention")
        object.__setattr__(self, "matrix", tuple(self.matrix))
        object.__setattr__(self, "viewport", tuple(self.viewport))
