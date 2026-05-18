#pragma once

#include "Buffer.h"
#include "Feature.h"

// =============================================================================
// SceneHeight — Layer 1 of the snow-deformation architecture.
//
// Foundation subsystem that surfaces spatial scene-data products to other
// features.  It does NOT itself render anything; it produces textures and
// cbuffer data that downstream features (SnowDeformation, future ice floes,
// water-aware decals, etc.) sample.
//
// Initial scope (Iteration 1 — this commit):
//   - WaterMask: per-cell R32_FLOAT texture, 128×128 over a 10,000-unit
//     player-centered region.  Each texel holds the water surface Z at that
//     world XY, or kNoWaterSentinel when no water body covers that point.
//     Filled progressively from RE::TES::GetWaterHeight on the CPU, uploaded
//     once per cell change.  Reusable for snow accumulation (avoid water),
//     ice floes (spawn locations), water-aware decal placement, etc.
//
// Next iteration (Layer 1.2):
//   - Borrowed read-only SRV for Skylighting::texOcclusion (top-down scene
//     depth) + PrecipitationOcclusionWorldViewProj matrix.  Lets shaders
//     reconstruct the world-space height of the highest opaque surface at
//     any (x,y), which feeds into the snow-mask CS (Layer 2).
//
// Architectural notes:
//   - Inherits Feature for automatic lifecycle (SetupResources / Prepass) and
//     SharedData cbuffer integration via GetCommonBufferData.  It has no
//     user-facing toggle: it's always-on infrastructure.
//   - Skylighting OWNS its texOcclusion; SceneHeight only reads it.  We add a
//     getter on Skylighting (next iteration) rather than refactoring it.
//   - Future game-start full-world cache (Unified-Water-style) is in scope —
//     the API doesn't preclude it.
// =============================================================================
struct SceneHeight : Feature
{
public:
	// -------------------------------------------------------------------------
	// Feature identity
	// -------------------------------------------------------------------------
	virtual inline std::string      GetName() override { return "Scene Height"; }
	virtual inline std::string      GetShortName() override { return "SceneHeight"; }
	virtual inline std::string_view GetShaderDefineName() override { return ""; }
	virtual std::string_view        GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual bool HasShaderDefine(RE::BSShader::Type) override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Foundation infrastructure layer that captures spatial scene data and exposes it to other features.  Owns a per-cell water-mask cache and (next iteration) brokers Skylighting's top-down scene depth.  No direct visual output — feeds snow accumulation, ice floes, water-aware effects, and future spatial features.",
			{ "Water mask: R32_FLOAT 128×128 over 10,000-unit player-centered region",
				"Progressive CPU fill from RE::TES::GetWaterHeight on cell change",
				"Per-frame cbuffer entry: world-XY → mask UV math for shaders",
				"Sentinel value (-NI_INFINITY) marks 'no water' texels for graceful shader fallback",
				"Reusable building block: snow accumulation / ice floes / decal placement / water physics" }
		};
	}

	// -------------------------------------------------------------------------
	// Compile-time configuration
	// -------------------------------------------------------------------------
	// Coverage matches Skylighting::occlusionDistance (10,000 units) — eventual
	// joint sampling of the scene-height SRV and water mask wants identical
	// world-space coverage so the two products align without per-feature
	// reprojection math.
	static constexpr float    kCoverage          = 10000.0f;
	static constexpr uint32_t kResolution        = 128;
	static constexpr float    kCellSize          = 4096.0f;  // matches engine grid

	// Sentinel encoded into water-mask texels with no water body covering them.
	// Shader checks `> -1e9` to detect a real value.  -NI_INFINITY (≈ -FLT_MAX)
	// is the engine convention returned by TES::GetWaterHeight on misses.
	static constexpr float    kNoWaterSentinel   = -1.0e10f;

	// Progressive fill cadence — rows of the water mask processed per frame
	// while the cache rebuilds after a cell change.  128 rows / 16 = 8 frames
	// to settle.  TES::GetWaterHeight is a CPU call (cell lookup + plane eval);
	// at ~16k calls per full refresh, spreading prevents a visible hitch.
	static constexpr uint32_t kFillRowsPerFrame  = 16;

	// PS register slot for the water mask SRV when bound by SceneHeight::Prepass.
	// Lives just below Snow Cover's t38-t44 range so the two don't collide.
	static constexpr uint32_t kWaterMaskPSSlot   = 36;

	// -------------------------------------------------------------------------
	// SharedData cbuffer entry — concatenated by FeatureBuffer.cpp.
	// Must stay byte-for-byte identical to SceneHeightSettings in SharedData.hlsli.
	// -------------------------------------------------------------------------
	struct alignas(16) PerFrame
	{
		// Row 0 — water mask (Layer 1.1)
		float WaterMaskWorldOriginX;  // world X of texel (0, 0) — SW corner of the 10K window
		float WaterMaskWorldOriginY;  // world Y of texel (0, 0)
		float WaterMaskInvCoverage;   // 1 / kCoverage — converts world-XY-offset to UV
		float WaterMaskReady;         // 0 while filling, 1 once a full refresh completes

		// Row 1 — scene height broker (Layer 1.2) + snow mask gate (Layer 2)
		// SceneHeightReady gates use of Skylighting's `OcclusionViewProj` matrix
		// and our bound texOcclusion SRV.  We borrow the matrix from
		// SharedData::skylightingSettings (no need to duplicate) but need this
		// explicit flag so shaders can safely no-op when Skylighting is unloaded
		// (uninitialized cbuffer would give a zero matrix → NaN on z reconstruct).
		//
		// SnowMaskReady (Layer 2): 0 until the snow-mask CS produces its first
		// result for the current cell, 1 thereafter.  Consumers gate sampling
		// on this; while it's 0 they should treat snow as "allowed everywhere"
		// (graceful fallback — show the M2 uniform slab until the mask catches
		// up).  Resets on cell change.
		float SceneHeightReady;
		float SnowMaskReady;
		float _pad0;
		float _pad1;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);

	// PS slot for the borrowed Skylighting texOcclusion SRV.  One above the
	// water mask's t36; both below Snow Cover's t38-t44 range.
	static constexpr uint32_t kSceneOcclusionPSSlot = 37;

	// PS slot for the snow mask SRV (Layer 2).  Adjacent to water + scene
	// occlusion so all SceneHeight products live in a contiguous block.
	static constexpr uint32_t kSnowMaskPSSlot = 35;

	// Slope threshold for snow accumulation, in cosine of angle from world-up.
	// 70° from horizontal → cos(70°) ≈ 0.342.  Surfaces steeper than this
	// reject snow (true walls, cliffsides).
	//
	// Tuned permissive: the heightmap reconstruction has texel-scale noise
	// (texOcclusion is sampled at ~78 unit per texel, rocks and small statics
	// register as 1-texel-wide bumps with steep apparent gradients).  A
	// 35° threshold rejected most of these features and left snow only on
	// truly-flat valley floors — visible as "shards of snow at varying
	// heights between rocks" in first-iteration screenshots.  70° lets snow
	// accumulate on the wide range of moderate slopes that natural snow
	// would actually cling to.
	static constexpr float kSnowSlopeCosineThreshold = 0.342f;

	// -------------------------------------------------------------------------
	// Per-feature state
	// -------------------------------------------------------------------------
	// Water mask GPU texture + SRV.
	eastl::unique_ptr<Texture2D> waterMaskTex;

	// Snow allowed mask (Layer 2).  R8_UNORM 128×128 over the same 10K window
	// as the water mask.  Each texel: 0 = no snow here, 255 = snow allowed.
	// Computed by `SnowMaskCS` from water mask + scene height + slope analysis.
	// Cached once per cell change; invalidated on `InvalidateCache`.
	eastl::unique_ptr<Texture2D> snowMaskTex;

	// Scene-height world-Z map (Layer 2.5).  R32_FLOAT 128×128 in the same
	// world-XY window as the water/snow masks.  Each texel holds the world Z
	// of the topmost opaque surface at that XY (reconstructed by SnowMaskCS
	// from texOcclusion's NDC z via the orthographic projection's z-row).
	//
	// We materialize this OURSELVES rather than read texOcclusion directly:
	//   1) Avoids depth-format `CopyResource` incompatibility — Skylighting's
	//      texOcclusion is a depth-stencil typed view, our staging would need
	//      matching format, and on AMD the typeless-vs-typed copy path can
	//      silently produce garbage.  With our own R32_FLOAT target the
	//      readback is trivial.
	//   2) Unifies texel coordinates with the water mask — both products
	//      live in the 10K window, indexed by the same world-XY → UV math.
	//      No projection-matrix multiply on CPU side.
	//   3) Stable format regardless of which depth precision Skyrim's
	//      engine selected (D24S8 vs D32F vs whatever).
	eastl::unique_ptr<Texture2D> sceneHeightWorldTex;

	// Tri-state for snow-mask dispatch gating:
	//   `snowMaskValid` flips true the frame the CS completes; downstream
	//   consumers (Layer 3) gate sampling on this. Reset by `InvalidateCache`
	//   and on cell change. The flag drives both the cbuffer `SnowMaskReady`
	//   and the dispatch-once-per-cell guard.
	bool snowMaskValid = false;

	// Compiled CS handle. Loaded in SetupResources, dispatched from Deferred.
	winrt::com_ptr<ID3D11ComputeShader> snowMaskCS;

	// Linear-clamp sampler for the CS's texOcclusion fractional-UV sampling.
	// Owned here so the CS dispatch can bind it at s0 without per-frame
	// allocation.
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;

	// =========================================================================
	// CPU mirrors (Layer 3 foundation — Stage 1)
	// =========================================================================
	// Slabs need per-vertex scene-height + snow-mask sampling at mesh-update
	// time.  CPU mirrors of texOcclusion + snowMaskTex (populated via async
	// D3D11 readback after SnowMaskCS dispatches) provide that without
	// repeating the GPU work on CPU.
	//
	// Update cadence: mirrors refresh once per cell change (same trigger as
	// the GPU mask).  `cpuMirrorsGeneration` increments on each refresh so
	// consumers (SnowDeformation slab updates) detect "fresh data available".

	// Snow mask CPU mirror — R8 byte per texel (0 = blocked, 255 = allowed).
	// Size = kResolution × kResolution (16 KB).
	eastl::vector<uint8_t> snowMaskCPU;

	// Scene height CPU mirror — float per texel, world-space Z reconstructed
	// by SnowMaskCS and written to sceneHeightWorldTex (R32_FLOAT 128×128).
	// Indexed by the same world-XY → mask UV math as snowMaskCPU; no
	// projection-matrix multiply needed on CPU.  16 KB total — trivial.
	eastl::vector<float> sceneHeightCPU;

	// Staging buffers — USAGE_STAGING + CPU_ACCESS_READ for the readback path.
	// We use intermediate textures because both source textures are
	// USAGE_DEFAULT (can't Map directly).  Both are R-format 128×128 here
	// (matching their source dims and formats exactly).
	eastl::unique_ptr<Texture2D> snowMaskStaging;
	eastl::unique_ptr<Texture2D> sceneHeightStaging;

	// Readback state machine:
	//   `readbackPending`  = staging copies were issued, awaiting map
	//   `cpuMirrorsValid`  = at least one successful map has populated mirrors
	//   `cpuMirrorsGeneration` = bumped each time mirrors refresh; consumers
	//                            compare against their own cached generation
	//                            to detect "new data since last seen"
	bool     readbackPending      = false;
	bool     cpuMirrorsValid      = false;
	uint64_t cpuMirrorsGeneration = 0;

	// CPU shadow used during progressive fill.  Re-uploaded to the GPU once
	// per full refresh (after all rows are populated).
	eastl::vector<float> waterMaskCPU;

	// World-space SW corner of the current cached window.  Recomputed when the
	// player crosses a cell boundary.
	float worldOriginX = 0.0f;
	float worldOriginY = 0.0f;

	// Cell-change detection — sentinel INT_MIN forces first-frame refresh.
	int cachedCellX = INT_MIN;
	int cachedCellY = INT_MIN;

	// Progressive fill state.  `fillRow` is the next row index to populate.
	// `ready` flips true when fillRow reaches kResolution AND we've uploaded
	// to GPU; resets to false on every cell change.
	uint32_t fillRow = 0;
	bool     ready   = false;

	// Scene height broker state (Layer 1.2).  `sceneHeightReady` is true only
	// when Skylighting is loaded AND its texOcclusion exists.  Updated each
	// Prepass from the Skylighting getters.  Drives both the shader-side
	// SceneHeightReady cbuffer flag and the SRV-binding logic.
	bool sceneHeightReady = false;

	// -------------------------------------------------------------------------
	// API consumed by FeatureBuffer + downstream features
	// -------------------------------------------------------------------------
	PerFrame GetCommonBufferData() const;

	// Explicit cache invalidation — separate from Feature::Reset() which is a
	// per-frame hook.  Call when world state changes invalidate cached water
	// data outside the normal cell-crossing flow (currently only wired to the
	// menu's "Force Refresh" button).
	void InvalidateCache();

	// =========================================================================
	// Consumer bind API
	// =========================================================================
	// Consumers (e.g. snow-mask CS, slab fragmenter PS) bind these textures
	// JUST-IN-TIME inside their own dispatch / draw pass.  We do NOT bind in
	// Prepass — that would conflict with Skylighting's per-frame DSV bind of
	// the same texOcclusion resource (RESOURCE_USAGE_CONFLICT → AMD TDR).
	//
	// Typical consumer pattern:
	//     auto& sh = globals::features::sceneHeight;
	//     ID3D11ShaderResourceView* srvs[2] = {
	//         sh.GetWaterMaskSRV(),
	//         sh.GetSceneOcclusionSRV(),
	//     };
	//     context->CSSetShaderResources(2, 2, srvs);
	//     // ... dispatch ...
	//     ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
	//     context->CSSetShaderResources(2, 2, nulls);
	//
	// Or call the bind/unbind helpers below to handle the PS slot 36/37
	// convention automatically.

	[[nodiscard]] ID3D11ShaderResourceView* GetWaterMaskSRV() const
	{
		return waterMaskTex ? waterMaskTex->srv.get() : nullptr;
	}

	[[nodiscard]] ID3D11ShaderResourceView* GetSnowMaskSRV() const
	{
		return snowMaskTex ? snowMaskTex->srv.get() : nullptr;
	}

	[[nodiscard]] bool IsSnowMaskValid() const { return snowMaskValid; }

	// Convenience: bind water mask + scene occlusion SRVs at their canonical
	// PS slots (t36, t37).  Caller is responsible for unbinding (typically by
	// just letting the next draw call overwrite, or calling UnbindFromPS()).
	void BindToPS();
	void UnbindFromPS();

	// =========================================================================
	// Layer 2 dispatch entry point
	// =========================================================================
	// Called from Deferred::DeferredPasses once per frame.  Checks gating
	// (water + scene-height both ready, not already valid for this cell),
	// dispatches the snow-mask CS if appropriate, marks `snowMaskValid` on
	// success.  Cheap when nothing to do (single bool check + early return).
	//
	// Dispatch from Deferred (not Prepass) because Skylighting's RenderOcclusion
	// completes before DeferredPasses fires.  Sampling texOcclusion at Prepass
	// time would read STALE data and risk the SRV+DSV conflict that crashed
	// AMD drivers (see GOTCHA #2 in project memory).
	void DispatchSnowMaskCSIfNeeded();

	// =========================================================================
	// CPU mirror sample API (Stage 1 of Layer 3)
	// =========================================================================
	// Once `cpuMirrorsValid` is true, consumers (e.g. SnowDeformation slab
	// update) can sample scene heights + snow mask at any world XY without
	// touching GPU resources.  Mirrors refresh asynchronously after each CS
	// dispatch — bump-detection via cpuMirrorsGeneration.

	[[nodiscard]] bool     AreCPUMirrorsValid() const { return cpuMirrorsValid; }
	[[nodiscard]] uint64_t GetCPUMirrorsGeneration() const { return cpuMirrorsGeneration; }

	// Samples the snow mask CPU mirror at a world position.  Returns:
	//   - 1.0  when mask is unavailable (graceful fallback — allow snow)
	//   - 0.0  when blocked by mask
	//   - 1.0  when allowed
	// Uses point sampling — caller should average neighbors if they want soft
	// transitions.
	[[nodiscard]] float SampleSnowMaskCPU(float worldX, float worldY) const;

	// Samples the scene-height CPU mirror at a world position, returns world Z
	// of the topmost surface (inverse-projected from texOcclusion's NDC z).
	// Returns kNoSceneHeightSentinel when out of cached window / Skylighting
	// inactive / data unavailable.
	static constexpr float kNoSceneHeightSentinel = -1.0e10f;
	[[nodiscard]] float SampleSceneHeightCPU(float worldX, float worldY) const;

	// -------------------------------------------------------------------------
	// Feature lifecycle overrides
	// -------------------------------------------------------------------------
	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual bool SupportsVR() override { return true; }

private:
	// Detects player cell change.  Returns true if the cached window needs
	// invalidation (i.e., a new refresh cycle should begin).
	bool DetectCellChange();

	// Recomputes worldOriginX/Y for the new cell and clears CPU state.
	void RebaseToNewCell(int newCellX, int newCellY);

	// Fills up to `rowCount` rows of the water mask starting at `fillRow`,
	// then advances `fillRow`.  Returns true if this call completed the
	// refresh (i.e., `fillRow` reached kResolution).
	bool FillWaterMaskRows(uint32_t rowCount);

	// Uploads the fully-populated CPU buffer to the GPU texture.
	void UploadWaterMaskToGPU();

	// Layer 1.2: scene-height readiness detection.  Updates `sceneHeightReady`
	// from Skylighting's current state.  Cheap (just pointer checks); safe
	// to call every Prepass.  Does NOT bind any SRV — that's the consumer's
	// job, called just-in-time via BindToPS() above.
	void DetectSceneHeightReadiness();

	// Layer 3 stage 1: CPU readback support.  Issued from
	// DispatchSnowMaskCSIfNeeded after the CS dispatch when staging targets
	// are ready.  Map happens in Prepass on a subsequent frame.
	void IssueCPUMirrorReadbacks();
	void TryFinalizeCPUMirrorReadbacks();
};
