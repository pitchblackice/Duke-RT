#pragma once
#include <stdint.h>
#include "d_gui.h"
#include "zstring.h"

// Input event types.
enum EGenericEvent
{
	EV_None,
	EV_KeyDown,		// data1: scan code, data2: Qwerty ASCII code
	EV_KeyUp,		// same
	EV_Mouse,		// x, y: mouse movement deltas
	EV_GUI_Event,	// subtype specifies actual event
	EV_DeviceChange,// a device has been connected or removed
};

// Event structure.
struct event_t
{
	uint8_t		type;
	uint8_t		subtype;
	int16_t 	data1;		// keys / mouse/joystick buttons
	int16_t		data2;
	int16_t		data3;
	float 		x;			// mouse/joystick x move
	float 		y;			// mouse/joystick y move
};



// Called by IO functions when input is detected.
void D_PostEvent (event_t* ev);
void D_RemoveNextCharEvent();
void D_ProcessEvents(void);
void PostMouseMove(int x, int y);

enum
{
	MAXEVENTS = 128
};

extern	event_t 		events[MAXEVENTS];
extern int eventhead;
extern int eventtail;

struct PerfLoopInputTraceStats
{
	uint32_t iGetEventCalls = 0;
	uint32_t startTicCalls = 0;
	uint32_t handleeventsCalls = 0;
	uint32_t peekedMessages = 0;
	uint32_t maxMessageBurst = 0;
	uint32_t rawInputMessages = 0;
	uint32_t rawKeyboardPackets = 0;
	uint32_t rawMousePackets = 0;
	uint32_t rawMouseMovePackets = 0;
	uint32_t rawMouseDroppedPackets = 0;
	uint32_t postedMouseMoves = 0;
	uint32_t dispatchedMouseMoves = 0;
	uint32_t sampledMouseInputs = 0;
	uint32_t keyDownEvents = 0;
	uint32_t keyUpEvents = 0;
	uint32_t deviceChangeEvents = 0;
	uint32_t eventQueueHighWater = 0;
	uint32_t eventQueueOverflows = 0;
	float postedMouseX = 0.0f;
	float postedMouseY = 0.0f;
	float dispatchedMouseX = 0.0f;
	float dispatchedMouseY = 0.0f;
	float sampledMouseX = 0.0f;
	float sampledMouseY = 0.0f;
};

bool PerfLoopTraceActive();
void PerfLoopTraceResetInputStats();
PerfLoopInputTraceStats PerfLoopTraceGetInputStats();
void PerfLoopTraceNoteHandleevents();
void PerfLoopTraceNoteIStartTic();
void PerfLoopTraceNoteIGetEvent(uint32_t peekedMessages);
void PerfLoopTraceNoteRawInputMessage(bool isMouse, bool isKeyboard);
void PerfLoopTraceNoteRawMousePacket(bool accepted, int dx, int dy);
void PerfLoopTraceNoteMousePost(float x, float y);
void PerfLoopTraceNoteMouseDispatch(float x, float y);
void PerfLoopTraceNoteGameInputSample(float x, float y);

struct FUiEvent
{
	// this essentially translates event_t UI events to ZScript.
	EGUIEvent Type;
	// for keys/chars/whatever
	FString KeyString;
	int KeyChar;
	// for mouse
	int MouseX;
	int MouseY;
	// global (?)
	bool IsShift;
	bool IsCtrl;
	bool IsAlt;

	FUiEvent(const event_t *ev);
};

struct FInputEvent
{
	// this translates regular event_t events to ZScript (not UI, UI events are sent via DUiEvent and only if requested!)
	EGenericEvent Type = EV_None;
	// for keys
	int KeyScan;
	FString KeyString;
	int KeyChar;
	// for mouse
	int MouseX;
	int MouseY;

	FInputEvent(const event_t *ev);
};
