/*
Weapon state the touch assists read from the loaded CSQC (touch_wepquery.c).
Ported from Xonotic Touch (https://github.com/dixonSolutions/Xonotic-Touch),
whose engine-side HUD is not part of Insanotic.
*/
#ifndef TOUCH_HUD_H
#define TOUCH_HUD_H

#include "touch_compat.h"

// False: Insanotic's touch controls are a Lua mod, not an engine HUD.
qbool TouchHUD_Active(void);

// A CSQC finished loading or was unloaded: bind its weapon registry.
void TouchHUD_ProgsChanged(void);

// Indices run 0..TouchHUD_NumWeapons()-1 over the loaded CSQC's weapon registry.
int TouchHUD_NumWeapons(void);
const char *TouchHUD_WeaponName(int index); // netname, "" when out of range
qbool TouchHUD_WeaponOwned(int index);
int TouchHUD_WeaponAmmo(int index);         // reserve; -1 = unlimited or uses none
int TouchHUD_HeldWeapon(void);              // index being held (switched to), -1 unknown
int TouchHUD_ActiveWeapon(void);            // index actually in hand (a switch done), -1 unknown
int TouchHUD_HeldClip(void);                // rounds loaded, -1 = no clip
int TouchHUD_AltFireState(void);            // TOUCH_ALTFIRE_* (touch_altfire.h)

#endif
