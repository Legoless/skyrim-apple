#!/usr/bin/env python3
"""Exercise the native renderer's real decoder, mixer and output status."""
import ctypes as C
import os
from pathlib import Path
import struct
import threading
import unittest


LIBRARY = Path(os.environ.get("SKY_AUDIO_RENDERER_LIB", Path(__file__).resolve().parents[1] /
    "build/audio-renderer/SkyAudioRenderer.dylib"))
REVISION = 0x36564552
PCM_CALLBACK = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_int16), C.c_uint32, C.c_uint32)
EVENT_CALLBACK = C.CFUNCTYPE(None, C.c_void_p, C.c_int)


class Renderer:
    def __init__(self, library, adpcm=False, center=False):
        self.library = library
        self.samples = []
        self.ready = threading.Event()
        self.signaled = threading.Event()
        self.frame_count = 0
        self.handle = None
        self.pool = C.c_void_p()
        self.libc = C.CDLL(None)
        self.libc.posix_memalign.argtypes = [C.POINTER(C.c_void_p), C.c_size_t, C.c_size_t]
        self.libc.free.argtypes = [C.c_void_p]
        if self.libc.posix_memalign(C.byref(self.pool), 4096, 0x4000):
            raise MemoryError()
        C.memset(self.pool, 0, 0x4000)

        def output(context, data, count, channels):
            assert channels == 2
            self.samples.extend(data[:count])

        def frame_event(context, signaled):
            if signaled:
                self.signaled.set()
                self.frame_count += 1
                if self.frame_count >= 4:
                    self.ready.set()

        self.pcm_callback = PCM_CALLBACK(output)
        self.event_callback = EVENT_CALLBACK(frame_event)
        parameter = C.create_string_buffer(0x38)
        for offset, value in [(0, 48000), (4, 240), (8, 6 if center else 2), (0x10, 1), (0x14, 1), (0x34, REVISION)]:
            struct.pack_into("<I", parameter, offset, value)
        self.handle = library.sky_audren_create(parameter, self.pcm_callback, self.event_callback, None)
        if not self.handle:
            self.close()
            raise RuntimeError("native renderer initialization failed")
        self.make_input(adpcm, center)
        self.output = C.create_string_buffer(4096)

    def make_input(self, adpcm, center):
        header = bytearray(64)
        behavior = bytearray(16)
        pools = bytearray(128)
        channel = bytearray(112)
        voice = bytearray(368)
        mix = bytearray(2352)
        sink = bytearray(320)
        performance = bytearray(16)
        struct.pack_into("<I", behavior, 0, REVISION)
        struct.pack_into("<QQI", pools, 0, self.pool.value, 0x4000, 4)
        struct.pack_into("<Iff", channel, 0, 0, 1., 1.)
        if center:
            struct.pack_into("<fff", channel, 4, 0., 0., 1.)
        channel[100] = 1
        struct.pack_into("<II4BIIIIff", voice, 0,
            0, 0x10000000, 1, 1, 0, 6 if adpcm else 2, 48000, 0, 0, 1, 1., 1.)
        struct.pack_into("<I", voice, 0x3c, 1)
        struct.pack_into("<iI", voice, 0x58, 0, 0xffffffff)
        sample_count = 14 if adpcm else 480
        data_size = 8 if adpcm else sample_count * 2
        if adpcm:
            # GC-ADPCM predictor zero, scale 2**8, all nibbles seven. With
            # zero coefficients every decoded input sample equals 1792.
            C.memmove(self.pool, bytes([8] + [0x77] * 7), 8)
            struct.pack_into("<QQ", voice, 0x48, self.pool.value + 0x1000, 32)
        else:
            samples = (C.c_int16 * sample_count).from_address(self.pool.value)
            for index in range(sample_count):
                samples[index] = 1000
        struct.pack_into("<QQIIBB", voice, 0x60, self.pool.value, data_size, 0, sample_count, 0, 1)
        for index in range(1, 4):
            voice[0x60 + index * 0x38 + 26] = 1
        channels = 6 if center else 2
        struct.pack_into("<fII", mix, 0, 1., 48000, channels)
        mix[12] = 1
        struct.pack_into("<iI", mix, 2340, 0x7fffffff, 0xffffffff)
        sink[0] = sink[1] = 1
        sink[32:45] = b"MainAudioOut\0"
        struct.pack_into("<I", sink, 288, channels)
        sink[292:292 + channels] = bytes(range(channels))
        sections = [behavior, pools, channel, voice, mix, sink, performance]
        struct.pack_into("<9I", header, 0, REVISION, 16, 128, 368, 112, 0, 2352, 320, 16)
        struct.pack_into("<I", header, 60, 64 + sum(map(len, sections)))
        self.input = C.create_string_buffer(bytes(header) + b"".join(sections))
        self.voice_offset = 64 + 16 + 128 + 112

    def update(self):
        return self.library.sky_audren_update(self.handle, self.output, len(self.output), None, 0,
            self.input, len(self.input) - 1)

    def close(self):
        if self.handle:
            self.library.sky_audren_destroy(self.handle)
            self.handle = None
        if self.pool.value:
            self.libc.free(self.pool)
            self.pool = C.c_void_p()


class NativeAudioRendererTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not LIBRARY.is_file():
            raise AssertionError(f"native renderer must be built: {LIBRARY}")
        cls.library = C.CDLL(str(LIBRARY))
        cls.library.sky_audren_create.argtypes = [C.c_void_p, PCM_CALLBACK, EVENT_CALLBACK, C.c_void_p]
        cls.library.sky_audren_create.restype = C.c_void_p
        cls.library.sky_audren_update.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p,
            C.c_size_t, C.c_void_p, C.c_size_t]
        cls.library.sky_audren_update.restype = C.c_uint32
        for name in ("start", "stop"):
            function = getattr(cls.library, f"sky_audren_{name}")
            function.argtypes = [C.c_void_p]
            function.restype = C.c_uint32
        cls.library.sky_audren_destroy.argtypes = [C.c_void_p]

    def render_and_verify(self, adpcm, minimum, maximum, center=False):
        renderer = Renderer(self.library, adpcm, center)
        self.addCleanup(renderer.close)
        self.assertEqual(renderer.update(), 0)
        self.assertEqual(struct.unpack_from("<I", renderer.output, 64)[0], 5)  # Pool attached.
        self.assertEqual(self.library.sky_audren_start(renderer.handle), 0)
        self.assertTrue(renderer.ready.wait(2), "actual DSP frames did not complete")
        self.assertEqual(self.library.sky_audren_stop(renderer.handle), 0)
        self.assertGreater(len(renderer.samples), 0)
        self.assertGreaterEqual(max(renderer.samples), minimum)
        self.assertLessEqual(max(renderer.samples), maximum)
        self.assertEqual(renderer.samples[::2], renderer.samples[1::2])
        # Send the next client update and verify real consumed-buffer feedback.
        renderer.input[renderer.voice_offset + 8] = 0
        renderer.input[renderer.voice_offset + 0x60 + 26] = 1
        self.assertEqual(renderer.update(), 0)
        played, consumed = struct.unpack_from("<QI", renderer.output, 128)
        # End-of-stream resets the played-sample counter after consumption.
        self.assertEqual(played, 0)
        self.assertEqual(consumed, 1)

    def test_pcm16_mixing_and_released_wave_status(self):
        self.render_and_verify(False, 950, 1000)

    def test_gc_adpcm_decoding_and_released_wave_status(self):
        self.render_and_verify(True, 1700, 1792)

    def test_surround_center_dialogue_is_downmixed_to_stereo(self):
        self.render_and_verify(False, 600, 800, center=True)

    def test_malformed_renderer_sections_are_rejected(self):
        renderer = Renderer(self.library)
        self.addCleanup(renderer.close)
        struct.pack_into("<I", renderer.input, 8, 32)
        self.assertNotEqual(renderer.update(), 0)
        self.assertFalse(renderer.samples)

    def test_dsp_failure_wakes_frame_waiter_and_returns_error(self):
        renderer = Renderer(self.library)
        self.addCleanup(renderer.close)
        self.assertEqual(renderer.update(), 0)
        # Retain an already valid wave buffer while switching the voice to the
        # unsupported PCM8 decoder. The update accepts its state; command
        # generation on the DSP thread then raises the real failure.
        renderer.input[renderer.voice_offset + 8] = 0
        renderer.input[renderer.voice_offset + 11] = 1
        renderer.input[renderer.voice_offset + 0x60 + 26] = 1
        self.assertEqual(renderer.update(), 0)
        self.assertEqual(self.library.sky_audren_start(renderer.handle), 0)
        self.assertTrue(renderer.signaled.wait(2), "DSP failure left its frame waiter asleep")
        self.assertEqual(renderer.update(), 0x2a2)
        self.assertFalse(renderer.samples)

    def test_consumed_pool_detaches_after_renderer_stop(self):
        renderer = Renderer(self.library)
        self.addCleanup(renderer.close)
        self.assertEqual(renderer.update(), 0)
        self.assertEqual(self.library.sky_audren_start(renderer.handle), 0)
        self.assertTrue(renderer.ready.wait(2))
        self.assertEqual(self.library.sky_audren_stop(renderer.handle), 0)
        # Stop using the voice and request detach of the pool used by the last
        # completed frame. No future frame is available to clear stale usage.
        renderer.input[renderer.voice_offset + 8] = 0
        renderer.input[renderer.voice_offset + 9] = 0
        struct.pack_into("<I", renderer.input, 64 + 16 + 16, 2)
        self.assertEqual(renderer.update(), 0)
        self.assertEqual(struct.unpack_from("<I", renderer.output, 64)[0], 3)


if __name__ == "__main__":
    unittest.main()
