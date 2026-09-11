import base64
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from animation_analysis import load_evidence, render_review, validate_review
from animation_analysis.adapters.legacy_evidence import load_evidence as load_legacy, normalize_evidence
from animation_analysis.jobs.visual_review import publish


class PortableEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=")
        self.data = dict(schema_version=2, frames=[dict(file="observation", time=dict(domain="sensor", seconds=1.25),
                    image_sha256=hashlib.sha256(png).hexdigest(), image="data:image/png;base64,"+base64.b64encode(png).decode())])
        self.path = self.root / "evidence.html"
        self.save()

    def save(self):
        self.path.write_text('<script id="visual-evidence" type="application/json">'+json.dumps(self.data)+'</script>', encoding='utf-8')

    def review(self):
        return dict(schema_version=1, title="Mechanism inspection", evidence_sha256=hashlib.sha256(self.path.read_bytes()).hexdigest(),
                    reviewer=dict(kind="human", identity="Fixture"), summary="Visibility review", reviewed_frames=["observation"],
                    findings=[dict(id="framing", category="Framing", assessment="indeterminate", basis="pixels",
                    observation="One retained observation", interpretation="Motion cannot be assessed", limits="Single observation",
                    next_action="Record a sequence", evidence=["observation"])])

    def test_canonical_clock_and_publication_without_project_fields(self):
        data, digest = load_evidence(self.path)
        review = self.review()
        summary = validate_review(review, data, digest)
        self.assertIn('sensor 1.250s', render_review(review, data, summary, 'evidence.html'))
        findings = self.root / 'findings.json'
        findings.write_text(json.dumps(review))
        self.assertEqual(publish(self.path, findings, self.root/'review.html')['status'], 'review_recorded')
        self.assertFalse(list(self.root.rglob('*.png')))

    def test_legacy_clock_normalization_preserves_bytes_and_original_hash(self):
        for field in ('simulation_time_s', 'montage_time_s'):
            self.data['schema_version'] = 1
            self.data['frames'][0] = {k:v for k,v in self.data['frames'][0].items() if k not in ('time','simulation_time_s','montage_time_s')}
            self.data['frames'][0][field] = 2.5
            self.save()
            original = self.path.read_bytes()
            with self.assertRaisesRegex(ValueError, 'schema 2'):
                load_evidence(self.path)
            normalized, digest = load_legacy(self.path)
            self.assertEqual(normalized['frames'][0]['time'], dict(domain=field.removesuffix('_time_s'), seconds=2.5))
            self.assertEqual(digest, hashlib.sha256(original).hexdigest())
            self.assertEqual(self.path.read_bytes(), original)

    def test_missing_invalid_or_conflicting_clocks_reject(self):
        for clock in (None, dict(domain='sensor', seconds=True), dict(domain='sensor', seconds=float('nan'))):
            self.data['frames'][0]['time'] = clock
            self.save()
            with self.assertRaises(ValueError):
                load_evidence(self.path)
        legacy = copy.deepcopy(self.data)
        legacy['schema_version'] = 1
        legacy['frames'][0].update(simulation_time_s=1, time=dict(domain='simulation', seconds=2))
        with self.assertRaisesRegex(ValueError, 'disagree'):
            normalize_evidence(legacy)
        self.data['frames'][0]['time'] = dict(domain='sensor', seconds=1.25)
        self.data['schema_version'] = 2.0
        self.save()
        with self.assertRaisesRegex(ValueError, 'schema 2'):
            load_evidence(self.path)

    def test_normalization_does_not_mutate_source_and_unknown_versions_reject(self):
        legacy = copy.deepcopy(self.data)
        legacy['schema_version'] = 1
        legacy['frames'][0].pop('time')
        legacy['frames'][0]['simulation_time_s'] = 3
        normalized = normalize_evidence(legacy)
        self.assertNotIn('time', legacy['frames'][0])
        self.assertEqual(normalized['schema_version'], 2)
        for version in (100, True, '1'):
            legacy['schema_version'] = version
            with self.assertRaisesRegex(ValueError, 'schema'):
                normalize_evidence(legacy)


if __name__ == '__main__':
    unittest.main()
