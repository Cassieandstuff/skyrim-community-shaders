// Snow deformation grid math helpers.
// Converts absolute world XY to a toroidal grid UV suitable for sampling
// texSnowDeform / texSnowRidge with a WRAP address-mode sampler.
//
// Requires SharedData.hlsli to be included first (provides snowDeformationSettings).

#ifndef __SNOW_DEFORMATION_HLSLI__
#define __SNOW_DEFORMATION_HLSLI__

namespace SnowDeformGrid
{
	static const uint GRID_DIM = 512;

	// Convert absolute world XY to grid UV for sampling with WRAP address mode.
	// Formula: UV = ((worldXY - GridWorldOrigin) / GridCellSize + float2(ArrayOriginX, ArrayOriginY)) / GRID_DIM
	float2 WorldToGridUV(float2 worldXY)
	{
		SharedData::SnowDeformationSettings s = SharedData::snowDeformationSettings;
		float2 localXY = (worldXY - float2(s.GridWorldOriginX, s.GridWorldOriginY)) / s.GridCellSize;
		float2 arrayCoord = localXY + float2((float)s.ArrayOriginX, (float)s.ArrayOriginY);
		return arrayCoord / (float)GRID_DIM;
	}
}

#endif  // __SNOW_DEFORMATION_HLSLI__
