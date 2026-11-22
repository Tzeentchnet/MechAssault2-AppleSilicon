"""Synthetic tests for non-Windows disc validation and generation stages."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'toolkit'))

import install  # noqa: E402
import pc_assets  # noqa: E402
from tools.xiso.test_xdvdfs import _build_image  # noqa: E402


def digest(data):
    return hashlib.sha256(data).hexdigest()


class DiscValidationStages(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.here = self.root / 'installer'
        (self.here / 'data').mkdir(parents=True)
        self.manifest_path = self.here / 'data' / 'disc-manifest.json'
        self.xbe = b'XBEH-synthetic executable'
        self.asset = b'payload'
        self.release = {'xbe': 'ma2.xbe', 'xbe_bytes': len(self.xbe), 'xbe_sha256': digest(self.xbe),
                        'release_executable_sha256': 'synthetic'}
        self.manifest = {'ma2.xbe': [len(self.xbe), digest(self.xbe)],
                         'media/asset.bin': [len(self.asset), digest(self.asset)]}
        self.write_manifest()
        self.image = self.root / 'input.iso'
        self.write_image()
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(install, 'HERE', self.here))
        self.stack.enter_context(mock.patch.object(install, 'RELEASE', self.release))
        self.stack.enter_context(mock.patch.object(install.sys, 'platform', 'darwin'))
        self.stack.enter_context(mock.patch.object(install.precheck, 'local_dir', return_value=self.root / 'profile'))
        self.stack.enter_context(mock.patch.object(install, 'find_visual_studio', side_effect=AssertionError('Windows compiler checked')))
        self.stack.enter_context(mock.patch.object(install, 'enhancer_python', side_effect=AssertionError('enhancer checked')))
        self.stack.enter_context(mock.patch.object(install.precheck, 'smart_app_control', side_effect=AssertionError('Windows policy checked')))
        self.stack.enter_context(mock.patch.object(install, 'clean_stale_build_folders', side_effect=AssertionError('build cleanup ran')))

    def write_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding='utf-8')

    def write_image(self, asset=None):
        self.image.write_bytes(_build_image(files=(('ma2.xbe', self.xbe),),
                                            subdir=('media', (('asset.bin', asset if asset is not None else self.asset),))))

    def installer(self, until='extract'):
        inst = install.Installer(self.image, self.root / 'dest', textures=('2x',), videos=True,
                                 keep_awake=False, log=lambda line: None)
        inst.until = until
        self.stack.enter_context(mock.patch.object(inst, 'ffmpeg_dir', side_effect=AssertionError('FFmpeg checked')))
        self.stack.enter_context(mock.patch.object(inst, 'remember_install', side_effect=AssertionError('install remembered')))
        return inst

    def test_extract_runs_quick_and_manifest_checks_without_build_prerequisites(self):
        inst = self.installer()
        inst.run()
        self.assertEqual(inst.state['done'], ['disc', 'extract'])
        self.assertEqual((inst.dest / 'game_files' / 'ma2.xbe').read_bytes(), self.xbe)
        self.assertEqual((inst.dest / 'game_files' / 'media' / 'asset.bin').read_bytes(), self.asset)
        self.assertFalse((inst.work / 'toolkit' / install.PROJECT).exists())

    def test_extract_rejects_same_size_corruption(self):
        self.write_image(asset=b'changed')
        inst = self.installer()
        with self.assertRaises(install.Stop) as result:
            inst.run()
        self.assertEqual(result.exception.kind, 'disc')
        self.assertEqual(inst.state['done'], ['disc'])

    def test_quick_check_does_not_need_a_manifest(self):
        self.manifest_path.unlink()
        inst = self.installer('disc')
        inst.run()
        self.assertEqual(inst.state['done'], ['disc'])
        self.assertFalse((inst.dest / 'game_files').exists())

    def test_extract_requires_the_full_valid_manifest_and_enough_space(self):
        inst = self.installer()
        inst.dest.mkdir()
        wrong_executable = dict(self.manifest)
        wrong_executable['ma2.xbe'] = [len(self.xbe), digest(b'other executable')]
        for contents in (None, '{}', '{"ma2.xbe": [1, "invalid"]}', json.dumps(wrong_executable)):
            with self.subTest(contents=contents):
                if contents is None:
                    self.manifest_path.unlink(missing_ok=True)
                else:
                    self.manifest_path.write_text(contents, encoding='utf-8')
                with self.assertRaises(install.Stop) as result:
                    inst.check()
                self.assertEqual(result.exception.kind, 'disc')
                self.assertIn('manifest', str(result.exception))
        self.write_manifest()
        with mock.patch.object(install.shutil, 'disk_usage', return_value=SimpleNamespace(free=0)):
            with self.assertRaises(install.Stop) as result:
                inst.check()
        self.assertEqual(result.exception.kind, 'disk_full')

    def test_validation_requires_a_fresh_destination(self):
        inst = self.installer()
        inst.dest.mkdir()
        for state in ({'done': ['disc']}, {'update': True, 'done': []}, {'addons_only': True, 'done': []}):
            with self.subTest(state=state):
                inst.state = state
                with self.assertRaises(install.Stop) as result:
                    inst.check()
                self.assertEqual(result.exception.kind, 'disc')
                self.assertIn('fresh', str(result.exception))

    def test_generation_defers_windows_prerequisites_but_build_stages_fail_explicitly(self):
        inst = self.installer('analyze')
        inst.dest.mkdir()
        inst.check()
        self.manifest_path.unlink()
        with self.assertRaises(install.Stop) as result: inst.check()
        self.assertEqual(result.exception.kind, 'disc')
        self.write_manifest()
        with mock.patch.dict(sys.modules, {'capstone': None}):
            with self.assertRaises(install.Stop) as result: inst.check()
        self.assertIn('Capstone', str(result.exception))
        for stage in ('build', 'assets', 'install', 'finish', None):
            inst.until = stage
            with self.subTest(stage=stage), self.assertRaises(install.Stop) as result: inst.check()
            self.assertEqual(result.exception.kind, 'compiler')
            self.assertIn('not supported', str(result.exception))
        inst.until = 'extract'
        with mock.patch.object(install.sys, 'platform', 'win32'), \
                mock.patch.object(install, 'find_visual_studio', side_effect=install.Stop('compiler required', 'compiler')) as find:
            with self.assertRaises(install.Stop) as result:
                inst.check()
        find.assert_called_once()
        self.assertEqual(str(result.exception), 'compiler required')

    def test_unsupported_build_stops_without_windows_side_effects(self):
        inst = self.installer('build')
        with mock.patch.object(install, 'clean_stale_build_folders') as cleanup, \
                mock.patch.object(inst, 'remember_install') as remember:
            with self.assertRaises(install.Stop) as result: inst.run()
        self.assertIn('not supported', str(result.exception))
        cleanup.assert_not_called()
        remember.assert_not_called()
        with self.assertRaises(install.Stop): inst.build()
        with self.assertRaises(install.Stop): inst.install()

    def test_generation_resume_and_checked_assets_do_not_run_the_build(self):
        self.stack.enter_context(mock.patch.object(install.Installer, 'analysis_outputs_exist', return_value=True))
        self.stack.enter_context(mock.patch.object(install.Installer, 'generation_outputs_valid', return_value=True))
        self.stack.enter_context(mock.patch.object(install.Installer, 'asset_outputs_valid', return_value=True))
        ran = []
        inst = self.installer('generate')
        inst.analyze = lambda: ran.append('analyze')
        inst.recompile = lambda: ran.append('recompile')
        inst.generate = lambda: ran.append('generate')
        inst.build = lambda: self.fail('build reached')
        inst.run()
        self.assertEqual(ran, ['analyze', 'recompile', 'generate'])
        self.assertEqual(inst.state['done'], ['disc', 'extract', 'analyze', 'recompile', 'generate'])

        resumed = self.installer('patch')
        resumed.analyze = resumed.recompile = resumed.generate = lambda: self.fail('finished stage reran')
        resumed.patch = lambda: ran.append('patch')
        resumed.build = lambda: self.fail('build reached')
        resumed.run()
        self.assertEqual(ran[-1], 'patch')
        self.assertEqual(resumed.state['done'][-1], 'patch')

        repeat = self.installer('patch')
        repeat.patch = repeat.build = lambda: self.fail('completed patch or build reran')
        repeat.run()
        self.assertEqual(repeat.stage, 'patch')

        (inst.dest / 'game_files' / 'ma2.xbe').write_bytes(b'X' * len(self.xbe))
        stale = self.installer('patch')
        stale.analyze = lambda: ran.append('reanalyze')
        stale.recompile = lambda: ran.append('rerecompile')
        stale.generate = lambda: ran.append('regenerate')
        stale.patch = lambda: ran.append('repatch')
        stale.build = lambda: self.fail('build reached')
        stale.run()
        self.assertEqual((inst.dest / 'game_files' / 'ma2.xbe').read_bytes(), self.xbe)
        self.assertEqual(ran[-4:], ['reanalyze', 'rerecompile', 'regenerate', 'repatch'])
        self.assertIn('extract', stale.state['done'])

        assets = self.installer('assets')
        assets.pc_assets_only = True
        assets.assets = lambda: (assets.dest / 'pc_assets').mkdir()
        assets.build = lambda: self.fail('build reached')
        extracted_asset = assets.dest / 'game_files' / 'media' / 'asset.bin'
        extracted_asset.write_bytes(b'changed')
        with self.assertRaises(install.Stop) as result: assets.run()
        self.assertEqual(result.exception.kind, 'disc')
        self.assertFalse((assets.dest / 'pc_assets').exists())
        extracted_asset.write_bytes(self.asset)
        assets.run()
        self.assertIn('assets', assets.state['done'])
        self.assertNotIn('build', assets.state['done'])
        assets.assets = lambda: self.fail('completed assets reran')
        assets.run()
        self.assertEqual(assets.stage, 'assets')
        repeat_after_assets = self.installer('patch')
        repeat_after_assets.patch = repeat_after_assets.build = lambda: self.fail('completed patch or build reran')
        repeat_after_assets.run()
        self.assertEqual(json.loads((repeat_after_assets.work / 'state.json').read_text(encoding='utf-8'))['last_step'], 'patch')

    def test_pc_assets_only_requires_completed_generation(self):
        inst = self.installer('assets')
        inst.pc_assets_only = True
        inst.dest.mkdir()
        inst.state = {'done': ['extract']}
        with self.assertRaises(install.Stop) as result: inst.check()
        self.assertIn('--until patch', str(result.exception))

    def test_asset_mode_rejects_conflicting_or_destructive_cli_actions(self):
        destination = self.root / 'command-destination'
        destination.mkdir()
        marker = destination / 'keep'
        marker.write_bytes(b'keep')
        for extra in (('--uninstall', '--yes'), ('--diagnostics',), ('--until', 'patch'),
                      ('--update',), ('--textures', '2x'), ('--videos',)):
            with self.subTest(extra=extra):
                result = subprocess.run([sys.executable, '-B', str(HERE / 'install.py'),
                                         '--dest', str(destination), '--pc-assets-only', *extra],
                                        stdin=subprocess.DEVNULL, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(marker.read_bytes(), b'keep')

    def test_helper_outputs_use_windows_bytes_without_touching_other_files(self):
        inst = self.installer('generate')
        gen = inst.project() / 'src' / 'recomp' / 'gen'
        gen.mkdir(parents=True)
        (gen / 'unrelated.c').write_bytes(b'unrelated\n')
        (gen / 'recomp_bringup_old.c').write_bytes(b'old')
        kit = self.root / 'kit'
        (kit / 'source' / 'game' / 'bringup').mkdir(parents=True)
        def fake_tool(args, **kwargs):
            bring = inst.project() / 'bringup'
            if kwargs['title'] == 'recover_callbacks':
                (gen / 'recomp_bringup.c').write_bytes(b'first\nsecond\r\n')
                (gen / 'recomp_bringup.h').write_bytes('caf\u00e9\n'.encode('utf-8'))
                (bring / 'callbacks.json').write_bytes(b'{}\n')
            else:
                (gen / 'recomp_fastpaths.c').write_bytes(b'last\n')
                (bring / 'fastpaths.json').write_bytes(b'[]\n')
                (bring / 'regression' / 'reference.c').write_bytes(b'reference\n')
        inst.run_cmd = fake_tool
        with mock.patch.object(install, 'KIT', kit): inst.generate()
        self.assertFalse((gen / 'recomp_bringup_old.c').exists())
        self.assertEqual((gen / 'recomp_bringup.c').read_bytes(), b'first\r\nsecond\r\n')
        self.assertEqual((gen / 'recomp_bringup.h').read_bytes(), 'caf\u00e9\r\n'.encode('utf-8'))
        self.assertEqual((gen / 'recomp_fastpaths.c').read_bytes(), b'last\r\n')
        self.assertEqual((gen / 'unrelated.c').read_bytes(), b'unrelated\n')
        self.assertEqual((inst.project() / 'bringup' / 'callbacks.json').read_bytes(), b'{}\r\n')
        self.assertEqual((inst.project() / 'bringup' / 'fastpaths.json').read_bytes(), b'[]\r\n')
        self.assertEqual((inst.project() / 'bringup' / 'regression' / 'reference.c').read_bytes(), b'reference\r\n')

    def test_fastcode_uses_utf8_and_rejects_wrong_final_bytes(self):
        inst = self.installer('patch')
        kit = self.root / 'kit'
        fc = kit / 'patches' / 'fastcode'
        fc.mkdir(parents=True)
        gen = self.root / 'gen'
        gen.mkdir()
        (gen / 'input.c').write_bytes(b'one\r\ntwo\r\n')
        output = 'caf\u00e9\n'.encode('utf-8')
        hashes = {'input.c': digest((gen / 'input.c').read_bytes())}
        (kit / 'patches' / 'manifest.json').write_text(
            json.dumps({'format': 'edits-2', 'base': hashes, 'final': hashes, 'patched': [], 'added': []}),
            encoding='utf-8')
        spec = {'lf': ['input.c'], 'whole': [], 'hot': 'hot.txt', 'final': {'output.c': digest(output)}}
        (fc / 'fastcode.json').write_text(json.dumps(spec), encoding='utf-8')
        calls = []
        def fake_tool(args, **kwargs):
            calls.append(args)
            self.assertEqual((gen / 'input.c').read_bytes(), b'one\ntwo\n')
            (gen / 'output.c').write_bytes(output)
        inst.run_cmd = fake_tool
        with mock.patch.object(install, 'KIT', kit):
            (fc / 'fastcode.json').write_text(json.dumps({**spec, 'final': {}}), encoding='utf-8')
            with self.assertRaises(install.Stop): inst.fastcode(gen)
            self.assertEqual((gen / 'input.c').read_bytes(), b'one\r\ntwo\r\n')
            (fc / 'fastcode.json').write_text(json.dumps(spec), encoding='utf-8')
            inst.fastcode(gen)
            self.assertEqual(calls[0][1:4], ['-X', 'utf8', '-I'])
            inst.run_cmd = lambda *a, **k: (gen / 'output.c').write_bytes(b'wrong')
            with self.assertRaises(install.Stop) as result: inst.fastcode(gen)
            (fc / 'fastcode.json').unlink()
            with self.assertRaises(install.Stop) as missing: inst.fastcode(gen)
        self.assertIn('do not match', str(result.exception))
        self.assertIn('manifest is missing', str(missing.exception))

    def test_stale_generation_verifies_manifests_before_skipping(self):
        inst = self.installer('patch')
        (inst.dest / 'game_files').mkdir(parents=True)
        (inst.dest / 'game_files' / 'ma2.xbe').write_bytes(self.xbe)
        (inst.dest / 'pc_assets').mkdir()
        gen = inst.project() / 'src' / 'recomp' / 'gen'
        gen.mkdir(parents=True)
        kit = self.root / 'kit'
        patches = kit / 'patches'
        post = patches / 'fastcode' / 'post'
        post.mkdir(parents=True)
        base = {'a.c': b'base\r\n', 'b.h': b'header\r\n', 'recomp_bringup.h': b'helper\r\n'}
        final = {**base, 'a.c': b'edit\r\n'}
        fast = b'fast copy\n'
        main_manifest = {'format': 'edits-2', 'base': {n: digest(b) for n, b in base.items()},
                         'final': {n: digest(b) for n, b in final.items()},
                         'patched': ['a.c'], 'added': [], 'removed': []}
        (patches / 'manifest.json').write_text(json.dumps(main_manifest), encoding='utf-8')
        spec = {'lf': ['a.c'], 'whole': [], 'hot': 'hot.txt',
                'final': {'fast.c': digest(fast)}, 'post': 'post'}
        (patches / 'fastcode' / 'fastcode.json').write_text(json.dumps(spec), encoding='utf-8')
        post_manifest = {'format': 'edits-2', 'base': {'fast.c': digest(b'before')},
                         'final': {'fast.c': digest(fast)}, 'patched': ['fast.c'], 'added': []}
        (post / 'manifest.json').write_text(json.dumps(post_manifest), encoding='utf-8')
        for n, data in final.items():
            (gen / n).write_bytes(data.replace(b'\r\n', b'\n') if n == 'a.c' else data)
        (gen / 'fast.c').write_bytes(fast)
        inst.state = {'done': ['extract', 'analyze', 'recompile', 'generate', 'patch', 'assets']}
        with mock.patch.object(install, 'KIT', kit), \
                mock.patch.object(inst, 'analysis_outputs_exist', return_value=True):
            self.assertTrue(inst.generation_outputs_valid('patch'))
            (patches / 'manifest.json').write_text(json.dumps({**main_manifest, 'base': {}}), encoding='utf-8')
            with self.assertRaises(install.Stop): inst.generation_outputs_valid('patch')
            with self.assertRaises(install.Stop): inst.patch()
            self.assertNotIn('patch_started', inst.state)
            self.assertEqual((gen / 'a.c').read_bytes(), b'edit\n')
            (patches / 'manifest.json').write_text(json.dumps({**main_manifest, 'patched': []}), encoding='utf-8')
            with self.assertRaises(install.Stop): inst.generation_outputs_valid('patch')
            (patches / 'manifest.json').write_text(json.dumps(main_manifest), encoding='utf-8')
            (patches / 'fastcode' / 'fastcode.json').write_text(json.dumps({**spec, 'final': {}}), encoding='utf-8')
            with self.assertRaises(install.Stop): inst.generation_outputs_valid('patch')
            (patches / 'fastcode' / 'fastcode.json').write_text(json.dumps(spec), encoding='utf-8')
            (post / 'manifest.json').write_text(
                json.dumps({**post_manifest, 'final': {'fast.c': digest(b'other')}}), encoding='utf-8')
            self.assertFalse(inst.generation_outputs_valid('patch'))
            (post / 'manifest.json').write_text(json.dumps(post_manifest), encoding='utf-8')
            (gen / 'a.c').write_bytes(b'evil\n')
            self.assertEqual(inst.stale_done(), ['recompile', 'generate', 'patch', 'assets'])
            self.assertEqual(inst.state['done'], ['extract', 'analyze'])
            inst.state = {'done': ['extract', 'analyze', 'recompile', 'generate'], 'patch_started': True}
            self.assertEqual(inst.stale_done(), [])
            for n, data in base.items(): (gen / n).write_bytes(data)
            self.assertTrue(inst.generation_outputs_valid('generate'))
            (gen / 'recomp_bringup.h').unlink()
            self.assertTrue(inst.generation_outputs_valid('recompile'))
            self.assertFalse(inst.generation_outputs_valid('generate'))
            inst.state = {'done': ['extract', 'analyze', 'recompile', 'generate']}
            with mock.patch.object(inst, 'analysis_outputs_exist', return_value=False):
                self.assertEqual(inst.stale_done(), ['analyze', 'recompile', 'generate'])

    @unittest.skipIf(os.name == 'nt', 'POSIX process groups are not available on Windows')
    def test_non_windows_process_flags_and_group_cancellation(self):
        inst = self.installer('analyze')
        inst.low_priority = True
        lines = []
        inst.log = lines.append
        with mock.patch.object(install, 'tool_job', side_effect=AssertionError('Windows job used')):
            inst.run_cmd([sys.executable, '-c', 'print("synthetic tool")'])
        self.assertTrue(any('synthetic tool' in line for line in lines))
        proc = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            inst.proc = proc
            inst.cancel()
            proc.wait(timeout=5)
            self.assertNotEqual(proc.returncode, 0)
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, install.signal.SIGKILL)
                proc.wait(timeout=5)

    def test_asset_cache_version_matches_windows_newlines(self):
        target = self.root / 'assets-install' / 'pc_assets' / 'common.mgf'
        data = b'synthetic archive'
        class FakeMgf:
            files = {pc_assets.DISPLAY: 0, pc_assets.scenario_assets.NAME: 1}
            def __init__(self, path): pass
            def file(self, name): return b''
        def build_base():
            target.parent.mkdir(parents=True)
            target.write_bytes(data)
        (self.here / 'data' / 'pc-overlay.json').write_text(
            json.dumps({'files': 2, 'changed': {}}), encoding='utf-8')
        with mock.patch.object(pc_assets, 'HERE', self.here), \
                mock.patch.object(pc_assets, 'Mgf', FakeMgf), \
                mock.patch.object(pc_assets.base, 'build', side_effect=build_base), \
                mock.patch.object(pc_assets.base, 'parse', return_value=[]), \
                mock.patch.object(pc_assets.base, 'encode', return_value=b''), \
                mock.patch.object(pc_assets, 'display_rows', return_value=[]), \
                mock.patch.object(pc_assets, 'replace'), \
                mock.patch.object(pc_assets, 'add_qte'), \
                mock.patch.object(pc_assets.qte_icons, 'icons', return_value={}), \
                mock.patch.object(pc_assets.scenario_assets, 'transform', return_value=b''), \
                mock.patch.object(pc_assets, 'overlay_changes', return_value={}):
            pc_assets.build(self.root / 'assets-install', log=lambda line: None)
            pc_assets.verify_existing(self.root / 'assets-install')
            inst = install.Installer(None, self.root / 'assets-install', log=lambda line: None)
            with mock.patch.object(install, 'KIT', self.root / 'no-extra-assets'):
                self.assertTrue(inst.asset_outputs_valid())
            (target.parent / 'cache.version').write_bytes(b'wrong')
            with self.assertRaises(ValueError): pc_assets.verify_existing(self.root / 'assets-install')
            with mock.patch.object(install, 'KIT', self.root / 'no-extra-assets'):
                self.assertFalse(inst.asset_outputs_valid())
            (target.parent / 'cache.version').write_bytes(
                ('PC2-' + digest(data)[:16] + '\r\n').encode('ascii'))
            with mock.patch.object(pc_assets, 'overlay_changes', return_value={'synthetic': 'wrong'}):
                with self.assertRaises(SystemExit): pc_assets.verify_existing(self.root / 'assets-install')
        self.assertEqual((target.parent / 'cache.version').read_bytes(),
                         ('PC2-' + digest(data)[:16] + '\r\n').encode('ascii'))


if __name__ == '__main__':
    unittest.main()
