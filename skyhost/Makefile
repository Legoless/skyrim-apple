CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
OBJCFLAGS ?= -O2 -g
OBJCFLAGS += -std=c11 -Wall -Wextra -Werror -fobjc-arc
CPPFLAGS += -D_DARWIN_C_SOURCE
NVN_LIBS := -framework Metal -framework Foundation -framework QuartzCore
WINDOW_LIBS := -framework AppKit -framework CoreGraphics
AUDIO_LIBS := -framework AudioUnit -framework AudioToolbox

BUILD := build
NVN_CORE_OBJECTS := $(BUILD)/nvn_program.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_transfer.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o
TARGET := skyhost
SOURCES := main.c loader.c gates.c support.c profile.c runtime.c thread.c svc.c crash.c startup.c keyboard.c fs.c net.c account.c bcat.c nfp.c hid.c audio.c audio_backend.c event.c vi.c nvn.c nvn_metal.m nvn_transfer.m nvn_bc.c macos_window.m input.c input_control.c nv.c entry.S
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(SOURCES))) $(patsubst %.m,$(BUILD)/%.o,$(filter %.m,$(SOURCES))) $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SOURCES))) $(BUILD)/nvn_program.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o
SKY_WORK_DIR ?= $(abspath ../skyrim-work)
SKY_REWRITE_DIR ?= $(SKY_WORK_DIR)/build/rewrite-native
SKY_META_DIR ?= $(if $(wildcard $(SKY_WORK_DIR)/data/switch/skyrim/base/meta/relocations.tsv),$(SKY_WORK_DIR)/data/switch/skyrim/base/meta,$(if $(wildcard $(SKY_WORK_DIR)/build/inventory-from-rewritten/meta/relocations.tsv),$(SKY_WORK_DIR)/build/inventory-from-rewritten/meta,$(SKY_WORK_DIR)/data/switch/skyrim/base/meta))
SKY_ROMFS ?= $(SKY_WORK_DIR)/data/switch/skyrim/base/Update/RomFS
SKY_RUN_DIR ?= $(SKY_WORK_DIR)/run
SKY_SHADER_CACHE_DIR ?= $(abspath ../skyrim-work/build/shaders)
export SKY_SHADER_CACHE_DIR
AUDIO_NATIVE := $(BUILD)/audio-renderer/SkyAudioRenderer.dylib
AUDIO_MANAGED_SOURCES := $(shell find audio_renderer -type f \( -name '*.cs' -o -name '*.csproj' \) -not -path '*/obj/*' -not -path '*/bin/*')

all: $(TARGET)

$(TARGET): $(OBJECTS) $(AUDIO_NATIVE)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(NVN_LIBS) $(WINDOW_LIBS) $(AUDIO_LIBS)

$(AUDIO_NATIVE): $(AUDIO_MANAGED_SOURCES) | $(BUILD)
	dotnet publish audio_renderer/SkyAudioRenderer.csproj -c Release -r osx-arm64 -o $(BUILD)/audio-renderer -v minimal

$(BUILD)/%.o: %.c skyhost.h profile.h runtime.h thread.h svc.h crash.h fs.h net.h startup.h account.h bcat.h nfp.h hid.h audio.h event.h vi.h nvn.h nvn_metal.h nv.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m nvn_metal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -c -o $@ $<

$(BUILD)/nvn.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_transfer.o: nvn_transfer.h
$(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o: nvn_bc.h
$(BUILD)/nvn_bc.o: third_party/bcdec/bcdec.h
$(BUILD)/main.o $(BUILD)/macos_window.o: macos_window.h
$(BUILD)/main.o $(BUILD)/input_control.o: input_control.h
$(BUILD)/macos_window.o $(BUILD)/input.o: input.h
$(BUILD)/macos_window.o: hid.h startup.h
$(BUILD)/macos_window.o $(BUILD)/startup.o $(BUILD)/keyboard.o: keyboard.h
$(BUILD)/audio.o $(BUILD)/audio_backend.o: audio_backend.h

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/nvn_tex.o: nvn/nvn_tex.c nvn/nvn_tex.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ nvn/nvn_tex.c

$(BUILD)/nvn_deswizzle.o: nvn/nvn_deswizzle.c nvn/nvn_deswizzle.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ nvn/nvn_deswizzle.c

$(BUILD)/nvn_fmt_mtl.o: nvn/nvn_fmt_mtl.c nvn/nvn_fmt_mtl.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ nvn/nvn_fmt_mtl.c

$(BUILD):
	mkdir -p $@

$(BUILD)/t_reloc: tests/t_reloc.c loader.c gates.c svc.c support.c profile.c thread.c entry.S tests/crash_stub.c skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_reloc.c gates.c svc.c support.c profile.c thread.c entry.S tests/crash_stub.c

$(BUILD)/t_gate_abort: tests/t_gate_abort.c gates.c svc.c support.c profile.c thread.c entry.S tests/crash_stub.c tests/globals_stub.c skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_gate_abort.c gates.c svc.c support.c profile.c thread.c entry.S tests/crash_stub.c tests/globals_stub.c

$(BUILD)/t_runtime: tests/t_runtime.c thread.c svc.c gates.c support.c profile.c entry.S tests/crash_stub.c skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_runtime.c thread.c svc.c gates.c support.c profile.c entry.S tests/crash_stub.c

$(BUILD)/t_fs: tests/t_fs.c fs.c support.c profile.c profile.h skyhost.h fs.h account.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_fs.c support.c profile.c

$(BUILD)/t_nifm: tests/t_nifm.c net.c support.c skyhost.h net.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nifm.c net.c support.c

$(BUILD)/t_aoc: tests/t_aoc.c startup.c keyboard.c keyboard.h event.c event.h support.c skyhost.h startup.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_aoc.c startup.c keyboard.c event.c support.c

$(BUILD)/t_account: tests/t_account.c account.c support.c skyhost.h account.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_account.c account.c support.c

$(BUILD)/t_startup: tests/t_startup.c startup.c keyboard.c keyboard.h support.c skyhost.h startup.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_startup.c startup.c keyboard.c support.c

$(BUILD)/t_bcat: tests/t_bcat.c bcat.c support.c skyhost.h bcat.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_bcat.c bcat.c support.c

$(BUILD)/t_nfp: tests/t_nfp.c nfp.c support.c skyhost.h nfp.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nfp.c nfp.c support.c

$(BUILD)/t_hid: tests/t_hid.c hid.c support.c profile.c profile.h skyhost.h hid.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_hid.c hid.c support.c profile.c

$(BUILD)/t_audio: tests/t_audio.c audio.c audio_backend.c event.c support.c skyhost.h audio.h audio_backend.h event.h $(AUDIO_NATIVE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_audio.c audio.c audio_backend.c event.c support.c $(AUDIO_LIBS)

$(BUILD)/t_vi: tests/t_vi.c vi.c support.c skyhost.h vi.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_vi.c vi.c support.c

$(BUILD)/t_nvn: tests/t_nvn.c nvn.c vi.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h vi.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn.c nvn.c vi.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_pool: tests/t_nvn_pool.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_pool.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_descriptors: tests/t_nvn_descriptors.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_descriptors.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_resource_batch: tests/t_nvn_resource_batch.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ $< support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_texture: tests/t_nvn_texture.c nvn.c vi.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h vi.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_texture.c nvn.c vi.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_sync: tests/t_nvn_sync.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_sync.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_views: tests/t_nvn_views.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_views.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_commands: tests/t_nvn_commands.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_commands.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/bench_nvn_pipeline: tests/bench_nvn_pipeline.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/bench_nvn_pipeline.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_nvn_program: tests/t_nvn_program.c nvn.c support.c tests/crash_stub.c skyhost.h nvn.h nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_program.c nvn.c support.c tests/crash_stub.c $(NVN_CORE_OBJECTS) $(NVN_LIBS)

$(BUILD)/t_transfer: tests/t_transfer.c thread.c svc.c gates.c support.c profile.c entry.S tests/crash_stub.c skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_transfer.c thread.c svc.c gates.c support.c profile.c entry.S tests/crash_stub.c

$(BUILD)/t_nv: tests/t_nv.c nv.c support.c skyhost.h nv.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nv.c nv.c support.c

$(BUILD)/t_input: tests/t_input.c input.c input.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_input.c input.c

$(BUILD)/t_touch: tests/t_touch.c touch.c touch.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_touch.c touch.c -lm

$(BUILD)/t_keyboard: tests/t_keyboard.c keyboard.c keyboard.h startup.c support.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_keyboard.c keyboard.c startup.c support.c

$(BUILD)/t_macos_input: tests/t_macos_input.m macos_window.m input.c input.h hid.c hid.h startup.c keyboard.c keyboard.h event.c support.c profile.c profile.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. -o $@ tests/t_macos_input.m input.c hid.c startup.c keyboard.c event.c support.c profile.c $(NVN_LIBS) $(WINDOW_LIBS)

$(BUILD)/t_input_control: tests/t_input_control.c input_control.c input_control.h hid.c hid.h support.c profile.c profile.h skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_input_control.c input_control.c hid.c support.c profile.c

$(BUILD)/t_support: tests/t_support.c support.c skyhost.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_support.c support.c

$(BUILD)/t_havok_observer: tests/t_havok_observer.c runtime.c thread.c svc.c gates.c support.c profile.c entry.S crash.c tests/globals_stub.c skyhost.h runtime.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_havok_observer.c thread.c svc.c gates.c support.c profile.c entry.S crash.c tests/globals_stub.c

$(BUILD)/t_nvn_transfer: tests/t_nvn_transfer.m nvn_transfer.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. tests/t_nvn_transfer.m $(BUILD)/nvn_transfer.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o $(NVN_LIBS) -o $@

$(BUILD)/t_nvn_bc: tests/t_nvn_bc.c nvn_bc.h $(BUILD)/nvn_bc.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/t_nvn_bc.c $(BUILD)/nvn_bc.o

$(BUILD)/t_nvn_bc_metal: tests/t_nvn_bc_metal.m nvn_metal.h nvn_bc.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. $< $(BUILD)/nvn_transfer.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o $(NVN_LIBS) -o $@

$(BUILD)/t_nvn_render_state: tests/t_nvn_render_state.m nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. $< $(BUILD)/nvn_transfer.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o $(NVN_LIBS) -o $@

$(BUILD)/t_nvn_buffer_lookup: tests/t_nvn_buffer_lookup.m nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. $< $(BUILD)/nvn_transfer.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o $(NVN_LIBS) -o $@

$(BUILD)/t_nvn_memory_mapping: tests/t_nvn_memory_mapping.m nvn_metal.h $(NVN_CORE_OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(OBJCFLAGS) -I. $< $(BUILD)/nvn_transfer.o $(BUILD)/nvn_metal.o $(BUILD)/nvn_bc.o $(BUILD)/nvn_tex.o $(BUILD)/nvn_deswizzle.o $(BUILD)/nvn_fmt_mtl.o $(NVN_LIBS) -o $@

test-unit: all $(BUILD)/t_reloc $(BUILD)/t_gate_abort $(BUILD)/t_runtime $(BUILD)/t_fs $(BUILD)/t_nifm $(BUILD)/t_aoc $(BUILD)/t_account $(BUILD)/t_startup $(BUILD)/t_bcat $(BUILD)/t_nfp $(BUILD)/t_nvn_transfer $(BUILD)/t_hid $(BUILD)/t_audio $(BUILD)/t_vi $(BUILD)/t_nvn $(BUILD)/t_nvn_pool $(BUILD)/t_nvn_descriptors $(BUILD)/t_nvn_texture $(BUILD)/t_nvn_sync $(BUILD)/t_nvn_views $(BUILD)/t_nvn_commands $(BUILD)/t_nvn_program $(BUILD)/t_transfer $(BUILD)/t_nv $(BUILD)/t_input $(BUILD)/t_input_control $(BUILD)/t_support $(BUILD)/t_havok_observer $(BUILD)/t_nvn_buffer_lookup $(BUILD)/t_nvn_memory_mapping $(BUILD)/t_touch $(BUILD)/t_macos_input $(BUILD)/t_keyboard
	$(BUILD)/t_input
	$(BUILD)/t_touch
	$(BUILD)/t_keyboard
	$(BUILD)/t_macos_input
	$(BUILD)/t_input_control
	$(BUILD)/t_support
	$(BUILD)/t_nvn_resource_batch
	python3 tests/test_ios_signed_loader.py -v
	python3 -m unittest discover -s audio_renderer/tests -v
	$(BUILD)/t_havok_observer
	MTL_DEBUG_LAYER=1 $(BUILD)/t_nvn_transfer
	$(BUILD)/t_nvn_bc
	MTL_DEBUG_LAYER=1 $(BUILD)/t_nvn_bc_metal
	MTL_DEBUG_LAYER=1 $(BUILD)/t_nvn_render_state
	MTL_DEBUG_LAYER=1 $(BUILD)/t_nvn_buffer_lookup
	MTL_DEBUG_LAYER=1 $(BUILD)/t_nvn_memory_mapping
	python3 tests/test_shaders.py -v
	python3 tests/test_shader_archive.py -v
	python3 tests/test_shader_embedded.py -v
	python3 tests/test_rebuild_shaders.py -v
	python3 tests/test_audio_renderer.py -v
	SKYHOST="$(abspath $(TARGET))" SKYHOST_BUILD="$(abspath $(BUILD))" \
	SKY_REWRITE_DIR="$(SKY_REWRITE_DIR)" SKY_META_DIR="$(SKY_META_DIR)" \
	python3 tests/test_map_only.py -v

test-unit: $(BUILD)/t_nvn_render_state $(BUILD)/t_nvn_resource_batch $(BUILD)/t_nvn_bc $(BUILD)/t_nvn_bc_metal

test: test-unit
	SKYHOST="$(abspath $(TARGET))" SKYHOST_BUILD="$(abspath $(BUILD))" \
	SKY_REWRITE_DIR="$(SKY_REWRITE_DIR)" SKY_RUN_DIR="$(SKY_RUN_DIR)" \
	SKY_ROMFS="$(SKY_ROMFS)" python3 tests/test_runtime.py -v
	SKYHOST="$(abspath $(TARGET))" SKYHOST_BUILD="$(abspath $(BUILD))" \
	SKY_REWRITE_DIR="$(SKY_REWRITE_DIR)" SKY_RUN_DIR="$(SKY_RUN_DIR)" \
	SKY_ROMFS="$(SKY_ROMFS)" python3 tests/test_saveload.py -v

test-ci: all $(BUILD)/t_reloc $(BUILD)/t_gate_abort $(BUILD)/t_runtime $(BUILD)/t_fs $(BUILD)/t_nifm $(BUILD)/t_aoc $(BUILD)/t_account $(BUILD)/t_startup $(BUILD)/t_bcat $(BUILD)/t_nfp $(BUILD)/t_nvn_transfer $(BUILD)/t_hid $(BUILD)/t_audio $(BUILD)/t_vi $(BUILD)/t_nvn $(BUILD)/t_nvn_pool $(BUILD)/t_nvn_descriptors $(BUILD)/t_nvn_texture $(BUILD)/t_nvn_sync $(BUILD)/t_nvn_views $(BUILD)/t_nvn_commands $(BUILD)/t_nvn_program $(BUILD)/t_transfer $(BUILD)/t_nv $(BUILD)/t_input $(BUILD)/t_input_control $(BUILD)/t_support $(BUILD)/t_havok_observer $(BUILD)/t_nvn_buffer_lookup $(BUILD)/t_nvn_memory_mapping $(BUILD)/t_touch $(BUILD)/t_macos_input $(BUILD)/t_keyboard $(BUILD)/t_nvn_render_state $(BUILD)/t_nvn_resource_batch $(BUILD)/t_nvn_bc $(BUILD)/t_nvn_bc_metal
	$(BUILD)/t_input
	$(BUILD)/t_touch
	$(BUILD)/t_keyboard
	$(BUILD)/t_macos_input
	$(BUILD)/t_input_control
	$(BUILD)/t_support
	$(BUILD)/t_nvn_resource_batch
	python3 tests/test_ios_signed_loader.py -v
	python3 -m unittest discover -s audio_renderer/tests -v
	$(BUILD)/t_havok_observer
	$(BUILD)/t_nvn_bc
	python3 tests/test_shaders.py -v
	python3 tests/test_shader_archive.py -v
	python3 tests/test_shader_embedded.py -v
	python3 tests/test_rebuild_shaders.py -v
	python3 tests/test_audio_renderer.py -v
	SKYHOST="$(abspath $(TARGET))" SKYHOST_BUILD="$(abspath $(BUILD))" \
	python3 -m unittest tests.test_map_only.SyntheticHostTests -v

clean:
	rm -rf $(BUILD) $(TARGET)

.PHONY: all test test-unit test-ci clean
