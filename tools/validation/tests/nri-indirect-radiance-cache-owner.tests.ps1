Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
function Read-RepoFile([string]$path) {
	Get-Content -LiteralPath (Join-Path $repo $path) -Raw
}
function Require([bool]$condition, [string]$message) {
	if (-not $condition) { throw $message }
}

$ownerHeader = Read-RepoFile 'source\common\rendering\nri\renderer\nri_indirect_radiance_cache.h'
$owner = Read-RepoFile 'source\common\rendering\nri\renderer\nri_indirect_radiance_cache.cpp'
$contracts = Read-RepoFile 'source\common\rendering\nri\renderer\nri_shader_contracts.h'
$shaderContract = Read-RepoFile 'source\common\rendering\nri\shaders\Include\IndirectRadianceCache.hlsli'
$shared = Read-RepoFile 'source\common\rendering\nri\shaders\Include\Shared.hlsli'
$trace = Read-RepoFile 'source\common\rendering\nri\shaders\TraceOpaque.cs.hlsl'
$pipelines = Read-RepoFile 'source\common\rendering\nri\renderer\nri_pipeline_state.cpp'
$renderer = Read-RepoFile 'source\common\rendering\nri\renderer\nri_renderer.h'
$dispatch = Read-RepoFile 'source\common\rendering\nri\renderer\nri_pass_dispatch.cpp'
$dispatchFactory = Read-RepoFile 'source\common\rendering\nri\renderer\nri_pass_dispatch_factory.cpp'
$cvars = Read-RepoFile 'source\common\rendering\nri\renderer\nri_cvars.cpp'
$cmake = Read-RepoFile 'source\CMakeLists.txt'

Require ($cvars -match 'CVAR\(Bool,\s*nri_ptindirectradiancecache,\s*false,\s*0\)') `
	'Indirect-radiance cache must remain default-disabled and session-only.'
Require ($owner -match 'if\s*\(!enabled\)[\s\S]{0,320}mActive\s*=\s*false;[\s\S]{0,80}return result;[\s\S]*EnsureResources') `
	'The disabled Prepare path must return before EnsureResources can allocate.'
Require ($ownerHeader -match 'mapIdentity[\s\S]*staticSceneIdentity[\s\S]*portalRouteIdentity[\s\S]*materialIdentity[\s\S]*mutationIdentity[\s\S]*voxelOccurrenceIdentity[\s\S]*lightingIdentity') `
	'Compatibility input must expose explicit scene, route, material, mutation, voxel, and lighting identities.'
Require ($owner -match 'CompareNRIIndirectRadianceCacheCompatibility[\s\S]*INVALID_MAP[\s\S]*INVALID_STATIC_SCENE[\s\S]*INVALID_PORTAL_ROUTE[\s\S]*INVALID_MATERIAL[\s\S]*INVALID_MUTATION[\s\S]*INVALID_VOXEL_OCCURRENCE[\s\S]*INVALID_LIGHTING') `
	'Compatibility comparison must fail closed for every required generation.'
Require ($dispatchFactory -match 'staticSceneIdentity[\s\S]{0,420}mStaticMapScene\.buildSerial') `
	'Static cache compatibility must use the static-scene serial.'
Require ($dispatchFactory -notmatch 'staticSceneIdentity[\s\S]{0,420}mWorldBlasContentGeneration') `
	'Dynamic world-BLAS generations must not force a physical cache clear every frame.'
Require ($ownerHeader -match 'MinimumEntryCount\s*=\s*131072[\s\S]*DefaultEntryCount\s*=\s*262144[\s\S]*MaximumEntryCount\s*=\s*262144') `
	'Initial cache capacity must remain explicitly bounded.'
Require ($contracts -match 'NRI_INDIRECT_RADIANCE_CACHE_RECORD_STRIDE\s*=\s*48') `
	'CPU cache records must retain the explicit 48-byte layout.'
Require ($shaderContract -match 'IndirectRadianceCacheRecord[\s\S]*gIndirectRadianceCachePrevious[\s\S]*gIndirectRadianceCacheCurrent[\s\S]*gIndirectRadianceCacheTelemetry') `
	'Shader contract must expose ping-pong records and telemetry in the cache-only set.'
Require ($contracts -match 'TELEMETRY_ACCEPTED_HITS') `
	'Cache telemetry must expose accepted hits explicitly.'
Require ($ownerHeader -match 'acceptedHitCount') `
	'CPU telemetry snapshot must publish accepted hits explicitly.'
Require ($ownerHeader -match 'TraceDispatch A/B is[\s\S]*authoritative cost measurement') `
	'Monolithic-dispatch sub-operation tick limitations must be explicit.'
Require ($owner -match 'fallbackStorageDescriptor[\s\S]*NRI_INDIRECT_RADIANCE_CACHE_DESCRIPTOR_NUM[\s\S]*:\s*fallback') `
	'Inactive cache bindings must alias a valid existing storage descriptor.'
Require ($owner -match 'mDescriptorSets\.data\(\)[\s\S]*mReadTableIndex\s*=\s*1u\s*-\s*mReadTableIndex') `
	'Cache ownership must rotate between immutable ping-pong descriptor sets.'
Require ($owner -notmatch 'mReadTableIndex\s*=\s*1u\s*-\s*mReadTableIndex[\s\S]{0,160}UpdateDescriptor') `
	'Ping-pong rotation must not rewrite a descriptor set already referenced by an in-flight dispatch.'
$prepareBody = [regex]::Match(
	$owner,
	'NRIIndirectRadianceCachePrepareResult NRIIndirectRadianceCache::Prepare[\s\S]*?(?=bool NRIIndirectRadianceCache::RecordPendingClear)').Value
Require ($prepareBody -notmatch 'UpdateDescriptorSets') `
	'Per-frame prepare/disable paths must not rewrite descriptor sets referenced by submitted frames.'
Require ($owner -match 'HOST_READBACK[\s\S]*GetRecordingCommandFenceValue[\s\S]*IsCommandFenceValueComplete') `
	'Telemetry readback must remain fence-driven and nonblocking.'
Require ($owner -match 'AdvanceFrame[\s\S]*GetRecordingCommandFenceValue[\s\S]*FrameCommitSlot[\s\S]*ReconcileFrameCommits[\s\S]*IsCommandFenceValueAbandoned') `
	'Clear completion and ping-pong rotation must be recoverable from an abandoned submission.'
Require ($owner -match 'SHADER_RESOURCE_STORAGE,\s*nri::StageBits::COMPUTE_SHADER[\s\S]{0,180}SHADER_RESOURCE_STORAGE,\s*nri::StageBits::COMPUTE_SHADER') `
	'Cache tables must retain an explicit cross-frame storage dependency.'
Require ($pipelines -match 'EnsureIndirectRadianceCachePipeline[\s\S]*DescriptorSetDesc descriptorSets\[6\][\s\S]*NRI_INDIRECT_RADIANCE_CACHE_REGISTER_SPACE') `
	'Cache variant must lazily own a separate six-set layout.'
Require ($pipelines -match 'EnsureIndirectRadianceCachePipeline[\s\S]*TraceOpaqueCache\.cs\.[\s\S]*CreateComputePipeline') `
	'Cache pipeline must be created only through its lazy infrastructure gate.'
Require ($pipelines -notmatch 'CreatePipelines[\s\S]*PipelineSlot::TraceOpaqueCache') `
	'Ordinary renderer initialization must not allocate the disabled cache pipeline.'
Require ($renderer -match 'TraceOpaque,\s*TraceOpaqueCache,\s*Composition') `
	'Renderer must expose a distinct cache pipeline slot without replacing TraceOpaque.'
Require ($dispatch -match 'indirectRadianceCacheActive\s*\?\s*NRIRenderer::PipelineSlot::TraceOpaqueCache\s*:\s*NRIRenderer::PipelineSlot::TraceOpaque') `
	'Only an active cache request may select the cache permutation; ordinary dispatch must retain TraceOpaque.'
Require ($trace -match 'defined\(NRI_INDIRECT_RADIANCE_CACHE\)[\s\S]*TryReadIndirectRadianceCache[\s\S]*TELEMETRY_EXACT_FALLBACK') `
	'Slice 4.1 must query the cache permutation and record exact fallback.'
Require ($trace -notmatch 'WriteIndirectRadianceCache') `
	'Slice 4.1 exact-miss mode must not publish a provisional radiance payload.'
Require ($dispatch -notmatch 'NRI_FLAG_INDIRECT_RADIANCE_CACHE_ACCEPT') `
	'Slice 4.1 must never authorize a historical cache hit.'
Require ($shared -match 'defined\(NRI_INDIRECT_RADIANCE_CACHE\)[\s\S]*IndirectRadianceCache\.hlsli') `
	'Cache resources must enter only the cache shader permutation.'

foreach ($extension in @('dxil', 'spirv')) {
	$artifact = "TraceOpaqueCache.cs.$extension"
	$commands = @($cmake -split "`r?`n" | Where-Object {
		$_ -match '^\s*COMMAND\s+' -and $_.Contains("/$artifact")
	})
	Require ($commands.Count -eq 1) "$artifact must have exactly one compile command."
	Require ($commands[0] -match '-DNRI_INDIRECT_RADIANCE_CACHE=1') `
		"$artifact must compile only the cache permutation."
}

$defaultTraceCommands = @($cmake -split "`r?`n" | Where-Object {
	$_ -match '^\s*COMMAND\s+' -and $_ -match '/TraceOpaque\.cs\.(?:dxil|spirv)"'
})
Require ($defaultTraceCommands.Count -eq 2) 'Ordinary TraceOpaque must retain exactly two compile commands.'
Require (-not ($defaultTraceCommands -match 'NRI_INDIRECT_RADIANCE_CACHE')) `
	'Ordinary TraceOpaque artifacts must not compile a cache shader branch.'

Write-Host 'NRI indirect-radiance cache owner contracts passed.'
