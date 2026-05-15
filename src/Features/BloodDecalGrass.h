#pragma once

#include "Buffer.h"

struct BloodDecalGrass : Feature,
                         public RE::BSTEventSink<RE::TESHitEvent>
{
public:
	virtual inline std::string GetName() override { return "Blood Decal Grass"; }
	virtual inline std::string GetShortName() override { return "BloodDecalGrass"; }
	virtual inline std::string_view GetShaderDefineName() override { return "BLOOD_DECAL_GRASS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kGrass; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Blood Decal Grass tints nearby grass red around dead actors and combat hit locations.",
			{ "Grass near dead actors soaks with blood",
				"Combat hits leave blood splatters on grass",
				"Configurable radius, intensity, and fade timing",
				"GPU fluid simulation for realistic blood pooling and flow" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	static constexpr uint MAX_BLOOD_ENTRIES = 128;
	static constexpr float MAX_SCAN_DISTANCE = 4096.0f;
	static constexpr float MAX_SCAN_SQ_DISTANCE = MAX_SCAN_DISTANCE * MAX_SCAN_DISTANCE;
	static constexpr float CORPSE_MOVE_THRESHOLD_SQ = 48.0f * 48.0f;
	static constexpr float CORPSE_SETTLE_TIME = 3.0f;

	// Fluid simulation grid constants
	static constexpr uint GRID_DIM = 512;
	static constexpr float GRID_WORLD_SIZE = 4096.0f;
	static constexpr float GRID_CELL_SIZE = GRID_WORLD_SIZE / GRID_DIM;
	static constexpr uint TERRAIN_FILL_ROWS_PER_FRAME = 32;

	struct Settings
	{
		bool Enable = true;
		float CorpseBloodRadius = 128.0f;
		float SplatterBloodRadius = 64.0f;
		float BloodIntensity = 0.7f;
		float CorpseSoakDuration = 12.0f;
		float CorpseDryIntensity = 0.65f;
		float CorpseDryDuration = 45.0f;
		float SplatterLifetime = 120.0f;
		float SplatterFadeInTime = 0.5f;
		float SplatterFadeOutTime = 30.0f;
		float BloodColorR = 0.3f;
		float BloodColorG = 0.02f;
		float BloodColorB = 0.02f;

		// Surface staining
		bool EnableSurfaceStaining = true;
		float SurfaceHeightThreshold = 64.0f;
		float SurfaceNormalThreshold = 0.5f;

		// Surface flow (drips)
		bool EnableSurfaceFlow = true;
		float DripReachMultiplier = 3.0f;
		float DripRivuletWidth = 0.4f;

		// Pulse injection
		float CorpsePulseCount = 5.0f;
		float CorpsePulseInterval = 0.8f;
		float CorpsePulseDecay = 1.3f;

		// Fluid simulation
		bool EnableFluidSim = true;
		float FluidViscosity = 0.85f;
		float FluidDryingRate = 0.1f;
		float FluidBloodVolumeRate = 0.05f;
		float FluidEvaporationRate = 0.0001f;
		int FluidIterations = 2;
		float FluidMomentumStrength = 0.3f;
		float FluidVelocityDamping = 2.0f;

		// Flow noise (pixel shader rivulets)
		float FlowNoiseScale = 0.3f;

		// Parallax (pixel shader depth)
		bool EnableBloodParallax = true;
		float BloodParallaxDepth = 3.0f;
	};

	struct alignas(16) CommonBufferData
	{
		// Row 0
		uint Enabled = 0;
		float BloodIntensity = 0.7f;
		uint EntryCount = 0;
		uint EnableSurfaceStaining = 0;

		// Row 1
		float SurfaceHeightThreshold = 64.0f;
		float SurfaceNormalThreshold = 0.5f;
		uint EnableSurfaceFlow = 0;
		float DripReachMultiplier = 3.0f;

		// Row 2
		float DripRivuletWidth = 0.4f;
		uint EnableFluidSim = 0;
		float GridCellSize = 0.0f;
		float GridWorldSize = 0.0f;

		// Row 3
		float GridWorldOriginX = 0.0f;
		float GridWorldOriginY = 0.0f;
		int ArrayOriginX = 0;
		int ArrayOriginY = 0;

		// Row 4
		int ValidMarginX = 0;
		int ValidMarginY = 0;
		float DeltaTime = 0.0f;
		float Viscosity = 0.0f;

		// Row 5
		float DryingRate = 0.0f;
		float BloodVolumeRate = 0.0f;
		float EvaporationRate = 0.0f;
		float RcpGridCellSize = 0.0f;

		// Row 6
		uint IterationCount = 1;
		float MomentumStrength = 0.0f;
		float VelocityDamping = 0.0f;
		float FlowNoiseScale = 0.0f;

		// Row 7
		float ParallaxDepthScale = 0.0f;
		uint EnableParallax = 0;
		float BloodColorR = 0.3f;
		float BloodColorG = 0.02f;

		// Row 8
		float BloodColorB = 0.02f;
		float pad0 = 0.0f;
		float pad1 = 0.0f;
		float pad2 = 0.0f;
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);

	struct BloodEntry
	{
		float4 positionRadius;   // xyz = world position, w = radius
		float4 colorIntensity;   // xyz = blood color, w = overall intensity multiplier
		float4 soakParams;       // x = soak progress (0 = none, 1 = fully soaked), yzw = padding
	};

	struct alignas(16) FlowIterationCB
	{
		uint IterationIndex = 0;
		float SubDeltaTime = 0.0f;
		uint pad0 = 0;
		uint pad1 = 0;
	};

	struct TrackedCorpse
	{
		RE::FormID actorFormID;
		RE::NiPoint3 bloodOrigin;
		float presentTime;
		bool stillPresent;
		bool frozen;

		// Pulse injection state
		float mass;
		int totalPulses;
		int pulsesRemaining;
		float pulseTimer;
		float pulseInterval;
		float pulseIntensity;
	};

	struct HitSplatter
	{
		RE::NiPoint3 position;
		float age;
		float lifetime;
	};

	Settings settings;

	// Blood entry buffer (StructuredBuffer for per-entry data)
	eastl::unique_ptr<Buffer> bloodBuffer = nullptr;

	std::mutex bloodMutex;
	eastl::vector<TrackedCorpse> trackedCorpses;
	eastl::vector<HitSplatter> hitSplatters;

	// Fluid simulation GPU resources
	eastl::unique_ptr<Texture2D> texBloodHeight[2];    // ping-pong: R=height, G=age (R16G16_FLOAT)
	eastl::unique_ptr<Texture2D> texBloodVelocity[2];  // ping-pong: RG=velocity XY (R16G16_FLOAT)
	eastl::unique_ptr<Texture2D> texTerrainHeight;     // terrain elevation (R32_FLOAT)
	eastl::unique_ptr<Texture2D> texBloodStain;        // stain intensity (R16_FLOAT)
	winrt::com_ptr<ID3D11ComputeShader> bloodFlowCS;
	winrt::com_ptr<ID3D11Buffer> iterationCB;          // per-iteration cbuffer (b7, CS only)
	winrt::com_ptr<ID3D11SamplerState> gridSampler;    // bilinear wrap for PS sampling

	// Grid management state
	int prevCellIDX = INT_MAX;
	int prevCellIDY = INT_MAX;
	int currentArrayOriginX = 0;
	int currentArrayOriginY = 0;
	int currentValidMarginX = 0;
	int currentValidMarginY = 0;
	float currentGridWorldOriginX = 0.0f;
	float currentGridWorldOriginY = 0.0f;
	uint simFrameIndex = 0;

	// Terrain height state
	bool terrainInitialized = false;
	uint terrainFillRow = 0;
	eastl::vector<float> terrainHeightCPU;

	CommonBufferData GetCommonBufferData();
	void ScanForDeadActors();
	void Update();
	void CompileComputeShaders();
	void UpdateTerrainHeight();
	void DispatchFluidSim(float dt);

	RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual bool SupportsVR() override { return true; };

	struct Hooks
	{
		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};
};
