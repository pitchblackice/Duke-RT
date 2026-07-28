#include "Include/SmokeDormantGridResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	if (index == 0u)
	{
		gDormantControl[0] = (SmokeDormantGridControl)0;
		gDormantControl[0].ArchiveCapacity = gDormantConstants.ArchiveCapacity;
		gDormantControl[0].HashCapacity = gDormantConstants.ArchiveHashCapacity;
		gDormantControl[0].FreeCount = gDormantConstants.ArchiveCapacity;
		gDormantControl[0].Epoch = gDormantConstants.SimulationEpoch;
		gDormantControl[0].FrameIndex = gDormantConstants.FrameIndex;
	}
	if (index < gDormantConstants.ArchiveCapacity)
	{
		gDormantRecords[index] = (SmokeDormantGridRecord)0;
		gDormantFreeList[index] = gDormantConstants.ArchiveCapacity - 1u - index;
	}
	if (index < gDormantConstants.ArchiveHashCapacity)
		gDormantHash[index] = (SmokeDormantGridHashEntry)0;
	if (index < gDormantConstants.DemotionCount + gDormantConstants.PromotionCount)
		gDormantResults[index] = (SmokeDormantGridResult)0;
}
