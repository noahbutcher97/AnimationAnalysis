"""Search bounds and independent analytic mesh controls."""
from fractions import Fraction as F
from dataclasses import FrozenInstanceError
import unittest

from animation_analysis._mesh_search import SearchLimitError, nearest_triangles, _build


def panel(x=0, z=0):
    return tuple(tuple(F(v) for v in p) for p in ((x, 0, z), (x+1, 0, z), (x, 1, z)))


class MeshSearchTests(unittest.TestCase):
    def search(self, first, second, **limits):
        return nearest_triangles(first, second, max_pair_tests=limits.get('pairs', 10000),
                                 max_node_visits=limits.get('nodes', 10000))

    def test_literal_minimum_and_global_ids(self):
        result = self.search([(12, panel(20)), (77, panel())], [(8, panel(0, 3))])
        self.assertEqual(result.distance_squared, 9)
        self.assertEqual((result.first_triangle, result.second_triangle), (77, 8))
        self.assertEqual(sum((a-b)**2 for a, b in zip(result.first_point, result.second_point)), 9)
        with self.assertRaises(FrozenInstanceError):
            result.distance_squared = 10

    def test_zero_stops_without_exhausting_later_pairs(self):
        result = self.search([(i, panel(i*10)) for i in range(50)], [(90, panel())], pairs=1)
        self.assertEqual(result.distance_squared, 0)
        self.assertEqual(result.triangle_tests, 1)

    def test_prunes_separated_panels(self):
        result = self.search([(i, panel(i*10)) for i in range(64)], [(90, panel(0, 3))], pairs=1)
        self.assertEqual(result.distance_squared, 9)
        self.assertEqual(result.triangle_tests, 1)
        self.assertLess(result.node_visits, 64)

    def test_pair_exhaustion_has_counts_and_no_minimum(self):
        # Both boxes have zero lower bound; neither surface intersects B.
        a = ((F(0), F(0), F(0)), (F(2), F(0), F(0)), (F(0), F(2), F(0)))
        b = ((F(2), F(2), F(0)), (F(3), F(2), F(0)), (F(2), F(3), F(0)))
        with self.assertRaises(SearchLimitError) as raised:
            self.search([(1, a), (2, a)], [(3, b)], pairs=1)
        self.assertEqual(raised.exception.triangle_tests, 1)
        self.assertGreater(raised.exception.node_visits, 0)
        self.assertFalse(hasattr(raised.exception, 'distance_squared'))

    def test_node_exhaustion(self):
        with self.assertRaises(SearchLimitError) as raised:
            self.search([(1, panel()), (2, panel(10))], [(3, panel(0, 3))], nodes=1)
        self.assertEqual(raised.exception.node_visits, 1)
        self.assertEqual(raised.exception.triangle_tests, 0)

    def test_deterministic_order(self):
        first = [(5, panel()), (2, panel())]
        self.assertEqual(self.search(first, [(7, panel(0, 1))]),
                         self.search(first[::-1], [(7, panel(0, 1))]))

    def test_linear_node_count_and_leaf_only_geometry(self):
        for count in (1, 2, 3, 31, 64):
            pending = [_build([(i, panel(i*2)) for i in range(count)])]
            visited = leaves = 0
            while pending:
                node = pending.pop()
                visited += 1
                if node.triangle is not None:
                    leaves += 1
                    self.assertIsNone(node.left)
                    self.assertIsNone(node.right)
                else:
                    self.assertIsNone(node.triangle_id)
                    pending.extend((node.left, node.right))
            self.assertEqual(leaves, count)
            self.assertEqual(visited, 2*count-1)

    def test_exact_one_pair_budgets_succeed(self):
        result = self.search([(1, panel())], [(2, panel(0, 3))], pairs=1, nodes=1)
        self.assertEqual((result.distance_squared, result.triangle_tests, result.node_visits), (9, 1, 1))

    def test_tiny_aabb_gap_stays_exact(self):
        gap = F(1, 2**200)
        result = self.search([(1, panel())], [(2, panel(0, gap)), (3, panel(0, gap*2))])
        self.assertEqual(result.distance_squared, gap**2)
        self.assertEqual(result.second_triangle, 2)

    def test_invalid_inputs(self):
        for value in (0, -1, True, 1.5):
            with self.assertRaises(ValueError):
                self.search([(1, panel())], [(2, panel())], pairs=value)
            with self.assertRaises(ValueError):
                self.search([(1, panel())], [(2, panel())], nodes=value)
        with self.assertRaises(ValueError):
            self.search([], [(2, panel())])


if __name__ == '__main__':
    unittest.main()
