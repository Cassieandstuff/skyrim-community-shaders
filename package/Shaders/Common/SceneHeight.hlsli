// =============================================================================
// SceneHeight.hlsli — shader-side access to the SceneHeight subsystem's data
// products.  Include from any feature that needs spatial scene queries (snow
// accumulation, ice floes, water-aware effects, etc.).
//
// Layer 1.1 (this iteration):
//   - SampleWaterHeight(worldXY) — returns water surface Z at (x,y), or a
//     large negative sentinel when no water body covers that point.
//   - IsOverWater(worldXY, worldZ) — true when (x,y,z) is at or below the
//     local water surface (i.e., the point is in / under water).
//
// Layer 1.2:
//   - SampleSceneHeight(worldXY) — returns world-space Z of the topmost
//     opaque surface (terrain + statics) by sampling Skylighting's
//     texOcclusion (borrowed at t37) and inverse-projecting via Skylighting's
//     OcclusionViewProj matrix (read from skylightingSettings cbuffer).
//   - IsOccluderAbove(worldXY, worldZ) — true when there's an opaque surface
//     above the query point.  Used by snow logic to suppress accumulation
//     under roofs / overhangs.
//
// Layer 2 (this iteration):
//   - SampleSnowMask(worldXY) — returns the per-texel snow-allowed value
//     (0 = blocked, 1 = allowed) baked by SnowMaskCS from water + scene
//     height + slope.  Sentinel return = 1 when the mask isn't ready
//     (graceful fallback: render snow everywhere until the mask catches up).
//   - IsSnowAllowed(worldXY) — boolean form of SampleSnowMask.
//
// Architecture:
//   - SharedData::sceneHeightSettings holds the per-frame metadata (window
//     origin, inverse-coverage scale, ready-flag).  No per-feature plumbing
//     required — the cbuffer is already bound everywhere.
//   - Water mask + scene occlusion SRVs are NOT bound globally.  Consumers
//     call `globals::features::sceneHeight.BindToPS()` inside their own
//     dispatch/draw pass, then unbind (or let the next draw overwrite).
//     This avoids a same-resource SRV+DSV conflict with Skylighting's
//     per-frame texOcclusion DSV bind, which crashed AMD drivers when the
//     SRV bind was persistent.
//   - PS register convention:
//       t35 = snow mask (Layer 2)
//       t36 = water mask (Layer 1.1)
//       t37 = scene occlusion (Layer 1.2 — borrowed from Skylighting)
//   - Sentinel-fill on init means consumers see "no water everywhere" before
//     the first cache refresh completes — graceful degradation, never NaN.
// =============================================================================

#ifndef __SCENE_HEIGHT_HLSLI__
#define __SCENE_HEIGHT_HLSLI__

#include "Common/SharedData.hlsli"

#if defined(PSHADER)

namespace SceneHeight
{
	// Water mask texture — R32_FLOAT, 128x128 over a 10,000-unit window
	// centered on the player's current cell.  Texel value = water surface Z,
	// or a large negative sentinel (≤ kNoWaterThreshold) when no water body.
	Texture2D<float> WaterHeightMap : register(t36);

	// Below this value = "no water at this XY".  Mirrors C++ kNoWaterSentinel.
	static const float kNoWaterThreshold = -1.0e9;

	// Converts a world-XY position to UV inside the water-mask window.
	// Returns false if the position is outside the cached window.
	bool WorldXYToWaterMaskUV(float2 worldXY, out float2 uv)
	{
		float2 origin = float2(
			SharedData::sceneHeightSettings.WaterMaskWorldOriginX,
			SharedData::sceneHeightSettings.WaterMaskWorldOriginY);
		uv = (worldXY - origin) * SharedData::sceneHeightSettings.WaterMaskInvCoverage;
		return all(uv >= 0.0) && all(uv <= 1.0);
	}

	// Sample water surface Z at a world-XY position.  Returns the sentinel
	// (well below kNoWaterThreshold) for either "outside cached window" or
	// "no water body at this XY".  Callers should compare against
	// kNoWaterThreshold to detect a real reading.
	float SampleWaterHeight(float2 worldXY)
	{
		// Cache not yet populated → return sentinel so consumers see "no water"
		// rather than garbage.  Avoids one-frame visual glitches at cell change.
		if (SharedData::sceneHeightSettings.WaterMaskReady < 0.5)
			return -1.0e10;

		float2 uv;
		if (!WorldXYToWaterMaskUV(worldXY, uv))
			return -1.0e10;

		return WaterHeightMap.SampleLevel(SampColorSampler, uv, 0);
	}

	// True when the point (x,y,z) is at or below the local water surface.
	// Use this for "should this point be considered submerged?" queries —
	// e.g., suppressing snow under water level.  Returns false when outside
	// the cached window OR when no water body is at that XY (so callers can
	// safely chain `if (!IsOverWater(...)) { paint snow; }`).
	bool IsOverWater(float2 worldXY, float worldZ)
	{
		float w = SampleWaterHeight(worldXY);
		return (w > kNoWaterThreshold) && (worldZ <= w);
	}

	// True only when we have a confirmed water reading at (x,y).  Useful for
	// "is this an aquatic location?" queries that don't care about the
	// caller's Z (ice-float spawn checks, shoreline detection, etc.).
	bool HasWaterAt(float2 worldXY)
	{
		return SampleWaterHeight(worldXY) > kNoWaterThreshold;
	}

	// =========================================================================
	// Scene height broker (Layer 1.2)
	// =========================================================================

	// Skylighting's top-down scene depth, borrowed read-only by SceneHeight.
	// Updated every frame inside Skylighting::RenderOcclusion with a near-
	// vertical orthographic projection covering 10,000 world units centered
	// on the player.  Stored as post-projection NDC z in [0, 1].
	Texture2D<float> SceneHeightOcclusion : register(t37);

	// Sentinel return for "no scene-height data available" — outside cached
	// area, Skylighting unloaded, or NaN-guard miss.  Same threshold as the
	// water mask for consistent caller patterns.
	static const float kNoSceneHeightThreshold = -1.0e9;

	// Sample the world-space Z of the topmost opaque surface at (worldX, worldY).
	// Returns ≤ kNoSceneHeightThreshold when no usable data (Skylighting
	// inactive, position outside the cached window, etc.).
	//
	// CRITICAL: `OcclusionViewProj` (built by Skylighting at Skylighting.cpp:191)
	// expects CAMERA-RELATIVE world positions, not absolute world.  Subtract
	// `FrameBuffer::CameraPosAdjust[0].xyz` from absolute world to get
	// camera-relative.  Also, Skylighting JITTERS the projection direction
	// off-nadir for temporal AA (~10° cone), making the matrix's _m20 / _m21
	// non-zero — the depth equation has full X/Y dependence and the inverse
	// must include those terms.  Naive `(ndcZ - _m23) / _m22` was correct
	// only at the exact projection center (the player); error grew with
	// distance, producing wildly wrong Z values for slab vertices.
	float SampleSceneHeight(float2 worldXY)
	{
		if (SharedData::sceneHeightSettings.SceneHeightReady < 0.5)
			return -1.0e10;

		// World → camera-relative XY.  Z=0 input is fine for the X/Y
		// projection (M[0][2] = M[1][2] = 0 for orthographic) — we reconstruct
		// Z below from the sampled depth, not from the input.
		float3 camRel = float3(worldXY, 0.0) - FrameBuffer::CameraPosAdjust[0].xyz;

		float4 clipPos = mul(SharedData::skylightingSettings.OcclusionViewProj,
			float4(camRel, 1.0));
		float2 ndc = clipPos.xy / clipPos.w;
		float2 uv  = ndc * 0.5 + 0.5;
		uv.y = 1.0 - uv.y;

		if (any(uv < 0.0) || any(uv > 1.0))
			return -1.0e10;

		float storedNdcZ = SceneHeightOcclusion.SampleLevel(SampColorSampler, uv, 0);

		// Full inverse: ndcZ = _m20*x + _m21*y + _m22*z + _m23  (for clip.w=1).
		float4x4 M = SharedData::skylightingSettings.OcclusionViewProj;
		if (abs(M._m22) < 1e-6)
			return -1.0e10;

		float zCamRel = (storedNdcZ - M._m20 * camRel.x - M._m21 * camRel.y - M._m23) / M._m22;

		// Camera-relative → absolute world Z (caller's coord system).
		return zCamRel + FrameBuffer::CameraPosAdjust[0].z;
	}

	// True when there's an opaque surface above (worldX, worldY, worldZ).  Use
	// this to suppress snow accumulation under roofs / overhangs / cave mouths.
	// Returns false (no occluder) when scene-height data is unavailable — safer
	// default for "should we draw snow here?" callers (graceful degradation
	// just renders snow everywhere as if it were open sky).
	bool IsOccluderAbove(float2 worldXY, float worldZ)
	{
		float topZ = SampleSceneHeight(worldXY);
		return (topZ > kNoSceneHeightThreshold) && (topZ > worldZ);
	}

	// Surface-aligned variant: true when the topmost surface at (x, y) is
	// approximately at `worldZ` (within tolerance).  Useful for "am I on
	// the snow-attachment surface itself, or below it?" queries.  Tolerance
	// is in world units; default 1.0 matches typical snow-layer-depth bias.
	bool IsAtTopSurface(float2 worldXY, float worldZ, float toleranceWorldUnits = 1.0)
	{
		float topZ = SampleSceneHeight(worldXY);
		return (topZ > kNoSceneHeightThreshold) && (abs(topZ - worldZ) <= toleranceWorldUnits);
	}

	// =========================================================================
	// Snow mask (Layer 2)
	// =========================================================================

	// Baked "snow allowed" mask: R8_UNORM 128×128 over the same 10K window as
	// the water mask.  Each texel value: 0 = blocked (water / steep slope /
	// under occluder), 1 = allowed.  Sampled with linear filtering so cell
	// boundaries get soft transitions if Layer 3 wants smooth slab cuts.
	Texture2D<float> SnowAllowedMask : register(t35);

	// Maps world XY → mask UV in the cached 10K window.  Returns false if the
	// position is outside the window.
	bool WorldXYToSnowMaskUV(float2 worldXY, out float2 uv)
	{
		float2 origin = float2(
			SharedData::sceneHeightSettings.WaterMaskWorldOriginX,
			SharedData::sceneHeightSettings.WaterMaskWorldOriginY);
		uv = (worldXY - origin) * SharedData::sceneHeightSettings.WaterMaskInvCoverage;
		return all(uv >= 0.0) && all(uv <= 1.0);
	}

	// Returns the raw mask value [0, 1].  Sentinel 1.0 when the mask isn't
	// ready (graceful fallback — show snow everywhere until SnowMaskCS lands
	// its first valid result) or outside the cached window (treat as open).
	float SampleSnowMask(float2 worldXY)
	{
		if (SharedData::sceneHeightSettings.SnowMaskReady < 0.5)
			return 1.0;

		float2 uv;
		if (!WorldXYToSnowMaskUV(worldXY, uv))
			return 1.0;

		return SnowAllowedMask.SampleLevel(SampColorSampler, uv, 0);
	}

	// Boolean convenience.  Threshold at 0.5 — anything above half-coverage
	// counts as "allowed" (matches a "majority of texel area is snowy" rule
	// when sampled with linear filtering).
	bool IsSnowAllowed(float2 worldXY)
	{
		return SampleSnowMask(worldXY) >= 0.5;
	}
}

#endif  // PSHADER

#endif  // __SCENE_HEIGHT_HLSLI__
