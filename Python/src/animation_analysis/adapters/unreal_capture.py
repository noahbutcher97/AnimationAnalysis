"""Interpret existing Unreal row-vector projection and observed-pose records."""
from ..contracts import PoseKey, Projection, require_same_pose
from ..geometry import project_point


def project(point, frame):
    return project_point(point, Projection(frame["world_to_clip_row_major"],
                                          frame["projection_view_rect"], "row", "up"))


def linked_actor(sample, frame, role):
    actors = [a for a in sample["actors"] if a["role"] == role]
    links = [a for a in frame["pose_links"] if a["role"] == role]
    if len(actors) != 1 or len(links) != 1 or not actors[0].get("valid"):
        raise ValueError(f"Missing or ambiguous participant/pose link: {role}")
    actor, link = actors[0], links[0]
    def key(record):
        return PoseKey(role, "unreal.render", record.get("pose_engine_frame"),
                       record.get("pose_evaluation_serial"))
    try:
        require_same_pose(key(actor), key(link))
    except ValueError as error:
        raise ValueError(f"Frame and sample pose mismatch: {role}: {error}") from error
    return actor
