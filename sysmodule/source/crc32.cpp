#include "omnisave.h"

static uint32_t s_tab[256];
static bool     s_rdy;

void crc_init(void) {
    if (s_rdy) return;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_tab[i] = c;
    }
    s_rdy = true;
}

uint32_t crc_run(uint32_t c, const void* d, size_t n) {
    const uint8_t* p = (const uint8_t*)d;
    c = ~c;
    while (n--) c = s_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
