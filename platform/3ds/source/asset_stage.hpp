#pragma once

#include "starfox/assets/runtime_bundle.hpp"

// C++ loader of the asset stager, used by game_3ds.cpp: reads the runtime bundle that
// starwing_prepare_assets() (asset_stage.h) writes to the SD card.

// Read a locally generated companion only after checking it against the
// exact patch and symbol resources packaged with this executable. Throws
// std::runtime_error when a file cannot be read or the bundle does not match.
starfox::assets::RuntimeBundlePayload starwing_load_runtime_payload(
    const char *path);
