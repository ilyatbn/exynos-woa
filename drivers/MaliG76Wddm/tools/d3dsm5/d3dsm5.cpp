/*
 * d3dsm5 - G34 shader model 5.0 check (notes/g34-fl11.md): cs_5_0 compute shaders on the Mali D3D11 UMD against WARP,
 * both at feature level 11_0. Each test writes its results to a raw UAV (u0) that is compared word for word.
 * The UMD reports FL 11_0 by default since Mesa 0218; "-native" (formerly: skip the D3D10UMD_FL11_0=1 override) is accepted
 * and ignored.
 *   bits       countbits / firstbithigh / firstbitlow / reversebits (signed and unsigned)
 *   bfe        bitfield extract / insert patterns (fxc: ubfe / ibfe / bfi)
 *   f16        f32tof16 / f16tof32
 *   rcp        rcp() (1 ULP tolerance, D3D11.3 spec 3.1.3)
 *   msad       msad4()
 *   swapc      a 4-element sorting network (fxc may emit swapc)
 *   bufinfo    RWByteAddressBuffer / RWStructuredBuffer GetDimensions
 *   typedbuf   RWBuffer<uint> (R32_UINT typed UAV): store, then typed load
 *   typedtex   RWTexture2D<uint> store + load
 *   typedatomic InterlockedAdd / Max on RWTexture2D<uint> texels
 *   rgba8store RWTexture2D<float4> stores into an R8G8B8A8_UNORM texture (read back texel by texel)
 *   atomics    InterlockedAdd / Max / Min / And / Or / Xor / CompareStore on a raw UAV
 *   immatomic  InterlockedAdd / Exchange / CompareExchange with the original value returned
 *   atomics2   signed InterlockedMax/Min on a RWStructuredBuffer<int>, value-returning And/Or/Xor/Max/Min
 *   tgsmatomic groupshared InterlockedAdd / Max
 *   uav8       eight UAVs u0..u7 written in one shader
 *   psuav      pixel shader: raw UAV stores per pixel + an atomic pixel count (OMSetRenderTargetsAndUnorderedAccessViews)
 *   psderiv    pixel shader: ddx/ddy coarse and fine written to a UAV
 *   append     AppendStructuredBuffer.Append + initial count 5 + CopyStructureCount (contents compared sorted)
 *   consume    ConsumeStructuredBuffer.Consume from a prefilled buffer with initial count 64
 *   drawind    DrawInstancedIndirect with arguments written by a compute shader
 *   drawindx   DrawIndexedInstancedIndirect (16-bit indices 0 1 2), arguments from a compute shader
 *   group1024  numthreads(1024,1,1) groupshared reduction with barriers
 *   groupz64   numthreads(1,1,64) SV_GroupThreadID.z
 * Usage: d3dsm5 [-native] [test ... | -caps]. -caps prints format support (+ SUPPORT2) on Mali vs WARP. Exit 0 = every selected test matches. Build: build-d3dsm5.cmd.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <d3d11.h>
#include <d3dcompiler.h>

enum Extra { EX_NONE, EX_TYPEDBUF, EX_TYPEDTEX, EX_UAV8, EX_STRUCTINT, EX_RGBA8TEX, EX_PS, EX_APPEND, EX_CONSUME, EX_PSIND, EX_PSINDX };

struct Test {
    const char *name;
    const char *hlsl;
    UINT out_words;
    UINT gx, gy, gz;
    Extra extra;
};

static const Test g_tests[] = {
    { "bits",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint v = src.Load(tid.x * 4) * 2654435761u;\n"
      "  int s = (int)v;\n"
      "  dst.Store4(tid.x * 16, uint4(countbits(v), firstbithigh(v), firstbitlow(v), reversebits(v)));\n"
      "  dst.Store(4096 + tid.x * 4, (uint)firstbithigh(s));\n"
      "}\n", 64 * 4 + 64, 1, 1, 1, EX_NONE },
    { "bfe",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint v = src.Load(tid.x * 4) * 2246822519u;\n"
      "  uint off = tid.x & 31u, w = (tid.x * 13u + 5u) & 31u;   /* offset + width reaches 62 (D3D: bits past 31 dropped) */\n"
      "  uint ue = (v >> off) & ((1u << w) - 1u);\n"
      "  int ie = ((int)(v << (32u - off - w))) >> (32u - w);\n"
      "  uint m = ((1u << w) - 1u) << off;\n"
      "  uint bi = (v & ~m) | ((tid.x << off) & m);\n"
      "  dst.Store4(tid.x * 16, uint4(ue, (uint)ie, bi, (v >> 7) & 0x3Fu));\n"
      "}\n", 64 * 4, 1, 1, 1, EX_NONE },
    { "f16",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  float f = ((float)src.Load(tid.x * 4) - 200.0) * 0.37;\n"
      "  uint h = f32tof16(f);\n"
      "  dst.Store2(tid.x * 8, uint2(h, asuint(f16tof32(h ^ (tid.x << 3)))));\n"
      "}\n", 64 * 2, 1, 1, 1, EX_NONE },
    { "f16swz",   /* review R3: fxc tends to emit f32tof16 r0.xyzw, r0.wzyx here (dst = swizzled src) */
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  float4 f = (float4)src.Load4((tid.x & 63u) * 16) * float4(0.25, -1.5, 3.0, 0.125);\n"
      "  uint4 h = f32tof16(f.wzyx);\n"
      "  float4 g = f16tof32(h.yxwz);\n"
      "  dst.Store4(tid.x * 32, h);\n"
      "  dst.Store4(tid.x * 32 + 16, asuint(g));\n"
      "}\n", 64 * 8, 1, 1, 1, EX_NONE },
    { "rcp",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  float f = (float)src.Load(tid.x * 4) * 0.25 + 1.0;\n"
      "  dst.Store(tid.x * 4, asuint(rcp(f)));\n"
      "}\n", 64, 1, 1, 1, EX_NONE },
    { "msad",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint a = src.Load(tid.x * 4) * 2654435761u, b = src.Load((tid.x ^ 5u) * 4) * 40503u;\n"
      "  dst.Store4(tid.x * 16, msad4(a, uint2(b, b ^ 0x80808080u), uint4(tid.x, 1, 2, 3)));\n"
      "}\n", 64 * 4, 1, 1, 1, EX_NONE },
    { "swapc",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint4 v = src.Load4((tid.x & 63u) * 16) * uint4(2654435761u, 40503u, 2246822519u, 3266489917u);\n"
      "  [unroll] for (uint i = 0; i < 3; i++) { [unroll] for (uint j = 0; j < 3 - i; j++) {\n"
      "    if (v[j] > v[j + 1]) { uint t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; } } }\n"
      "  dst.Store4(tid.x * 16, v);\n"
      "}\n", 64 * 4, 1, 1, 1, EX_NONE },
    { "bufinfo",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(1,1,1)] void main() {\n"
      "  uint n; dst.GetDimensions(n);\n"
      "  dst.Store(0, n);\n"
      "}\n", 16, 1, 1, 1, EX_NONE },
    { "typedbuf",
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWBuffer<uint> tb : register(u1);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  tb[tid.x] = tid.x * 97u + 5u;\n"
      "  DeviceMemoryBarrierWithGroupSync();\n"
      "  uint v = tb[tid.x ^ 1u];\n"
      "  dst.Store(tid.x * 4, v + 1u);\n"
      "}\n", 64, 1, 1, 1, EX_TYPEDBUF },
    { "typedtex",
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWTexture2D<uint> tt : register(u1);\n"
      "[numthreads(8,8,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  tt[tid.xy] = (tid.y << 16) | tid.x;\n"
      "  DeviceMemoryBarrierWithGroupSync();\n"
      "  uint v = tt[tid.xy ^ uint2(1, 0)];\n"
      "  dst.Store((tid.y * 8 + tid.x) * 4, v ^ 0x55u);\n"
      "}\n", 64, 1, 1, 1, EX_TYPEDTEX },
    { "bufinfotyped",   /* review R6: GetDimensions on a typed RWBuffer = element count (64) */
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWBuffer<uint> tb : register(u1);\n"
      "[numthreads(1,1,1)] void main() {\n"
      "  uint n; tb.GetDimensions(n);\n"
      "  dst.Store(0, n);\n"
      "}\n", 4, 1, 1, 1, EX_TYPEDBUF },
    { "typedatomic",   /* image atomics on R32_UINT RWTexture2D / RWBuffer */
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWTexture2D<uint> tt : register(u1);\n"
      "[numthreads(8,8,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  InterlockedAdd(tt[uint2(tid.x & 3u, 0)], tid.y + 1u);\n"
      "  uint o; InterlockedMax(tt[uint2(7, 7)], tid.x * 8u + tid.y, o);\n"
      "  DeviceMemoryBarrierWithGroupSync();\n"
      "  if (tid.y == 0) dst.Store(tid.x * 4, tt[uint2(tid.x, 0)]);\n"
      "  if (tid.x == 0 && tid.y == 0) dst.Store(32, tt[uint2(7, 7)]);\n"
      "}\n", 16, 1, 1, 1, EX_TYPEDTEX },
    { "rgba8store",   /* RWTexture2D<float4> on R8G8B8A8_UNORM; the texture itself is read back */
      "RWTexture2D<float4> tt : register(u1);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(8,8,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  tt[tid.xy] = float4(tid.x / 7.0, tid.y / 7.0, (tid.x ^ tid.y) / 7.0, 0.5);\n"
      "  dst.Store(0, 1u);\n"
      "}\n", 64, 1, 1, 1, EX_RGBA8TEX },
    /* pixel shaders (EX_PS): a full-screen triangle on a 16x16 RGBA8 target, the PS's UAV u1 = the compared buffer */
    { "psuav",
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  dst.Store((p.y * 16 + p.x) * 4 + 16, (p.x << 16) | p.y);\n"
      "  dst.InterlockedAdd(0, 1u);\n"
      "  return float4(1, 0, 0, 1);\n"
      "}\n", 4 + 256, 1, 1, 1, EX_PS },
    { "psderiv2",   /* isolates derivative inputs: ddx(x), ddx(x*x), ddx_fine(x*x), ddy(y*y) */
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  float a = pos.x * pos.x, c = pos.y * pos.y;\n"
      "  dst.Store4((p.y * 16 + p.x) * 16, asuint(float4(ddx(pos.x), ddx(a), ddx_fine(a), ddy(c))));\n"
      "  return float4(a, c, 0, 1);\n"
      "}\n", 4 * 256, 1, 1, 1, EX_PS },
    { "psposval",   /* SV_Position values themselves: x, y, x*x and ddx(x*x) */
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  float a = pos.x * pos.x;\n"
      "  dst.Store4((p.y * 16 + p.x) * 16, asuint(float4(pos.x, pos.y, a, ddx(a))));\n"
      "  return float4(a, 0, 0, 1);\n"
      "}\n", 4 * 256, 1, 1, 1, EX_PS },
    { "psderiv3",   /* the same on an interpolated TEXCOORD (= pixel coordinates) instead of SV_Position */
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position, float2 t : TEXCOORD0) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  float a = t.x * t.x, c = t.y * t.y;\n"
      "  dst.Store4((p.y * 16 + p.x) * 16, asuint(float4(ddx(t.x), ddx(a), ddx_fine(a), ddy(c))));\n"
      "  return float4(a, c, 0, 1);\n"
      "}\n", 4 * 256, 1, 1, 1, EX_PS },
    { "psderiv",
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  float f = pos.x * pos.x * 0.5 + pos.y * pos.x * 0.25 + pos.y * 3.0;\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  dst.Store4((p.y * 16 + p.x) * 16, asuint(float4(ddx_coarse(f), ddx_fine(f), ddy_coarse(f), ddy_fine(f))));\n"
      "  return float4(f, 0, 0, 1);\n"
      "}\n", 4 * 256, 1, 1, 1, EX_PS },
    /* append / consume (EX_APPEND / EX_CONSUME): u1 = a 128-element uint structured buffer with the APPEND flag,
     * bound with an initial count (5 / 64); afterwards out[0] = CopyStructureCount, out[1..64] = the touched elements
     * sorted (the order of appends is undefined) */
    { "append",
      "AppendStructuredBuffer<uint> ab : register(u1);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  ab.Append(tid.x * 3u + 1u);\n"
      "}\n", 1 + 64, 1, 1, 1, EX_APPEND },
    { "consume",
      "ConsumeStructuredBuffer<uint> cb : register(u1);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(32,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  dst.Store(4 + tid.x * 4, cb.Consume());\n"
      "}\n", 1 + 64, 1, 1, 1, EX_CONSUME },
    /* indirect draws (EX_PSIND / EX_PSINDX): a compute shader writes the arguments {3 vertices / indices, 1 instance}
     * into a DRAWINDIRECT_ARGS buffer, then DrawInstancedIndirect / DrawIndexedInstancedIndirect (indices 0 1 2) draw
     * the full-screen triangle with this pixel shader */
    { "drawind",
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  dst.Store((p.y * 16 + p.x) * 4 + 16, p.x + p.y * 100u);\n"
      "  dst.InterlockedAdd(0, 1u);\n"
      "  return float4(0, 1, 0, 1);\n"
      "}\n", 4 + 256, 1, 1, 1, EX_PSIND },
    { "drawindx",
      "RWByteAddressBuffer dst : register(u1);\n"
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  uint2 p = (uint2)pos.xy;\n"
      "  dst.Store((p.y * 16 + p.x) * 4 + 16, p.x * 3u + p.y);\n"
      "  dst.InterlockedAdd(0, 1u);\n"
      "  return float4(0, 0, 1, 1);\n"
      "}\n", 4 + 256, 1, 1, 1, EX_PSINDX },
    { "atomics",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  dst.InterlockedAdd(0, tid.x + 1u);\n"
      "  dst.InterlockedMax(4, tid.x * 3u);\n"
      "  dst.InterlockedMin(8, 1000u - tid.x);\n"
      "  dst.InterlockedOr(12, 1u << (tid.x & 31u));\n"
      "  dst.InterlockedXor(16, tid.x * 0x01010101u);\n"
      "  dst.InterlockedAnd(20, ~(1u << (tid.x & 31u)) | 0xF0000000u);\n"
      "  dst.InterlockedCompareStore(24, 0u, 77u);\n"
      "}\n", 8, 2, 1, 1, EX_NONE },
    { "immatomic",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint o;\n"
      "  dst.InterlockedAdd(0, 1u, o);\n"
      "  dst.Store(64 + o * 4, 1u);\n"   /* every slot 0..63 written once, whatever the order */
      "  uint x; dst.InterlockedExchange(4, tid.x, x);\n"
      "  uint c; dst.InterlockedCompareExchange(8, 0u, 5u, c);\n"
      "  dst.InterlockedAdd(12, c == 0u ? 1u : 0u);\n"   /* exactly one thread saw 0 */
      "}\n", 16 + 64, 1, 1, 1, EX_NONE },
    { "atomics2",   /* review R5: signed min/max and the value-returning and/or/xor/min/max forms */
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWStructuredBuffer<int> si : register(u1);\n"
      "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  int v = (int)tid.x - 40;\n"
      "  InterlockedMax(si[0], v * 3);\n"
      "  InterlockedMin(si[1], v * 5);\n"
      "  int o;\n"
      "  InterlockedMax(si[2], v, o);\n"
      "  InterlockedMin(si[3], -v, o);\n"
      "  uint a, b, c, d, e;\n"
      "  dst.InterlockedAnd(64, ~(1u << (tid.x & 31u)), a);\n"
      "  dst.InterlockedOr(68, 1u << (tid.x & 31u), b);\n"
      "  dst.InterlockedXor(72, tid.x, c);\n"
      "  dst.InterlockedMax(76, tid.x * 7u, d);\n"
      "  dst.InterlockedMin(80, 500u - tid.x, e);\n"
      "  dst.InterlockedAdd(84, (a != 0xDEADu) + (b != 0xDEADu) + (c != 0xDEADu) + (d != 0xDEADu) + (e != 0xDEADu));\n"
      "  DeviceMemoryBarrierWithGroupSync();\n"
      "  if (tid.x == 0) dst.Store4(0, asuint(int4(si[0], si[1], si[2], si[3])));\n"
      "}\n", 32, 1, 1, 1, EX_STRUCTINT },
    { "tgsmatomic",
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint s[2];\n"
      "[numthreads(64,1,1)] void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {\n"
      "  if (gi < 2) s[gi] = 0;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  InterlockedAdd(s[0], gi * 2u + gid.x);\n"
      "  InterlockedMax(s[1], gi ^ 0x2Au);\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  if (gi == 0) dst.Store2(gid.x * 8, uint2(s[0], s[1]));\n"
      "}\n", 8, 4, 1, 1, EX_NONE },
    { "uav8",
      "RWByteAddressBuffer dst : register(u0);\n"
      "RWByteAddressBuffer o1 : register(u1); RWByteAddressBuffer o2 : register(u2);\n"
      "RWByteAddressBuffer o3 : register(u3); RWByteAddressBuffer o4 : register(u4);\n"
      "RWByteAddressBuffer o5 : register(u5); RWByteAddressBuffer o6 : register(u6);\n"
      "RWByteAddressBuffer o7 : register(u7);\n"
      "[numthreads(16,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
      "  o1.Store(tid.x * 4, 1u); o2.Store(tid.x * 4, 2u); o3.Store(tid.x * 4, 3u); o4.Store(tid.x * 4, 4u);\n"
      "  o5.Store(tid.x * 4, 5u); o6.Store(tid.x * 4, 6u); o7.Store(tid.x * 4, 7u);\n"
      "  DeviceMemoryBarrier();\n"
      "  uint s = o1.Load(tid.x * 4) + o2.Load(tid.x * 4) * 10u + o3.Load(tid.x * 4) * 100u + o4.Load(tid.x * 4) * 1000u\n"
      "         + o5.Load(tid.x * 4) * 10000u + o6.Load(tid.x * 4) * 100000u + o7.Load(tid.x * 4) * 1000000u;\n"
      "  dst.Store(tid.x * 4, s);\n"
      "}\n", 16, 1, 1, 1, EX_UAV8 },
    { "group384",   /* the hardware's THREAD_MAX_WORKGROUP_SZ: runs natively */
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint sh[384];\n"
      "[numthreads(384,1,1)] void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {\n"
      "  sh[gi] = src.Load(gi * 4) + gid.x;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  if (gi < 128) sh[gi] += sh[gi + 128] + sh[gi + 256];\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  for (uint st = 64; st > 0; st >>= 1) {\n"
      "    if (gi < st) sh[gi] += sh[gi + st];\n"
      "    GroupMemoryBarrierWithGroupSync();\n"
      "  }\n"
      "  if (gi == 0) dst.Store(gid.x * 4, sh[0]);\n"
      "}\n", 4, 2, 1, 1, EX_NONE },
    { "group1024",
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint sh[1024];\n"
      "[numthreads(1024,1,1)] void main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {\n"
      "  sh[gi] = src.Load((gi & 1023u) * 4) + gid.x;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  for (uint st = 512; st > 0; st >>= 1) {\n"
      "    if (gi < st) sh[gi] += sh[gi + st];\n"
      "    GroupMemoryBarrierWithGroupSync();\n"
      "  }\n"
      "  if (gi == 0) dst.Store(gid.x * 4, sh[0]);\n"
      "}\n", 4, 2, 1, 1, EX_NONE },
    { "group512nb",   /* 512 threads, no barrier: SV_GroupThreadID / SV_DispatchThreadID / SV_GroupIndex */
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(16,32,1)] void main(uint3 gt : SV_GroupThreadID, uint3 tid : SV_DispatchThreadID,\n"
      "                               uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {\n"
      "  dst.Store((gid.x * 512 + gi) * 4, (gt.x << 24) | (gt.y << 16) | (tid.x << 8) | tid.y);\n"
      "}\n", 1024, 2, 1, 1, EX_NONE },
    { "tile1024",   /* 32x32 groupshared tile transpose (1024 threads, one barrier) */
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint tile[32][33];\n"
      "[numthreads(32,32,1)] void main(uint3 gt : SV_GroupThreadID) {\n"
      "  tile[gt.y][gt.x] = src.Load(((gt.y * 32 + gt.x) & 1023u) * 4) ^ (gt.x << 20);\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  dst.Store((gt.y * 32 + gt.x) * 4, tile[gt.x][gt.y]);\n"
      "}\n", 1024, 1, 1, 1, EX_NONE },
    { "group768ret",   /* 768 threads: a barrier, then a per-thread early return */
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint cnt;\n"
      "[numthreads(768,1,1)] void main(uint gi : SV_GroupIndex) {\n"
      "  if (gi == 0) cnt = 0;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  InterlockedAdd(cnt, 1u);\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  if (gi % 3u == 1u) return;\n"
      "  dst.Store(gi * 4, cnt + gi);\n"
      "}\n", 768, 1, 1, 1, EX_NONE },
    { "group512arr",   /* review of 0213: an indexable temp (dynamic index) live across a barrier in a 512 group */
      "ByteAddressBuffer src : register(t0);\n"
      "RWByteAddressBuffer dst : register(u0);\n"
      "groupshared uint sh[512];\n"
      "[numthreads(512,1,1)] void main(uint gi : SV_GroupIndex) {\n"
      "  uint a[8];\n"
      "  [loop] for (uint i = 0; i < 8; i++) a[(i + gi) & 7u] = src.Load(((gi + i) & 1023u) * 4) * (i + 1u);\n"
      "  sh[gi] = a[gi & 7u];\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  uint k = sh[(gi + 1u) & 511u] & 7u;\n"
      "  dst.Store(gi * 4, a[k] + a[(k + 3u) & 7u] + sh[511u - gi]);\n"
      "}\n", 512, 1, 1, 1, EX_NONE },
    { "groupz64",
      "RWByteAddressBuffer dst : register(u0);\n"
      "[numthreads(1,1,64)] void main(uint3 gt : SV_GroupThreadID, uint3 gid : SV_GroupID) {\n"
      "  dst.Store((gid.x * 64 + gt.z) * 4, gt.z * 3u + gid.x);\n"
      "}\n", 128, 2, 1, 1, EX_NONE },
};
static const int NTEST = sizeof(g_tests) / sizeof(g_tests[0]);

static UINT g_in[1024];

static HRESULT RawBuffer(ID3D11Device *dev, UINT words, UINT bind, const void *init, ID3D11Buffer **b)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = words * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA sd = { init, 0, 0 };
    return dev->CreateBuffer(&bd, init ? &sd : NULL, b);
}

static HRESULT RawUav(ID3D11Device *dev, ID3D11Buffer *b, UINT words, ID3D11UnorderedAccessView **u)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.Buffer.NumElements = words;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return dev->CreateUnorderedAccessView(b, &ud, u);
}

static bool RunTest(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Test *t, ID3DBlob *code, UINT *out)
{
    HRESULT hr;
    ID3D11ComputeShader *cs = NULL;
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &cs);
    if (FAILED(hr)) { printf("    CreateComputeShader 0x%08lx\n", (unsigned long)hr); return false; }

    ID3D11Buffer *inbuf = NULL, *outbuf = NULL, *st = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11UnorderedAccessView *uav[8] = {};
    ID3D11Resource *extra[8] = {};
    hr = RawBuffer(dev, 1024, D3D11_BIND_SHADER_RESOURCE, g_in, &inbuf);
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.Format = DXGI_FORMAT_R32_TYPELESS;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        vd.BufferEx.NumElements = 1024;
        vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr = dev->CreateShaderResourceView(inbuf, &vd, &srv);
    }
    if (SUCCEEDED(hr)) hr = RawBuffer(dev, t->out_words, D3D11_BIND_UNORDERED_ACCESS, NULL, &outbuf);
    if (SUCCEEDED(hr)) hr = RawUav(dev, outbuf, t->out_words, &uav[0]);
    UINT nuav = 1;
    if (SUCCEEDED(hr) && t->extra == EX_TYPEDBUF) {
        D3D11_BUFFER_DESC bd = { 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };
        ID3D11Buffer *b = NULL;
        hr = dev->CreateBuffer(&bd, NULL, &b);
        extra[1] = b;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.Buffer.NumElements = 64;
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(b, &ud, &uav[1]);
        nuav = 2;
    } else if (SUCCEEDED(hr) && t->extra == EX_TYPEDTEX) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 8; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D *tx = NULL;
        hr = dev->CreateTexture2D(&td, NULL, &tx);
        extra[1] = tx;
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(tx, NULL, &uav[1]);
        nuav = 2;
    } else if (SUCCEEDED(hr) && t->extra == EX_RGBA8TEX) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 8; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D *tx = NULL;
        hr = dev->CreateTexture2D(&td, NULL, &tx);
        extra[1] = tx;
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(tx, NULL, &uav[1]);
        nuav = 2;
    } else if (SUCCEEDED(hr) && (t->extra == EX_APPEND || t->extra == EX_CONSUME)) {
        D3D11_BUFFER_DESC bd = { 128 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 };
        UINT init[128];
        for (UINT k = 0; k < 128; k++)
            init[k] = 1000u + k * 7u;
        D3D11_SUBRESOURCE_DATA sdi = { init, 0, 0 };
        ID3D11Buffer *b = NULL;
        hr = dev->CreateBuffer(&bd, &sdi, &b);
        extra[1] = b;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.Buffer.NumElements = 128;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(b, &ud, &uav[1]);
        nuav = 2;
    } else if (SUCCEEDED(hr) && t->extra == EX_STRUCTINT) {
        D3D11_BUFFER_DESC bd = { 4 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 };
        ID3D11Buffer *b = NULL;
        hr = dev->CreateBuffer(&bd, NULL, &b);
        extra[1] = b;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.Buffer.NumElements = 4;
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(b, &ud, &uav[1]);
        nuav = 2;
    } else if (SUCCEEDED(hr) && t->extra == EX_UAV8) {
        for (UINT k = 1; k < 8 && SUCCEEDED(hr); k++) {
            ID3D11Buffer *b = NULL;
            hr = RawBuffer(dev, 16, D3D11_BIND_UNORDERED_ACCESS, NULL, &b);
            extra[k] = b;
            if (SUCCEEDED(hr)) hr = RawUav(dev, b, 16, &uav[k]);
        }
        nuav = 8;
    }
    D3D11_BUFFER_DESC sd = { t->out_words * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&sd, NULL, &st);
    if (FAILED(hr)) { printf("    resources 0x%08lx\n", (unsigned long)hr); return false; }

    UINT zero[4] = { 0, 0, 0, 0 };
    for (UINT k = 0; k < nuav; k++)
        ctx->ClearUnorderedAccessViewUint(uav[k], zero);
    if (!strcmp(t->name, "atomics")) {   /* start values the min / and operations can lower */
        UINT init[8] = { 0, 0, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, 0, 0 };
        ctx->UpdateSubresource(outbuf, 0, NULL, init, 0, 0);
    }
    if (!strcmp(t->name, "atomics2")) {   /* and / min start from all ones */
        UINT init[32] = {};
        init[16] = 0xFFFFFFFFu;
        init[20] = 0xFFFFFFFFu;
        ctx->UpdateSubresource(outbuf, 0, NULL, init, 0, 0);
    }
    ctx->CSSetShader(cs, NULL, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    UINT counts[8] = { ~0u, t->extra == EX_APPEND ? 5u : 64u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
    bool counted = t->extra == EX_APPEND || t->extra == EX_CONSUME;
    ctx->CSSetUnorderedAccessViews(0, nuav, uav, counted ? counts : NULL);
    ctx->Dispatch(t->gx, t->gy, t->gz);
    if (counted) {   /* out[0] = the hidden counter */
        ctx->CopyStructureCount(outbuf, 0, uav[1]);
    }
    ID3D11UnorderedAccessView *none[8] = {};
    ctx->CSSetUnorderedAccessViews(0, 8, none, NULL);
    if (t->extra == EX_RGBA8TEX) {   /* compare the texels: copy the texture through a staging texture into out */
        D3D11_TEXTURE2D_DESC td = {};
        ((ID3D11Texture2D *)extra[1])->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *stt = NULL;
        if (SUCCEEDED(dev->CreateTexture2D(&td, NULL, &stt))) {
            ctx->CopyResource(stt, extra[1]);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(stt, 0, D3D11_MAP_READ, 0, &m))) {
                for (UINT y = 0; y < 8; y++)
                    memcpy(out + y * 8, (const BYTE *)m.pData + y * m.RowPitch, 32);
                ctx->Unmap(stt, 0);
            }
            stt->Release();
        }
        for (UINT k = 0; k < 8; k++) {
            if (uav[k]) uav[k]->Release();
            if (extra[k]) extra[k]->Release();
        }
        srv->Release(); inbuf->Release(); outbuf->Release(); st->Release(); cs->Release();
        return true;
    }
    ctx->CopyResource(st, outbuf);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("    Map 0x%08lx\n", (unsigned long)hr); return false; }
    memcpy(out, map.pData, t->out_words * 4);
    ctx->Unmap(st, 0);
    if (t->extra == EX_APPEND) {   /* the 64 elements after the initial count 5, sorted */
        D3D11_BUFFER_DESC sd2 = { 128 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        ID3D11Buffer *s2 = NULL;
        if (SUCCEEDED(dev->CreateBuffer(&sd2, NULL, &s2))) {
            ctx->CopyResource(s2, extra[1]);
            D3D11_MAPPED_SUBRESOURCE m2;
            if (SUCCEEDED(ctx->Map(s2, 0, D3D11_MAP_READ, 0, &m2))) {
                memcpy(out + 1, (const UINT *)m2.pData + 5, 64 * 4);
                ctx->Unmap(s2, 0);
            }
            s2->Release();
        }
    }
    if (counted)   /* order of appends / consumes is undefined: sort the payload */
        qsort(out + 1, t->out_words - 1, 4, [](const void *a, const void *b) {
            UINT x = *(const UINT *)a, y = *(const UINT *)b;
            return x < y ? -1 : x > y;
        });
    for (UINT k = 0; k < 8; k++) {
        if (uav[k]) uav[k]->Release();
        if (extra[k]) extra[k]->Release();
    }
    srv->Release(); inbuf->Release(); outbuf->Release(); st->Release(); cs->Release();
    return true;
}

static ID3D11VertexShader *g_vs[2];

/* EX_PS: draw a full-screen triangle with the test's pixel shader; its UAV u1 (slot 1, after the one RTV) is read back */
static bool RunPsTest(ID3D11Device *dev, ID3D11DeviceContext *ctx, int which, const Test *t, ID3DBlob *code, UINT *out)
{
    HRESULT hr;
    if (!g_vs[which]) {
        static const char vs[] =
            "struct O { float4 pos : SV_Position; float2 t : TEXCOORD0; };\n"
            "O main(uint id : SV_VertexID) {\n"
            "  float2 uv = float2((id << 1) & 2, id & 2);\n"
            "  O o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.t = uv * 8.0; return o;\n"
            "}\n";
        ID3DBlob *vb = NULL, *err = NULL;
        hr = D3DCompile(vs, strlen(vs), "vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err);
        if (FAILED(hr) || FAILED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), NULL, &g_vs[which]))) {
            printf("    VS 0x%08lx\n", (unsigned long)hr);
            return false;
        }
        vb->Release();
    }
    ID3D11PixelShader *ps = NULL;
    hr = dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ps);
    if (FAILED(hr)) { printf("    CreatePixelShader 0x%08lx\n", (unsigned long)hr); return false; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 16; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Buffer *outbuf = NULL, *st = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    hr = dev->CreateTexture2D(&td, NULL, &rt);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
    if (SUCCEEDED(hr)) hr = RawBuffer(dev, t->out_words, D3D11_BIND_UNORDERED_ACCESS, NULL, &outbuf);
    if (SUCCEEDED(hr)) hr = RawUav(dev, outbuf, t->out_words, &uav);
    D3D11_BUFFER_DESC sd = { t->out_words * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&sd, NULL, &st);
    if (FAILED(hr)) { printf("    resources 0x%08lx\n", (unsigned long)hr); return false; }
    UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(uav, zero);
    D3D11_VIEWPORT vp = { 0, 0, 16, 16, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(NULL);
    ctx->VSSetShader(g_vs[which], NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ID3D11Buffer *args = NULL, *ib = NULL;
    if (t->extra == EX_PSIND || t->extra == EX_PSINDX) {
        /* the arguments come from a compute shader, as in GPU-driven rendering */
        static const char csa[] =
            "RWByteAddressBuffer a : register(u0);\n"
            "[numthreads(1,1,1)] void main() { a.Store4(0, uint4(3, 1, 0, 0)); a.Store(16, 0); }\n";
        ID3DBlob *cb = NULL, *err = NULL;
        ID3D11ComputeShader *cs = NULL;
        ID3D11UnorderedAccessView *au = NULL;
        D3D11_BUFFER_DESC ad = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
        hr = D3DCompile(csa, strlen(csa), "csa", NULL, NULL, "main", "cs_5_0", 0, 0, &cb, &err);
        if (SUCCEEDED(hr)) hr = dev->CreateComputeShader(cb->GetBufferPointer(), cb->GetBufferSize(), NULL, &cs);
        if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&ad, NULL, &args);
        if (SUCCEEDED(hr)) hr = RawUav(dev, args, 8, &au);
        if (FAILED(hr)) { printf("    indirect args 0x%08lx\n", (unsigned long)hr); return false; }
        ctx->CSSetShader(cs, NULL, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &au, NULL);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView *nu = NULL;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, NULL);
        au->Release(); cs->Release(); cb->Release();
        if (t->extra == EX_PSINDX) {
            USHORT idx[4] = { 0, 1, 2, 0 };
            D3D11_BUFFER_DESC id = { sizeof idx, D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA isd = { idx, 0, 0 };
            if (FAILED(dev->CreateBuffer(&id, &isd, &ib))) return false;
            ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        }
    }
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, NULL, 1, 1, &uav, NULL);
    if (t->extra == EX_PSIND)
        ctx->DrawInstancedIndirect(args, 0);
    else if (t->extra == EX_PSINDX)
        ctx->DrawIndexedInstancedIndirect(args, 0);
    else
        ctx->Draw(3, 0);
    if (args) args->Release();
    if (ib) { ctx->IASetIndexBuffer(NULL, DXGI_FORMAT_R16_UINT, 0); ib->Release(); }
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, 0, 0, NULL, NULL);
    ctx->CopyResource(st, outbuf);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("    Map 0x%08lx\n", (unsigned long)hr); return false; }
    memcpy(out, map.pData, t->out_words * 4);
    ctx->Unmap(st, 0);
    uav->Release(); outbuf->Release(); st->Release(); rtv->Release(); rt->Release(); ps->Release();
    return true;
}

static bool Open(bool warp, IDXGIAdapter *ad, ID3D11Device **dev, ID3D11DeviceContext **ctx)
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = D3D11CreateDevice(warp ? NULL : ad, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                   &fl, 1, D3D11_SDK_VERSION, dev, &got, ctx);
    printf("%s: D3D11CreateDevice(FL 11_0) 0x%08lx FL 0x%x\n", warp ? "WARP" : "Mali", (unsigned long)hr, got);
    return SUCCEEDED(hr);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a GPU hang must not lose the lines before it */
    int first_sel = 1;
    if (argc > 1 && !strcmp(argv[1], "-native"))
        first_sel = 2;
    for (UINT i = 0; i < 1024; i++)
        g_in[i] = i * 7u + 1u;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL, *pick = NULL;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (!pick && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            pick = ad;
        else
            ad->Release();
    }
    ID3D11Device *md = NULL, *wd = NULL;
    ID3D11DeviceContext *mc = NULL, *wc = NULL;
    if (!pick || !Open(false, pick, &md, &mc) || !Open(true, NULL, &wd, &wc))
        return 2;
    if (argc > first_sel && !strcmp(argv[first_sel], "-caps")) {   /* F11.11: format support, Mali vs WARP */
        static const DXGI_FORMAT fmts[] = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R11G11B10_FLOAT,
            DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_D24_UNORM_S8_UINT };
        for (UINT f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
            UINT sm = 0, sw = 0;
            md->CheckFormatSupport(fmts[f], &sm);
            wd->CheckFormatSupport(fmts[f], &sw);
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 m2 = { fmts[f], 0 }, w2 = { fmts[f], 0 };
            md->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &m2, sizeof m2);
            wd->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &w2, sizeof w2);
            printf("fmt %3u: support Mali %08x WARP %08x (missing %08x)  support2 Mali %08x WARP %08x (missing %08x)\n",
                   (unsigned)fmts[f], sm, sw, sw & ~sm, m2.OutFormatSupport2, w2.OutFormatSupport2,
                   w2.OutFormatSupport2 & ~m2.OutFormatSupport2);
        }
        return 0;
    }
    int fails = 0, run = 0;
    static UINT om[4096], ow[4096];
    for (int i = 0; i < NTEST; i++) {
        const Test *t = &g_tests[i];
        bool sel = argc <= first_sel;
        for (int a = first_sel; a < argc; a++)
            sel |= !strcmp(argv[a], t->name);
        if (!sel)
            continue;
        run++;
        ID3DBlob *code = NULL, *err = NULL;
        bool is_ps = t->extra == EX_PS || t->extra == EX_PSIND || t->extra == EX_PSINDX;
        HRESULT hr = D3DCompile(t->hlsl, strlen(t->hlsl), t->name, NULL, NULL, "main",
                                is_ps ? (getenv("D3DSM5_PS40") ? "ps_4_0" : "ps_5_0") : "cs_5_0", 0, 0,
                                &code, &err);   /* D3DSM5_PS40: pixel tests as ps_4_0 (A/B of SM4 vs SM5 opcodes) */
        if (FAILED(hr)) {
            printf("%-11s D3DCompile 0x%08lx %s\n", t->name, (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
            fails++;
            continue;
        }
        memset(om, 0xCD, sizeof om);
        memset(ow, 0xAB, sizeof ow);
        printf("%-11s running...\n", t->name);
        bool okw, okm;
        if (is_ps) {
            okw = RunPsTest(wd, wc, 1, t, code, ow);
            okm = RunPsTest(md, mc, 0, t, code, om);
        } else {
            okw = RunTest(wd, wc, t, code, ow);
            okm = RunTest(md, mc, t, code, om);
        }
        UINT bad = 0, firstbad = 0;
        /* D3D11.3 functional spec 3.1.3: "sqrt and rcp have 1 ULP tolerance" - rcp results may differ from WARP's by
         * one float step; every other test is exact */
        int tol = !strcmp(t->name, "rcp") ? 1 : 0;
        if (okm && okw)
            for (UINT k = 0; k < t->out_words; k++)
                if (abs((int)om[k] - (int)ow[k]) > tol && !bad++)
                    firstbad = k;
        bool ok = okm && okw && !bad;
        printf("%-11s %s", t->name, ok ? "ok" : "DIFF");
        if (okm && okw && bad)
            printf(" (%u of %u words; first [%u] Mali 0x%08x WARP 0x%08x)", bad, t->out_words, firstbad, om[firstbad],
                   ow[firstbad]);
        if (!okm || !okw)
            printf(" (did not run on %s)", !okm ? "Mali" : "WARP");
        printf("  sample [0..3] Mali %08x %08x %08x %08x\n", om[0], om[1], om[2], om[3]);
        fails += !ok;
        code->Release();
    }
    printf("%d of %d tests differ -> %s\n", fails, run, fails ? "FAIL" : "PASS");
    return fails ? 3 : 0;
}
