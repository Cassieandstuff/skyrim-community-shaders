// Snow sheet layer: separate mesh rendered on top of terrain.
// VS: passthrough world position
// HS: distance-adaptive tessellation
// DS: sample terrain height + deformation + GrassCollision, displace Z, reproject
// PS: snow surface (flat colour for initial test)

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

// ── Textures ─────────────────────────────────────────────────────────────────
Texture2D<float>  TerrainHeightDS  : register(t2);
Texture2D<float>  texSnowDeformDS  : register(t106);
Texture2D<float4> GrassCollisionDS : register(t5);
SamplerState      deformSamplerDS  : register(s8);

// ── VS output / HS input ──────────────────────────────────────────────────────
struct VS_OUTPUT
{
	float4 WorldPosition : POSITION0;
};

// ── Patch-constant output ─────────────────────────────────────────────────────
struct SnowSheet_PatchConstant
{
	float Edges[3]   : SV_TessFactor;
	float Inside[1]  : SV_InsideTessFactor;
};

// ── Vertex shader: passthrough ────────────────────────────────────────────────
VS_OUTPUT SnowSheetVS(float3 position : POSITION0)
{
	VS_OUTPUT v;
	v.WorldPosition = float4(position, 1.0);
	return v;
}

// ── Patch-constant function: distance-adaptive tessellation ───────────────────
SnowSheet_PatchConstant SnowSheet_PatchConstantFunc(
	InputPatch<VS_OUTPUT, 3> patch,
	uint                     PatchID : SV_PrimitiveID)
{
	SnowSheet_PatchConstant output;

	float3 patchCenter = (patch[0].WorldPosition.xyz +
	                      patch[1].WorldPosition.xyz +
	                      patch[2].WorldPosition.xyz) / 3.0;
	float dist = length(patchCenter);

	SharedData::SnowDeformationSettings s = SharedData::snowDeformationSettings;

	float maxFactor = s.TessellationScale * 64.0;
	float t         = saturate(dist / max(s.TessellationFalloff, 1.0));
	float factor    = max(lerp(maxFactor, 1.0, t), 1.0);

	output.Edges[0]  = factor;
	output.Edges[1]  = factor;
	output.Edges[2]  = factor;
	output.Inside[0] = factor;
	return output;
}

// ── Hull shader ───────────────────────────────────────────────────────────────
[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("SnowSheet_PatchConstantFunc")]
[maxtessfactor(64.0f)]
VS_OUTPUT SnowSheetHS(
	InputPatch<VS_OUTPUT, 3> patch,
	uint                     i       : SV_OutputControlPointID,
	uint                     PatchID : SV_PrimitiveID)
{
	return patch[i];
}

// ── Domain shader: displace + reproject ───────────────────────────────────────
[domain("tri")]
VS_OUTPUT SnowSheetDS(
	SnowSheet_PatchConstant          pcData,
	float3                           bary : SV_DomainLocation,
	const OutputPatch<VS_OUTPUT, 3>  patch)
{
	VS_OUTPUT v;
	v.WorldPosition = bary.x * patch[0].WorldPosition +
	                  bary.y * patch[1].WorldPosition +
	                  bary.z * patch[2].WorldPosition;

	SharedData::SnowDeformationSettings s = SharedData::snowDeformationSettings;

	// Convert camera-relative XY to absolute world XY
	float2 absXY = v.WorldPosition.xy + FrameBuffer::CameraPosAdjust[0].xy;

	// Grid UV
	float2 localXY    = (absXY - float2(s.GridWorldOriginX, s.GridWorldOriginY)) / s.GridCellSize;
	float2 arrayCoord = localXY + float2((float)s.ArrayOriginX, (float)s.ArrayOriginY);
	float2 gridUV     = arrayCoord / 512.0;

	// Sample terrain height at this position
	float terrainZ = TerrainHeightDS.SampleLevel(deformSamplerDS, gridUV, 0);

	// Undeformed snow surface = terrain + SnowLayerDepth
	float baseZ = terrainZ + s.SnowLayerDepth;

	// Deformation field (from CS — tracks actor-contact compression)
	float displacement = texSnowDeformDS.SampleLevel(deformSamplerDS, gridUV, 0);

	// Apply deformation: compression reduces the snow height
	float finalZ = baseZ - displacement;

	// GrassCollision contact: push further down if actor presses into snow
	float4 packed = GrassCollisionDS.SampleLevel(deformSamplerDS, gridUV, 0);
	if (packed.x > 0.01)
	{
		float camRelZ = lerp(2048.0, -2048.0, packed.x);
		float contactZ = camRelZ + FrameBuffer::CameraPosAdjust[0].z;
		if (contactZ < finalZ)
			finalZ = contactZ;
	}

	// Never go below the physics terrain
	finalZ = max(finalZ, terrainZ);

	v.WorldPosition = float4(v.WorldPosition.xy, finalZ, 1.0);
	v.WorldPosition = mul(FrameBuffer::CameraViewProj[0], v.WorldPosition);

	return v;
}

// ── Pixel shader (flat colour for initial test) ──────────────────────────────
float4 SnowSheetPS(float4 svPos : SV_Position) : SV_Target
{
	return float4(0.85, 0.88, 0.92, 1.0);
}
