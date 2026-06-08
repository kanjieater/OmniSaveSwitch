#include "signal_writer.hpp"
#include <cstdio>

bool write_signal(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputc('1', f);
    fclose(f);
    return true;
}
