#include "lightoverlay_smoke_editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

#include "c_cvars.h"
#include "cmdlib.h"
#include "d_gui.h"
#include "filesystem.h"
#include "gamestate.h"
#include "i_time.h"
#include "keydef.h"
#include "lightoverlay_editor.h"
#include "mapinfo.h"
#include "printf.h"
#include "v_video.h"

EXTERN_CVAR(Bool, nri_ptactorlighteditmode)
EXTERN_CVAR(Bool, nri_ptmaplighteditmode)
EXTERN_CVAR(Bool, nri_ptemissivelighteditmode)

CVAR(Bool, nri_ptmapsmokeeditmode, false, 0)

namespace
{
	static constexpr float DefaultSize = 32.0f;
	static constexpr float DefaultOffset = 1.0f;
	static constexpr float SizeStep = 4.0f;
	static constexpr float OffsetStep = 2.0f;
	static constexpr float RotationStep = 15.0f;
	static constexpr float IntervalStep = 0.025f;

	struct MapSmokeEmitterEditorState
	{
		bool enabled = false;
		bool draftActive = false;
		bool editingPersistedRule = false;
		uint32_t revision = 1;
		ParsedLightOverlayMapSmokeEmitterRule draft;
		FString selectedStyleId;
		FString selectedMapName;
		FString selectedRuleId;
		bool placePressed = false;
		bool selectPressed = false;
		bool reloadPressed = false;
		bool commitPressed = false;
		bool deletePressed = false;
	};

	static MapSmokeEmitterEditorState GMapSmokeEditor;

	static bool HasConflictingEditorMode()
	{
		return !!nri_ptactorlighteditmode || !!nri_ptmaplighteditmode || !!nri_ptemissivelighteditmode;
	}

	static bool IsActionKey(const event_t* ev, char key)
	{
		if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp))
		{
			return false;
		}
		const unsigned char ascii = static_cast<unsigned char>(ev->data2 & 0xff);
		return ascii != 0 && std::tolower(ascii) == key;
	}

	static bool CurrentMapName(FString& outMapName)
	{
		outMapName = "";
		if (currentLevel == nullptr || currentLevel->labelName.IsEmpty())
		{
			return false;
		}
		outMapName = currentLevel->labelName;
		return true;
	}

	static void ResetDraft()
	{
		GMapSmokeEditor.draftActive = false;
		GMapSmokeEditor.editingPersistedRule = false;
		GMapSmokeEditor.draft = {};
		GMapSmokeEditor.selectedMapName = "";
		GMapSmokeEditor.selectedRuleId = "";
		GMapSmokeEditor.revision++;
	}

	static void PrintDraft(const char* prefix)
	{
		if (!GMapSmokeEditor.draftActive)
		{
			return;
		}
		const auto& rule = GMapSmokeEditor.draft;
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG,
			"%s smokeemitter '%s': style=%s size=(%.1f,%.1f) rotation=%.1f height=%.1f count=%u interval=%.3f\n",
			prefix != nullptr ? prefix : "Draft",
			rule.id.GetChars(), rule.styleId.GetChars(), rule.size[0], rule.size[1],
			rule.rotation, rule.offset, rule.count, rule.intervalSeconds);
	}

	static bool SelectInitialStyle()
	{
		const auto& resolved = GetResolvedLightOverlaySet();
		if (resolved.smokeStyles.Size() == 0)
		{
			GMapSmokeEditor.selectedStyleId = "";
			return false;
		}
		for (const auto& style : resolved.smokeStyles)
		{
			if (style.id.CompareNoCase(GMapSmokeEditor.selectedStyleId) == 0)
			{
				return true;
			}
		}
		GMapSmokeEditor.selectedStyleId = resolved.smokeStyles[0].id;
		return true;
	}

	static bool CycleStyle(int direction)
	{
		const auto& resolved = GetResolvedLightOverlaySet();
		const int count = (int)resolved.smokeStyles.Size();
		if (count == 0)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No resolved smoke styles are available.\n");
			return true;
		}
		int current = -1;
		for (int i = 0; i < count; ++i)
		{
			if (resolved.smokeStyles[i].id.CompareNoCase(GMapSmokeEditor.selectedStyleId) == 0)
			{
				current = i;
				break;
			}
		}
		const int next = current < 0 ? 0 : (current + (direction >= 0 ? 1 : count - 1)) % count;
		GMapSmokeEditor.selectedStyleId = resolved.smokeStyles[next].id;
		if (GMapSmokeEditor.draftActive)
		{
			GMapSmokeEditor.draft.styleId = GMapSmokeEditor.selectedStyleId;
			GMapSmokeEditor.revision++;
			PrintDraft("Draft");
		}
		else
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke emitter style: %s\n", GMapSmokeEditor.selectedStyleId.GetChars());
		}
		return true;
	}

	static FString BuildUniqueRuleId(const ParsedLightOverlayDatabase& database, const FString& mapName)
	{
		auto exists = [&](const FString& id)
		{
			for (const auto& rule : database.mapSmokeEmitterRules)
			{
				if (rule.mapName.CompareNoCase(mapName) == 0 && rule.id.CompareNoCase(id) == 0)
				{
					return true;
				}
			}
			return false;
		};
		const FString base = "SmokeEmitter";
		if (!exists(base))
		{
			return base;
		}
		for (uint32_t suffix = 2; suffix < 1000000u; ++suffix)
		{
			const FString candidate = FStringf("SmokeEmitter_%u", suffix);
			if (!exists(candidate))
			{
				return candidate;
			}
		}
		return FStringf("SmokeEmitter_%u", (unsigned)I_msTime());
	}

	static bool BuildSurfacePlacement(float outPosition[3], float outNormal[3])
	{
		if (screen == nullptr)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke editor requires an active renderer.\n");
			return false;
		}
		PathTracingEmissiveLightEditTarget target = {};
		if (!screen->BuildPathTracingSurfaceLightEditTarget(target))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Cannot place smoke emitter: %s.\n",
				target.failureReason.IsEmpty() ? "no PT surface is aimed" : target.failureReason.GetChars());
			return false;
		}
		// PT render coordinates are { world X, -world Z, -world Y }.
		outPosition[0] = target.position[0];
		outPosition[1] = -target.position[2];
		outPosition[2] = -target.position[1];
		outNormal[0] = target.normal[0];
		outNormal[1] = -target.normal[2];
		outNormal[2] = -target.normal[1];
		const float length = std::sqrt(outNormal[0] * outNormal[0] + outNormal[1] * outNormal[1] + outNormal[2] * outNormal[2]);
		if (!std::isfinite(length) || length <= 1e-6f)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Cannot place smoke emitter: aimed surface has no finite normal.\n");
			return false;
		}
		outNormal[0] /= length;
		outNormal[1] /= length;
		outNormal[2] /= length;
		return true;
	}

	static void PlaceDraft(bool cloneActive)
	{
		FString mapName;
		if (!CurrentMapName(mapName))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke editor requires an active map.\n");
			return;
		}
		if (!SelectInitialStyle())
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke editor requires at least one resolved smokestyle.\n");
			return;
		}
		float position[3] = {};
		float normal[3] = {};
		if (!BuildSurfacePlacement(position, normal))
		{
			return;
		}

		ParsedLightOverlayMapSmokeEmitterRule rule = {};
		if (cloneActive && GMapSmokeEditor.draftActive)
		{
			rule = GMapSmokeEditor.draft;
		}
		else
		{
			rule.styleId = GMapSmokeEditor.selectedStyleId;
			rule.hasSize = true;
			rule.size[0] = DefaultSize;
			rule.size[1] = DefaultSize;
			rule.offset = DefaultOffset;
		}
		rule.mapName = mapName;
		rule.id = BuildUniqueRuleId(GetParsedLightOverlayDatabase(), mapName);
		rule.styleId = cloneActive && GMapSmokeEditor.draftActive ? rule.styleId : GMapSmokeEditor.selectedStyleId;
		rule.hasPosition = true;
		std::copy(position, position + 3, rule.position);
		rule.hasNormal = true;
		std::copy(normal, normal + 3, rule.normal);
		rule.hasSize = true;
		rule.size[0] = std::clamp(rule.size[0], 1.0f, 256.0f);
		rule.size[1] = std::clamp(rule.size[1], 1.0f, 256.0f);

		GMapSmokeEditor.draft = rule;
		GMapSmokeEditor.draftActive = true;
		GMapSmokeEditor.editingPersistedRule = false;
		GMapSmokeEditor.selectedMapName = mapName;
		GMapSmokeEditor.selectedRuleId = rule.id;
		GMapSmokeEditor.selectedStyleId = rule.styleId;
		GMapSmokeEditor.revision++;
		PrintDraft(cloneActive ? "Cloned draft" : "Placed draft");
	}

	static bool SelectNextPersistedRule()
	{
		FString mapName;
		if (!CurrentMapName(mapName))
		{
			return true;
		}
		const auto& database = GetParsedLightOverlayDatabase();
		TArray<int> matches;
		for (unsigned i = 0; i < (unsigned)database.mapSmokeEmitterRules.Size(); ++i)
		{
			if (database.mapSmokeEmitterRules[i].mapName.CompareNoCase(mapName) == 0)
			{
				matches.Push((int)i);
			}
		}
		if (matches.Size() == 0)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No persisted smokeemitters exist for %s.\n", mapName.GetChars());
			return true;
		}
		int selected = -1;
		for (unsigned i = 0; i < (unsigned)matches.Size(); ++i)
		{
			const auto& candidate = database.mapSmokeEmitterRules[matches[i]];
			if (candidate.id.CompareNoCase(GMapSmokeEditor.selectedRuleId) == 0)
			{
				selected = (int)i;
				break;
			}
		}
		const int next = (selected + 1) % (int)matches.Size();
		GMapSmokeEditor.draft = database.mapSmokeEmitterRules[matches[next]];
		GMapSmokeEditor.draftActive = true;
		GMapSmokeEditor.editingPersistedRule = true;
		GMapSmokeEditor.selectedMapName = mapName;
		GMapSmokeEditor.selectedRuleId = GMapSmokeEditor.draft.id;
		GMapSmokeEditor.selectedStyleId = GMapSmokeEditor.draft.styleId;
		GMapSmokeEditor.revision++;
		PrintDraft("Selected");
		return true;
	}

	static bool WriteDatabase(const FString& path, const ParsedLightOverlayDatabase& database)
	{
		const FString serialized = SerializeLightOverlayDatabase(database);
		std::unique_ptr<FileWriter> file(FileWriter::Open(path.GetChars()));
		if (file == nullptr)
		{
			return false;
		}
		const size_t byteCount = serialized.Len();
		const size_t written = file->Write(serialized.GetChars(), byteCount);
		file->Close();
		return written == byteCount;
	}

	static bool PrepareWritableDatabase(ParsedLightOverlayDatabase& outDatabase, FString& outPath, int& outLumpNum)
	{
		if (!ParseLightOverlays(true))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke editor refused to write because LIGHTOVR has parse errors.\n");
			return false;
		}
		if (!ActorLightEditorResolveWritableSource(outPath, &outLumpNum))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke editor requires a loose writable LIGHTOVR source.\n");
			return false;
		}
		outDatabase = GetParsedLightOverlayDatabase();
		return true;
	}

	static bool CommitDraft()
	{
		if (!GMapSmokeEditor.draftActive)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No map smoke emitter draft is active.\n");
			return true;
		}
		ParsedLightOverlayDatabase database;
		FString path;
		int lumpNum = -1;
		if (!PrepareWritableDatabase(database, path, lumpNum))
		{
			return true;
		}
		auto rule = GMapSmokeEditor.draft;
		rule.source.lumpNum = lumpNum;
		rule.source.sourceName = path;
		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);
		if (!WriteDatabase(path, database))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Failed to write map smoke emitter to %s.\n", path.GetChars());
			return true;
		}
		Printf("NRI PT map smoke editor: %s smokeemitter '%s' for map '%s' in %s.\n",
			replaced ? "updated" : "created", rule.id.GetChars(), rule.mapName.GetChars(), path.GetChars());
		ResetDraft();
		ParseLightOverlays(true);
		return true;
	}

	static bool DeleteSelection()
	{
		if (!GMapSmokeEditor.draftActive)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "No map smoke emitter is selected.\n");
			return true;
		}
		if (!GMapSmokeEditor.editingPersistedRule)
		{
			ResetDraft();
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Discarded uncommitted smoke emitter draft.\n");
			return true;
		}
		ParsedLightOverlayDatabase database;
		FString path;
		int lumpNum = -1;
		if (!PrepareWritableDatabase(database, path, lumpNum))
		{
			return true;
		}
		const FString mapName = GMapSmokeEditor.draft.mapName;
		const FString ruleId = GMapSmokeEditor.draft.id;
		if (GMapSmokeEditor.draft.source.lumpNum != lumpNum)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG,
				"Cannot remove smokeemitter '%s': its source is not the writable loose LIGHTOVR. Add a loose override or edit the owning source instead.\n",
				ruleId.GetChars());
			return true;
		}
		if (!RemoveLightOverlayRule(database, LightOverlayRuleKind::MapSmokeEmitter, ruleId.GetChars(), mapName.GetChars()))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke emitter '%s' no longer exists.\n", ruleId.GetChars());
			ResetDraft();
			return true;
		}
		if (!WriteDatabase(path, database))
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Failed to remove map smoke emitter from %s.\n", path.GetChars());
			return true;
		}
		ResetDraft();
		ParseLightOverlays(true);
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Removed smokeemitter '%s' from %s.\n", ruleId.GetChars(), mapName.GetChars());
		return true;
	}

	static bool ModifyDraft(int action, float delta)
	{
		if (!GMapSmokeEditor.draftActive)
		{
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Place or select a map smoke emitter first.\n");
			return true;
		}
		auto& rule = GMapSmokeEditor.draft;
		switch (action)
		{
		case 0: rule.size[0] = std::clamp(rule.size[0] + delta, 1.0f, 256.0f); break;
		case 1: rule.size[1] = std::clamp(rule.size[1] + delta, 1.0f, 256.0f); break;
		case 2:
			rule.rotation = std::fmod(rule.rotation + delta, 360.0f);
			if (rule.rotation < 0.0f) rule.rotation += 360.0f;
			break;
		case 3: rule.offset += delta; break;
		case 4: rule.count = (uint32_t)std::clamp((int)rule.count + (int)delta, 1, 256); break;
		case 5: rule.intervalSeconds = std::max(0.001f, rule.intervalSeconds + delta); break;
		default: return false;
		}
		GMapSmokeEditor.revision++;
		PrintDraft("Draft");
		return true;
	}
}

bool IsMapSmokeEmitterEditorEnabled()
{
	return !!nri_ptmapsmokeeditmode;
}

void TickMapSmokeEmitterEditor()
{
	const bool requested = IsMapSmokeEmitterEditorEnabled();
	if (!requested)
	{
		if (GMapSmokeEditor.enabled)
		{
			GMapSmokeEditor = {};
		}
		return;
	}
	if (HasConflictingEditorMode())
	{
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Map smoke edit mode cannot overlap another LIGHTOVR editor mode.\n");
		nri_ptmapsmokeeditmode = false;
		GMapSmokeEditor = {};
		return;
	}
	if (!GMapSmokeEditor.enabled)
	{
		GMapSmokeEditor.enabled = true;
		SelectInitialStyle();
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG,
			"Map smoke edit mode enabled. p place, arrows size, ,/. height, ;/' count, j/k interval, [/] style, Enter commit, Escape cancel, l reload.\n");
	}
}

bool MapSmokeEmitterEditorResponder(event_t* ev)
{
	if (!IsMapSmokeEmitterEditorEnabled() || ev == nullptr)
	{
		return false;
	}
	if (HasConflictingEditorMode())
	{
		return false;
	}
	const bool down = ev->type == EV_KeyDown;
	const bool up = ev->type == EV_KeyUp;
	if (!down && !up)
	{
		return false;
	}

	if (ev->data1 == KEY_ESCAPE && down && GMapSmokeEditor.draftActive)
	{
		ResetDraft();
		Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "Cancelled map smoke emitter draft.\n");
		return true;
	}
	if (ev->data1 == KEY_ENTER)
	{
		if (down && !GMapSmokeEditor.commitPressed)
		{
			GMapSmokeEditor.commitPressed = true;
			CommitDraft();
		}
		if (up) GMapSmokeEditor.commitPressed = false;
		return true;
	}
	if (ev->data1 == KEY_DEL)
	{
		if (down && !GMapSmokeEditor.deletePressed)
		{
			GMapSmokeEditor.deletePressed = true;
			DeleteSelection();
		}
		if (up) GMapSmokeEditor.deletePressed = false;
		return true;
	}

	if (down)
	{
		switch (ev->data1)
		{
		case KEY_LEFTARROW: return ModifyDraft(0, -SizeStep);
		case KEY_RIGHTARROW: return ModifyDraft(0, SizeStep);
		case KEY_UPARROW: return ModifyDraft(1, SizeStep);
		case KEY_DOWNARROW: return ModifyDraft(1, -SizeStep);
		case KEY_MWHEELUP: return ModifyDraft(2, RotationStep);
		case KEY_MWHEELDOWN: return ModifyDraft(2, -RotationStep);
		default: break;
		}
		if (IsActionKey(ev, ',') || IsActionKey(ev, '<')) return ModifyDraft(3, -OffsetStep);
		if (IsActionKey(ev, '.') || IsActionKey(ev, '>')) return ModifyDraft(3, OffsetStep);
		if (IsActionKey(ev, ';')) return ModifyDraft(4, -1.0f);
		if (IsActionKey(ev, '\'')) return ModifyDraft(4, 1.0f);
		if (IsActionKey(ev, 'j')) return ModifyDraft(5, -IntervalStep);
		if (IsActionKey(ev, 'k')) return ModifyDraft(5, IntervalStep);
		if (IsActionKey(ev, '[')) return CycleStyle(-1);
		if (IsActionKey(ev, ']')) return CycleStyle(1);
	}

	if (IsActionKey(ev, 'p'))
	{
		if (down && !GMapSmokeEditor.placePressed)
		{
			GMapSmokeEditor.placePressed = true;
			PlaceDraft((ev->data3 & GKM_CTRL) != 0);
		}
		if (up) GMapSmokeEditor.placePressed = false;
		return true;
	}
	if (IsActionKey(ev, 'o'))
	{
		if (down && !GMapSmokeEditor.selectPressed)
		{
			GMapSmokeEditor.selectPressed = true;
			SelectNextPersistedRule();
		}
		if (up) GMapSmokeEditor.selectPressed = false;
		return true;
	}
	if (IsActionKey(ev, 'l'))
	{
		if (down && !GMapSmokeEditor.reloadPressed)
		{
			GMapSmokeEditor.reloadPressed = true;
			ResetDraft();
			const bool ok = ParseLightOverlays(true);
			SelectInitialStyle();
			Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "LIGHTOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
		}
		if (up) GMapSmokeEditor.reloadPressed = false;
		return true;
	}

	// Consume releases for all adjustment keys so editor actions cannot leak
	// into gameplay bindings while this mode owns input.
	if (up && (ev->data1 == KEY_LEFTARROW || ev->data1 == KEY_RIGHTARROW ||
		ev->data1 == KEY_UPARROW || ev->data1 == KEY_DOWNARROW ||
		ev->data1 == KEY_MWHEELUP || ev->data1 == KEY_MWHEELDOWN ||
		IsActionKey(ev, ',') || IsActionKey(ev, '<') || IsActionKey(ev, '.') || IsActionKey(ev, '>') || IsActionKey(ev, ';') ||
		IsActionKey(ev, '\'') || IsActionKey(ev, 'j') || IsActionKey(ev, 'k') ||
		IsActionKey(ev, '[') || IsActionKey(ev, ']')))
	{
		return true;
	}
	return false;
}

bool GetMapSmokeEmitterEditorRuntimePreview(MapSmokeEmitterEditorRuntimePreview& outPreview)
{
	outPreview = {};
	if (!IsMapSmokeEmitterEditorEnabled() || !GMapSmokeEditor.draftActive)
	{
		return false;
	}
	outPreview.active = true;
	outPreview.suppressPersistedRule = GMapSmokeEditor.editingPersistedRule;
	outPreview.revision = GMapSmokeEditor.revision;
	outPreview.rule = GMapSmokeEditor.draft;
	return true;
}
