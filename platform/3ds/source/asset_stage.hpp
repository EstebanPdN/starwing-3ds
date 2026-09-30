#pragma once

#include "starfox/assets/runtime_bundle.hpp"

// Read a locally generated companion only after checking it against the
// exact patch and symbol resources packaged with this executable.
starfox::assets::RuntimeBundlePayload starwing_load_runtime_payload(
    const char *path);
