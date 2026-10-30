/* Sprite-list demand, before the hardware's 96-entry truncation.
 * SCB3 matching ignores X, transparency and shrink. Screen y=0 corresponds
 * to hardware scanline 16; sprite zero is reserved for list padding.
 */
#ifndef GNGEO_SPRITE_DEMAND_H
#define GNGEO_SPRITE_DEMAND_H
struct sprite_demand { unsigned total, peak, line, beyond; };
static struct sprite_demand sprite_demand_measure(const unsigned short *scb3) {
    int delta[225] = {0}, count = 0;
    unsigned rows = 0, y = 0, i, wrap;
    struct sprite_demand result = {0, 0, 0, 0};
    for (i = 1; i < 448; ++i) {
        unsigned word = scb3[i];
        int active = 0;
        if (!(word & 0x40)) { rows = word & 63; y = (496 - (word >> 7)) & 511; }
        if (!rows) continue;
        for (wrap = 0; wrap < 2; ++wrap) {
            int start = (int)y - (wrap ? 512 : 0);
            int end = start + (rows >= 32 ? 512 : rows * 16);
            if (start < 0) start = 0;
            if (end > 224) end = 224;
            if (start >= end) continue;
            active = 1;
            if (i <= 381) { ++delta[start]; --delta[end]; }
        }
        if (active) { ++result.total; if (i > 381) ++result.beyond; }
    }
    for (i = 0; i < 224; ++i) {
        count += delta[i];
        if ((unsigned)count > result.peak) { result.peak = count; result.line = i; }
    }
    return result;
}
#endif
