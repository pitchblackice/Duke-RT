#include "lightoverlay_editor.h"

#include "c_cvars.h"
#include "c_dispatch.h"
#include "coreactor.h"
#include "coreplayer.h"
#include "d_net.h"
#include "gamefuncs.h"
#include "gamestate.h"
#include "printf.h"

CVAR(Bool, nri_ptactorlighteditmode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	static ActorLightEditorState GActorLightEditorState;

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
}

bool ActorLightEditorResponder(event_t* ev)
{
	if (!IsActorLightEditorEnabled() || ev == nullptr)
	{
		return false;
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
