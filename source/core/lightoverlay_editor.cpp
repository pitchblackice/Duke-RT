#include <cctype>

#include "lightoverlay_editor.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "d_net.h"
#include "gamefuncs.h"
#include "gamestate.h"
#include "i_time.h"
#include "printf.h"
#include "v_video.h"

CVAR(Bool, nri_ptactorlighteditmode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	static ActorLightEditorState GActorLightEditorState;
	static constexpr uint64_t ActorLightEditorNotifyRepeatMs = 750;
	static constexpr uint64_t ActorLightEditorNotifyClearGraceMs = 250;

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
