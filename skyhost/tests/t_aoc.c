#include "startup.h"
#include "event.h"
#include "skyhost.h"
#include <assert.h>

void *guest_sym(const char *name) {
    (void)name;
    return NULL;
}

int main(void) {
    assert(sky_aoc_count_add_on_content() == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_aoc_count"));
    assert(sky_aoc_count_add_on_content() == 0);
    int entries[4];
    memset(entries, 0xa5, sizeof entries);
    assert(sky_aoc_list_add_on_content(entries, 0, 4) == 0);
    for (size_t index = 0; index < sizeof entries / sizeof entries[0]; index++)
        assert(entries[index] == (int)0xa5a5a5a5);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_aoc_list"));
    uint8_t change_event[0x30];
    memset(change_event, 0, sizeof change_event);
    sky_aoc_get_add_on_content_list_changed_event(change_event);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_aoc_change_event"));
    assert(change_event[0x28] == 1);
    assert(sky_event_wait(change_event, 0) == 0xEA01u);
    assert(sky_event_signal(change_event) == 0u);
    assert(sky_event_wait(change_event, 0) == 0u);
    assert(sky_event_wait(change_event, 0) == 0xEA01u);
    assert(sky_event_destroy(change_event) == 0u);
    puts("T_AOC OK offline-count-zero empty-list change-event");
    return 0;
}
