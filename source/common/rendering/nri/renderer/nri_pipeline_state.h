#pragma once

class NRIRenderer;

class NRIPipelineStateManager
{
public:
	static bool CreatePipelineLayout(NRIRenderer& renderer);
	static bool EnsureIndirectRadianceCachePipeline(NRIRenderer& renderer);
	static bool CreateTaaPipelineLayout(NRIRenderer& renderer);
	static bool CreatePresentPipelineLayout(NRIRenderer& renderer);
	static bool CreateExposurePipelineLayout(NRIRenderer& renderer);
	static bool CreateBloomPipelineLayout(NRIRenderer& renderer);
	static bool CreateVoxelComputePipelineLayout(NRIRenderer& renderer);
	static bool CreatePipelines(NRIRenderer& renderer);
};
