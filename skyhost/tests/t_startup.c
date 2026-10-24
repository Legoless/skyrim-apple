#include "startup.h"
#include "skyhost.h"
#include <assert.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#include <unistd.h>

static void test_offline_shop_abi(void) {
    typedef struct { const char *name, *why; void *function; } force_entry_t;
    typedef void (*show_shop_fn)(uint64_t, const void *);
    extern const struct mach_header_64 _mh_execute_header;
    const char *symbol = "_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE";
    unsigned long size = 0;
    const force_entry_t *entries = (const force_entry_t *)getsectiondata(
        &_mh_execute_header, "__DATA", "swforce", &size);
    assert(entries && size % sizeof *entries == 0);
    show_shop_fn show = NULL;
    for (unsigned long index = 0; index < size / sizeof *entries; index++) {
        if (strcmp(entries[index].name, symbol)) continue;
        assert(!show);
        show = (show_shop_fn)entries[index].function;
    }
    assert(show);

    struct {
        uint64_t before, handle[3], after;
    } user = {UINT64_C(0x1122334455667788), {1, 2, UINT64_C(0xfedcba9876543210)},
              UINT64_C(0x8877665544332211)}, original = user;
    const uint64_t application_id = UINT64_C(0x123456789abcdef0);
    void *unreadable = mmap(NULL, SKY_PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(unreadable != MAP_FAILED);
    FILE *diagnostic = tmpfile();
    assert(diagnostic);
    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0 && dup2(fileno(diagnostic), STDERR_FILENO) >= 0);

    /* No SDK/AM implementation is linked into this focused test. A protected
     * opaque handle also proves that the offline boundary does not inspect it. */
    show(application_id, user.handle);
    assert(!memcmp(&user, &original, sizeof user));
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_ec_shop_unavailable"));
    show(UINT64_MAX, unreadable);
    show(0, NULL);
    for (unsigned index = 0; index < 256; index++) show(application_id, user.handle);
    assert(!memcmp(&user, &original, sizeof user));
    assert(sky_oe_get_current_focus_state() == 1 && sky_oe_is_startup_logo_finished());
    assert(!sky_oe_exit_request_handling_depth());
    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0 && close(saved_stderr) == 0);
    assert(fseek(diagnostic, 0, SEEK_SET) == 0);
    char text[1024];
    size_t length = fread(text, 1, sizeof text - 1, diagnostic);
    assert(length < sizeof text - 1 && !ferror(diagnostic));
    text[length] = 0;
    assert(fclose(diagnostic) == 0);
    const char *logged = strstr(text, "EC_SHOP_UNAVAILABLE application_id=0x123456789abcdef0");
    assert(logged && !strstr(logged + 1, "EC_SHOP_UNAVAILABLE"));
    assert(strstr(text, "mode=offline action=closed no-applet no-network"));
    assert(!strstr(text, "UNIMPL") && !strstr(text, "RUN_CRASH"));
    assert(munmap(unreadable, SKY_PAGE_SIZE) == 0);
    puts("T_EC_SHOP OK exact-void-ABI opaque-handle-preserved repeated-return no-AM bounded-offline-log");
}

int main(void) {
    sky_oe_initialize();
    assert(!sky_oe_is_startup_logo_finished());
    assert(sky_oe_get_current_focus_state() == 1);
    uint32_t notification = 0xdeadbeef;
    assert(!sky_oe_try_pop_notification_message(&notification));
    assert(notification == 0xdeadbeef);
    sky_oe_set_host_focus(0);
    assert(sky_oe_get_current_focus_state() == 2);
    assert(!sky_oe_try_pop_notification_message(NULL));
    assert(sky_oe_try_pop_notification_message(&notification));
    assert(notification == 15);
    assert(!sky_oe_try_pop_notification_message(&notification));
    sky_oe_set_host_focus(0);
    assert(!sky_oe_try_pop_notification_message(&notification));
    sky_oe_set_host_focus(1);
    assert(sky_oe_get_current_focus_state() == 1);
    assert(sky_oe_try_pop_notification_message(&notification) && notification == 15);
    assert(!sky_oe_exit_request_handling_depth());
    sky_oe_enter_exit_request_handling_section();
    sky_oe_enter_exit_request_handling_section();
    assert(sky_oe_exit_request_handling_depth() == 2);
    sky_oe_leave_exit_request_handling_section();
    assert(sky_oe_exit_request_handling_depth() == 1);
    sky_oe_leave_exit_request_handling_section();
    assert(!sky_oe_exit_request_handling_depth());
    sky_oe_finish_startup_logo();
    assert(sky_oe_is_startup_logo_finished());
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_oe_startup_logo_finished"));
    sky_oe_initialize();
    assert(sky_oe_is_startup_logo_finished());
    assert(sky_time_initialize() == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_time_initialize"));
    int64_t current_time = 0;
    assert(sky_time_get_current_time(&current_time) == 0 && current_time > 1700000000LL);
    assert(sky_time_now() >= current_time);
    sky_time_calendar_time_t cal = {0};
    sky_time_calendar_additional_info_t info = {0};
    assert(sky_time_to_calendar_time(&cal, &info, &current_time) == 0);
    assert(cal.year >= 2024 && cal.year <= 9999);
    assert(cal.month >= 1 && cal.month <= 12);
    assert(cal.day >= 1 && cal.day <= 31);
    char prefix[32];
    int prefix_len = snprintf(prefix, sizeof prefix, "%04d%02d%02d%02d%02d%02d",
                              cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second);
    assert(prefix_len == 14);
    int out_count = 0;
    int64_t roundtrip_time = 0;
    assert(sky_time_to_posix_time(&out_count, &roundtrip_time, 1, &cal) == 0);
    assert(out_count == 1 && roundtrip_time == current_time);
    assert(sky_time_finalize() == 0);
    sky_oe_set_focus_handling_mode(1);
    sky_oe_set_performance_mode_changed_notification_enabled(1);
    sky_oe_set_performance_configuration(0, 0x20004);
    assert(sky_oe_get_operation_mode() == 1);
    assert(sky_oe_get_performance_mode() == 0);
    assert(sky_oe_get_performance_configuration(0) == 0x20004);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_oe_defaults"));
    uint64_t packed_language = sky_oe_get_desired_language();
    char language[8];
    memcpy(language, &packed_language, sizeof language);
    assert(!strcmp(language, "en-US"));
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_oe_language"));
    test_offline_shop_abi();
    puts("T_STARTUP OK host-time fixed-oe stored-performance-config language-en-US");
    return 0;
}
