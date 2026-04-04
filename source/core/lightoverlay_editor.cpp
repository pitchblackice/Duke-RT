#include "lightoverlay_editor.h"

#include "c_cvars.h"

CVAR(Bool, nri_ptactorlighteditmode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	static ActorLightEditorState GActorLightEditorState;
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
	return false;
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
