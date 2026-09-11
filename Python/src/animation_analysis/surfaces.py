"""Pure raster measurements; labels, depth tolerance and subject intent belong to callers."""
import math


def _squared_distance_line(values):
    """Lower envelope of parabolas: exact squared pixel-centre distance in O(n)."""
    sites = [i for i, v in enumerate(values) if math.isfinite(v)]
    if not sites:
        return [math.inf] * len(values)
    vertices, bounds = [sites[0]], [-math.inf, math.inf]
    for q in sites[1:]:
        while True:
            p = vertices[-1]
            crossing = ((values[q] + q*q) - (values[p] + p*p)) / (2*(q-p))
            if crossing > bounds[-2]:
                break
            vertices.pop()
            bounds.pop(-2)
        bounds[-1] = crossing
        vertices.append(q)
        bounds.append(math.inf)
    result, k = [], 0
    for q in range(len(values)):
        while bounds[k+1] < q:
            k += 1
        p = vertices[k]
        result.append((q-p)**2 + values[p])
    return result


def minimum_pixel_distance(first, second, width, height):
    """Distance between occupied pixel centres, not subpixel mesh boundaries."""
    # Horizontal/vertical passes avoid an O(boundary_a * boundary_b) pairwise scan.
    rows = [_squared_distance_line([0.0 if first[y*width+x] else math.inf for x in range(width)]) for y in range(height)]
    closest = math.inf
    for x in range(width):
        column = _squared_distance_line([rows[y][x] for y in range(height)])
        for y in range(height):
            if second[y*width+x]:
                closest = min(closest, column[y])
    return math.sqrt(closest)


def measure_surface_relation(labels, scene_depth_cm, label_depth_cm, width, height, first_id, second_id, visibility_tolerance_cm):
    """Measure visible separation and observed-label occlusion without a contact verdict.

    Labels describe the frontmost custom-depth surface. A second labelled surface
    hidden by the first is unavailable, not an empty/full silhouette measurement.
    """
    if type(width) is not int or type(height) is not int or min(width, height) <= 0 or width*height > 1920*1080:
        raise ValueError("Invalid or unbounded raster dimensions")
    if any(len(v) != width*height for v in (labels, scene_depth_cm, label_depth_cm)):
        raise ValueError("Raster channels must have matching dimensions")
    if any(type(v) is not int or not 1 <= v <= 255 for v in (first_id, second_id)) or first_id == second_id:
        raise ValueError("Subjects require distinct nonzero byte labels")
    if isinstance(visibility_tolerance_cm, bool) or not isinstance(visibility_tolerance_cm, (float, int)) or not math.isfinite(visibility_tolerance_cm) or visibility_tolerance_cm <= 0:
        raise ValueError("Visibility tolerance must be explicitly positive and finite")
    if any(type(v) is not int or not 0 <= v <= 255 for v in labels):
        raise ValueError("Labels must be unsigned byte values")
    if any(math.isnan(v) or v <= 0 for channel in (scene_depth_cm, label_depth_cm) for v in channel):
        raise ValueError("Depth must be positive camera-axis distance or clear infinity")
    masks, subjects = [], []
    for label in (first_id, second_id):
        visible = bytearray(width*height)
        observed = occluded = invalid = 0
        minimum, maximum = math.inf, -math.inf
        for i, value in enumerate(labels):
            if value != label:
                continue
            observed += 1
            depth, scene = label_depth_cm[i], scene_depth_cm[i]
            if not math.isfinite(depth) or not math.isfinite(scene) or depth < scene-visibility_tolerance_cm:
                invalid += 1
            elif depth > scene+visibility_tolerance_cm:
                occluded += 1
            else:
                visible[i] = 1
                minimum, maximum = min(minimum, depth), max(maximum, depth)
        count = sum(visible)
        masks.append(visible)
        subjects.append(dict(label=label, observed_label_pixels=observed, visible_pixels=count,
                             scene_occluded_pixels=occluded, inconsistent_depth_pixels=invalid,
                             observed_label_visible_fraction=count/observed if observed else None,
                             visible_depth_range_cm=[minimum, maximum] if count else None))
    measurable = all(s['visible_pixels'] > 0 and s['inconsistent_depth_pixels'] == 0 for s in subjects)
    distance = minimum_pixel_distance(*masks, width, height) if measurable else None
    return dict(status="measured" if measurable else "indeterminate", subjects=subjects,
                minimum_visible_pixel_center_distance_px=distance,
                visibility_tolerance_cm=visibility_tolerance_cm,
                limits="Visible pixel centres only. Frontmost custom labels omit surfaces behind other labelled surfaces. Pixel adjacency cannot distinguish touching from intersection; no complete silhouette, 3D contact or artistic verdict.")
