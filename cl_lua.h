/*
Insanotic: client-side Lua mods.
SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef CL_LUA_H
#define CL_LUA_H

// true while a Lua mod drives the touchscreen (xon.touch_capture); the
// engine's built-in touchscreen areas and touch-to-mouse emulation stand down
extern qboolean cl_lua_touch_captured;
extern cvar_t cl_lua_touch_mouse;

void CL_Lua_Init(void);
void CL_Lua_Shutdown(void);
// once per client frame (loads the mods on the first call)
void CL_Lua_Frame(void);
// lets mods adjust the move about to be sent; called from CL_SendMove
void CL_Lua_Input(int *buttons, int *impulse);
// 2D drawing pass, after the game HUD and before menu and console
void CL_Lua_DrawHUD(void);
// connection closed: forget per-server permissions
void CL_Lua_Disconnected(void);
// a sound a mod played ahead of time (xon.play_sound predicted) arrives for
// real: returns true once per prediction so the duplicate is skipped
struct sfx_s;
qboolean CL_Lua_ConsumePredictedSound(struct sfx_s *sfx, int entnum);

// touch input reported by the video backend: fingers in normalized screen
// coordinates; returns the number written (vid_sdl.c, 0 elsewhere)
#define VID_MAXTOUCHES 11
typedef struct vid_touch_s
{
	int id;
	float x, y;
} vid_touch_t;
int VID_GetTouches(vid_touch_t *out, int max);
// the last input came from a touchscreen rather than keyboard or mouse
int VID_InputIsTouch(void);
// a touchscreen is present
int VID_HasTouch(void);

#endif
