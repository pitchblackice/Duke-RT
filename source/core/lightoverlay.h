#pragma once

#include "tarray.h"
#include "zstring.h"

class PClassActor;

enum class LightOverlayAnchorType : uint8_t
{
	None,
	Position,
	Sector,
	Wall,
};

struct LightOverlaySourceLocation
{
	FString sourceName;
	int lumpNum = -1;
	int lineStart = 0;
	int lineEnd = 0;
	uint32_t orderIndex = 0;
};

struct ParsedLightOverlayDefaults
{
	bool present = false;
	LightOverlaySourceLocation source;
};

struct ParsedLightOverlayActorRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
	bool hasFullbright = false;
	bool fullbright = false;
	bool hasTileFilter = false;
	int tileFilter = -1;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNudgeFromSurface = false;
	float nudgeFromSurfaceDistance = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
	bool hasRandom = false;
	float randomIntensityRange[2] = { 0.0f, 0.0f };
	bool hasLocalSpacePolicy = false;
	FString localSpacePolicy;
};

struct ParsedLightOverlayDirectionalRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, -1.0f };
	bool hasAngularSize = false;
	float angularSize = 0.0f;
	bool hasShadow = false;
	bool shadow = true;
};

struct ParsedLightOverlayMuzzleFlashRule
{
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasIntensityRandom = false;
	float intensityRandomRange[2] = { 1.0f, 1.0f };
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRadiusRandom = false;
	float radiusRandomRange[2] = { 1.0f, 1.0f };
	bool hasDelaySeconds = false;
	float delaySeconds = 0.0f;
	bool hasDelayRandomSeconds = false;
	float delayRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasDurationSeconds = false;
	float durationSeconds = 0.0f;
	bool hasDurationRandomSeconds = false;
	float durationRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
};

struct ParsedLightOverlayMapLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString lightType;
	LightOverlayAnchorType anchorType = LightOverlayAnchorType::None;
	bool hasAnchorPosition = false;
	float anchorPosition[3] = { 0.0f, 0.0f, 0.0f };
	int anchorIndex = -1;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
};

struct ParsedLightOverlayEmissiveOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasSectorFilter = false;
	int sectorFilter = -1;
	bool hasWallFilter = false;
	int wallFilter = -1;
	bool hasTileFilter = false;
	int tileFilter = -1;
	bool hasIntensityScale = false;
	float intensityScale = 1.0f;
	bool hasReachScale = false;
	float reachScale = 1.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
};

struct ParsedLightOverlayActorOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
};

struct ParsedLightOverlaySourceFile
{
	FString sourceName;
	int lumpNum = -1;
	bool hadParseErrors = false;
};

struct ParsedLightOverlayDatabase
{
	uint32_t generation = 0;
	uint64_t contentHash = 0;
	bool hadParseErrors = false;
	ParsedLightOverlayDefaults defaults;
	TArray<ParsedLightOverlaySourceFile> sourceFiles;
	TArray<ParsedLightOverlayActorRule> actorRules;
	TArray<ParsedLightOverlayDirectionalRule> directionalRules;
	TArray<ParsedLightOverlayMuzzleFlashRule> muzzleFlashRules;
	TArray<ParsedLightOverlayMapLightRule> mapLightRules;
	TArray<ParsedLightOverlayEmissiveOverrideRule> emissiveOverrideRules;
	TArray<ParsedLightOverlayActorOverrideRule> actorOverrideRules;
};

struct ResolvedLightOverlayActorRule
{
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	PClassActor* actorClass = nullptr;
	bool actorClassResolved = false;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
	bool hasFullbright = false;
	bool fullbright = false;
	bool hasTileFilter = false;
	int tileFilter = -1;
	FString lightType;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasNudgeFromSurface = false;
	float nudgeFromSurfaceDistance = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
	bool hasRandom = false;
	float randomIntensityRange[2] = { 0.0f, 0.0f };
	bool hasLocalSpacePolicy = false;
	FString localSpacePolicy;
};

struct ResolvedLightOverlayDirectionalRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, -1.0f };
	bool hasAngularSize = false;
	float angularSize = 0.0f;
	bool hasShadow = false;
	bool shadow = true;
};

struct ResolvedLightOverlayMuzzleFlashRule
{
	FString id;
	LightOverlaySourceLocation source;
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasIntensityRandom = false;
	float intensityRandomRange[2] = { 1.0f, 1.0f };
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRadiusRandom = false;
	float radiusRandomRange[2] = { 1.0f, 1.0f };
	bool hasDelaySeconds = false;
	float delaySeconds = 0.0f;
	bool hasDelayRandomSeconds = false;
	float delayRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasDurationSeconds = false;
	float durationSeconds = 0.0f;
	bool hasDurationRandomSeconds = false;
	float durationRandomSecondsRange[2] = { 0.0f, 0.0f };
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
};

struct ResolvedLightOverlayMapLightRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString lightType;
	LightOverlayAnchorType anchorType = LightOverlayAnchorType::None;
	bool hasAnchorPosition = false;
	float anchorPosition[3] = { 0.0f, 0.0f, 0.0f };
	int anchorIndex = -1;
	bool hasOffset = false;
	float offset[3] = { 0.0f, 0.0f, 0.0f };
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasColor = false;
	float color[3] = { 1.0f, 1.0f, 1.0f };
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasRadius = false;
	float radius = 0.0f;
	bool hasRange = false;
	float range = 0.0f;
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
};

struct ResolvedLightOverlayEmissiveOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	bool hasSectorFilter = false;
	int sectorFilter = -1;
	bool hasWallFilter = false;
	int wallFilter = -1;
	bool hasTileFilter = false;
	int tileFilter = -1;
	bool hasIntensityScale = false;
	float intensityScale = 1.0f;
	bool hasReachScale = false;
	float reachScale = 1.0f;
	bool hasSectorResponse = false;
	bool sectorResponse = true;
	bool hasSignalSector = false;
	int signalSector = -1;
};

struct ResolvedLightOverlayActorOverrideRule
{
	FString mapName;
	FString id;
	LightOverlaySourceLocation source;
	FString actorClassName;
	PClassActor* actorClass = nullptr;
	bool actorClassResolved = false;
	bool hasShadowReceive = false;
	bool shadowReceive = true;
	bool hasShadowCast = false;
	bool shadowCast = true;
};

struct ResolvedLightOverlaySet
{
	uint32_t parsedGeneration = 0;
	uint32_t resolvedGeneration = 0;
	FString activeMapName;
	bool currentMapAvailable = false;
	TArray<ResolvedLightOverlayActorRule> actorRules;
	TArray<ResolvedLightOverlayDirectionalRule> directionalRules;
	TArray<ResolvedLightOverlayMuzzleFlashRule> muzzleFlashRules;
	TArray<ResolvedLightOverlayMapLightRule> mapLightRules;
	TArray<ResolvedLightOverlayEmissiveOverrideRule> emissiveOverrideRules;
	TArray<ResolvedLightOverlayActorOverrideRule> actorOverrideRules;
};

enum class LightOverlayRuleKind : uint8_t
{
	ActorRule,
	Directional,
	MuzzleFlash,
	MapLight,
	EmissiveOverride,
	ActorOverride,
};

const ParsedLightOverlayDatabase& GetParsedLightOverlayDatabase();
const ResolvedLightOverlaySet& GetResolvedLightOverlaySet();
const ResolvedLightOverlaySet& ResolveLightOverlaysForMap(const char* mapName);
bool ParseLightOverlays(bool verbose = false);
bool ApplyParsedLightOverlayDatabase(const ParsedLightOverlayDatabase& database, bool verbose = false);
FString SerializeLightOverlayDatabase(const ParsedLightOverlayDatabase& database);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayDirectionalRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMuzzleFlashRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayMapLightRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayEmissiveOverrideRule& rule, bool* outReplaced = nullptr);
bool AddOrReplaceLightOverlayRule(ParsedLightOverlayDatabase& database, const ParsedLightOverlayActorOverrideRule& rule, bool* outReplaced = nullptr);
bool RemoveLightOverlayRule(ParsedLightOverlayDatabase& database, LightOverlayRuleKind kind, const char* id, const char* mapName = nullptr);
