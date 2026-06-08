#pragma once

// Write a single-byte '1' trigger to the given path.
// Returns true on success, false if the file could not be opened.
bool write_signal(const char* path);
