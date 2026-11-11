/*
Insanotic: server-side Lua mod loader (XON_LUA extension).
SPDX-License-Identifier: GPL-3.0-or-later

Every mod gets its own lua_State with:
  - only the base, coroutine, table, string, math and utf8 libraries
    (io, os, debug and package are not even compiled in, see lua/LICENSE)
  - a require() that can only load .lua files from the mod's own folder in
    the virtual filesystem
  - an instruction budget per entry (load or hook call), enforced with a
    count hook; exceeding it kills and disables the mod
  - a memory cap enforced by the allocator
Any error raised out of a mod is logged and the mod is disabled; the server
keeps running.

The game code (mutator_lua) forwards mutator hooks with lua_hook(). Hook
arguments live in the QC globals MUTATOR_ARGV_<n>_<type>; the second argument
of lua_hook is a signature string naming them, e.g. "e0 e1 f4 >f4":
  <type><n>   input parameter  (read before the handlers run)
  ><type><n>  output parameter (written back after the handlers ran)
  types: b=bool i=int f=float v=vector s=string e=entity
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "sv_lua.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

cvar_t lua_instruction_limit = {0, "lua_instruction_limit", "2000000", "Lua VM instructions one mod may execute per entry (loading, or one hook call) before it is killed and disabled"};
cvar_t lua_memory_limit = {0, "lua_memory_limit", "32", "memory in MB one Lua mod may allocate before it is killed and disabled"};

#define LUAMOD_MAX 64
#define LUAMOD_HOOKSTEP 1000
#define LUAMOD_QC_RELOAD "Lua_ReloadMods"

typedef struct luamod_s
{
	char path[MAX_QPATH];      // mods/<dir>/init.lua
	char dir[MAX_QPATH];       // mods/<dir>
	char name[64];
	char version[32];
	lua_State *L;
	size_t mem;
	size_t memlimit;
	long budget;
	qboolean killed;           // sticky: every count hook raises once set
	qboolean disabled;
	char reason[256];
	int handlers;              // number of xon.on registrations
} luamod_t;

static luamod_t *luamods[LUAMOD_MAX];
static int luamods_num;

// hook name -> number of handlers over all enabled mods
typedef struct luahook_s
{
	char name[64];
	int count;
	struct luahook_s *next;
} luahook_t;
static luahook_t *luahooks;

// caches tied to the lifetime of the server progs
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
	qboolean in, out;
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

static mempool_t *lua_mempool;

static unsigned int LuaMods_Hash(const char *s)
{
	unsigned int h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h % LUA_HASHSIZE;
}

static char *LuaMods_StrDup(mempool_t *pool, const char *str)
{
	size_t len = strlen(str) + 1;
	char *d = (char *)Mem_Alloc(pool, len);
	memcpy(d, str, len);
	return d;
}

static prvm_prog_t *LuaMods_Prog(void)
{
	prvm_prog_t *prog = SVVM_prog;
	return prog->loaded ? prog : NULL;
}

//============================================================================
// mod lifetime

static luamod_t *LuaMods_FromState(lua_State *L)
{
	return *(luamod_t **)lua_getextraspace(L);
}

static void *LuaMods_Alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	luamod_t *m = (luamod_t *)ud;
	void *n;
	if (!ptr)
		osize = 0;
	if (nsize == 0)
	{
		free(ptr);
		m->mem -= osize;
		return NULL;
	}
	if (nsize > osize && m->memlimit && m->mem - osize + nsize > m->memlimit)
	{
		if (!m->killed)
		{
			m->killed = true;
			dpsnprintf(m->reason, sizeof(m->reason), "memory limit of %u KB exceeded", (unsigned int)(m->memlimit / 1024));
		}
		return NULL;
	}
	n = realloc(ptr, nsize);
	if (!n)
		return NULL;
	m->mem = m->mem - osize + nsize;
	return n;
}

static void LuaMods_CountHook(lua_State *L, lua_Debug *ar)
{
	luamod_t *m = LuaMods_FromState(L);
	(void)ar;
	m->budget -= LUAMOD_HOOKSTEP;
	if (m->budget <= 0 && !m->killed)
	{
		m->killed = true;
		dpsnprintf(m->reason, sizeof(m->reason), "instruction limit of %d exceeded (runaway script?)", lua_instruction_limit.integer);
	}
	if (m->killed)
		luaL_error(L, "%s", m->reason);
}

static luahook_t *LuaMods_FindHook(const char *name, qboolean create)
{
	luahook_t *h;
	for (h = luahooks; h; h = h->next)
		if (!strcmp(h->name, name))
			return h;
	if (!create)
		return NULL;
	h = (luahook_t *)Mem_Alloc(lua_mempool, sizeof(*h));
	strlcpy(h->name, name, sizeof(h->name));
	h->next = luahooks;
	luahooks = h;
	return h;
}

// removes the mod's handlers from the hook counts and closes its state
static void LuaMods_Disable(luamod_t *m, const char *reason)
{
	if (m->disabled)
		return;
	if (reason && !m->reason[0])
		strlcpy(m->reason, reason, sizeof(m->reason));
	m->disabled = true;
	if (m->L)
	{
		lua_State *L = m->L;
		lua_getfield(L, LUA_REGISTRYINDEX, "xon.handlers");
		if (lua_istable(L, -1))
		{
			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				if (lua_type(L, -2) == LUA_TSTRING)
				{
					luahook_t *h = LuaMods_FindHook(lua_tostring(L, -2), false);
					if (h)
						h->count = max(0, h->count - (int)lua_rawlen(L, -1));
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		// finalizers may still run during lua_close; keep them on a short leash
		m->killed = true;
		lua_close(L);
		m->L = NULL;
	}
	m->mem = 0;
	m->handlers = 0;
}

static void LuaMods_Crashed(luamod_t *m, const char *what, const char *err)
{
	Con_Printf("^1[lua] mod %s (%s) %s: %s^7\n", m->name, m->path, what, err ? err : "unknown error");
	if (!m->reason[0])
		dpsnprintf(m->reason, sizeof(m->reason), "%s: %s", what, err ? err : "unknown error");
	Con_Printf("^1[lua] mod %s disabled; the server keeps running^7\n", m->name);
	LuaMods_Disable(m, NULL);
}

static void LuaMods_CloseAll(void)
{
	int i;
	luahook_t *h, *next;
	for (i = 0; i < luamods_num; i++)
	{
		LuaMods_Disable(luamods[i], "unloaded");
		Mem_Free(luamods[i]);
		luamods[i] = NULL;
	}
	luamods_num = 0;
	for (h = luahooks; h; h = next)
	{
		next = h->next;
		Mem_Free(h);
	}
	luahooks = NULL;
}

void LuaMods_ProgReset(void)
{
	int i;
	if (!lua_mempool)
		return;
	LuaMods_CloseAll();
	for (i = 0; i < LUA_HASHSIZE; i++)
	{
		while (luafields[i]) { luafield_t *f = luafields[i]; luafields[i] = f->next; Mem_Free(f->name); Mem_Free(f); }
		while (luastrings[i]) { luastr_t *s = luastrings[i]; luastrings[i] = s->next; Mem_Free(s->s); Mem_Free(s); }
		while (luasigs[i]) { luasig_t *s = luasigs[i]; luasigs[i] = s->next; Mem_Free(s->key); Mem_Free(s); }
	}
}

//============================================================================
// progs helpers

static luafield_t *LuaMods_Field(prvm_prog_t *prog, const char *name)
{
	unsigned int h = LuaMods_Hash(name);
	luafield_t *f;
	ddef_t *d;
	for (f = luafields[h]; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	f = (luafield_t *)Mem_Alloc(lua_mempool, sizeof(*f));
	f->name = LuaMods_StrDup(lua_mempool, name);
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
	unsigned int h = LuaMods_Hash(s);
	luastr_t *e;
	char *p;
	for (e = luastrings[h]; e; e = e->next)
		if (!strcmp(e->s, s))
			return e->num;
	e = (luastr_t *)Mem_Alloc(lua_mempool, sizeof(*e));
	e->s = LuaMods_StrDup(lua_mempool, s);
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
// vectors

static void LuaMods_PushVec(lua_State *L, const vec3_t v)
{
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
	lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
	lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
	lua_getfield(L, LUA_REGISTRYINDEX, "xon.vec");
	lua_setmetatable(L, -2);
}

static qboolean LuaMods_ToVec(lua_State *L, int idx, vec3_t out)
{
	static const char *named[3] = {"x", "y", "z"};
	int i;
	idx = lua_absindex(L, idx);
	if (!lua_istable(L, idx))
		return false;
	for (i = 0; i < 3; i++)
	{
		if (lua_getfield(L, idx, named[i]) != LUA_TNUMBER)
		{
			lua_pop(L, 1);
			lua_geti(L, idx, i + 1);
		}
		if (!lua_isnumber(L, -1))
		{
			lua_pop(L, 1);
			return false;
		}
		out[i] = lua_tonumber(L, -1);
		lua_pop(L, 1);
	}
	return true;
}

static void LuaMods_CheckVec(lua_State *L, int idx, vec3_t out)
{
	if (!LuaMods_ToVec(L, idx, out))
		luaL_typeerror(L, idx, "vector");
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
	if (ed->priv.required->free)
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
	if (ed->priv.required->free || ed->priv.required->serial != e->serial)
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
		LuaMods_PushVec(L, PRVM_EDICTFIELDVECTOR(ed, f->ofs));
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

static const luaL_Reg luaent_methods[] =
{
	{"valid", LuaEnt_valid},
	{"num", LuaEnt_num},
	{"is_client", LuaEnt_is_client},
	{"is_bot", LuaEnt_is_bot},
	{"userinfo", LuaEnt_userinfo},
	{"sprint", LuaEnt_sprint},
	{"centerprint", LuaEnt_centerprint},
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
		LuaMods_CheckVec(L, 3, v);
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

static int LuaXon_on(lua_State *L)
{
	luamod_t *m = LuaMods_FromState(L);
	const char *hook = luaL_checkstring(L, 1);
	luahook_t *h;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (strlen(hook) >= sizeof(h->name))
		return luaL_error(L, "hook name too long");
	lua_getfield(L, LUA_REGISTRYINDEX, "xon.handlers");
	if (lua_getfield(L, -1, hook) == LUA_TNIL)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, hook);
	}
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	lua_pop(L, 2);
	h = LuaMods_FindHook(hook, true);
	h->count++;
	m->handlers++;
	return 0;
}

static int LuaXon_print(lua_State *L)
{
	luamod_t *m = LuaMods_FromState(L);
	luaL_Buffer b;
	int i, n = lua_gettop(L);
	luaL_buffinit(L, &b);
	for (i = 1; i <= n; i++)
	{
		if (i > 1)
			luaL_addchar(&b, '\t');
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	Con_Printf("^3[%s]^7 %s\n", m->name, lua_tostring(L, -1));
	return 0;
}

static cvar_t *LuaMods_CvarForRead(lua_State *L, const char *name)
{
	cvar_t *c = Cvar_FindVar(name);
	if (c && (c->flags & CVAR_PRIVATE))
		luaL_error(L, "cvar '%s' is private", name);
	return c;
}

static int LuaXon_cvar(lua_State *L)
{
	cvar_t *c = LuaMods_CvarForRead(L, luaL_checkstring(L, 1));
	if (!c)
		return 0;
	lua_pushnumber(L, c->value);
	return 1;
}

static int LuaXon_cvar_string(lua_State *L)
{
	cvar_t *c = LuaMods_CvarForRead(L, luaL_checkstring(L, 1));
	if (!c)
		return 0;
	lua_pushstring(L, c->string);
	return 1;
}

static int LuaXon_cvar_set(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *value = luaL_tolstring(L, 2, NULL);
	cvar_t *c = Cvar_FindVar(name);
	if (!c)
		return luaL_error(L, "no such cvar '%s' (use xon.cvar_register)", name);
	if (c->flags & (CVAR_PRIVATE | CVAR_READONLY))
		return luaL_error(L, "cvar '%s' is protected", name);
	Cvar_SetQuick(c, value);
	return 0;
}

static int LuaXon_cvar_register(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *def = luaL_tolstring(L, 2, NULL);
	const char *desc = luaL_optstring(L, 3, "registered by a Lua mod");
	cvar_t *c = Cvar_FindVar(name);
	if (!c)
		c = Cvar_Get(name, def, 0, desc);
	if (!c || (c->flags & CVAR_PRIVATE))
		return luaL_error(L, "cannot register cvar '%s'", name);
	lua_pushnumber(L, c->value);
	return 1;
}

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
	LuaMods_CheckVec(L, 1, v1);
	LuaMods_CheckVec(L, 2, v2);
	if (lua_isboolean(L, 3))
		move = lua_toboolean(L, 3) ? MOVE_NOMONSTERS : MOVE_NORMAL;
	else if (!lua_isnoneornil(L, 3))
		move = (int)luaL_checkinteger(L, 3);
	ignore = PRVM_EDICT_NUM(LuaMods_OptEntNum(L, 4));
	trace = SV_TraceLine(v1, v2, move, ignore, SV_GenericHitSuperContentsMask(ignore), 0, 0, collision_extendtracelinelength.value);
	lua_createtable(L, 0, 6);
	lua_pushnumber(L, trace.fraction); lua_setfield(L, -2, "fraction");
	VectorCopy(trace.endpos, endpos);
	LuaMods_PushVec(L, endpos); lua_setfield(L, -2, "endpos");
	VectorCopy(trace.plane.normal, normal);
	LuaMods_PushVec(L, normal); lua_setfield(L, -2, "normal");
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
		if (ed->priv.required->free)
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

static const luaL_Reg luaxon_funcs[] =
{
	{"on", LuaXon_on},
	{"print", LuaXon_print},
	{"cvar", LuaXon_cvar},
	{"cvar_string", LuaXon_cvar_string},
	{"cvar_set", LuaXon_cvar_set},
	{"cvar_register", LuaXon_cvar_register},
	{"time", LuaXon_time},
	{"traceline", LuaXon_traceline},
	{"players", LuaXon_players},
	{"find", LuaXon_find},
	{"entity", LuaXon_entity},
	{NULL, NULL}
};

//============================================================================
// sandbox

static int LuaInternal_killed(lua_State *L)
{
	lua_pushboolean(L, LuaMods_FromState(L)->killed);
	return 1;
}

// require() backend: module name (already validated) -> source, chunkname
static int LuaInternal_readsrc(lua_State *L)
{
	luamod_t *m = LuaMods_FromState(L);
	const char *name = luaL_checkstring(L, 1);
	char path[MAX_QPATH];
	char *p;
	unsigned char *buf;
	fs_offset_t size;
	if (dpsnprintf(path, sizeof(path), "%s/%s.lua", m->dir, name) >= (int)sizeof(path))
	{
		lua_pushnil(L);
		lua_pushstring(L, "path too long");
		return 2;
	}
	for (p = path + strlen(m->dir) + 1; *p; p++)
		if (*p == '.' && strcmp(p, ".lua"))
			*p = '/';
	buf = FS_LoadFile(path, tempmempool, true, &size);
	if (!buf)
	{
		lua_pushnil(L);
		lua_pushstring(L, path);
		return 2;
	}
	lua_pushlstring(L, (const char *)buf, (size_t)size);
	Mem_Free(buf);
	lua_pushfstring(L, "@%s", path);
	return 2;
}

// runs with the internal helper table as its only argument and returns the
// vector metatable; everything it does not keep is unreachable afterwards
static const char *luamods_prelude =
"local C = ...\n"
"local rawpcall, rawxpcall, rawload, killed = pcall, xpcall, load, C.killed\n"
"local readsrc = C.readsrc\n"
"-- a killed mod must not be able to swallow its own death\n"
"local function check(ok, ...)\n"
"  if not ok and killed() then error((...), 0) end\n"
"  return ok, ...\n"
"end\n"
"pcall = function(f, ...) return check(rawpcall(f, ...)) end\n"
"xpcall = function(f, h, ...) return check(rawxpcall(f, h, ...)) end\n"
"local resume, close = coroutine.resume, coroutine.close\n"
"coroutine.resume = function(co, ...) return check(resume(co, ...)) end\n"
"coroutine.close = function(co) return check(close(co)) end\n"
"-- text chunks only: no precompiled bytecode\n"
"load = function(chunk, name, mode, env)\n"
"  if env ~= nil then return rawload(chunk, name, 't', env) end\n"
"  return rawload(chunk, name, 't')\n"
"end\n"
"dofile, loadfile, string.dump = nil, nil, nil\n"
"print = xon.print\n"
"local loaded = {}\n"
"require = function(name)\n"
"  if type(name) ~= 'string' or not name:match('^[%w_]+$') and not name:match('^[%w_]+[%w_%.]*[%w_]$') or name:find('..', 1, true) then\n"
"    error(\"require: bad module name '\" .. tostring(name) .. \"'\", 2)\n"
"  end\n"
"  local v = loaded[name]\n"
"  if v ~= nil then return v end\n"
"  local src, chunk = readsrc(name)\n"
"  if not src then error(\"require: module '\" .. name .. \"' not found (\" .. chunk .. \")\", 2) end\n"
"  local f, err = rawload(src, chunk, 't')\n"
"  if not f then error(err, 2) end\n"
"  loaded[name] = true\n"
"  local r = f(name)\n"
"  if r == nil then r = true end\n"
"  loaded[name] = r\n"
"  return r\n"
"end\n"
"local vec = {}\n"
"vec.__index = vec\n"
"local function V(x, y, z) return setmetatable({x = x or 0, y = y or 0, z = z or 0}, vec) end\n"
"local function num(a) return type(a) == 'number' end\n"
"vec.__add = function(a, b) return V(a.x + b.x, a.y + b.y, a.z + b.z) end\n"
"vec.__sub = function(a, b) return V(a.x - b.x, a.y - b.y, a.z - b.z) end\n"
"vec.__unm = function(a) return V(-a.x, -a.y, -a.z) end\n"
"vec.__mul = function(a, b)\n"
"  if num(a) then return V(a * b.x, a * b.y, a * b.z) end\n"
"  if num(b) then return V(a.x * b, a.y * b, a.z * b) end\n"
"  return a.x * b.x + a.y * b.y + a.z * b.z\n"
"end\n"
"vec.__div = function(a, b) return V(a.x / b, a.y / b, a.z / b) end\n"
"vec.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end\n"
"vec.__tostring = function(a) return string.format(\"'%g %g %g'\", a.x, a.y, a.z) end\n"
"function vec:len() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end\n"
"function vec:normalize() local l = self:len() if l == 0 then return V() end return V(self.x / l, self.y / l, self.z / l) end\n"
"function vec:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end\n"
"function vec:cross(b) return V(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z, self.x * b.y - self.y * b.x) end\n"
"function vec:copy() return V(self.x, self.y, self.z) end\n"
"xon.vec = V\n"
"-- angles (pitch yaw roll, Quake convention) <-> direction\n"
"function xon.forward(ang)\n"
"  local p, y = math.rad(ang.x), math.rad(ang.y)\n"
"  return V(math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p))\n"
"end\n"
"function xon.vectoangles(d)\n"
"  local yaw = math.deg(math.atan(d.y, d.x))\n"
"  local pitch = math.deg(math.atan(-d.z, math.sqrt(d.x * d.x + d.y * d.y)))\n"
"  return V(pitch, yaw, 0)\n"
"end\n"
"return vec\n";

static void LuaMods_OpenLib(lua_State *L, const char *name, lua_CFunction f)
{
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
}

static int LuaMods_MsgHandler(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	if (!msg)
		msg = luaL_tolstring(L, 1, NULL);
	luaL_traceback(L, L, msg, 1);
	return 1;
}

// pcall with a fresh instruction budget; returns true on success. On failure
// the error is logged and the mod disabled (state closed!)
static qboolean LuaMods_Call(luamod_t *m, int nargs, int nresults, const char *what)
{
	lua_State *L = m->L;
	int base = lua_gettop(L) - nargs;
	int status;
	lua_pushcfunction(L, LuaMods_MsgHandler);
	lua_insert(L, base);
	m->budget = max(lua_instruction_limit.integer, LUAMOD_HOOKSTEP);
	status = lua_pcall(L, nargs, nresults, base);
	lua_remove(L, base);
	if (status != LUA_OK || m->killed)
	{
		char err[1024];
		strlcpy(err, m->killed ? m->reason : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error object is not a string"), sizeof(err));
		LuaMods_Crashed(m, what, err);
		return false;
	}
	return true;
}

static qboolean LuaMods_CreateState(luamod_t *m)
{
	lua_State *L;
	m->memlimit = (size_t)max(lua_memory_limit.value, 1) * 1024 * 1024;
	L = lua_newstate(LuaMods_Alloc, m);
	if (!L)
		return false;
	m->L = L;
	*(luamod_t **)lua_getextraspace(L) = m;
	lua_sethook(L, LuaMods_CountHook, LUA_MASKCOUNT, LUAMOD_HOOKSTEP);
	m->budget = max(lua_instruction_limit.integer, LUAMOD_HOOKSTEP);

	LuaMods_OpenLib(L, LUA_GNAME, luaopen_base);
	LuaMods_OpenLib(L, LUA_COLIBNAME, luaopen_coroutine);
	LuaMods_OpenLib(L, LUA_TABLIBNAME, luaopen_table);
	LuaMods_OpenLib(L, LUA_STRLIBNAME, luaopen_string);
	LuaMods_OpenLib(L, LUA_MATHLIBNAME, luaopen_math);
	LuaMods_OpenLib(L, LUA_UTF8LIBNAME, luaopen_utf8);

	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "xon.handlers");

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

	luaL_newlib(L, luaxon_funcs);
	lua_pushstring(L, m->name); lua_setfield(L, -2, "mod_name");
	lua_pushstring(L, m->version); lua_setfield(L, -2, "mod_version");
	lua_pushstring(L, m->dir); lua_setfield(L, -2, "mod_dir");
	lua_setglobal(L, "xon");

	if (luaL_loadbufferx(L, luamods_prelude, strlen(luamods_prelude), "=xon_prelude", "t") != LUA_OK)
	{
		Con_Printf("^1[lua] internal prelude error: %s^7\n", lua_tostring(L, -1));
		return false;
	}
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, LuaInternal_killed); lua_setfield(L, -2, "killed");
	lua_pushcfunction(L, LuaInternal_readsrc); lua_setfield(L, -2, "readsrc");
	if (!LuaMods_Call(m, 1, 1, "prelude"))
		return false;
	lua_setfield(L, LUA_REGISTRYINDEX, "xon.vec");
	return true;
}

// tiny scanner for "key": "value" in a flat mod.json; good enough for the
// name/version metadata (load order is read by the game code)
static void LuaMods_JsonString(const char *json, const char *key, char *out, size_t outsize)
{
	char pat[80];
	const char *p, *e;
	dpsnprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (!p)
		return;
	p += strlen(pat);
	while (*p && (ISWHITESPACE(*p) || *p == ':'))
		p++;
	if (*p != '"')
		return;
	p++;
	for (e = p; *e && *e != '"'; e++)
		;
	if (*e != '"' || (size_t)(e - p) >= outsize)
		return;
	memcpy(out, p, e - p);
	out[e - p] = 0;
}

static luamod_t *LuaMods_Load(const char *path)
{
	luamod_t *m;
	char json[MAX_QPATH];
	const char *slash;
	unsigned char *buf;
	fs_offset_t size;
	int i;

	for (i = 0; i < luamods_num; i++)
		if (!strcmp(luamods[i]->path, path))
			return luamods[i];
	if (luamods_num >= LUAMOD_MAX)
	{
		Con_Printf("^1[lua] too many mods (max %d), not loading %s^7\n", LUAMOD_MAX, path);
		return NULL;
	}
	if (strncmp(path, "mods/", 5) || strstr(path, "..") || strlen(path) >= MAX_QPATH || !(slash = strrchr(path, '/')) || slash == path + 4)
	{
		Con_Printf("^1[lua] refusing to load %s: mods must live in mods/<name>/^7\n", path);
		return NULL;
	}

	m = (luamod_t *)Mem_Alloc(lua_mempool, sizeof(*m));
	luamods[luamods_num++] = m;
	strlcpy(m->path, path, sizeof(m->path));
	memcpy(m->dir, path, slash - path);
	m->dir[slash - path] = 0;
	strlcpy(m->name, m->dir + 5, sizeof(m->name));
	strlcpy(m->version, "0", sizeof(m->version));

	dpsnprintf(json, sizeof(json), "%s/mod.json", m->dir);
	buf = FS_LoadFile(json, tempmempool, true, &size);
	if (buf)
	{
		char *text = (char *)Mem_Alloc(tempmempool, size + 1);
		memcpy(text, buf, size);
		text[size] = 0;
		LuaMods_JsonString(text, "name", m->name, sizeof(m->name));
		LuaMods_JsonString(text, "version", m->version, sizeof(m->version));
		Mem_Free(text);
		Mem_Free(buf);
	}

	buf = FS_LoadFile(path, tempmempool, true, &size);
	if (!buf)
	{
		dpsnprintf(m->reason, sizeof(m->reason), "file not found");
		Con_Printf("^1[lua] mod %s: %s not found^7\n", m->name, path);
		m->disabled = true;
		return m;
	}
	if (!LuaMods_CreateState(m))
	{
		Mem_Free(buf);
		LuaMods_Disable(m, "could not create Lua state");
		return m;
	}
	{
		char chunkname[MAX_QPATH + 1];
		dpsnprintf(chunkname, sizeof(chunkname), "@%s", path);
		if (luaL_loadbufferx(m->L, (const char *)buf, (size_t)size, chunkname, "t") != LUA_OK)
		{
			char err[1024];
			strlcpy(err, lua_tostring(m->L, -1), sizeof(err));
			Mem_Free(buf);
			LuaMods_Crashed(m, "failed to compile", err);
			return m;
		}
	}
	Mem_Free(buf);
	if (!LuaMods_Call(m, 0, 0, "failed to load"))
		return m;
	Con_Printf("[lua] loaded mod %s %s (%s), %d handler%s\n", m->name, m->version, m->path, m->handlers, m->handlers == 1 ? "" : "s");
	return m;
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
	unsigned int h = LuaMods_Hash(sig);
	luasig_t *s;
	const char *p = sig;
	for (s = luasigs[h]; s; s = s->next)
		if (!strcmp(s->key, sig))
			return s;
	s = (luasig_t *)Mem_Alloc(lua_mempool, sizeof(*s));
	s->key = LuaMods_StrDup(lua_mempool, sig);
	while (*p)
	{
		qboolean out = false;
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
			ddef_t *d;
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
			v->s = LuaMods_StrDup(tempmempool, PRVM_GetString(prog, PRVM_GLOBALFIELDSTRING(p->ofs)));
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
			PRVM_GLOBALFIELDSTRING(p->ofs) = PRVM_SetTempString(prog, v->s ? v->s : "");
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
	case 'v': LuaMods_PushVec(L, v->v); break;
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
		LuaMods_ToVec(L, -1, v->v);
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
			v->s = LuaMods_StrDup(tempmempool, s);
		}
		break;
	}
	lua_pop(L, 1);
}

static qboolean LuaMods_Dispatch(prvm_prog_t *prog, const char *hook, const char *sigstr)
{
	luahook_t *h = LuaMods_FindHook(hook, false);
	luasig_t *sig;
	luaval_t vals[LUASIG_MAXPARMS];
	qboolean ret = false;
	int i, j, k;

	if (!h || h->count <= 0)
		return false;
	sig = LuaMods_Signature(prog, sigstr);
	memset(vals, 0, sizeof(vals));
	LuaMods_ReadGlobals(prog, sig, vals);

	for (i = 0; i < luamods_num; i++)
	{
		luamod_t *m = luamods[i];
		lua_State *L;
		int handlers, ev, n;
		if (m->disabled || !m->L)
			continue;
		L = m->L;
		lua_getfield(L, LUA_REGISTRYINDEX, "xon.handlers");
		if (lua_getfield(L, -1, hook) != LUA_TTABLE)
		{
			lua_pop(L, 2);
			continue;
		}
		handlers = lua_gettop(L);
		n = (int)lua_rawlen(L, handlers);

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
			if (!LuaMods_Call(m, 1, 1, hook))
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
		lua_settop(L, handlers - 2);
	}

	LuaMods_WriteGlobals(prog, sig, vals);
	for (k = 0; k < sig->num; k++)
		if (vals[k].s)
			Mem_Free(vals[k].s);
	return ret;
}

//============================================================================
// builtins

void VM_SV_lua_load(prvm_prog_t *prog)
{
	luamod_t *m;
	VM_SAFEPARMCOUNT(1, VM_SV_lua_load);
	m = LuaMods_Load(PRVM_G_STRING(OFS_PARM0));
	PRVM_G_FLOAT(OFS_RETURN) = (m && !m->disabled) ? 1 : 0;
}

void VM_SV_lua_hook(prvm_prog_t *prog)
{
	VM_SAFEPARMCOUNTRANGE(1, 8, VM_SV_lua_hook);
	PRVM_G_FLOAT(OFS_RETURN) = LuaMods_Dispatch(prog, PRVM_G_STRING(OFS_PARM0), prog->argc > 1 ? PRVM_G_STRING(OFS_PARM1) : "");
}

void VM_SV_lua_hookcount(prvm_prog_t *prog)
{
	luahook_t *h;
	VM_SAFEPARMCOUNT(1, VM_SV_lua_hookcount);
	h = LuaMods_FindHook(PRVM_G_STRING(OFS_PARM0), false);
	PRVM_G_FLOAT(OFS_RETURN) = h ? h->count : 0;
}

//============================================================================
// console commands

static void LuaMods_List_f(void)
{
	int i, ok = 0;
	for (i = 0; i < luamods_num; i++)
	{
		luamod_t *m = luamods[i];
		if (m->disabled)
			Con_Printf("%2d ^1%-20s^7 %-8s %-32s ^1disabled: %s^7\n", i + 1, m->name, m->version, m->path, m->reason);
		else
		{
			Con_Printf("%2d ^2%-20s^7 %-8s %-32s %d handlers, %u KB\n", i + 1, m->name, m->version, m->path, m->handlers, (unsigned int)(m->mem / 1024));
			ok++;
		}
	}
	Con_Printf("%d mod%s loaded, %d active\n", luamods_num, luamods_num == 1 ? "" : "s", ok);
}

static void LuaMods_Reload_f(void)
{
	prvm_prog_t *prog = LuaMods_Prog();
	char paths[LUAMOD_MAX][MAX_QPATH];
	int i, n;
	mfunction_t *f;

	if (!sv.active || !prog)
	{
		Con_Print("lua_reload: no server running\n");
		return;
	}
	n = luamods_num;
	for (i = 0; i < n; i++)
		strlcpy(paths[i], luamods[i]->path, sizeof(paths[i]));
	LuaMods_CloseAll();
	Con_Printf("[lua] reloading mods\n");

	// let the game code rescan mods/ (picks up new and removed mods); if it
	// does not provide the function, reload the previous list in order
	f = PRVM_ED_FindFunction(prog, LUAMOD_QC_RELOAD);
	if (f)
	{
		PRVM_serverglobalfloat(time) = sv.time;
		PRVM_serverglobaledict(self) = 0;
		prog->ExecuteProgram(prog, f - prog->functions, LUAMOD_QC_RELOAD " missing");
	}
	else
		for (i = 0; i < n; i++)
			LuaMods_Load(paths[i]);
	LuaMods_List_f();
}

void LuaMods_Init(void)
{
	lua_mempool = Mem_AllocPool("lua mods", 0, NULL);
	Cvar_RegisterVariable(&lua_instruction_limit);
	Cvar_RegisterVariable(&lua_memory_limit);
	Cmd_AddCommand("lua_mods", LuaMods_List_f, "list Lua mods and their status");
	Cmd_AddCommand("lua_reload", LuaMods_Reload_f, "reload every Lua mod without restarting the map");
}
