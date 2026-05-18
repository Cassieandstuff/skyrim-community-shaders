#pragma once

#include "Buffer.h"

struct SnowDeformation : Feature
{
public:
	virtual inline std::string      GetName() override { return "Snow Deformation"; }
	virtual inline std::string      GetShortName() override { return "SnowDeformation"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SNOW_DEFORMATION"; }
	virtual std::string_view        GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Snow Deformation builds per-quad snow BSTriShapes from each cell's heightmap data and attaches them as independent scene-graph objects, so the engine renders them through the standard chain (shadows, deferred lighting, fog) for free.  The mesh is owned by us — no terrain identity is inherited.",
			{ "TESObjectLAND::SetupMaterial hook computes snow heightmap = terrain.heights[q] + SnowLayerDepth",
				"BuildSnowQuadMesh constructs a fresh BSTriShape patterned after GlobalLandRenderInit",
				"17×17 vertex grid, 1536-uint16 triangle-strip IB, 28-byte landscape vertex layout",
				"PBR material via BSLightingShaderMaterialPBR + kVertexLighting flag (TruePBR pipeline)",
				"Snow textures loaded via the engine's canonical BSShaderManager::GetTexture loader",
				"Engine handles render passes: Z-prepass, 4 shadow cascades, deferred G-buffer, composite, fog",
				"Per-frame VB UAV deformation write — planned for Milestone 3",
				"Toroidal 512×512 deformation grid CS (output texture; consumer comes in M3)" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	// Toroidal-grid constants for the deformation CS (still running, output unused
	// in Milestone 1 — wired up for Milestone 2's per-vertex VB update).
	static constexpr uint32_t GRID_DIM       = 512;
	static constexpr float    GRID_WORLD_SIZE = 4096.0f;
	static constexpr float    GRID_CELL_SIZE  = GRID_WORLD_SIZE / GRID_DIM;
	static constexpr uint32_t TERRAIN_FILL_ROWS_PER_FRAME = GRID_DIM;

	// -------------------------------------------------------------------------
	// Settings (serialised to JSON)
	// -------------------------------------------------------------------------
	struct Settings
	{
		bool  Enable                = true;
		float SnowLayerDepth        = 8.0f;   // node-translate Z offset of each snow clone above its terrain quad
		float SnowContactDepth      = 32.0f;  // (M2) actor-pixel height above terrain that counts as contact
		float SettlingRate          = 0.5f;   // (M2) fraction of deformation lost per second
		float RidgeStrength         = 2.0f;   // (M2) Sobel ridge multiplier
		float SnowAltitudeMin       = -8192.0f;
		float TerrainSurfaceEpsilon = 4.0f;
		bool  DebugForceDeform      = false;  // (M2) force fully-compressed deformation in the CS output

		// Master switch for the mesh build path.  Currently off because the
		// manual BSTriShape + BSGraphics::TriShape + D3D11 buffer construction
		// triggers a GPU TDR (freeze, no crash log).  Likely cause is a vertex
		// format / stride mismatch — CommonLibSSE-NG's GetSize() reports
		// 28-byte packed (int16 UV + int16 normal) while our RE-derived
		// layout assumes 37-byte float-precision.  See discussion before
		// re-enabling.
		bool BuildAndAttachMesh = false;
	};

	// -------------------------------------------------------------------------
	// CommonBufferData — mirrors SnowDeformationSettings in SharedData.hlsli
	// Must stay byte-for-byte identical.
	// -------------------------------------------------------------------------
	struct alignas(16) CommonBufferData
	{
		// Row 0
		uint32_t Enabled         = 0;
		float    GridWorldSize   = 0.0f;
		float    GridCellSize    = 0.0f;
		float    RcpGridCellSize = 0.0f;

		// Row 1
		float   GridWorldOriginX = 0.0f;
		float   GridWorldOriginY = 0.0f;
		int32_t ArrayOriginX     = 0;
		int32_t ArrayOriginY     = 0;

		// Row 2
		float SnowLayerDepth   = 8.0f;
		float SnowContactDepth = 32.0f;
		float SettlingRate     = 0.5f;
		float RidgeStrength    = 2.0f;

		// Row 3 — kept for FeatureData layout compatibility with SharedData.hlsli.
		float TessellationScale   = 0.0625f;
		float TessellationFalloff = 500.0f;
		float SnowAltitudeMin     = -8192.0f;
		float SnowSlopeFactor     = 0.0f;

		// Row 4
		float    DeltaTime              = 0.0f;
		float    TerrainSurfaceEpsilon  = 4.0f;
		uint32_t DebugForceDeform       = 0;
		uint32_t Reserved               = 1;
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);

	// -------------------------------------------------------------------------
	// State
	// -------------------------------------------------------------------------
	Settings settings;

	// Deformation CS resources — output drives Milestone 2's vertex displacement.
	eastl::unique_ptr<Texture2D> texSnowDeform[2];
	eastl::unique_ptr<Texture2D> texSnowRidge;
	eastl::unique_ptr<Texture2D> texTerrainHeight;
	winrt::com_ptr<ID3D11ComputeShader> snowDeformCS;
	winrt::com_ptr<ID3D11SamplerState>  deformSampler;

	// PBR snow textures, lazily loaded via the engine's canonical texture loader
	// (BSShaderManager::GetTexture @ AE 0x141480030).  Living as NiPointers means
	// they participate in the engine's refcount-based texture cache — assigning
	// into a material's diffuseTexture/normalTexture/rmaosTexture members AddRefs
	// through NiPointer semantics, so the textures stay alive as long as ANY
	// snow slab references them, and are released when the last slab unloads.
	//
	// Loaded lazily on first slab build (EnsureSnowTexturesLoaded), not in
	// SetupResources — avoids holding refcounts on textures the engine's own
	// streaming pipeline may not have prefetched yet at plugin-init time.
	RE::NiPointer<RE::NiSourceTexture> snowDiffuseTexture;
	RE::NiPointer<RE::NiSourceTexture> snowNormalTexture;
	RE::NiPointer<RE::NiSourceTexture> snowRmaosTexture;

	// Toroidal-grid rolling-window state.
	int      prevCellIDX             = INT_MAX;
	int      prevCellIDY             = INT_MAX;
	int      currentArrayOriginX     = 0;
	int      currentArrayOriginY     = 0;
	int      currentValidMarginX     = 0;
	int      currentValidMarginY     = 0;
	float    currentGridWorldOriginX = 0.0f;
	float    currentGridWorldOriginY = 0.0f;
	uint32_t simFrameIndex           = 0;

	// CPU-side terrain height progressive fill (driven from Prepass).
	bool                  terrainInitialized = false;
	uint32_t              terrainFillRow     = 0;
	eastl::vector<float>  terrainHeightCPU;

	// Serialises concurrent SetupMaterial dispatch (cell loading can come from
	// the engine's worker thread pool).  Will protect the per-cell snow-mesh
	// registry once Milestone 1's builder lands.
	//
	// `mutable` so the const IsRegisteredSnowSlab() can lock during read-only
	// queries from the render thread (Skylighting's precip-occlusion hook).
	mutable std::mutex snowMeshMutex;

	// =========================================================================
	// Layer 3 Stage 1: active slab tracking
	// =========================================================================
	// Slabs built by BuildSnowQuadMesh get registered here so the per-frame
	// vertex update path can find them.  We hold raw pointers (NOT NiPointers)
	// because:
	//   - Scene-graph attach via AttachChild already gives the engine refcount
	//     ownership; we don't need our own ref.
	//   - Cell unload causes the engine to release; if we held a NiPointer,
	//     the slab would persist past the cell unload as a memory leak.
	//   - Stale pointers caught via the scene-graph walk validation: each
	//     update pass calls IsStillAttached() before touching geometry.
	//
	// `slabUpdateGen` parallels `activeSlabs` — the SceneHeight CPU-mirror
	// generation at which each slab was last vertex-updated.  When the
	// SceneHeight generation advances past a slab's recorded value, that
	// slab needs a fresh vertex pass.
	//
	// LIFETIME: hold NiPointer (strong refcount) for both the slab and its
	// parent.  Raw pointers in this list caused a use-after-free crash
	// (2026-05-18 11:06:27 log) — when the engine unloaded a cell, its
	// BSTriShape + NiNode got freed, our raw pointers dangled, and the next
	// frame's child-walk dereferenced freed memory.  With NiPointer, both
	// stay alive at our refcount until WE drop them.
	//
	// Engine-detach detection: each pass we walk `parent`'s children to verify
	// our slab is still there.  When the engine calls DetachChild during cell
	// unload, our slab leaves parent->children → we detect → drop both
	// NiPointers → engine-side refcount finally goes to 0 → slab dies.
	struct ActiveSlab {
		RE::NiPointer<RE::BSTriShape> triShape;        // strong ref
		RE::NiPointer<RE::NiNode>     parent;          // strong ref, for children walk
		float    worldOriginX  = 0.0f;
		float    worldOriginY  = 0.0f;
		uint64_t lastUpdateGen = 0;
		bool     everUpdated   = false;
	};
	eastl::vector<ActiveSlab> activeSlabs;

	// Reusable scratch buffer for UpdateSlabVerticesIfNeeded's per-vertex pack.
	// Sized on first use to LAND_VERTS × LAND_VERTEX_STRIDE; subsequent calls
	// reuse the allocation.  Member (not stack) so density bumps don't risk
	// overflowing worker-thread stacks (65×65 × 32 bytes = 132 KB).
	eastl::vector<uint8_t> slabVertexScratch;

	// Called per-Prepass: walks `activeSlabs`, finds entries whose mirror
	// generation is stale relative to SceneHeight's current generation, and
	// regenerates their vertex Z + cull state from the CPU mirrors.
	// First-update flips triangleCount from 0 to full so the slab appears.
	void UpdateSlabVerticesIfNeeded();

	// Called from BuildSnowQuadMesh after a slab is successfully constructed.
	// Stashes the slab into `activeSlabs` so UpdateSlabVerticesIfNeeded can
	// find it.  Caller has already attached the slab to the scene graph.
	void RegisterSlab(RE::BSTriShape* triShape, RE::NiNode* parent,
		float worldOriginX, float worldOriginY);

	// O(N) lookup over the active slab list — used by Skylighting's
	// precipitation-occlusion hook to skip our slabs during top-down depth
	// renders.  Without this exclusion, our slab geometry gets rendered into
	// texOcclusion at frame N, the SnowMaskCS reads it back as "scene Z" at
	// frame N+1, the slabs follow that Z, and we end up with a stable
	// feedback loop at the slab's own height (verified empirically in
	// 2026-05-18 diagnostic log: SceneHeight readbacks returned ~95% sentinel
	// + the remaining 5% clustered around layerDepth-stepped values like
	// -7081, -7049, -7017).
	//
	// Activeslab count is small (per-cell quads × loaded cells = ~4-16 slabs
	// in practice), so a linear scan under the snow-mesh mutex is fast enough
	// for the per-geometry per-frame call frequency.  If profiling ever shows
	// this as a bottleneck, switch to a separate `std::unordered_set` rebuilt
	// on activeSlabs mutations.
	[[nodiscard]] bool IsRegisteredSnowSlab(const RE::BSGeometry* geom) const noexcept;

	// (Previously declared LAND_VERTS_PER_QUAD = 289 — the engine's native
	// terrain quad density.  Removed after the slab mesh density bump
	// decoupled our internal grid from the engine's.  Slab vertex count
	// now derives from LAND_VERTS_PER_SIDE in SnowDeformation.cpp; the
	// engine's per-quad height data is no longer read at slab construction
	// time — SceneHeight's CPU mirror is the authoritative source.)

	// -------------------------------------------------------------------------
	// API
	// -------------------------------------------------------------------------
	CommonBufferData GetCommonBufferData();
	void             UpdateTerrainHeight();
	void             DeformationPass();
	void             CompileShaders();

	// TESObjectLAND::SetupMaterial hook handler.  For each cell-quad,
	// computes the snow heightmap (terrain heights + SnowLayerDepth) and
	// asks BuildSnowQuadMesh to produce a snow BSTriShape.  Future work:
	// attach the returned mesh to the scene graph, track for cleanup.
	void OnLandSetupMaterial(RE::TESObjectLAND* land);

	// Constructs a fresh, render-ready snow BSTriShape from a single quad's
	// 17×17 heightmap.  Builds VB+IB via Renderer::CreateTriShape, allocates a
	// BSTriShape + BSLightingShaderProperty + BSLightingShaderMaterialPBR, and
	// wires engine-loaded snow PBR textures (BSShaderManager::GetTexture) onto
	// the material so the standard lighting shader's PBR path can render the
	// slab through TruePBR's SetupMaterial pipeline without bespoke handling.
	// Returns the mesh ready for AttachChild.
	//
	// a_terrainGeom: the cell-quad's existing terrain BSTriShape (data->geom[q]).
	//                Currently unused in the build path (M2 textures come from
	//                the engine loader, not from terrain).  Retained as a context
	//                handle for the caller's scene-graph attachment phase and
	//                for M3 displacement work, which will read its VertexDesc/UV
	//                layout for shared-vertex deformation correspondence.
	RE::NiPointer<RE::BSTriShape> BuildSnowQuadMesh(RE::BSTriShape* a_terrainGeom);

	// =========================================================================
	// Engine texture loader (M2 — replaces M1's borrow-from-terrain hack)
	// =========================================================================
	//
	// LoadEngineTexture
	//   Loads a texture via the engine's canonical loader,
	//   BSShaderManager::GetTexture @ AE 0x141480030.  The engine builds the
	//   full pipeline: DDS file → ID3D11Texture2D + ID3D11ShaderResourceView →
	//   BSGraphics::Texture (refcount-tracked) → NiSourceTexture::rendererTexture
	//   → NiSourceTexture.  Result is bit-identical to anything the engine
	//   loads during normal scene processing — feeds into the standard lighting
	//   shader's PBR / SetupMaterial / SetupGeometry pipelines without any
	//   bespoke handling.
	//
	//   a_dataRelativePath: path relative to the Skyrim Data folder, e.g.
	//                       "Textures\\landscape\\snow\\snow01.dds".  We prepend
	//                       "Data\\" before invoking the engine entry point,
	//                       matching BSTextureSet::SetTexture (AE 0x1403272b0).
	//
	//   Returns either a real NiSourceTexture or the engine's default fallback
	//   (defaultTextureNormalMap / defaultTextureWhite from graphicsState) when
	//   the file is missing/corrupt — the engine never returns null for a
	//   well-formed call, so callers don't need to crash-handle nullptr.  To
	//   detect fallback, compare against
	//   `globals::game::graphicsState->GetRuntimeData().defaultTexture*`.
	//
	//   AE-only during development.  SE/VR addresses are a mechanical follow-up
	//   (RELOCATION_ID lookup) before release.
	static RE::NiPointer<RE::NiSourceTexture> LoadEngineTexture(const char* a_dataRelativePath);

	// EnsureSnowTexturesLoaded
	//   Lazy-load the snow PBR texture set into snowDiffuseTexture /
	//   snowNormalTexture / snowRmaosTexture.  Safe to call repeatedly — returns
	//   immediately once all three are non-null.  Texture paths are vanilla
	//   Skyrim snow ground textures by default; engine substitutes default
	//   textures for any missing file (notably the rmaos variant — vanilla
	//   Skyrim doesn't ship a PBR rmaos, so that slot lands on
	//   defaultTextureWhite which gives the PBR-neutral rough=1/metal=0/AO=1/F0=1).
	//   Future: load Snow Cover's PBR snow set or a feature-shipped texture pack.
	void EnsureSnowTexturesLoaded();

	// Feature interface
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;
	virtual bool SupportsVR() override { return false; }

	// -------------------------------------------------------------------------
	// Hooks
	// -------------------------------------------------------------------------
	struct Hooks
	{
		// TESObjectLAND::SetupMaterial — fires once per cell-quad after the engine
		// has built the terrain BSTriShape.  Same relocation TerrainHelper hooks
		// (Detours chains both).
		struct TESObjectLAND_SetupMaterial
		{
			static bool thunk(RE::TESObjectLAND* land);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};
};
