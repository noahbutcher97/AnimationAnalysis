"""Literal analytic controls for the exact surface kernel."""
from fractions import Fraction as F
import unittest

from animation_analysis._triangle_geometry import is_degenerate, triangle_distance_squared


def tri(*points):
    return tuple(tuple(F(v) for v in p) for p in points)


BASE = tri((0, 0, 0), (2, 0, 0), (0, 2, 0))


class TriangleGeometryTests(unittest.TestCase):
    def check_distance(self, a, b, expected):
        distance, first, second = triangle_distance_squared(a, b)
        self.assertEqual(distance, expected)
        self.assertTrue(all(isinstance(v, F) for p in (first, second) for v in p))
        self.assertEqual(sum((x-y)**2 for x, y in zip(first, second)), expected)
        self.assertEqual(triangle_distance_squared(b, a)[0], expected)
        self.assertEqual(triangle_distance_squared(a[::-1], b[::-1])[0], expected)

    def test_parallel_panels(self):
        self.check_distance(BASE, tri((0, 0, 3), (2, 0, 3), (0, 2, 3)), F(9))

    def test_interior_piercing(self):
        self.check_distance(BASE, tri((F(1, 2), F(1, 2), -2),
                                    (F(1, 2), F(1, 2), 2), (3, 3, 1)), F(0))

    def test_coplanar_inclusion(self):
        self.check_distance(BASE, tri((F(1, 4), F(1, 4), 0),
                                    (F(1, 2), F(1, 4), 0),
                                    (F(1, 4), F(1, 2), 0)), F(0))

    def test_coplanar_crossing_without_included_vertices(self):
        self.check_distance(tri((-2, -1, 0), (2, -1, 0), (0, 2, 0)),
                            tri((-2, 1, 0), (2, 1, 0), (0, -2, 0)), F(0))

    def test_shared_edge_and_point(self):
        self.check_distance(BASE, tri((0, 0, 0), (2, 0, 0), (1, -1, 1)), F(0))
        self.check_distance(BASE, tri((2, 0, 0), (3, 0, 1), (3, 1, 1)), F(0))

    def test_edge_interiors_are_closest(self):
        # Edges cross in XY but all of B has z >= 3, all of A has z <= 0.
        a = tri((-2, 0, 0), (2, 0, 0), (0, -2, -1))
        b = tri((0, -2, 3), (0, 2, 3), (2, 0, 4))
        self.check_distance(a, b, F(9))
        self.assertEqual(triangle_distance_squared(a, b)[1:],
                         ((F(0), F(0), F(0)), (F(0), F(0), F(3))))

    def test_projected_vertex_face(self):
        self.check_distance(BASE, tri((F(1, 2), F(1, 2), 3),
                                    (3, 3, 4), (4, 3, 4)), F(9))

    def test_tiny_gap_remains_nonzero(self):
        gap = F(1, 10**100)
        self.check_distance(BASE, tuple(tuple(v + (gap if i == 2 else 0)
                                             for i, v in enumerate(p)) for p in BASE), gap**2)

    def test_squared_distance_below_float_range(self):
        # Plane normal is (e**2, -e, 1); B's first point is e**3 off
        # that plane before dividing by the normal length. Other B vertices
        # lie farther on its positive side; projection lies inside A.
        e = F(1, 2**200)
        a = tri((0, 0, 0), (1, e, 0), (0, 1, e))
        b = tri((e, F(1, 2), e/2), (e, F(1, 2), 1), (e, 1, 1))
        expected = e**6 / (1 + e**2 + e**4)
        self.assertEqual(float(expected), 0)
        self.check_distance(a, b, expected)

    def test_coplanar_separation(self):
        self.check_distance(BASE, tri((3, 0, 0), (4, 0, 0), (3, 1, 0)), F(1))

    def test_degeneracy_is_explicit(self):
        self.assertFalse(is_degenerate(BASE))
        for a in (tri((0, 0, 0), (1, 1, 1), (2, 2, 2)),
                  tri((0, 0, 0), (0, 0, 0), (1, 2, 3))):
            self.assertTrue(is_degenerate(a))
            with self.assertRaises(ValueError):
                triangle_distance_squared(a, BASE)
            with self.assertRaises(ValueError):
                triangle_distance_squared(BASE, a)


if __name__ == '__main__':
    unittest.main()
