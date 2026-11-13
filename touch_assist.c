/*
Server switch for the touch assists, ported from Xonotic Touch's
touch_assist.c (https://github.com/dixonSolutions/Xonotic-Touch) and changed
to use Insanotic's single permission.

A server with sv_allow_assist 1 sends each joining client "set
_cl_assist_allowed 1" (sv_main.c). The touch assists (aim pull, auto-shoot,
automatic weapon switching, auto dodge, ledge guard, hook fall rescue) run only
while that cvar is 1. Anything else (sv_allow_assist 0, the default, or a stock
Xonotic server that says nothing) leaves them off. This is a request the client
honours, not something the server can enforce.
*/

#include "quakedef.h"
#include "touch_compat.h"

#include "touch_assist.h"

static cvar_t *assist_cvar(void)
{
	static cvar_t *c;
	if (!c)
		c = Cvar_FindVar("_cl_assist_allowed");
	return c;
}

void TouchAssist_Init(void)
{
}

// Once a frame (before this frame's packets are read): a new connection, or
// none, starts from "the server has said nothing".
void TouchAssist_Frame(void)
{
	static const void *netcon;
	cvar_t *c = assist_cvar();
	if (c && c->integer && (cls.state != ca_connected || (const void *)cls.netcon != netcon))
		Cvar_SetValueQuick(c, 0);
	netcon = cls.netcon;
}

qbool TouchAssist_Allowed(int bit)
{
	cvar_t *c = assist_cvar();
	(void)bit;
	return c && c->integer != 0;
}
