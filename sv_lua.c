/*
Insanotic: server-side Lua plugins (XON_LUA extension).
SPDX-License-Identifier: GPL-3.0-or-later

Plugins live in plugins/<name>/init.lua and run inside the sandbox from
lua_sandbox.c (own lua_State each, instruction budget, memory cap, no io/os/
debug/package, require jailed to the plugin folder). This file adds the
server API: entity handles over the server progs, traceline, players, and
the mutator hook bridge.

The game code (mutator lua) forwards mutator hooks with lua_hook(). Hook
arguments live in the QC globals MUTATOR_ARGV_<n>_<type>; the second argument
of lua_hook is a signature string naming them, e.g. "e0 e1 f4 >f4":
  <type><n>   input parameter  (read before the handlers run)
  ><type><n>  output parameter (written back after the handlers ran)
  types: b=bool i=int f=float v=vector s=string e=entity
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "lua_sandbox.h"
#include "sv_lua.h"

cvar_t sv_lua_instruction_limit = {0, "sv_lua_instruction_limit", "2000000", "Lua VM instructions one server plugin may execute per entry (loading, or one hook call) before it is killed and disabled"};
cvar_t sv_lua_memory_limit = {0, "sv_lua_memory_limit", "32", "memory in MB one server plugin may allocate before it is killed and disabled"};

cvar_t sv_lua_disabled = {CF_SERVER | CF_ARCHIVE, "sv_lua_disabled", "", "space separated list of server plugin folder names not to load (plugins/<name>)"};

#define SV_LUA_QC_RELOAD "Lua_ReloadPlugins"

// is plugins/<name>/... listed in sv_lua_disabled?
static qbool SV_Lua_IsDisabled(const char *path)
{
	const char *name, *end, *p = sv_lua_disabled.string;
	size_t len;
	if (strncmp(path, "plugins/", 8))
		return false;
	name = path + 8;
	end = strchr(name, '/');
	len = end ? (size_t)(end - name) : strlen(name);
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

static void SV_Lua_RegisterAPI(lua_State *L);

static luahost_t sv_luahost =
{
	"plugin", "plugins", "plugin.json", "server",
	&sv_lua_instruction_limit, &sv_lua_memory_limit, 0,
	SV_Lua_RegisterAPI,
	{NULL}, 0, NULL
};

static mempool_t *sv_lua_mempool;

typedef struct luafield_s
{
	char *name;
	int ofs;      // -1: not a field
	int type;
	struct luafield_s *next;
} luafield_t;
#define LUA_HASHSIZE 256
static luafield_t *luafields[LUA_HASHSIZE];

typedef struct luastr_s
{
	char *s;
	int num;
	struct luastr_s *next;
} luastr_t;
static luastr_t *luastrings[LUA_HASHSIZE];

#define LUASIG_MAXPARMS 24
typedef struct luaparm_s
{
	char t;          // b i f v s e
	int idx;
	qbool in, out;
	int ofs;         // global offset, -1 when missing
	int etype;       // type of the global in progs
} luaparm_t;
typedef struct luasig_s
{
	char *key;
	int num;
	luaparm_t p[LUASIG_MAXPARMS];
	struct luasig_s *next;
} luasig_t;
static luasig_t *luasigs[LUA_HASHSIZE];

static prvm_prog_t *LuaMods_Prog(void)
{
	prvm_prog_t *prog = SVVM_prog;
	return prog->loaded ? prog : NULL;
}

void LuaMods_ProgReset(void)
{
	int i;
	if (!sv_lua_mempool)
		return;
	LuaSandbox_CloseAll(&sv_luahost);
	for (i = 0; i < LUA_HASHSIZE; i++)
	{
		while (luafields[i]) { luafield_t *f = luafields[i]; luafields[i] = f->next; Mem_Free(f->name); Mem_Free(f); }
		while (luastrings[i]) { luastr_t *e = luastrings[i]; luastrings[i] = e->next; Mem_Free(e->s); Mem_Free(e); }
		while (luasigs[i]) { luasig_t *g = luasigs[i]; luasigs[i] = g->next; Mem_Free(g->key); Mem_Free(g); }
	}
}

//============================================================================
// progs helpers

static luafield_t *LuaMods_Field(prvm_prog_t *prog, const char *name)
{
	unsigned int h = LuaSandbox_Hash(name, LUA_HASHSIZE);
	luafield_t *f;
	mdef_t *d;
	for (f = luafields[h]; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	f = (luafield_t *)Mem_Alloc(sv_lua_mempool, sizeof(*f));
	f->name = LuaSandbox_StrDup(sv_lua_mempool, name);
	d = PRVM_ED_FindField(prog, name);
	f->ofs = d ? (int)d->ofs : -1;
	f->type = d ? (d->type & ~DEF_SAVEGLOBAL) : ev_void;
	f->next = luafields[h];
	luafields[h] = f;
	return f;
}

// strings stored into entity fields must outlive the current frame; intern
// them for the lifetime of the progs so repeated writes do not leak
static int LuaMods_InternString(prvm_prog_t *prog, const char *s)
{
	unsigned int h = LuaSandbox_Hash(s, LUA_HASHSIZE);
	luastr_t *e;
	char *p;
	for (e = luastrings[h]; e; e = e->next)
		if (!strcmp(e->s, s))
			return e->num;
	e = (luastr_t *)Mem_Alloc(sv_lua_mempool, sizeof(*e));
	e->s = LuaSandbox_StrDup(sv_lua_mempool, s);
	e->num = PRVM_AllocString(prog, strlen(s) + 1, &p);
	memcpy(p, s, strlen(s) + 1);
	e->next = luastrings[h];
	luastrings[h] = e;
	return e->num;
}

static client_t *LuaMods_Client(int entnum)
{
	if (entnum < 1 || entnum > svs.maxclients || !svs.clients[entnum - 1].active)
		return NULL;
	return svs.clients + entnum - 1;
}

//============================================================================
// entity handles

typedef struct luaent_s
{
	int num;
	unsigned int serial;
} luaent_t;
#define LUAENT_MT "xon.entity"

static void LuaMods_PushEntity(lua_State *L, prvm_prog_t *prog, int num)
{
	prvm_edict_t *ed;
	luaent_t *e;
	if (!prog || num <= 0 || num >= prog->num_edicts)
	{
		lua_pushnil(L); // world / none
		return;
	}
	ed = PRVM_EDICT_NUM(num);
	if (ed->free)
	{
		lua_pushnil(L);
		return;
	}
	e = (luaent_t *)lua_newuserdatauv(L, sizeof(*e), 0);
	e->num = num;
	e->serial = ed->priv.required->serial;
	luaL_setmetatable(L, LUAENT_MT);
}

// returns NULL for a stale handle (entity freed, or slot reused since)
static prvm_edict_t *LuaMods_Edict(lua_State *L, int idx, prvm_prog_t **progout)
{
	luaent_t *e = (luaent_t *)luaL_checkudata(L, idx, LUAENT_MT);
	prvm_prog_t *prog = LuaMods_Prog();
	prvm_edict_t *ed;
	if (progout)
		*progout = prog;
	if (!prog || e->num <= 0 || e->num >= prog->num_edicts)
		return NULL;
	ed = PRVM_EDICT_NUM(e->num);
	if (ed->free || ed->priv.required->serial != e->serial)
		return NULL;
	return ed;
}

static prvm_edict_t *LuaMods_CheckLiveEdict(lua_State *L, int idx, prvm_prog_t **progout)
{
	prvm_edict_t *ed = LuaMods_Edict(L, idx, progout);
	if (!ed)
		luaL_error(L, "stale entity handle (entity was removed)");
	return ed;
}

// entity or nil -> edict number (0 = world); errors on stale handles
static int LuaMods_OptEntNum(lua_State *L, int idx)
{
	prvm_prog_t *prog;
	prvm_edict_t *ed;
	if (lua_isnoneornil(L, idx))
		return 0;
	ed = LuaMods_CheckLiveEdict(L, idx, &prog);
	return PRVM_NUM_FOR_EDICT(ed);
}

static void LuaMods_PushField(lua_State *L, prvm_prog_t *prog, prvm_edict_t *ed, luafield_t *f)
{
	switch (f->type)
	{
	case ev_float:
		lua_pushnumber(L, PRVM_EDICTFIELDFLOAT(ed, f->ofs));
		break;
	case ev_vector:
		LuaSandbox_PushVec(L, PRVM_EDICTFIELDVECTOR(ed, f->ofs));
		break;
	case ev_string:
		lua_pushstring(L, PRVM_GetString(prog, PRVM_EDICTFIELDSTRING(ed, f->ofs)));
		break;
	case ev_entity:
		LuaMods_PushEntity(L, prog, PRVM_EDICTFIELDEDICT(ed, f->ofs));
		break;
	default:
		lua_pushnil(L); // functions, fields, pointers are not exposed
		break;
	}
}

static int LuaMods_FieldFloat(prvm_prog_t *prog, prvm_edict_t *ed, const char *name, float *out)
{
	luafield_t *f = LuaMods_Field(prog, name);
	if (f->ofs < 0 || f->type != ev_float)
		return false;
	*out = PRVM_EDICTFIELDFLOAT(ed, f->ofs);
	return true;
}

static const char *LuaMods_FieldAlias(const char *key)
{
	if (!strcmp(key, "armor"))
		return "armorvalue";
	return key;
}

// name of the weapon in the player's first weapon slot, or nil
static void LuaMods_PushWeapon(lua_State *L, prvm_prog_t *prog, prvm_edict_t *ed)
{
	static const char *slotnames[] = {"weaponentities[0]", "weaponentities_0", "weaponentity", NULL};
	luafield_t *f = NULL, *w, *n;
	prvm_edict_t *went, *wep;
	int i, num;
	for (i = 0; slotnames[i]; i++)
	{
		f = LuaMods_Field(prog, slotnames[i]);
		if (f->ofs >= 0 && f->type == ev_entity)
			break;
		f = NULL;
	}
	w = LuaMods_Field(prog, "m_weapon");
	n = LuaMods_Field(prog, "netname");
	if (!f || w->ofs < 0 || n->ofs < 0)
	{
		lua_pushnil(L);
		return;
	}
	num = PRVM_EDICTFIELDEDICT(ed, f->ofs);
	if (num <= 0 || num >= prog->num_edicts)
	{
		lua_pushnil(L);
		return;
	}
	went = PRVM_EDICT_NUM(num);
	num = PRVM_EDICTFIELDEDICT(went, w->ofs);
	if (num <= 0 || num >= prog->num_edicts)
	{
		lua_pushnil(L);
		return;
	}
	wep = PRVM_EDICT_NUM(num);
	lua_pushstring(L, PRVM_GetString(prog, PRVM_EDICTFIELDSTRING(wep, n->ofs)));
}

static int LuaEnt_valid(lua_State *L)
{
	lua_pushboolean(L, LuaMods_Edict(L, 1, NULL) != NULL);
	return 1;
}

static int LuaEnt_num(lua_State *L)
{
	luaent_t *e = (luaent_t *)luaL_checkudata(L, 1, LUAENT_MT);
	lua_pushinteger(L, e->num);
	return 1;
}

static int LuaEnt_is_client(lua_State *L)
{
	prvm_edict_t *ed = LuaMods_Edict(L, 1, NULL);
	luaent_t *e = (luaent_t *)lua_touserdata(L, 1);
	lua_pushboolean(L, ed && LuaMods_Client(e->num) != NULL);
	return 1;
}

static int LuaEnt_is_bot(lua_State *L)
{
	prvm_edict_t *ed = LuaMods_Edict(L, 1, NULL);
	luaent_t *e = (luaent_t *)lua_touserdata(L, 1);
	client_t *c = ed ? LuaMods_Client(e->num) : NULL;
	lua_pushboolean(L, c && !c->netconnection);
	return 1;
}

static int LuaEnt_userinfo(lua_State *L)
{
	prvm_edict_t *ed = LuaMods_Edict(L, 1, NULL);
	luaent_t *e = (luaent_t *)lua_touserdata(L, 1);
	const char *key = luaL_checkstring(L, 2);
	client_t *c = ed ? LuaMods_Client(e->num) : NULL;
	char value[MAX_USERINFO_STRING];
	if (!c)
		return 0;
	InfoString_GetValue(c->userinfo, key, value, sizeof(value));
	if (!value[0])
		return 0;
	lua_pushstring(L, value);
	return 1;
}

static int LuaEnt_sprint(lua_State *L)
{
	prvm_edict_t *ed = LuaMods_CheckLiveEdict(L, 1, NULL);
	luaent_t *e = (luaent_t *)lua_touserdata(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	client_t *c = LuaMods_Client(e->num);
	(void)ed;
	if (c && c->netconnection)
	{
		MSG_WriteChar(&c->netconnection->message, svc_print);
		MSG_WriteString(&c->netconnection->message, msg);
	}
	return 0;
}

static int LuaEnt_centerprint(lua_State *L)
{
	prvm_edict_t *ed = LuaMods_CheckLiveEdict(L, 1, NULL);
	luaent_t *e = (luaent_t *)lua_touserdata(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	client_t *c = LuaMods_Client(e->num);
	(void)ed;
	if (c && c->netconnection)
	{
		MSG_WriteChar(&c->netconnection->message, svc_centerprint);
		MSG_WriteString(&c->netconnection->message, msg);
	}
	return 0;
}


//============================================================================
// pages: a plugin shows an RmlUi page in a client and hears back from it
//
// show_page sends "rml_show_server <path>" to the client; the client checks
// the path again and may refuse (cl_allow_server_pages 0). A button in the
// page answers with "rml_reply <page> <action> [name value]...", which the
// engine turns into the PageReply hook (see LuaMods_PageReply).

// a path a page may have: ui/<folder>/<name>.rml in the characters of a
// file name, no ".." and nothing a console line could be split by
static qbool LuaMods_PagePathOK(const char *path)
{
	size_t n = strlen(path);
	const char *c;
	if (n < 8 || n > 120 || strncmp(path, "ui/", 3) || strcmp(path + n - 4, ".rml") || strstr(path, ".."))
		return false;
	for (c = path; *c; c++)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.' || *c == '/'))
			return false;
	return true;
}

static void LuaMods_StuffClient(client_t *c, const char *text)
{
	if (c && c->netconnection)
	{
		MSG_WriteChar(&c->netconnection->message, svc_stufftext);
		MSG_WriteString(&c->netconnection->message, text);
	}
}

static int LuaEnt_show_page(lua_State *L)
{
	luaent_t *e;
	const char *path;
	char line[160];
	LuaMods_CheckLiveEdict(L, 1, NULL);
	e = (luaent_t *)lua_touserdata(L, 1);
	path = luaL_checkstring(L, 2);
	if (!LuaMods_PagePathOK(path))
		return luaL_error(L, "show_page: the path must be ui/<folder>/<name>.rml, got \"%s\"", path);
	dpsnprintf(line, sizeof(line), "rml_show_server %s\n", path);
	LuaMods_StuffClient(LuaMods_Client(e->num), line);
	return 0;
}

static int LuaEnt_close_page(lua_State *L)
{
	luaent_t *e;
	LuaMods_CheckLiveEdict(L, 1, NULL);
	e = (luaent_t *)lua_touserdata(L, 1);
	LuaMods_StuffClient(LuaMods_Client(e->num), "rml_close_server\n");
	return 0;
}

static const luaL_Reg luaent_methods[] =
{
	{"valid", LuaEnt_valid},
	{"num", LuaEnt_num},
	{"is_client", LuaEnt_is_client},
	{"is_bot", LuaEnt_is_bot},
	{"userinfo", LuaEnt_userinfo},
	{"sprint", LuaEnt_sprint},
	{"centerprint", LuaEnt_centerprint},
	{"show_page", LuaEnt_show_page},
	{"close_page", LuaEnt_close_page},
	{NULL, NULL}
};

static int LuaEnt_index(lua_State *L)
{
	prvm_prog_t *prog;
	prvm_edict_t *ed;
	const char *key = luaL_checkstring(L, 2);
	luafield_t *f;
	float deadflag, health;

	// methods first
	lua_getfield(L, LUA_REGISTRYINDEX, "xon.entity_methods");
	if (lua_getfield(L, -1, key) != LUA_TNIL)
		return 1;
	lua_pop(L, 2);

	ed = LuaMods_Edict(L, 1, &prog);
	if (!ed)
		return 0; // stale handles read as nil

	if (!strcmp(key, "alive"))
	{
		if (!LuaMods_FieldFloat(prog, ed, "deadflag", &deadflag))
			deadflag = 0;
		if (!LuaMods_FieldFloat(prog, ed, "health", &health))
			health = 1;
		lua_pushboolean(L, deadflag == 0 && health > 0);
		return 1;
	}
	if (!strcmp(key, "weapon"))
	{
		LuaMods_PushWeapon(L, prog, ed);
		return 1;
	}
	f = LuaMods_Field(prog, LuaMods_FieldAlias(key));
	if (f->ofs < 0)
		return 0;
	LuaMods_PushField(L, prog, ed, f);
	return 1;
}

static int LuaEnt_newindex(lua_State *L)
{
	prvm_prog_t *prog;
	prvm_edict_t *ed = LuaMods_CheckLiveEdict(L, 1, &prog);
	const char *key = luaL_checkstring(L, 2);
	luafield_t *f;
	vec3_t v;

	if (!strcmp(key, "alive") || !strcmp(key, "weapon"))
		return luaL_error(L, "entity field '%s' is read-only", key);
	f = LuaMods_Field(prog, LuaMods_FieldAlias(key));
	if (f->ofs < 0)
		return luaL_error(L, "entity has no field '%s'", key);
	switch (f->type)
	{
	case ev_float:
		if (lua_isboolean(L, 3))
			PRVM_EDICTFIELDFLOAT(ed, f->ofs) = lua_toboolean(L, 3);
		else
			PRVM_EDICTFIELDFLOAT(ed, f->ofs) = (float)luaL_checknumber(L, 3);
		break;
	case ev_vector:
		LuaSandbox_CheckVec(L, 3, v);
		VectorCopy(v, PRVM_EDICTFIELDVECTOR(ed, f->ofs));
		if (!strcmp(key, "origin"))
			SV_LinkEdict(ed);
		break;
	case ev_string:
		PRVM_EDICTFIELDSTRING(ed, f->ofs) = LuaMods_InternString(prog, luaL_checkstring(L, 3));
		break;
	case ev_entity:
		PRVM_EDICTFIELDEDICT(ed, f->ofs) = LuaMods_OptEntNum(L, 3);
		break;
	default:
		return luaL_error(L, "entity field '%s' cannot be written from Lua", key);
	}
	return 0;
}

static int LuaEnt_eq(lua_State *L)
{
	luaent_t *a = (luaent_t *)luaL_checkudata(L, 1, LUAENT_MT);
	luaent_t *b = (luaent_t *)luaL_checkudata(L, 2, LUAENT_MT);
	lua_pushboolean(L, a->num == b->num && a->serial == b->serial);
	return 1;
}

static int LuaEnt_tostring(lua_State *L)
{
	luaent_t *e = (luaent_t *)luaL_checkudata(L, 1, LUAENT_MT);
	lua_pushfstring(L, "entity %d%s", e->num, LuaMods_Edict(L, 1, NULL) ? "" : " (stale)");
	return 1;
}

//============================================================================
// xon.* API

static int LuaXon_time(lua_State *L)
{
	lua_pushnumber(L, sv.time);
	return 1;
}

static int LuaXon_traceline(lua_State *L)
{
	prvm_prog_t *prog = LuaMods_Prog();
	vec3_t v1, v2, normal, endpos;
	trace_t trace;
	int move = MOVE_NORMAL;
	prvm_edict_t *ignore;
	if (!prog || !sv.active)
		return luaL_error(L, "no server running");
	LuaSandbox_CheckVec(L, 1, v1);
	LuaSandbox_CheckVec(L, 2, v2);
	if (lua_isboolean(L, 3))
		move = lua_toboolean(L, 3) ? MOVE_NOMONSTERS : MOVE_NORMAL;
	else if (!lua_isnoneornil(L, 3))
		move = (int)luaL_checkinteger(L, 3);
	ignore = PRVM_EDICT_NUM(LuaMods_OptEntNum(L, 4));
	trace = SV_TraceLine(v1, v2, move, ignore, SV_GenericHitSuperContentsMask(ignore), 0, 0, collision_extendtracelinelength.value);
	lua_createtable(L, 0, 6);
	lua_pushnumber(L, trace.fraction); lua_setfield(L, -2, "fraction");
	VectorCopy(trace.endpos, endpos);
	LuaSandbox_PushVec(L, endpos); lua_setfield(L, -2, "endpos");
	VectorCopy(trace.plane.normal, normal);
	LuaSandbox_PushVec(L, normal); lua_setfield(L, -2, "normal");
	lua_pushboolean(L, trace.startsolid); lua_setfield(L, -2, "startsolid");
	lua_pushboolean(L, trace.allsolid); lua_setfield(L, -2, "allsolid");
	LuaMods_PushEntity(L, prog, trace.ent ? PRVM_NUM_FOR_EDICT((prvm_edict_t *)trace.ent) : 0);
	lua_setfield(L, -2, "ent");
	return 1;
}

static int LuaXon_players(lua_State *L)
{
	prvm_prog_t *prog = LuaMods_Prog();
	int i, n = 0;
	lua_newtable(L);
	if (!prog)
		return 1;
	for (i = 0; i < svs.maxclients; i++)
	{
		if (!svs.clients[i].active)
			continue;
		LuaMods_PushEntity(L, prog, i + 1);
		if (lua_isnil(L, -1))
			lua_pop(L, 1);
		else
			lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int LuaXon_find(lua_State *L)
{
	prvm_prog_t *prog = LuaMods_Prog();
	const char *classname = luaL_checkstring(L, 1);
	luafield_t *f;
	int i, n = 0;
	lua_newtable(L);
	if (!prog)
		return 1;
	f = LuaMods_Field(prog, "classname");
	if (f->ofs < 0 || f->type != ev_string)
		return 1;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		if (ed->free)
			continue;
		if (strcmp(PRVM_GetString(prog, PRVM_EDICTFIELDSTRING(ed, f->ofs)), classname))
			continue;
		LuaMods_PushEntity(L, prog, i);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int LuaXon_entity(lua_State *L)
{
	LuaMods_PushEntity(L, LuaMods_Prog(), (int)luaL_checkinteger(L, 1));
	return 1;
}

static const luaL_Reg sv_luaxon_funcs[] =
{
	{"time", LuaXon_time},
	{"traceline", LuaXon_traceline},
	{"players", LuaXon_players},
	{"find", LuaXon_find},
	{"entity", LuaXon_entity},
	{NULL, NULL}
};

// called by the sandbox with the xon table on top of the stack
static void SV_Lua_RegisterAPI(lua_State *L)
{
	luaL_setfuncs(L, sv_luaxon_funcs, 0);

	luaL_newmetatable(L, LUAENT_MT);
	lua_pushcfunction(L, LuaEnt_index); lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, LuaEnt_newindex); lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, LuaEnt_eq); lua_setfield(L, -2, "__eq");
	lua_pushcfunction(L, LuaEnt_tostring); lua_setfield(L, -2, "__tostring");
	lua_pushliteral(L, "entity"); lua_setfield(L, -2, "__name");
	lua_pushboolean(L, 0); lua_setfield(L, -2, "__metatable");
	lua_pop(L, 1);
	luaL_newlib(L, luaent_methods);
	lua_setfield(L, LUA_REGISTRYINDEX, "xon.entity_methods");
}

//============================================================================
// hook dispatch

typedef struct luaval_s
{
	float f;
	vec3_t v;
	int ent;
	char *s;
} luaval_t;

static const char *LuaMods_TypeName(char t)
{
	switch (t)
	{
	case 'b': return "bool";
	case 'i': return "int";
	case 'f': return "float";
	case 'v': return "vector";
	case 's': return "string";
	case 'e': return "entity";
	}
	return NULL;
}

static luasig_t *LuaMods_Signature(prvm_prog_t *prog, const char *sig)
{
	unsigned int h = LuaSandbox_Hash(sig, LUA_HASHSIZE);
	luasig_t *s;
	const char *p = sig;
	for (s = luasigs[h]; s; s = s->next)
		if (!strcmp(s->key, sig))
			return s;
	s = (luasig_t *)Mem_Alloc(sv_lua_mempool, sizeof(*s));
	s->key = LuaSandbox_StrDup(sv_lua_mempool, sig);
	while (*p)
	{
		qbool out = false;
		char t;
		int idx, i;
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (*p == '>')
		{
			out = true;
			p++;
		}
		t = *p++;
		idx = (int)strtol(p, (char **)&p, 10);
		if (!LuaMods_TypeName(t))
		{
			Con_Printf("^1[lua] bad hook signature \"%s\"^7\n", sig);
			break;
		}
		for (i = 0; i < s->num; i++)
			if (s->p[i].t == t && s->p[i].idx == idx)
				break;
		if (i == s->num)
		{
			char gname[64];
			mdef_t *d;
			if (s->num >= LUASIG_MAXPARMS)
				break;
			s->num++;
			s->p[i].t = t;
			s->p[i].idx = idx;
			dpsnprintf(gname, sizeof(gname), "MUTATOR_ARGV_%d_%s", idx, LuaMods_TypeName(t));
			d = PRVM_ED_FindGlobal(prog, gname);
			s->p[i].ofs = d ? (int)d->ofs : -1;
			s->p[i].etype = d ? (d->type & ~DEF_SAVEGLOBAL) : ev_void;
		}
		if (out)
			s->p[i].out = true;
		else
			s->p[i].in = true;
	}
	s->next = luasigs[h];
	luasigs[h] = s;
	return s;
}

static float LuaMods_GlobalNum(prvm_prog_t *prog, luaparm_t *p)
{
	if (p->etype == ev_float)
		return PRVM_GLOBALFIELDFLOAT(p->ofs);
	return (float)prog->globals.ip[p->ofs]; // integer globals (non-standard progs)
}

static void LuaMods_ReadGlobals(prvm_prog_t *prog, luasig_t *sig, luaval_t *vals)
{
	int i;
	for (i = 0; i < sig->num; i++)
	{
		luaparm_t *p = &sig->p[i];
		luaval_t *v = &vals[i];
		if (p->ofs < 0)
			continue;
		switch (p->t)
		{
		case 'b': case 'i': case 'f':
			v->f = LuaMods_GlobalNum(prog, p);
			break;
		case 'v':
			VectorCopy(PRVM_GLOBALFIELDVECTOR(p->ofs), v->v);
			break;
		case 'e':
			v->ent = PRVM_GLOBALFIELDEDICT(p->ofs);
			break;
		case 's':
		{
			v->s = LuaSandbox_StrDup(tempmempool, PRVM_GetString(prog, PRVM_GLOBALFIELDSTRING(p->ofs)));
			break;
		}
		}
	}
}

static void LuaMods_WriteGlobals(prvm_prog_t *prog, luasig_t *sig, luaval_t *vals)
{
	int i;
	for (i = 0; i < sig->num; i++)
	{
		luaparm_t *p = &sig->p[i];
		luaval_t *v = &vals[i];
		if (p->ofs < 0 || !p->out)
			continue;
		switch (p->t)
		{
		case 'b': case 'i': case 'f':
			if (p->etype == ev_float)
				PRVM_GLOBALFIELDFLOAT(p->ofs) = v->f;
			else
				prog->globals.ip[p->ofs] = (int)v->f;
			break;
		case 'v':
			VectorCopy(v->v, PRVM_GLOBALFIELDVECTOR(p->ofs));
			break;
		case 'e':
			PRVM_GLOBALFIELDEDICT(p->ofs) = v->ent;
			break;
		case 's':
			PRVM_GLOBALFIELDSTRING(p->ofs) = PRVM_SetTempString(prog, v->s ? v->s : "", v->s ? strlen(v->s) : 0);
			break;
		}
	}
}

static void LuaMods_PushVal(lua_State *L, prvm_prog_t *prog, luaparm_t *p, luaval_t *v)
{
	switch (p->t)
	{
	case 'b': lua_pushboolean(L, v->f != 0); break;
	case 'i': lua_pushinteger(L, (lua_Integer)v->f); break;
	case 'f': lua_pushnumber(L, v->f); break;
	case 'v': LuaSandbox_PushVec(L, v->v); break;
	case 'e': LuaMods_PushEntity(L, prog, v->ent); break;
	case 's': lua_pushstring(L, v->s ? v->s : ""); break;
	default: lua_pushnil(L); break;
	}
}

// reads ev[idx] back into v; values of the wrong type are ignored
static void LuaMods_PullVal(lua_State *L, prvm_prog_t *prog, int evidx, luaparm_t *p, luaval_t *v)
{
	lua_geti(L, evidx, p->idx);
	switch (p->t)
	{
	case 'b':
		v->f = lua_toboolean(L, -1);
		break;
	case 'i': case 'f':
		if (lua_isnumber(L, -1))
			v->f = (float)lua_tonumber(L, -1);
		break;
	case 'v':
		LuaSandbox_ToVec(L, -1, v->v);
		break;
	case 'e':
		if (lua_isnil(L, -1))
			v->ent = 0;
		else if (luaL_testudata(L, -1, LUAENT_MT))
		{
			prvm_edict_t *ed = LuaMods_Edict(L, lua_gettop(L), NULL);
			v->ent = ed ? PRVM_NUM_FOR_EDICT(ed) : 0;
		}
		break;
	case 's':
		if (lua_type(L, -1) == LUA_TSTRING)
		{
			const char *s = lua_tostring(L, -1);
			if (v->s)
				Mem_Free(v->s);
			v->s = LuaSandbox_StrDup(tempmempool, s);
		}
		break;
	}
	lua_pop(L, 1);
}

static qbool LuaMods_Dispatch(prvm_prog_t *prog, const char *hook, const char *sigstr)
{
	luasig_t *sig;
	luaval_t vals[LUASIG_MAXPARMS];
	qbool ret = false;
	int i, j, k;

	if (LuaSandbox_HookCount(&sv_luahost, hook) <= 0)
		return false;
	sig = LuaMods_Signature(prog, sigstr);
	memset(vals, 0, sizeof(vals));
	LuaMods_ReadGlobals(prog, sig, vals);

	for (i = 0; i < sv_luahost.num; i++)
	{
		luasandbox_t *m = sv_luahost.items[i];
		lua_State *L = m->L;
		int handlers, ev, n;
		n = LuaSandbox_PushHandlers(m, hook);
		if (!n)
			continue;
		handlers = lua_gettop(L);

		lua_createtable(L, sig->num, 1);
		ev = lua_gettop(L);
		lua_pushstring(L, hook);
		lua_setfield(L, ev, "hook");
		for (k = 0; k < sig->num; k++)
		{
			LuaMods_PushVal(L, prog, &sig->p[k], &vals[k]);
			lua_seti(L, ev, sig->p[k].idx);
		}

		for (j = 1; j <= n; j++)
		{
			lua_geti(L, handlers, j);
			lua_pushvalue(L, ev);
			if (!LuaSandbox_Call(m, 1, 1, hook))
				break; // state is gone
			if (lua_toboolean(L, -1))
				ret = true;
			lua_pop(L, 1);
		}
		if (m->disabled)
			continue;
		for (k = 0; k < sig->num; k++)
			if (sig->p[k].out)
				LuaMods_PullVal(L, prog, ev, &sig->p[k], &vals[k]);
		lua_settop(L, handlers - 1);
	}

	LuaMods_WriteGlobals(prog, sig, vals);
	for (k = 0; k < sig->num; k++)
		if (vals[k].s)
			Mem_Free(vals[k].s);
	return ret;
}


//============================================================================
// page replies

#define PAGEREPLY_MAXLEN 1024
#define PAGEREPLY_PERSECOND 20
#define PAGEREPLY_MAXWORDS 40

// Called for "rml_reply ..." from a client (sv_user.c). Returns true when a
// plugin listens to PageReply, so the line is not passed to the game code too.
qbool LuaMods_PageReply(client_t *c, const char *line)
{
	static double window[256];
	static int count[256];
	prvm_prog_t *prog = LuaMods_Prog();
	int slot = (int)(c - svs.clients);
	char words[PAGEREPLY_MAXWORDS][256];
	int i, j, argc;
	qbool handled = false;

	if (!prog || LuaSandbox_HookCount(&sv_luahost, "PageReply") <= 0)
		return false;
	if (slot < 0 || slot >= 256 || strlen(line) > PAGEREPLY_MAXLEN)
		return true; // too much to be a page; do not hand it on either
	// a page answers to a click, not to a loop: a client cannot flood a plugin
	if (host.realtime - window[slot] >= 1.0)
	{
		window[slot] = host.realtime;
		count[slot] = 0;
	}
	if (++count[slot] > PAGEREPLY_PERSECOND)
		return true;

	// words, quoted as the client wrote them; the first is "rml_reply"
	{
		const char *p = line;
		argc = 0;
		while (argc < PAGEREPLY_MAXWORDS && COM_ParseToken_Console(&p))
			dp_strlcpy(words[argc++], com_token, sizeof(words[0]));
	}
	if (argc < 3) // "rml_reply" page action
		return true;

	for (i = 0; i < sv_luahost.num; i++)
	{
		luasandbox_t *m = sv_luahost.items[i];
		lua_State *L = m->L;
		int handlers, ev, n;
		n = LuaSandbox_PushHandlers(m, "PageReply");
		if (!n)
			continue;
		handlers = lua_gettop(L);
		lua_createtable(L, 3, 4);
		ev = lua_gettop(L);
		lua_pushstring(L, "PageReply");
		lua_setfield(L, ev, "hook");
		LuaMods_PushEntity(L, prog, (int)(c - svs.clients) + 1);
		lua_seti(L, ev, 0);
		lua_pushstring(L, words[1]);
		lua_seti(L, ev, 1);
		lua_pushstring(L, words[2]);
		lua_seti(L, ev, 2);
		lua_newtable(L); // values by name
		for (j = 3; j + 1 < argc; j += 2)
		{
			lua_pushstring(L, words[j + 1]);
			lua_setfield(L, -2, words[j]);
		}
		lua_setfield(L, ev, "values");
		for (j = 1; j <= n; j++)
		{
			lua_geti(L, handlers, j);
			lua_pushvalue(L, ev);
			if (!LuaSandbox_Call(m, 1, 1, "PageReply"))
				break; // state is gone
			lua_pop(L, 1);
		}
		if (!m->disabled)
			lua_settop(L, handlers - 1);
	}
	handled = true;
	return handled;
}

//============================================================================
// builtins

void VM_SV_lua_load(prvm_prog_t *prog)
{
	luasandbox_t *m;
	VM_SAFEPARMCOUNT(1, VM_SV_lua_load);
	if (SV_Lua_IsDisabled(PRVM_G_STRING(OFS_PARM0)))
	{
		Con_Printf("[lua] plugin %s is disabled (sv_lua_disabled)\n", PRVM_G_STRING(OFS_PARM0));
		PRVM_G_FLOAT(OFS_RETURN) = 0;
		return;
	}
	m = LuaSandbox_Load(&sv_luahost, PRVM_G_STRING(OFS_PARM0));
	PRVM_G_FLOAT(OFS_RETURN) = (m && !m->disabled) ? 1 : 0;
}

void VM_SV_lua_hook(prvm_prog_t *prog)
{
	VM_SAFEPARMCOUNTRANGE(1, 8, VM_SV_lua_hook);
	PRVM_G_FLOAT(OFS_RETURN) = LuaMods_Dispatch(prog, PRVM_G_STRING(OFS_PARM0), prog->argc > 1 ? PRVM_G_STRING(OFS_PARM1) : "");
}

void VM_SV_lua_hookcount(prvm_prog_t *prog)
{
	VM_SAFEPARMCOUNT(1, VM_SV_lua_hookcount);
	PRVM_G_FLOAT(OFS_RETURN) = LuaSandbox_HookCount(&sv_luahost, PRVM_G_STRING(OFS_PARM0));
}

//============================================================================
// console commands

static void SV_Lua_Plugins_f(cmd_state_t *cmd)
{
	LuaSandbox_PrintList(&sv_luahost);
}

static void SV_Lua_Reload_f(cmd_state_t *cmd)
{
	prvm_prog_t *prog = LuaMods_Prog();
	char paths[LUASANDBOX_MAX][MAX_QPATH];
	int i, n;
	mfunction_t *f;

	if (!sv.active || !prog)
	{
		Con_Print("sv_lua_reload: no server running\n");
		return;
	}
	n = sv_luahost.num;
	for (i = 0; i < n; i++)
		dp_strlcpy(paths[i], sv_luahost.items[i]->path, sizeof(paths[i]));
	LuaSandbox_CloseAll(&sv_luahost);
	Con_Printf("[lua] reloading plugins\n");

	// let the game code rescan plugins/ (picks up new and removed plugins);
	// if it does not provide the function, reload the previous list in order
	f = PRVM_ED_FindFunction(prog, SV_LUA_QC_RELOAD);
	if (f)
	{
		PRVM_serverglobalfloat(time) = sv.time;
		PRVM_serverglobaledict(self) = 0;
		prog->ExecuteProgram(prog, f - prog->functions, SV_LUA_QC_RELOAD " missing");
	}
	else
		for (i = 0; i < n; i++)
			LuaSandbox_Load(&sv_luahost, paths[i]);
	SV_Lua_Plugins_f(cmd_local);
}

void LuaMods_Init(void)
{
	LuaSandbox_Init();
	sv_lua_mempool = Mem_AllocPool("lua plugins", 0, NULL);
	Cvar_RegisterVariable(&sv_lua_instruction_limit);
	Cvar_RegisterVariable(&sv_lua_memory_limit);
	Cvar_RegisterVariable(&sv_lua_disabled);
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugins", SV_Lua_Plugins_f, "list server Lua plugins and their status");
	Cmd_AddCommand(CF_SERVER, "sv_lua_reload", SV_Lua_Reload_f, "reload every server Lua plugin without restarting the map");
}
