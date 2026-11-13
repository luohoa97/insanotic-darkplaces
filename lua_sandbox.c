/*
Insanotic: sandboxed Lua runtime shared by server plugins (sv_lua.c) and
client mods (cl_lua.c).
SPDX-License-Identifier: GPL-3.0-or-later

Every script (plugin or mod) gets its own lua_State with:
  - only the base, coroutine, table, string, math and utf8 libraries
    (io, os, debug and package are not even compiled in, see lua/LICENSE)
  - a require() that can only load .lua files from the script's own folder
    in the virtual filesystem; load() only accepts text chunks
  - an instruction budget per entry (load or hook call), enforced with a
    count hook; exceeding it kills and disables the script
  - a memory cap enforced by the allocator
Any error raised out of a script is logged and the script is disabled; the
server or game keeps running. pcall, xpcall and coroutine.resume are wrapped
so a killed script cannot catch its own death.
*/

#include "quakedef.h"
#include "lua_sandbox.h"

#define LUASANDBOX_HOOKSTEP 1000

static mempool_t *luasandbox_mempool;

void LuaSandbox_Init(void)
{
	if (!luasandbox_mempool)
		luasandbox_mempool = Mem_AllocPool("lua sandbox", 0, NULL);
}

char *LuaSandbox_StrDup(mempool_t *pool, const char *str)
{
	size_t len = strlen(str) + 1;
	char *d = (char *)Mem_Alloc(pool, len);
	memcpy(d, str, len);
	return d;
}

unsigned int LuaSandbox_Hash(const char *s, unsigned int size)
{
	unsigned int h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h % size;
}

luasandbox_t *LuaSandbox_FromState(lua_State *L)
{
	return *(luasandbox_t **)lua_getextraspace(L);
}

//============================================================================
// limits

static void *LuaSandbox_Alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	luasandbox_t *m = (luasandbox_t *)ud;
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

static void LuaSandbox_CountHook(lua_State *L, lua_Debug *ar)
{
	luasandbox_t *m = LuaSandbox_FromState(L);
	(void)ar;
	m->budget -= LUASANDBOX_HOOKSTEP;
	if (m->budget <= 0 && !m->killed)
	{
		m->killed = true;
		dpsnprintf(m->reason, sizeof(m->reason), "instruction limit of %d exceeded (runaway script?)", m->host->instruction_limit->integer);
	}
	if (m->killed)
		luaL_error(L, "%s", m->reason);
}

//============================================================================
// hook names

luahookname_t *LuaSandbox_FindHook(luahost_t *host, const char *name)
{
	luahookname_t *h;
	for (h = host->hooks; h; h = h->next)
		if (!strcmp(h->name, name))
			return h;
	return NULL;
}

int LuaSandbox_HookCount(luahost_t *host, const char *name)
{
	luahookname_t *h = LuaSandbox_FindHook(host, name);
	return h ? h->count : 0;
}

static luahookname_t *LuaSandbox_AddHook(luahost_t *host, const char *name)
{
	luahookname_t *h = LuaSandbox_FindHook(host, name);
	if (h)
		return h;
	h = (luahookname_t *)Mem_Alloc(luasandbox_mempool, sizeof(*h));
	strlcpy(h->name, name, sizeof(h->name));
	h->next = host->hooks;
	host->hooks = h;
	return h;
}

int LuaSandbox_PushHandlers(luasandbox_t *m, const char *hook)
{
	lua_State *L = m->L;
	int n;
	if (m->disabled || !L)
		return 0;
	lua_getfield(L, LUA_REGISTRYINDEX, "xon.handlers");
	if (lua_getfield(L, -1, hook) != LUA_TTABLE)
	{
		lua_pop(L, 2);
		return 0;
	}
	lua_remove(L, -2);
	n = (int)lua_rawlen(L, -1);
	if (!n)
		lua_pop(L, 1);
	return n;
}

//============================================================================
// lifetime

void LuaSandbox_Disable(luasandbox_t *m, const char *reason)
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
					luahookname_t *h = LuaSandbox_FindHook(m->host, lua_tostring(L, -2));
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

static void LuaSandbox_Crashed(luasandbox_t *m, const char *what, const char *err)
{
	Con_Printf("^1[lua] %s %s (%s) %s: %s^7\n", m->host->kind, m->name, m->path, what, err ? err : "unknown error");
	if (!m->reason[0])
	{
		char *nl;
		dpsnprintf(m->reason, sizeof(m->reason), "%s: %s", what, err ? err : "unknown error");
		if ((nl = strchr(m->reason, '\n')))
			*nl = 0; // the traceback was printed above; keep the list short
	}
	Con_Printf("^1[lua] %s %s disabled; the %s keeps running^7\n", m->host->kind, m->name, m->host->survivor);
	LuaSandbox_Disable(m, NULL);
}

void LuaSandbox_CloseAll(luahost_t *host)
{
	int i;
	luahookname_t *h, *next;
	for (i = 0; i < host->num; i++)
	{
		LuaSandbox_Disable(host->items[i], "unloaded");
		Mem_Free(host->items[i]);
		host->items[i] = NULL;
	}
	host->num = 0;
	for (h = host->hooks; h; h = next)
	{
		next = h->next;
		Mem_Free(h);
	}
	host->hooks = NULL;
}

void LuaSandbox_PrintList(luahost_t *host)
{
	int i, ok = 0;
	for (i = 0; i < host->num; i++)
	{
		luasandbox_t *m = host->items[i];
		if (m->disabled)
			Con_Printf("%2d ^1%-20s^7 %-8s %-34s ^1disabled: %s^7\n", i + 1, m->name, m->version, m->path, m->reason);
		else
		{
			Con_Printf("%2d ^2%-20s^7 %-8s %-34s %d handlers, %u KB\n", i + 1, m->name, m->version, m->path, m->handlers, (unsigned int)(m->mem / 1024));
			ok++;
		}
	}
	Con_Printf("%d %s%s loaded, %d active\n", host->num, host->kind, host->num == 1 ? "" : "s", ok);
}

//============================================================================
// vectors

void LuaSandbox_PushVec(lua_State *L, const vec3_t v)
{
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
	lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
	lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
	lua_getfield(L, LUA_REGISTRYINDEX, "xon.vec");
	lua_setmetatable(L, -2);
}

qboolean LuaSandbox_ToVec(lua_State *L, int idx, vec3_t out)
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

void LuaSandbox_CheckVec(lua_State *L, int idx, vec3_t out)
{
	if (!LuaSandbox_ToVec(L, idx, out))
		luaL_typeerror(L, idx, "vector");
}

//============================================================================
// common xon.* API

static int LuaXon_on(lua_State *L)
{
	luasandbox_t *m = LuaSandbox_FromState(L);
	const char *hook = luaL_checkstring(L, 1);
	luahookname_t *h;
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
	h = LuaSandbox_AddHook(m->host, hook);
	h->count++;
	m->handlers++;
	return 0;
}

static int LuaXon_print(lua_State *L)
{
	luasandbox_t *m = LuaSandbox_FromState(L);
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

static cvar_t *LuaXon_CvarForRead(lua_State *L, const char *name)
{
	cvar_t *c = Cvar_FindVar(name);
	if (c && (c->flags & CVAR_PRIVATE))
		luaL_error(L, "cvar '%s' is private", name);
	return c;
}

static int LuaXon_cvar(lua_State *L)
{
	cvar_t *c = LuaXon_CvarForRead(L, luaL_checkstring(L, 1));
	if (!c)
		return 0;
	lua_pushnumber(L, c->value);
	return 1;
}

static int LuaXon_cvar_string(lua_State *L)
{
	cvar_t *c = LuaXon_CvarForRead(L, luaL_checkstring(L, 1));
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
	luasandbox_t *m = LuaSandbox_FromState(L);
	const char *name = luaL_checkstring(L, 1);
	const char *def = luaL_tolstring(L, 2, NULL);
	const char *desc = luaL_optstring(L, 3, NULL);
	char descbuf[128];
	cvar_t *c = Cvar_FindVar(name);
	if (!desc)
	{
		dpsnprintf(descbuf, sizeof(descbuf), "registered by Lua %s %s", m->host->kind, m->name);
		desc = descbuf;
	}
	if (!c)
		c = Cvar_Get(name, def, m->host->cvar_flags, desc);
	if (!c || (c->flags & CVAR_PRIVATE))
		return luaL_error(L, "cannot register cvar '%s'", name);
	lua_pushnumber(L, c->value);
	return 1;
}

static const luaL_Reg luaxon_common[] =
{
	{"on", LuaXon_on},
	{"print", LuaXon_print},
	{"cvar", LuaXon_cvar},
	{"cvar_string", LuaXon_cvar_string},
	{"cvar_set", LuaXon_cvar_set},
	{"cvar_register", LuaXon_cvar_register},
	{NULL, NULL}
};

//============================================================================
// sandbox

static int LuaInternal_killed(lua_State *L)
{
	lua_pushboolean(L, LuaSandbox_FromState(L)->killed);
	return 1;
}

// require() backend: module name (already validated) -> source, chunkname
static int LuaInternal_readsrc(lua_State *L)
{
	luasandbox_t *m = LuaSandbox_FromState(L);
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
static const char *luasandbox_prelude =
"local C = ...\n"
"local rawpcall, rawxpcall, rawload, killed = pcall, xpcall, load, C.killed\n"
"local readsrc = C.readsrc\n"
"-- a killed script must not be able to swallow its own death\n"
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
"-- view angles (pitch yaw roll; positive pitch looks down) <-> direction\n"
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

static void LuaSandbox_OpenLib(lua_State *L, const char *name, lua_CFunction f)
{
	luaL_requiref(L, name, f, 1);
	lua_pop(L, 1);
}

static int LuaSandbox_MsgHandler(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	if (!msg)
		msg = luaL_tolstring(L, 1, NULL);
	luaL_traceback(L, L, msg, 1);
	return 1;
}

qboolean LuaSandbox_Call(luasandbox_t *m, int nargs, int nresults, const char *what)
{
	lua_State *L = m->L;
	int base = lua_gettop(L) - nargs;
	int status;
	lua_pushcfunction(L, LuaSandbox_MsgHandler);
	lua_insert(L, base);
	m->budget = max(m->host->instruction_limit->integer, LUASANDBOX_HOOKSTEP);
	status = lua_pcall(L, nargs, nresults, base);
	lua_remove(L, base);
	if (status != LUA_OK || m->killed)
	{
		char err[1024];
		strlcpy(err, m->killed ? m->reason : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error object is not a string"), sizeof(err));
		LuaSandbox_Crashed(m, what, err);
		return false;
	}
	return true;
}

static qboolean LuaSandbox_CreateState(luasandbox_t *m)
{
	lua_State *L;
	m->memlimit = (size_t)max(m->host->memory_limit->value, 1) * 1024 * 1024;
	L = lua_newstate(LuaSandbox_Alloc, m);
	if (!L)
		return false;
	m->L = L;
	*(luasandbox_t **)lua_getextraspace(L) = m;
	lua_sethook(L, LuaSandbox_CountHook, LUA_MASKCOUNT, LUASANDBOX_HOOKSTEP);
	m->budget = max(m->host->instruction_limit->integer, LUASANDBOX_HOOKSTEP);

	LuaSandbox_OpenLib(L, LUA_GNAME, luaopen_base);
	LuaSandbox_OpenLib(L, LUA_COLIBNAME, luaopen_coroutine);
	LuaSandbox_OpenLib(L, LUA_TABLIBNAME, luaopen_table);
	LuaSandbox_OpenLib(L, LUA_STRLIBNAME, luaopen_string);
	LuaSandbox_OpenLib(L, LUA_MATHLIBNAME, luaopen_math);
	LuaSandbox_OpenLib(L, LUA_UTF8LIBNAME, luaopen_utf8);

	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "xon.handlers");

	luaL_newlib(L, luaxon_common);
	lua_pushstring(L, m->name); lua_setfield(L, -2, "script_name");
	lua_pushstring(L, m->version); lua_setfield(L, -2, "script_version");
	lua_pushstring(L, m->dir); lua_setfield(L, -2, "script_dir");
	lua_pushstring(L, m->host->kind); lua_setfield(L, -2, "script_kind");
	if (m->host->register_api)
		m->host->register_api(L);
	lua_setglobal(L, "xon");

	if (luaL_loadbufferx(L, luasandbox_prelude, strlen(luasandbox_prelude), "=xon_prelude", "t") != LUA_OK)
	{
		Con_Printf("^1[lua] internal prelude error: %s^7\n", lua_tostring(L, -1));
		return false;
	}
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, LuaInternal_killed); lua_setfield(L, -2, "killed");
	lua_pushcfunction(L, LuaInternal_readsrc); lua_setfield(L, -2, "readsrc");
	if (!LuaSandbox_Call(m, 1, 1, "prelude"))
		return false;
	lua_setfield(L, LUA_REGISTRYINDEX, "xon.vec");
	return true;
}

// tiny scanner for "key": "value" in a flat manifest; good enough for the
// name/version metadata (load order is read by whoever orders the loads)
static void LuaSandbox_JsonString(const char *json, const char *key, char *out, size_t outsize)
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

luasandbox_t *LuaSandbox_Load(luahost_t *host, const char *path)
{
	luasandbox_t *m;
	char json[MAX_QPATH];
	const char *slash;
	unsigned char *buf;
	fs_offset_t size;
	size_t rootlen = strlen(host->root);
	int i;

	LuaSandbox_Init();
	for (i = 0; i < host->num; i++)
		if (!strcmp(host->items[i]->path, path))
			return host->items[i];
	if (host->num >= LUASANDBOX_MAX)
	{
		Con_Printf("^1[lua] too many %ss (max %d), not loading %s^7\n", host->kind, LUASANDBOX_MAX, path);
		return NULL;
	}
	if (strncmp(path, host->root, rootlen) || path[rootlen] != '/' || strstr(path, "..") || strlen(path) >= MAX_QPATH
	 || !(slash = strrchr(path, '/')) || slash == path + rootlen)
	{
		Con_Printf("^1[lua] refusing to load %s: %ss must live in %s/<name>/^7\n", path, host->kind, host->root);
		return NULL;
	}

	m = (luasandbox_t *)Mem_Alloc(luasandbox_mempool, sizeof(*m));
	host->items[host->num++] = m;
	m->host = host;
	strlcpy(m->path, path, sizeof(m->path));
	memcpy(m->dir, path, slash - path);
	m->dir[slash - path] = 0;
	strlcpy(m->name, m->dir + rootlen + 1, sizeof(m->name));
	strlcpy(m->version, "0", sizeof(m->version));

	dpsnprintf(json, sizeof(json), "%s/%s", m->dir, host->manifest);
	buf = FS_LoadFile(json, tempmempool, true, &size);
	if (buf)
	{
		char *text = (char *)Mem_Alloc(tempmempool, size + 1);
		memcpy(text, buf, size);
		text[size] = 0;
		LuaSandbox_JsonString(text, "name", m->name, sizeof(m->name));
		LuaSandbox_JsonString(text, "version", m->version, sizeof(m->version));
		Mem_Free(text);
		Mem_Free(buf);
	}

	buf = FS_LoadFile(path, tempmempool, true, &size);
	if (!buf)
	{
		dpsnprintf(m->reason, sizeof(m->reason), "file not found");
		Con_Printf("^1[lua] %s %s: %s not found^7\n", host->kind, m->name, path);
		m->disabled = true;
		return m;
	}
	if (!LuaSandbox_CreateState(m))
	{
		Mem_Free(buf);
		LuaSandbox_Disable(m, "could not create Lua state");
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
			LuaSandbox_Crashed(m, "failed to compile", err);
			return m;
		}
	}
	Mem_Free(buf);
	if (!LuaSandbox_Call(m, 0, 0, "failed to load"))
		return m;
	Con_Printf("[lua] loaded %s %s %s (%s), %d handler%s\n", host->kind, m->name, m->version, m->path, m->handlers, m->handlers == 1 ? "" : "s");
	return m;
}
