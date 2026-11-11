/*
Insanotic: client-side Lua mods.
SPDX-License-Identifier: GPL-3.0-or-later

Mods live in mods/<name>/init.lua (optional mods/<name>/mod.json with name,
version and order) and run in the sandbox from lua_sandbox.c. They load when
the client starts and on lua_reload.

Hooks fired from the engine:
  Frame       every frame                ev.frametime, ev.in_game
  Input       before a move is sent      ev.angles, ev.move, ev.buttons,
                                         ev.impulse (all writable)
  HUD         2D pass over the game view ev.width, ev.height, ev.in_game
  Connect     signon finished
  Disconnect  left the server

Reading other players follows the approach of Xonotic Touch's touch_aim.c
(https://github.com/dixonSolutions/Xonotic-Touch): Xonotic networks players
as CSQC entities, so a CSQC edict whose entnum is 1..maxclients is that
player's model with its interpolated origin and networked size; dead bodies
have solid SOLID_CORPSE; in team games the pants colour from the scoreboard
is the team, and the CSQC's own "teamplay" global says whether it is one.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "csprogs.h"
#include "cl_screen.h"
#include "lua_sandbox.h"
#include "cl_lua.h"

cvar_t cl_lua = {CVAR_SAVE, "cl_lua", "1", "load client Lua mods from mods/<name>/init.lua"};
cvar_t cl_lua_instruction_limit = {0, "cl_lua_instruction_limit", "500000", "Lua VM instructions one client mod may execute per entry (loading, or one hook call) before it is killed and disabled"};
cvar_t cl_lua_memory_limit = {0, "cl_lua_memory_limit", "32", "memory in MB one client mod may allocate before it is killed and disabled"};
cvar_t cl_lua_touch_mouse = {0, "cl_lua_touch_mouse", "0", "development aid: the left mouse button acts as a finger on the touchscreen while a mod captures touch input"};

qboolean cl_lua_touch_captured;

static void CL_Lua_RegisterAPI(lua_State *L);

static luahost_t cl_luahost =
{
	"mod", "mods", "mod.json", "game",
	&cl_lua_instruction_limit, &cl_lua_memory_limit,
	CL_Lua_RegisterAPI,
	{NULL}, 0, NULL
};

static qboolean cl_lua_loaded;
static qboolean cl_lua_drawing;
static qboolean cl_lua_was_connected;

//============================================================================
// dispatch

// calls every handler of hook with a fresh ev table built by push(); after
// each mod, pull() may read the table back. Returns true when any handler
// returned true.
typedef void (*cl_lua_evfunc_t)(lua_State *L, int ev, void *data);

static qboolean CL_Lua_Dispatch(const char *hook, cl_lua_evfunc_t push, cl_lua_evfunc_t pull, void *data)
{
	qboolean ret = false;
	int i, j;
	if (LuaSandbox_HookCount(&cl_luahost, hook) <= 0)
		return false;
	for (i = 0; i < cl_luahost.num; i++)
	{
		luasandbox_t *m = cl_luahost.items[i];
		lua_State *L = m->L;
		int handlers, ev, n;
		n = LuaSandbox_PushHandlers(m, hook);
		if (!n)
			continue;
		handlers = lua_gettop(L);
		lua_createtable(L, 0, 8);
		ev = lua_gettop(L);
		lua_pushstring(L, hook);
		lua_setfield(L, ev, "hook");
		if (push)
			push(L, ev, data);
		for (j = 1; j <= n; j++)
		{
			lua_geti(L, handlers, j);
			lua_pushvalue(L, ev);
			if (!LuaSandbox_Call(m, 1, 1, hook))
				break;
			if (lua_toboolean(L, -1))
				ret = true;
			lua_pop(L, 1);
		}
		if (m->disabled)
			continue;
		if (pull)
			pull(L, ev, data);
		lua_settop(L, handlers - 1);
	}
	return ret;
}

static qboolean CL_Lua_InGame(void)
{
	return cls.state == ca_connected && cls.signon == SIGNONS && key_dest == key_game && !key_consoleactive;
}

//============================================================================
// loading

static float CL_Lua_ManifestOrder(const char *dir)
{
	char path[MAX_QPATH];
	unsigned char *buf;
	fs_offset_t size;
	float order = 0;
	char *text, *p;
	dpsnprintf(path, sizeof(path), "%s/mod.json", dir);
	buf = FS_LoadFile(path, tempmempool, true, &size);
	if (!buf)
		return 0;
	text = (char *)Mem_Alloc(tempmempool, size + 1);
	memcpy(text, buf, size);
	text[size] = 0;
	if ((p = strstr(text, "\"order\"")) && (p = strchr(p, ':')))
		order = atof(p + 1);
	Mem_Free(text);
	Mem_Free(buf);
	return order;
}

static void CL_Lua_LoadAll(void)
{
	fssearch_t *search;
	char *paths[LUASANDBOX_MAX];
	float orders[LUASANDBOX_MAX];
	int i, j, n = 0;

	cl_lua_loaded = true;
	if (!cl_lua.integer)
		return;
	search = FS_Search("mods/*/init.lua", true, true);
	if (search)
	{
		for (i = 0; i < search->numfilenames && n < LUASANDBOX_MAX; i++)
		{
			char dir[MAX_QPATH];
			const char *path = search->filenames[i];
			float order;
			strlcpy(dir, path, sizeof(dir));
			if (strlen(dir) > 9)
				dir[strlen(dir) - 9] = 0; // strip "/init.lua"
			order = CL_Lua_ManifestOrder(dir);
			// insertion sort by (order, path)
			for (j = n; j > 0 && (orders[j - 1] > order || (orders[j - 1] == order && strcmp(paths[j - 1], path) > 0)); j--)
			{
				paths[j] = paths[j - 1];
				orders[j] = orders[j - 1];
			}
			paths[j] = (char *)path;
			orders[j] = order;
			n++;
		}
		for (i = 0; i < n; i++)
			LuaSandbox_Load(&cl_luahost, paths[i]);
		FS_FreeSearch(search);
	}
	if (n)
		Con_Printf("[lua] %d client mod%s found\n", n, n == 1 ? "" : "s");
}

static void CL_Lua_CloseAll(void)
{
	LuaSandbox_CloseAll(&cl_luahost);
	cl_lua_touch_captured = false;
}

//============================================================================
// engine hooks

static void CL_Lua_PushFrame(lua_State *L, int ev, void *data)
{
	lua_pushnumber(L, cl.realframetime); lua_setfield(L, ev, "frametime");
	lua_pushboolean(L, CL_Lua_InGame()); lua_setfield(L, ev, "in_game");
}

void CL_Lua_Frame(void)
{
	qboolean connected;
	if (!cl_lua_loaded)
		CL_Lua_LoadAll();
	if (!cl_luahost.num)
		return;
	connected = cls.state == ca_connected && cls.signon == SIGNONS;
	if (connected != cl_lua_was_connected)
	{
		cl_lua_was_connected = connected;
		CL_Lua_Dispatch(connected ? "Connect" : "Disconnect", NULL, NULL, NULL);
	}
	CL_Lua_Dispatch("Frame", CL_Lua_PushFrame, NULL, NULL);
}

typedef struct cl_lua_input_s
{
	int *buttons;
	int *impulse;
} cl_lua_input_t;

static void CL_Lua_PushInput(lua_State *L, int ev, void *data)
{
	cl_lua_input_t *in = (cl_lua_input_t *)data;
	vec3_t move;
	LuaSandbox_PushVec(L, cl.viewangles); lua_setfield(L, ev, "angles");
	VectorSet(move, cl.cmd.forwardmove, cl.cmd.sidemove, cl.cmd.upmove);
	LuaSandbox_PushVec(L, move); lua_setfield(L, ev, "move");
	lua_pushinteger(L, *in->buttons); lua_setfield(L, ev, "buttons");
	lua_pushinteger(L, *in->impulse); lua_setfield(L, ev, "impulse");
	lua_pushnumber(L, cl.realframetime); lua_setfield(L, ev, "frametime");
}

static void CL_Lua_PullInput(lua_State *L, int ev, void *data)
{
	cl_lua_input_t *in = (cl_lua_input_t *)data;
	vec3_t v;
	if (lua_getfield(L, ev, "angles") == LUA_TTABLE && LuaSandbox_ToVec(L, -1, v))
		VectorCopy(v, cl.viewangles);
	lua_pop(L, 1);
	if (lua_getfield(L, ev, "move") == LUA_TTABLE && LuaSandbox_ToVec(L, -1, v))
	{
		cl.cmd.forwardmove = v[0];
		cl.cmd.sidemove = v[1];
		cl.cmd.upmove = v[2];
	}
	lua_pop(L, 1);
	if (lua_getfield(L, ev, "buttons") == LUA_TNUMBER)
		*in->buttons = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (lua_getfield(L, ev, "impulse") == LUA_TNUMBER)
		*in->impulse = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
}

void CL_Lua_Input(int *buttons, int *impulse)
{
	cl_lua_input_t in;
	if (!cl_luahost.num || !CL_Lua_InGame())
		return;
	in.buttons = buttons;
	in.impulse = impulse;
	CL_Lua_Dispatch("Input", CL_Lua_PushInput, CL_Lua_PullInput, &in);
}

static void CL_Lua_PushHUD(lua_State *L, int ev, void *data)
{
	lua_pushnumber(L, vid_conwidth.value); lua_setfield(L, ev, "width");
	lua_pushnumber(L, vid_conheight.value); lua_setfield(L, ev, "height");
	lua_pushboolean(L, CL_Lua_InGame()); lua_setfield(L, ev, "in_game");
	lua_pushnumber(L, cl.realframetime); lua_setfield(L, ev, "frametime");
}

void CL_Lua_DrawHUD(void)
{
	if (!cl_luahost.num)
		return;
	cl_lua_drawing = true;
	CL_Lua_Dispatch("HUD", CL_Lua_PushHUD, NULL, NULL);
	cl_lua_drawing = false;
	DrawQ_ResetClipArea();
}

void CL_Lua_Disconnected(void)
{
	cvar_t *c = Cvar_FindVar("_cl_assist_allowed");
	if (c)
		Cvar_SetQuick(c, "0");
}

//============================================================================
// xon.* client API

static int LuaCl_time(lua_State *L)
{
	lua_pushnumber(L, cl.time);
	return 1;
}

static int LuaCl_realtime(lua_State *L)
{
	lua_pushnumber(L, realtime);
	return 1;
}

static int LuaCl_in_game(lua_State *L)
{
	lua_pushboolean(L, CL_Lua_InGame());
	return 1;
}

static int LuaCl_screen_size(lua_State *L)
{
	lua_pushnumber(L, vid_conwidth.value);
	lua_pushnumber(L, vid_conheight.value);
	return 2;
}

// aim assistance is allowed in local games and on servers that say so
static int LuaCl_assist_allowed(lua_State *L)
{
	cvar_t *c = Cvar_FindVar("_cl_assist_allowed");
	lua_pushboolean(L, sv.active || (c && c->integer));
	return 1;
}

static int LuaCl_view(lua_State *L)
{
	vec3_t org;
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, org);
	LuaSandbox_PushVec(L, org);
	LuaSandbox_PushVec(L, cl.viewangles);
	return 2;
}

static int LuaCl_traceline(lua_State *L)
{
	vec3_t v1, v2, endpos, normal;
	trace_t trace;
	LuaSandbox_CheckVec(L, 1, v1);
	LuaSandbox_CheckVec(L, 2, v2);
	if (!cl.worldmodel)
		return luaL_error(L, "not in a game");
	trace = CL_TraceLine(v1, v2, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, 0, true, false, NULL, false, false);
	lua_createtable(L, 0, 5);
	lua_pushnumber(L, trace.fraction); lua_setfield(L, -2, "fraction");
	VectorCopy(trace.endpos, endpos);
	LuaSandbox_PushVec(L, endpos); lua_setfield(L, -2, "endpos");
	VectorCopy(trace.plane.normal, normal);
	LuaSandbox_PushVec(L, normal); lua_setfield(L, -2, "normal");
	lua_pushboolean(L, trace.startsolid); lua_setfield(L, -2, "startsolid");
	lua_pushboolean(L, trace.allsolid); lua_setfield(L, -2, "allsolid");
	return 1;
}

// CSQC "teamplay" global (Xonotic defines it); -1 when unknown
static int CL_Lua_TeamPlay(void)
{
	prvm_prog_t *prog = CLVM_prog;
	ddef_t *d;
	if (!prog->loaded)
		return -1;
	d = PRVM_ED_FindGlobal(prog, "teamplay");
	if (!d)
		return -1;
	return PRVM_GLOBALFIELDFLOAT(d->ofs) != 0;
}

static int LuaCl_teamplay(lua_State *L)
{
	int t = CL_Lua_TeamPlay();
	if (t < 0)
		lua_pushnil(L);
	else
		lua_pushboolean(L, t);
	return 1;
}

static void CL_Lua_PushPlayer(lua_State *L, int entnum, const vec3_t origin, const vec3_t mins, const vec3_t maxs, const vec3_t velocity, qboolean alive)
{
	int colors = cl.scores ? cl.scores[entnum - 1].colors : 0;
	lua_createtable(L, 0, 9);
	lua_pushinteger(L, entnum); lua_setfield(L, -2, "num");
	lua_pushstring(L, cl.scores ? cl.scores[entnum - 1].name : ""); lua_setfield(L, -2, "name");
	LuaSandbox_PushVec(L, origin); lua_setfield(L, -2, "origin");
	LuaSandbox_PushVec(L, mins); lua_setfield(L, -2, "mins");
	LuaSandbox_PushVec(L, maxs); lua_setfield(L, -2, "maxs");
	LuaSandbox_PushVec(L, velocity); lua_setfield(L, -2, "velocity");
	lua_pushinteger(L, colors & 15); lua_setfield(L, -2, "team_color");
	lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
	lua_pushboolean(L, entnum == cl.playerentity); lua_setfield(L, -2, "is_me");
}

// every other player the client currently knows a position for
static int LuaCl_players(lua_State *L)
{
	prvm_prog_t *prog = CLVM_prog;
	int i, n = 0;
	lua_newtable(L);
	if (cls.state != ca_connected || !cl.scores)
		return 1;
	if (prog->loaded && prog->fieldoffsets.entnum >= 0 && prog->fieldoffsets.origin >= 0)
	{
		for (i = 1; i < prog->num_edicts; i++)
		{
			prvm_edict_t *ed = PRVM_EDICT_NUM(i);
			int entnum;
			float solid;
			vec3_t vel;
			if (ed->priv.required->free)
				continue;
			entnum = (int)PRVM_clientedictfloat(ed, entnum);
			if (entnum < 1 || entnum > cl.maxclients)
				continue;
			solid = prog->fieldoffsets.solid >= 0 ? PRVM_clientedictfloat(ed, solid) : SOLID_BBOX;
			if (prog->fieldoffsets.velocity >= 0)
				VectorCopy(PRVM_clientedictvector(ed, velocity), vel);
			else
				VectorClear(vel);
			CL_Lua_PushPlayer(L, entnum, PRVM_clientedictvector(ed, origin),
				prog->fieldoffsets.mins >= 0 ? PRVM_clientedictvector(ed, mins) : vec3_origin,
				prog->fieldoffsets.maxs >= 0 ? PRVM_clientedictvector(ed, maxs) : vec3_origin,
				vel, solid != SOLID_CORPSE && solid != SOLID_NOT);
			lua_rawseti(L, -2, ++n);
		}
		if (n)
			return 1;
	}
	// no CSQC players: fall back to the engine's own entities
	for (i = 1; i <= cl.maxclients && i < cl.num_entities; i++)
	{
		entity_t *ent = cl.entities + i;
		static const vec3_t pmins = {-16, -16, -24}, pmaxs = {16, 16, 45};
		if (!ent->state_current.active)
			continue;
		CL_Lua_PushPlayer(L, i, ent->state_current.origin, pmins, pmaxs, vec3_origin, true);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int LuaCl_me(lua_State *L)
{
	lua_pushinteger(L, cl.playerentity);
	return 1;
}

static int LuaCl_touches(lua_State *L)
{
	vid_touch_t t[VID_MAXTOUCHES];
	int i, n = VID_GetTouches(t, VID_MAXTOUCHES);
	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++)
	{
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, t[i].id); lua_setfield(L, -2, "id");
		lua_pushnumber(L, t[i].x * vid_conwidth.value); lua_setfield(L, -2, "x");
		lua_pushnumber(L, t[i].y * vid_conheight.value); lua_setfield(L, -2, "y");
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

static int LuaCl_touch_capture(lua_State *L)
{
	cl_lua_touch_captured = lua_toboolean(L, 1);
	return 0;
}

static int LuaCl_touchscreen(lua_State *L)
{
	lua_pushboolean(L, vid_touchscreen.integer != 0);
	return 1;
}

// "touch" when a touchscreen exists and was used last, else "desktop"
static int LuaCl_input_mode(lua_State *L)
{
	lua_pushstring(L, (VID_HasTouch() && VID_InputIsTouch()) || vid_touchscreen.integer ? "touch" : "desktop");
	return 1;
}

static int LuaCl_has_touch(lua_State *L)
{
	lua_pushboolean(L, VID_HasTouch());
	return 1;
}

// console commands a mod may run: the ones an on-screen control needs
static int LuaCl_command(lua_State *L)
{
	static const char *allowed[] =
	{
		"togglemenu", "toggleconsole", "messagemode", "messagemode2",
		"+showscores", "-showscores", "+zoom", "-zoom",
		"weapnext", "weapprev", "weaplast", "weapbest", "impulse",
		"+hook", "-hook", "+use", "-use", "dropweapon", "reload",
		"quickmenu", NULL
	};
	const char *text = luaL_checkstring(L, 1);
	const char *p;
	char first[64];
	size_t len;
	int i;
	if (strpbrk(text, ";\n\r\"$"))
		return luaL_error(L, "xon.command: one plain command only");
	for (p = text; *p && !ISWHITESPACE(*p); p++)
		;
	len = (size_t)(p - text);
	if (len >= sizeof(first))
		return luaL_error(L, "xon.command: unknown command");
	memcpy(first, text, len);
	first[len] = 0;
	for (i = 0; allowed[i]; i++)
		if (!strcmp(first, allowed[i]))
			break;
	if (!allowed[i])
		return luaL_error(L, "xon.command: '%s' is not allowed", first);
	Cbuf_AddText(text);
	Cbuf_AddText("\n");
	return 0;
}

//============================================================================
// drawing (inside the HUD hook only)

static cachepic_t *cl_lua_disc, *cl_lua_ring;

static cachepic_t *CL_Lua_MakeCircle(const char *name, float inner)
{
	enum { S = 128 };
	unsigned char *pixels = (unsigned char *)Mem_Alloc(tempmempool, S * S * 4);
	cachepic_t *pic;
	int x, y;
	for (y = 0; y < S; y++)
	{
		for (x = 0; x < S; x++)
		{
			float dx = (x + 0.5f) / (S / 2) - 1, dy = (y + 0.5f) / (S / 2) - 1;
			float r = sqrt(dx * dx + dy * dy) * (S / 2);
			float a = bound(0, (S / 2) - r, 1); // 1px soft outer edge
			if (inner > 0)
				a *= bound(0, r - inner * (S / 2), 1);
			pixels[(y * S + x) * 4 + 0] = 255;
			pixels[(y * S + x) * 4 + 1] = 255;
			pixels[(y * S + x) * 4 + 2] = 255;
			pixels[(y * S + x) * 4 + 3] = (unsigned char)(a * 255);
		}
	}
	pic = Draw_NewPic(name, S, S, true, pixels);
	Mem_Free(pixels);
	return pic;
}

static void CL_Lua_CheckDrawing(lua_State *L)
{
	if (!cl_lua_drawing)
		luaL_error(L, "drawing is only possible inside a HUD hook");
}

static void CL_Lua_Color(lua_State *L, int idx, float *rgba)
{
	rgba[0] = (float)luaL_optnumber(L, idx, 1);
	rgba[1] = (float)luaL_optnumber(L, idx + 1, 1);
	rgba[2] = (float)luaL_optnumber(L, idx + 2, 1);
	rgba[3] = (float)luaL_optnumber(L, idx + 3, 1);
}

// xon.draw_fill(x, y, w, h [, r, g, b, a])
static int LuaCl_draw_fill(lua_State *L)
{
	float c[4];
	CL_Lua_CheckDrawing(L);
	CL_Lua_Color(L, 5, c);
	DrawQ_Fill(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3), luaL_checknumber(L, 4), c[0], c[1], c[2], c[3], 0);
	return 0;
}

// xon.draw_pic(path, x, y, w, h [, r, g, b, a]); true when the image exists
static int LuaCl_draw_pic(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	cachepic_t *pic;
	float c[4];
	CL_Lua_CheckDrawing(L);
	CL_Lua_Color(L, 6, c);
	pic = Draw_CachePic_Flags(path, CACHEPICFLAG_NOTPERSISTENT | CACHEPICFLAG_QUIET);
	if (!pic || !pic->tex || pic->tex == r_texture_notexture)
	{
		lua_pushboolean(L, false);
		return 1;
	}
	DrawQ_Pic(luaL_checknumber(L, 2), luaL_checknumber(L, 3), pic, luaL_checknumber(L, 4), luaL_checknumber(L, 5), c[0], c[1], c[2], c[3], 0);
	lua_pushboolean(L, true);
	return 1;
}

static void CL_Lua_DrawRound(lua_State *L, qboolean ring)
{
	float x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2), r = luaL_checknumber(L, 3);
	float c[4];
	CL_Lua_CheckDrawing(L);
	CL_Lua_Color(L, 4, c);
	if (!cl_lua_disc)
		cl_lua_disc = CL_Lua_MakeCircle("insanotic/disc", 0);
	if (!cl_lua_ring)
		cl_lua_ring = CL_Lua_MakeCircle("insanotic/ring", 0.88f);
	DrawQ_Pic(x - r, y - r, ring ? cl_lua_ring : cl_lua_disc, r * 2, r * 2, c[0], c[1], c[2], c[3], 0);
}

// xon.draw_circle(x, y, radius [, r, g, b, a]) filled disc
static int LuaCl_draw_circle(lua_State *L)
{
	CL_Lua_DrawRound(L, false);
	return 0;
}

// xon.draw_ring(x, y, radius [, r, g, b, a]) outline, about radius/8 wide
static int LuaCl_draw_ring(lua_State *L)
{
	CL_Lua_DrawRound(L, true);
	return 0;
}

// xon.draw_text(text, x, y, size [, r, g, b, a]); returns the width
static int LuaCl_draw_text(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	float size = luaL_checknumber(L, 4);
	float c[4];
	CL_Lua_CheckDrawing(L);
	CL_Lua_Color(L, 5, c);
	lua_pushnumber(L, DrawQ_String(luaL_checknumber(L, 2), luaL_checknumber(L, 3), text, 0, size, size, c[0], c[1], c[2], c[3], 0, NULL, false, FONT_INFOBAR));
	return 1;
}

static int LuaCl_text_width(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	float size = luaL_checknumber(L, 2);
	lua_pushnumber(L, DrawQ_TextWidth(text, 0, size, size, false, FONT_INFOBAR));
	return 1;
}

static const luaL_Reg cl_luaxon_funcs[] =
{
	{"time", LuaCl_time},
	{"realtime", LuaCl_realtime},
	{"in_game", LuaCl_in_game},
	{"screen_size", LuaCl_screen_size},
	{"assist_allowed", LuaCl_assist_allowed},
	{"view", LuaCl_view},
	{"traceline", LuaCl_traceline},
	{"teamplay", LuaCl_teamplay},
	{"players", LuaCl_players},
	{"me", LuaCl_me},
	{"touches", LuaCl_touches},
	{"touch_capture", LuaCl_touch_capture},
	{"touchscreen", LuaCl_touchscreen},
	{"input_mode", LuaCl_input_mode},
	{"has_touch", LuaCl_has_touch},
	{"command", LuaCl_command},
	{"draw_fill", LuaCl_draw_fill},
	{"draw_pic", LuaCl_draw_pic},
	{"draw_circle", LuaCl_draw_circle},
	{"draw_ring", LuaCl_draw_ring},
	{"draw_text", LuaCl_draw_text},
	{"text_width", LuaCl_text_width},
	{NULL, NULL}
};

static void CL_Lua_RegisterAPI(lua_State *L)
{
	static const struct { const char *name; int bit; } buttons[] =
	{
		{"BUTTON_ATTACK", 1}, {"BUTTON_JUMP", 2}, {"BUTTON_ATTACK2", 4},
		{"BUTTON_ZOOM", 8}, {"BUTTON_CROUCH", 16}, {"BUTTON_HOOK", 32},
		{"BUTTON_USE", 256}, {NULL, 0}
	};
	int i;
	luaL_setfuncs(L, cl_luaxon_funcs, 0);
	for (i = 0; buttons[i].name; i++)
	{
		lua_pushinteger(L, buttons[i].bit);
		lua_setfield(L, -2, buttons[i].name);
	}
}

//============================================================================
// console commands

static void CL_Lua_Mods_f(void)
{
	LuaSandbox_PrintList(&cl_luahost);
}

static void CL_Lua_Reload_f(void)
{
	CL_Lua_CloseAll();
	Con_Printf("[lua] reloading mods\n");
	cl_lua_was_connected = false;
	CL_Lua_LoadAll();
	CL_Lua_Mods_f();
}

void CL_Lua_Init(void)
{
	LuaSandbox_Init();
	Cvar_RegisterVariable(&cl_lua);
	Cvar_RegisterVariable(&cl_lua_instruction_limit);
	Cvar_RegisterVariable(&cl_lua_memory_limit);
	Cvar_RegisterVariable(&cl_lua_touch_mouse);
	Cmd_AddCommand("lua_mods", CL_Lua_Mods_f, "list client Lua mods and their status");
	Cmd_AddCommand("lua_reload", CL_Lua_Reload_f, "reload every client Lua mod");
}

void CL_Lua_Shutdown(void)
{
	CL_Lua_CloseAll();
}
