#include "game_event.h"
#include "omnisave.h"

bool game_event_signal(uint64_t title_id) {
    uint64_t expected = 0;
    return s_pending_extract_title.compare_exchange_strong(
        expected, title_id,
        std::memory_order_release,
        std::memory_order_relaxed);
}

uint64_t game_event_drain(void) {
    return s_pending_extract_title.exchange(0, std::memory_order_acquire);
}
