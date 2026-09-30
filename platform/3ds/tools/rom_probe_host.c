#include "../source/rom_probe.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        StarwingRomInfo info;
        if (!starwing_probe_rom(argv[i], &info)) {
            printf("REJECT %s\n", argv[i]);
            ++failures;
        } else {
            printf("ACCEPT %s: %s\n", argv[i], info.revision);
        }
    }
    return failures ? 1 : 0;
}
