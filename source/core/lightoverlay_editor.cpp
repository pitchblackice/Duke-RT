#include <cctype>
#include <memory>

#include "lightoverlay_editor.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "d_net.h"
#include "filesystem.h"
#include "gamefuncs.h"
#include "gamestate.h"
#include "i_time.h"
#include "lightoverlay.h"
#include "printf.h"
#include "v_video.h"

CVAR(Bool, nri_ptactorlighteditmode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	static ActorLightEditorState GActorLightEditorState;
	static constexpr uint64_t ActorLightEditorNotifyRepeatMs = 750;
	static constexpr uint64_t ActorLightEditorNotifyClearGraceMs = 250;

	enum class ActorLightEditorWritableSourceKind : uint8_t
	{
		None,
		Directory,
		StandaloneFile,
		ReadOnlyContainer,
	};

	static const char* GetActorLightEditorClassName(const DCoreActor* actor)
	{
		if (actor == nullptr || actor->GetClass() == nullptr)
		{
			return "(null)";
		}
		return actor->GetClass()->TypeName.GetChars();
	}

	static bool GetActorLightEditorSamplingContext(DCorePlayer*& outPlayer, DCoreActor*& outActor, DVector3& outOrigin, DRotator& outViewRotation, sectortype*& outStartSector)
	{
		outPlayer = nullptr;
		outActor = nullptr;
		outOrigin = {};
		outViewRotation = {};
		outStartSector = nullptr;

		if (gamestate != GS_LEVEL || myconnectindex < 0 || myconnectindex >= MAXPLAYERS)
		{
			return false;
		}

		outPlayer = PlayerArray[myconnectindex];
		if (outPlayer == nullptr)
		{
			return false;
		}

		outActor = outPlayer->GetActor();
		if (outActor == nullptr || !outActor->exists() || (outActor->ObjectFlags & OF_EuthanizeMe) != 0)
		{
			return false;
		}

		outOrigin = outPlayer->CameraPos;
		outStartSector = outActor->sector();
		updatesector(outOrigin, &outStartSector);
		if (outStartSector == nullptr)
		{
			outOrigin = outActor->getPosWithOffsetZ();
			outStartSector = outActor->sector();
			updatesector(outOrigin, &outStartSector);
			if (outStartSector == nullptr)
			{
				return false;
			}
		}

		outViewRotation = DRotator(
			outPlayer->getPitchWithView(),
			outActor->spr.Angles.Yaw + outPlayer->ViewAngles.Yaw,
			outActor->spr.Angles.Roll + outPlayer->ViewAngles.Roll);
		return true;
	}

	static void PrintActorLightEditorTarget(const ActorLightEditorTarget& target)
	{
		switch (target.kind)
		{
		case ActorLightEditorTargetKind::Actor:
			Printf(
				"NRI PT actor light editor target: kind=actor actor=%d class=%s sector=%d hitpos=(%.2f, %.2f, %.2f)\n",
				target.actorIndex,
				target.actorClassName.GetChars(),
				target.hitSector != nullptr ? sectindex(target.hitSector) : -1,
				target.hitX,
				target.hitY,
				target.hitZ);
			break;

		case ActorLightEditorTargetKind::Surface:
			Printf(
				"NRI PT actor light editor target: kind=surface sector=%d wall=%d hitpos=(%.2f, %.2f, %.2f)\n",
				target.hitSector != nullptr ? sectindex(target.hitSector) : -1,
				target.hitWall != nullptr ? wallindex(target.hitWall) : -1,
				target.hitX,
				target.hitY,
				target.hitZ);
			break;

		default:
			Printf("NRI PT actor light editor target: kind=none\n");
			break;
		}
	}

	static ActorLightEditorWritableSourceKind ClassifyActorLightEditorWritableSource(int lumpNum, FString& outPath)
	{
		outPath = "";
		if (lumpNum < 0)
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		const int container = fileSystem.GetFileContainer(lumpNum);
		const char* entryName = fileSystem.GetFileFullName(lumpNum);
		const char* containerPath = fileSystem.GetResourceFileFullName(container);
		if (container < 0 || entryName == nullptr || entryName[0] == 0 || containerPath == nullptr || containerPath[0] == 0)
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		FString normalizedContainerPath = containerPath;
		FixPathSeperator(normalizedContainerPath);

		if (DirExists(normalizedContainerPath.GetChars()))
		{
			outPath = normalizedContainerPath;
			if (outPath.IsEmpty() || outPath.Back() != '/')
			{
				outPath << '/';
			}
			outPath << entryName;
			FixPathSeperator(outPath);
			return ActorLightEditorWritableSourceKind::Directory;
		}

		if (!FileExists(normalizedContainerPath.GetChars()))
		{
			return ActorLightEditorWritableSourceKind::None;
		}

		const FString resourceBase = ExtractFileBase(normalizedContainerPath.GetChars(), true);
		const FString entryBase = ExtractFileBase(entryName, true);
		if (fileSystem.GetEntryCount(container) == 1 &&
			resourceBase.CompareNoCase("LIGHTOVR") == 0 &&
			entryBase.CompareNoCase("LIGHTOVR") == 0)
		{
			outPath = normalizedContainerPath;
			return ActorLightEditorWritableSourceKind::StandaloneFile;
		}

		return ActorLightEditorWritableSourceKind::ReadOnlyContainer;
	}

	static void PrintActorLightEditorWritableSourceFailure()
	{
		const auto& database = GetParsedLightOverlayDatabase();
		if (database.sourceFiles.Size() == 0)
		{
			Printf("NRI PT actor light editor: no mounted LIGHTOVR source is available.\n");
			return;
		}

		bool sawMountedSource = false;
		bool sawReadOnlySource = false;
		for (int i = database.sourceFiles.Size() - 1; i >= 0; --i)
		{
			const auto& sourceFile = database.sourceFiles[i];
			if (sourceFile.lumpNum < 0)
			{
				continue;
			}

			sawMountedSource = true;
			FString ignoredPath;
			const auto kind = ClassifyActorLightEditorWritableSource(sourceFile.lumpNum, ignoredPath);
			if (kind == ActorLightEditorWritableSourceKind::ReadOnlyContainer)
			{
				sawReadOnlySource = true;
				break;
			}
		}

		if (sawReadOnlySource)
		{
			Printf("NRI PT actor light editor: no writable mounted LIGHTOVR source is available; archive-backed sources are read-only.\n");
		}
		else if (sawMountedSource)
		{
			Printf("NRI PT actor light editor: no writable mounted LIGHTOVR source could be resolved from the current overlay metadata.\n");
		}
		else
		{
			Printf("NRI PT actor light editor: no mounted LIGHTOVR source is available.\n");
		}
	}

	static FString FindActorLightEditorSourceNameForLump(const ParsedLightOverlayDatabase& database, int lumpNum)
	{
		for (const auto& sourceFile : database.sourceFiles)
		{
			if (sourceFile.lumpNum == lumpNum)
			{
				return sourceFile.sourceName;
			}
		}

		const char* fullName = lumpNum >= 0 ? fileSystem.GetFileFullName(lumpNum) : nullptr;
		return fullName != nullptr ? FString(fullName) : FString("LIGHTOVR");
	}

	static bool HasActorLightEditorRuleId(const ParsedLightOverlayDatabase& database, const FString& id)
	{
		for (const auto& rule : database.actorRules)
		{
			if (rule.id.CompareNoCase(id) == 0)
			{
				return true;
			}
		}

		return false;
	}

	static FString BuildActorLightEditorUniqueRuleId(const ParsedLightOverlayDatabase& database, const FString& actorClassName)
	{
		FString baseId = actorClassName;
		if (!HasActorLightEditorRuleId(database, baseId))
		{
			return baseId;
		}

		for (int suffix = 2; suffix < 1000000; ++suffix)
		{
			FString candidate = FStringf("%s_%d", baseId.GetChars(), suffix);
			if (!HasActorLightEditorRuleId(database, candidate))
			{
				return candidate;
			}
		}

		return FStringf("%s_%u", baseId.GetChars(), (unsigned)I_msTime());
	}

	static bool WriteActorLightEditorTextFile(const FString& path, const FString& text)
	{
		std::unique_ptr<FileWriter> file(FileWriter::Open(path.GetChars()));
		if (file == nullptr)
		{
			return false;
		}

		file->Write(text.GetChars(), text.Len());
		return true;
	}

	static bool ReloadActorLightEditorOverlays()
	{
		const bool ok = ParseLightOverlays(true);
		Printf("LIGHTOVR reload %s.\n", ok ? "completed" : "completed with parse errors");
		return ok;
	}

	static void PrintActorLightEditorActorData(const ActorLightEditorTarget& target)
	{
		const auto* actor = target.actor;
		if (actor == nullptr)
		{
			Printf("NRI PT actor light editor: sampled actor target is no longer valid.\n");
			return;
		}

		Printf(
			"NRI PT actor light editor: actor=%d class=%s stat=%d sector=%d type=%d pal=%d shade=%d cstat=0x%x cstat2=0x%x pos=(%.2f, %.2f, %.2f) hitpos=(%.2f, %.2f, %.2f)\n",
			actor->GetIndex(),
			target.actorClassName.GetChars(),
			(int)actor->spr.statnum,
			actor->sectno(),
			(int)actor->spr.type,
			(int)actor->spr.pal,
			(int)actor->spr.shade,
			(unsigned int)actor->spr.cstat,
			(unsigned int)actor->spr.cstat2,
			actor->spr.pos.X,
			actor->spr.pos.Y,
			actor->spr.pos.Z,
			target.hitX,
			target.hitY,
			target.hitZ);

		Printf(
			"NRI PT actor light editor: lotag=%d hitag=%d extra=%d detail=%d owner=%d clipdist=%.2f clipdist_map=%u blend=%u pal=%u scale=(%.3f, %.3f) offset=(%d, %d) angles=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f, %.2f) object_flags=0x%x\n",
			(int)actor->spr.lotag,
			(int)actor->spr.hitag,
			(int)actor->spr.extra,
			(int)actor->spr.detail,
			(int)actor->spr.intowner,
			actor->clipdist,
			(unsigned int)actor->spr.clipdist,
			(unsigned int)actor->spr.blend,
			(unsigned int)actor->spr.pal,
			actor->spr.scale.X,
			actor->spr.scale.Y,
			(int)actor->spr.xoffset,
			(int)actor->spr.yoffset,
			actor->spr.Angles.Yaw.Degrees(),
			actor->spr.Angles.Pitch.Degrees(),
			actor->spr.Angles.Roll.Degrees(),
			actor->vel.X,
			actor->vel.Y,
			actor->vel.Z,
			(unsigned int)actor->ObjectFlags);
	}

	static void PrintActorLightEditorSurfaceData()
	{
		if (screen == nullptr)
		{
			Printf("NRI PT actor light editor: surface probe status is unavailable because no screen backend is active.\n");
			return;
		}

		screen->PrintPathTracingSurfaceProbeStatus();
	}

	static bool IsActorLightEditorActionKey(const event_t* ev, char key)
	{
		if (ev == nullptr || (ev->type != EV_KeyDown && ev->type != EV_KeyUp))
		{
			return false;
		}

		const unsigned char ascii = static_cast<unsigned char>(ev->data2 & 0xff);
		return ascii != 0 && std::tolower(ascii) == key;
	}

	static void PerformActorLightEditorPrintAction()
	{
		ActorLightEditorTarget target;
		if (!ActorLightEditorSampleTarget(target))
		{
			Printf("NRI PT actor light editor: no local gameplay sampling context is available.\n");
			return;
		}

		GActorLightEditorState.currentTarget = target;

		switch (target.kind)
		{
		case ActorLightEditorTargetKind::Actor:
			PrintActorLightEditorActorData(target);
			break;

		case ActorLightEditorTargetKind::Surface:
			PrintActorLightEditorSurfaceData();
			break;

		default:
			PrintActorLightEditorTarget(target);
			break;
		}
	}

	static void PerformActorLightEditorCreateRuleAction()
	{
		ActorLightEditorTarget target;
		if (!ActorLightEditorSampleTarget(target))
		{
			Printf("NRI PT actor light editor: no local gameplay sampling context is available.\n");
			return;
		}

		GActorLightEditorState.currentTarget = target;
		if (target.kind != ActorLightEditorTargetKind::Actor || target.actor == nullptr || target.actorClassName.IsEmpty())
		{
			Printf("NRI PT actor light editor: placeholder rule creation requires an actor target.\n");
			return;
		}

		FString writablePath;
		int writableLumpNum = -1;
		if (!ActorLightEditorResolveWritableSource(writablePath, &writableLumpNum))
		{
			PrintActorLightEditorWritableSourceFailure();
			return;
		}

		ParsedLightOverlayDatabase database = GetParsedLightOverlayDatabase();
		ParsedLightOverlayActorRule rule = {};
		rule.id = BuildActorLightEditorUniqueRuleId(database, target.actorClassName);
		rule.actorClassName = target.actorClassName;
		rule.lightType = "point";
		rule.hasColor = true;
		rule.color[0] = 1.0f;
		rule.color[1] = 0.9f;
		rule.color[2] = 0.7f;
		rule.hasIntensity = true;
		rule.intensity = 8.0f;
		rule.hasRadius = true;
		rule.radius = 96.0f;
		rule.hasOffset = true;
		rule.offset[0] = 0.0f;
		rule.offset[1] = 0.0f;
		rule.offset[2] = 0.0f;
		rule.source.lumpNum = writableLumpNum;
		rule.source.sourceName = FindActorLightEditorSourceNameForLump(database, writableLumpNum);

		bool replaced = false;
		AddOrReplaceLightOverlayRule(database, rule, &replaced);

		const FString serialized = SerializeLightOverlayDatabase(database);
		if (!WriteActorLightEditorTextFile(writablePath, serialized))
		{
			Printf("NRI PT actor light editor: failed to open writable LIGHTOVR '%s'.\n", writablePath.GetChars());
			return;
		}

		Printf(
			"NRI PT actor light editor: created placeholder actorrule '%s' for class '%s' and wrote %d bytes to %s.\n",
			rule.id.GetChars(),
			rule.actorClassName.GetChars(),
			serialized.Len(),
			writablePath.GetChars());

		ReloadActorLightEditorOverlays();
	}

	static void UpdateActorLightEditorNotify()
	{
		const uint64_t nowMs = I_msTime();
		const auto& target = GActorLightEditorState.currentTarget;

		if (target.kind == ActorLightEditorTargetKind::Actor && !target.actorClassName.IsEmpty())
		{
			GActorLightEditorState.lastActorSeenTimeMs = nowMs;

			const bool classChanged = GActorLightEditorState.lastNotifyActorClassName != target.actorClassName;
			const bool cooldownExpired =
				GActorLightEditorState.lastNotifyTimeMs == 0 ||
				(nowMs - GActorLightEditorState.lastNotifyTimeMs) >= ActorLightEditorNotifyRepeatMs;

			if (classChanged || cooldownExpired)
			{
				Printf(PRINT_LOW | PRINT_NOTIFY | PRINT_NOLOG, "%s\n", target.actorClassName.GetChars());
				GActorLightEditorState.lastNotifyActorClassName = target.actorClassName;
				GActorLightEditorState.lastNotifyTimeMs = nowMs;
			}

			return;
		}

		if (!GActorLightEditorState.lastNotifyActorClassName.IsEmpty() &&
			GActorLightEditorState.lastActorSeenTimeMs != 0 &&
			(nowMs - GActorLightEditorState.lastActorSeenTimeMs) >= ActorLightEditorNotifyClearGraceMs)
		{
			GActorLightEditorState.lastNotifyActorClassName = "";
			GActorLightEditorState.lastNotifyTimeMs = 0;
			GActorLightEditorState.lastActorSeenTimeMs = 0;
		}
	}
}

bool IsActorLightEditorEnabled()
{
	return !!nri_ptactorlighteditmode;
}

void ResetActorLightEditorState()
{
	const bool enabled = IsActorLightEditorEnabled();
	GActorLightEditorState = {};
	GActorLightEditorState.enabled = enabled;
}

const ActorLightEditorState& GetActorLightEditorState()
{
	return GActorLightEditorState;
}

void TickActorLightEditor()
{
	const bool enabled = IsActorLightEditorEnabled();
	if (!enabled)
	{
		if (GActorLightEditorState.enabled)
		{
			ResetActorLightEditorState();
		}
		return;
	}

	if (!GActorLightEditorState.enabled)
	{
		ResetActorLightEditorState();
	}

	ActorLightEditorSampleTarget(GActorLightEditorState.currentTarget);
	UpdateActorLightEditorNotify();
}

bool ActorLightEditorResponder(event_t* ev)
{
	if (!IsActorLightEditorEnabled() || ev == nullptr)
	{
		return false;
	}

	if (IsActorLightEditorActionKey(ev, 'p'))
	{
		if (ev->type == EV_KeyDown)
		{
			if (!GActorLightEditorState.printActionPressed)
			{
				GActorLightEditorState.printActionPressed = true;
				PerformActorLightEditorPrintAction();
			}
		}
		else
		{
			GActorLightEditorState.printActionPressed = false;
		}

		return true;
	}

	if (IsActorLightEditorActionKey(ev, 'o'))
	{
		if (ev->type == EV_KeyDown)
		{
			if (!GActorLightEditorState.createRuleActionPressed)
			{
				GActorLightEditorState.createRuleActionPressed = true;
				PerformActorLightEditorCreateRuleAction();
			}
		}
		else
		{
			GActorLightEditorState.createRuleActionPressed = false;
		}

		return true;
	}

	return false;
}

bool ActorLightEditorSampleTarget(ActorLightEditorTarget& outTarget)
{
	outTarget.Clear();

	DCorePlayer* player = nullptr;
	DCoreActor* actor = nullptr;
	DVector3 origin;
	DRotator viewRotation;
	sectortype* startSector = nullptr;
	if (!GetActorLightEditorSamplingContext(player, actor, origin, viewRotation, startSector))
	{
		return false;
	}

	HitInfoBase hit;
	hitscan(origin, startSector, DVector3(viewRotation), hit, CLIPMASK1, 65536.0);
	outTarget.hitSector = hit.hitSector;
	outTarget.hitWall = hit.hitWall;
	outTarget.hitX = hit.hitpos.X;
	outTarget.hitY = hit.hitpos.Y;
	outTarget.hitZ = hit.hitpos.Z;

	if (hit.hitActor != nullptr &&
		hit.hitActor->exists() &&
		(hit.hitActor->ObjectFlags & OF_EuthanizeMe) == 0)
	{
		outTarget.kind = ActorLightEditorTargetKind::Actor;
		outTarget.actor = hit.hitActor;
		outTarget.actorIndex = hit.hitActor->GetIndex();
		outTarget.actorClassName = GetActorLightEditorClassName(hit.hitActor);
	}
	else if (hit.hitWall != nullptr || hit.hitSector != nullptr)
	{
		outTarget.kind = ActorLightEditorTargetKind::Surface;
	}

	return true;
}

bool ActorLightEditorResolveWritableSource(FString& outPath, int* outLumpNum)
{
	outPath = "";
	if (outLumpNum != nullptr)
	{
		*outLumpNum = -1;
	}

	const auto& database = GetParsedLightOverlayDatabase();
	for (int i = database.sourceFiles.Size() - 1; i >= 0; --i)
	{
		const auto& sourceFile = database.sourceFiles[i];
		if (sourceFile.lumpNum < 0)
		{
			continue;
		}

		FString candidatePath;
		const auto kind = ClassifyActorLightEditorWritableSource(sourceFile.lumpNum, candidatePath);
		if (kind != ActorLightEditorWritableSourceKind::Directory &&
			kind != ActorLightEditorWritableSourceKind::StandaloneFile)
		{
			continue;
		}

		outPath = candidatePath;
		if (outLumpNum != nullptr)
		{
			*outLumpNum = sourceFile.lumpNum;
		}
		GActorLightEditorState.writableLightOvrPath = candidatePath;
		GActorLightEditorState.writableLightOvrLumpNum = sourceFile.lumpNum;
		return true;
	}

	GActorLightEditorState.writableLightOvrPath = "";
	GActorLightEditorState.writableLightOvrLumpNum = -1;
	return false;
}

CCMD(nri_ptactorlightedittarget)
{
	ActorLightEditorTarget target;
	if (!ActorLightEditorSampleTarget(target))
	{
		Printf("nri_ptactorlightedittarget: no local gameplay sampling context is available.\n");
		return;
	}

	PrintActorLightEditorTarget(target);
}

CCMD(nri_ptactorlighteditwritable)
{
	FString path;
	int lumpNum = -1;
	if (!ActorLightEditorResolveWritableSource(path, &lumpNum))
	{
		PrintActorLightEditorWritableSourceFailure();
		return;
	}

	Printf(
		"NRI PT actor light editor writable LIGHTOVR: path=%s lump=%d source=%s\n",
		path.GetChars(),
		lumpNum,
		lumpNum >= 0 ? fileSystem.GetFileFullPath(lumpNum).c_str() : "(none)");
}
