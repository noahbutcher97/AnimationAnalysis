"""Raw performance records must not silently become stronger evidence."""
import importlib.util
from pathlib import Path
import unittest

_path = Path(__file__).resolve().parents[1] / 'summarize_readback_performance.py'


class PerformanceSummaryTests(unittest.TestCase):
    def load(self):
        self.assertTrue(_path.exists(), 'Performance evidence summarizer is missing')
        spec = importlib.util.spec_from_file_location('readback_performance', _path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_nearest_rank_percentiles_preserve_outlier(self):
        module = self.load()
        self.assertEqual(module.quantiles([1, 2, 3, 4, 100]), {'p50': 3, 'p95': 100, 'p99': 100})

    def test_missing_repetition_or_nonfinite_sample_is_incomplete(self):
        module = self.load()
        rows = [dict(mode='disabled', repetition=0, sample=i, frame_wall_s=.01,
                     acquisition_wall_s=0, completion_latency_s=0) for i in range(2)]
        with self.assertRaisesRegex(ValueError, 'repetition'):
            module.summarize(dict(samples=rows), repetitions=2, samples=2)
        rows[0]['frame_wall_s'] = float('nan')
        with self.assertRaisesRegex(ValueError, 'finite'):
            module.summarize(dict(samples=rows), repetitions=1, samples=2)

    def test_failed_attempt_does_not_become_zero_completion_latency(self):
        module = self.load()
        rows = [dict(mode='asynchronous', repetition=0, sample=0, terminal_status='completed',
                     frame_wall_s=.01, acquisition_wall_s=.001, completion_latency_s=.02),
                dict(mode='asynchronous', repetition=0, sample=1, terminal_status='failed',
                     frame_wall_s=.03, acquisition_wall_s=.001, completion_latency_s=None)]
        report = module.summarize(dict(samples=rows), repetitions=1, samples=2)
        self.assertFalse(report['comparison_eligible'])
        self.assertEqual(report['runs'][0]['outcomes'], {'completed': 1, 'failed': 1})
        self.assertEqual(report['runs'][0]['metrics']['completion_latency_ms']['p50'], 20)


if __name__ == '__main__':
    unittest.main()
