#include "lightoverlay.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "cmdlib.h"
#include "c_dispatch.h"
#include "filesystem.h"
#include "coreactor.h"
#include "mapinfo.h"
#include "printf.h"
#include "sc_man.h"

namespace
{
	static ParsedLightOverlayDatabase GLightOverlayDatabase;
	static uint32_t GLightOverlayGeneration = 0;
	static ResolvedLightOverlaySet GResolvedLightOverlaySet;
	static uint32_t GResolvedLightOverlayGeneration = 0;

	static FString GetLumpDisplayName(int lumpNum)
	{
		const char* fullName = lumpNum >= 0 ? fileSystem.GetFileFullName(lumpNum) : nullptr;
		return fullName != nullptr ? FString(fullName) : FStringf("lump:%d", lumpNum);
	}

	static std::string MakeNormalizedKey(const FString& value)
	{
		return std::string(value.MakeLower().GetChars());
	}

	static std::string MakeMapScopedKey(const FString& mapName, const char* kind, const FString& id)
	{
		FString normalized;
		normalized << mapName.MakeLower() << "|" << kind << "|" << id.MakeLower();
		return std::string(normalized.GetChars());
	}

	static FString SourceLocationText(const LightOverlaySourceLocation& source)
	{
		return FStringf("%s:%d", source.sourceName.GetChars(), source.lineStart);
	}

	static const char* AnchorTypeName(LightOverlayAnchorType type)
	{
		switch (type)
		{
		case LightOverlayAnchorType::Position: return "position";
		case LightOverlayAnchorType::Sector: return "sector";
		case LightOverlayAnchorType::Wall: return "wall";
		default: return "none";
		}
	}

	static const char* ReceiverModeName(LightOverlayReceiverMode mode)
	{
		switch (mode)
		{
		case LightOverlayReceiverMode::NoShadowReceive: return "no_shadow_receive";
		default: return "default";
		}
	}

	static void CopyVector3(const float source[3], float destination[3])
	{
		destination[0] = source[0];
		destination[1] = source[1];
		destination[2] = source[2];
	}

	static FString CanonicalizeMapName(const char* mapName)
	{
		if (mapName == nullptr || mapName[0] == 0)
		{
			return "";
		}

		if (auto* map = FindMapByName(mapName))
		{
			return map->labelName;
		}

		return ExtractFileBase(mapName);
	}

	static FString GetCurrentResolvedMapName()
	{
		return currentLevel != nullptr ? currentLevel->labelName : FString("");
	}

	struct ParsedLightOverlayDatabaseBuilder
	{
		ParsedLightOverlayDatabase database;
		uint32_t nextOrderIndex = 0;
		std::unordered_map<std::string, int> actorRuleLookup;
		std::unordered_map<std::string, int> directionalRuleLookup;
		std::unordered_map<std::string, int> mapLightRuleLookup;
		std::unordered_map<std::string, int> actorOverrideLookup;

		void SetDefaults(const ParsedLightOverlayDefaults& defaults)
		{
			database.defaults = defaults;
		}

		void AddActorRule(const ParsedLightOverlayActorRule& rule)
		{
			const std::string key = MakeNormalizedKey(rule.id);
			auto it = actorRuleLookup.find(key);
			if (it == actorRuleLookup.end())
			{
				actorRuleLookup.emplace(key, database.actorRules.Size());
				database.actorRules.Push(rule);
				return;
			}

			auto& existing = database.actorRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate actorrule '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddDirectionalRule(const ParsedLightOverlayDirectionalRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "directional", rule.id);
			auto it = directionalRuleLookup.find(key);
			if (it == directionalRuleLookup.end())
			{
				directionalRuleLookup.emplace(key, database.directionalRules.Size());
				database.directionalRules.Push(rule);
				return;
			}

			auto& existing = database.directionalRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate directional rule '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddMapLightRule(const ParsedLightOverlayMapLightRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "light", rule.id);
			auto it = mapLightRuleLookup.find(key);
			if (it == mapLightRuleLookup.end())
			{
				mapLightRuleLookup.emplace(key, database.mapLightRules.Size());
				database.mapLightRules.Push(rule);
				return;
			}

			auto& existing = database.mapLightRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate light rule '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}

		void AddActorOverrideRule(const ParsedLightOverlayActorOverrideRule& rule)
		{
			const std::string key = MakeMapScopedKey(rule.mapName, "actoroverride", rule.id);
			auto it = actorOverrideLookup.find(key);
			if (it == actorOverrideLookup.end())
			{
				actorOverrideLookup.emplace(key, database.actorOverrideRules.Size());
				database.actorOverrideRules.Push(rule);
				return;
			}

			auto& existing = database.actorOverrideRules[it->second];
			if (existing.source.lumpNum == rule.source.lumpNum)
			{
				Printf(TEXTCOLOR_ORANGE "LIGHTOVR warning: duplicate actoroverride '%s' for map '%s' in %s; using the last definition.\n",
					rule.id.GetChars(), rule.mapName.GetChars(), rule.source.sourceName.GetChars());
			}
			existing = rule;
		}
	};

	class LightOverlayParser
	{
	public:
		LightOverlayParser(int lumpNum, ParsedLightOverlayDatabaseBuilder& builder, ParsedLightOverlaySourceFile& sourceFile)
			: sc(lumpNum), builder(builder), sourceFile(sourceFile), lumpNum(lumpNum)
		{
			sc.SetCMode(true);
			sc.SetNoFatalErrors(true);
			sc.SetPrependMessage("LIGHTOVR: ");
		}

		void Parse()
		{
			while (sc.GetString())
			{
				if (sc.Compare("LIGHTOVR"))
				{
					ParseRootBlock();
				}
				else
				{
					sc.ScriptMessage("Unknown top-level token '%s'; expected LIGHTOVR", sc.String);
					SkipUnknownDefinition();
				}
			}
			sourceFile.hadParseErrors = sc.ParseError;
		}

	private:
		FScanner sc;
		ParsedLightOverlayDatabaseBuilder& builder;
		ParsedLightOverlaySourceFile& sourceFile;
		int lumpNum = -1;

		LightOverlaySourceLocation MakeSourceLocation(int lineStart)
		{
			LightOverlaySourceLocation location;
			location.sourceName = sourceFile.sourceName;
			location.lumpNum = lumpNum;
			location.lineStart = lineStart;
			location.lineEnd = sc.GetMessageLine();
			location.orderIndex = ++builder.nextOrderIndex;
			return location;
		}

		void FinalizeSourceLocation(LightOverlaySourceLocation& location)
		{
			location.lineEnd = sc.GetMessageLine();
		}

		static bool ParseOnOffToken(const char* token, bool& outValue)
		{
			if (!stricmp(token, "on") || !stricmp(token, "true") || !strcmp(token, "1"))
			{
				outValue = true;
				return true;
			}
			if (!stricmp(token, "off") || !stricmp(token, "false") || !strcmp(token, "0"))
			{
				outValue = false;
				return true;
			}
			return false;
		}

		void MustParseVector3(float outValue[3])
		{
			sc.MustGetFloat();
			outValue[0] = (float)sc.Float;
			sc.MustGetFloat();
			outValue[1] = (float)sc.Float;
			sc.MustGetFloat();
			outValue[2] = (float)sc.Float;
		}

		void SkipUnknownField(const char* context, const char* fieldName)
		{
			sc.ScriptMessage("Unknown field '%s' in %s definition", fieldName, context);
			if (sc.CheckString("{"))
			{
				sc.SkipToEndOfBlock();
				return;
			}
			sc.MustGetAnyToken();
		}

		void SkipUnknownDefinition()
		{
			while (sc.GetString())
			{
				if (sc.Compare("{"))
				{
					sc.SkipToEndOfBlock();
					return;
				}
				if (sc.Compare("}"))
				{
					sc.UnGet();
					return;
				}
			}
		}

		void ParseRootBlock()
		{
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("defaults"))
				{
					ParseDefaultsBlock();
				}
				else if (sc.Compare("actorrule"))
				{
					ParseActorRule();
				}
				else if (sc.Compare("map"))
				{
					ParseMapBlock();
				}
				else
				{
					sc.ScriptMessage("Unknown LIGHTOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseDefaultsBlock()
		{
			const int lineStart = sc.GetMessageLine();
			ParsedLightOverlayDefaults defaults;
			defaults.present = true;
			defaults.source = MakeSourceLocation(lineStart);

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				SkipUnknownField("defaults", sc.String);
			}

			FinalizeSourceLocation(defaults.source);
			builder.SetDefaults(defaults);
		}

		void ParseActorRule()
		{
			sc.MustGetString();
			ParsedLightOverlayActorRule rule;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("actorclass"))
				{
					sc.MustGetString();
					rule.actorClassName = sc.String;
				}
				else if (sc.Compare("tile"))
				{
					sc.MustGetNumber();
					rule.hasTileFilter = true;
					rule.tileFilter = sc.Number;
				}
				else if (sc.Compare("type"))
				{
					sc.MustGetString();
					rule.lightType = FString(sc.String).MakeLower();
				}
				else if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("range"))
				{
					sc.MustGetFloat();
					rule.hasRange = true;
					rule.range = (float)sc.Float;
				}
				else if (sc.Compare("offset"))
				{
					rule.hasOffset = true;
					MustParseVector3(rule.offset);
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("flicker"))
				{
					sc.MustGetNumber();
					rule.hasFlicker = true;
					rule.flickerFrames = std::max(sc.Number, 0);
				}
				else if (sc.Compare("localspace"))
				{
					sc.MustGetString();
					rule.hasLocalSpacePolicy = true;
					rule.localSpacePolicy = sc.String;
				}
				else
				{
					SkipUnknownField("actorrule", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddActorRule(rule);
		}

		void ParseMapBlock()
		{
			sc.MustGetString();
			FString mapName = sc.String;

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("directional"))
				{
					ParseDirectionalRule(mapName);
				}
				else if (sc.Compare("light"))
				{
					ParseMapLightRule(mapName);
				}
				else if (sc.Compare("actoroverride"))
				{
					ParseActorOverrideRule(mapName);
				}
				else
				{
					sc.ScriptMessage("Unknown map-local LIGHTOVR block '%s'", sc.String);
					SkipUnknownDefinition();
				}
			}
		}

		void ParseDirectionalRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayDirectionalRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("angularsize"))
				{
					sc.MustGetFloat();
					rule.hasAngularSize = true;
					rule.angularSize = (float)sc.Float;
				}
				else if (sc.Compare("shadow"))
				{
					sc.MustGetString();
					bool parsed = false;
					rule.hasShadow = ParseOnOffToken(sc.String, rule.shadow);
					parsed = rule.hasShadow;
					if (!parsed)
					{
						sc.ScriptMessage("Invalid shadow value '%s'; expected on/off", sc.String);
					}
				}
				else
				{
					SkipUnknownField("directional", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddDirectionalRule(rule);
		}

		void ParseMapLightRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayMapLightRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("type"))
				{
					sc.MustGetString();
					rule.lightType = FString(sc.String).MakeLower();
				}
				else if (sc.Compare("anchor"))
				{
					sc.MustGetString();
					if (sc.Compare("position"))
					{
						rule.anchorType = LightOverlayAnchorType::Position;
						rule.hasAnchorPosition = true;
						MustParseVector3(rule.anchorPosition);
					}
					else if (sc.Compare("sector"))
					{
						rule.anchorType = LightOverlayAnchorType::Sector;
						sc.MustGetNumber();
						rule.anchorIndex = sc.Number;
					}
					else if (sc.Compare("wall"))
					{
						rule.anchorType = LightOverlayAnchorType::Wall;
						sc.MustGetNumber();
						rule.anchorIndex = sc.Number;
					}
					else
					{
						sc.ScriptMessage("Unknown anchor type '%s' in light definition", sc.String);
					}
				}
				else if (sc.Compare("offset"))
				{
					rule.hasOffset = true;
					MustParseVector3(rule.offset);
				}
				else if (sc.Compare("direction"))
				{
					rule.hasDirection = true;
					MustParseVector3(rule.direction);
				}
				else if (sc.Compare("color"))
				{
					rule.hasColor = true;
					MustParseVector3(rule.color);
				}
				else if (sc.Compare("intensity"))
				{
					sc.MustGetFloat();
					rule.hasIntensity = true;
					rule.intensity = (float)sc.Float;
				}
				else if (sc.Compare("radius"))
				{
					sc.MustGetFloat();
					rule.hasRadius = true;
					rule.radius = (float)sc.Float;
				}
				else if (sc.Compare("range"))
				{
					sc.MustGetFloat();
					rule.hasRange = true;
					rule.range = (float)sc.Float;
				}
				else if (sc.Compare("flicker"))
				{
					sc.MustGetNumber();
					rule.hasFlicker = true;
					rule.flickerFrames = std::max(sc.Number, 0);
				}
				else
				{
					SkipUnknownField("light", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddMapLightRule(rule);
		}

		void ParseActorOverrideRule(const FString& mapName)
		{
			sc.MustGetString();
			ParsedLightOverlayActorOverrideRule rule;
			rule.mapName = mapName;
			rule.id = sc.String;
			rule.source = MakeSourceLocation(sc.GetMessageLine());

			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("actorclass"))
				{
					sc.MustGetString();
					rule.actorClassName = sc.String;
				}
				else if (sc.Compare("shadowreceive"))
				{
					sc.MustGetString();
					rule.hasReceiverMode = true;
					if (!stricmp(sc.String, "off"))
					{
						rule.receiverMode = LightOverlayReceiverMode::NoShadowReceive;
					}
					else if (!stricmp(sc.String, "default") || !stricmp(sc.String, "on"))
					{
						rule.receiverMode = LightOverlayReceiverMode::Default;
					}
					else
					{
						sc.ScriptMessage("Invalid shadowreceive value '%s'; expected off/default", sc.String);
					}
				}
				else
				{
					SkipUnknownField("actoroverride", sc.String);
				}
			}

			FinalizeSourceLocation(rule.source);
			builder.AddActorOverrideRule(rule);
		}
	};

	template <typename RuleType>
	static std::vector<const RuleType*> SortRulesByOrder(const TArray<RuleType>& rules)
	{
		std::vector<const RuleType*> sorted;
		sorted.reserve(rules.Size());
		for (auto& rule : rules)
		{
			sorted.push_back(&rule);
		}
		std::sort(sorted.begin(), sorted.end(), [](const RuleType* left, const RuleType* right)
		{
			if (left->source.orderIndex != right->source.orderIndex)
			{
				return left->source.orderIndex < right->source.orderIndex;
			}
			if (left->source.sourceName.CompareNoCase(right->source.sourceName) != 0)
			{
				return left->source.sourceName.CompareNoCase(right->source.sourceName) < 0;
			}
			return left->id.CompareNoCase(right->id) < 0;
		});
		return sorted;
	}

	static void DumpParsedLightOverlayDatabase(const ParsedLightOverlayDatabase& database)
	{
		Printf("LIGHTOVR: generation=%u files=%d actor_rules=%d map_lights=%d directional=%d actor_overrides=%d parse_errors=%s\n",
			database.generation,
			database.sourceFiles.Size(),
			database.actorRules.Size(),
			database.mapLightRules.Size(),
			database.directionalRules.Size(),
			database.actorOverrideRules.Size(),
			database.hadParseErrors ? "yes" : "no");

		for (const auto& sourceFile : database.sourceFiles)
		{
			Printf("LIGHTOVR file: %s parse_errors=%s\n",
				sourceFile.sourceName.GetChars(),
				sourceFile.hadParseErrors ? "yes" : "no");
		}

		if (database.defaults.present)
		{
			Printf("LIGHTOVR defaults: source=%s\n", SourceLocationText(database.defaults.source).GetChars());
		}

		for (const ParsedLightOverlayActorRule* rule : SortRulesByOrder(database.actorRules))
		{
			Printf("LIGHTOVR actorrule %s: actorclass=%s type=%s source=%s\n",
				rule->id.GetChars(),
				rule->actorClassName.GetChars(),
				rule->lightType.GetChars(),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasTileFilter) Printf("  tile=%d\n", rule->tileFilter);
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasRadius) Printf("  radius=%.3f\n", rule->radius);
			if (rule->hasRange) Printf("  range=%.3f\n", rule->range);
			if (rule->hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule->offset[0], rule->offset[1], rule->offset[2]);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasFlicker) Printf("  flicker_frames=%u\n", rule->flickerFrames);
			if (rule->hasLocalSpacePolicy) Printf("  localspace=%s\n", rule->localSpacePolicy.GetChars());
		}

		for (const ParsedLightOverlayDirectionalRule* rule : SortRulesByOrder(database.directionalRules))
		{
			Printf("LIGHTOVR directional %s map=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasAngularSize) Printf("  angularsize=%.3f\n", rule->angularSize);
			if (rule->hasShadow) Printf("  shadow=%s\n", rule->shadow ? "on" : "off");
		}

		for (const ParsedLightOverlayMapLightRule* rule : SortRulesByOrder(database.mapLightRules))
		{
			Printf("LIGHTOVR light %s map=%s type=%s anchor=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				rule->lightType.GetChars(),
				AnchorTypeName(rule->anchorType),
				SourceLocationText(rule->source).GetChars());
			if (rule->hasAnchorPosition) Printf("  anchor_position=(%.3f, %.3f, %.3f)\n", rule->anchorPosition[0], rule->anchorPosition[1], rule->anchorPosition[2]);
			if (rule->anchorType == LightOverlayAnchorType::Sector || rule->anchorType == LightOverlayAnchorType::Wall) Printf("  anchor_index=%d\n", rule->anchorIndex);
			if (rule->hasOffset) Printf("  offset=(%.3f, %.3f, %.3f)\n", rule->offset[0], rule->offset[1], rule->offset[2]);
			if (rule->hasDirection) Printf("  direction=(%.3f, %.3f, %.3f)\n", rule->direction[0], rule->direction[1], rule->direction[2]);
			if (rule->hasColor) Printf("  color=(%.3f, %.3f, %.3f)\n", rule->color[0], rule->color[1], rule->color[2]);
			if (rule->hasIntensity) Printf("  intensity=%.3f\n", rule->intensity);
			if (rule->hasRadius) Printf("  radius=%.3f\n", rule->radius);
			if (rule->hasRange) Printf("  range=%.3f\n", rule->range);
			if (rule->hasFlicker) Printf("  flicker_frames=%u\n", rule->flickerFrames);
		}

		for (const ParsedLightOverlayActorOverrideRule* rule : SortRulesByOrder(database.actorOverrideRules))
		{
			Printf("LIGHTOVR actoroverride %s map=%s actorclass=%s receiver=%s source=%s\n",
				rule->id.GetChars(),
				rule->mapName.GetChars(),
				rule->actorClassName.GetChars(),
				ReceiverModeName(rule->receiverMode),
				SourceLocationText(rule->source).GetChars());
		}
	}

	static void DumpResolvedLightOverlaySet(const ResolvedLightOverlaySet& resolved)
	{
		Printf("LIGHTOVR resolved: parsed_generation=%u resolved_generation=%u map=%s current_map=%s actor_rules=%d map_lights=%d directional=%d actor_overrides=%d\n",
			resolved.parsedGeneration,
			resolved.resolvedGeneration,
			resolved.activeMapName.IsNotEmpty() ? resolved.activeMapName.GetChars() : "(none)",
			resolved.currentMapAvailable ? "yes" : "no",
			resolved.actorRules.Size(),
			resolved.mapLightRules.Size(),
			resolved.directionalRules.Size(),
			resolved.actorOverrideRules.Size());

		for (const auto& rule : resolved.actorRules)
		{
			Printf("LIGHTOVR resolved actorrule %s: actorclass=%s resolved=%s type=%s source=%s\n",
				rule.id.GetChars(),
				rule.actorClassName.GetChars(),
				rule.actorClassResolved ? "yes" : "no",
				rule.lightType.GetChars(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.directionalRules)
		{
			Printf("LIGHTOVR resolved directional %s map=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.mapLightRules)
		{
			Printf("LIGHTOVR resolved light %s map=%s type=%s anchor=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				rule.lightType.GetChars(),
				AnchorTypeName(rule.anchorType),
				SourceLocationText(rule.source).GetChars());
		}

		for (const auto& rule : resolved.actorOverrideRules)
		{
			Printf("LIGHTOVR resolved actoroverride %s map=%s actorclass=%s resolved=%s receiver=%s source=%s\n",
				rule.id.GetChars(),
				rule.mapName.GetChars(),
				rule.actorClassName.GetChars(),
				rule.actorClassResolved ? "yes" : "no",
				ReceiverModeName(rule.receiverMode),
				SourceLocationText(rule.source).GetChars());
		}
	}

	static void CopyActorRule(const ParsedLightOverlayActorRule& source, ResolvedLightOverlayActorRule& destination)
	{
		destination.id = source.id;
		destination.source = source.source;
		destination.actorClassName = source.actorClassName;
		destination.actorClass = PClass::FindActor(source.actorClassName);
		destination.actorClassResolved = destination.actorClass != nullptr;
		destination.hasTileFilter = source.hasTileFilter;
		destination.tileFilter = source.tileFilter;
		destination.lightType = source.lightType;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasRange = source.hasRange;
		destination.range = source.range;
		destination.hasOffset = source.hasOffset;
		CopyVector3(source.offset, destination.offset);
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasFlicker = source.hasFlicker;
		destination.flickerFrames = source.flickerFrames;
		destination.hasLocalSpacePolicy = source.hasLocalSpacePolicy;
		destination.localSpacePolicy = source.localSpacePolicy;
	}

	static void CopyDirectionalRule(const ParsedLightOverlayDirectionalRule& source, ResolvedLightOverlayDirectionalRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasAngularSize = source.hasAngularSize;
		destination.angularSize = source.angularSize;
		destination.hasShadow = source.hasShadow;
		destination.shadow = source.shadow;
	}

	static void CopyMapLightRule(const ParsedLightOverlayMapLightRule& source, ResolvedLightOverlayMapLightRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.lightType = source.lightType;
		destination.anchorType = source.anchorType;
		destination.hasAnchorPosition = source.hasAnchorPosition;
		CopyVector3(source.anchorPosition, destination.anchorPosition);
		destination.anchorIndex = source.anchorIndex;
		destination.hasOffset = source.hasOffset;
		CopyVector3(source.offset, destination.offset);
		destination.hasDirection = source.hasDirection;
		CopyVector3(source.direction, destination.direction);
		destination.hasColor = source.hasColor;
		CopyVector3(source.color, destination.color);
		destination.hasIntensity = source.hasIntensity;
		destination.intensity = source.intensity;
		destination.hasRadius = source.hasRadius;
		destination.radius = source.radius;
		destination.hasRange = source.hasRange;
		destination.range = source.range;
		destination.hasFlicker = source.hasFlicker;
		destination.flickerFrames = source.flickerFrames;
	}

	static void CopyActorOverrideRule(const ParsedLightOverlayActorOverrideRule& source, ResolvedLightOverlayActorOverrideRule& destination)
	{
		destination.mapName = source.mapName;
		destination.id = source.id;
		destination.source = source.source;
		destination.actorClassName = source.actorClassName;
		destination.actorClass = PClass::FindActor(source.actorClassName);
		destination.actorClassResolved = destination.actorClass != nullptr;
		destination.receiverMode = source.receiverMode;
		destination.hasReceiverMode = source.hasReceiverMode;
	}

	static const ResolvedLightOverlaySet& ResolveLightOverlaysForMapInternal(const FString& mapName, bool currentMapAvailable)
	{
		const bool sameGeneration = GResolvedLightOverlaySet.parsedGeneration == GLightOverlayDatabase.generation;
		const bool sameMap = GResolvedLightOverlaySet.activeMapName.CompareNoCase(mapName) == 0;
		if (sameGeneration && sameMap && GResolvedLightOverlaySet.currentMapAvailable == currentMapAvailable)
		{
			return GResolvedLightOverlaySet;
		}

		ResolvedLightOverlaySet resolved;
		resolved.parsedGeneration = GLightOverlayDatabase.generation;
		resolved.resolvedGeneration = ++GResolvedLightOverlayGeneration;
		resolved.activeMapName = mapName;
		resolved.currentMapAvailable = currentMapAvailable;

		for (const auto& source : GLightOverlayDatabase.actorRules)
		{
			ResolvedLightOverlayActorRule destination;
			CopyActorRule(source, destination);
			resolved.actorRules.Push(destination);
		}

		for (const auto& source : GLightOverlayDatabase.directionalRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayDirectionalRule destination;
				CopyDirectionalRule(source, destination);
				resolved.directionalRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.mapLightRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayMapLightRule destination;
				CopyMapLightRule(source, destination);
				resolved.mapLightRules.Push(destination);
			}
		}

		for (const auto& source : GLightOverlayDatabase.actorOverrideRules)
		{
			if (mapName.IsNotEmpty() && source.mapName.CompareNoCase(mapName) == 0)
			{
				ResolvedLightOverlayActorOverrideRule destination;
				CopyActorOverrideRule(source, destination);
				resolved.actorOverrideRules.Push(destination);
			}
		}

		GResolvedLightOverlaySet = std::move(resolved);
		return GResolvedLightOverlaySet;
	}
}

const ParsedLightOverlayDatabase& GetParsedLightOverlayDatabase()
{
	return GLightOverlayDatabase;
}

const ResolvedLightOverlaySet& GetResolvedLightOverlaySet()
{
	return ResolveLightOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);
}

const ResolvedLightOverlaySet& ResolveLightOverlaysForMap(const char* mapName)
{
	const FString requestedMapName = CanonicalizeMapName(mapName);
	return ResolveLightOverlaysForMapInternal(requestedMapName, requestedMapName.IsNotEmpty());
}

bool ParseLightOverlays(bool verbose)
{
	ParsedLightOverlayDatabaseBuilder builder;

	int workingLump = -1;
	int lastLump = 0;
	static const char* lightOverlayNames[] = { "LIGHTOVR", nullptr };
	while ((workingLump = fileSystem.FindLumpMulti(lightOverlayNames, &lastLump)) != -1)
	{
		ParsedLightOverlaySourceFile sourceFile;
		sourceFile.lumpNum = workingLump;
		sourceFile.sourceName = GetLumpDisplayName(workingLump);
		builder.database.sourceFiles.Push(sourceFile);
		auto& storedSourceFile = builder.database.sourceFiles.Last();

		LightOverlayParser parser(workingLump, builder, storedSourceFile);
		parser.Parse();
		builder.database.hadParseErrors = builder.database.hadParseErrors || storedSourceFile.hadParseErrors;
	}

	builder.database.generation = ++GLightOverlayGeneration;
	GLightOverlayDatabase = std::move(builder.database);

	if (verbose)
	{
		Printf("LIGHTOVR: parsed generation=%u files=%d actor_rules=%d map_lights=%d directional=%d actor_overrides=%d parse_errors=%s\n",
			GLightOverlayDatabase.generation,
			GLightOverlayDatabase.sourceFiles.Size(),
			GLightOverlayDatabase.actorRules.Size(),
			GLightOverlayDatabase.mapLightRules.Size(),
			GLightOverlayDatabase.directionalRules.Size(),
			GLightOverlayDatabase.actorOverrideRules.Size(),
			GLightOverlayDatabase.hadParseErrors ? "yes" : "no");
	}

	ResolveLightOverlaysForMapInternal(GetCurrentResolvedMapName(), currentLevel != nullptr);

	return !GLightOverlayDatabase.hadParseErrors;
}

CCMD(lightoverlay_reload)
{
	const bool ok = ParseLightOverlays(true);
	Printf("LIGHTOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
}

CCMD(lightoverlay_dump)
{
	DumpParsedLightOverlayDatabase(GetParsedLightOverlayDatabase());
}

CCMD(lightoverlay_dumpresolved)
{
	if (argv.argc() > 2)
	{
		Printf("usage: lightoverlay_dumpresolved [mapname]\n");
		return;
	}

	const ResolvedLightOverlaySet& resolved = argv.argc() == 2 ?
		ResolveLightOverlaysForMap(argv[1]) :
		GetResolvedLightOverlaySet();
	DumpResolvedLightOverlaySet(resolved);
}
