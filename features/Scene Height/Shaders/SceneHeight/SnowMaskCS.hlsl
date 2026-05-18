// =============================================================================
// SnowMaskCS — Layer 2 of the snow accumulation architecture.
//
// Derives a "snow allowed" mask (R8_UNORM 128x128) from Layer 1 data products:
//   - WaterMaskTex      (Layer 1.1, R32_FLOAT) — water surface Z per texel
//   - SceneOcclusionTex (Layer 1.2, borrowed from Skylighting::texOcclusion) —
//     post-projection depth of topmost opaque surface
//   - skylightingSettings.OcclusionViewProj (cbuffer, borrowed) — world→clip
//   - sceneHeightSettings.WaterMaskWorldOriginX/Y, WaterMaskInvCoverage —
//     the cached window's world-space origin and coverage
//
// Per texel decision:
//   1. Sample water mask at this texel. If water present → mask = 0
//      (snow doesn't accumulate over water surfaces).
//   2. Sample scene occlusion at this texel's world XY (project through
//      OcclusionViewProj). If no scene-height data (Skylighting unloaded or
//      this point is outside its window) → mask = 255 (assume open sky,
//      let snow fall).
//   3. Sample scene occlusion at 4 cardinal neighbors. Compute world-Z
//      gradient (central differences). If the surface is steeper than the
//      slope threshold (cos angle from world-up < kSnowSlopeCosineThreshold)
//      → mask = 0 (snow doesn't stick to walls / cliffsides).
//   4. Otherwise → mask = 255.
//
// Dispatch: once per cell change, gated on water-ready AND scene-height-ready.
// Driven from Deferred::DeferredPasses to ensure Skylighting's RenderOcclusion
// has completed before we sample texOcclusion (avoids the SRV+DSV conflict
// that crashed AMD drivers when we tried persistent binding in Prepass).
//
// Bindings (set by SceneHeight::DispatchSnowMaskCS):
//   t0 = WaterMaskTex       — our own R32_FLOAT 128x128
//   t1 = SceneOcclusionTex  — borrowed Skylighting depth-typed R32_FLOAT
//   s0 = LinearClampSampler — fractional UV sampling on texOcclusion
//   u0 = SnowMaskOut        — output R8_UNORM 128x128 (snow allowed?)
//   u1 = SceneHeightOut     — output R32_FLOAT 128x128 (world-Z values for
//                             CPU readback; eliminates depth-format readback
//                             games on the CPU side)
//   b5 = SharedData cbuffer (provides skylighting + sceneHeight settings)
// =============================================================================

#include "Common/SharedData.hlsli"

Texture2D<float>         WaterMaskTex       : register(t0);
Texture2D<float>         SceneOcclusionTex  : register(t1);
SamplerState             LinearClampSampler : register(s0);
RWTexture2D<unorm float> SnowMaskOut        : register(u0);
RWTexture2D<float>       SceneHeightOut     : register(u1);

// Match SceneHeight::kResolution / kCoverage / kSnowSlopeCosineThreshold.
// Hardcoded here rather than passed through a cbuffer entry because they're
// compile-time constants on the C++ side and shader recompile is cheap.
static const uint   RES                  = 128;
static const float  COVERAGE             = 10000.0;
static const float  TEXEL_SPACING        = COVERAGE / float(RES);  // ≈ 78.125 world units
static const float  SLOPE_COS_THRESHOLD  = 0.342;                  // cos(70°) — see SceneHeight::kSnowSlopeCosineThreshold
static const float  NO_WATER_THRESHOLD   = -1.0e9;                 // matches SceneHeight::kNoWaterSentinel
static const float  NO_SCENE_THRESHOLD   = -1.0e9;

// Project a world-XY position into Skylighting's occlusion-map UV space and
// reconstruct world Z from the sampled depth.
//
// CRITICAL: `OcclusionViewProj` is built with `PosOffset = cellOrigin - eyePos`
// (Skylighting.cpp:191).  Its inputs are CAMERA-RELATIVE world positions, not
// absolute world.  Skyrim's whole shader pipeline works in camera-relative
// coordinates (FrameBuffer::CameraPosAdjust holds the camera's absolute world
// position; subtract from absolute to get camera-relative).  Feeding the
// matrix raw absolute worldXY gives bizarrely-offset UV + Z values — was
// the cause of the "floating slab planes high above terrain" symptom.
//
// Also: Skylighting's projection direction is JITTERED off-nadir for temporal
// AA (MaxZenith setting, ~10° cone).  That makes the matrix's _m20 and _m21
// entries non-zero, so the depth equation is:
//   ndcZ = _m20*x + _m21*y + _m22*z + _m23   (assuming clip.w=1 for ortho)
// Inverting:
//   z_camRel = (ndcZ - _m20*x_camRel - _m21*y_camRel - _m23) / _m22
// Naively dropping the _m20*x and _m21*y terms (as if pure nadir) introduces
// error proportional to distance from the projection center — invisible
// at the player's position, severe at slab vertices a thousand units away.
float SampleSceneZ(float2 worldXY)
{
	// World → camera-relative XY.  Z input is 0 because orthographic X/Y
	// projection is independent of input Z (M[0][2] = M[1][2] = 0).
	float3 camRel = float3(worldXY, 0.0) - FrameBuffer::CameraPosAdjust[0].xyz;

	float4 clipPos = mul(SharedData::skylightingSettings.OcclusionViewProj,
		float4(camRel, 1.0));
	float2 ndc = clipPos.xy / clipPos.w;
	float2 uv  = ndc * 0.5 + 0.5;
	uv.y = 1.0 - uv.y;  // D3D: clip y up, UV y down

	if (any(uv < 0.0) || any(uv > 1.0))
		return -1.0e10;

	float ndcZ = SceneOcclusionTex.SampleLevel(LinearClampSampler, uv, 0);

	// Full inverse-projection including the X/Y dependence of the depth.
	float4x4 M = SharedData::skylightingSettings.OcclusionViewProj;
	if (abs(M._m22) < 1e-6)
		return -1.0e10;

	float zCamRel = (ndcZ - M._m20 * camRel.x - M._m21 * camRel.y - M._m23) / M._m22;

	// Camera-relative → absolute world Z.  CPU-side SampleSceneHeightCPU
	// expects absolute world Z values (slab vertex math uses
	// `slab.triShape->world.translate.z` in absolute world space).
	return zCamRel + FrameBuffer::CameraPosAdjust[0].z;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 tc = DTid.xy;
	if (any(tc >= RES))
		return;

	// World-XY at the texel's CENTER (shifted by 0.5 to match the CPU water-fill
	// loop in SceneHeight.cpp's FillWaterMaskRows — same convention so both
	// products align in world space).
	float worldX = SharedData::sceneHeightSettings.WaterMaskWorldOriginX
	             + (float(tc.x) + 0.5) * TEXEL_SPACING;
	float worldY = SharedData::sceneHeightSettings.WaterMaskWorldOriginY
	             + (float(tc.y) + 0.5) * TEXEL_SPACING;

	// Default the scene-height output to the sentinel — we may early-out
	// below before computing zC.  CPU readback distinguishes "no data" via
	// this sentinel (matches SceneHeight::kNoSceneHeightSentinel).
	SceneHeightOut[tc] = -1.0e10;

	// ---- 1) Water check ----
	// Water mask is aligned 1:1 with this CS output, so direct texel Load.
	float waterZ = WaterMaskTex.Load(int3(tc, 0));
	if (waterZ > NO_WATER_THRESHOLD) {
		SnowMaskOut[tc]    = 0.0;  // submerged / over a water body
		SceneHeightOut[tc] = waterZ;  // surface IS the water level (useful for ice-float later)
		return;
	}

	// ---- 2) Scene-height availability ----
	// If Skylighting isn't producing texOcclusion, we can't do the slope check.
	// Fall back to "allow snow" (graceful — terrain ground will get snow,
	// walls/cliffs miss out but at least snow appears somewhere).
	if (SharedData::sceneHeightSettings.SceneHeightReady < 0.5) {
		SnowMaskOut[tc] = 1.0;
		// SceneHeightOut already set to sentinel above.
		return;
	}

	// ---- 3) Scene-height sample at center + 4 neighbors for slope ----
	float zC = SampleSceneZ(float2(worldX, worldY));
	if (zC < NO_SCENE_THRESHOLD) {
		// Outside Skylighting's coverage window — no occluder data.  Treat as
		// open sky.  This lets snow appear far from the player (mask doesn't
		// have to know about distant cells) without false-negative rejection.
		SnowMaskOut[tc] = 1.0;
		// SceneHeightOut already set to sentinel.
		return;
	}

	// Publish the world-Z to the readback texture now that we know it's valid.
	SceneHeightOut[tc] = zC;

	float zR = SampleSceneZ(float2(worldX + TEXEL_SPACING, worldY));
	float zL = SampleSceneZ(float2(worldX - TEXEL_SPACING, worldY));
	float zU = SampleSceneZ(float2(worldX, worldY + TEXEL_SPACING));
	float zD = SampleSceneZ(float2(worldX, worldY - TEXEL_SPACING));

	// Out-of-bounds neighbors fall back to the center sample — gradient is
	// zero on the missing side, which gracefully degrades to "less steep"
	// (more permissive) at window edges rather than introducing artifacts.
	if (zR < NO_SCENE_THRESHOLD) zR = zC;
	if (zL < NO_SCENE_THRESHOLD) zL = zC;
	if (zU < NO_SCENE_THRESHOLD) zU = zC;
	if (zD < NO_SCENE_THRESHOLD) zD = zC;

	// Central differences over a 2-texel span.
	float dzdx = (zR - zL) / (2.0 * TEXEL_SPACING);
	float dzdy = (zU - zD) / (2.0 * TEXEL_SPACING);

	// Surface normal up-component = 1 / sqrt(dzdx² + dzdy² + 1).
	// Slope test: reject when up-component < cos(angle threshold).
	float gradMagSq = dzdx * dzdx + dzdy * dzdy;
	float upCos     = rsqrt(gradMagSq + 1.0);

	if (upCos < SLOPE_COS_THRESHOLD) {
		SnowMaskOut[tc] = 0.0;  // surface too steep
		return;
	}

	// All checks passed.
	SnowMaskOut[tc] = 1.0;
}
