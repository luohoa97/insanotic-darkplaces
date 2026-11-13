/*
Offline tactics: not part of Insanotic.

Xonotic Touch's touch_tactics.c (https://github.com/dixonSolutions/Xonotic-Touch)
reads every enemy's health, armour, weapon, position and aim straight from
the local server and steers by it. That is see-through help by design, so it
is not ported. This stub keeps the aim helpers' calls into it closed: nothing
here ever opens, draws, steers or picks anything.
*/

#include "quakedef.h"
#include "touch_compat.h"
#include "touch_aim.h"
#include "touch_tactics.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "touch_weapons.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Read by touch_aim.c; fixed at "off" / 0 and read-only, so no setting can
// turn the see-through helpers on.
cvar_t cl_touch_esp_enabled = {CVAR_READONLY, "cl_touch_esp_enabled", "off", "always off in Insanotic: no see-through or intel helpers"};
cvar_t cl_touch_practice_exact = {CVAR_READONLY, "cl_touch_practice_exact", "0", "always 0 in Insanotic"};

void TouchTactics_Init(void) {}
void TouchTactics_Frame(void) {}
int TouchTactics_Buttons(void) { return 0; }
float TouchTactics_TargetBonus(int entnum) { (void)entnum; return 0; }
int TouchTactics_PickWeapon(int entnum, float dist, const char *const *names, const qbool *ready, int n, int heldpos)
{
	(void)entnum; (void)dist; (void)names; (void)ready; (void)n; (void)heldpos;
	return -1;
}
int TouchTactics_EspGate(int g) { (void)g; return TW_GATE_NO_SERVER; }
qbool TouchTactics_Open(void) { return false; }
qbool TouchTactics_ExactVelocity(int n, float *vel) { (void)n; (void)vel; return false; }
void TouchTactics_Draw(void) {}
