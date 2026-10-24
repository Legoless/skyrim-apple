#!/usr/bin/env python3
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sky_extract_xci as sky

FAKE = r'''
import json, os, struct, sys
from pathlib import Path
args = sys.argv[1:]
assert '-k' in args and '--suppresskeys' in args and '--disablekeywarns' in args
assert '--basenca' not in ' '.join(args)
config = json.loads(Path(os.environ['FAKE_CONFIG']).read_text())
with open(os.environ['FAKE_LOG'], 'a') as log: log.write(json.dumps(args) + '\n')
def option(prefix):
    return Path(next(a[len(prefix):] for a in args if a.startswith(prefix)))
if '-t' in args and args[args.index('-t')+1] == 'xci':
    out = option('--securedir=')
    for index, spec in enumerate(config['ncas']):
        (out / (str(index)+'.nca')).write_text(json.dumps(spec))
elif '-i' in args:
    spec = json.loads(Path(args[-1]).read_text())
    if config.get('invalid_header'):
        print('Invalid NCA header! PRIVATE-KEY-MATERIAL', file=sys.stderr)
    else:
        print('Magic: NCA3\nTitle ID: '+spec['title']+'\nContent Type: '+spec['type'])
        print('        Title ID: ffffffffffffffff')
        if spec.get('bktr'): print('Partition Type: Patch RomFS\nBKTR')
elif any(a.startswith('--section0dir=') for a in args):
    header = struct.pack('<QI', int(config.get('cnmt_title', '01000a10041ea000'),16), config.get('version',0))
    header += bytes([config.get('cnmt_type',0x80)]) + bytes(19)
    (option('--section0dir=') / 'base.cnmt').write_bytes(header)
elif not any(a.startswith('--exefsdir=') for a in args):
    control = option('--romfsdir='); control.mkdir(parents=True)
    nacp = bytearray(0x4000)
    capacities = config.get('capacities', [262144,65536,0,0])
    struct.pack_into('<qq', nacp, 0x3080, *capacities[:2])
    struct.pack_into('<qq', nacp, 0x3148, *capacities[2:])
    if config.get('short_nacp'): nacp = nacp[:0x3157]
    if not config.get('missing_nacp'): (control/'control.nacp').write_bytes(nacp)
else:
    exefs, romfs = option('--exefsdir='), option('--romfsdir=')
    exefs.mkdir(parents=True); (romfs/'Data').mkdir(parents=True)
    for name, byte in [('main',b'm'),('sdk',b's')]:
        data = bytearray(256); data[:4] = b'NSO0'; data[0x40:0x60] = byte*32
        (exefs/name).write_bytes(data)
    (romfs/'Skyrim.ini').write_bytes(b'[General]')
    (romfs/'Data/base.bsa').write_bytes(b'base cartridge assets')
    if config.get('bad_nso'): (exefs/'main').write_bytes(b'bad')
    if config.get('failure'):
        print('PRIVATE-KEY-MATERIAL', file=sys.stderr)
        sys.exit(1)
'''


class ExtractXciTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.work = self.root / "work with spaces"
        self.source = self.root / "owned cartridge.xci"
        self.source.write_bytes(bytes(256) + b"HEAD" + bytes(1024))
        self.keys = self.root / "console keys.txt"
        self.keys.write_text("private-key-value")
        self.config_path = self.root / "config.json"
        self.log = self.root / "calls.jsonl"
        self.tool = self.root / "fake hactool"
        self.tool.write_text("#!" + sys.executable + "\n" + FAKE)
        self.tool.chmod(0o755)
        self.env = dict(os.environ, HACTOOL=str(self.tool), FAKE_CONFIG=str(self.config_path), FAKE_LOG=str(self.log))
        self.config = {"ncas": [{"title": sky.TITLE_ID, "type": kind} for kind in ("Program", "Meta", "Control")]}
        self.configure()

    def configure(self, **changes):
        self.config.update(changes)
        self.config_path.write_text(json.dumps(self.config))

    def extract(self):
        return sky.extract_xci(self.source, self.keys, self.work, self.env)

    def test_extracts_base_only_and_records_actual_build_ids_without_keys(self):
        game, metadata = self.extract()
        self.assertEqual(game, self.work / "data/switch/skyrim/base/Update")
        state = json.loads((metadata / "extraction.json").read_text())
        self.assertEqual(state['content_version'], 0)
        self.assertEqual(state['modules']['main']['build_id'], (b'm'*32).hex())
        self.assertIn('Control/control.nacp', state['files'])
        self.assertEqual(sky._control_capacities(game/'Control/control.nacp'), (262144,65536,262144,65536))
        with (metadata / "packages.tsv").open() as stream:
            rows = list(csv.DictReader(stream, delimiter='\t'))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['role'], 'base')
        self.assertEqual(rows[0]['source_package'], str(self.source))
        self.assertEqual(rows[0]['package_sha256'], hashlib.sha256(self.source.read_bytes()).hexdigest())
        combined = ''.join(path.read_text() for path in metadata.iterdir())
        self.assertNotIn('private-key-value', combined)
        self.assertNotIn(str(self.keys), combined)

    def test_accepts_original_nested_split_dump_with_spaces(self):
        original = self.source.read_bytes()
        self.source.unlink(); self.source.mkdir()
        nested = self.source / self.source.name; nested.mkdir()
        (nested/'00').write_bytes(original[:700]); (nested/'01').write_bytes(original[700:])
        (self.source/'certificate.bin').write_bytes(b'certificate')
        (self.source/'initialData.bin').write_bytes(b'initial data')
        _, metadata = self.extract()
        self.assertIn(str(self.source), (metadata/'packages.tsv').read_text())
        self.assertEqual((self.work/'build/cache/skyrim/input.xci').read_bytes(), original)

    def test_same_input_reuses_only_unchanged_extracted_hashes(self):
        game, metadata = self.extract()
        calls = self.log.read_bytes()
        self.assertEqual(self.extract(), (game, metadata))
        self.assertEqual(self.log.read_bytes(), calls)
        archive = game/'RomFS/Data/base.bsa'
        archive.write_bytes(b'x'*archive.stat().st_size)
        with self.assertRaisesRegex(ValueError, 'files have changed'):
            self.extract()
        self.assertEqual(self.log.read_bytes(), calls)

    def test_changed_xci_or_partial_destination_is_preserved_and_refused(self):
        game, _ = self.extract()
        expected = (game/'ExeFS/main').read_bytes()
        self.source.write_bytes(self.source.read_bytes()+b'changed')
        with self.assertRaisesRegex(ValueError, 'another XCI'):
            self.extract()
        self.assertEqual((game/'ExeFS/main').read_bytes(), expected)
        (game.parent/'meta/extraction.json').unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete or mismatched'):
            self.extract()
        self.assertEqual((game/'ExeFS/main').read_bytes(), expected)

    def test_rejects_wrong_title_ambiguous_or_bktr_programs(self):
        scenarios = [
            [{'title':'ffffffffffffffff','type':'Program'}, self.config['ncas'][1]],
            self.config['ncas'] + [self.config['ncas'][0]],
            [dict(self.config['ncas'][0],bktr=True), self.config['ncas'][1]],
        ]
        for index, ncas in enumerate(scenarios):
            with self.subTest(index=index):
                self.configure(ncas=ncas)
                with self.assertRaisesRegex(ValueError, 'exactly one|BKTR'):
                    self.extract()
                self.assertFalse((self.work/'data/switch/skyrim/base').exists())

    def test_missing_wrong_title_or_ambiguous_control_fails_before_publish(self):
        original = self.config['ncas'][:]
        for ncas in (original[:2], original[:2] + [dict(original[2], title='ffffffffffffffff')],
                     original + [original[2]]):
            with self.subTest(ncas=ncas):
                self.configure(ncas=ncas)
                with self.assertRaisesRegex(ValueError, 'exactly one Skyrim Control'):
                    self.extract()
                self.assertFalse((self.work/'data/switch/skyrim/base').exists())

    def test_invalid_control_save_capacities_fail_before_publish(self):
        for changes in ({'short_nacp':True}, {'missing_nacp':True}, {'capacities':[0,0,0,0]},
                        {'capacities':[1,-1,0,0]}, {'capacities':[1,0,-1,0]},
                        {'capacities':[2,0,1,0]}):
            with self.subTest(changes=changes):
                self.configure(short_nacp=False, missing_nacp=False, capacities=[262144,65536,0,0])
                self.configure(**changes)
                with self.assertRaisesRegex(ValueError, 'control.nacp'):
                    self.extract()
                self.assertFalse((self.work/'data/switch/skyrim/base').exists())

    def test_zero_exit_invalid_nca_header_is_rejected(self):
        self.configure(invalid_header=True)
        with self.assertRaisesRegex(ValueError, 'Invalid NCA metadata') as caught:
            self.extract()
        self.assertNotIn('PRIVATE-KEY-MATERIAL', str(caught.exception))
        self.assertFalse((self.work/'data/switch/skyrim/base').exists())

    def test_rejects_wrong_cnmt_title_version_or_type(self):
        for changes in [{'version':327680}, {'cnmt_title':'ffffffffffffffff'}, {'cnmt_type':0x81}]:
            with self.subTest(changes=changes):
                self.configure(version=0, cnmt_title=sky.TITLE_ID, cnmt_type=0x80)
                self.configure(**changes)
                with self.assertRaisesRegex(ValueError, 'content version 0|CNMT title/type'):
                    self.extract()
                self.assertFalse((self.work/'data/switch/skyrim/base').exists())

    def test_failed_or_interrupted_extraction_preserves_unrelated_state(self):
        save = self.work/'run/save/keep.sav'; save.parent.mkdir(parents=True); save.write_bytes(b'keep')
        self.configure(failure=True)
        with self.assertRaisesRegex(ValueError, 'Base game extraction failed'):
            self.extract()
        self.configure(failure=False)
        actual = sky._hactool
        def interrupted(env, keys, description, *args):
            result = actual(env, keys, description, *args)
            if description == 'Base game extraction': raise KeyboardInterrupt()
            return result
        with patch.object(sky, '_hactool', side_effect=interrupted):
            with self.assertRaises(KeyboardInterrupt): self.extract()
        self.assertEqual(save.read_bytes(), b'keep')
        self.assertFalse((self.work/'data/switch/skyrim/base').exists())
        self.assertFalse(list((self.work/'data/switch/skyrim').glob('.base-extract-*')))

    def test_invalid_nso_missing_keys_and_overlapping_workspace_fail(self):
        self.configure(bad_nso=True)
        with self.assertRaisesRegex(ValueError, 'Invalid extracted NSO'): self.extract()
        self.keys.unlink()
        with self.assertRaisesRegex(ValueError, 'prod.keys'): self.extract()
        self.source.unlink(); self.source.mkdir()
        with self.assertRaisesRegex(ValueError, 'outside the original split dump'):
            sky.extract_xci(self.source,self.keys,self.source/'generated',self.env)


if __name__ == '__main__': unittest.main()
