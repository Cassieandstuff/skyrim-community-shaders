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
			"Snow Deformation tessellates landscape terrain and displaces it based on actor footprints.",
			{ "GPU-based snow mesh deformation via hull/domain shaders",
				"Actor contact via GrassCollision Havok-sphere rasterization",
				"Toroidal 512x512 grid at 8 units/cell covers a 4096-unit radius",
				"Sobel ridge map for subtle snow-pile edge highlights",
				"1-frame lag (CS dispatch during DeferredPasses, DS reads previous output)" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	// Grid constants — mirror BloodDecalGrass for consistent world coverage
	static constexpr uint32_t GRID_DIM       = 512;
	static constexpr float    GRID_WORLD_SIZE = 4096.0f;
	static constexpr float    GRID_CELL_SIZE  = GRID_WORLD_SIZE / GRID_DIM;

	static constexpr uint32_t TERRAIN_FILL_ROWS_PER_FRAME = 32;

	// -------------------------------------------------------------------------
	// Settings (serialised to JSON)
	// -------------------------------------------------------------------------
	struct Settings
	{
		bool  Enable                = true;
		float SnowLayerDepth        = 8.0f;   // height of the default snow layer (= max compression depth)
		float SnowContactDepth      = 32.0f;  // actor-pixel height above terrain that counts as contact
		float SettlingRate          = 0.5f;   // fraction of deformation lost per second (exponential)
		float RidgeStrength         = 2.0f;   // multiplier on the Sobel gradient ridge output
		float TessellationScale     = 1.0f;   // max tess factor = TessellationScale * 64 (hardware limit)
		float TessellationFalloff   = 500.0f; // distance (world units) at which tess factor reaches 1
		float SnowAltitudeMin       = -8192.0f;// world Z below which displacement is skipped in DS (default: disabled)
		float SnowSlopeFactor       = 0.0f;   // reserved; slope-based fade (not used in v1)
		float TerrainSurfaceEpsilon = 4.0f;   // contact tolerance below terrain surface (float imprecision)
		bool  DebugForceDeform      = false;  // diagnostic: force fully-compressed state (terrain at physics level, no raise)
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
		float SnowLayerDepth   = 8.0f;   // mirrors Settings::SnowLayerDepth
		float SnowContactDepth = 32.0f;
		float SettlingRate     = 0.5f;
		float RidgeStrength    = 2.0f;

		// Row 3
		float TessellationScale   = 1.0f;
		float TessellationFalloff = 500.0f;
		float SnowAltitudeMin     = -8192.0f;
		float SnowSlopeFactor     = 0.0f;

		// Row 4
		float    DeltaTime              = 0.0f;
		float    TerrainSurfaceEpsilon  = 4.0f;
		uint32_t DebugForceDeform       = 0;     // mirrors Settings::DebugForceDeform
		uint32_t Reserved               = 1;
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);

	// -------------------------------------------------------------------------
	// State
	// -------------------------------------------------------------------------
	Settings settings;

	// GPU resources
	eastl::unique_ptr<Texture2D> texSnowDeform[2];     // ping-pong deformation field    (R16_FLOAT)
	eastl::unique_ptr<Texture2D> texSnowRidge;          // Sobel ridge magnitude          (R16_FLOAT)
	eastl::unique_ptr<Texture2D> texTerrainHeight;      // absolute world Z per cell      (R32_FLOAT)

	winrt::com_ptr<ID3D11ComputeShader>      snowDeformCS;
	winrt::com_ptr<ID3D11HullShader>         snowHS;
	winrt::com_ptr<ID3D11DomainShader>       snowDS;
	winrt::com_ptr<ID3D11SamplerState>       deformSampler;    // bilinear WRAP for DS/PS
	// Cached LESS_EQUAL depth-stencil state used by the DrawIndexed hook for
	// the tessellated snow shell draw. Created unconditionally in SetupResources.
	winrt::com_ptr<ID3D11DepthStencilState>  lessEqualDSS;

	// Snow sheet layer resources
	static constexpr uint32_t SNOW_SHEET_QUADS = 256;          // quads per axis
	static constexpr uint32_t SNOW_SHEET_VERTS = SNOW_SHEET_QUADS + 1;  // 257 verts per axis
	static constexpr uint32_t SNOW_SHEET_STRIDE = 12;          // float3 position
	static constexpr uint32_t SNOW_SHEET_TOTAL_VERTS = SNOW_SHEET_VERTS * SNOW_SHEET_VERTS;

	winrt::com_ptr<ID3D11Buffer>             snowSheetVB;
	winrt::com_ptr<ID3D11Buffer>             snowSheetIB;
	uint32_t                                 snowSheetIndexCount = 0;
	winrt::com_ptr<ID3D11VertexShader>       snowSheetVS;
	winrt::com_ptr<ID3D11HullShader>         snowSheetHS;
	winrt::com_ptr<ID3D11DomainShader>       snowSheetDS;
	winrt::com_ptr<ID3D11PixelShader>        snowSheetPS;
	winrt::com_ptr<ID3D11DepthStencilState>  snowSheetDSS;  // ALWAYS / write ON

	// Grid management (toroidal rolling window)
	int     prevCellIDX            = INT_MAX;
	int     prevCellIDY            = INT_MAX;
	int     currentArrayOriginX    = 0;
	int     currentArrayOriginY    = 0;
	int     currentValidMarginX    = 0;
	int     currentValidMarginY    = 0;
	float   currentGridWorldOriginX = 0.0f;
	float   currentGridWorldOriginY = 0.0f;
	uint32_t simFrameIndex         = 0;

	// Terrain height progressive fill
	bool                  terrainInitialized = false;
	uint32_t              terrainFillRow     = 0;
	eastl::vector<float>  terrainHeightCPU;

	// Tessellation hook state
	bool tessellationBound   = false;
	bool snowBatchPending    = false;  // set in SetupGeometry, consumed in DrawIndexed hook

	// -------------------------------------------------------------------------
	// API
	// -------------------------------------------------------------------------
	CommonBufferData GetCommonBufferData();
	void             UpdateTerrainHeight();
	void             DeformationPass();
	void             DrawSnowLayer();
	void             BindTessellationShaders();
	void             RestoreTessellationShaders();
	void             CompileShaders();

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
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ID3D11DeviceContext::DrawIndexed vtable hook — intercepts draw calls for
		// snow landscape batches and issues two draws: base terrain + tessellated snow shell.
		struct ID3D11DeviceContext_DrawIndexed
		{
			static void STDMETHODCALLTYPE thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};
};
