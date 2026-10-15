/* Host tool for the fuzzer: decodes instruction words with the translator's decoder and prints
 * "<unpredictable 0/1> <op> <sub>" per input line. Input lines: "a <hex>" (A32), "t <hex>" (T32: 4 hex digits for
 * 16-bit, 8 for 32-bit), optional third field = ITSTATE condition (Thumb, executes inside an IT block). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/insn.h"

int main(void)
{
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        char mode;
        char hex[32];
        int itc = -1;
        if (sscanf(line, " %c %31s %d", &mode, hex, &itc) < 2) continue;
        uint32_t w = (uint32_t)strtoul(hex, NULL, 16);
        insn_t i;
        if (mode == 'a') {
            a32_decode(w, 0x10000, &i);
        } else {
            uint16_t hw[2];
            if (strlen(hex) <= 4) hw[0] = (uint16_t)w, hw[1] = 0;
            else hw[0] = (uint16_t)(w >> 16), hw[1] = (uint16_t)w;
            t32_decode(hw, 0x10000, itc >= 0, itc >= 0 ? (itc | 0x100) : 0, &i);
        }
        printf("%d %d %d\n", i.unpred, i.op, i.sub);
    }
    return 0;
}
