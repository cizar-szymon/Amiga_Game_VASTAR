/* Vastar port - Z80 core test: runs CP/M test programs (prelim, zexdoc, zexall)
 *
 *   z80_test FILE.com|FILE.cim
 *
 * The program is loaded at 0x0100, BDOS functions 2 and 9 (print) are trapped at 0x0005,
 * a jump to 0x0000 ends the test. Prints the cycle count at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include "z80.h"

static uint8_t ram[0x10000 + 4];

int main(int argc, char **argv)
{
    Z80 z;
    FILE *fp;
    size_t n;
    unsigned long long cycles = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: z80_test FILE.com\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 2;
    }
    n = fread(ram + 0x100, 1, 0x10000 - 0x100, fp);
    fclose(fp);
    printf("loaded %u bytes\n", (unsigned)n);

    z80_init(&z, ram);
    ram[0x0000] = 0x76;   /* HALT if the program warm boots */
    ram[0x0005] = 0xc9;   /* BDOS: RET */
    z.pc = 0x0100;
    z.sp = 0xf000;

    for (;;) {
        if (z.pc == 0x0005) {
            if (z.c == 2) {
                putchar(z.e);
            } else if (z.c == 9) {
                uint16_t a = (uint16_t)((z.d << 8) | z.e);
                while (ram[a] != '$') {
                    int ch = ram[a++];    /* putchar() may be a macro */
                    putchar(ch);
                }
            }
            fflush(stdout);
        } else if (z.pc == 0x0000) {
            break;
        }
        z.icount = 0;   /* exactly one instruction per call */
        cycles += (unsigned long long)z80_run(&z, 1);
    }
    printf("\n%llu cycles\n", cycles);
    return 0;
}
