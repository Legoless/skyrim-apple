#pragma once

#include <stdint.h>

int sky_aoc_count_add_on_content(void);
int sky_aoc_list_add_on_content(int *out, int offset, int count);
/* Skyrim's void eShop overload closes synchronously while offline. The
 * referenced account handle is opaque and is neither read nor modified. */
void sky_ec_show_shop_add_on_content_list(uint64_t application_id, const void *user_handle);
typedef struct {
    int16_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    int8_t second;
} sky_time_calendar_time_t;

typedef struct {
    uint32_t day_of_week;
    int32_t day_of_year;
    char timezone_name[8];
    uint8_t is_dst;
    uint8_t pad[3];
    int32_t utc_offset_seconds;
} sky_time_calendar_additional_info_t;

uint32_t sky_time_initialize(void);
uint32_t sky_time_finalize(void);
uint32_t sky_time_get_current_time(int64_t *out_posix_time);
int64_t sky_time_now(void);
uint32_t sky_time_to_calendar_time(sky_time_calendar_time_t *out_cal,
                                   sky_time_calendar_additional_info_t *out_info,
                                   const int64_t *posix_time);
uint32_t sky_time_to_posix_time(int *out_count,
                                int64_t *out_times,
                                int count,
                                const sky_time_calendar_time_t *cal);
void sky_oe_initialize(void);
void sky_oe_finish_startup_logo(void);
int sky_oe_is_startup_logo_finished(void);
int sky_oe_get_current_focus_state(void);
void sky_oe_set_host_focus(int focused);
int sky_oe_try_pop_notification_message(uint32_t *message);
void sky_oe_enter_exit_request_handling_section(void);
void sky_oe_leave_exit_request_handling_section(void);
unsigned sky_oe_exit_request_handling_depth(void);
void sky_oe_set_focus_handling_mode(int mode);
void sky_oe_set_performance_mode_changed_notification_enabled(unsigned enabled);
void sky_oe_set_performance_configuration(int mode, int configuration);
int sky_oe_get_operation_mode(void);
int sky_oe_get_performance_mode(void);
int sky_oe_get_performance_configuration(int mode);
uint64_t sky_oe_get_desired_language(void);
