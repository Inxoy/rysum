"""Validate the draft contract and the checked-in evidence, without approving it."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import unittest
import wave

from jsonschema import Draft202012Validator, FormatChecker, ValidationError

ROOT = Path(__file__).resolve().parents[1]


class PitchMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads((ROOT / 'docs/pitch-metadata-v1.schema.json').read_text())
        cls.example = json.loads((ROOT / 'docs/pitch-metadata-v1.example.json').read_text())
        cls.validator = Draft202012Validator(cls.schema, format_checker=FormatChecker())

    def test_contract_and_example(self):
        Draft202012Validator.check_schema(self.schema)
        self.validator.validate(self.example)

    def test_bad_frequency_or_version_is_rejected(self):
        for field in ('frequency', 'version'):
            value = deepcopy(self.example)
            if field == 'frequency':
                value['assets'][0]['pitch']['measurement']['frequency_hz'] = 0
            else:
                value['schema_version'] = 2
            with self.subTest(field=field), self.assertRaises(ValidationError):
                self.validator.validate(value)

    def test_approval_cannot_be_implicit(self):
        value = deepcopy(self.example)
        value['assets'][0]['pitch']['measurement']['status'] = 'approved'
        with self.assertRaises(ValidationError):
            self.validator.validate(value)
        value['assets'][0]['pitch']['measurement']['approval'] = {
            'reviewer': 'schema test only', 'date': '2026-11-07', 'notes': 'Not production approval.'}
        self.validator.validate(value)

    def test_audit_covers_every_manifest_pipe_and_matches_final_wavs(self):
        audit = json.loads((ROOT / 'docs/pitch-analysis/final-wav-pitch-analysis.json').read_text())
        manifest_path = ROOT / 'samples/rysum_samples.json'
        manifest = json.loads(manifest_path.read_text())
        expected = {(s['id'], p['key'], p['file']) for s in manifest['stops'] for p in s['pipes']}
        actual = {(p['stop_id'], p['key'], p['file']) for p in audit['samples']}
        self.assertEqual(expected, actual)
        self.assertEqual(len(audit['samples']), len(actual))
        self.assertEqual(audit['manifest_sha256'], hashlib.sha256(manifest_path.read_bytes()).hexdigest())
        self.assertEqual(audit['analysis_script_sha256'], hashlib.sha256((ROOT / 'scripts/analyse_sample_pitch.py').read_bytes()).hexdigest())
        for sample in audit['samples']:
            with self.subTest(file=sample['file']):
                path = ROOT / 'samples' / sample['file']
                self.assertEqual(sample['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
                with wave.open(str(path)) as wav:
                    self.assertEqual(sample['sample_rate_hz'], wav.getframerate())
                    self.assertEqual(sample['frames'], wav.getnframes())
                for region in sample['windows']:
                    self.assertTrue(0 <= region['start_frame'] < region['end_frame'] <= sample['frames'])
                self.assertIn(sample['status'], ('candidate', 'review', 'compound_unresolved'))
                if sample['status'] == 'compound_unresolved':
                    self.assertNotIn('measured_hz', sample)

    def test_example_assets_and_derived_mapping_share_raw_source(self):
        assets = {a['id']: a for a in self.example['assets']}
        for pipe in self.example['pipes']:
            self.assertIn(pipe['asset_id'], assets)
        own = next(p for p in self.example['pipes'] if p['stop_id'] == 'praestant')
        derived = next(p for p in self.example['pipes'] if p['stop_id'] == 'praestant16')
        self.assertEqual(own['asset_id'], derived['asset_id'])
        self.assertEqual(derived['source_playback_ratio'], 0.5)
        self.assertEqual(derived['target_reference_ratio'], {'numerator': 1, 'denominator': 2})


if __name__ == '__main__':
    unittest.main()
