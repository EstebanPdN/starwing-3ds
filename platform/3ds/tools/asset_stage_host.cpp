#include "../source/asset_stage.h"

#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    char message[160];
    const int result = starwing_prepare_assets(argv[1], argv[2], message, sizeof(message));
    std::printf("result=%d %s\n", result, message);
    return result > 0 ? 0 : 1;
}
