"""Portable animation analysis; producers and project integrations are optional."""
from .contracts import ClockStamp, PoseKey, Projection, require_same_pose
from .geometry import project_point, segment_gap
from .evidence import load_evidence
from .reviews import render_review, validate_review
from .mesh_records import (FeatureCoverage, MeshCompletion, MeshEligibility, MeshObservation,
                           MeshRecordLimits, MeshRequest, MeshRequirement, MeshSection,
                           MeshTopology, assess_mesh_coverage)
from .mesh_replay import read_mesh_observation, write_mesh_observation
from .mesh_analysis import (MeshAnalysisLimits, MeshAnalysisStats, MeshInputIdentity,
                            MeshPairResult, MeshPairSample, MeshRegion, MeshSelection,
                            measure_mesh_pair)
from .mesh_intervals import MeshIntervalSummary, MeshSampleGap, summarize_mesh_interval

__all__ = ["ClockStamp", "PoseKey", "Projection", "require_same_pose", "project_point",
           "segment_gap", "load_evidence", "render_review", "validate_review",
           "FeatureCoverage", "MeshCompletion", "MeshEligibility", "MeshObservation",
           "MeshRecordLimits", "MeshRequest", "MeshRequirement", "MeshSection", "MeshTopology",
           "assess_mesh_coverage", "read_mesh_observation", "write_mesh_observation",
           "MeshAnalysisLimits", "MeshAnalysisStats", "MeshInputIdentity", "MeshPairResult",
           "MeshPairSample", "MeshRegion", "MeshSelection", "measure_mesh_pair",
           "MeshIntervalSummary", "MeshSampleGap", "summarize_mesh_interval"]
