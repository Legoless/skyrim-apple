import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("shaders", Path(__file__).parents[1] / "tools/shaders.py")
shaders = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shaders)


class ShaderTranslationTests(unittest.TestCase):
    def metadata(self, stage="vertex"):
        return {"stage": stage, "stage_ordinal": shaders.ORDINALS[stage],
                "program_start": 48, "program_length": 80,
                "constant_offset": 256, "constant_size": 16,
                "workgroup": [8, 1, 1], "shader_key": "abc"}

    def test_identity_ignores_runtime_metadata_and_unused_graphics_dimensions(self):
        first = self.metadata()
        second = dict(first, data_address="0x100000", control_sha256="different", workgroup=[32, 2, 1])
        self.assertEqual(shaders.shader_key(first, b"original instructions"),
                         shaders.shader_key(second, b"original instructions"))
        self.assertNotEqual(shaders.shader_key(first, b"original instructions"),
                            shaders.shader_key(first, b"changed instructions"))
        first["stage_ordinal"] = 5
        second["stage_ordinal"] = 5
        self.assertNotEqual(shaders.shader_key(first, b"same instructions"),
                            shaders.shader_key(second, b"same instructions"))

    def test_reflects_actual_nvn_constant_and_combined_texture_slots(self):
        source = """
struct spvDescriptorSetBuffer0 {
    constant _11* vp_c3 [[id(2)]];
    constant _12* vp_c1 [[id(3)]];
    texture2d<float> vp_t_tcb_8 [[id(4)]];
    sampler vp_t_tcb_8Smplr [[id(5)]];
};
struct main0_in { float4 m_0 [[attribute(0)]]; };
vertex main0_out main0(main0_in in [[stage_in]]) { return {}; }
"""
        resources = {"sbufs": [], "notes": []}
        result = shaders.reflection(self.metadata(), source, resources, bytes.fromhex("01020304"))
        self.assertEqual(result["entry"], "main0")
        self.assertEqual(result["c1"], "01020304")
        self.assertEqual(result["driver_texture_base"], 32)
        self.assertEqual([(b["kind"], b.get("reg"), b["arg_id"]) for b in result["bindings"]],
                         [("cbuf", 0, 2), ("c1", None, 3), ("tex", 0, 4), ("samp_tex", 0, 5)])
        self.assertEqual(result["vertex_inputs"], [{"location": 0, "msl_type": "float4"}])

    def test_unresolvable_resource_fails_instead_of_binding_an_arbitrary_slot(self):
        source = """
struct spvDescriptorSetBuffer0 {\n texture2d<float> fp_t_cb3_1 [[id(0)]];\n};
fragment float4 main0() { return {}; }
"""
        with self.assertRaisesRegex(ValueError, "unresolved NVN slot"):
            shaders.reflection(self.metadata("fragment"), source, {"sbufs": [], "notes": []}, b"")

    def test_capture_integrity_is_required_before_compilation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            metadata = dict(self.metadata(), accepted=True, code_file="code.bin", code_size=4,
                            code_sha256=hashlib.sha256(b"code").hexdigest())
            metadata.pop("shader_key")
            path = directory / "shader.json"
            path.write_text(json.dumps(metadata))
            (directory / "code.bin").write_bytes(b"code")
            self.assertIsNotNone(shaders.load_capture(path))
            (directory / "code.bin").write_bytes(b"oops")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                shaders.load_capture(path)


class AuthoredShaderFixtureTests(unittest.TestCase):
    def test_fixtures_have_matching_entry_points_and_argument_layouts(self):
        fixtures = Path(__file__).resolve().parent / "fixtures/shaders"
        sources = {path.stem: path for path in fixtures.glob("*.metal")}
        reflections = {path.stem: path for path in fixtures.glob("*.json")}
        self.assertTrue(sources, "Project-authored shader fixtures are required")
        self.assertEqual(sources.keys(), reflections.keys())
        for name, source in sources.items():
            with self.subTest(name=name):
                metadata = json.loads(reflections[name].read_text())
                code = source.read_text()
                self.assertEqual(metadata["route"], "B")
                self.assertTrue(metadata["entry"])
                self.assertRegex(code, r"\b(?:vertex|fragment|kernel)\b[\s\S]*?\b"
                                 + re.escape(metadata["entry"]) + r"\s*\(")
                argument_ids = [binding["arg_id"] for binding in metadata["bindings"]]
                self.assertEqual(len(argument_ids), len(set(argument_ids)))
                self.assertEqual(set(argument_ids),
                                 {int(value) for value in re.findall(r"\[\[\s*id\((\d+)\)\s*\]\]", code)})
                self.assertEqual({item["location"] for item in metadata["vertex_inputs"]},
                                 {int(value) for value in re.findall(r"\[\[\s*attribute\((\d+)\)\s*\]\]", code)})


if __name__ == "__main__":
    unittest.main()
