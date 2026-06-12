#pragma once

#include "nri_renderer.h"

struct NRISceneContribution
{
	const nri_scene::GeometryData* geometry = nullptr;
	const NRIRenderer::SceneBufferUploadProducerStamp* producerStamp = nullptr;
	const nri_scene::MaterialBridgeData* materials = nullptr;
	NRIRenderer::SceneBufferUploadDomain uploadDomain = NRIRenderer::SceneBufferUploadDomain::StaticOverlay;
};

struct NRISceneContributionReserve
{
	size_t vertices = 0;
	size_t indices = 0;
	size_t primitives = 0;
	size_t primitiveProvenance = 0;
	size_t materials = 0;
	size_t lightMetadata = 0;
	size_t textures = 0;
	size_t paletteLookup = 0;
};

struct NRISceneContributionAppendStats
{
	double* totalMs = nullptr;
	double* geometryMs = nullptr;
	double* materialMs = nullptr;
	uint32_t* primitiveCount = nullptr;
	uint32_t* materialCount = nullptr;
	NRIRenderer::PerfShellTraceStats::OverlayAppendSourceTraceEntry* sourceTrace = nullptr;
};

void AccumulateNRISceneContributionReserve(const NRISceneContribution& contribution, NRISceneContributionReserve& reserve);
void ReserveNRISceneContributionCapacity(
	const NRISceneContributionReserve& reserve,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge);
void AppendNRISceneContribution(
	const NRISceneContribution& contribution,
	NRISceneContributionAppendStats stats,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge,
	std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans);
