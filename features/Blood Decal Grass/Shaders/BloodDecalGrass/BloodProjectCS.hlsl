// BloodProjectCS.hlsl
// Full-screen deferred projection of blood onto the composited scene.
//
// Runs after DeferredCompositeCS and DrawStereoBlend. For each screen pixel:
//   1. Reconstruct world position from depth + camera matrices.
//   2. Decode view-space normal from G-buffer, transform to world space.
//   3. Call GetSurfaceBloodInfluence (noise-soak) and GetFluidSurfaceInfluence
//      (fluid sim pooling) independently — each is gated by its own enable flag.
//   4. Pick the stronger influence, then lerp the final scene colour toward the
//      blood tint in MainRW.
//
// Height rejection inside both functions prevents blood from painting elevated
// surfaces (actors, walls) that share XY with a blood pool.
//
// Register layout:
//   t0   : scene depth  (R32_FLOAT with TERRAIN_BLENDING, unorm float otherwise)
//   t1   : NormalRoughness G-buffer (NORMALROUGHNESS = kRAWINDIRECT_DOWNSCALED)
//   t2   : Actor mask RT (ACTOR_MASK = kRAWINDIRECT_PREVIOUS_DOWNSCALED)
//           0 = world surface (apply blood), 1 = actor/creature (skip blood)
//   t101 : BloodDecals  (declared in BloodDecalGrass.hlsli)
//   t102 : BloodHeightTex
//   t103 : BloodStainTex
//   t104 : TerrainHeightTex
//   t105 : BloodVelocityTex
//   s7   : BloodGridSampler
//   u0   : MainRW (kMAIN scene color output)
//   b5   : SharedData cbuffer
//   b6   : FeatureData cbuffer
//   b12  : PerFrame / FrameBuffer cbuffer  (bound by DeferredPasses before dispatch)

#include "Common/GBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "BloodDecalGrass/BloodDecalGrass.hlsli"

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT, OFF -> unorm (matches DeferredCompositeCS).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthBuffer : register(t0);
#else
Texture2D<unorm float> DepthBuffer : register(t0);
#endif

// NormalRoughness G-buffer: xy = oct-encoded view-space normal, z = glossiness.
Texture2D<unorm float3> NormalRoughnessBuffer : register(t1);

// Actor mask RT: 0 = world surface (apply blood), 1 = actor/creature (skip blood).
Texture2D<float> ActorMaskBuffer : register(t2);

// Scene colour output — read-modify-write.
RWTexture2D<float4> MainRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid
                                : SV_DispatchThreadID) {
	// Bounds check against the render buffer dimensions.
	if (any(dtid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	// ---- Screen UV and stereo eye ----
	float2 uv = (float2(dtid.xy) + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	// ---- Read depth ----
	float depth = DepthBuffer[dtid.xy];

	// Skip skybox / background pixels.
	if (depth >= 1.0)
		return;

	// Skip actor/creature pixels — blood is applied to world surfaces only.
	if (ActorMaskBuffer[dtid.xy] > 0.5)
		return;

	// ---- Reconstruct world position ----
	// posCS is in NDC: x in [-1,1] left-to-right, y in [-1,1] bottom-to-top.
	float4 posCS = float4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
	float4 posCR = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], posCS);
	posCR.xyz /= posCR.w;
	// posCR is camera-relative world; add camera origin to get absolute world pos.
	float3 worldPos = posCR.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	// ---- Decode world-space normal ----
	float3 normalGlossiness = NormalRoughnessBuffer[dtid.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);
	// Transform view-space direction to world space (w=0 = direction, not point).
	float3 worldNormal = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0.0)).xyz);

	// ---- View direction (world space, pointing toward camera) ----
	float3 camWorldPos = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 viewDirection = normalize(camWorldPos - worldPos);

	// ---- Sample blood influence ----
	// Noise-soak (decal-driven) and fluid sim pooling are independent systems.
	// Each is gated by its own enable flag; evaluate both and pick the stronger.
	float3 soakInfluence  = BloodDecalGrass::GetSurfaceBloodInfluence(worldPos, worldNormal, viewDirection);
	float3 fluidInfluence = BloodDecalGrass::GetFluidSurfaceInfluence(worldPos, worldNormal, viewDirection);

	float soakStr  = saturate(length(soakInfluence));
	float fluidStr = saturate(length(fluidInfluence));

	float bloodStrength = max(soakStr, fluidStr);

	if (bloodStrength < 0.001)
		return;

	// Fluid pooling takes priority when strengths are equal; soak wins otherwise.
	float3 bloodInfluence = fluidStr >= soakStr ? fluidInfluence : soakInfluence;

	// ---- Blend blood tint onto scene colour ----
	float4 mainColor = MainRW[dtid.xy];
	mainColor.rgb = lerp(mainColor.rgb, bloodInfluence, bloodStrength);
	MainRW[dtid.xy] = mainColor;
}
