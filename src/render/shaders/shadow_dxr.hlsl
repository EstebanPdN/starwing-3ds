RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer coverage : register(t1);
RWByteAddressBuffer outputMask : register(u0);
cbuffer Settings : register(b0) {
    float4 camera; // width, height, focal length, center x
    float4 options; // center y, ground enabled
    float4 groundPoint;
    float4 groundNormal;
    float4 lights[8];
};

bool covered(uint primitive, float2 bary) {
    uint record = 16 + primitive * 40;
    if (primitive >= coverage.Load(0)) return false;
    if (coverage.Load(record + 36) == 0) return true;
    float2 a = asfloat(coverage.Load2(record));
    float2 b = asfloat(coverage.Load2(record + 8));
    float2 c = asfloat(coverage.Load2(record + 16));
    float2 uv = a * (1 - bary.x - bary.y) + b * bary.x + c * bary.y;
    uint2 mask = coverage.Load2(record + 28);
    uint2 xy = uint2(int2(floor(uv))) & mask;
    uint index = coverage.Load(record + 24) + xy.y * (mask.x + 1) + xy.x;
    return (coverage.Load(coverage.Load(8) + index * 4) >> 24) != 0;
}

uint trace_pixel(uint2 id) {
    float3 ray = float3((id.x + .5 - camera.w) / camera.z,
        (id.y + .5 - options.x) / options.z, 1);
    float depth = 65536;
    bool receiverFound = false;
    if (options.y != 0) {
        float denominator = dot(ray, groundNormal.xyz);
        if (abs(denominator) > 1e-10) {
            float groundDepth = dot(groundPoint.xyz, groundNormal.xyz) / denominator;
            if (groundDepth > 1 && groundDepth < depth) {
                depth = groundDepth;
                receiverFound = true;
            }
        }
    }
    RayDesc receiverRay;
    receiverRay.Origin = 0;
    receiverRay.Direction = ray;
    receiverRay.TMin = 1;
    receiverRay.TMax = depth;
    RayQuery<RAY_FLAG_NONE> receiver;
    receiver.TraceRayInline(scene, options.w != 0 ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_FORCE_OPAQUE, 255, receiverRay);
    while (receiver.Proceed()) {
        if (receiver.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && covered(receiver.CandidatePrimitiveIndex(), receiver.CandidateTriangleBarycentrics()))
            receiver.CommitNonOpaqueTriangleHit();
    }
    if (receiver.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        depth = receiver.CommittedRayT();
        receiverFound = true;
    }
    uint blocked = 0;
    if (receiverFound) {
        RayDesc shadowRay;
        shadowRay.Origin = ray * depth;
        shadowRay.TMin = max(.1, depth * 1e-5);
        shadowRay.TMax = 65536;
        for (uint sample = 0; sample < 8; ++sample) {
            shadowRay.Direction = lights[sample].xyz;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadow;
            shadow.TraceRayInline(scene, options.w != 0 ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_FORCE_OPAQUE, 255, shadowRay);
            while (shadow.Proceed()) {
                if (shadow.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && covered(shadow.CandidatePrimitiveIndex(), shadow.CandidateTriangleBarycentrics()))
                    shadow.CommitNonOpaqueTriangleHit();
            }
            if (shadow.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ++blocked;
        }
    }
    return 160 * blocked / 8;
}

// One byte per pixel, with four-byte row alignment for ByteAddressBuffer.
// Keep one ray workload per lane; shared packing avoids serializing four
// pixels' ray queries and does not assume a hardware wave/lane ordering.
groupshared uint shadowValues[64];
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID, uint local : SV_GroupIndex) {
    uint width = (uint)camera.x;
    bool valid = id.x < width && id.y < (uint)camera.y;
    shadowValues[local] = valid ? trace_pixel(id.xy) : 0;
    GroupMemoryBarrierWithGroupSync();
    if ((local & 3) == 0 && valid) {
        uint packed = shadowValues[local] | (shadowValues[local+1] << 8)
            | (shadowValues[local+2] << 16) | (shadowValues[local+3] << 24);
        outputMask.Store(id.y * ((width+3)&~3U) + id.x, packed);
    }
}
