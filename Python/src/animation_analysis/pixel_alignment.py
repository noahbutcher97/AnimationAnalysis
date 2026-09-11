"""Measure a visible coloured segment independently of its projected telemetry.

The region and unobscured subject identity are reviewed inputs. This detector does
not infer visibility, identify arbitrary weapons, or measure mesh surface contact.
"""
from dataclasses import asdict, dataclass, fields
import math
import hashlib
from importlib.metadata import version
from pathlib import Path

VERSION = "1"


def detector_identity():
    """Calibration is tied to actual measurement/decoder code and decoder version."""
    directory = Path(__file__).parent
    digest = hashlib.sha256()
    for name in ("pixel_alignment.py", "images.py"):
        digest.update(name.encode())
        digest.update((directory / name).read_bytes())
    digest.update(version("Pillow").encode())
    return digest.hexdigest()


@dataclass(frozen=True)
class SegmentAlignmentSettings:
    minimum_green: int = 80
    minimum_blue: int = 80
    minimum_green_over_red: int = 30
    minimum_blue_over_red: int = 30
    component_join_radius_px: int = 2
    minimum_component_pixels: int = 30
    significant_fragment_pixels: int = 12
    maximum_other_component_fraction: float = 0.6
    minimum_span_px: float = 20
    maximum_line_width_rms_px: float = 2
    minimum_elongation: float = 50
    maximum_alignment_error_px: float = 3
    minimum_span_overlap: float = 0.5

    def validate(self):
        for key, value in asdict(self).items():
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ValueError(f"Non-finite detector setting: {key}")
        for key in ("minimum_green", "minimum_blue", "minimum_green_over_red", "minimum_blue_over_red"):
            if type(getattr(self, key)) is not int or not 0 <= getattr(self, key) <= 255:
                raise ValueError("Colour thresholds must be integer channel values")
        if type(self.component_join_radius_px) is not int or not 1 <= self.component_join_radius_px <= 3:
            raise ValueError("Component join radius must be 1..3 pixels")
        if any(type(v) is not int or v < 2 for v in (self.minimum_component_pixels, self.significant_fragment_pixels)):
            raise ValueError("Component pixel counts must be integers >= 2")
        if not 0 < self.maximum_other_component_fraction < 1 or not 0 < self.minimum_span_overlap <= 1:
            raise ValueError("Invalid ambiguity or span fraction")
        if min(self.minimum_span_px, self.maximum_line_width_rms_px, self.maximum_alignment_error_px) <= 0 or self.minimum_elongation <= 1:
            raise ValueError("Invalid segment fit limits")


def _abstain(reason, **measurements):
    return dict(assessment="indeterminate", reason=reason, measurements=measurements)


def analyze_segment(rgb, width, height, region, expected_segment, visibility, settings):
    """Fit pixels first, then compare telemetry. Never move the region with a control."""
    settings.validate()
    if visibility not in ("visible", "occluded", "unknown"):
        raise ValueError("Visibility must be visible, occluded or unknown")
    if visibility != "visible":
        return _abstain("Subject identity and unobscured visibility are not established")
    if type(width) is not int or type(height) is not int or width <= 0 or height <= 0 or len(rgb) != width * height * 3:
        raise ValueError("Expected packed RGB pixels matching image dimensions")
    if len(region) != 4 or any(type(v) is not int for v in region):
        raise ValueError("Expected an integer pixel region")
    x0, y0, x1, y1 = region
    if not 0 <= x0 < x1 <= width or not 0 <= y0 < y1 <= height:
        return _abstain("Reviewed region is outside the image")
    if len(expected_segment) != 2 or any(len(p) != 2 or not all(math.isfinite(v) for v in p) for p in expected_segment):
        raise ValueError("Expected a finite projected segment")
    expected_length = math.dist(*expected_segment)
    if expected_length < settings.minimum_span_px:
        return _abstain("Projected segment is too short for this calibration")
    points = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            index = (y * width + x) * 3
            r, g, b = rgb[index:index+3]
            if g >= settings.minimum_green and b >= settings.minimum_blue and g-r >= settings.minimum_green_over_red and b-r >= settings.minimum_blue_over_red:
                points.add((x, y))
    components = []
    radius = settings.component_join_radius_px
    offsets = [(x, y) for x in range(-radius, radius+1) for y in range(-radius, radius+1) if x or y]
    while points:
        seed = points.pop()
        pending, component = [seed], [seed]
        while pending:
            x, y = pending.pop()
            for dx, dy in offsets:
                neighbour = (x+dx, y+dy)
                if neighbour in points:
                    points.remove(neighbour)
                    pending.append(neighbour)
                    component.append(neighbour)
        components.append(component)
    components.sort(key=len, reverse=True)
    if not components or len(components[0]) < settings.minimum_component_pixels:
        return _abstain("Insufficient matching pixels")
    primary = components[0]
    other_count = sum(len(c) for c in components[1:] if len(c) >= settings.significant_fragment_pixels)
    if other_count >= len(primary) * settings.maximum_other_component_fraction:
        return _abstain("Competing segments or fragmented evidence", primary_pixels=len(primary), other_pixels=other_count)
    if any(x <= x0 or x >= x1-1 or y <= y0 or y >= y1-1 for x, y in primary):
        return _abstain("Detected component touches the reviewed region boundary")
    count = len(primary)
    cx = sum(x for x, y in primary) / count
    cy = sum(y for x, y in primary) / count
    xx = sum((x-cx)**2 for x, y in primary) / count
    yy = sum((y-cy)**2 for x, y in primary) / count
    xy = sum((x-cx)*(y-cy) for x, y in primary) / count
    discriminant = math.sqrt((xx-yy)**2 + 4*xy*xy)
    major, minor = (xx+yy+discriminant)/2, max(0, (xx+yy-discriminant)/2)
    elongation, line_width = major/max(minor, 1e-9), math.sqrt(minor)
    if elongation < settings.minimum_elongation or line_width > settings.maximum_line_width_rms_px:
        return _abstain("Matching pixels do not form an unambiguous thin segment", elongation=elongation, line_width_rms_px=line_width)
    angle = 0.5 * math.atan2(2*xy, xx-yy)
    direction = (math.cos(angle), math.sin(angle))
    normal = (-direction[1], direction[0])
    along = [(x-cx)*direction[0]+(y-cy)*direction[1] for x, y in primary]
    low, high = min(along), max(along)
    if high-low < settings.minimum_span_px:
        return _abstain("Detected segment is too short for this calibration")
    detected = [[cx+t*direction[0], cy+t*direction[1]] for t in (low, high)]
    projected_along = [(p[0]-cx)*direction[0]+(p[1]-cy)*direction[1] for p in expected_segment]
    error = max(abs((p[0]-cx)*normal[0]+(p[1]-cy)*normal[1]) for p in expected_segment)
    overlap = max(0, min(high, max(projected_along))-max(low, min(projected_along))) / max(high-low, expected_length)
    consistent = error <= settings.maximum_alignment_error_px and overlap >= settings.minimum_span_overlap
    return dict(assessment="consistent" if consistent else "concern",
                reason="Visible segment agrees within declared limits" if consistent else "Visible segment and projected telemetry disagree",
                measurements=dict(detected_segment=detected, maximum_perpendicular_error_px=error, span_overlap_fraction=overlap,
                                  primary_pixels=count, other_pixels=other_count, elongation=elongation, line_width_rms_px=line_width))


def read_settings(profile):
    if profile.get("schema_version") != 1 or profile.get("detector") != "colored_segment_alignment" or profile.get("detector_version") != VERSION:
        raise ValueError("Unsupported detector profile")
    if not isinstance(profile.get("image_size"), list) or len(profile["image_size"]) != 2 or any(type(v) is not int or v <= 0 for v in profile["image_size"]):
        raise ValueError("Profile must declare its image dimensions")
    values = profile["settings"]
    if set(values) != {f.name for f in fields(SegmentAlignmentSettings)}:
        raise ValueError("Detector profile must declare every setting exactly")
    settings = SegmentAlignmentSettings(**values)
    settings.validate()
    return settings


