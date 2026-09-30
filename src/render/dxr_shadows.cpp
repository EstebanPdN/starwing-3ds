#include "starfox/render/dxr_shadows.hpp"
#if defined(STARFOX_DXR)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "shadow_dxr_shader.hpp"
#include <cstring>
#include <stdexcept>
#include <sstream>
#endif

namespace starfox::render::shadows {
#if defined(STARFOX_DXR)
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(message.str());
    }
}
struct Buffer { ComPtr<ID3D12Resource> resource; UINT64 capacity{}; };
struct Float4 { float x{}, y{}, z{}, w{}; };
Float4 floats(Vec3 v) { return {float(v.x),float(v.y),float(v.z),0}; }
struct Constants { Float4 camera,options,point,normal; std::array<Float4,8> lights; };
static_assert(sizeof(Constants)==192);
}
struct DxrShadows::Impl {
    bool attempted{}, failed{};
    std::optional<std::array<std::uint8_t,8>> requested_adapter;
    std::string status{"DXR not initialized"};
    HMODULE d3d{}, dxgi{};
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Fence> geometry_fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    HANDLE event{};
    UINT64 serial{};
    Buffer vertices,instances,constants,blas,tlas,scratch,output,readback,shared_geometry,coverage_buffer;
    std::vector<float> built_positions;
    std::vector<float> position_scratch;
    std::vector<std::uint8_t> uploaded_coverage;
    std::vector<std::uint8_t> coverage_scratch;
    bool output_common{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO cached_bottom_info{},cached_top_info{};
    std::size_t cached_triangle_count{};
    UINT cached_vertex_stride{};
    ~Impl() {
        // Submitted work is waited before resources are recycled or destroyed.
        if (queue && fence && event) {
            const auto value=++serial;
            if (SUCCEEDED(queue->Signal(fence.Get(),value))
                && SUCCEEDED(fence->SetEventOnCompletion(value,event)))
                WaitForSingleObject(event,5000);
        }
        coverage_buffer.resource.Reset();shared_geometry.resource.Reset();readback.resource.Reset(); output.resource.Reset(); scratch.resource.Reset();
        tlas.resource.Reset(); blas.resource.Reset(); constants.resource.Reset();
        instances.resource.Reset(); vertices.resource.Reset(); pipeline.Reset(); root.Reset();
        list.Reset(); allocator.Reset(); queue.Reset(); geometry_fence.Reset(); fence.Reset(); device.Reset();
        if (event) CloseHandle(event);
        if (dxgi) FreeLibrary(dxgi);
        if (d3d) FreeLibrary(d3d);
    }
    void initialize() {
        attempted=true;
        d3d=LoadLibraryExW(L"d3d12.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!d3d || !dxgi) throw std::runtime_error("Direct3D 12 unavailable");
        const auto createDevice=reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(d3d,"D3D12CreateDevice"));
        const auto createFactory=reinterpret_cast<decltype(&CreateDXGIFactory2)>(GetProcAddress(dxgi,"CreateDXGIFactory2"));
        const auto serialize=reinterpret_cast<decltype(&D3D12SerializeRootSignature)>(GetProcAddress(d3d,"D3D12SerializeRootSignature"));
        if (!createDevice || !createFactory || !serialize) throw std::runtime_error("Direct3D 12 entry points unavailable");
        ComPtr<IDXGIFactory6> factory;
        check(createFactory(0,IID_PPV_ARGS(factory.GetAddressOf())),"DXGI factory");
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(adapter.GetAddressOf()))==DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) continue;
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (requested_adapter && std::memcmp(requested_adapter->data(),&desc.AdapterLuid,8)!=0) continue;
            ComPtr<ID3D12Device5> candidate;
            if (FAILED(createDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,
                IID_ID3D12Device5, reinterpret_cast<void**>(candidate.GetAddressOf())))) continue;
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 caps{};
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
            if (FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&caps,sizeof(caps)))
                || caps.RaytracingTier<D3D12_RAYTRACING_TIER_1_1
                || FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))
                || model.HighestShaderModel<D3D_SHADER_MODEL_6_5) continue;
            device=std::move(candidate);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
            status=std::string("Hardware DXR 1.1: ")+name;
            break;
        }
        if (!device) throw std::runtime_error(requested_adapter
            ? "Requested adapter has no hardware DXR 1.1 device" : "No hardware DXR 1.1 device");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&queueDesc,IID_ID3D12CommandQueue, reinterpret_cast<void**>(queue.GetAddressOf())),"DXR queue");
        check(device->CreateCommandAllocator(queueDesc.Type,IID_ID3D12CommandAllocator, reinterpret_cast<void**>(allocator.GetAddressOf())),"DXR allocator");
        check(device->CreateCommandList(0,queueDesc.Type,allocator.Get(),nullptr,
            IID_ID3D12GraphicsCommandList4, reinterpret_cast<void**>(list.GetAddressOf())),"DXR command list");
        check(list->Close(),"DXR initial close");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_ID3D12Fence, reinterpret_cast<void**>(fence.GetAddressOf())),"DXR fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event) throw std::runtime_error("DXR fence event unavailable");
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[3].Descriptor.ShaderRegister=1;
        for (auto& p:parameters) p.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters=4; rootDesc.pParameters=parameters;
        ComPtr<ID3DBlob> blob,error;
        check(serialize(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,blob.GetAddressOf(),error.GetAddressOf()),"DXR root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            IID_ID3D12RootSignature, reinterpret_cast<void**>(root.GetAddressOf())),"DXR root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature=root.Get();
        pipelineDesc.CS={shadow_dxr_shader,sizeof(shadow_dxr_shader)};
        check(device->CreateComputePipelineState(&pipelineDesc,IID_ID3D12PipelineState, reinterpret_cast<void**>(pipeline.GetAddressOf())),"DXR shader pipeline");
    }
    void ensure(Buffer& buffer, UINT64 bytes, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, bool uav=false,bool shared=false) {
        if (buffer.resource && buffer.capacity>=bytes) return;
        buffer.resource.Reset(); buffer.capacity=0;
        D3D12_HEAP_PROPERTIES properties{}; properties.Type=heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=(std::max<UINT64>(bytes,256)+255)&~UINT64(255);
        desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
        desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        check(device->CreateCommittedResource(&properties,shared?D3D12_HEAP_FLAG_SHARED:D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
            IID_ID3D12Resource, reinterpret_cast<void**>(buffer.resource.GetAddressOf())),"DXR buffer allocation");
        buffer.capacity=desc.Width;
    }
    void upload(Buffer& buffer,const void* data,std::size_t size) {
        ensure(buffer,size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* target{}; const D3D12_RANGE empty{0,0};
        check(buffer.resource->Map(0,&empty,&target),"DXR upload map");
        std::memcpy(target,data,size);
        const D3D12_RANGE written{0,size}; buffer.resource->Unmap(0,&written);
    }
    void barrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; barrier.UAV.pResource=resource;
        list->ResourceBarrier(1,&barrier);
    }
    void await_producer() {
        if(!serial) return;
        check(device->GetDeviceRemovedReason(),"DXR producer device");
        if(fence->GetCompletedValue()>=serial) return;
        check(fence->SetEventOnCompletion(serial,event),"DXR producer completion");
        if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)
            throw std::runtime_error("DXR producer reuse timeout");
        check(device->GetDeviceRemovedReason(),"DXR completed device");
    }
    void render(const Scene& scene,Camera camera,Vec3 light,std::optional<ReceiverPlane> ground,
        std::vector<std::uint8_t>& mask,bool download=true,ID3D12Resource* external=nullptr,UINT external_count=0,UINT stride=16,const Coverage* coverage=nullptr,bool release_for_external=false,bool defer_completion=false) {
        // Upload buffers and the command allocator belong to the previous
        // producer submission until its fence completes.
        await_producer();
        const auto count=external?external_count/3:scene.triangle_count();
        const auto pixels=std::size_t(camera.width)*camera.height;
        const auto row_bytes=(std::size_t(camera.width)+3U)&~std::size_t(3U);
        const auto transfer_bytes=row_bytes*camera.height;
        if (!pixels || !count) { mask.assign(pixels,0); return; }
        if (count>UINT_MAX/3 || camera.width>16384 || camera.height>16384)
            throw std::runtime_error("DXR scene exceeds supported dimensions");
        static_assert(sizeof(TriangleCoverage)==40);
        const std::uint32_t header[4]={coverage?std::uint32_t(count):0,
            coverage?std::uint32_t(coverage->texels.size()):0, std::uint32_t(16+count*40),0};
        auto& coverage_bytes=coverage_scratch;
        coverage_bytes.resize(coverage?16+count*40+coverage->texels.size()*4:16);
        std::memcpy(coverage_bytes.data(),header,16);
        if(coverage) {
            std::memcpy(coverage_bytes.data()+16,coverage->triangles.data(),count*40);
            if(!coverage->texels.empty()) std::memcpy(coverage_bytes.data()+16+count*40,coverage->texels.data(),coverage->texels.size()*4);
        }
        // Compare complete bytes, not source identities: UV scroll, palettes,
        // transparency and recycled models must all invalidate retained data.
        if(coverage_bytes!=uploaded_coverage) {
            upload(coverage_buffer,coverage_bytes.data(),coverage_bytes.size());
            uploaded_coverage.swap(coverage_bytes);
        }
        auto& positions=position_scratch;
        positions.clear();
        if(!external) positions.reserve(count*9);
        if(!external) for (const auto& triangle:scene.triangles()) for (auto v:{triangle.a,triangle.b,triangle.c}) {
            positions.push_back(float(v.x)); positions.push_back(float(v.y)); positions.push_back(float(v.z));
        }
        // Compare actual GPU input bytes, not object identities or hashes:
        // animation, exploding faces and recycled objects must invalidate this.
        const bool rebuild=external || positions.size()!=built_positions.size()
            || std::memcmp(positions.data(),built_positions.data(),positions.size()*sizeof(float))!=0;
        if(rebuild && !external) upload(vertices,positions.data(),positions.size()*sizeof(float));
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount=UINT(count*3);
        geometry.Triangles.VertexBuffer={external?external->GetGPUVirtualAddress():vertices.resource->GetGPUVirtualAddress(),external?stride:12};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
        bottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bottom.NumDescs=1; bottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY; bottom.pGeometryDescs=&geometry;
        auto bottomInfo=cached_bottom_info;
        // Allocation requirements depend on layout/count, not animated positions.
        // External GPU geometry still rebuilds AS contents every frame below.
        const UINT vertex_stride=external?stride:12;
        if(!bottomInfo.ResultDataMaxSizeInBytes || cached_triangle_count!=count || cached_vertex_stride!=vertex_stride)
            device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom,&bottomInfo);
        ensure(blas,bottomInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true);
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;
        instance.InstanceMask=255;
        instance.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure=blas.resource->GetGPUVirtualAddress();
        if(rebuild) upload(instances,&instance,sizeof(instance));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        top.NumDescs=1; top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.InstanceDescs=instances.resource->GetGPUVirtualAddress();
        auto topInfo=cached_top_info;
        if(!topInfo.ResultDataMaxSizeInBytes) device->GetRaytracingAccelerationStructurePrebuildInfo(&top,&topInfo);
        ensure(tlas,topInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true);
        ensure(scratch,std::max(bottomInfo.ScratchDataSizeInBytes,topInfo.ScratchDataSizeInBytes),
            D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);
        if(!output.resource || output.capacity<transfer_bytes) output_common=false;
        ensure(output,transfer_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true,true);
        if(download) ensure(readback,transfer_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Constants settings{};
        settings.camera={float(camera.width),float(camera.height),float(camera.focal_length),float(camera.center_x)};
        settings.options={float(camera.center_y),ground?1.f:0.f,float(camera.vertical_focal_length()),coverage?1.f:0.f};
        if (ground) { settings.point=floats(ground->point); settings.normal=floats(ground->normal); }
        light=light*(1.0/std::sqrt(dot(light,light)));
        const auto reference=std::abs(light.y)<.9?Vec3{0,1,0}:Vec3{1,0,0};
        auto tangent=cross(light,reference); tangent=tangent*(1.0/std::sqrt(dot(tangent,tangent)));
        const auto bitangent=cross(light,tangent);
        for (unsigned i=0;i<8;++i) {
            const auto radius=.015*std::sqrt((i+.5)/8);
            const auto angle=i*2.399963229728653;
            auto direction=light+tangent*(radius*std::cos(angle))+bitangent*(radius*std::sin(angle));
            settings.lights[i]=floats(direction*(1.0/std::sqrt(dot(direction,direction))));
        }
        upload(constants,&settings,sizeof(settings));
        check(allocator->Reset(),"DXR allocator reset");
        check(list->Reset(allocator.Get(),pipeline.Get()),"DXR command reset");
        D3D12_RESOURCE_BARRIER vertex_access{};
        if(external) {
            vertex_access.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            vertex_access.Transition={external,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            list->ResourceBarrier(1,&vertex_access);
        }
        if(output_common) {
            D3D12_RESOURCE_BARRIER acquire{};acquire.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            acquire.Transition={output.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            list->ResourceBarrier(1,&acquire);output_common=false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs=bottom; build.DestAccelerationStructureData=blas.resource->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=scratch.resource->GetGPUVirtualAddress();
        if(rebuild) {
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        barrier(blas.resource.Get()); barrier(scratch.resource.Get());
        build.Inputs=top; build.DestAccelerationStructureData=tlas.resource->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        barrier(tlas.resource.Get());
        }
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootShaderResourceView(0,tlas.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1,output.resource->GetGPUVirtualAddress());
        list->SetComputeRootConstantBufferView(2,constants.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3,coverage_buffer.resource->GetGPUVirtualAddress());
        list->Dispatch((camera.width+7)/8,(camera.height+7)/8,1);
        if(download) {
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition={output.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&transition);
        list->CopyBufferRegion(readback.resource.Get(),0,output.resource.Get(),0,transfer_bytes);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);
        list->ResourceBarrier(1,&transition);
        }
        if(external) {
            std::swap(vertex_access.Transition.StateBefore,vertex_access.Transition.StateAfter);
            list->ResourceBarrier(1,&vertex_access);
        }
        if(release_for_external && !download) {
            D3D12_RESOURCE_BARRIER release{};
            release.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            release.Transition={output.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON};
            list->ResourceBarrier(1,&release);
            output_common=true;
        }
        check(list->Close(),"DXR command close");
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        const auto value=++serial;
        check(queue->Signal(fence.Get(),value),"DXR signal");
        if(!defer_completion) await_producer();
        if(rebuild) {
            built_positions.swap(positions);
            cached_bottom_info=bottomInfo;cached_top_info=topInfo;
            cached_triangle_count=count;cached_vertex_stride=vertex_stride;
        }
        if(!download) return;
        void* data{}; const D3D12_RANGE range{0,transfer_bytes};
        check(readback.resource->Map(0,&range,&data),"DXR readback map");
        mask.resize(pixels);
        const auto* values=static_cast<const std::uint8_t*>(data);
        if(row_bytes==camera.width) std::memcpy(mask.data(),values,pixels);
        else for(std::size_t y=0;y<camera.height;++y)
            std::memcpy(mask.data()+y*camera.width,values+y*row_bytes,camera.width);
        const D3D12_RANGE empty{0,0}; readback.resource->Unmap(0,&empty);
    }
};
#else
struct DxrShadows::Impl { std::string status{"Hardware DXR unavailable on this build"}; };
#endif
DxrShadows::DxrShadows(std::optional<std::array<std::uint8_t,8>> adapter_luid):impl_(std::make_unique<Impl>()) {
#if defined(STARFOX_DXR)
    impl_->requested_adapter=adapter_luid;
#else
    (void)adapter_luid;
#endif
}
DxrShadows::~DxrShadows()=default;
void* DxrShadows::export_geometry_completion_handle() {
#if defined(STARFOX_DXR)
    if(!available()) return nullptr;
    if(!impl_->geometry_fence && FAILED(impl_->device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,
        IID_ID3D12Fence,reinterpret_cast<void**>(impl_->geometry_fence.GetAddressOf())))) return nullptr;
    HANDLE handle{};
    if(SUCCEEDED(impl_->device->CreateSharedHandle(impl_->geometry_fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle))) return handle;
#endif
    return nullptr;
}
bool DxrShadows::wait_for_geometry(std::uint64_t value) {
#if defined(STARFOX_DXR)
    if(!value || !available() || !impl_->geometry_fence) return false;
    return SUCCEEDED(impl_->queue->Wait(impl_->geometry_fence.Get(),value));
#else
    (void)value;return false;
#endif
}
DxrShadows::ResidentGeometry DxrShadows::prepare_shared_geometry(std::uint32_t count) {
#if defined(STARFOX_DXR)
    if(!count || count%3 || count>12'000'000 || !available()) return {};
    try {
        impl_->await_producer();
        impl_->ensure(impl_->shared_geometry,uint64_t(count)*16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,true,true);
        ResidentGeometry result{impl_->shared_geometry.resource.Get(),count,16};
        LUID luid{};
#if defined(_MSC_VER)
        luid=impl_->device->GetAdapterLuid();
#else
        impl_->device->GetAdapterLuid(&luid);
#endif
        std::memcpy(result.adapter_luid.data(),&luid,sizeof(luid));
        const auto value=++impl_->serial;
        check(impl_->queue->Signal(impl_->fence.Get(),value),"DXR geometry ready signal");
        result.ready_value=value;
        return result;
    } catch(const std::exception& error) {impl_->status=error.what();}
#else
    (void)count;
#endif
    return {};
}
void* DxrShadows::export_geometry_handle() {
#if defined(STARFOX_DXR)
    if(impl_->shared_geometry.resource && !impl_->failed) {
        HANDLE handle{};
        if(SUCCEEDED(impl_->device->CreateSharedHandle(impl_->shared_geometry.resource.Get(),nullptr,GENERIC_ALL,nullptr,&handle))) return handle;
    }
#endif
    return nullptr;
}
void* DxrShadows::export_ready_fence_handle() {
#if defined(STARFOX_DXR)
    if((!resident_.resource && !impl_->shared_geometry.resource) || impl_->failed) return nullptr;
    HANDLE handle{};
    try {
        auto& p=*impl_;
        if(resident_.resource && !p.output_common) {
            p.await_producer();
            check(p.allocator->Reset(),"DXR export allocator");
            check(p.list->Reset(p.allocator.Get(),nullptr),"DXR export command");
            D3D12_RESOURCE_BARRIER release{};release.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            release.Transition={p.output.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON};
            p.list->ResourceBarrier(1,&release);
            check(p.list->Close(),"DXR export close");
            ID3D12CommandList* lists[]{p.list.Get()};p.queue->ExecuteCommandLists(1,lists);
            const auto value=++p.serial;
            check(p.queue->Signal(p.fence.Get(),value),"DXR export signal");
            check(p.fence->SetEventOnCompletion(value,p.event),"DXR export completion");
            if(WaitForSingleObject(p.event,5000)!=WAIT_OBJECT_0) throw std::runtime_error("DXR export timeout");
            check(p.device->GetDeviceRemovedReason(),"DXR export device");
            p.output_common=true;resident_.ready_value=value;
        }
    } catch(const std::exception& error) {
        impl_->status=error.what();impl_->failed=true;resident_={};return nullptr;
    }
    if(FAILED(impl_->device->CreateSharedHandle(impl_->fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle))) return nullptr;
    return handle;
#else
    return nullptr;
#endif
}
void* DxrShadows::export_resident_handle() {
#if defined(STARFOX_DXR)
    if(!resident_.resource || impl_->failed) return nullptr;
    HANDLE handle{};
    const auto result=impl_->device->CreateSharedHandle(impl_->output.resource.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
    if(FAILED(result)) {impl_->status="DXR shared-output handle export failed";return nullptr;}
    return handle;
#else
    return nullptr;
#endif
}
const std::string& DxrShadows::status() const { return impl_->status; }
bool DxrShadows::available() {
#if defined(STARFOX_DXR)
    if(impl_->failed) return false;
    try {if(!impl_->attempted) impl_->initialize();return true;}
    catch(const std::exception& error) {impl_->status=std::string("Hardware ray tracing unavailable: ")+error.what();impl_->failed=true;return false;}
#else
    return false;
#endif
}
bool DxrShadows::render(const Scene& scene,Camera camera,Vec3 light,
    std::optional<ReceiverPlane> ground,std::vector<std::uint8_t>& mask) {
    resident_={};
#if defined(STARFOX_DXR)
    if (impl_->failed) return false;
    if (!std::isfinite(dot(light,light)) || dot(light,light)<1e-20 || camera.focal_length<=0
        || camera.vertical_focal_length()<=0 || !std::isfinite(camera.vertical_focal_length())) return false;
    try {
        if (!impl_->attempted) impl_->initialize();
        impl_->render(scene,camera,light,ground,mask);
        return true;
    } catch (const std::exception& error) {
        impl_->status=std::string("CPU shadow fallback: ")+error.what();
        impl_->failed=true;
        return false;
    }
#else
    (void)scene; (void)camera; (void)light; (void)ground; (void)mask;
    return false;
#endif
}
bool DxrShadows::readback_resident(std::vector<std::uint8_t>& mask) {
    mask.clear();
#if defined(STARFOX_DXR)
    if(!resident_.resource || impl_->failed) return false;
    try {
        auto& p=*impl_;
        const auto bytes=std::size_t(resident_.row_bytes)*resident_.height;
        p.await_producer();
        p.ensure(p.readback,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        check(p.allocator->Reset(),"DXR download allocator");
        check(p.list->Reset(p.allocator.Get(),nullptr),"DXR download command");
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition={p.output.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            p.output_common?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        p.list->ResourceBarrier(1,&transition);
        p.list->CopyBufferRegion(p.readback.resource.Get(),0,p.output.resource.Get(),0,bytes);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);
        p.list->ResourceBarrier(1,&transition);
        check(p.list->Close(),"DXR download close");
        ID3D12CommandList* lists[]{p.list.Get()};p.queue->ExecuteCommandLists(1,lists);
        const auto value=++p.serial;
        check(p.queue->Signal(p.fence.Get(),value),"DXR download signal");
        check(p.fence->SetEventOnCompletion(value,p.event),"DXR download completion");
        if(WaitForSingleObject(p.event,5000)!=WAIT_OBJECT_0) throw std::runtime_error("DXR download timeout");
        check(p.device->GetDeviceRemovedReason(),"DXR download device");
        mask.resize(std::size_t(resident_.width)*resident_.height);
        void* data{};const D3D12_RANGE range{0,bytes};
        check(p.readback.resource->Map(0,&range,&data),"DXR resident map");
        if(resident_.row_bytes==resident_.width)
            std::memcpy(mask.data(),data,mask.size());
        else for(std::size_t y=0;y<resident_.height;++y)
            std::memcpy(mask.data()+y*resident_.width,static_cast<const std::uint8_t*>(data)+y*resident_.row_bytes,resident_.width);
        const D3D12_RANGE empty{0,0};p.readback.resource->Unmap(0,&empty);
        return true;
    } catch(const std::exception& error) {
        impl_->status=std::string("DXR download failure: ")+error.what();impl_->failed=true;resident_={};mask.clear();
    }
#endif
    return false;
}
bool DxrShadows::render_resident(const Scene& scene,Camera camera,Vec3 light,
    std::optional<ReceiverPlane> ground,const ResidentGeometry* geometry,const Coverage* coverage,bool release_for_external,bool defer_completion) {
    resident_={};
#if defined(STARFOX_DXR)
    if(impl_->failed || (defer_completion && !release_for_external) || (!geometry && !scene.triangle_count()) || !camera.width || !camera.height
        || !std::isfinite(dot(light,light)) || dot(light,light)<1e-20 || camera.focal_length<=0
        || camera.vertical_focal_length()<=0 || !std::isfinite(camera.vertical_focal_length())) return false;
    try {
        if(coverage) {
            const auto count=geometry?geometry->vertex_count/3:scene.triangle_count();
            if(coverage->triangles.size()!=count || count>4000000 || coverage->texels.size()>16000000) return false;
            for(const auto& c:coverage->triangles) {
                if(c.flags>1) return false;
                for(auto uv:c.uv) if(!std::isfinite(uv) || std::abs(uv)>1e8f) return false;
                if(c.flags && (c.u_mask>4095 || c.v_mask>4095 || (c.u_mask&(c.u_mask+1)) || (c.v_mask&(c.v_mask+1))
                    || std::uint64_t(c.offset)+std::uint64_t(c.u_mask+1)*(c.v_mask+1)>coverage->texels.size())) return false;
            }
        }
        if(!impl_->attempted) impl_->initialize();
        auto* external=geometry?static_cast<ID3D12Resource*>(geometry->resource):nullptr;
        if(geometry) {
            if(!external || !geometry->vertex_count || geometry->vertex_count%3 || geometry->stride<12 || geometry->stride>256 || geometry->stride%4) return false;
            D3D12_RESOURCE_DESC desc{};
#if defined(_MSC_VER)
            desc=external->GetDesc();
#else
            external->GetDesc(&desc);
#endif
            ComPtr<ID3D12Device5> owner;
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER
                || desc.Width<uint64_t(geometry->vertex_count-1)*geometry->stride+12
                || FAILED(external->GetDevice(IID_ID3D12Device5,reinterpret_cast<void**>(owner.GetAddressOf()))) || owner.Get()!=impl_->device.Get()) return false;
        }
        std::vector<std::uint8_t> unused;
        impl_->render(scene,camera,light,ground,unused,false,external,geometry?geometry->vertex_count:0,geometry?geometry->stride:16,coverage,release_for_external,defer_completion);
        resident_={impl_->device.Get(),impl_->output.resource.Get(),camera.width,camera.height,(camera.width+3U)&~3U};
        LUID luid{};
#if defined(_MSC_VER)
        luid=impl_->device->GetAdapterLuid();
#else
        impl_->device->GetAdapterLuid(&luid);
#endif
        static_assert(sizeof(luid)==8);
        std::memcpy(resident_.adapter_luid.data(),&luid,sizeof(luid));
        resident_.ready_value=impl_->serial;
        return true;
    } catch(const std::exception& error) {
        impl_->status=std::string("DXR resident failure: ")+error.what();impl_->failed=true;
    }
#else
    (void)scene;(void)camera;(void)light;(void)ground;
    (void)geometry;(void)coverage;(void)release_for_external;(void)defer_completion;
#endif
    return false;
}
}
