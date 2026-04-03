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
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
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
	bool hadParseErrors = false;
	ParsedLightOverlayDefaults defaults;
	TArray<ParsedLightOverlaySourceFile> sourceFiles;
	TArray<ParsedLightOverlayActorRule> actorRules;
	TArray<ParsedLightOverlayDirectionalRule> directionalRules;
	TArray<ParsedLightOverlayMapLightRule> mapLightRules;
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
	bool hasDirection = false;
	float direction[3] = { 0.0f, 0.0f, 0.0f };
	bool hasFlicker = false;
	uint32_t flickerFrames = 0;
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
	TArray<ResolvedLightOverlayMapLightRule> mapLightRules;
	TArray<ResolvedLightOverlayActorOverrideRule> actorOverrideRules;
};

const ParsedLightOverlayDatabase& GetParsedLightOverlayDatabase();
const ResolvedLightOverlaySet& GetResolvedLightOverlaySet();
const ResolvedLightOverlaySet& ResolveLightOverlaysForMap(const char* mapName);
bool ParseLightOverlays(bool verbose = false);
