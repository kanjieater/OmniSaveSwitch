#pragma once
#include <switch.h>

// Reset offset state. Call once after pdmqryInitialize().
// Server watermark is acquired lazily on the first activity_flush() that gets a 200 response.
void activity_init(void);

// Read new pdm events since last offset and POST to server.
// Non-reentrant: if already in progress, returns 0 immediately.
int activity_flush(void);
