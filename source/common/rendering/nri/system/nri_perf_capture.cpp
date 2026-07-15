#include "nri_renderdevice.h"

#include "../renderer/nri_renderer.h"
#include "perf_capture.h"

void NRIRenderDevice::CaptureCompactPerfRendererStats(bool rendered)
{
	const PerfCompactCaptureToken token = PerfCompactCaptureGetCurrentToken();
	if (!token || mRenderer == nullptr) return;

	const auto& shell = mRenderer->GetLastPerfShellTraceStats();
	const auto& resource = mRenderer->GetLastPerfResourceTraceStats();
	PerfCompactNriStats stats = {};
	stats.frame = mLastFrameBoundaryStats.frameNumber;
	stats.totalMs = shell.totalMs;
	stats.initMs = shell.initResourcesMs;
	stats.mapMs = shell.mapWorldMs;
	stats.stateMs = shell.updateStateMs;
	stats.selectMs = shell.sceneSelectMs;
	stats.lightsMs = shell.sceneLightsMs;
	stats.frameGraphMs = shell.frameGraphMs;
	stats.postDiagnosticsMs = shell.postFrameDiagnosticsMs;
	stats.unattributedMs = shell.unattributedMs;
	stats.mutationMs = shell.runtimeMutationMs;
	stats.dynamicCaptureMs = shell.dynamicCaptureMs;
	stats.persistentBatchMs = shell.sceneSelectPersistentVoxelBatchMs;
	stats.materialBridgeMs = shell.sceneSelectMaterialBridgeMs;
	stats.texturesMs = shell.sceneSelectTexturesMs;
	stats.bufferUploadMs = shell.sceneSelectBufferUploadMs;
	stats.persistentVoxelAsMs = shell.persistentVoxelAsMs;
	stats.dynamicAsMs = shell.dynamicAsMs;
	stats.worldTlasMs = shell.worldTlasMs;
	stats.sceneDataMs = shell.sceneDataSetMs;
	stats.stateCommitMs = shell.sceneSelectStateCommitMs;
	stats.resourceWaitMs = resource.waitMs;
	stats.sceneUploadBytes = resource.sceneUploadBytes;
	stats.resourceWaitCalls = resource.waitCalls;
	stats.activePrimitives = shell.activePrimitiveCount;
	stats.dynamicPrimitives = shell.dynamicPrimitiveCount;
	stats.activeMaterials = shell.activeMaterialCount;
	stats.sceneInstances = shell.sceneInstanceCount;
	stats.mutationStructural = shell.runtimeMutationValidStructuralCount;
	stats.mutationMaterial = shell.runtimeMutationValidMaterialCount;
	stats.mutationResident = shell.runtimeMutationResidentApplyCount;
	stats.rendered = rendered;
	stats.valid = true;
	PerfCompactCaptureNoteNri(token, stats);
}

void NRIRenderDevice::CaptureCompactPerfFrameBoundary(bool presentOk)
{
	const PerfCompactCaptureToken token = PerfCompactCaptureGetCurrentToken();
	if (!token) return;
	PerfCompactBoundaryStats stats = {};
	stats.frame = mLastFrameBoundaryStats.frameNumber;
	stats.waitMs = mLastFrameBoundaryStats.waitMs;
	stats.waitPresentMs = mLastFrameBoundaryStats.waitForPresentMs;
	stats.acquireMs = mLastFrameBoundaryStats.acquireMs;
	stats.submitMs = mLastFrameBoundaryStats.submitMs;
	stats.presentMs = mLastFrameBoundaryStats.presentMs;
	stats.pathTraced = mLastFrameBoundaryStats.pathTracedSceneRendered;
	stats.presentOk = presentOk;
	stats.valid = true;
	PerfCompactCaptureNoteBoundary(token, stats);
}
