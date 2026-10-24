#include "startup.h"
#include "skyhost.h"
#include "keyboard.h"

#include <limits.h>
#include <stdatomic.h>
#include <time.h>

static pthread_mutex_t g_oe_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_oe_initialized;
static int g_startup_logo_finished;
static int g_focus_state = 1;
static int g_focus_notification_pending;
static unsigned g_exit_request_handling_depth;
static atomic_int g_shop_unavailable_logged;

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

SKY_FORCE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE",
          "Skyrim software keyboard uses the synchronous native text presenter",
          uint32_t, sky_keyboard_show_entry, void *out, const void *arg) {
    return sky_keyboard_show(out,arg);
}

void sky_ec_show_shop_add_on_content_list(uint64_t application_id, const void *user_handle) {
    (void)user_handle;
    /* This public SDK overload returns void. Close the unavailable offline
     * applet request before the original SDK enters Nintendo AM service code. */
    sky_checkpoint("post_nnMain_ec_shop_unavailable");
    if (!atomic_exchange_explicit(&g_shop_unavailable_logged, 1, memory_order_relaxed))
        LOG("EC_SHOP_UNAVAILABLE application_id=%#llx mode=offline action=closed no-applet no-network",
            (unsigned long long)application_id);
}

SKY_FORCE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE",
          "Close the unavailable offline eShop request without Nintendo AM, network access or purchases",
          void, sky_ec_show_shop_add_on_content_list_entry, uint64_t application_id, const void *user_handle) {
    sky_ec_show_shop_add_on_content_list(application_id, user_handle);
}

int sky_aoc_count_add_on_content(void) {
    sky_checkpoint("post_nnMain_aoc_count");
    LOG("AOC_COUNT count=0 source=host-offline");
    return 0;
}

SKY_FORCE("nndiagStartup", "Skyrim diagnostics use the host stderr log instead of an lm IPC session",
          void, sky_nndiag_startup, void) {
    LOG("STARTUP_SERVICE nndiagStartup implementation=host-stderr");
}

void sky_oe_initialize(void) {
    pthread_mutex_lock(&g_oe_lock);
    g_oe_initialized = 1;
    pthread_mutex_unlock(&g_oe_lock);
    LOG("STARTUP_SERVICE nn::oe::Initialize implementation=host-defaults");
}

SKY_FORCE("_ZN2nn2oe10InitializeEv", "Skyrim operation-environment startup is host-managed without an am IPC session",
          void, sky_oe_initialize_entry, void) { sky_oe_initialize(); }

/* The SDK wrappers dispatch these through AM vtables. Since the host owns OE,
 * keep their lifecycle here as well; creating a partial AM interface object
 * would leave other SDK wrappers dereferencing an invalid service table.
 */
void sky_oe_finish_startup_logo(void) {
    pthread_mutex_lock(&g_oe_lock);
    if (!g_oe_initialized) {
        pthread_mutex_unlock(&g_oe_lock);
        die("nn::oe::FinishStartupLogo called before Initialize");
    }
    g_startup_logo_finished = 1;
    pthread_mutex_unlock(&g_oe_lock);
    sky_set_startup_logo_finished(1);
    sky_checkpoint("post_nnMain_oe_startup_logo_finished");
    LOG("OE_STARTUP_LOGO_FINISHED implementation=host-compositor");
}

int sky_oe_is_startup_logo_finished(void) {
    pthread_mutex_lock(&g_oe_lock);
    int result = g_startup_logo_finished;
    pthread_mutex_unlock(&g_oe_lock);
    return result;
}

int sky_oe_get_current_focus_state(void) {
    pthread_mutex_lock(&g_oe_lock);
    int result = g_focus_state;
    pthread_mutex_unlock(&g_oe_lock);
    return result;
}

void sky_oe_set_host_focus(int focused) {
    int state = focused ? 1 : 2;
    pthread_mutex_lock(&g_oe_lock);
    if (g_focus_state != state) {
        g_focus_state = state;
        g_focus_notification_pending = 1;
    }
    pthread_mutex_unlock(&g_oe_lock);
}

int sky_oe_try_pop_notification_message(uint32_t *message) {
    if (!message) return 0;
    pthread_mutex_lock(&g_oe_lock);
    int result = g_focus_notification_pending;
    if (result) {
        /* ICommonStateGetter::ReceiveMessage focus-state notification. */
        *message = 15;
        g_focus_notification_pending = 0;
    }
    pthread_mutex_unlock(&g_oe_lock);
    return result;
}

void sky_oe_enter_exit_request_handling_section(void) {
    pthread_mutex_lock(&g_oe_lock);
    if (g_exit_request_handling_depth == UINT_MAX) {
        pthread_mutex_unlock(&g_oe_lock);
        die("OE exit request handling section nesting overflow");
    }
    unsigned depth = ++g_exit_request_handling_depth;
    pthread_mutex_unlock(&g_oe_lock);
    LOG("OE_EXIT_REQUEST_HANDLING depth=%u", depth);
}

void sky_oe_leave_exit_request_handling_section(void) {
    pthread_mutex_lock(&g_oe_lock);
    if (!g_exit_request_handling_depth) {
        pthread_mutex_unlock(&g_oe_lock);
        die("OE exit request handling section nesting underflow");
    }
    unsigned depth = --g_exit_request_handling_depth;
    pthread_mutex_unlock(&g_oe_lock);
    LOG("OE_EXIT_REQUEST_HANDLING depth=%u", depth);
}

unsigned sky_oe_exit_request_handling_depth(void) {
    pthread_mutex_lock(&g_oe_lock);
    unsigned result = g_exit_request_handling_depth;
    pthread_mutex_unlock(&g_oe_lock);
    return result;
}

SKY_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "Finish the host startup lifecycle without dereferencing an uninitialized AM service",
          void, sky_oe_finish_startup_logo_entry, void) { sky_oe_finish_startup_logo(); }
SKY_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "Return the native host window focus state",
          int, sky_oe_get_current_focus_state_entry, void) { return sky_oe_get_current_focus_state(); }
SKY_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "Consume host operation-environment notifications without an AM IPC session",
          int, sky_oe_try_pop_notification_message_entry, uint32_t *message) {
    return sky_oe_try_pop_notification_message(message);
}
SKY_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "Track nested guest exit-request handling sections in the host",
          void, sky_oe_enter_exit_request_handling_section_entry, void) {
    sky_oe_enter_exit_request_handling_section();
}
SKY_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "Balance nested guest exit-request handling sections in the host",
          void, sky_oe_leave_exit_request_handling_section_entry, void) {
    sky_oe_leave_exit_request_handling_section();
}

SKY_FORCE("_ZN2nn3aoc10InitializeEv", "Skyrim add-on-content startup is host-managed and offline",
          void, sky_aoc_initialize, void) {
    LOG("STARTUP_SERVICE nn::aoc::Initialize implementation=host-offline");
}

SKY_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "No add-on content is mounted in the host-managed offline state",
          int, sky_aoc_count_entry, void) { return sky_aoc_count_add_on_content(); }

SKY_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "Skyrim error context is reported by host crash/checkpoint diagnostics",
          void, sky_err_initialize_context_writer, void) {
    LOG("STARTUP_SERVICE nn::err::InitializeContextWriterApi implementation=host-diagnostics");
}

SKY_FORCE("_ZN2nn4pctl10InitializeEv", "Skyrim parental-control startup is offline and host-managed",
          void, sky_pctl_initialize, void) {
    LOG("STARTUP_SERVICE nn::pctl::Initialize implementation=host-offline");
}

uint32_t sky_time_initialize(void) {
    sky_checkpoint("post_nnMain_time_initialize");
    LOG("TIME_INITIALIZE implementation=host-clock no-ipc");
    return 0;
}

uint32_t sky_time_finalize(void) {
    return 0;
}

uint32_t sky_time_get_current_time(int64_t *out_posix_time) {
    if (!out_posix_time) return 0x74u;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        *out_posix_time = (int64_t)ts.tv_sec;
    } else {
        *out_posix_time = 1760880000LL;
    }
    return 0;
}

int64_t sky_time_now(void) {
    int64_t value = 0;
    sky_time_get_current_time(&value);
    return value;
}

uint32_t sky_time_to_calendar_time(sky_time_calendar_time_t *out_cal,
                                   sky_time_calendar_additional_info_t *out_info,
                                   const int64_t *posix_time) {
    if (!out_cal || !posix_time) return 0x74u;
    time_t raw = (time_t)(*posix_time > 0 ? *posix_time : 0);
    struct tm local_tm;
    memset(&local_tm, 0, sizeof local_tm);
    if (!localtime_r(&raw, &local_tm)) {
        gmtime_r(&raw, &local_tm);
    }
    int year = local_tm.tm_year + 1900;
    if (year < 1970) year = 1970;
    if (year > 9999) year = 9999;
    out_cal->year = (int16_t)year;
    out_cal->month = (int8_t)(local_tm.tm_mon + 1);
    out_cal->day = (int8_t)(local_tm.tm_mday >= 1 ? local_tm.tm_mday : 1);
    out_cal->hour = (int8_t)(local_tm.tm_hour >= 0 ? local_tm.tm_hour : 0);
    out_cal->minute = (int8_t)(local_tm.tm_min >= 0 ? local_tm.tm_min : 0);
    out_cal->second = (int8_t)(local_tm.tm_sec >= 0 ? local_tm.tm_sec : 0);
    if (out_info) {
        memset(out_info, 0, sizeof *out_info);
        out_info->day_of_week = (uint32_t)(local_tm.tm_wday >= 0 ? local_tm.tm_wday : 0);
        out_info->day_of_year = (int32_t)(local_tm.tm_yday >= 0 ? local_tm.tm_yday : 0);
        if (local_tm.tm_zone) {
            snprintf(out_info->timezone_name, sizeof out_info->timezone_name, "%s", local_tm.tm_zone);
        } else {
            memcpy(out_info->timezone_name, "UTC", 4);
        }
        out_info->is_dst = local_tm.tm_isdst > 0 ? 1u : 0u;
        out_info->utc_offset_seconds = (int32_t)local_tm.tm_gmtoff;
    }
    return 0;
}

uint32_t sky_time_to_posix_time(int *out_count,
                                int64_t *out_times,
                                int count,
                                const sky_time_calendar_time_t *cal) {
    if (!cal) return 0x74u;
    struct tm local_tm;
    memset(&local_tm, 0, sizeof local_tm);
    int year = cal->year >= 1970 ? cal->year : 1970;
    int month = (cal->month >= 1 && cal->month <= 12) ? cal->month : 1;
    int day = (cal->day >= 1 && cal->day <= 31) ? cal->day : 1;
    local_tm.tm_year = year - 1900;
    local_tm.tm_mon = month - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = (cal->hour >= 0 && cal->hour <= 23) ? cal->hour : 0;
    local_tm.tm_min = (cal->minute >= 0 && cal->minute <= 59) ? cal->minute : 0;
    local_tm.tm_sec = (cal->second >= 0 && cal->second <= 60) ? cal->second : 0;
    local_tm.tm_isdst = -1;
    time_t converted = mktime(&local_tm);
    if (converted < 0) converted = 0;
    if (out_times && count > 0) {
        out_times[0] = (int64_t)converted;
    }
    if (out_count) {
        *out_count = (out_times && count > 0) ? 1 : 0;
    }
    return 0;
}

_Static_assert(sizeof(sky_time_calendar_time_t) == 8, "Switch CalendarTime size");
_Static_assert(sizeof(sky_time_calendar_additional_info_t) == 24, "Switch CalendarAdditionalInfo size");

SKY_FORCE("_ZN2nn4time10InitializeEv", "Initialize time using the host clock without an IPC time service",
          uint32_t, sky_time_initialize_entry, void) { return sky_time_initialize(); }
SKY_FORCE("_ZN2nn4time8FinalizeEv", "Finalize host-clock time service",
          uint32_t, sky_time_finalize_entry, void) { return sky_time_finalize(); }
SKY_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE",
          "Read current PosixTime from the host realtime clock",
          uint32_t, sky_time_user_get_current_time_entry, int64_t *out_posix_time) {
    return sky_time_get_current_time(out_posix_time);
}
SKY_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE",
          "Read network PosixTime from the host realtime clock",
          uint32_t, sky_time_network_get_current_time_entry, int64_t *out_posix_time) {
    return sky_time_get_current_time(out_posix_time);
}
SKY_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv",
          "Return current host PosixTime value for StandardUserSystemClock::now()",
          int64_t, sky_time_user_now_entry, void) {
    return sky_time_now();
}
SKY_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "Convert PosixTime to CalendarTime using host localtime_r for savegame timestamps",
          uint32_t, sky_time_to_calendar_time_entry,
          sky_time_calendar_time_t *out_cal,
          sky_time_calendar_additional_info_t *out_info,
          const int64_t *posix_time) {
    return sky_time_to_calendar_time(out_cal, out_info, posix_time);
}
SKY_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "Convert CalendarTime to PosixTime using host mktime for savegame sorting",
          uint32_t, sky_time_to_posix_time_entry,
          int *out_count,
          int64_t *out_times,
          int count,
          const sky_time_calendar_time_t *cal) {
    return sky_time_to_posix_time(out_count, out_times, count, cal);
}

static uint32_t g_performance_configurations[2];

static int valid_performance_mode(int mode) {
    return (unsigned)mode < 2;
}

void sky_oe_set_focus_handling_mode(int mode) {
    LOG("OE_FOCUS_HANDLING mode=%d host=fixed", mode);
}

void sky_oe_set_performance_mode_changed_notification_enabled(unsigned enabled) {
    LOG("OE_PERFORMANCE_NOTIFICATION enabled=%u host=fixed", enabled & 1u);
}

void sky_oe_set_performance_configuration(int mode, int configuration) {
    if (!valid_performance_mode(mode)) die("unsupported OE performance mode %d", mode);
    g_performance_configurations[mode] = (uint32_t)configuration;
    LOG("OE_SET_PERFORMANCE_CONFIGURATION mode=%d value=%#x", mode, configuration);
}

int sky_oe_get_operation_mode(void) {
    LOG("OE_OPERATION_MODE mode=1 name=console");
    return 1;
}

int sky_oe_get_performance_mode(void) {
    LOG("OE_PERFORMANCE_MODE mode=0 name=normal");
    return 0;
}

int sky_oe_get_performance_configuration(int mode) {
    if (!valid_performance_mode(mode)) die("unsupported OE performance mode %d", mode);
    uint32_t configuration = g_performance_configurations[mode];
    sky_checkpoint("post_nnMain_oe_defaults");
    LOG("OE_PERFORMANCE_CONFIGURATION mode=%d value=%#x", mode, configuration);
    return (int)configuration;
}

SKY_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "Use the fixed host focus policy without an am IPC session",
          void, sky_oe_focus_mode_entry, int mode) { sky_oe_set_focus_handling_mode(mode); }
SKY_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "The fixed host performance mode never changes",
          void, sky_oe_performance_notification_entry, unsigned enabled) {
    sky_oe_set_performance_mode_changed_notification_enabled(enabled);
}
SKY_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "Store the requested guest performance configuration without host tuning",
          void, sky_oe_set_performance_configuration_entry, int mode, int configuration) {
    sky_oe_set_performance_configuration(mode, configuration);
}
SKY_FORCE("_ZN2nn2oe16GetOperationModeEv", "The macOS host uses the fixed console operation profile",
          int, sky_oe_get_operation_mode_entry, void) { return sky_oe_get_operation_mode(); }
SKY_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "The fixed host performance profile is Normal",
          int, sky_oe_get_performance_mode_entry, void) { return sky_oe_get_performance_mode(); }
SKY_FORCE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", "Return the configuration stored by the guest",
          int, sky_oe_get_performance_configuration_entry, int mode) {
    return sky_oe_get_performance_configuration(mode);
}

int sky_aoc_list_add_on_content(int *out, int offset, int count) {
    (void)out;
    (void)offset;
    (void)count;
    sky_checkpoint("post_nnMain_aoc_list");
    LOG("AOC_LIST count=0 source=host-offline");
    return 0;
}

SKY_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "No add-on content is mounted, so the list is empty",
          int, sky_aoc_list_entry, int *out, int offset, int count) {
    return sky_aoc_list_add_on_content(out, offset, count);
}

uint64_t sky_oe_get_desired_language(void) {
    uint64_t value = 0;
    memcpy(&value, "en-US", 5);
    sky_checkpoint("post_nnMain_oe_language");
    LOG("OE_DESIRED_LANGUAGE code=en-US");
    return value;
}

SKY_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "Return the fixed host en-US LanguageCode in its 8-byte ABI representation",
          uint64_t, sky_oe_get_desired_language_entry, void) { return sky_oe_get_desired_language(); }
