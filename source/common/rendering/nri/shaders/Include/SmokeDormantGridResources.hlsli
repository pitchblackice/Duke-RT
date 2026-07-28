#ifndef NRI_SMOKE_DORMANT_GRID_RESOURCES_HLSLI
#define NRI_SMOKE_DORMANT_GRID_RESOURCES_HLSLI

#include "SmokeDormantGridData.hlsli"
#include "SmokeGridData.hlsli"
#include "NRI.hlsl"

RWStructuredBuffer<SmokeGridControl> gDormantFineControl : register(u0, space0);
RWStructuredBuffer<SmokeGridHashEntry> gDormantFineHash : register(u1, space0);
RWStructuredBuffer<SmokeGridBrick> gDormantFineBricks : register(u2, space0);
RWStructuredBuffer<uint> gDormantFineFreeList : register(u3, space0);
RWStructuredBuffer<uint> gDormantFineActiveA : register(u4, space0);
RWStructuredBuffer<uint> gDormantFineActiveB : register(u5, space0);
RWStructuredBuffer<float4> gDormantFineScalarA : register(u6, space0);
RWStructuredBuffer<float4> gDormantFineScalarB : register(u7, space0);
RWStructuredBuffer<float4> gDormantFineVelocityA : register(u8, space0);
RWStructuredBuffer<float4> gDormantFineVelocityB : register(u9, space0);
RWStructuredBuffer<float4> gDormantFineOpticalA : register(u10, space0);
RWStructuredBuffer<float4> gDormantFineOpticalB : register(u11, space0);
RWStructuredBuffer<float4> gDormantFineDynamicsA : register(u12, space0);
RWStructuredBuffer<float4> gDormantFineDynamicsB : register(u13, space0);
RWStructuredBuffer<int4> gDormantFineDeposit0 : register(u14, space0);
RWStructuredBuffer<int4> gDormantFineDeposit1 : register(u15, space0);
RWStructuredBuffer<int4> gDormantFineDeposit2 : register(u16, space0);
RWStructuredBuffer<int4> gDormantFineDeposit3 : register(u17, space0);

RWStructuredBuffer<SmokeDormantGridControl> gDormantControl : register(u0, space1);
RWStructuredBuffer<SmokeDormantGridHashEntry> gDormantHash : register(u1, space1);
RWStructuredBuffer<SmokeDormantGridRecord> gDormantRecords : register(u2, space1);
RWStructuredBuffer<uint> gDormantFreeList : register(u3, space1);
RWStructuredBuffer<float4> gDormantScalar : register(u4, space1);
RWStructuredBuffer<float4> gDormantVelocity : register(u5, space1);
RWStructuredBuffer<float4> gDormantOptical : register(u6, space1);
RWStructuredBuffer<float4> gDormantDynamics : register(u7, space1);
RWStructuredBuffer<SmokeDormantGridWork> gDormantDemotions : register(u8, space1);
RWStructuredBuffer<SmokeDormantGridWork> gDormantPromotions : register(u9, space1);
RWStructuredBuffer<SmokeDormantGridResult> gDormantResults : register(u10, space1);
RWStructuredBuffer<SmokeDormantGridInjection> gDormantInjections : register(u11, space1);

NRI_ROOT_CONSTANTS(SmokeDormantGridConstants, gDormantConstants, 0, 2);

uint DormantCellIndex(uint archiveIndex, uint localIndex)
{
	return archiveIndex * NRI_SMOKE_DORMANT_GRID_CELLS_PER_BRICK + localIndex;
}

float4 DormantLoadFineScalar(uint ping, uint index)
{
	return ping == 0u ? gDormantFineScalarA[index] : gDormantFineScalarB[index];
}

float4 DormantLoadFineVelocity(uint ping, uint index)
{
	return ping == 0u ? gDormantFineVelocityA[index] : gDormantFineVelocityB[index];
}

float4 DormantLoadFineOptical(uint ping, uint index)
{
	return ping == 0u ? gDormantFineOpticalA[index] : gDormantFineOpticalB[index];
}

float4 DormantLoadFineDynamics(uint ping, uint index)
{
	return ping == 0u ? gDormantFineDynamicsA[index] : gDormantFineDynamicsB[index];
}

void DormantStoreFineFields(uint ping, uint index, float4 scalar, float4 velocity,
	float4 optical, float4 dynamics)
{
	if (ping == 0u)
	{
		gDormantFineScalarA[index] = scalar;
		gDormantFineVelocityA[index] = velocity;
		gDormantFineOpticalA[index] = optical;
		gDormantFineDynamicsA[index] = dynamics;
	}
	else
	{
		gDormantFineScalarB[index] = scalar;
		gDormantFineVelocityB[index] = velocity;
		gDormantFineOpticalB[index] = optical;
		gDormantFineDynamicsB[index] = dynamics;
	}
}

bool DormantPopArchiveFree(out uint index)
{
	index = 0xffffffffu;
	uint previous;
	InterlockedAdd(gDormantControl[0].FreeCount, 0xffffffffu, previous);
	if (previous == 0u)
	{
		uint ignored;
		InterlockedAdd(gDormantControl[0].FreeCount, 1u, ignored);
		return false;
	}
	index = gDormantFreeList[previous - 1u];
	return index < gDormantConstants.ArchiveCapacity;
}

bool DormantPushArchiveFree(uint index)
{
	uint destination;
	InterlockedAdd(gDormantControl[0].FreeCount, 1u, destination);
	if (destination >= gDormantConstants.ArchiveCapacity)
	{
		uint ignored;
		InterlockedAdd(gDormantControl[0].FreeCount, 0xffffffffu, ignored);
		return false;
	}
	gDormantFreeList[destination] = index;
	return true;
}

bool DormantPopFineFree(out uint index)
{
	index = 0xffffffffu;
	uint previous;
	InterlockedAdd(gDormantFineControl[0].FreeCount, 0xffffffffu, previous);
	if (previous == 0u)
	{
		uint ignored;
		InterlockedAdd(gDormantFineControl[0].FreeCount, 1u, ignored);
		return false;
	}
	index = gDormantFineFreeList[previous - 1u];
	return index < gDormantConstants.FineBrickCapacity;
}

bool DormantPushFineFree(uint index)
{
	uint destination;
	InterlockedAdd(gDormantFineControl[0].FreeCount, 1u, destination);
	if (destination >= gDormantConstants.FineBrickCapacity)
	{
		uint ignored;
		InterlockedAdd(gDormantFineControl[0].FreeCount, 0xffffffffu, ignored);
		return false;
	}
	gDormantFineFreeList[destination] = index;
	return true;
}

#endif
