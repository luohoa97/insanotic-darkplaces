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
cvar_t cl_lua_disabled = {CVAR_SAVE, "cl_lua_disabled", "", "space separated list of client mod folder names not to load (mods/<name>)"};
cvar_t cl_lua_touch_mouse = {0, "cl_lua_touch_mouse", "0", "development aid: the left mouse button acts as a finger on the touchscreen while a mod captures touch input"};

qboolean cl_lua_touch_captured;

static void CL_Lua_RegisterAPI(lua_State *L);

static luahost_t cl_luahost =
{
	"mod", "mods", "mod.json", "game",
	&cl_lua_instruction_limit, &cl_lua_memory_limit, CVAR_SAVE, // mod settings persist in config.cfg
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

// is name listed in cl_lua_disabled?
static qboolean CL_Lua_IsDisabled(const char *name)
{
	const char *p = cl_lua_disabled.string;
	size_t len = strlen(name);
	while (*p)
	{
		while (*p && ISWHITESPACE(*p))
			p++;
		if (!strncmp(p, name, len) && (p[len] == 0 || ISWHITESPACE(p[len])))
			return true;
		while (*p && !ISWHITESPACE(*p))
			p++;
	}
	return false;
}

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
			if (CL_Lua_IsDisabled(dir + 5)) // after "mods/"
			{
				Con_Printf("[lua] mod %s is disabled (cl_lua_disabled)\n", dir + 5);
				continue;
			}
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
	// world geometry only: networked brush entities include see-through
	// forcefields and doors players walk through, which shots pass too
	trace = CL_TraceLine(v1, v2, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, MATERIALFLAGMASK_TRANSLUCENT, 0, false, false, NULL, false, false);
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

static qboolean CL_Lua_PlayerInfo(int entnum, int *team, qboolean *dead, qboolean *spectator);

static void CL_Lua_PushPlayer(lua_State *L, int entnum, const vec3_t origin, const vec3_t mins, const vec3_t maxs, const vec3_t velocity, qboolean alive)
{
	int colors = cl.scores ? cl.scores[entnum - 1].colors : 0;
	int team;
	qboolean dead, spectator, known = CL_Lua_PlayerInfo(entnum, &team, &dead, &spectator);
	if (known)
		alive = !dead && !spectator;
	lua_createtable(L, 0, 9);
	lua_pushinteger(L, entnum); lua_setfield(L, -2, "num");
	lua_pushstring(L, cl.scores ? cl.scores[entnum - 1].name : ""); lua_setfield(L, -2, "name");
	LuaSandbox_PushVec(L, origin); lua_setfield(L, -2, "origin");
	LuaSandbox_PushVec(L, mins); lua_setfield(L, -2, "mins");
	LuaSandbox_PushVec(L, maxs); lua_setfield(L, -2, "maxs");
	LuaSandbox_PushVec(L, velocity); lua_setfield(L, -2, "velocity");
	lua_pushinteger(L, colors & 15); lua_setfield(L, -2, "team_color");
	lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
	if (known)
	{
		// the game's own view: team number, death and spectating
		lua_pushinteger(L, team); lua_setfield(L, -2, "team");
		lua_pushboolean(L, dead); lua_setfield(L, -2, "dead");
		lua_pushboolean(L, spectator); lua_setfield(L, -2, "spectator");
	}
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
			if (prog->fieldoffsets.angles >= 0)
			{
				// where the model faces (yaw; model pitch is a third of the view's)
				LuaSandbox_PushVec(L, PRVM_clientedictvector(ed, angles));
				lua_setfield(L, -2, "angles");
			}
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

//============================================================================
// weapons, ammo and projectiles from the loaded CSQC
//
// Same binding as Xonotic Touch's touch_hud.c: Xonotic numbers its weapons
// and stats registries per build, so ids are read from whatever CSQC is
// loaded: STAT_WEAPONS.m_id, the weapon list from Weapons_first along
// .enemy, the held weapon from viewmodels[0].activeweapon/switchweapon.

#define CL_LUA_MAXWEAPONS 72
typedef struct cl_lua_weapon_s
{
	char name[32];
	int id;
	int ammo_stat; // -1: no ammo needed
} cl_lua_weapon_t;

static struct
{
	double bound_starttime;
	qboolean bound;
	cl_lua_weapon_t weapons[CL_LUA_MAXWEAPONS];
	int numweapons;
	int stat_weapons;
	int viewmodels_ofs;
	int f_id, f_activeweapon, f_switchweapon, f_draw;
	int f_id_type;
	int fn_projectile_draw;
	// Xonotic's own per-player state (common/ent_cs.qh), -1 when missing
	int fn_team, fn_dead, fn_spec;
	int stat_damage_dealt; // STAT_HITSOUND_DAMAGE_DEALT_TOTAL, -1 when missing
} cl_lua_csqc;

static int CL_Lua_FieldOfs(prvm_prog_t *prog, const char *name, int *type)
{
	ddef_t *d = PRVM_ED_FindField(prog, name);
	if (type)
		*type = d ? (d->type & ~DEF_SAVEGLOBAL) : ev_void;
	return d ? (int)d->ofs : -1;
}

static int CL_Lua_FieldInt(prvm_prog_t *prog, prvm_edict_t *ed, int ofs, int type)
{
	if (ofs < 0)
		return 0;
	return type == ev_float ? (int)PRVM_EDICTFIELDFLOAT(ed, ofs) : PRVM_EDICTFIELDVALUE(ed, ofs)->_int;
}

static prvm_edict_t *CL_Lua_EdictOrNull(prvm_prog_t *prog, int n)
{
	if (n <= 0 || n >= prog->num_edicts)
		return NULL;
	return PRVM_EDICT_NUM(n);
}

static prvm_edict_t *CL_Lua_GlobalEdict(prvm_prog_t *prog, const char *name)
{
	ddef_t *g = PRVM_ED_FindGlobal(prog, name);
	if (!g || (g->type & ~DEF_SAVEGLOBAL) != ev_entity)
		return NULL;
	return CL_Lua_EdictOrNull(prog, PRVM_GLOBALFIELDEDICT(g->ofs));
}

static qboolean CL_Lua_BindCSQC(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ed;
	ddef_t *g;
	mfunction_t *fn;
	int f_impulse, f_next, f_name, f_ammo, f_nametype, guard, stat_fuel = -1, stat_plasma = -1, res_none = 0, type;
	if (!prog->loaded)
	{
		cl_lua_csqc.bound = false;
		return false;
	}
	if (cl_lua_csqc.bound && cl_lua_csqc.bound_starttime == prog->starttime)
		return true;
	memset(&cl_lua_csqc, 0, sizeof(cl_lua_csqc));
	cl_lua_csqc.bound = true;
	cl_lua_csqc.bound_starttime = prog->starttime;
	cl_lua_csqc.stat_weapons = -1;
	cl_lua_csqc.viewmodels_ofs = -1;
	cl_lua_csqc.f_id = CL_Lua_FieldOfs(prog, "m_id", &cl_lua_csqc.f_id_type);
	cl_lua_csqc.f_activeweapon = CL_Lua_FieldOfs(prog, "activeweapon", NULL);
	cl_lua_csqc.f_switchweapon = CL_Lua_FieldOfs(prog, "switchweapon", NULL);
	cl_lua_csqc.f_draw = CL_Lua_FieldOfs(prog, "draw", NULL);
	fn = PRVM_ED_FindFunction(prog, "Projectile_Draw");
	cl_lua_csqc.fn_projectile_draw = fn ? (int)(fn - prog->functions) : -1;
	fn = PRVM_ED_FindFunction(prog, "entcs_GetTeam");
	cl_lua_csqc.fn_team = fn ? (int)(fn - prog->functions) : -1;
	fn = PRVM_ED_FindFunction(prog, "entcs_IsDead");
	cl_lua_csqc.fn_dead = fn ? (int)(fn - prog->functions) : -1;
	fn = PRVM_ED_FindFunction(prog, "entcs_GetSpecState");
	cl_lua_csqc.fn_spec = fn ? (int)(fn - prog->functions) : -1;
	f_impulse = CL_Lua_FieldOfs(prog, "impulse", NULL);
	f_next = CL_Lua_FieldOfs(prog, "enemy", NULL);
	f_name = CL_Lua_FieldOfs(prog, "netname", &f_nametype);
	f_ammo = CL_Lua_FieldOfs(prog, "ammo_type", NULL);
	if (cl_lua_csqc.f_id < 0 || f_next < 0 || f_name < 0)
		return true;
	if ((ed = CL_Lua_GlobalEdict(prog, "STAT_WEAPONS")))
	{
		cl_lua_csqc.stat_weapons = CL_Lua_FieldInt(prog, ed, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type);
		if (cl_lua_csqc.stat_weapons < 0 || cl_lua_csqc.stat_weapons + 2 >= MAX_CL_STATS)
			cl_lua_csqc.stat_weapons = -1;
	}
	cl_lua_csqc.stat_damage_dealt = -1;
	if ((ed = CL_Lua_GlobalEdict(prog, "STAT_HITSOUND_DAMAGE_DEALT_TOTAL")))
	{
		cl_lua_csqc.stat_damage_dealt = CL_Lua_FieldInt(prog, ed, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type);
		if (cl_lua_csqc.stat_damage_dealt < 0 || cl_lua_csqc.stat_damage_dealt >= MAX_CL_STATS)
			cl_lua_csqc.stat_damage_dealt = -1;
	}
	if ((ed = CL_Lua_GlobalEdict(prog, "STAT_FUEL")))
		stat_fuel = CL_Lua_FieldInt(prog, ed, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type);
	if ((ed = CL_Lua_GlobalEdict(prog, "STAT_PLASMA")))
		stat_plasma = CL_Lua_FieldInt(prog, ed, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type);
	if ((ed = CL_Lua_GlobalEdict(prog, "RES_NONE")))
		res_none = PRVM_NUM_FOR_EDICT(ed);
	g = PRVM_ED_FindGlobal(prog, "viewmodels");
	if (g && (g->type & ~DEF_SAVEGLOBAL) == ev_entity)
		cl_lua_csqc.viewmodels_ofs = g->ofs;
	(void)f_impulse;
	for (ed = CL_Lua_GlobalEdict(prog, "Weapons_first"), guard = 0; ed && guard < 256 && cl_lua_csqc.numweapons < CL_LUA_MAXWEAPONS; guard++)
	{
		int id = CL_Lua_FieldInt(prog, ed, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type);
		if (id > 0)
		{
			cl_lua_weapon_t *w = &cl_lua_csqc.weapons[cl_lua_csqc.numweapons++];
			w->id = id;
			strlcpy(w->name, PRVM_GetString(prog, PRVM_EDICTFIELDSTRING(ed, f_name)), sizeof(w->name));
			w->ammo_stat = -1;
			if (f_ammo >= 0)
			{
				int res = PRVM_EDICTFIELDEDICT(ed, f_ammo);
				prvm_edict_t *re = res != res_none ? CL_Lua_EdictOrNull(prog, res) : NULL;
				if (re)
				{
					const char *kind = PRVM_GetString(prog, PRVM_EDICTFIELDSTRING(re, f_name));
					if (!strcmp(kind, "shells")) w->ammo_stat = STAT_SHELLS;
					else if (!strcmp(kind, "bullets")) w->ammo_stat = STAT_NAILS;
					else if (!strcmp(kind, "rockets")) w->ammo_stat = STAT_ROCKETS;
					else if (!strcmp(kind, "cells")) w->ammo_stat = STAT_CELLS;
					else if (!strcmp(kind, "fuel")) w->ammo_stat = stat_fuel;
					else if (!strcmp(kind, "plasma")) w->ammo_stat = stat_plasma;
					if (w->ammo_stat >= MAX_CL_STATS)
						w->ammo_stat = -1;
				}
			}
		}
		ed = CL_Lua_EdictOrNull(prog, PRVM_EDICTFIELDEDICT(ed, f_next));
	}
	(void)type;
	return true;
}

// calls a CSQC function taking one number and returning one
static float CL_Lua_CallCSQC1(int fnum, float arg)
{
	prvm_prog_t *prog = CLVM_prog;
	PRVM_G_FLOAT(OFS_PARM0) = arg;
	prog->ExecuteProgram(prog, fnum, "Insanotic: CSQC function missing");
	return PRVM_G_FLOAT(OFS_RETURN);
}

// what the game itself says about a player (entnum 1..maxclients): team
// (Xonotic's team number, 0 = none, NUM_SPECTATOR for spectators), dead,
// spectator. Returns false when the loaded CSQC does not offer it.
static qboolean CL_Lua_PlayerInfo(int entnum, int *team, qboolean *dead, qboolean *spectator)
{
	if (!CL_Lua_BindCSQC() || cl_lua_csqc.fn_team < 0)
		return false;
	*team = (int)CL_Lua_CallCSQC1(cl_lua_csqc.fn_team, entnum - 1);
	*dead = cl_lua_csqc.fn_dead >= 0 && CL_Lua_CallCSQC1(cl_lua_csqc.fn_dead, entnum - 1) != 0;
	*spectator = cl_lua_csqc.fn_spec >= 0 && (int)CL_Lua_CallCSQC1(cl_lua_csqc.fn_spec, entnum - 1) == 1; // ENTCS_SPEC_PURE
	return true;
}

static qboolean CL_Lua_WeaponOwned(const cl_lua_weapon_t *w)
{
	int bit = w->id - 1, s = cl_lua_csqc.stat_weapons;
	if (s < 0 || bit < 0)
		return false;
	if (bit < 24)
		return (cl.stats[s] & (1 << bit)) != 0;
	if (bit < 48)
		return (cl.stats[s + 1] & (1 << (bit - 24))) != 0;
	return (cl.stats[s + 2] & (1 << (bit - 48))) != 0;
}

// id of the weapon in hand (or being switched to), 0 when unknown
static int CL_Lua_HeldWeaponId(qboolean switching)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve, *we;
	int f;
	if (!CL_Lua_BindCSQC() || cl_lua_csqc.viewmodels_ofs < 0)
		return 0;
	ve = CL_Lua_EdictOrNull(prog, PRVM_GLOBALFIELDEDICT(cl_lua_csqc.viewmodels_ofs));
	f = switching ? cl_lua_csqc.f_switchweapon : cl_lua_csqc.f_activeweapon;
	if (!ve || f < 0)
		return 0;
	we = CL_Lua_EdictOrNull(prog, PRVM_EDICTFIELDEDICT(ve, f));
	return we ? CL_Lua_FieldInt(prog, we, cl_lua_csqc.f_id, cl_lua_csqc.f_id_type) : 0;
}

// xon.weapons(): every weapon of the game with name, impulse (selects it),
// owned, ammo (nil when it needs none), held, switching_to
static int LuaCl_weapons(lua_State *L)
{
	int i, n = 0, held, target;
	lua_newtable(L);
	if (cls.state != ca_connected || !CL_Lua_BindCSQC())
		return 1;
	held = CL_Lua_HeldWeaponId(false);
	target = CL_Lua_HeldWeaponId(true);
	for (i = 0; i < cl_lua_csqc.numweapons; i++)
	{
		cl_lua_weapon_t *w = &cl_lua_csqc.weapons[i];
		lua_createtable(L, 0, 7);
		lua_pushstring(L, w->name); lua_setfield(L, -2, "name");
		lua_pushinteger(L, w->id); lua_setfield(L, -2, "id");
		lua_pushinteger(L, 230 + w->id - 1); lua_setfield(L, -2, "impulse");
		lua_pushboolean(L, CL_Lua_WeaponOwned(w)); lua_setfield(L, -2, "owned");
		if (w->ammo_stat >= 0)
		{
			lua_pushinteger(L, cl.stats[w->ammo_stat]);
			lua_setfield(L, -2, "ammo");
		}
		lua_pushboolean(L, w->id == held); lua_setfield(L, -2, "held");
		lua_pushboolean(L, w->id == target && target != held); lua_setfield(L, -2, "switching_to");
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// xon.held_weapon(): name of the weapon in hand, or nil
static int LuaCl_held_weapon(lua_State *L)
{
	int i, id;
	if (cls.state != ca_connected || !CL_Lua_BindCSQC())
		return 0;
	id = CL_Lua_HeldWeaponId(false);
	for (i = 0; i < cl_lua_csqc.numweapons; i++)
		if (cl_lua_csqc.weapons[i].id == id)
		{
			lua_pushstring(L, cl_lua_csqc.weapons[i].name);
			return 1;
		}
	return 0;
}

// xon.stats(): your health, armor and ammo reserves
static int LuaCl_stats(lua_State *L)
{
	lua_createtable(L, 0, 6);
	lua_pushinteger(L, cl.stats[STAT_HEALTH]); lua_setfield(L, -2, "health");
	lua_pushinteger(L, cl.stats[STAT_ARMOR]); lua_setfield(L, -2, "armor");
	lua_pushinteger(L, cl.stats[STAT_SHELLS]); lua_setfield(L, -2, "shells");
	lua_pushinteger(L, cl.stats[STAT_NAILS]); lua_setfield(L, -2, "bullets");
	lua_pushinteger(L, cl.stats[STAT_ROCKETS]); lua_setfield(L, -2, "rockets");
	lua_pushinteger(L, cl.stats[STAT_CELLS]); lua_setfield(L, -2, "cells");
	// total damage you have dealt (Xonotic's hit-sound counter), when known
	if (cls.state == ca_connected && CL_Lua_BindCSQC() && cl_lua_csqc.stat_damage_dealt >= 0)
	{
		lua_pushinteger(L, cl.stats[cl_lua_csqc.stat_damage_dealt]);
		lua_setfield(L, -2, "damage_dealt");
	}
	return 1;
}

// xon.projectiles(): projectiles in flight (Xonotic's CSQC projectiles):
// num, kind (the PROJECTILE_* number), origin, velocity
static int LuaCl_projectiles(lua_State *L)
{
	prvm_prog_t *prog = CLVM_prog;
	int i, n = 0, f_cnt, cnt_type;
	lua_newtable(L);
	if (cls.state != ca_connected || !CL_Lua_BindCSQC() || cl_lua_csqc.f_draw < 0 || cl_lua_csqc.fn_projectile_draw < 0)
		return 1;
	f_cnt = CL_Lua_FieldOfs(prog, "cnt", &cnt_type);
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		if (ed->priv.required->free || PRVM_EDICTFIELDFUNCTION(ed, cl_lua_csqc.f_draw) != cl_lua_csqc.fn_projectile_draw)
			continue;
		lua_createtable(L, 0, 4);
		lua_pushinteger(L, i); lua_setfield(L, -2, "num");
		lua_pushinteger(L, CL_Lua_FieldInt(prog, ed, f_cnt, cnt_type)); lua_setfield(L, -2, "kind");
		LuaSandbox_PushVec(L, PRVM_clientedictvector(ed, origin)); lua_setfield(L, -2, "origin");
		LuaSandbox_PushVec(L, PRVM_clientedictvector(ed, velocity)); lua_setfield(L, -2, "velocity");
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// xon.me(): your entity number and team colour (scoreboard pants colour)
static int LuaCl_me(lua_State *L)
{
	int team;
	qboolean dead, spectator;
	lua_pushinteger(L, cl.playerentity);
	if (cl.scores && cl.playerentity >= 1 && cl.playerentity <= cl.maxclients)
		lua_pushinteger(L, cl.scores[cl.playerentity - 1].colors & 15);
	else
		lua_pushnil(L);
	// third: your team as the game reports it (nil when unknown)
	if (cl.playerentity >= 1 && cl.playerentity <= cl.maxclients && CL_Lua_PlayerInfo(cl.playerentity, &team, &dead, &spectator))
		lua_pushinteger(L, team);
	else
		lua_pushnil(L);
	return 3;
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
	{"weapons", LuaCl_weapons},
	{"held_weapon", LuaCl_held_weapon},
	{"stats", LuaCl_stats},
	{"projectiles", LuaCl_projectiles},
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
	Cvar_RegisterVariable(&cl_lua_disabled);
	Cmd_AddCommand("lua_mods", CL_Lua_Mods_f, "list client Lua mods and their status");
	Cmd_AddCommand("lua_reload", CL_Lua_Reload_f, "reload every client Lua mod");
}

void CL_Lua_Shutdown(void)
{
	CL_Lua_CloseAll();
}
