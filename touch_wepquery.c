/*
Weapon state for the touch assists, taken from Xonotic Touch's touch_hud.c
(https://github.com/dixonSolutions/Xonotic-Touch). Only the part the assists
read: the loaded CSQC's weapon registry and the held weapon. The engine-side
Touch HUD itself is not part of Insanotic (its touch controls are a Lua mod).
*/
#include "quakedef.h"
#include "touch_compat.h"
#include "touch_hud.h"
#include "touch_altfire.h"

static int stat_weapons = -1;

#define MAX_WEAPONS 72
// ammo_stat: the stat holding this weapon's reserve (-1 = none: infinite).
typedef struct wepdef_s { char name[32]; char pic[64]; char ammo_icon[32]; int impulse; int id; int ammo_stat; } wepdef_t;
static wepdef_t weapons[MAX_WEAPONS];
static int num_weapons = 0;
// STAT_TOUCH_ALTFIRE when the server's CSQC registers it (this port's game
// code), -1 on any other server: touch_altfire.h then judges from the stock
// balance.
static int stat_altfire = -1;
// The CSQC's viewmodels[0] (the wepent) tells the held weapon and its clip,
// which no stat does: global offset plus the fields read from it each frame.
typedef struct qcfield_s { int ofs; int type; } qcfield_t;
static int viewmodels_ofs = -1;
static qcfield_t f_switchweapon, f_activeweapon, f_clip_load, f_clip_size, f_wep_id;

static qcfield_t qcfield(prvm_prog_t *prog, const char *name)
{
	qcfield_t f = {-1, 0};
	mdef_t *d = PRVM_ED_FindField(prog, name);
	if (d)
	{
		f.ofs = d->ofs;
		f.type = d->type & ~DEF_SAVEGLOBAL;
	}
	return f;
}

static int qcfield_int(prvm_prog_t *prog, prvm_edict_t *ed, qcfield_t f)
{
	prvm_eval_t *v;
	if (f.ofs < 0)
		return 0;
	v = PRVM_EDICTFIELDVALUE(ed, f.ofs);
	return f.type == ev_float ? (int)v->_float : v->_int;
}

static int def_type(const mdef_t *d)
{
	return d->type & ~DEF_SAVEGLOBAL;
}

// A field as an int, whether the QC declared it int or float.
static int field_int(prvm_prog_t *prog, prvm_edict_t *ed, const mdef_t *f)
{
	prvm_eval_t *v = PRVM_EDICTFIELDVALUE(ed, f->ofs);
	return def_type(f) == ev_float ? (int)v->_float : v->_int;
}

static const char *field_string(prvm_prog_t *prog, prvm_edict_t *ed, const mdef_t *f)
{
	return f ? PRVM_GetString(prog, PRVM_EDICTFIELDVALUE(ed, f->ofs)->string) : "";
}

static prvm_edict_t *edict_or_null(prvm_prog_t *prog, int n)
{
	if (n <= 0 || n >= prog->num_edicts)
		return NULL;
	return PRVM_EDICT_NUM(n);
}

static prvm_edict_t *global_edict(prvm_prog_t *prog, const char *name)
{
	mdef_t *g = PRVM_ED_FindGlobal(prog, name);
	if (!g || def_type(g) != ev_entity)
		return NULL;
	return edict_or_null(prog, PRVM_GLOBALFIELDEDICT(g->ofs));
}

static void bind_weapons(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *f_id, *f_impulse, *f_pic, *f_name, *f_next;
	prvm_edict_t *ed;
	int guard;
	mdef_t *f_ammo_type, *f_ammo_icon, *g_viewmodels;
	int res_none = 0, stat_fuel = -1, stat_plasma = -1;
	num_weapons = 0;
	stat_weapons = -1;
	stat_altfire = -1;
	viewmodels_ofs = -1;
	if (!prog->loaded)
		return;
	f_id = PRVM_ED_FindField(prog, "m_id");
	f_impulse = PRVM_ED_FindField(prog, "impulse");
	f_next = PRVM_ED_FindField(prog, "enemy");
	f_pic = PRVM_ED_FindField(prog, "model2");
	f_name = PRVM_ED_FindField(prog, "netname");
	f_ammo_type = PRVM_ED_FindField(prog, "ammo_type");
	f_ammo_icon = PRVM_ED_FindField(prog, "m_icon");
	if (!f_id || !f_impulse || !f_next)
		return;
	ed = global_edict(prog, "STAT_WEAPONS");
	if (ed)
	{
		stat_weapons = field_int(prog, ed, f_id);
		if (stat_weapons < 0 || stat_weapons + 2 >= MAX_CL_STATS)
			stat_weapons = -1;
	}
	ed = global_edict(prog, "STAT_TOUCH_ALTFIRE");
	if (ed)
	{
		stat_altfire = field_int(prog, ed, f_id);
		if (stat_altfire < 0 || stat_altfire >= MAX_CL_STATS)
			stat_altfire = -1;
	}
	// GetAmmoStat: shells/bullets/rockets/cells are engine stats, fuel and
	// plasma registered ones; RES_NONE is a weapon that uses no ammo.
	ed = global_edict(prog, "STAT_FUEL");
	if (ed)
		stat_fuel = field_int(prog, ed, f_id);
	ed = global_edict(prog, "STAT_PLASMA");
	if (ed)
		stat_plasma = field_int(prog, ed, f_id);
	ed = global_edict(prog, "RES_NONE");
	if (ed)
		res_none = PRVM_NUM_FOR_EDICT(ed);
	g_viewmodels = PRVM_ED_FindGlobal(prog, "viewmodels");
	if (g_viewmodels && def_type(g_viewmodels) == ev_entity)
		viewmodels_ofs = g_viewmodels->ofs;
	f_switchweapon = qcfield(prog, "switchweapon");
	f_activeweapon = qcfield(prog, "activeweapon");
	f_clip_load = qcfield(prog, "clip_load");
	f_clip_size = qcfield(prog, "clip_size");
	f_wep_id = qcfield(prog, "m_id");
	// The registry's linked list, in the order the CSQC's own FOREACH walks it.
	for (ed = global_edict(prog, "Weapons_first"), guard = 0; ed && guard < 256 && num_weapons < MAX_WEAPONS; guard++)
	{
		int id = field_int(prog, ed, f_id);
		int impulse = field_int(prog, ed, f_impulse);
		if (id > 0 && impulse >= 0)
		{
			wepdef_t *wd = &weapons[num_weapons++];
			wd->id = id;
			wd->impulse = impulse;
			strlcpy(wd->name, field_string(prog, ed, f_name), sizeof(wd->name));
			strlcpy(wd->pic, field_string(prog, ed, f_pic), sizeof(wd->pic));
			wd->ammo_stat = -1;
			wd->ammo_icon[0] = 0;
			if (f_ammo_type)
			{
				int res = PRVM_EDICTFIELDEDICT(ed, f_ammo_type->ofs);
				prvm_edict_t *re = (res != res_none) ? edict_or_null(prog, res) : NULL;
				if (re)
				{
					const char *kind = field_string(prog, re, f_name);
					if (!strcmp(kind, "shells")) wd->ammo_stat = STAT_SHELLS;
					else if (!strcmp(kind, "bullets")) wd->ammo_stat = STAT_NAILS;
					else if (!strcmp(kind, "rockets")) wd->ammo_stat = STAT_ROCKETS;
					else if (!strcmp(kind, "cells")) wd->ammo_stat = STAT_CELLS;
					else if (!strcmp(kind, "fuel")) wd->ammo_stat = stat_fuel;
					else if (!strcmp(kind, "plasma")) wd->ammo_stat = stat_plasma;
					if (wd->ammo_stat >= MAX_CL_STATS)
						wd->ammo_stat = -1;
					strlcpy(wd->ammo_icon, field_string(prog, re, f_ammo_icon), sizeof(wd->ammo_icon));
				}
			}
		}
		ed = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ed, f_next->ofs));
	}
	Con_DPrintf("Touch assists: %d weapons from the CSQC, WEAPONS stat %d\n", num_weapons, stat_weapons);
}

void TouchHUD_ProgsChanged(void)
{
	bind_weapons();
}

// The wepent (viewmodels[0]) of the loaded CSQC, or NULL.
static prvm_edict_t *wepent(void)
{
	prvm_prog_t *prog = CLVM_prog;
	if (viewmodels_ofs < 0 || !prog->loaded)
		return NULL;
	return edict_or_null(prog, PRVM_GLOBALFIELDEDICT(viewmodels_ofs));
}

// Index in weapons[] of the weapon being switched to (held, once the switch
// lands), like the CSQC's wepent.switchweapon; -1 when the CSQC does not say.
static int held_weapon_index(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent(), *we = NULL;
	int id, i;
	if (!ve)
		return -1;
	if (f_switchweapon.ofs >= 0)
		we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_switchweapon.ofs));
	if (!we && f_activeweapon.ofs >= 0)
		we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_activeweapon.ofs));
	if (!we)
		return -1;
	id = qcfield_int(prog, we, f_wep_id);
	for (i = 0; i < num_weapons; i++)
		if (weapons[i].id == id)
			return i;
	return -1;
}

// WepSet_FromWeapon: bit id-1 of the first stat for ids 1..24, then the next two.
static qbool weapon_owned(int index)
{
	int bit = weapons[index].id - 1;
	if (stat_weapons < 0 || bit < 0)
		return false;
	if (bit < 24)
		return (cl.stats[stat_weapons] & (1 << bit)) != 0;
	if (bit < 48)
		return (cl.stats[stat_weapons + 1] & (1 << (bit - 24))) != 0;
	return (cl.stats[stat_weapons + 2] & (1 << (bit - 48))) != 0;
}


// What +fire2 would do now (TOUCH_ALTFIRE_*): the server's stat when it sends
// one, else judged from the held weapon and its ammo (touch_altfire.h).
static int altfire_state(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve;
	const wepdef_t *wd;
	int held, reserve = 0, clip = -1;
	if (stat_altfire >= 0)
		return cl.stats[stat_altfire];
	if (cl.stats[STAT_HEALTH] <= 0)
		return TOUCH_ALTFIRE_NONE;
	held = held_weapon_index();
	if (held < 0)
		return TOUCH_ALTFIRE_NONE;
	wd = &weapons[held];
	if (wd->ammo_stat >= 0)
		reserve = max(0, cl.stats[wd->ammo_stat]);
	ve = wepent();
	if (ve && qcfield_int(prog, ve, f_clip_size) > 0)
		clip = qcfield_int(prog, ve, f_clip_load);
	return TouchAltFire_Fallback(wd->name, reserve, clip,
		(cl.stats[STAT_ITEMS] & 1) || wd->ammo_stat < 0); // IT_UNLIMITED_AMMO
}

// No engine-side HUD here: Insanotic's touch controls are a Lua mod.
qbool TouchHUD_Active(void)
{
	return false;
}
// ---------------------------------------------------------------------------
// Read-only weapon state for touch auto-shoot (touch_aim.c), from the same
// binding of the loaded CSQC the weapon strip uses, so it holds under the
// port's CSQC and a public server's alike.
// ---------------------------------------------------------------------------
int TouchHUD_NumWeapons(void)
{
	return num_weapons;
}

const char *TouchHUD_WeaponName(int index)
{
	return index >= 0 && index < num_weapons ? weapons[index].name : "";
}

qbool TouchHUD_WeaponOwned(int index)
{
	return index >= 0 && index < num_weapons && weapon_owned(index);
}

int TouchHUD_WeaponAmmo(int index)
{
	const wepdef_t *wd;
	if (index < 0 || index >= num_weapons)
		return 0;
	wd = &weapons[index];
	if (wd->ammo_stat < 0 || (cl.stats[STAT_ITEMS] & 1)) // IT_UNLIMITED_AMMO
		return -1;
	return max(0, cl.stats[wd->ammo_stat]);
}

int TouchHUD_HeldWeapon(void)
{
	return held_weapon_index();
}

// The weapon in hand once a switch has landed (the wepent's activeweapon;
// switchweapon changes as soon as the server takes the request).
int TouchHUD_ActiveWeapon(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent(), *we;
	int id, i;
	if (!ve || f_activeweapon.ofs < 0)
		return -1;
	we = edict_or_null(prog, PRVM_EDICTFIELDEDICT(ve, f_activeweapon.ofs));
	if (!we)
		return -1;
	id = qcfield_int(prog, we, f_wep_id);
	for (i = 0; i < num_weapons; i++)
		if (weapons[i].id == id)
			return i;
	return -1;
}

int TouchHUD_HeldClip(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_edict_t *ve = wepent();
	if (!ve || qcfield_int(prog, ve, f_clip_size) <= 0)
		return -1;
	return qcfield_int(prog, ve, f_clip_load);
}

int TouchHUD_AltFireState(void)
{
	return altfire_state();
}

