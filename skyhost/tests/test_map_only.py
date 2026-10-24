#!/usr/bin/env python3
import csv
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


HOST = Path(os.environ.get("SKYHOST", Path(__file__).resolve().parents[1] / "skyhost"))
BUILD = Path(os.environ.get("SKYHOST_BUILD", HOST.parent / "build"))
WORK_DIR = Path(os.environ.get("SKY_WORK_DIR", Path(__file__).resolve().parents[2] / "skyrim-work"))
REWRITE_DIR = Path(os.environ.get(
    "SKY_REWRITE_DIR", str(WORK_DIR / "build/rewrite-native")
))
def _default_meta_dir(work_dir):
    for candidate in (
        work_dir / "data/switch/skyrim/base/meta",
        work_dir / "data/switch/skyrim/v327680/meta",
        work_dir / "build/inventory-from-rewritten/meta",
    ):
        if (candidate / "relocations.tsv").is_file():
            return candidate
    return work_dir / "data/switch/skyrim/base/meta"


META_DIR = Path(os.environ.get(
    "SKY_META_DIR", str(_default_meta_dir(WORK_DIR))
))
MODULES = ("main", "sdk")
REQUIRED_EXTENSIONS = ("patched.bin", "fixups.tsv", "gates.tsv")
RULE_EXTENSIONS = ("cuts.tsv", "obs.tsv", "params.tsv")


class HostTestCase(unittest.TestCase):
    def run_process(self, arguments, timeout=120, loadhash=False):
        environment = os.environ.copy()
        if loadhash:
            environment["DM_LOADHASH"] = "1"
        result = subprocess.run(
            [str(argument) for argument in arguments],
            text=True,
            capture_output=True,
            env=environment,
            timeout=timeout,
            check=False,
        )
        return result, result.stdout + result.stderr

    def require_real_inputs(self):
        self.assertTrue(HOST.is_file(), f"missing skyhost binary: {HOST}")
        for module in MODULES:
            for extension in REQUIRED_EXTENSIONS:
                path = REWRITE_DIR / f"{module}.{extension}"
                self.assertTrue(path.is_file(), f"missing rewritten input: {path}")
        for filename in ("relocations.tsv", "imports.tsv", "modules.tsv"):
            self.assertTrue((META_DIR / filename).is_file(), f"missing inventory: {filename}")

    def assert_rule_manifests_empty(self):
        for module in MODULES:
            for extension in RULE_EXTENSIONS:
                path = REWRITE_DIR / f"{module}.{extension}"
                self.assertTrue(path.is_file(), f"missing rule manifest: {path}")
                self.assertEqual(path.stat().st_size, 0, path)


class SyntheticHostTests(HostTestCase):
    def test_relocation_success(self):
        result, output = self.run_process([BUILD / "t_reloc"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_RELOC OK relative/abs64/glob_dat/jump_slot weak-null forced-entry", output)
        self.assertIn("FORCE entries=1 unused=0", output)

    def test_relocation_failures_and_unknown_svc(self):
        cases = (
            ("unsupported", "unsupported relocation type 261"),
            ("nonweak", "unresolved non-weak import required required by main"),
            ("nonweak-unknown", "unresolved non-weak import required required by main"),
            ("force-unused", "unused forced entry host_only"),
        )
        for mode, message in cases:
            with self.subTest(mode=mode):
                result, output = self.run_process([BUILD / "t_reloc", mode])
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(message, output)
        result, output = self.run_process([BUILD / "t_gate_abort"])
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("UNIMPL SVC 0x7f", output)
        self.assertIn("handler=sky_svc_unimplemented", output)

    def test_runtime_tls_and_services(self):
        result, output = self.run_process([BUILD / "t_runtime"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_RUNTIME OK tls/tlr main-handle getinfo querymemory split/merge movememory thread-create sync-wait sync-cycle priority/core", output)
        self.assertIn("GUEST_DEBUG synthetic-debug", output)

    def test_offline_aoc(self):
        result, output = self.run_process([BUILD / "t_aoc"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_AOC OK offline-count-zero empty-list", output)
        self.assertIn("AOC_COUNT count=0 source=host-offline", output)
        self.assertIn("AOC_LIST count=0 source=host-offline", output)

    def test_offline_account(self):
        result, output = self.run_process([BUILD / "t_account"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_ACCOUNT OK init stable-uid-16 handle-24 offline-query", output)
        self.assertIn("ACCOUNT_INITIALIZE mode=offline no-ipc", output)
        self.assertIn("ACCOUNT_READY uid_lo=0x1 uid_hi=0 network_service=0 mode=offline", output)

    def test_host_startup_defaults(self):
        result, output = self.run_process([BUILD / "t_startup"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_STARTUP OK host-time fixed-oe stored-performance-config language-en-US", output)
        self.assertIn("TIME_INITIALIZE implementation=host-clock no-ipc", output)
        self.assertIn("OE_PERFORMANCE_CONFIGURATION mode=0 value=0x20004", output)
        self.assertIn("OE_DESIRED_LANGUAGE code=en-US", output)

    def test_read_only_fs(self):
        result, output = self.run_process([BUILD / "t_fs"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_FS OK open/read/size/close entry-type short-read missing traversal mount-scope save-ensure-mount-create directory-io file-create-delete write-commit", output)
        self.assertIn("FS_OPEN_NOT_FOUND path=/missing.bin", output)
        self.assertIn("FS_ENTRY_TYPE_NOT_FOUND path=/missing.bin", output)
        self.assertIn("FS_CREATE_DIRECTORY path=/Saves", output)
        self.assertIn("FS_READ_DIRECTORY path=/Saves count=0", output)
        self.assertIn("FS_CREATE_FILE path=/created.bin size=4", output)
        self.assertIn("FS_WRITE path=/write.bin offset=2 requested=4 wrote=4", output)
        self.assertIn("SAVE_COMMIT name=save implementation=host-posix", output)
        self.assertIn("read=2", output)

    def test_offline_nifm(self):
        result, output = self.run_process([BUILD / "t_nifm"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NIFM OK offline-init unavailable request-noop submit-wait-failure failure-results socket-init", output)
        self.assertIn("NIFM_INITIALIZE mode=offline no-ipc", output)
        self.assertIn("NIFM_ERROR operation=SubmitNetworkRequestAndWait result=0x2a2 mode=offline", output)
        self.assertIn("SOCKET_INITIALIZE mode=offline no-session no-ipc", output)

    def test_offline_bcat(self):
        result, output = self.run_process([BUILD / "t_bcat"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_BCAT OK offline-init-no-download", output)
        self.assertIn("BCAT_INITIALIZE mode=offline no-ipc", output)

    def test_offline_nfp(self):
        result, output = self.run_process([BUILD / "t_nfp"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NFP OK offline-init-no-devices availability-failure", output)
        self.assertIn("NFP_INITIALIZE mode=offline devices=0 no-ipc", output)
        self.assertIn("NFP_ERROR operation=AttachAvailabilityChangeEvent result=0x2a2 mode=offline", output)

    def test_neutral_hid(self):
        result, output = self.run_process([BUILD / "t_hid"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_HID OK init style neutral-state-0x28 sampling", output)
        self.assertIn("HID_INITIALIZE mode=neutral no-ipc", output)
        self.assertIn("HID_STATE sampling=1 buttons=0 sticks=0 attributes=1", output)

    def test_host_audio(self):
        result, output = self.run_process([BUILD / "t_audio"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_AUDIO OK config-size host-open-no-hipc coherent-cache update-start-stop-close-event-destroy", output)
        self.assertIn("AUDIO_WORK_BUFFER_SIZE value=20480 source=skyrim-config", output)
        self.assertIn("AUDIO_OPEN handle=", output)
        self.assertIn("AUDIO_CACHE_FLUSH mode=host-coherent", output)
        self.assertIn("AUDIO_UPDATE handle=", output)
        self.assertIn("AUDIO_START handle=", output)
        self.assertIn("AUDIO_CLOSE handle=", output)
        self.assertIn("EVENT_DESTROY handle=", output)

    def test_transfer_memory(self):
        result, output = self.run_process([BUILD / "t_transfer"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_TRANSFER OK create-map-shared-unmap-close alignment-permission", output)
        self.assertIn("TRANSFER_CREATE handle=", output)
        self.assertIn("TRANSFER_MAP handle=", output)

    def test_offscreen_vi(self):
        result, output = self.run_process([BUILD / "t_vi"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_VI OK display-layer-native-offscreen", output)
        self.assertIn("VI_INITIALIZE mode=offscreen no-window", output)
        self.assertIn("VI_NATIVE_WINDOW native=", output)

    def test_nvn_bootstrap(self):
        result, output = self.run_process([BUILD / "t_nvn"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN OK bootstrap-device-debug-sync-queue-window-named-traps", output)
        self.assertIn("NVN_DEVICE_INITIALIZED device=", output)
        self.assertIn("NVN_DEVICE_INFO item=0x54 name=QUEUE_COMPUTE_MEMORY_MIN_SIZE value=16384", output)
        self.assertIn("NVN_DEVICE_INFO item=0x51 name=QUEUE_COMMAND_MEMORY_MIN_SIZE value=65536", output)
        self.assertIn("NVN_DEVICE_INFO item=0xf name=TEXTURE_DESCRIPTOR_SIZE value=32", output)
        self.assertIn("NVN_DEVICE_INFO item=0x24 name=MAX_TEXTURE_POOL_SIZE value=1048576", output)
        self.assertIn("NVN_DEVICE_INFO item=0x10 name=SAMPLER_DESCRIPTOR_SIZE value=32", output)
        self.assertIn("NVN_DEVICE_INFO item=0x12 name=RESERVED_SAMPLER_DESCRIPTORS value=256", output)
        self.assertIn("NVN_DEVICE_INFO item=0x25 name=MAX_SAMPLER_POOL_SIZE value=4096", output)
        self.assertIn("NVN_DEVICE_INFO item=0x39 name=LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT value=128", output)
        self.assertIn("NVN_SYNC_INITIALIZED sync=", output)
        self.assertIn("NVN_QUEUE_BUILDER_DEFAULTS device=", output)
        self.assertIn("NVN_QUEUE_INIT_ERROR reason=invalid-memory", output)
        self.assertIn("NVN_QUEUE_INITIALIZED queue=", output)
        self.assertIn("memory_size=12288", output)
        self.assertIn("metal_command_queue=1 memory_metal=shared memory_gpu_address=", output)
        self.assertIn("NVN_QUEUE_FINALIZED queue=", output)
        self.assertIn("VI_NATIVE_WINDOW native=", output)
        self.assertIn("NVN_WINDOW_INIT_ERROR reason=invalid-native-window", output)
        self.assertIn("NVN_WINDOW_INITIALIZED window=", output)
        self.assertIn("textures=0 active_textures=0 present_interval=2", output)
        unknown, unknown_output = self.run_process([BUILD / "t_nvn", "unknown"])
        self.assertNotEqual(unknown.returncode, 0, unknown_output)
        self.assertIn("NVN_UNIMPL name=nvnDeviceGetCurrentTimestampInNanoseconds", unknown_output)
        self.assertIn("reason=UNIMPL_NVN", unknown_output)
        textures, textures_output = self.run_process([BUILD / "t_nvn", "textures"])
        self.assertNotEqual(textures.returncode, 0, textures_output)
        self.assertIn("NVN_WINDOW_BUILDER_ERROR reason=invalid-textures count=5", textures_output)
        device_info, device_info_output = self.run_process([BUILD / "t_nvn", "device-info"])
        self.assertNotEqual(device_info.returncode, 0, device_info_output)
        self.assertIn(
            "NVN_RESOURCE_BOUNDARY name=nvnDeviceGetInteger item=0x60 reason=DEVICE_INFO_REQUIRED",
            device_info_output,
        )
        self.assertIn("NVN_UNIMPL name=nvnDeviceGetInteger", device_info_output)
        self.assertIn("reason=UNIMPL_NVN", device_info_output)

    def test_nvn_memory_pool(self):
        result, output = self.run_process([BUILD / "t_nvn_pool"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_POOL_OK shared-zero-copy-validation-map-address-ranges-cleanup", output)
        self.assertIn("NVN_MEMORY_POOL_INIT_ERROR reason=invalid-storage", output)
        self.assertIn("NVN_MEMORY_POOL_INIT_ERROR reason=invalid-device", output)
        self.assertIn("NVN_MEMORY_POOL_INITIALIZED pool=", output)
        self.assertIn("metal=shared zero_copy=1 gpu_address=", output)
        self.assertIn("operation=FlushMappedRange offset=1 size=0x11", output)
        self.assertIn("operation=InvalidateMappedRange offset=0", output)
        self.assertIn("mode=shared-coherent no-gpu-wait", output)
        self.assertIn("NVN_MEMORY_POOL_RANGE_ERROR operation=FlushMappedRange offset=-1", output)
        self.assertIn("NVN_BUFFER_INIT_ERROR reason=invalid-storage", output)
        self.assertIn("NVN_BUFFER_INITIALIZED buffer=", output)
        self.assertIn("NVN_BUFFER_ADDRESS buffer=", output)
        self.assertIn("NVN_MEMORY_POOL_FINALIZED pool=", output)

    def test_nvn_descriptor_pools(self):
        result, output = self.run_process([BUILD / "t_nvn_descriptors"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_DESCRIPTORS_OK storage-capacity-hash-collision-replacement-concurrent-resolve-pool-reuse-retained-lifetimes", output)
        self.assertIn("NVN_TEXTURE_POOL_INITIALIZED object=", output)
        self.assertIn("NVN_SAMPLER_POOL_INITIALIZED object=", output)
        self.assertIn("descriptor_size=32", output)
        self.assertIn("descriptors=uninitialized", output)
        self.assertIn("reason=invalid-alignment", output)
        self.assertIn("reason=invalid-capacity", output)
        self.assertIn("reason=invalid-range", output)
        self.assertIn("NVN_DESCRIPTOR_REGISTER_ERROR", output)
        self.assertIn("reason=invalid-id", output)
        self.assertIn("NVN_TEXTURE_POOL_FINALIZED object=", output)
        self.assertIn("NVN_SAMPLER_INITIALIZED sampler=", output)
        self.assertIn("NVN_SAMPLER_POOL_REGISTERED pool=", output)
        self.assertIn("id=0 sampler=", output)
        self.assertIn("id=1 sampler_builder=", output)
        self.assertIn("NVN_SAMPLER_POOL_FINALIZED object=", output)
        for mode, name in (
            ("register-texture", "nvnTexturePoolRegisterTexture"),
            ("register-image", "nvnTexturePoolRegisterImage"),
        ):
            with self.subTest(mode=mode):
                invalid, invalid_output = self.run_process([BUILD / "t_nvn_descriptors", mode])
                self.assertNotEqual(invalid.returncode, 0, invalid_output)
                self.assertIn(f"NVN_DESCRIPTOR_REGISTER_ERROR name={name}", invalid_output)
                self.assertIn("reason=invalid-texture", invalid_output)
                self.assertNotIn("NVN_UNIMPL", invalid_output)
        cases = (
            ("register-sampler", "nvnSamplerPoolRegisterSampler", "invalid-sampler"),
            ("register-sampler-builder", "nvnSamplerPoolRegisterSamplerBuilder", "invalid-sampler-builder"),
        )
        for mode, name, reason in cases:
            with self.subTest(mode=mode):
                invalid, invalid_output = self.run_process([BUILD / "t_nvn_descriptors", mode])
                self.assertNotEqual(invalid.returncode, 0, invalid_output)
                self.assertIn(f"NVN_DESCRIPTOR_REGISTER_ERROR name={name}", invalid_output)
                self.assertIn(f"reason={reason}", invalid_output)
                self.assertNotIn("NVN_UNIMPL", invalid_output)

    def test_nvn_texture_objects(self):
        result, output = self.run_process([BUILD / "t_nvn_texture"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_TEXTURE_OK metal-properties-registration-window-cleanup", output)
        self.assertIn("NVN_TEXTURE_INIT_ERROR reason=unsupported-format", output)
        self.assertIn("NVN_TEXTURE_INIT_ERROR reason=unsupported-target", output)
        self.assertIn("NVN_TEXTURE_INITIALIZED texture=", output)
        self.assertIn("pixel_format=70 texture_type=2 usage=0x5", output)
        self.assertIn("NVN_TEXTURE_POOL_REGISTERED pool=", output)
        self.assertIn("NVN_WINDOW_INITIALIZED window=", output)
        self.assertIn("NVN_TEXTURE_FINALIZED texture=", output)
        self.assertIn("NVN_WINDOW_FINALIZED window=", output)

    def test_nvn_sync(self):
        result, output = self.run_process([BUILD / "t_nvn_sync"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_SYNC_OK idle-outstanding-timeout-wake-cleanup", output)
        self.assertIn("NVN_QUEUE_FENCE queue=", output)
        self.assertIn("fence=0 submits=0 completed=0", output)
        self.assertIn("NVN_SYNC_WAIT sync=", output)
        self.assertIn("fence=1 result=1", output)
        self.assertIn("NVN_QUEUE_FLUSH queue=", output)
        self.assertIn("NVN_QUEUE_FINISH queue=", output)
        self.assertIn("submits=3 completed=3", output)

    def test_nvn_views(self):
        result, output = self.run_process([BUILD / "t_nvn_views"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_VIEWS_OK ranges-swizzle-depth-stencil-ownership-cleanup", output)
        self.assertIn("NVN_DEVICE_INFO item=0x38 name=LINEAR_TEXTURE_STRIDE_ALIGNMENT value=32", output)
        self.assertIn("NVN_TEXTURE_POOL_REGISTERED pool=", output)
        self.assertIn("has_view=1", output)
        self.assertIn("NVN_TEXTURE_FORMAT_CONVERSION format=0x35 pixel_format=260", output)
        self.assertIn("NVN_DESCRIPTOR_REGISTER_ERROR", output)
        self.assertIn("reason=unsupported-view", output)
        self.assertIn("NVN_TEXTURE_FINALIZED texture=", output)
        self.assertIn("NVN_TEXTURE_POOL_FINALIZED object=", output)

    def test_nvn_commands(self):
        result, output = self.run_process([BUILD / "t_nvn_commands"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn(
            "T_NVN_COMMANDS_OK lifecycle-indexed-color-depth-stencil-fence-inflight-readback-cleanup",
            output,
        )
        self.assertIn("NVN_COMMAND_BUFFER_INITIALIZED command_buffer=", output)
        self.assertIn("NVN_COMMAND_BUFFER_ERROR reason=begin-recording-state", output)
        self.assertIn("NVN_COMMAND_BUFFER_ERROR reason=end-recording-state", output)
        self.assertIn("NVN_QUEUE_SUBMIT_ERROR reason=invalid-command-state", output)
        self.assertIn("NVN_SHADER_GPU_OK", output)
        self.assertIn("NVN_METAL_WORK_COMPLETED draws=1", output)
        self.assertIn("NVN_QUEUE_SUBMIT queue=", output)
        self.assertIn("NVN_QUEUE_FENCE queue=", output)
        self.assertIn("fence=2 submits=2", output)
        self.assertIn("NVN_SYNC_WAIT sync=", output)
        self.assertIn("NVN_QUEUE_FINISH queue=", output)
        self.assertIn("submits=3 completed=3", output)
        self.assertIn(
            "NVN_COMMAND_READBACK depth={0.25,0.25,0.25,0.25} "
            "stencil={0xa5,0xa5,0xa5,0xa5}",
            output,
        )
        self.assertNotIn("NVN_UNIMPL", output)

    def test_nvn_program(self):
        result, output = self.run_process([BUILD / "t_nvn_program"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NVN_PROGRAM_OK lifecycle-validation-exact-capture-malformed-cleanup", output)
        self.assertIn("NVN_PROGRAM_INITIALIZED program=", output)
        self.assertIn("NVN_PROGRAM_SHADERS_SET program=", output)
        self.assertIn("count=2 stage_mask=0x3", output)
        self.assertIn("NVN_SHADER_CAPTURE program=", output)
        self.assertIn("stage=vertex code_size=0x200 control_size=0x880", output)
        self.assertIn("stage=fragment code_size=0x200 control_size=0x880", output)
        self.assertIn("reason=duplicate-stage", output)
        self.assertIn("reason=truncated-maxwell-header", output)
        self.assertIn("reason=truncated-or-unreadable-control", output)
        self.assertIn("reason=capture-io-failed", output)
        self.assertIn("NVN_PROGRAM_FINALIZED program=", output)
        artifact_paths = re.findall(r"^NVN_PROGRAM_TEST_ARTIFACTS dir=(.+)$", output, re.MULTILINE)
        self.assertEqual(len(artifact_paths), 1, output)
        artifact_dir = Path(artifact_paths[0])
        self.assertTrue(artifact_dir.is_absolute(), artifact_dir)
        with (artifact_dir / "manifest.tsv").open(newline="", encoding="utf-8") as stream:
            captures = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual([row["stage"] for row in captures[:2]], ["vertex", "fragment"])
        for row in captures:
            artifact = Path(row["json"])
            self.assertEqual(artifact.parent, artifact_dir)
            self.assertTrue(artifact.is_file(), artifact)
        self.assertNotIn("NVN_UNIMPL", output)

    def test_nv_capability(self):
        result, output = self.run_process([BUILD / "t_nv"])
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("T_NV OK offscreen-initialize", output)
        self.assertIn("NV_INITIALIZE_GRAPHICS mode=offscreen no-nvdrv", output)


class RealMapOnlyTests(HostTestCase):
    def inventory_expectations(self):
        relocation_names = {
            "RELATIVE": "relative",
            "ABS64": "abs64",
            "GLOB_DAT": "glob_dat",
            "JUMP_SLOT": "jump_slot",
        }
        expected = {module: {name: 0 for name in relocation_names.values()} for module in MODULES}
        with (META_DIR / "relocations.tsv").open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                module = row["module"]
                if module in expected:
                    expected[module][relocation_names[row["type_name"]]] += int(row["count"])
        weak_null = {module: 0 for module in MODULES}
        with (META_DIR / "imports.tsv").open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                if row["module"] not in MODULES or row["resolution"] != "unresolved":
                    continue
                self.assertEqual(row["binding"], "WEAK", row)
                weak_null[row["module"]] += int(row["relocation_count"])
        for module in MODULES:
            expected[module]["total"] = sum(expected[module].values())
            expected[module]["weak_null"] = weak_null[module]
        return expected

    def parse_and_validate(self, output, expected):
        map_rows = re.findall(
            r"\[skyhost\] MAP (main|sdk) base=(0x[0-9a-f]+) size=(0x[0-9a-f]+) "
            r"file=(0x[0-9a-f]+) text=rx data=rw key_page=r patch=rx",
            output,
        )
        self.assertEqual([row[0] for row in map_rows], list(MODULES), output)
        for module, _, mapped_size, file_size in map_rows:
            actual_file_size = (REWRITE_DIR / f"{module}.patched.bin").stat().st_size
            self.assertEqual(int(file_size, 16), actual_file_size)
            self.assertEqual(int(mapped_size, 16), (actual_file_size + 0x3FFF) & ~0x3FFF)

        gates = dict(re.findall(r"\[skyhost\] GATES (main|sdk) count=(\d+)", output))
        self.assertEqual(set(gates), set(MODULES), output)
        self.assertEqual(sum(int(count) for count in gates.values()), 118, output)

        relocations = {}
        pattern = re.compile(
            r"\[skyhost\] RELOC (main|sdk) relative=(\d+) abs64=(\d+) glob_dat=(\d+) "
            r"jump_slot=(\d+) total=(\d+) weak_null=(\d+)"
        )
        for row in pattern.findall(output):
            module = row[0]
            values = [int(value) for value in row[1:]]
            relocations[module] = dict(zip(
                ("relative", "abs64", "glob_dat", "jump_slot", "total", "weak_null"), values
            ))
        self.assertEqual(relocations, expected, output)

        hashes = {}
        for module, size, count, digest in re.findall(
            r"\[skyhost\] LOADHASH (main|sdk) size=(0x[0-9a-f]+) "
            r"relocations=(\d+) sha256=([0-9a-f]{64})",
            output,
        ):
            self.assertEqual(int(size, 16), (REWRITE_DIR / f"{module}.patched.bin").stat().st_size + 0x3FFF & ~0x3FFF)
            self.assertEqual(int(count), expected[module]["total"])
            hashes[module] = digest
        self.assertEqual(set(hashes), set(MODULES), output)
        self.assertRegex(output, r"\[skyhost\] FORCE entries=\d+ unused=0")
        self.assertIn("[skyhost] LOAD_OK modules=2 gates=118", output)
        self.assertIn("[skyhost] MAP_ONLY_OK modules=main,sdk gates=118", output)
        self.assertNotIn("rtld", output.lower())
        for marker in ("UNIMPL", "GUEST_START", "FRONT_RUN", "NNMAIN", "DT_INIT"):
            self.assertNotIn(marker, output.upper())
        return hashes

    def test_real_map_only_twice_hash_stable_and_no_rule_files_required(self):
        self.require_real_inputs()
        self.assert_rule_manifests_empty()
        expected = self.inventory_expectations()

        first, first_output = self.run_process([HOST, "--map-only", REWRITE_DIR], loadhash=True)
        self.assertEqual(first.returncode, 0, first_output)
        first_hashes = self.parse_and_validate(first_output, expected)

        with tempfile.TemporaryDirectory() as temporary:
            minimal = Path(temporary)
            for module in MODULES:
                for extension in REQUIRED_EXTENSIONS:
                    os.symlink(REWRITE_DIR / f"{module}.{extension}", minimal / f"{module}.{extension}")
            for module in MODULES:
                for extension in RULE_EXTENSIONS:
                    self.assertFalse((minimal / f"{module}.{extension}").exists())
            second, second_output = self.run_process([HOST, "--map-only", minimal], loadhash=True)
            self.assertEqual(second.returncode, 0, second_output)
            second_hashes = self.parse_and_validate(second_output, expected)

        self.assertEqual(first_hashes, second_hashes)
        self.assert_rule_manifests_empty()


if __name__ == "__main__":
    unittest.main()
