/*
Insanotic: sandboxed Lua runtime shared by server plugins (sv_lua.c) and
client mods (cl_lua.c).
SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef LUA_SANDBOX_H
#define LUA_SANDBOX_H

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#define LUASANDBOX_MAX 64

struct luahost_s;

// one loaded plugin or mod: its own lua_State and limits
typedef struct luasandbox_s
{
	struct luahost_s *host;
	char path[MAX_QPATH];      // <root>/<dir>/init.lua
	char dir[MAX_QPATH];       // <root>/<dir>
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
} luasandbox_t;

typedef struct luahookname_s
{
	char name[64];
	int count;                 // handlers over all enabled scripts
	struct luahookname_s *next;
} luahookname_t;

// one runtime: the server's plugins or the client's mods
typedef struct luahost_s
{
	const char *kind;          // "plugin" / "mod" (log messages)
	const char *root;          // "plugins" / "mods" (VFS folder)
	const char *manifest;      // "plugin.json" / "mod.json"
	const char *survivor;      // "server" / "game" (log messages)
	cvar_t *instruction_limit;
	cvar_t *memory_limit;
	int cvar_flags;            // flags for cvars scripts create (client: CVAR_SAVE)
	// adds host specific functions to the xon table on top of the stack
	void (*register_api)(lua_State *L);
	luasandbox_t *items[LUASANDBOX_MAX];
	int num;
	luahookname_t *hooks;
} luahost_t;

void LuaSandbox_Init(void);
char *LuaSandbox_StrDup(mempool_t *pool, const char *str);
unsigned int LuaSandbox_Hash(const char *s, unsigned int size);

luasandbox_t *LuaSandbox_FromState(lua_State *L);
// loads <root>/<name>/init.lua (and the optional manifest) into host
luasandbox_t *LuaSandbox_Load(luahost_t *host, const char *path);
// pcall with a fresh instruction budget; on failure the script is logged and
// disabled (its state is closed) and false is returned
qboolean LuaSandbox_Call(luasandbox_t *m, int nargs, int nresults, const char *what);
void LuaSandbox_Disable(luasandbox_t *m, const char *reason);
void LuaSandbox_CloseAll(luahost_t *host);
void LuaSandbox_PrintList(luahost_t *host);
luahookname_t *LuaSandbox_FindHook(luahost_t *host, const char *name);
int LuaSandbox_HookCount(luahost_t *host, const char *name);
// pushes the handler list for hook (a table) and returns its length; pushes
// nothing and returns 0 when the script has no handler for it
int LuaSandbox_PushHandlers(luasandbox_t *m, const char *hook);

void LuaSandbox_PushVec(lua_State *L, const vec3_t v);
qboolean LuaSandbox_ToVec(lua_State *L, int idx, vec3_t out);
void LuaSandbox_CheckVec(lua_State *L, int idx, vec3_t out);

#endif
