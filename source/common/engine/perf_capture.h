#pragma once

#include <cstdint>

struct PerfCompactCaptureToken
{
	uint64_t epoch = 0;
	uint64_t presentationGeneration = 0;
	uint32_t recordIndex = 0;
	explicit operator bool() const { return epoch != 0; }
};

struct PerfCompactNriStats
{
	uint64_t frame = 0;
	uint64_t traceRendererFrame = 0;
	uint64_t traceSettingsKey = 0;
	uint64_t traceWorkloadKey = 0;
	double totalMs = 0.0, initMs = 0.0, mapMs = 0.0, stateMs = 0.0;
	double selectMs = 0.0, lightsMs = 0.0, frameGraphMs = 0.0;
	double postDiagnosticsMs = 0.0, unattributedMs = 0.0;
	double mutationMs = 0.0, dynamicCaptureMs = 0.0, persistentBatchMs = 0.0;
	double materialBridgeMs = 0.0, texturesMs = 0.0, bufferUploadMs = 0.0;
	double persistentVoxelAsMs = 0.0, dynamicAsMs = 0.0, worldTlasMs = 0.0;
	double sceneDataMs = 0.0, stateCommitMs = 0.0, resourceWaitMs = 0.0;
	uint64_t sceneUploadBytes = 0;
	uint32_t resourceWaitCalls = 0;
	uint32_t activePrimitives = 0, dynamicPrimitives = 0, activeMaterials = 0, sceneInstances = 0;
	uint32_t mutationStructural = 0, mutationMaterial = 0, mutationResident = 0;
	uint32_t traceRenderWidth = 0, traceRenderHeight = 0, traceOutputWidth = 0, traceOutputHeight = 0;
	uint32_t traceDispatchX = 0, traceDispatchY = 0, traceDispatchZ = 0;
	uint32_t traceLightBounces = 0, traceMirrorBounces = 0, tracePortalDepth = 0, traceEmissiveSamples = 0;
	uint32_t traceEmissiveRequestedSamples = 0, traceEmissivePrimaryBudget = 0;
	uint32_t traceIndirectSamplingRequestedMode = 0;
	uint32_t traceIndirectSamplingEffectiveMode = 0, traceIndirectSamplingActiveMode = 0;
	uint32_t traceHitDistanceReconstructionMode = 0;
	uint32_t traceRuntimeLights = 0, traceRuntimeLightTilesX = 0, traceRuntimeLightTilesY = 0;
	uint32_t traceRuntimeLightTileSize = 0, traceRuntimeLightTileIndices = 0, traceRuntimeLightMaxOccupancy = 0;
	uint32_t traceEmissivePrimitiveCount = 0;
	double traceEmissiveTotalPower = 0.0;
	uint32_t traceFlags = 0, traceDebugMode = 0, traceBootstrapMode = 0;
	uint32_t traceUpscalerKind = 0, traceUpscalerMode = 0, traceDenoiserMode = 0;
	uint32_t traceDirectScene = 0, traceDirectional = 0, traceDirectionalShadow = 0;
	uint32_t traceSplitShadow = 0, traceFastEmissiveShadow = 0, traceVisibleChunkGate = 0;
	bool rendered = false;
	bool valid = false;
};

struct PerfCompactBoundaryStats
{
	uint64_t frame = 0;
	double waitMs = 0.0, waitPresentMs = 0.0, acquireMs = 0.0, submitMs = 0.0, presentMs = 0.0;
	bool pathTraced = false;
	bool presentOk = false;
	bool valid = false;
};

struct PerfCompactOuterFrame
{
	uint64_t traceFrame = 0, presentationGeneration = 0, simulationGeneration = 0, engineGeneration = 0;
	int gametic = 0;
	double startFrameMs = 0.0, tryMs = 0.0, tryTracedMs = 0.0;
	double displayMs = 0.0, displayBeginMs = 0.0, displayRenderMs = 0.0;
	double displayOverlayMs = 0.0, displayUpdateMs = 0.0;
	double startTicMs = 0.0, musicMs = 0.0, frameMs = 0.0;
	double nriTotalMs = 0.0, nriInitializeMs = 0.0, nriFrameResourcesMs = 0.0;
	double nriUpdateStateMs = 0.0, nriSceneCaptureMs = 0.0, nriGeometryBuildMs = 0.0;
	double nriMaterialBuildMs = 0.0, nriSceneTexturesMs = 0.0, nriSceneBuffersMs = 0.0;
	double nriAccelerationMs = 0.0, nriFrameGraphMs = 0.0, nriTraceMs = 0.0;
	double nriDenoiseMs = 0.0, nriComposeMs = 0.0, nriUpscaleMs = 0.0, nriFinalMs = 0.0;
	int realtics = 0, availabletics = 0, counts = 0, ticks = 0, waitLoops = 0;
	bool doWait = false, zeroReturn = false, waitReturn = false, pausedReturn = false;
	bool displaySkipped = false, levelRendered = false, stateIsLevel = false, nriActive = false;
};

struct PerfCompactGpuTiming
{
	double segmentMs = 0.0, sceneMs = 0.0, traceMs = 0.0, traceDispatchMs = 0.0, denoiseMs = 0.0;
	double compositionMs = 0.0, upscaleMs = 0.0, finalMs = 0.0;
	uint32_t segmentCount = 0, invalidPairs = 0, droppedScopes = 0;
};

void PerfCompactCaptureBeginOuterFrame(uint64_t presentationGeneration);
void PerfCompactCaptureFlushIfReady();
bool PerfCompactCaptureTimingActive();
PerfCompactCaptureToken PerfCompactCaptureGetCurrentToken();
void PerfCompactCaptureNoteNri(const PerfCompactCaptureToken& token, const PerfCompactNriStats& stats);
void PerfCompactCaptureNoteBoundary(const PerfCompactCaptureToken& token, const PerfCompactBoundaryStats& stats);
void PerfCompactCaptureExpectGpuSegment(const PerfCompactCaptureToken& token);
void PerfCompactCaptureResolveGpuSegment(const PerfCompactCaptureToken& token, const PerfCompactGpuTiming& timing);
void PerfCompactCaptureEndOuterFrame(const PerfCompactOuterFrame& frame);
void PerfCompactCaptureAbort(const char* reason);
