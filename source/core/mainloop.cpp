/*
** mainloop.cpp
** Implements the main game loop
**
**---------------------------------------------------------------------------
** Copyright 2020 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/


// For TryRunTics the following applies:
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright 1999-2016 Randy Heit
// Copyright 2002-2020 Christoph Oelckers
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//		DOOM Network game communication and protocol,
//		all OS independent parts.
//
//-----------------------------------------------------------------------------


#include <chrono>
#include <thread>
#include "c_cvars.h"
#include "i_time.h"
#include "d_net.h"
#include "gamecontrol.h"
#include "c_console.h"
#include "razemenu.h"
#include "i_system.h"
#include "raze_sound.h"
#include "raze_music.h"
#include "vm.h"
#include "gamestate.h"
#include "screenjob_.h"
#include "c_console.h"
#include "uiinput.h"
#include "v_video.h"
#include "palette.h"
#include "build.h"
#include "mapinfo.h"
#include "automap.h"
#include "statusbar.h"
#include "gamestruct.h"
#include "savegamehelp.h"
#include "v_draw.h"
#include "gamehud.h"
#include "wipe.h"
#include "i_interface.h"
#include "texinfo.h"
#include "texturemanager.h"
#include "gameinput.h"
#include "d_eventbase.h"
#include "hw_clock.h"

CVAR(Bool, vid_activeinbackground, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, r_ticstability, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vid_dontdowait, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, cl_capfps)
CVAR(Bool, cl_resumesavegame, true, CVAR_ARCHIVE)
EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR (Int, vid_maxfps)
EXTERN_CVAR (Int, perf_looptraceframes)

static uint64_t stabilityticduration = 0;
static uint64_t stabilitystarttime = 0;

DCorePlayer* PlayerArray[MAXPLAYERS];

IMPLEMENT_CLASS(DCorePlayer, true, true)
IMPLEMENT_POINTERS_START(DCorePlayer)
IMPLEMENT_POINTER(actor)
IMPLEMENT_POINTERS_END

void MarkPlayers()
{
	GC::MarkArray(PlayerArray, MAXPLAYERS);
}

bool r_NoInterpolate;
int entertic;
int oldentertics;
int gametic;
int nextwipe = wipe_None;

FString savename;
FString BackupSaveGame;

void DoLoadGame(const char* name);

bool sendsave;
FString	savedescription;
FString	savegamefile;

namespace
{
	struct PerfTryRunTicsTraceStats
	{
		bool doWait = false;
		bool pausedReturn = false;
		bool zeroCountReturn = false;
		bool waitLoopReturn = false;
		int realtics = 0;
		int availabletics = 0;
		int counts = 0;
		int lowtic = 0;
		int waitLoopIterations = 0;
		int ticksRun = 0;
		double durationMs = 0.0;
	};

	struct PerfDisplayTraceStats
	{
		bool skippedInactive = false;
		bool levelRendered = false;
		double beginFrameMs = 0.0;
		double renderMs = 0.0;
		double overlayMs = 0.0;
		double updateMs = 0.0;
	};

	static PerfTryRunTicsTraceStats perfTryRunTicsTraceStats;
	static PerfDisplayTraceStats perfDisplayTraceStats;

	static const char* GetGameStateName(int state)
	{
		switch (state)
		{
		case GS_STARTUP: return "startup";
		case GS_LEVEL: return "level";
		case GS_MENUSCREEN: return "menu";
		case GS_FULLCONSOLE: return "console";
		case GS_CUTSCENE: return "cutscene";
		case GS_INTRO: return "intro";
		default: return "unknown";
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void G_BuildTiccmd(ticcmd_t* cmd) 
{
	if (sendsave)
	{
		sendsave = false;
		Net_WriteByte(DEM_SAVEGAME);
		Net_WriteString(savegamefile.GetChars());
		Net_WriteString(savedescription.GetChars());
		savegamefile = "";
	}
	cmd->ucmd = {};
	gameInput.getInput(&cmd->ucmd);
	cmd->consistency = consistency[myconnectindex][(maketic / ticdup) % BACKUPTICS];
}

//==========================================================================
//
//
//
//==========================================================================
bool newGameStarted;

void NewGame(MapRecord* map, int skill, bool ns = false)
{
	gi->FreeLevelData();
	newGameStarted = true;
	ShowIntermission(nullptr, map, nullptr, [=](bool) { 
		gi->NewGame(map, skill, ns); 
		gameaction = ga_level;
		ResetStatusBar();
		gameInput.resetCrouchToggle();
		});
}

//==========================================================================
//
//
//
//==========================================================================

static void GameTicker()
{
	handleevents();

	// Todo: Migrate state changes to here instead of doing them ad-hoc
	while (gameaction != ga_nothing)
	{
		auto ga = gameaction;
		gameaction = ga_nothing;
		switch (ga)
		{
		case ga_autoloadgame:
			C_FlushDisplay();
			if (BackupSaveGame.IsNotEmpty() && cl_resumesavegame)
			{
				DoLoadGame(BackupSaveGame.GetChars());
			}
			else
			{
				g_nextmap = currentLevel;
				FX_StopAllSounds();
				S_SetReverb(0);
				NewGame(g_nextmap, -1);
				BackupSaveGame = "";
			}
			break;

		case ga_completed:
			FX_StopAllSounds();
			S_SetReverb(0);
			gi->LevelCompleted(g_nextmap, g_nextskill);
			break;

		case ga_nextlevel:
			gi->FreeLevelData();
			gameaction = ga_level;
			gi->NextLevel(g_nextmap, g_nextskill);
			ResetStatusBar();
			if (!isBlood()) M_Autosave();
			break;

		case ga_newgame:
			FX_StopAllSounds();
			[[fallthrough]];
		case ga_newgamenostopsound:
			DeleteScreenJob();
			S_SetReverb(0);
			C_FlushDisplay();
			BackupSaveGame = "";
			NewGame(g_nextmap, g_nextskill, ga == ga_newgamenostopsound);
			break;

		case ga_startup:
			Mus_Stop();
			FX_StopAllSounds();
			gi->FreeLevelData();
			gamestate = GS_STARTUP;
			break;

		case ga_mainmenu:
			FX_StopAllSounds();
			if (isBlood()) Mus_Stop();
			[[fallthrough]];
		case ga_mainmenunostopsound:
			gi->FreeLevelData();
			gamestate = GS_MENUSCREEN;
			M_StartControlPanel(ga == ga_mainmenu);
			M_SetMenu(NAME_Mainmenu);
			break;

		case ga_creditsmenu:
			FX_StopAllSounds();
			gi->FreeLevelData();
			gamestate = GS_MENUSCREEN;
			M_StartControlPanel(false);
			M_SetMenu(NAME_Mainmenu);
			M_SetMenu(NAME_CreditsMenu);
			break;

		case ga_savegame:
			G_DoSaveGame(true, false, savegamefile.GetChars(), savedescription.GetChars());
			gameaction = ga_nothing;
			savegamefile = "";
			savedescription = "";
			break;

		case ga_loadgame:
		case ga_loadgamehidecon:
		//case ga_autoloadgame:
			G_DoLoadGame();
			break;

		case ga_autosave:
			if (gamestate == GS_LEVEL && !newGameStarted) M_Autosave();
			newGameStarted = false;
			break;

		case ga_level:
			Net_ClearFifo();
			inputState.ClearAllInput();
			gameInput.Clear();
			gamestate = GS_LEVEL;
			return;

		case ga_intro:
			gamestate = GS_INTRO;
			break;

		case ga_intermission:
			gamestate = GS_CUTSCENE;
			break;

		case ga_fullconsole:
			C_FullConsole();
			Mus_Stop();
			gameaction = ga_nothing;
			break;

		case ga_endscreenjob:
			EndScreenJob();
			break;

			// for later
		// case ga_recordgame,			// start a new demo recording (later)
		// case ga_loadgameplaydemo,	// load a savegame and play a demo.

		default:
			break;
		}
		C_AdjustBottom();
	}

	// get commands, check consistancy, and build new consistancy check
	int buf = (gametic / ticdup) % BACKUPTICS;

	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			ticcmd_t* cmd = &PlayerArray[i]->cmd;
			ticcmd_t* newcmd = &netcmds[i][buf];
			PlayerArray[i]->lastcmd = *cmd;
			PlayerArray[i]->resetCameraAngles();

			if ((gametic % ticdup) == 0)
			{
				RunNetSpecs(i, buf);
			}
#if 0
			if (demorecording)
			{
				G_WriteDemoTiccmd(newcmd, i, buf);
			}
			if (demoplayback)
			{
				G_ReadDemoTiccmd(cmd, i);
			}
			else
#endif
			{
				*cmd = *newcmd;
			}


			if (netgame && /*!demoplayback &&*/ (gametic % ticdup) == 0)
			{
#if 0
				//players[i].inconsistant = 0;
				if (gametic > BACKUPTICS * ticdup && consistancy[i][buf] != cmd->consistancy)
				{
					players[i].inconsistant = gametic - BACKUPTICS * ticdup;
				}
#endif
				consistency[i][buf] = gi->GetPlayerChecksum(i);
			}
		}
	}

	C_RunDelayedCommands();
	updatePauseStatus();

	switch (gamestate)
	{
	default:
	case GS_STARTUP:
		gi->Startup();
		break;

	case GS_LEVEL:
		gameupdatetime.Reset();
		gameupdatetime.Clock();
		gameInput.ResetInputSync();
		gi->Ticker();
		TickStatusBar();
		levelTextTime--;
		gameupdatetime.Unclock();
		break;

	case GS_MENUSCREEN:
	case GS_FULLCONSOLE:
		break;
	case GS_CUTSCENE:
	case GS_INTRO:
		if (ScreenJobTick())
		{
			// synchronize termination with the playsim.
			Net_WriteByte(DEM_ENDSCREENJOB);
		}
		break;

	}
}


void DrawOverlays()
{
	NetUpdate();			// send out any new accumulation

	if (gamestate != GS_INTRO) // do not draw overlays on the intros
	{
		// Draw overlay elements
		CT_Drawer();
		C_DrawConsole();
		M_Drawer();
		FStat::PrintStat(twod);
	}
	DrawRateStuff();
}

//==========================================================================
//
// Display
//
//==========================================================================
CVAR(String, drawtile, "", 0)	// debug stuff. Draws the tile with the given number on top of thze HUD

void Display()
{
	if (screen == nullptr || (!AppActive && (screen->IsFullscreen() || !vid_activeinbackground)))
	{
		perfDisplayTraceStats.skippedInactive = true;
		return;
	}
	
	FTexture* wipestart = nullptr;
	if (nextwipe != wipe_None)
	{
		wipestart = screen->WipeStartScreen();
	}

	double stageStart = I_msTimeF();
	screen->FrameTime = I_msTimeFS();
	tileUpdateAnimations();
	screen->BeginFrame();
	perfDisplayTraceStats.beginFrameMs += I_msTimeF() - stageStart;
	twodpsp.Clear();
	twodpsp.SetSize(screen->GetWidth(), screen->GetHeight());
	twodpsp.ClearClipRect();
	twod->Clear();
	//twod->SetSize(screen->GetWidth(), screen->GetHeight());
	twod->Begin(screen->GetWidth(), screen->GetHeight());
	twod->ClearClipRect();
	switch (gamestate)
	{
	case GS_MENUSCREEN:
	case GS_FULLCONSOLE:
		gi->DrawBackground();
		break;

	case GS_INTRO:
	case GS_CUTSCENE:
		ScreenJobDraw();
		break;

	case GS_LEVEL:
		if (gametic != 0)
		{
			perfDisplayTraceStats.levelRendered = true;
			stageStart = I_msTimeF();
			screen->FrameTime = I_msTimeFS();
			screen->BeginFrame();
			screen->SetSceneRenderTarget(gl_ssao != 0);
			//updateModelInterpolation();
			gi->Render();
			DrawFullscreenBlends();
			drawMapTitle();
			perfDisplayTraceStats.renderMs += I_msTimeF() - stageStart;
			break;
		}
		[[fallthrough]];

	default:
		twod->ClearScreen();
		break;
	}
	
	stageStart = I_msTimeF();
	if (nextwipe == wipe_None)
	{
		DrawOverlays();
		if (drawtile[0])
		{
			auto tex = TexMan.CheckForTexture(drawtile, ETextureType::Any);
			if (!tex.isValid()) tex = tileGetTextureID(atoi(drawtile));
			if (tex.isValid())
			{
				auto tx = TexMan.GetGameTexture(tex, true);
				if (tx)
				{
					int width = (int)tx->GetDisplayWidth();
					int height = (int)tx->GetDisplayHeight();
					int dwidth, dheight;
					if (width > height)
					{
						dwidth = screen->GetWidth() / 4;
						dheight = height * dwidth / width;
					}
					else
					{
						dheight = screen->GetHeight() / 4;
						dwidth = width * dheight / height;
					}
					DrawTexture(twod, tx, 0, 0, DTA_DestWidth, dwidth, DTA_DestHeight, dheight, TAG_DONE);
				}
			}
		}
	}
	else
	{
		PerformWipe(wipestart, screen->WipeEndScreen(), nextwipe, true, DrawOverlays);
		nextwipe = wipe_None;
	}
	perfDisplayTraceStats.overlayMs += I_msTimeF() - stageStart;

	stageStart = I_msTimeF();
	screen->Update();
	perfDisplayTraceStats.updateMs += I_msTimeF() - stageStart;
}

//==========================================================================
//
// Forces playsim processing time to be consistent across frames.
// This improves interpolation for frames in between tics.
//
// With this cvar off the mods with a high playsim processing time will appear
// less smooth as the measured time used for interpolation will vary.
//
//==========================================================================

static void TicStabilityWait()
{
	using namespace std::chrono;
	using namespace std::this_thread;

	if (!r_ticstability)
		return;

	uint64_t start = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	while (true)
	{
		uint64_t cur = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
		if (cur - start > stabilityticduration)
			break;
	}
}

static void TicStabilityBegin()
{
	using namespace std::chrono;
	stabilitystarttime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static void TicStabilityEnd()
{
	using namespace std::chrono;
	uint64_t stabilityendtime = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	stabilityticduration = min(stabilityendtime - stabilitystarttime, (uint64_t)1'000'000);
}

//==========================================================================
//
// The most important function in the engine.
//
//==========================================================================

void TryRunTics (void)
{
	int 		i;
	int 		lowtic;
	int 		realtics;
	int 		availabletics;
	int 		counts;
	int 		numplaying;
	const double traceStartMs = I_msTimeF();
	perfTryRunTicsTraceStats = {};

	// If paused, do not eat more CPU time than we need, because it
	// will all be wasted anyway.
	bool doWait = (cl_capfps || pauseext || (r_NoInterpolate && !M_IsAnimated() && gamestate != GS_CUTSCENE && gamestate != GS_INTRO));

	if (vid_dontdowait && ((vid_maxfps > 0) || (vid_vsync == true)))
		doWait = false;
	perfTryRunTicsTraceStats.doWait = doWait;

	// get real tics
	if (doWait)
	{
		entertic = I_WaitForTic (oldentertics);
	}
	else
	{
		entertic = I_GetTime ();
	}
	realtics = entertic - oldentertics;
	oldentertics = entertic;
	perfTryRunTicsTraceStats.realtics = realtics;

	// get available tics
	NetUpdate ();

	if (pauseext)
	{
		perfTryRunTicsTraceStats.pausedReturn = true;
		perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
		return;
	}

	lowtic = INT_MAX;
	numplaying = 0;
	for (i = 0; i < doomcom.numnodes; i++)
	{
		if (nodeingame[i])
		{
			numplaying++;
			if (nettics[i] < lowtic)
				lowtic = nettics[i];
		}
	}

	availabletics = lowtic - gametic / ticdup;
	perfTryRunTicsTraceStats.availabletics = availabletics;
	perfTryRunTicsTraceStats.lowtic = lowtic;

	// decide how many tics to run
	if (realtics < availabletics-1)
		counts = realtics+1;
	else if (realtics < availabletics)
		counts = realtics;
	else
		counts = availabletics;
	perfTryRunTicsTraceStats.counts = counts;

	// Uncapped framerate needs seprate checks
	if (counts == 0 && !doWait)
	{
		TicStabilityWait();

		// Check possible stall conditions
		Net_CheckLastReceived(counts);
		if (realtics >= 1)
		{
			C_Ticker();
			M_Ticker();
			// Repredict the player for new buffered movement
#if 0
			gi->Unpredict();
			gi->Predict(myconnectindex);
#endif
		}
		if (!gameInput.SyncInput())
		{
			gameInput.getInput();
		}
		perfTryRunTicsTraceStats.zeroCountReturn = true;
		perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
		return;
	}

	if (counts < 1)
		counts = 1;

	// wait for new tics if needed
	while (lowtic < gametic + counts)
	{
		perfTryRunTicsTraceStats.waitLoopIterations++;
		NetUpdate ();
		lowtic = INT_MAX;

		for (i = 0; i < doomcom.numnodes; i++)
			if (nodeingame[i] && nettics[i] < lowtic)
				lowtic = nettics[i];

		lowtic = lowtic * ticdup;

		if (lowtic < gametic)
			I_Error ("TryRunTics: lowtic < gametic");

		// Check possible stall conditions
		Net_CheckLastReceived (counts);

		// Update time returned by I_GetTime, but only if we are stuck in this loop
		if (lowtic < gametic + counts)
			I_SetFrameTime();

		// don't stay in here forever -- give the menu a chance to work
		if (I_GetTime () - entertic >= 1)
		{
			C_Ticker ();
			M_Ticker ();
			// Repredict the player for new buffered movement
#if 0
			gi->Unpredict();
			gi->Predict(myconnectindex);
#endif
			perfTryRunTicsTraceStats.waitLoopReturn = true;
			perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
			return;
		}
	}

	//Tic lowtic is high enough to process this gametic. Clear all possible waiting info
	hadlate = false;
#if 0
	for (i = 0; i < MAXPLAYERS; i++)
		players[i].waiting = false;
#endif
	lastglobalrecvtime = I_GetTime (); //Update the last time the game tic'd over

	// run the count tics
	if (counts > 0)
	{
#if 0
		gi->Unpredict();
#endif
		while (counts--)
		{
			perfTryRunTicsTraceStats.ticksRun++;
			TicStabilityBegin();
			if (gametic > lowtic)
			{
				I_Error ("gametic>lowtic");
			}
#if 0
			if (advancedemo)
			{
				D_DoAdvanceDemo ();
			}
#endif
			C_Ticker ();
			M_Ticker ();
			GameTicker();
			gametic++;

			NetUpdate ();	// check for new console commands
			TicStabilityEnd();
		}
#if 0
		gi->Predict(myconnectindex);
#endif
		gi->UpdateSounds();
		soundEngine->UpdateSounds(I_GetTime());
	}
	else
	{
		TicStabilityWait();
	}
	perfTryRunTicsTraceStats.durationMs = I_msTimeF() - traceStartMs;
}


//==========================================================================
//
// MainLoop - will never return aside from exceptions being thrown.
//
//==========================================================================

void MainLoop ()
{
	int lasttic = 0;
	uint64_t traceFrame = 0;

	// Clamp the timer to TICRATE until the playloop has been entered.
	r_NoInterpolate = true;

	if (userConfig.CommandMap.IsNotEmpty())
	{
		auto maprecord = FindMapByName(userConfig.CommandMap.GetChars());
		if (maprecord == nullptr)
		{
			maprecord = SetupUserMap(userConfig.CommandMap.GetChars(), g_gameType & GAMEFLAG_DUKE? "dethtoll.mid" : nullptr);
		}
		userConfig.CommandMap = "";
		if (maprecord)
		{
			DeferredStartGame(maprecord, g_nextskill);
		}
	}

	for (;;)
	{
		try
		{
			traceFrame++;
			if (PerfLoopTraceActive())
			{
				PerfLoopTraceResetInputStats();
				perfTryRunTicsTraceStats = {};
				perfDisplayTraceStats = {};
			}

			// frame syncronous IO operations
			const double frameStartMs = I_msTimeF();
			double startFrameMs = 0.0;
			if (gametic > lasttic)
			{
				const double stageStartMs = I_msTimeF();
				lasttic = gametic;
				I_StartFrame ();
				startFrameMs = I_msTimeF() - stageStartMs;
			}
			I_SetFrameTime();

			// update the scale factor for unsynchronised input here.
			gameInput.UpdateInputScale();

			const double tryRunStartMs = I_msTimeF();
			TryRunTics (); // will run at least one tic
			const double tryRunMs = I_msTimeF() - tryRunStartMs;
			// Update display, next frame, with current state.
			const double startTicStartMs = I_msTimeF();
			I_StartTic();
			const double startTicMs = I_msTimeF() - startTicStartMs;

			const double displayStartMs = I_msTimeF();
			Display();
			const double displayMs = I_msTimeF() - displayStartMs;
			const double musicStartMs = I_msTimeF();
			Mus_UpdateMusic();		// must be at the end.
			const double musicMs = I_msTimeF() - musicStartMs;

			if (PerfLoopTraceActive())
			{
				const auto inputTrace = PerfLoopTraceGetInputStats();
				const auto renderTrace = GetPerfRenderTraceStats();
				const double frameMs = I_msTimeF() - frameStartMs;
				Printf(
					"PERF loop trace: frame=%llu state=%s gametic=%d startframe_ms=%.3f try_ms=%.3f try_traced_ms=%.3f display_ms=%.3f display_begin_ms=%.3f display_render_ms=%.3f display_overlay_ms=%.3f display_update_ms=%.3f starttic_ms=%.3f music_ms=%.3f frame_ms=%.3f do_wait=%d realtics=%d avail=%d counts=%d ticks=%d wait_loops=%d zero_return=%d wait_return=%d paused_return=%d display_skip=%d level_rendered=%d\n",
					(unsigned long long)traceFrame,
					GetGameStateName(gamestate),
					gametic,
					startFrameMs,
					tryRunMs,
					perfTryRunTicsTraceStats.durationMs,
					displayMs,
					perfDisplayTraceStats.beginFrameMs,
					perfDisplayTraceStats.renderMs,
					perfDisplayTraceStats.overlayMs,
					perfDisplayTraceStats.updateMs,
					startTicMs,
					musicMs,
					frameMs,
					perfTryRunTicsTraceStats.doWait ? 1 : 0,
					perfTryRunTicsTraceStats.realtics,
					perfTryRunTicsTraceStats.availabletics,
					perfTryRunTicsTraceStats.counts,
					perfTryRunTicsTraceStats.ticksRun,
					perfTryRunTicsTraceStats.waitLoopIterations,
					perfTryRunTicsTraceStats.zeroCountReturn ? 1 : 0,
					perfTryRunTicsTraceStats.waitLoopReturn ? 1 : 0,
					perfTryRunTicsTraceStats.pausedReturn ? 1 : 0,
					perfDisplayTraceStats.skippedInactive ? 1 : 0,
					perfDisplayTraceStats.levelRendered ? 1 : 0);
				Printf(
					"PERF render trace: frame=%llu hw_all=%.3f hw_finish=%.3f hw_render=%.3f hw_setup=%.3f hw_portal=%.3f hw_post=%.3f hw_drawcalls=%.3f wall_render=%.3f wall_setup=%.3f wall_clip=%.3f bsp=%.3f flat_render=%.3f flat_setup=%.3f sprite_render=%.3f sprite_setup=%.3f twod=%.3f finish3d=%.3f mt_wait=%.3f wt_total=%.3f walls=%d flats=%d sprites=%d decals=%d portals=%d verts=%d flat_verts=%d flat_prims=%d\n",
					(unsigned long long)traceFrame,
					renderTrace.allMs,
					renderTrace.finishMs,
					renderTrace.renderAllMs,
					renderTrace.processAllMs,
					renderTrace.portalAllMs,
					renderTrace.postProcessMs,
					renderTrace.drawCallsMs,
					renderTrace.renderWallMs,
					renderTrace.setupWallMs,
					renderTrace.clipWallMs,
					renderTrace.bspMs,
					renderTrace.renderFlatMs,
					renderTrace.setupFlatMs,
					renderTrace.renderSpriteMs,
					renderTrace.setupSpriteMs,
					renderTrace.twoDMs,
					renderTrace.finish3DMs,
					renderTrace.mtWaitMs,
					renderTrace.wtTotalMs,
					renderTrace.renderedWalls,
					renderTrace.renderedFlats,
					renderTrace.renderedSprites,
					renderTrace.renderedDecals,
					renderTrace.renderedPortals,
					renderTrace.renderedVertices,
					renderTrace.flatVertexCount,
					renderTrace.flatPrimitiveCount);
				if (renderTrace.nriActive)
				{
					Printf(
						"PERF render trace NRI: frame=%llu total=%.3f init=%.3f res=%.3f state=%.3f capture=%.3f geo=%.3f mats=%.3f palette=%.3f textures=%.3f buffers=%.3f as=%.3f bootstrap=%.3f graph=%.3f copy=%.3f wait=%.3f wait_present=%.3f acquire=%.3f submit=%.3f present=%.3f trace=%.3f denoise=%.3f compose=%.3f upscale=%.3f final=%.3f raw_present=%.3f final_present=%.3f\n",
						(unsigned long long)traceFrame,
						renderTrace.nriAllMs,
						renderTrace.nriInitializeMs,
						renderTrace.nriFrameResourcesMs,
						renderTrace.nriUpdateStateMs,
						renderTrace.nriSceneCaptureMs,
						renderTrace.nriGeometryBuildMs,
						renderTrace.nriMaterialBuildMs,
						renderTrace.nriPaletteUploadMs,
						renderTrace.nriSceneTexturesMs,
						renderTrace.nriSceneBuffersMs,
						renderTrace.nriAccelerationMs,
						renderTrace.nriBootstrapDispatchMs,
						renderTrace.nriFrameGraphMs,
						renderTrace.nriCopyFinalMs,
						renderTrace.nriFrameWaitMs,
						renderTrace.nriWaitPresentMs,
						renderTrace.nriAcquireSwapMs,
						renderTrace.nriQueueSubmitMs,
						renderTrace.nriQueuePresentMs,
						renderTrace.nriTraceOpaqueMs,
						renderTrace.nriDenoiserMs,
						renderTrace.nriCompositionMs,
						renderTrace.nriUpscaleMs,
						renderTrace.nriFinalMs,
						renderTrace.nriRawPresentMs,
						renderTrace.nriFinalPresentMs);
				}
				Printf(
					"PERF input trace: frame=%llu getevent=%u starttic_calls=%u handleevents=%u msgs=%u burst=%u raw_input=%u raw_keyboard=%u raw_mouse=%u raw_mouse_moves=%u raw_mouse_drop=%u posted_mouse=%u dispatched_mouse=%u sampled_mouse=%u posted_delta=(%.1f,%.1f) dispatched_delta=(%.1f,%.1f) sampled_delta=(%.1f,%.1f) key_down=%u key_up=%u device_change=%u queue_hw=%u queue_overflow=%u\n",
					(unsigned long long)traceFrame,
					inputTrace.iGetEventCalls,
					inputTrace.startTicCalls,
					inputTrace.handleeventsCalls,
					inputTrace.peekedMessages,
					inputTrace.maxMessageBurst,
					inputTrace.rawInputMessages,
					inputTrace.rawKeyboardPackets,
					inputTrace.rawMousePackets,
					inputTrace.rawMouseMovePackets,
					inputTrace.rawMouseDroppedPackets,
					inputTrace.postedMouseMoves,
					inputTrace.dispatchedMouseMoves,
					inputTrace.sampledMouseInputs,
					inputTrace.postedMouseX,
					inputTrace.postedMouseY,
					inputTrace.dispatchedMouseX,
					inputTrace.dispatchedMouseY,
					inputTrace.sampledMouseX,
					inputTrace.sampledMouseY,
					inputTrace.keyDownEvents,
					inputTrace.keyUpEvents,
					inputTrace.deviceChangeEvents,
					inputTrace.eventQueueHighWater,
					inputTrace.eventQueueOverflows);
				const int remainingTraceFrames = (int)perf_looptraceframes - 1;
				perf_looptraceframes = remainingTraceFrames > 0 ? remainingTraceFrames : 0;
			}
		}
		catch (CRecoverableError &error)
		{
			if (PerfLoopTraceActive())
			{
				Printf("PERF loop trace caught: frame=%llu type=recoverable state=%s gametic=%d\n",
					(unsigned long long)traceFrame,
					GetGameStateName(gamestate),
					gametic);
			}
			if (error.GetMessage ())
			{
				Printf (PRINT_BOLD, "\n%s\n", error.GetMessage());
			}
			gi->ErrorCleanup();
			M_ClearMenus();
			C_FullConsole();
			gameaction = ga_nothing;
		}
		catch (CVMAbortException &error)
		{
			if (PerfLoopTraceActive())
			{
				Printf("PERF loop trace caught: frame=%llu type=vmabort state=%s gametic=%d\n",
					(unsigned long long)traceFrame,
					GetGameStateName(gamestate),
					gametic);
			}
			error.MaybePrintMessage();
			Printf("%s", error.stacktrace.GetChars());
			gi->ErrorCleanup();
			twod->SetOffset(DVector2(0, 0));
			M_ClearMenus();
			C_FullConsole();
		}
	}
}
