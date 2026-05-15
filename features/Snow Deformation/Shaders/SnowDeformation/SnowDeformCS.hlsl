// Snow deformation compute shader.
// One thread per grid cell (8x8 threadgroups, 64x64 dispatch for 512x512 grid).
//
// Per-cell work:
//   1. Read the GrassCollision actor-contact heightfield (t5) — already
//      rasterised from Havok collision spheres by the GrassCollision CS.
//   2. Compare against the visual snow surface Z to compute penetration depth.
//   3. Settle previous deformation, apply new contact deformation.
//   4. Write the updated deformation field (u0) and a Sobel-based ridge map (u1).
//
// Bindings:
//   t2  = TerrainHeightTex  (absolute world Z, R32_FLOAT, CPU-uploaded)
//   t3  = PrevDeformTex     (previous frame deformation, R16_FLOAT)
//   t5  = GrassCollisionTex (actor collision heightfield, R32G32_FLOAT, packed [0,1])
//   u0  = CurrDeformTex     (current frame deformation output, R16_FLOAT)
//   u1  = RidgeTex          (Sobel ridge magnitude output, R16_FLOAT)
//   b5  = SharedData cbuffer
//   b6  = FeatureData cbuffer
//   b12 = FrameBuffer cbuffer (bound by DeferredPasses, re-bound explicitly)

#include "Common/SharedData.hlsli"
#include "SnowDeformation/SnowDeformation.hlsli"

Texture2D<float>  TerrainHeightTex : register(t2);
Texture2D<float>  PrevDeformTex    : register(t3);
Texture2D<float4> GrassCollisionTex : register(t5);

RWTexture2D<float> CurrDeformTex : register(u0);
RWTexture2D<float> RidgeTex      : register(u1);

static const uint GRID_DIM_CS  = 512;
static const float ZRANGE_X    = 2048.0;   // GrassCollision pack range (camera-relative)
static const float ZRANGE_Y    = -2048.0;

// Sobel kernel — row-major 3x3
static const int  SOBEL_KX[9] = { -1, 0, 1, -2, 0, 2, -1, 0, 1 };
static const int  SOBEL_KY[9] = { -1, -2, -1, 0, 0, 0, 1, 2, 1 };
static const int2 SOBEL_OFFSETS[9] = {
	int2(-1, -1), int2(0, -1), int2(1, -1),
	int2(-1,  0), int2(0,  0), int2(1,  0),
	int2(-1,  1), int2(0,  1), int2(1,  1)
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 tc = DTid.xy;
	if (any(tc >= GRID_DIM_CS))
		return;

	SharedData::SnowDeformationSettings s = SharedData::snowDeformationSettings;

	// Absolute terrain Z for this cell (from CPU-uploaded height field)
	float terrainZ = TerrainHeightTex[tc];

	// Previous frame's deformation
	float prevDeform = PrevDeformTex[tc];

	// Visual snow surface Z = physics terrain + SnowLayerDepth - prevDeform
	float raisedSurfaceZ = terrainZ + s.SnowLayerDepth - prevDeform;

	// ---------- Contact height from GrassCollision heightfield ----------
	// GrassCollision rasterises Havok collision spheres into a 512×512
	// texture at t5.  Packed to [0,1] via ZRANGE.
	//   packed ≈ 0  →  no collision (height near ZRANGE.x = +2048 above camera)
	//   packed → 1  →  collision at ground level (ZRANGE.y = -2048 below camera)
	bool  actorContact  = false;
	float bestContactDepth = 0.0;

	float4 packed = GrassCollisionTex[tc];
	if (packed.x > 0.01)  // skip cells with negligible collision
	{
		// Unpack from [0,1] to camera-relative Z, then to absolute world Z
		float camRelZ = lerp(ZRANGE_X, ZRANGE_Y, packed.x);
		float contactZ = camRelZ + FrameBuffer::CameraPosAdjust[0].z;

		float penetration = raisedSurfaceZ - contactZ;
		if (penetration > 0.0)
		{
			actorContact = true;
			bestContactDepth = saturate(penetration / s.SnowLayerDepth);
		}
	}

	// ---------- Deformation update ----------

	// Exponential settling decay
	float newDeform = prevDeform * max(0.0, 1.0 - s.SettlingRate * s.DeltaTime);

	// Actor contact pushes displacement toward SnowLayerDepth (full compression)
	if (actorContact)
	{
		float target = s.SnowLayerDepth * bestContactDepth;
		newDeform = max(newDeform, target);
	}

	newDeform = clamp(newDeform, 0.0, s.SnowLayerDepth);
	CurrDeformTex[tc] = newDeform;

	// ---------- Sobel ridge computation (reads previous frame) ----------
	float gx = 0.0;
	float gy = 0.0;

	[unroll]
	for (int k = 0; k < 9; k++)
	{
		uint2 neighbor = (uint2)((int2)tc + SOBEL_OFFSETS[k] + int2(GRID_DIM_CS, GRID_DIM_CS)) & (GRID_DIM_CS - 1);
		float d = PrevDeformTex[neighbor];
		gx += (float)SOBEL_KX[k] * d;
		gy += (float)SOBEL_KY[k] * d;
	}

	RidgeTex[tc] = sqrt(gx * gx + gy * gy) * s.RidgeStrength;
}
