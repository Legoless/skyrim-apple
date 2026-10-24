/* Compiled with a tiny generated Mach-O image by test_ios_signed_loader.py.
 * Include the production mapper so its remapping and validation are exercised. */
#include "../loader.c"
#include <assert.h>

static Elf64_Dyn dynamic_table[3];

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const char *mode = argv[1];
    module_t module = {0};
    strcpy(module.name, "main");
    read_layout(&module, argv[2]);
    if (!strcmp(mode, "overflow-layout")) {
        module.file_size = 0x10010;
        module.key_page_off = UINT64_MAX - 0x3fff;
        validate_layout(&module);
        return 3;
    }
    if (!strcmp(mode, "section-mismatch")) {
        module.text_end = 0x4004;
        module.ro_off = 0x5000;
    }
    create_keys();
    map_image(&module, argv[2]);
    assert(module.file_size == 0x10010 && module.size == 0x14000);
    unsigned long source_bytes = 0;
    const uint8_t *source = getsectiondata(&_mh_execute_header, "__DMmainW", "__img", &source_bytes);
    assert(source && source_bytes == 0xc000);
    assert(!memcmp(source + 0x4000, "ORIGINAL", 8));
    assert(!memcmp(module.base + 0x8000, "ORIGINAL", 8));
    memcpy(module.base + 0x8000, "MUTATED!", 8);
    assert(!memcmp(source + 0x4000, "ORIGINAL", 8));
    assert(((uint64_t (*)(void))module.base)() == 0x1234);
    assert(((uint64_t (*)(void))(module.base + module.patch_off))() == 0x2345);
    uint64_t *keys = (uint64_t *)(module.base + module.key_page_off);
    for (unsigned index = 0; index < KEY_N; index++)
        assert(keys[module.key_idx[index]] == (uint64_t)key_value(index) * 8);

    dynamic_table[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = 0x8200};
    dynamic_table[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = sizeof(Elf64_Rela)};
    module.dyn = dynamic_table;
    Elf64_Rela *relocation = (Elf64_Rela *)(module.base + 0x8200);
    *relocation = (Elf64_Rela){.r_offset = 0x8100, .r_info = R_AARCH64_RELATIVE, .r_addend = 0x88};
    if (!strcmp(mode, "text-reloc")) relocation->r_offset = 0;
    else if (!strcmp(mode, "ro-reloc")) relocation->r_offset = 0x4000;
    else if (!strcmp(mode, "key-reloc")) relocation->r_offset = 0xc000;
    else if (!strcmp(mode, "patch-reloc")) relocation->r_offset = 0x10000;
    else if (strcmp(mode, "success")) return 2;
    relocate(&module);
    assert(*(uint64_t *)(module.base + 0x8100) == (uintptr_t)(module.base + 0x88));
    assert(!memcmp(source + 0x4100, "\0\0\0\0\0\0\0\0", 8));
    assert(vm_deallocate(mach_task_self(), (vm_address_t)module.base, module.size) == KERN_SUCCESS);
    puts("T_SIGNED_LOADER OK embedded-rx-execution cow-data ro-keypage relative-relocation");
    return 0;
}
