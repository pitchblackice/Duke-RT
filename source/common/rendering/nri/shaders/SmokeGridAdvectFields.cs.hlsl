#include "Include/SmokeGridResources.hlsli"

[numthreads(8, 8, 8)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	const uint activeIndex = groupId.x;
	if (activeIndex >= min(SmokeGridActiveCount(), gSmokeGridConstants.BrickCapacity))
		return;
	const uint brickIndex = SmokeGridActiveBrick(activeIndex);
	if (brickIndex >= gSmokeGridConstants.BrickCapacity)
		return;
	const SmokeGridBrick brick = gSmokeGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT)
		return;

	const uint inputPing = min(gSmokeGridConstants.FieldPing, 1u);
	const uint outputPing = 1u - inputPing;
	const uint cellIndex = SmokeGridCellIndex(brickIndex, groupThreadId);
	const float3 worldPosition = SmokeGridCellCenter(brick.Coordinate, groupThreadId,
		max(gSmokeGridConstants.CellSize, 0.0001));
	const float deltaTime = max(gSmokeGridConstants.DeltaTime * gSmokeGridConstants.TimeScale, 0.0);
	const float3 traceVelocity = SmokeGridLoadVelocity(outputPing, cellIndex).xyz;
	bool ignoredCflEvent, ignoredClampEvent;
	const float3 backtrace = SmokeGridBacktrace(worldPosition, traceVelocity, deltaTime,
		ignoredCflEvent, ignoredClampEvent);

	float4 scalar, ignoredVelocity, optical, dynamics;
	SmokeGridSampleFields(backtrace, inputPing, scalar, ignoredVelocity, optical, dynamics);
	const float mass = max(scalar.x, 0.0);
	const float inverseMass = mass > 1e-8 ? rcp(mass) : 0.0;
	const float densityRate = max(dynamics.x * inverseMass, 0.0) /
		max(gSmokeGridConstants.DensityHalfLifeScale, 0.001);
	const float coolingRate = max(dynamics.y * inverseMass, 0.0) /
		max(gSmokeGridConstants.CoolingScale, 0.001);
	const float densityDecay = exp(-densityRate * deltaTime);
	const float coolingDecay = exp(-coolingRate * deltaTime);

	scalar.x *= densityDecay;
	scalar.y *= densityDecay * coolingDecay;
	scalar.z *= densityDecay;
	scalar.w *= densityDecay;
	optical *= densityDecay;
	dynamics *= densityDecay;
	scalar.xyz = max(scalar.xyz, 0.0);
	optical = max(optical, 0.0);
	dynamics = max(dynamics, 0.0);

	const float previousDenominator = optical.w;
	const float anisotropy = previousDenominator > 1e-8 ?
		clamp(scalar.w / previousDenominator, -0.95, 0.95) : 0.0;
	optical.xyz = min(optical.xyz, scalar.z.xxx);
	optical.w = dot(optical.xyz, float3(0.2126, 0.7152, 0.0722));
	scalar.w = anisotropy * optical.w;

	if (!all(isfinite(scalar)) || !all(isfinite(optical)) || !all(isfinite(dynamics)))
	{
		scalar = 0.0;
		optical = 0.0;
		dynamics = 0.0;
		InterlockedAdd(gSmokeGridControl[0].NanRejects, 1u);
	}
	SmokeGridStoreScalar(outputPing, cellIndex, scalar);
	SmokeGridStoreOptical(outputPing, cellIndex, optical);
	SmokeGridStoreDynamics(outputPing, cellIndex, dynamics);
}
