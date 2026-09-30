#pragma once
#include "starfox/render/portable_shadows.hpp"
#include "starfox/render/gpu_scene.hpp"
namespace starfox::render::shadows {
// Owns SDL's copy of a native DXR mask. The SDL device is borrowed and must
// outlive this object (or release_device). Unsupported backends decline.
class SdlDxrShadows {
public:
    SdlDxrShadows();
    ~SdlDxrShadows();
    // Optional complete scene geometry must have been submitted on this SDL
    // device, and remain borrowed through this call. No vertex readback.
    bool render_resident(void*,const Scene&,Camera,Vec3,std::optional<ReceiverPlane>,
        const GpuScene::RayGeometryOutput* geometry=nullptr);
    GpuShadowOutput output() const;
    bool readback(std::vector<std::uint8_t>&); // Explicit fallback/diagnostic only.
    void release_device() noexcept;
    const std::string& status() const;
    // Set optional native Vulkan creation requirements on SDL properties.
    // Caller retries without these options if the adapter cannot provide them.
    static bool request_vulkan_interop(std::uint32_t properties);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
