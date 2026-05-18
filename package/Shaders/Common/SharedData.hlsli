#ifndef __SHARED_DATA_DEPENDENCY_HLSL__
#define __SHARED_DATA_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"

namespace SharedData
{

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER) || defined(HULLSHADER) || defined(DOMAINSHADER) || defined(VSHADER)
	cbuffer SharedData : register(b5)
	{
		float4 WaterData[25];
		row_major float3x4 DirectionalAmbient;
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		bool InInterior;          // If the area lacks a directional shadow light e.g. the sun or moon
		bool InMapMenu;           // If the world/local map is open (note that the renderer is still deferred here)
		bool HideSky;             // HideSky flag in WorldSpace, e.g. Blackreach
		float MipBias;            // Offset to mip level for TAA sharpness
		float WaterSystemHeight;  // TES::GetWaterHeight at eye-0 in camera-relative Z; -FLT_MAX when no water body found (VR only)
		float4 AmbientSHR;
		float4 AmbientSHG;
		float4 AmbientSHB;
		float4 HDRData;
	};

	struct GrassLightingSettings
	{
		float Glossiness;
		float SpecularStrength;
		float SubsurfaceScatteringAmount;
		bool OverrideComplexGrassSettings;

		float BasicGrassBrightness;
		bool EnableWrappedLighting;
		float ComplexGrassThreshold;
		float1 pad0;
	};

	struct CPMSettings
	{
		bool EnableComplexMaterial;
		bool EnableParallax;
		bool EnableTerrainParallax;
		bool EnableHeightBlending;
		bool EnableShadows;
		bool ExtendShadows;
		bool EnableParallaxWarpingFix;
		bool pad0;
	};

	struct CubemapCreatorSettings
	{
		uint Enabled;
		float3 pad0;

		float4 CubemapColor;
	};

	struct TerraOccSettings
	{
		bool EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
	};

	struct LightLimitFixSettings
	{
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		float2 pad0;
		uint4 ClusterSize;
	};

	struct WetnessEffectsSettings
	{
		row_major float4x4 OcclusionViewProj;

		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;

		bool EnableWetnessEffects;
		float MaxRainWetness;
		float MaxPuddleWetness;
		float MaxShoreWetness;

		uint ShoreRange;
		float PuddleRadius;
		float PuddleMaxAngle;
		float PuddleMinWetness;

		float MinRainWetness;
		float SkinWetness;
		float WeatherTransitionSpeed;
		bool EnableRaindropFx;

		bool EnableSplashes;
		bool EnableRipples;
		uint EnableVanillaRipples;
		float RaindropFxRange;

		float RaindropGridSizeRcp;
		float RaindropIntervalRcp;
		float RaindropChance;
		float SplashesLifetime;

		float SplashesStrength;
		float SplashesMinRadius;
		float SplashesMaxRadius;
		float RippleStrength;

		float RippleRadius;
		float RippleBreadth;
		float RippleLifetimeRcp;
		float pad0;
	};

	struct SkylightingSettings
	{
		row_major float4x4 OcclusionViewProj;
		float4 OcclusionDir;

		float4 PosOffset;   // xyz: cell origin in camera model space
		uint4 ArrayOrigin;  // xyz: array origin
		int4 ValidMargin;

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint2 pad0;
	};

	struct SnowCoverSettings
	{
		float Month;
		float TimeSnowing;
		float SnowingDensity;
		float SeasonalAltitude;

		uint EnableExpensiveFoliage;
		float SnowHeightOffset;
		uint AffectHavok;
		float TreeSnowAmount;

		uint EnableSnowCover;
		uint AffectGrassTint;
		uint AffectTreeTint;
		float FoliageHeightOffset;

		float UVScale;
		float peakMainAngle;
		float peakAltAngle;
		float minAngle;

		float maxAngle;
		float mainSpec;
		float altSpec;
		float mapZscale;

		float2 mapScale;
		float2 mapOffset;

		float4 Glint;
		float4 MainTint;
		float4 AltTint;

		float BlendSmoothness;
		uint3 pad2;
	};

	struct CloudShadowsSettings
	{
		float Opacity;
		float3 pad0;
	};

	struct LODBlendingSettings
	{
		float LODTerrainBrightness;
		float LODObjectBrightness;
		float LODObjectSnowBrightness;
		bool DisableTerrainVertexColors;
		float LODTerrainGamma;
		float LODObjectGamma;
		float LODObjectSnowGamma;
		float pad0;
	};

	struct HairSpecularSettings
	{
		uint Enabled;
		float HairGlossiness;
		float SpecularMult;
		float DiffuseMult;
		uint EnableTangentShift;
		float PrimaryTangentShift;
		float SecondaryTangentShift;
		float HairSaturation;
		float SpecularIndirectMult;
		float DiffuseIndirectMult;
		float BaseColorMult;
		float Transmission;
		uint EnableSelfShadow;
		float SelfShadowStrength;
		float SelfShadowExponent;
		float SelfShadowScale;
		uint HairMode;  // 0: Kajiya-Kay, 1: Marschner
		uint3 pad;
	};

	struct TerrainVariationSettings
	{
		uint enableTilingFix;
		uint enableLODTerrainTilingFix;
		float2 pad0;
	};

	struct IBLSettings
	{
		uint EnableIBL;
		uint PreserveFogLuminance;
		uint UseStaticIBL;
		float DALCAmount;
		float EnvIBLScale;
		float SkyIBLScale;
		float EnvIBLSaturation;
		float SkyIBLSaturation;
		float FogAmount;
		uint DALCMode;  // 0: Luminance Ratio, 1: Color Ratio, 2: DALC + Sky, 3: DALC + Sky (Directional)
		uint DisableInInteriors;
		float pad0;
	};

	struct ExtendedTranslucencySettings
	{
		uint MaterialModel;  // [0,1,2,3] The MaterialModel
		float Reduction;     // [0, 1.0] The factor to reduce the transparency to matain the average transparency [0,1]
		float Softness;      // [0, 2.0] The soft remap upper limit [0,2]
		float Strength;      // [0, 1.0] The inverse blend weight of the effect
	};

	struct LinearLightingSettings
	{
		uint enableLinearLighting;
		uint enableGammaCorrection;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float effectGamma;
		float effectAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float vanillaDiffuseColorMult;
		float directionalLightMult;
		float pointLightMult;
		float ambientMult;
		float emitColorMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
	};

	struct TerrainBlendingSettings
	{
		uint Enabled;
		uint3 _padding;
	};

	struct ExponentialHeightFogSettings
	{
		uint enabled;
		uint useDynamicCubemaps;
		float startDistance;
		float fogHeight;
		float fogHeightFalloff;
		float fogDensity;
		float directionalInscatteringMultiplier;
		float directionalInscatteringExponent;
		float4 inscatteringTint;
		float cubemapMipLevel;
		uint respectVanillaFogFade;
		float2 pad;
	};

	struct BloodDecalGrassSettings
	{
		// Row 0
		uint Enabled;
		float BloodIntensity;
		uint EntryCount;
		uint EnableSurfaceStaining;

		// Row 1
		float SurfaceHeightThreshold;
		float SurfaceNormalThreshold;
		uint EnableSurfaceFlow;
		float DripReachMultiplier;

		// Row 2
		float DripRivuletWidth;
		uint EnableFluidSim;
		float GridCellSize;
		float GridWorldSize;

		// Row 3
		float2 GridWorldOrigin;
		int2 ArrayOrigin;

		// Row 4
		int2 ValidMargin;
		float DeltaTime;
		float Viscosity;

		// Row 5
		float DryingRate;
		float BloodVolumeRate;
		float EvaporationRate;
		float RcpGridCellSize;

		// Row 6
		uint IterationCount;
		float MomentumStrength;
		float VelocityDamping;
		float FlowNoiseScale;

		// Row 7
		float ParallaxDepthScale;
		uint EnableParallax;
		float BloodColorR;
		float BloodColorG;

		// Row 8
		float BloodColorB;
		float pad0;
		float pad1;
		float pad2;
	};

	// =========================================================================
	// SceneHeightSettings
	//   Layer 1 infrastructure cbuffer entry.  Mirrors SceneHeight::PerFrame in
	//   src/SceneHeight.h byte-for-byte.  Used by Common/SceneHeight.hlsli's
	//   SampleWaterHeight / SampleSceneHeight / IsOverWater / IsOccluderAbove.
	//
	//   Row 0 — water mask (Layer 1.1)
	//     WaterMaskWorldOriginX/Y : SW corner of cached window (world space)
	//     WaterMaskInvCoverage    : 1 / 10000 — world-XY-offset → UV scale
	//     WaterMaskReady          : 0 while filling, 1 after first refresh
	//
	//   Row 1 — scene height broker (Layer 1.2) + snow mask gate (Layer 2)
	//     SceneHeightReady        : 1 only when Skylighting is loaded AND its
	//                               texOcclusion is initialized.  The projection
	//                               matrix itself is borrowed from
	//                               skylightingSettings.OcclusionViewProj — no
	//                               duplication.  Gate ALL scene-height math on
	//                               this flag (otherwise zero matrix → NaN).
	//     SnowMaskReady            : 1 once the snow-mask CS has produced its
	//                               first result for the current cell.  Layer 3
	//                               (and any consumer of SampleSnowMask) gates
	//                               on this; while 0 → "snow allowed everywhere"
	//                               graceful fallback.
	// =========================================================================
	struct SceneHeightSettings
	{
		// Row 0
		float WaterMaskWorldOriginX;
		float WaterMaskWorldOriginY;
		float WaterMaskInvCoverage;
		float WaterMaskReady;

		// Row 1
		float SceneHeightReady;
		float SnowMaskReady;
		float _pad0;
		float _pad1;
	};

	struct SnowDeformationSettings
	{
		// Row 0
		uint Enabled;
		float GridWorldSize;
		float GridCellSize;
		float RcpGridCellSize;

		// Row 1
		float GridWorldOriginX;
		float GridWorldOriginY;
		int ArrayOriginX;
		int ArrayOriginY;

		// Row 2
		float SnowLayerDepth;  // height of the default snow layer (= max compression depth)
		float SnowContactDepth;
		float SettlingRate;
		float RidgeStrength;

		// Row 3
		float TessellationScale;
		float TessellationFalloff;
		float SnowAltitudeMin;
		float SnowSlopeFactor;

		// Row 4
		float DeltaTime;
		float TerrainSurfaceEpsilon;
		uint  DebugForceDeform;  // 1 = simulate fully-compressed snow (displacement = SnowLayerDepth, no raise)
		uint  Reserved; // Unused — CPU-rasterized contact grid replaces screen-space detection
	};

	cbuffer FeatureData : register(b6)
	{
		GrassLightingSettings grassLightingSettings;
		CPMSettings extendedMaterialSettings;
		CubemapCreatorSettings cubemapCreatorSettings;
		TerraOccSettings terraOccSettings;
		LightLimitFixSettings lightLimitFixSettings;
		WetnessEffectsSettings wetnessEffectsSettings;
		SkylightingSettings skylightingSettings;
		SnowCoverSettings snowCoverSettings;
		CloudShadowsSettings cloudShadowsSettings;
		LODBlendingSettings lodBlendingSettings;
		HairSpecularSettings hairSpecularSettings;
		TerrainVariationSettings terrainVariationSettings;
		IBLSettings iblSettings;
		ExtendedTranslucencySettings extendedTranslucencySettings;
		LinearLightingSettings linearLightingSettings;
		TerrainBlendingSettings terrainBlendingSettings;
		ExponentialHeightFogSettings exponentialHeightFogSettings;
		BloodDecalGrassSettings bloodDecalGrassSettings;
		SnowDeformationSettings snowDeformationSettings;
		SceneHeightSettings sceneHeightSettings;
	};

	Texture2D<float4> DepthTexture : register(t17);

	// Get a int3 to be used as texture sample coord. [0,1] in uv space
	int3 ConvertUVToSampleCoord(float2 uv, uint a_eyeIndex)
	{
		uv = Stereo::ConvertToStereoUV(uv, a_eyeIndex);
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		return int3(uv * BufferDim.xy, 0);
	}

	// Get a raw depth from the depth buffer. [0,1] in uv space
	float GetDepth(float2 uv, uint a_eyeIndex = 0)
	{
		return DepthTexture.Load(ConvertUVToSampleCoord(uv, a_eyeIndex)).x;
	}

	float GetScreenDepth(float depth)
	{
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
	}

	float4 GetScreenDepths(float4 depths)
	{
		return (CameraData.w / (-depths * CameraData.z + CameraData.x));
	}

	float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
	{
		float depth = GetDepth(uv, a_eyeIndex);
		return GetScreenDepth(depth);
	}

	// Returns water data for the tile containing worldPosition (camera-relative XY).
	// The .w component (water surface height) is stored in C++ as camera-relative Z of
	// eye 0 (left eye).  Pass eyeIndex to have .w corrected into the current eye's
	// camera-relative frame; defaults to 0 (no correction, backwards-compatible).
	float4 GetWaterData(float3 worldPosition, uint eyeIndex = 0)
	{
		float2 cellF = (((worldPosition.xy + FrameBuffer::CameraPosAdjust[0].xy)) / 4096.0) + 64.0;  // always positive
		int2 cellInt;
		float2 cellFrac = modf(cellF, cellInt);

		cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
		cellF += 2.5;                                       // 5x5 cell grid
		cellF -= cellFrac;                                  // align to cell borders
		cellInt = round(cellF);

		uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24

		float4 waterData = float4(1.0, 1.0, 1.0, -2147483648);

		[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
			waterData = WaterData[waterTile];

#	if defined(VR)
		// Correct .w from eye-0 camera-relative Z to the current eye's camera-relative Z.
		// No-op when eyeIndex == 0 (both terms are identical).
		waterData.w += FrameBuffer::CameraPosAdjust[0].z - FrameBuffer::CameraPosAdjust[eyeIndex].z;
#	endif

		return waterData;
	}

	float3 GetAmbient(float3 normal)
	{
		return SphericalHarmonics::Unproject(AmbientSHR, AmbientSHG, AmbientSHB, normal);
	}

#endif  // PSHADER
}
#endif  // __SHARED_DATA_DEPENDENCY_HLSL__
