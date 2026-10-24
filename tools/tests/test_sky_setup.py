#!/usr/bin/env python3
import contextlib
import io
import json
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sky_setup as sky
import sky_shader_cache


class SkySetupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "checkout with spaces"
        self.work = self.root / "private workspace"
        (self.repo / "tools").mkdir(parents=True)
        for name in ("sky_patch_nso.py", "requirements-skyrim.txt"):
            (self.repo / "tools" / name).write_text("fixture " + name)
        self.cache = self.repo / "skyrim-work/build/shaders"
        self.cache.mkdir(parents=True)
        for suffix, contents in (("metal", "shader source"), ("json", "{}")):
            (self.cache / ("a" * 64 + "." + suffix)).write_text(contents)
        self.base, self.keys = [self.root / name for name in ("owned base.xci", "prod.keys")]
        for path in (self.base, self.keys):
            path.write_bytes(("private fixture " + path.name).encode())
        self.base.write_bytes(bytes(0x100) + b"HEAD" + self.base.read_bytes())
        self.modules = {name: b"NSO0" + bytes(124) + name.encode()
                        for name in ("main", "sdk")}
        self.game, self.metadata = sky.game_paths(self.work)
        self.output = self.work / "build/rewrite-native"
        self.fail_module = None
        self.fail_shaders = False
        self.unsupported = 0
        self.previous = None
        self.calls = []
        replacement = patch.object(sky, "REPO", self.repo)
        replacement.start()
        self.addCleanup(replacement.stop)
        # Setup integration uses a small authored cache with its own exact identity.
        replacement = patch.object(sky, "SHADER_REFERENCE", sky.shader_fingerprint(self.cache))
        replacement.start()
        self.addCleanup(replacement.stop)
        for attribute, options in (
                ("ensure_tools", {"side_effect": lambda *args: {"SKY_PYTHON": "/pinned/python3"}}),
                ("check_tools", {"return_value": []}),
                ("extract_xci", {"side_effect": self.fake_extract}),
                ("run", {"side_effect": self.fake_run})):
            replacement = patch.object(sky, attribute, **options)
            setattr(self, attribute, replacement.start())
            self.addCleanup(replacement.stop)

    def snapshot(self, root):
        return {str(path.relative_to(root)): path.read_bytes()
                for path in root.rglob("*") if path.is_file()}

    def test_workspace_inside_split_dump_fails_before_installing_tools(self):
        dump = self.root / "original split.xci"
        dump.mkdir()
        (dump / "00").write_bytes(bytes(0x100) + b"HEAD")
        before = self.snapshot(dump)
        for mode in ([], ["--tools-only"]):
            with self.subTest(mode=mode), contextlib.redirect_stderr(io.StringIO()):
                result = sky.main([str(dump), "--keys", str(self.keys),
                                   "--work-dir", str(dump / "generated"), *mode])
                self.assertEqual(result, 1)
                self.ensure_tools.assert_not_called()
                self.assertEqual(self.snapshot(dump), before)
                self.assertFalse((dump / "generated").exists())

    def create_game(self):
        (self.game / "ExeFS").mkdir(parents=True)
        for name, data in self.modules.items():
            (self.game / "ExeFS" / name).write_bytes(data)
        (self.game / "Control").mkdir()
        nacp = bytearray(0x3158)
        struct.pack_into("<QQ", nacp, 0x3080, 0x100000, 0x20000)
        struct.pack_into("<QQ", nacp, 0x3148, 0x10000000, 0x2000000)
        (self.game / "Control/control.nacp").write_bytes(nacp)
        (self.game / "RomFS/Data").mkdir(parents=True)
        (self.game / "RomFS/Skyrim.ini").write_text("[General]\n")
        (self.game / "RomFS/Data/Textures.bsa").write_bytes(b"complete archive fixture")
        (self.game / "RomFS/Data/Skyrim - Shaders.bsa").write_bytes(b"shader archive fixture")
        self.metadata.mkdir(parents=True)

    def fake_extract(self, source, keys, work, env):
        if not self.game.exists():
            self.create_game()
        return self.game, self.metadata

    def fake_run(self, command, env):
        command = [str(part) for part in command]
        name = Path(command[1]).name if len(command) > 1 else command[0]
        self.calls.append((name, command, env.copy()))
        if name == "sky_inventory.py":
            (self.metadata / "modules.tsv").write_text(
                "filename\tunsupported_relocation_count\nmain\t%d\nsdk\t0\n" % self.unsupported)
            for filename in ("imports.tsv", "relocations.tsv"):
                (self.metadata / filename).write_text("module\n")
        elif name == "sky_patch_nso.py":
            self.assertEqual(self.snapshot(self.output), self.previous or {})
            module = command[4]
            if module == self.fail_module:
                raise subprocess.CalledProcessError(1, command)
            staged = Path(command[3])
            staged.mkdir(parents=True, exist_ok=True)
            for suffix in sky.MODULE_FILES:
                (staged / (module + "." + suffix)).write_bytes(
                    b"" if suffix == "gates.tsv" else (module + " " + suffix).encode())
        elif name == "rebuild_shaders.py":
            self.assertTrue(sky.rewrite_is_current(self.output, sky.rewrite_identity(self.game)))
            self.assertTrue((self.metadata / "romfs-files.json").is_file())
            if self.fail_shaders:
                raise subprocess.CalledProcessError(1, command)
            if not self.cache.exists():
                self.cache.mkdir(parents=True)
                for suffix, contents in (("metal", "shader source"), ("json", "{}")):
                    (self.cache / ("a" * 64 + "." + suffix)).write_text(contents)

    def invoke(self, *extra):
        arguments = ["--work-dir", str(self.work), *extra]
        if not any(mode in extra for mode in ("--check", "--tools-only")):
            arguments += [str(self.base), "--keys", str(self.keys)]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return sky.main(arguments)

    def names(self):
        return [name for name, _, _ in self.calls]

    def remove_shader_cache(self):
        for path in self.cache.iterdir():
            path.unlink()
        self.cache.rmdir()

    def input_invoke(self, arguments=(), answers=(), interactive=True):
        with patch.object(sys.stdin, "isatty", return_value=interactive), \
                patch("builtins.input", side_effect=answers) as prompt, \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            result = sky.main(["--work-dir", str(self.work), *arguments])
        return result, prompt, errors.getvalue()

    def test_interactive_setup_accepts_quoted_paths_and_retries_invalid_answers(self):
        result, prompt, _ = self.input_invoke(answers=[
            "", str(self.root / "missing.xci"), f"'{self.base}'", f'"{self.keys}"'])
        self.assertEqual(result, 0)
        self.assertEqual(prompt.call_count, 4)
        self.extract_xci.assert_called_once()
        self.assertEqual(self.extract_xci.call_args.args[:3], (self.base, self.keys, self.work))
        self.assertEqual(self.extract_xci.call_args.args[3]["SKY_PYTHON"], "/pinned/python3")

    def test_prompt_expands_home_directory(self):
        with patch.object(sys.stdin, "isatty", return_value=True), \
                patch("builtins.input", return_value="~"):
            self.assertEqual(sky.input_path(None, "--base PATH", "Dump", Path.is_dir), Path.home().resolve())

    def test_ready_command_uses_absolute_quoted_launcher_and_workspace(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(sky.main([str(self.base), "--keys", str(self.keys),
                                       "--work-dir", str(self.work)]), 0)
        self.assertIn("Run SKY_WORK_DIR=" + shlex.quote(str(self.work)) + " "
                      + shlex.quote(str(self.repo / "skyhost/run.sh")), output.getvalue())

    def test_interactive_setup_prompts_only_for_missing_input(self):
        result, prompt, _ = self.input_invoke([str(self.base)], answers=[str(self.keys)])
        self.assertEqual(result, 0)
        self.assertEqual(prompt.call_count, 1)
        self.assertIn("prod.keys", prompt.call_args.args[0])

    def test_explicit_inputs_do_not_prompt_even_in_a_terminal(self):
        result, prompt, _ = self.input_invoke(["--base", str(self.base), "--keys", str(self.keys)])
        self.assertEqual(result, 0)
        prompt.assert_not_called()

    def create_default_inputs(self, name="Skyrim.XCI", split=False):
        directory = self.repo / "inputs"
        directory.mkdir()
        source = directory / name
        if split:
            source.mkdir()
            (source / "00").write_bytes(self.base.read_bytes())
        else:
            source.write_bytes(self.base.read_bytes())
        keys = directory / "prod.keys"
        keys.write_bytes(self.keys.read_bytes())
        return source, keys

    def test_repo_input_defaults_prepare_mac_and_ios_without_prompts(self):
        source, keys = self.create_default_inputs()
        before = self.snapshot(self.repo / "inputs")
        decoy = self.work / "inputs"
        decoy.mkdir(parents=True)
        (decoy / "other.xci").write_bytes(b"wrong workspace input")
        for mode in ([], ["--ios"]):
            with self.subTest(mode=mode):
                result, prompt, errors = self.input_invoke(mode, interactive=False)
                self.assertEqual(result, 0, errors)
                prompt.assert_not_called()
                self.assertEqual(self.extract_xci.call_args.args[:3], (source, keys, self.work))
                self.ensure_tools.assert_called_with(self.work, bool(mode))
                self.assertEqual(self.snapshot(self.repo / "inputs"), before)

    def test_repo_input_defaults_accept_xci_split_directory(self):
        source, keys = self.create_default_inputs("Split dump.xci", split=True)
        result, prompt, errors = self.input_invoke(interactive=False)
        self.assertEqual(result, 0, errors)
        prompt.assert_not_called()
        self.assertEqual(self.extract_xci.call_args.args[:2], (source, keys))

    def test_repo_input_defaults_accept_numbered_parts_directly(self):
        source, keys = self.create_default_inputs("00")
        result, prompt, errors = self.input_invoke(interactive=False)
        self.assertEqual(result, 0, errors)
        prompt.assert_not_called()
        self.assertEqual(self.extract_xci.call_args.args[:2], (source.parent, keys))

    def test_multiple_default_dumps_require_explicit_selection_before_setup(self):
        source, _ = self.create_default_inputs()
        for name in ("other.xci", "00"):
            candidate = source.parent / name
            candidate.write_bytes(b"another fake dump")
            with self.subTest(name=name):
                result, prompt, errors = self.input_invoke(interactive=True)
                self.assertEqual(result, 1)
                self.assertIn("Multiple game dumps found in inputs/", errors)
                self.assertIn("--base PATH", errors)
                prompt.assert_not_called()
                self.ensure_tools.assert_not_called()
                self.extract_xci.assert_not_called()
            candidate.unlink()

    def test_explicit_paths_override_even_ambiguous_input_folder(self):
        source, _ = self.create_default_inputs()
        (source.parent / "another.xci").write_bytes(b"another fake dump")
        result, prompt, errors = self.input_invoke(
            ["--base", str(self.base), "--keys", str(self.keys)], interactive=False)
        self.assertEqual(result, 0, errors)
        prompt.assert_not_called()
        self.assertEqual(self.extract_xci.call_args.args[:2], (self.base, self.keys))

    def test_invalid_explicit_paths_never_fall_back_to_default_inputs(self):
        self.create_default_inputs()
        for arguments in ([str(self.root / "missing.xci")],
                          [str(self.base), "--keys", str(self.root / "missing.keys")]):
            with self.subTest(arguments=arguments):
                result, prompt, errors = self.input_invoke(arguments, interactive=False)
                self.assertEqual(result, 1)
                self.assertIn("existing path", errors)
                prompt.assert_not_called()
                self.ensure_tools.assert_not_called()
                self.extract_xci.assert_not_called()

    def test_input_folder_without_game_files_preserves_terminal_prompts(self):
        directory = self.repo / "inputs"
        directory.mkdir()
        (directory / "README.md").write_text("Place owned inputs here")
        result, prompt, errors = self.input_invoke(answers=[str(self.base), str(self.keys)])
        self.assertEqual(result, 0, errors)
        self.assertEqual(prompt.call_count, 2)

    def test_default_dump_with_missing_keys_prompts_only_for_keys(self):
        source, keys = self.create_default_inputs()
        keys.unlink()
        result, prompt, errors = self.input_invoke(answers=[str(self.keys)])
        self.assertEqual(result, 0, errors)
        self.assertEqual(prompt.call_count, 1)
        self.assertIn("prod.keys", prompt.call_args.args[0])
        self.assertEqual(self.extract_xci.call_args.args[:2], (source, self.keys))

    def test_nonpreparation_modes_never_discover_default_inputs(self):
        source, _ = self.create_default_inputs()
        (source.parent / "another.xci").write_bytes(b"another fake dump")
        before = self.snapshot(source.parent)
        with patch.object(sky, "default_dump_path", side_effect=AssertionError("must not discover")):
            for mode in ("--check", "--tools-only", "--deploy-ios"):
                with self.subTest(mode=mode):
                    _, prompt, errors = self.input_invoke([mode], interactive=False)
                    self.assertNotIn("Multiple game dumps", errors)
                    prompt.assert_not_called()
                    self.extract_xci.assert_not_called()
                    self.assertEqual(self.snapshot(source.parent), before)

    def test_noninteractive_missing_input_fails_before_tools_or_writes(self):
        before = self.snapshot(self.root)
        for arguments, option in (([], "--base PATH"), ([str(self.base)], "--keys PATH")):
            with self.subTest(arguments=arguments):
                result, prompt, errors = self.input_invoke(arguments, interactive=False)
                self.assertEqual(result, 1)
                self.assertIn(option, errors)
                prompt.assert_not_called()
                self.ensure_tools.assert_not_called()
                self.extract_xci.assert_not_called()
                self.assertEqual(self.snapshot(self.root), before)

    def test_invalid_explicit_input_fails_without_prompt_or_download(self):
        for arguments in ([str(self.root / "missing.xci")],
                          [str(self.base), "--keys", str(self.root)]):
            with self.subTest(arguments=arguments):
                result, prompt, errors = self.input_invoke(arguments)
                self.assertEqual(result, 1)
                self.assertIn("existing path", errors)
                prompt.assert_not_called()
                self.ensure_tools.assert_not_called()

    def test_check_and_tools_only_never_prompt_for_game_paths(self):
        for mode in ("--check", "--tools-only"):
            with self.subTest(mode=mode):
                _, prompt, _ = self.input_invoke([mode])
                prompt.assert_not_called()
                self.extract_xci.assert_not_called()
                self.run.assert_not_called()

    def test_deployment_uses_prepared_workspace_without_game_inputs_or_prompts(self):
        result, prompt, _ = self.input_invoke(["--deploy-ios", "--team", "ABCDEFGHIJ",
                                              "--device", "My iPad", "--bundle-id", "dev.example.game",
                                              "--romfs", str(self.root / "extracted RomFS")])
        self.assertEqual(result, 0)
        prompt.assert_not_called()
        self.ensure_tools.assert_not_called()
        self.extract_xci.assert_not_called()
        self.assertEqual(self.names(), ["sky_deploy_ios.py"])
        self.assertEqual(self.calls[0][1], [sys.executable, str(self.repo / "tools/sky_deploy_ios.py"),
                                          "--work-dir", str(self.work), "--team", "ABCDEFGHIJ",
                                          "--device", "My iPad", "--bundle-id", "dev.example.game",
                                          "--romfs", str(self.root / "extracted RomFS")])
        self.assertFalse(self.work.exists())

    def test_deployment_leaves_signing_choices_to_the_deployment_helper(self):
        result, prompt, _ = self.input_invoke(["--deploy-ios"])
        self.assertEqual(result, 0)
        prompt.assert_not_called()
        self.assertEqual(self.calls[0][1][2:], ["--work-dir", str(self.work)])

    def test_deployment_rejects_dump_keys_and_flags_on_other_modes(self):
        for arguments in (["--deploy-ios", str(self.base)],
                          ["--deploy-ios", "--keys", str(self.keys)],
                          ["--check", "--team", "ABCDEFGHIJ"],
                          ["--tools-only", "--romfs", str(self.root)],
                          ["--deploy-ios", "--check"]):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit) as error:
                self.input_invoke(arguments)
            self.assertEqual(error.exception.code, 2)
        self.ensure_tools.assert_not_called()
        self.run.assert_not_called()

    def test_eof_and_cancellation_exit_cleanly_without_installing(self):
        for exception, code in ((EOFError, 1), (KeyboardInterrupt, 130)):
            with self.subTest(exception=exception):
                result, _, errors = self.input_invoke(answers=exception)
                self.assertEqual(result, code)
                self.assertNotIn("Traceback", errors)
                self.ensure_tools.assert_not_called()
                self.assertFalse(self.work.exists())

    def test_fresh_dump_prepares_both_modules_and_builds_mac_host(self):
        self.assertEqual(self.invoke(), 0)
        self.assertEqual(self.names(), ["sky_inventory.py", "sky_patch_nso.py",
                                       "sky_patch_nso.py", "rebuild_shaders.py", "-C"])
        self.ensure_tools.assert_called_once_with(self.work, False)
        self.extract_xci.assert_called_once_with(self.base, self.keys, self.work,
                                                self.calls[0][2])
        self.assertEqual(self.game, self.work / "data/switch/skyrim/base/Update")
        self.assertEqual(self.calls[1][1][5:], ["--control-nacp",
                                              str(self.game / "Control/control.nacp")])
        self.assertEqual(self.calls[2][1][5:], [])
        self.assertTrue((self.metadata / "romfs-files.json").is_file())
        self.assertTrue(sky.rewrite_is_current(self.output, sky.rewrite_identity(self.game)))
        self.assertEqual(self.calls[-1][1][:2], ["/usr/bin/make", "-C"])
        self.assertEqual(self.calls[-1][2]["SKY_SHADER_CACHE_DIR"],
                         str(self.repo / "skyrim-work/build/shaders"))
        self.assertEqual(self.keys.read_bytes(), b"private fixture prod.keys")
        self.assertNotIn("prod.keys", "\n".join(self.snapshot(self.work)))

    def test_fresh_mac_setup_rebuilds_shaders_before_host_without_prompting(self):
        self.remove_shader_cache()
        result, prompt, errors = self.input_invoke([str(self.base), "--keys", str(self.keys)],
                                                  interactive=False)
        self.assertEqual(result, 0)
        prompt.assert_not_called()
        self.assertEqual(errors, "")
        self.assertEqual(self.names(), ["sky_inventory.py", "sky_patch_nso.py", "sky_patch_nso.py",
                                       "rebuild_shaders.py", "-C"])
        self.assertTrue(sky.rewrite_is_current(self.output, sky.rewrite_identity(self.game)))
        self.assertEqual(sky.shader_problems(), [])
        _, command, env = self.calls[-2]
        self.assertEqual(command, ["/pinned/python3", str(self.repo / "skyhost/tools/rebuild_shaders.py"),
                                   "--archive", str(self.game / "RomFS/Data/Skyrim - Shaders.bsa"),
                                   "--main", str(self.output / "main.patched.bin"),
                                   "--output", str(self.cache), "--work-dir", str(self.work), "--ensure"])
        self.assertEqual(env["SKY_SHADER_CACHE_DIR"], str(self.cache))
        self.assertFalse((self.work / "build/shaders").exists())

    def test_fresh_ios_setup_rebuilds_shaders_before_preparing_project_without_prompting(self):
        self.remove_shader_cache()
        result, prompt, errors = self.input_invoke([str(self.base), "--keys", str(self.keys), "--ios"],
                                                  interactive=False)
        self.assertEqual(result, 0)
        prompt.assert_not_called()
        self.assertEqual(errors, "")
        self.ensure_tools.assert_called_once_with(self.work, True)
        self.extract_xci.assert_called_once()
        self.assertEqual(self.names(), ["sky_inventory.py", "sky_patch_nso.py", "sky_patch_nso.py",
                                       "rebuild_shaders.py", "build.py"])
        self.assertEqual(sky.shader_problems(), [])

    def test_shader_verification_failure_stops_both_build_paths_and_preserves_cache(self):
        self.fail_shaders = True
        before = self.snapshot(self.cache)
        for mode in ((), ("--ios",)):
            with self.subTest(mode=mode):
                self.calls.clear()
                result, prompt, errors = self.input_invoke([str(self.base), "--keys", str(self.keys), *mode],
                                                          interactive=False)
                self.assertEqual(result, 1)
                prompt.assert_not_called()
                self.assertIn("rebuild_shaders.py", errors)
                self.assertNotIn("-C", self.names())
                self.assertNotIn("build.py", self.names())
                self.assertEqual(self.names()[-1], "rebuild_shaders.py")
                self.assertEqual(self.snapshot(self.cache), before)

    def test_check_rejects_missing_shaders_after_mac_preparation_without_writing(self):
        self.assertEqual(self.invoke(), 0)
        self.remove_shader_cache()
        before = self.snapshot(self.root)
        self.ensure_tools.reset_mock()
        self.extract_xci.reset_mock()
        self.run.reset_mock()
        result, _, errors = self.input_invoke(["--check"])
        self.assertEqual(result, 1)
        self.assertIn("Local translated shaders are required", errors)
        self.assertIn(str(self.cache), errors)
        self.ensure_tools.assert_not_called()
        self.extract_xci.assert_not_called()
        self.run.assert_not_called()
        self.assertEqual(self.snapshot(self.root), before)

    def test_partial_shader_pair_is_rejected_by_setup_and_check(self):
        for suffix, contents in (("metal", "shader source"), ("json", "{}")):
            path = self.cache / ("a" * 64 + "." + suffix)
            path.unlink()
            for mode in ((), ("--ios",), ("--check",)):
                with self.subTest(missing=suffix, mode=mode):
                    arguments = list(mode) if mode == ("--check",) else [
                        str(self.base), "--keys", str(self.keys), *mode]
                    result, _, errors = self.input_invoke(arguments)
                    self.assertEqual(result, 1)
                    component = "source" if suffix == "metal" else "reflection"
                    self.assertIn(f"Local shader {component} is missing", errors)
                    self.ensure_tools.assert_not_called()
                    self.extract_xci.assert_not_called()
                    self.assertEqual(self.calls, [])
            path.write_text(contents)

    def test_empty_shader_pair_is_rejected(self):
        for suffix, contents in (("metal", "shader source"), ("json", "{}")):
            path = self.cache / ("a" * 64 + "." + suffix)
            path.write_bytes(b"")
            with self.subTest(empty=suffix):
                self.assertIn("Local shader pair is empty", sky.shader_problems()[0])
                self.assertEqual(self.invoke(), 1)
                self.ensure_tools.assert_not_called()
                self.extract_xci.assert_not_called()
            path.write_text(contents)

    def test_one_arbitrary_pair_does_not_satisfy_production_shader_reference(self):
        before = self.snapshot(self.root)
        with patch.object(sky, "SHADER_REFERENCE", sky_shader_cache.REFERENCE):
            self.assertIn("differs from the recorded", sky.shader_problems()[0])
            self.assertEqual(self.invoke(), 1)
        self.ensure_tools.assert_not_called()
        self.extract_xci.assert_not_called()
        self.assertEqual(self.snapshot(self.root), before)

    def test_check_rejects_paired_but_incomplete_cache_without_writes(self):
        self.assertEqual(self.invoke(), 0)
        additional = [self.cache / ("b" * 64 + "." + suffix) for suffix in ("metal", "json")]
        for path in additional:
            path.write_bytes((self.cache / ("a" * 64 + path.suffix)).read_bytes())
        complete = sky.shader_fingerprint(self.cache)
        for path in additional:
            path.unlink()
        before = self.snapshot(self.root)
        with patch.object(sky, "SHADER_REFERENCE", complete):
            result, _, errors = self.input_invoke(["--check"])
        self.assertEqual(result, 1)
        self.assertIn("differs from the recorded", errors)
        self.assertEqual(self.snapshot(self.root), before)

    def test_same_size_shader_corruption_fails_setup_and_check_without_writes(self):
        self.assertEqual(self.invoke(), 0)
        for suffix in ("metal", "json"):
            path = self.cache / ("a" * 64 + "." + suffix)
            original = path.read_bytes()
            path.write_bytes(b"!" + original[1:])
            for mode in ((), ("--ios",), ("--check",)):
                with self.subTest(corrupt=suffix, mode=mode):
                    before = self.snapshot(self.root)
                    self.ensure_tools.reset_mock()
                    self.extract_xci.reset_mock()
                    self.run.reset_mock()
                    arguments = list(mode) if mode == ("--check",) else [
                        str(self.base), "--keys", str(self.keys), *mode]
                    result, _, errors = self.input_invoke(arguments)
                    self.assertEqual(result, 1)
                    self.assertIn("differs from the recorded", errors)
                    self.ensure_tools.assert_not_called()
                    self.extract_xci.assert_not_called()
                    self.run.assert_not_called()
                    self.assertEqual(self.snapshot(self.root), before)
            path.write_bytes(original)

    def test_bootstrap_allows_only_absent_or_empty_shader_cache(self):
        self.remove_shader_cache()
        self.assertEqual(sky.shader_problems(allow_missing=True), [])
        self.cache.mkdir()
        self.assertEqual(sky.shader_problems(allow_missing=True), [])
        (self.cache / "unverified-output.txt").write_bytes(b"retain this diagnostic")
        before = self.snapshot(self.root)
        self.assertTrue(sky.shader_problems(allow_missing=True))
        self.assertEqual(self.snapshot(self.root), before)

    def test_shader_cache_symlink_is_rejected_even_when_bootstrapping(self):
        self.remove_shader_cache()
        target = self.root / "elsewhere"
        target.mkdir()
        self.cache.symlink_to(target, target_is_directory=True)
        before = self.snapshot(self.root)
        for allow_missing in (False, True):
            with self.subTest(allow_missing=allow_missing):
                self.assertIn("regular directory", sky.shader_problems(allow_missing)[0])
                self.assertEqual(self.snapshot(self.root), before)

    def test_invalid_extracted_nso_stops_before_inventory_or_build(self):
        self.create_game()
        (self.game / "ExeFS/main").write_bytes(b"another game build")
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.calls, [])
        self.assertFalse(self.output.exists())

    def test_extractor_validation_error_preserves_workspace_and_saves(self):
        self.create_game()
        save = self.work / "run/save/Saves/keep.sav0"
        save.parent.mkdir(parents=True)
        save.write_bytes(b"irreplaceable saved game")
        before = self.snapshot(self.work)
        self.extract_xci.side_effect = ValueError("Existing extraction belongs to another cartridge")
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.snapshot(self.work), before)

    def test_unsupported_inventory_relocations_prevent_rewrite_and_build(self):
        self.unsupported = 1
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.names(), ["sky_inventory.py"])
        self.assertFalse(self.output.exists())

    def test_unchanged_rewrite_is_reused(self):
        self.assertEqual(self.invoke(), 0)
        before = self.snapshot(self.output)
        self.calls.clear()
        self.assertEqual(self.invoke(), 0)
        self.assertEqual(self.names(), ["sky_inventory.py", "rebuild_shaders.py", "-C"])
        self.assertEqual(self.snapshot(self.output), before)

    def test_changed_cartridge_control_settings_invalidate_rewrite(self):
        self.assertEqual(self.invoke(), 0)
        self.previous = self.snapshot(self.output)
        nacp = self.game / "Control/control.nacp"
        changed = bytearray(nacp.read_bytes())
        struct.pack_into("<Q", changed, 0x3080, 0x200000)
        nacp.write_bytes(changed)
        self.calls.clear()
        self.assertEqual(self.invoke(), 0)
        self.assertEqual(self.names().count("sky_patch_nso.py"), 2)
        state = json.loads((self.output / "setup.json").read_text())
        self.assertEqual(state["identity"]["control_nacp"], sky.sha256(nacp))

    def test_changed_valid_nso_identity_invalidates_rewrite(self):
        self.assertEqual(self.invoke(), 0)
        self.previous = self.snapshot(self.output)
        module = self.game / "ExeFS/main"
        module.write_bytes(module.read_bytes() + b"another cartridge module")
        self.calls.clear()
        self.assertEqual(self.invoke(), 0)
        self.assertEqual(self.names().count("sky_patch_nso.py"), 2)
        state = json.loads((self.output / "setup.json").read_text())
        self.assertEqual(state["identity"]["modules"]["main"], sky.sha256(module))

    def test_second_module_failure_preserves_published_rewrite_and_saves(self):
        self.assertEqual(self.invoke(), 0)
        self.previous = self.snapshot(self.output)
        save = self.work / "run/save/Saves/keep.sav0"
        save.parent.mkdir(parents=True)
        save.write_bytes(b"irreplaceable saved game")
        (self.repo / "tools/sky_patch_nso.py").write_text("changed rewriter")
        self.fail_module = "sdk"
        self.calls.clear()
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.snapshot(self.output), self.previous)
        self.assertEqual(save.read_bytes(), b"irreplaceable saved game")
        self.assertEqual(self.names(), ["sky_inventory.py", "sky_patch_nso.py", "sky_patch_nso.py"])
        self.assertFalse(self.output.with_name("rewrite-native.previous").exists())
        self.assertEqual(list(self.output.parent.glob("rewrite-*")), [self.output])

    def test_success_publishes_both_modules_together_and_accepts_empty_gates(self):
        self.assertEqual(self.invoke(), 0)
        self.previous = self.snapshot(self.output)
        (self.repo / "tools/sky_patch_nso.py").write_text("updated rewriter")
        self.assertEqual(self.invoke(), 0)
        state = json.loads((self.output / "setup.json").read_text())
        self.assertEqual(state["identity"], sky.rewrite_identity(self.game))
        for module in ("main", "sdk"):
            self.assertEqual((self.output / (module + ".gates.tsv")).read_bytes(), b"")
            self.assertIn(module + ".patched.bin", state["outputs"])
        self.assertFalse(self.output.with_name("rewrite-native.previous").exists())

    def test_check_never_installs_runs_commands_or_writes_files(self):
        self.assertEqual(self.invoke(), 0)
        before = self.snapshot(self.root)
        self.ensure_tools.reset_mock()
        self.extract_xci.reset_mock()
        self.run.reset_mock()
        with patch.object(Path, "mkdir", side_effect=AssertionError("check created directory")), \
                patch.object(Path, "write_text", side_effect=AssertionError("check wrote file")), \
                patch.object(Path, "write_bytes", side_effect=AssertionError("check wrote file")):
            self.assertEqual(self.invoke("--check", "--ios"), 0)
        self.ensure_tools.assert_not_called()
        self.extract_xci.assert_not_called()
        self.run.assert_not_called()
        self.check_tools.assert_called_once_with(self.work, True)
        self.assertEqual(self.snapshot(self.root), before)

    def test_ios_prepares_device_release_without_building_or_installing(self):
        for bundle, extra in (("dev.skyhost.skyrim", ()),
                              ("example.owned.skyrim", ("--bundle-id", "example.owned.skyrim"))):
            with self.subTest(bundle=bundle):
                self.calls.clear()
                self.assertEqual(self.invoke("--ios", *extra), 0)
                self.assertEqual(self.calls[-1][0], "build.py")
                self.assertEqual(self.calls[-1][1][2:], ["--sdk", "device", "--configuration",
                                                       "Release", "--bundle-id", bundle, "--prepare-only"])
                self.assertNotIn("-C", self.names())

    def test_recorded_romfs_rejects_truncation_without_blessing_new_manifest(self):
        self.assertEqual(self.invoke(), 0)
        manifest = self.metadata / "romfs-files.json"
        before = manifest.read_bytes()
        (self.game / "RomFS/Data/Textures.bsa").write_bytes(b"short")
        with self.assertRaises(ValueError):
            sky.validate_romfs_manifest(self.game, self.metadata)
        with self.assertRaises(ValueError):
            sky.record_romfs(self.game, self.metadata)
        self.assertEqual(manifest.read_bytes(), before)
        self.assertEqual(self.invoke("--check"), 1)


if __name__ == "__main__":
    unittest.main()
