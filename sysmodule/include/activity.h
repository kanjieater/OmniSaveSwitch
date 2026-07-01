#pragma once
#include <switch.h>

// Load persisted pdm offset from SD. Call once after pdmqryInitialize() and SD mount.
// sd must be open for reading; activity_init does not close it.
void activity_init(FsFileSystem* sd);

// Read new pdm events since last offset and POST to server.
// sd must be open (used to persist the updated offset on success).
// Non-reentrant: if already in progress, returns 0 immediately.
int activity_flush(FsFileSystem* sd);
