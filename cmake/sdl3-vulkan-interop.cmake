# Guard every insertion against the exact pinned SDL source. Device extension
# options are validated upstream but were omitted from VkDeviceCreateInfo.
set(backend "${SOURCE_DIR}/src/gpu/vulkan/SDL_gpu_vulkan.c")
file(READ "${backend}" code)
set(marker "/* Star Fox Vulkan interop v1 */")
string(FIND "${code}" "${marker}" applied)
if(applied GREATER_EQUAL 0)
    return()
endif()
function(replace_exact old new)
    string(FIND "${code}" "${old}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Unexpected pinned SDL Vulkan source: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" code "${code}")
    set(code "${code}" PARENT_SCOPE)
endfunction()
replace_exact("        &renderer->supports);\n    deviceExtensions = SDL_stack_alloc("
    "        &renderer->supports) + features->additionalDeviceExtensionCount;\n    deviceExtensions = SDL_stack_alloc(")
replace_exact("    CreateDeviceExtensionArray(&renderer->supports, deviceExtensions);"
    "    CreateDeviceExtensionArray(&renderer->supports, deviceExtensions);\n    for (Uint32 extra = 0; extra < features->additionalDeviceExtensionCount; ++extra) {\n        deviceExtensions[GetDeviceExtensionCount(&renderer->supports) + extra] = features->additionalDeviceExtensionNames[extra];\n    }")
replace_exact("static SDL_GPUDevice *VULKAN_CreateDevice(bool debugMode, bool preferLowPower, SDL_PropertiesID props)"
    "${marker}\n#include \"sdl_vulkan_bridge.inc\"\n\nstatic SDL_GPUDevice *VULKAN_CreateDevice(bool debugMode, bool preferLowPower, SDL_PropertiesID props)")
replace_exact("    result->driverData = (SDL_GPURenderer *)renderer;"
    "    result->driverData = (SDL_GPURenderer *)renderer;\n    Starfox_Vulkan_PublishBridge(renderer);")
file(WRITE "${backend}" "${code}")
