"""Regression checks for variant provenance and incomplete capture evidence."""
import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from write_build_manifest import check_validation
from prepare_quality_experiments import division_group, variants
from quality_capture_matrix import matrix, inventory

class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.directory=ROOT/'src/addons/mfgunlock'
        self.report={'metadata':{'variant':'control','generated_table_sha256':'table',
            'warp_source_sha256':hashlib.sha256((self.directory/'adaptive_quality_v3.hpp').read_bytes()).hexdigest(),
            'rewrite_source_sha256':hashlib.sha256((self.directory/'thin_geometry.hpp').read_bytes()).hexdigest()}}
    def test_provenance_rejects_stale_or_mislabeled_report(self):
        check_validation(self.report,'control',self.directory,'table')
        for field in ['variant','generated_table_sha256','warp_source_sha256','rewrite_source_sha256']:
            wrong=copy.deepcopy(self.report)
            wrong['metadata'][field]='wrong'
            with self.assertRaises(ValueError): check_validation(wrong,'control',self.directory,'table')
    def test_division_experiments_require_unique_constant_and_sites(self):
        text='mov.f32 %f528, 0f40800000;\n'+'div.approx.ftz.f32 %f520, %f44, %f528;\n'*8
        for wrong in [text.replace('0f40800000','0f40400000'),text+text,text.replace('div.approx','div.rn',1)]:
            with self.assertRaises(ValueError): division_group(wrong,'f528','0f40800000','0f3E800000',8)
        with self.assertRaises(ValueError): variants('// MFGUNLOCK_AXIS_BLEND_CANDIDATE')
    def test_runs_repeat_and_never_certify_missing_capture(self):
        plan=matrix()
        self.assertEqual(len(plan['jobs']),42)
        self.assertEqual(len({j['id'] for j in plan['jobs']}),42)
        groups={}
        for job in plan['jobs']:
            key=(job['game'],job['requested_mode'],job['color'],job['variant'])
            groups.setdefault(key,[]).append(job['repetition'])
        self.assertTrue(all(sorted(r)==[1,2,3] for r in groups.values()))
        with tempfile.TemporaryDirectory() as temp:
            result=inventory(plan,Path(temp))
            self.assertEqual(result['promotion'],'not evaluated')
            self.assertTrue(all(not e['summaries'] and e['status'].startswith('pending') for e in result['evidence']))
            d=Path(temp)/plan['jobs'][0]['id'];d.mkdir()
            (d/'capture-metadata.json').write_text('{}')
            (d/'test.summary.json').write_text('{}')
            result=inventory(plan,Path(temp))
            self.assertEqual(result['promotion'],'not evaluated')
            self.assertIn('review pending',result['evidence'][0]['status'])
    def test_extended_matrix_separates_fixed_and_dynamic(self):
        plan=matrix('all')
        self.assertEqual(len(plan['jobs']),288)
        self.assertTrue(any(j['game']=='witcher3' and j['requested_mode']=='dynamic' for j in plan['jobs']))

if __name__=='__main__': unittest.main()
