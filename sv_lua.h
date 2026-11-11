/*
Insanotic: server-side Lua mod loader (XON_LUA extension).
SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef SV_LUA_H
#define SV_LUA_H

// registers cvars and console commands (called from SV_Init)
void LuaMods_Init(void);

// closes every mod and drops caches tied to the server progs; called when the
// server progs are reset (map change, server shutdown)
void LuaMods_ProgReset(void);

// "rml_reply <page> <action> [name value]..." from a client: runs the PageReply
// hook of the plugins. False when no plugin listens (the line then goes on to
// the game code).
struct client_s;
qboolean LuaMods_PageReply(struct client_s *c, const char *line);

// QC builtins (XON_LUA)
struct prvm_prog_s;
void VM_SV_lua_load(struct prvm_prog_s *prog);      // #680 float lua_load(string vfs_path)
void VM_SV_lua_hook(struct prvm_prog_s *prog);      // #681 float lua_hook(string hook_name, ...)
void VM_SV_lua_hookcount(struct prvm_prog_s *prog); // #682 float lua_hookcount(string hook_name)

#endif
