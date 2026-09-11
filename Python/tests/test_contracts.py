import unittest

from animation_analysis import ClockStamp, PoseKey, Projection, project_point, require_same_pose, segment_gap
from animation_analysis.adapters.unreal_capture import linked_actor, project


class ObservationContractTests(unittest.TestCase):
    def test_clock_domains_are_not_interchangeable(self):
        self.assertEqual(ClockStamp("sensor", 4).delta_seconds(ClockStamp("sensor", 1.5)), 2.5)
        with self.assertRaisesRegex(ValueError, "clock domains"):
            ClockStamp("sensor", 4).delta_seconds(ClockStamp("presentation", 4))
        for value in (True, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                ClockStamp("sensor", value)

    def test_delayed_results_retain_subject_stream_frame_and_revision(self):
        key = PoseKey("piston", "inspection", 12, 4)
        require_same_pose(key, PoseKey("piston", "inspection", 12, 4))
        for other in (PoseKey("rod", "inspection", 12, 4), PoseKey("piston", "other", 12, 4),
                      PoseKey("piston", "inspection", 13, 4), PoseKey("piston", "inspection", 12, 5)):
            with self.assertRaises(ValueError):
                require_same_pose(key, other)
        with self.assertRaises(ValueError):
            PoseKey("piston", "inspection", True, 4)

    def test_row_and_column_projection_agree_for_transposed_matrices(self):
        row = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .25, -.5, 0, 1]
        column = [row[c*4+r] for r in range(4) for c in range(4)]
        viewport = [10, 20, 210, 120]
        self.assertEqual(project_point([0, 0, 0], Projection(row, viewport, "row", "up")), [135, 95])
        self.assertEqual(project_point([0, 0, 0], Projection(column, viewport, "column", "up")), [135, 95])
        self.assertEqual(project_point([0, 0, 0], Projection(row, viewport, "row", "down")), [135, 45])

    def test_projection_owns_immutable_validated_input(self):
        matrix = [1 if i % 5 == 0 else 0 for i in range(16)]
        projection = Projection(matrix, [0, 0, 100, 100], "row", "up")
        matrix[15] = 0
        self.assertEqual(project_point([0, 0, 0], projection), [50, 50])
        with self.assertRaisesRegex(ValueError, "behind"):
            project_point([0, 0, 0], Projection(matrix, [0, 0, 100, 100], "row", "up"))
        with self.assertRaises(ValueError):
            Projection(matrix, [0, 0, 100, 100], "guess", "up")

    def test_generic_geometry_has_no_role_or_unit_default(self):
        self.assertEqual(segment_gap([0, 0, 0], [2, 0, 0], [1, 3, 0], 1), 2)
        with self.assertRaisesRegex(ValueError, "Degenerate segment"):
            segment_gap([0, 0, 0], [0, 0, 0], [1, 3, 0], 1)

    def test_legacy_projection_and_pose_adapter_preserve_checks(self):
        matrix = [1 if i % 5 == 0 else 0 for i in range(16)]
        frame = dict(world_to_clip_row_major=matrix, projection_view_rect=[0, 0, 100, 100],
                     pose_links=[dict(role="tool", pose_engine_frame=5, pose_evaluation_serial=8)])
        sample = dict(actors=[dict(role="tool", valid=True, pose_engine_frame=5, pose_evaluation_serial=8)])
        self.assertEqual(project([.5, .5, 0], frame), [75, 25])
        self.assertIs(linked_actor(sample, frame, "tool"), sample['actors'][0])
        sample['actors'][0]['pose_evaluation_serial'] = 9
        with self.assertRaisesRegex(ValueError, "pose mismatch"):
            linked_actor(sample, frame, "tool")


if __name__ == "__main__":
    unittest.main()
