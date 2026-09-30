#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace starfox::render {
struct GpuBspSettings {
    std::uint32_t tree_count{},node_count{},visibility_count{},face_count{};
    std::uint32_t output_count{},reserved[3]{};
};
struct GpuBspOutput {void* order{};void* results{};};
// Node: uint4(visibility,fallthrough,alternate,firstFace),
//       uint4(faceCount,isLeaf,0,0). Missing links are UINT32_MAX.
// Tree: uint4(root,outputFirst,outputCapacity,workLimit).
// Tree output ranges must be disjoint and fit output_count.
// Results: uint2(count,status), status 0 succeeds; 1 invalid batch/range,
// 2 depth >64, 3 capacity exhausted, 4 work limit. Failure count is zero.
class GpuBsp {
public:
    GpuBsp();~GpuBsp();
    // Borrowed resident buffers on the caller's SDL command. No submit/wait or
    // readback. Consume before the next enqueue, cancel command on failure,
    // release before destroying device. Inputs must not alias owned outputs.
    GpuBspOutput enqueue(void* device,void* command,void* nodes,void* visibility,
        void* faces,void* trees,const GpuBspSettings& settings);
    void release_device() noexcept;
    const std::string& status() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
static_assert(sizeof(GpuBspSettings)==32);
}
