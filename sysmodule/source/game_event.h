#pragma once
#include <stdint.h>

// Atomic game-close event channel between game_monitor_thread (producer)
// and the main FSM tick loop (consumer).
//
// Only one pending title slot exists at a time. If a second close arrives
// while the first is still unprocessed, it is silently dropped — this matches
// the existing single-slot design in main.cpp and is acceptable because two
// games cannot run concurrently on Horizon OS.

// Called from game_monitor_thread when a game closes.
// Writes title_id into the pending slot via CAS (only if slot is empty).
// Returns true if the event was accepted, false if a prior event is still pending.
bool     game_event_signal(uint64_t title_id);

// Called from the main thread once per tick.
// Atomically reads and clears the pending slot.
// Returns 0 if nothing was pending.
uint64_t game_event_drain(void);
