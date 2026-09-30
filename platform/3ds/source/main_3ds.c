#include "rom_probe.h"
#include "asset_stage.h"
#include "core_probe.h"

#include <3ds.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define APP_DIR "sdmc:/3ds/Starwing"
#define ASSET_PATH APP_DIR "/Starfox-Assets.BIN"
#define TOP_WIDTH 400u
#define TOP_HEIGHT 240u

static unsigned char top_art[TOP_WIDTH * TOP_HEIGHT * 3u];
static int top_art_ready;

static int prepare_storage(void) {
    if (mkdir("sdmc:/3ds", 0777) != 0 && errno != EEXIST) return 0;
    if (mkdir(APP_DIR, 0777) != 0 && errno != EEXIST) return 0;
    return 1;
}

static void load_top_art(void) {
    FILE *file = fopen("romfs:/top-background.rgb", "rb");
    if (file == NULL) return;
    top_art_ready = fread(top_art, 1, sizeof(top_art), file) == sizeof(top_art);
    fclose(file);
}

static void draw_top(void) {
    u16 width, height;
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    /* libctru reports the rotated physical framebuffer as 240 x 400. */
    if (framebuffer == NULL || width != TOP_HEIGHT || height != TOP_WIDTH) return;
    for (unsigned x = 0; x < TOP_WIDTH; ++x) {
        for (unsigned y = 0; y < TOP_HEIGHT; ++y) {
            const size_t target = ((size_t)x * TOP_HEIGHT + TOP_HEIGHT - 1u - y) * 3u;
            if (top_art_ready) {
                const size_t source = ((size_t)y * TOP_WIDTH + x) * 3u;
                framebuffer[target] = top_art[source + 2u];
                framebuffer[target + 1u] = top_art[source + 1u];
                framebuffer[target + 2u] = top_art[source];
            } else {
                framebuffer[target] = 22u;
                framebuffer[target + 1u] = 14u;
                framebuffer[target + 2u] = 5u;
            }
        }
    }
}

static void show_probe_result(int storage_ready) {
    consoleClear();
    printf("STARWING 3DS - DIAGNOSTIC 0.1\n\n");
    if (!storage_ready) {
        printf("SD error: could not create\n%s\n", APP_DIR);
        return;
    }
    printf("SD folder ready:\n%s\n\n", APP_DIR);
    FILE *existing = fopen(ASSET_PATH, "rb");
    if (existing != NULL) {
        fclose(existing);
        char existing_message[160];
        if (starwing_prepare_assets(NULL, ASSET_PATH,
                existing_message, sizeof(existing_message)) == 1) {
            printf("%s\n", existing_message);
            printf("No ROM needed on later boots.\n");
            printf("\nA: Scan  X: Core test\nSTART: Exit\n\n");
            printf("Game runtime is not yet linked.\n");
            return;
        }
    }
    DIR *directory = opendir(APP_DIR);
    if (directory == NULL) {
        printf("Could not read SD folder.\n");
        return;
    }
    int candidates = 0;
    int accepted = 0;
    char supported_path[512] = {0};
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || !starwing_has_rom_extension(entry->d_name))
            continue;
        ++candidates;
        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", APP_DIR, entry->d_name)
            >= (int)sizeof(path)) continue;
        StarwingRomInfo info;
        if (starwing_probe_rom(path, &info)) {
            printf("Detected:\n%s\n", info.revision);
            snprintf(supported_path, sizeof(supported_path), "%s", path);
            accepted = 1;
            break;
        }
    }
    closedir(directory);
    if (!accepted) {
        if (candidates)
            printf("No clean supported revision\nfound in this folder.\n");
        else
            printf("Put a clean Star Fox or\nStarwing .sfc/.smc here.\n");
    }
    printf("\nChecking asset companion...\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
    char asset_message[160];
    const int asset_status = starwing_prepare_assets(
        accepted ? supported_path : NULL, ASSET_PATH,
        asset_message, sizeof(asset_message));
    printf("%s\n", asset_message);
    if (asset_status == 2) printf("Created Starfox-Assets.BIN\n");
    printf("\nA: Scan  X: Core test\nSTART: Exit\n\n");
    printf("Game runtime is not yet linked.\n");
}

int main(void) {
    gfxInitDefault();
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    consoleInit(GFX_BOTTOM, NULL);
    const int romfs_ready = R_SUCCEEDED(romfsInit());
    if (romfs_ready) load_top_art();
    const int storage_ready = prepare_storage();
    show_probe_result(storage_ready);

    while (aptMainLoop()) {
        hidScanInput();
        const u32 pressed = hidKeysDown();
        if (pressed & KEY_START) break;
        if (pressed & KEY_A) show_probe_result(storage_ready);
        if (pressed & KEY_X) {
            consoleClear();
            printf("Running native core test...\n");
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
            char result[240];
            const int passed = starwing_core_probe(ASSET_PATH,
                result, sizeof(result));
            printf("%s\n\n", result);
            printf("%s\n", passed ? "Core test passed" : "Core test failed");
            printf("\nA: Scan  X: Retry\nSTART: Exit\n");
        }
        draw_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    if (romfs_ready) romfsExit();
    gfxExit();
    return 0;
}
