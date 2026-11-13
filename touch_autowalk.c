// Ported from Xonotic Touch (https://github.com/dixonSolutions/Xonotic-Touch, commit b73fe57) by dixonSolutions;
// adapted to Insanotic's DarkPlaces base (touch_compat.h). See NOTICE-touch.md.
/*
Auto walk: the navigation half of Xonotic Touch's offline autopilot.

While the stick is idle, the move heads for the nearest pickup worth having
(a weapon not owned, health when hurt, armour when short), along the map's bot
waypoint graph when the pickup is out of sight, over safe ground (no void,
lava or slime), jumping low steps and getting unstuck.

What it does not do, on purpose: it never reads another player. No enemy
health, weapon, position or aim, no fighting, no target choice. It reads the
player's own state and the map's pickups and waypoints from the local server,
so it works only in a game this process hosts. It runs only when
cl_touch_autowalk is 1 and the server allows assists (sv_allow_assist 1).
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "touch_compat.h"
#include "touch_assist.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_hud.h"
#include "touch_autowalk.h"

extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;

cvar_t cl_touch_autowalk = {CVAR_SAVE, "cl_touch_autowalk", "0",
	"Walk for you while the stick is idle: to the nearest pickup worth having, over safe ground. Needs sv_allow_assist 1 and a game this machine hosts. "
	"Never looks at other players. Touching the stick takes over"};

#define TT_EVAL 0.1     // seconds between direction choices
#define TT_DIRS 16
#define TT_LOOK 192.0f  // units of room looked for in each direction

static struct
{
	qboolean open;
	int me;
	vec3_t my_org, my_eye, my_vel;
	// steering
	int buttons;
	qboolean steering;
	double eval_next;
	vec3_t move_dir;
	qboolean jump_dir;
	double slow_since, stuck_until;
	vec3_t stuck_dir;
	const char *mode;
	int stucks;
	// pickups
	double items_next;
	int item;
	vec3_t item_org;
	vec3_t last_safe;
	qboolean have_safe;
	double item_since;
	float item_best_dist;
	int item_ban[8];
	double item_ban_until[8];
	int pickups_tried;
} tt;

static const char *sv_string(prvm_prog_t *prog, prvm_edict_t *ed, int ofs)
{
	const char *s = PRVM_GetString(prog, PRVM_EDICTFIELDVALUE(ed, ofs)->string);
	return s ? s : "";
}

static qboolean line_clear(const vec3_t a, const vec3_t b)
{
	trace_t tr = CL_TraceLine(a, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

static qboolean hazard_below(const vec3_t p, float floor_z)
{
	const int bad = SUPERCONTENTS_LAVA | SUPERCONTENTS_SLIME | SUPERCONTENTS_NODROP;
	vec3_t at;
	float z;
	// every 12 units from just over the floor up to p: a liquid surface lies
	// somewhere in between, not always at the floor or halfway
	VectorCopy(p, at);
	for (z = floor_z + 4; z < p[2]; z += 12)
	{
		at[2] = z;
		if (CL_PointSuperContents(at) & bad)
			return true;
	}
	at[2] = p[2];
	return (CL_PointSuperContents(at) & bad) != 0;
}

static qboolean safe_ground(const vec3_t p)
{
	static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
	vec3_t down;
	trace_t box, thin;
	VectorCopy(p, down);
	down[2] -= 200;
	box = CL_TraceBox(p, mins, maxs, down, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false);
	// Straight down from the centre: the full box can rest on the lip of a
	// trench, or start inside a wall it hugs, while the player's middle hangs
	// over lava (Stormkeep's lava trench runs along a wall).
	thin = CL_TraceLine(p, down, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false, false);
	if (thin.startsolid)
		return box.startsolid || box.fraction < 1; // inside a wall: the run check has it
	if (hazard_below(p, thin.fraction < 1 ? thin.endpos[2] : down[2]))
		return false;
	if (!box.startsolid && box.fraction < 1 && hazard_below(p, box.endpos[2]))
		return false;
	return box.startsolid || box.fraction < 1 || thin.fraction < 1;
}

// Room to move along dir from the player's feet: the box's free run, whether
// there is ground (no void, lava or slime) at its end, and whether a jump
// clears what stops it.
static float room(const vec3_t dir, qboolean *ground, qboolean *jump)
{
	static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
	vec3_t start, end;
	trace_t tr, tr2;
	float run;
	VectorCopy(tt.my_org, start);
	start[2] += -24 + 20; // feet, plus a step
	VectorMA(start, TT_LOOK, dir, end);
	tr = CL_TraceBox(start, mins, maxs, end, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false);
	run = tr.startsolid ? 0 : tr.fraction * TT_LOOK;
	*jump = false;
	if (run < 48 && !tr.startsolid)
	{
		vec3_t js, je;
		VectorCopy(start, js);
		js[2] += 40;
		VectorMA(js, 64, dir, je);
		tr2 = CL_TraceBox(js, mins, maxs, je, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
			collision_extendmovelength.value, true, false, NULL, false);
		// a jump only onto safe ground on the far side
		*jump = !tr2.startsolid && tr2.fraction > 0.95f && safe_ground(je);
	}
	// ground along the run: a third of the way, two thirds, and its end (a
	// short step where there is no run), so a pit or a lava pool is seen
	// before the player is over it
	{
		int k;
		*ground = true;
		for (k = 1; k <= 3 && *ground; k++)
		{
			VectorMA(start, max(run * k / 3.0f, 24.0f), dir, end);
			*ground = safe_ground(end);
		}
	}
	return run;
}

static void flat_norm(vec3_t v)
{
	float l;
	v[2] = 0;
	l = sqrt(v[0] * v[0] + v[1] * v[1]);
	if (l > 0.001f)
		VectorScale(v, 1 / l, v);
}

// ---------------------------------------------------------------------------
// Routes over the bots' waypoint graph (local server)
// ---------------------------------------------------------------------------
// Xonotic's bots navigate by waypoint entities (classname "waypoint"), each
// with up to 32 outgoing links (.wp00-.wp31) and their costs
// (.wp00mincost-...). The graph is read once per map; a route is the
// cheapest chain of links from the waypoint nearest the player (in clear
// view) to the one nearest the goal (in clear view of it), and the move
// heads for the next waypoint on it.
static int field_ofs(prvm_prog_t *prog, const char *name, int *is_float)
{
	ddef_t *d = PRVM_ED_FindField(prog, name);
	if (!d)
		return -1;
	*is_float = (d->type & ~DEF_SAVEGLOBAL) == ev_float;
	return d->ofs;
}

#define WP_MAX 2048
#define WP_LINKS 32
static struct
{
	int n;
	vec3_t org[WP_MAX];
	short link[WP_MAX][WP_LINKS];
	float cost[WP_MAX][WP_LINKS];
	unsigned char nl[WP_MAX];
	const void *defs;
	int edicts;
	double checked;
	// the route being followed
	short path[WP_MAX];
	int plen, pnext;
	vec3_t goal;
	double planned, node_since;
} wpg;

static void wp_build(prvm_prog_t *prog)
{
	static short idx[MAX_EDICTS];
	int ofs_link[WP_LINKS], ofs_cost[WP_LINKS], i, k, isf;
	char name[16];
	wpg.n = 0;
	wpg.plen = 0;
	for (k = 0; k < WP_LINKS; k++)
	{
		dpsnprintf(name, sizeof(name), "wp%02d", k);
		ofs_link[k] = PRVM_ED_FindFieldOffset(prog, name);
		dpsnprintf(name, sizeof(name), "wp%02dmincost", k);
		ofs_cost[k] = field_ofs(prog, name, &isf);
		if (ofs_link[k] < 0)
			return;
	}
	for (i = 0; i < prog->num_edicts && i < MAX_EDICTS; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		idx[i] = -1;
		if (ed->priv.required->free || wpg.n >= WP_MAX || strcmp(sv_string(prog, ed, prog->fieldoffsets.classname), "waypoint"))
			continue;
		idx[i] = (short)wpg.n;
		VectorAdd(PRVM_serveredictvector(ed, absmin), PRVM_serveredictvector(ed, absmax), wpg.org[wpg.n]);
		VectorScale(wpg.org[wpg.n], 0.5f, wpg.org[wpg.n]);
		wpg.n++;
	}
	for (i = 0; i < prog->num_edicts && i < MAX_EDICTS; i++)
	{
		prvm_edict_t *ed;
		int w = idx[i];
		if (w < 0)
			continue;
		ed = PRVM_EDICT_NUM(i);
		wpg.nl[w] = 0;
		for (k = 0; k < WP_LINKS; k++)
		{
			int to = PRVM_EDICTFIELDVALUE(ed, ofs_link[k])->edict;
			if (to <= 0 || to >= prog->num_edicts || to >= MAX_EDICTS || idx[to] < 0)
				continue;
			wpg.link[w][wpg.nl[w]] = idx[to];
			wpg.cost[w][wpg.nl[w]] = ofs_cost[k] >= 0 && ed->fields.fp[ofs_cost[k]] > 0 ? ed->fields.fp[ofs_cost[k]]
				: VectorDistance(wpg.org[w], wpg.org[idx[to]]) / 360; // seconds at run speed, as the bots count
			wpg.nl[w]++;
		}
	}
}

// The graph for the map now loaded (rebuilt when the progs or edict count change).
static void wp_check(prvm_prog_t *prog, double now)
{
	if (wpg.defs == (const void *)prog->fielddefs && wpg.edicts == prog->num_edicts && (wpg.n || now < wpg.checked))
		return;
	wpg.defs = prog->fielddefs;
	wpg.edicts = prog->num_edicts;
	wpg.checked = now + 5;
	wp_build(prog);
}

// The waypoint nearest p that p sees (of the 8 nearest), -1 none.
static int wp_near(const vec3_t p)
{
	int best[8], i, k, n = 0;
	float bd[8];
	for (i = 0; i < wpg.n; i++)
	{
		float d = VectorDistance(p, wpg.org[i]);
		if (n < 8 || d < bd[n - 1])
		{
			k = n < 8 ? n++ : 7;
			while (k > 0 && bd[k - 1] > d)
			{
				bd[k] = bd[k - 1];
				best[k] = best[k - 1];
				k--;
			}
			bd[k] = d;
			best[k] = i;
		}
	}
	for (k = 0; k < n; k++)
		if (line_clear(p, wpg.org[best[k]]))
			return best[k];
	return -1;
}

// Cheapest route from waypoint a to b (Dijkstra, binary heap) into wpg.path.
static qboolean wp_route(int a, int b)
{
	static float dist[WP_MAX];
	static short prev[WP_MAX], heap[WP_MAX * 4];
	static unsigned char done[WP_MAX];
	int hn = 0, i, u, n;
	for (i = 0; i < wpg.n; i++)
	{
		dist[i] = 1e30f;
		prev[i] = -1;
		done[i] = 0;
	}
	dist[a] = 0;
	heap[hn++] = (short)a;
	while (hn)
	{
		// pop the nearest
		int c = 0, j;
		u = heap[0];
		heap[0] = heap[--hn];
		for (;;)
		{
			int l = 2 * c + 1, r = l + 1, m = c;
			if (l < hn && dist[heap[l]] < dist[heap[m]]) m = l;
			if (r < hn && dist[heap[r]] < dist[heap[m]]) m = r;
			if (m == c) break;
			j = heap[c]; heap[c] = heap[m]; heap[m] = (short)j; c = m;
		}
		if (done[u])
			continue;
		done[u] = 1;
		if (u == b)
			break;
		for (i = 0; i < wpg.nl[u]; i++)
		{
			int v = wpg.link[u][i];
			float nd = dist[u] + wpg.cost[u][i];
			if (nd < dist[v] && hn < WP_MAX * 4)
			{
				dist[v] = nd;
				prev[v] = (short)u;
				// push
				c = hn;
				heap[hn++] = (short)v;
				while (c > 0 && dist[heap[(c - 1) / 2]] > dist[heap[c]])
				{
					j = heap[c]; heap[c] = heap[(c - 1) / 2]; heap[(c - 1) / 2] = (short)j; c = (c - 1) / 2;
				}
			}
		}
	}
	if (!done[b])
		return false;
	for (n = 0, u = b; u >= 0 && n < WP_MAX; u = prev[u])
		n++;
	wpg.plen = n;
	for (u = b; u >= 0 && n > 0; u = prev[u])
		wpg.path[--n] = (short)u;
	wpg.pnext = 0;
	return true;
}

// Toward goal along the waypoint route (replanned every second, when the
// goal moves, or when a waypoint is not reached in 4 s); straight when the
// goal is in clear view or there is no route. False with no way at all.
static qboolean route_to(double now, const vec3_t goal, vec3_t want)
{
	vec3_t eye;
	VectorCopy(goal, eye);
	eye[2] += 16;
	if (!wpg.n || line_clear(tt.my_eye, eye))
	{
		VectorSubtract(goal, tt.my_org, want);
		flat_norm(want);
		return true;
	}
	if (now - wpg.planned > 1 || VectorDistance(goal, wpg.goal) > 200 || now - wpg.node_since > 4 || wpg.pnext >= wpg.plen)
	{
		int a = wp_near(tt.my_eye), b = wp_near(eye);
		wpg.plen = 0;
		wpg.planned = now;
		wpg.node_since = now;
		VectorCopy(goal, wpg.goal);
		if (a < 0 || b < 0 || !wp_route(a, b))
		{
			VectorSubtract(goal, tt.my_org, want);
			flat_norm(want);
			return true;
		}
	}
	// at the next waypoint, or the one after it in clear view: on
	while (wpg.pnext < wpg.plen)
	{
		const float *w = wpg.org[wpg.path[wpg.pnext]];
		float dxy = sqrt((w[0] - tt.my_org[0]) * (w[0] - tt.my_org[0]) + (w[1] - tt.my_org[1]) * (w[1] - tt.my_org[1]));
		qboolean reached = dxy < 48 && fabs(w[2] - tt.my_org[2]) < 72;
		qboolean skip = wpg.pnext + 1 < wpg.plen && VectorDistance(wpg.org[wpg.path[wpg.pnext + 1]], tt.my_org) < 500
			&& line_clear(tt.my_eye, wpg.org[wpg.path[wpg.pnext + 1]]) && fabs(wpg.org[wpg.path[wpg.pnext + 1]][2] - tt.my_org[2]) < 72;
		if (!reached && !skip)
			break;
		wpg.pnext++;
		wpg.node_since = now;
	}
	if (wpg.pnext >= wpg.plen)
		VectorSubtract(goal, tt.my_org, want);
	else
		VectorSubtract(wpg.org[wpg.path[wpg.pnext]], tt.my_org, want);
	flat_norm(want);
	return true;
}

// Toward p (flat), or held still within `there` units.
static void go_to(const vec3_t p, float there, vec3_t want)
{
	VectorSubtract(p, tt.my_org, want);
	want[2] = 0;
	if (VectorLength(want) < there)
		VectorClear(want);
	else
		route_to(realtime, p, want);
}

static qboolean owns(const char *netname)
{
	int i;
	for (i = 0; i < TouchHUD_NumWeapons(); i++)
		if (TouchHUD_WeaponOwned(i) && !strcmp(TouchHUD_WeaponName(i), netname))
			return true;
	return false;
}

// What a pickup is worth to the player now: a weapon not owned, health when
// hurt, armour when short of it.
static float item_value(const char *cname, float health, float armor)
{
	if (!strncmp(cname, "weapon_", 7))
	{
		const char *w = cname + 7;
		if (owns(w))
			return 0;
		return !strcmp(w, "vortex") || !strcmp(w, "devastator") || !strcmp(w, "vaporizer") || !strcmp(w, "electro") ? 3 : 2;
	}
	if (!strncmp(cname, "item_health", 11))
	{
		if (strstr(cname, "mega"))
			return health < 200 ? 3 : 0;
		return health < 100 ? (100 - health) / 25 : 0;
	}
	if (!strncmp(cname, "item_armor", 10))
	{
		if (strstr(cname, "mega") || strstr(cname, "large") || strstr(cname, "big"))
			return armor < 150 ? 2 : 0;
		return armor < 100 ? 0.5f : 0;
	}
	return 0;
}

static qboolean item_banned(int e, double now)
{
	int i;
	for (i = 0; i < 8; i++)
		if (tt.item_ban[i] == e && now < tt.item_ban_until[i])
			return true;
	return false;
}

// The pickup to fetch: the best value for the way, on about the player's
// level (no route finding: one that needs a jump pad or a lift is left).
static void pick_item(double now)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *me = PRVM_EDICT_NUM(tt.me);
	float health = PRVM_serveredictfloat(me, health), armor = PRVM_serveredictfloat(me, armorvalue), bs = 0.6f;
	int i, best = 0;
	vec3_t bo = { 0, 0, 0 };
	for (i = svs.maxclients + 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		const char *cname;
		float v, d, dz;
		vec3_t o;
		if (ed->priv.required->free || PRVM_serveredictfloat(ed, solid) != SOLID_TRIGGER)
			continue;
		cname = sv_string(prog, ed, prog->fieldoffsets.classname);
		if (strncmp(cname, "weapon_", 7) && strncmp(cname, "item_", 5))
			continue;
		v = item_value(cname, health, armor);
		if (v <= 0 || item_banned(i, now))
			continue;
		VectorCopy(PRVM_serveredictvector(ed, origin), o);
		dz = fabs(o[2] - (tt.my_org[2] - 24));
		d = VectorDistance(o, tt.my_org);
		if (d > 2500 || dz > 120)
			continue;
		{
			// not one standing in or over lava or slime
			vec3_t q;
			VectorCopy(o, q);
			q[2] += 20;
			if (!safe_ground(q))
				continue;
			// nor one at the edge of a pit: the approach slides in
			{
				int k;
				qboolean edge = false;
				for (k = 0; k < 8 && !edge; k++)
				{
					vec3_t r;
					VectorSet(r, q[0] + 80 * cos(k * M_PI / 4), q[1] + 80 * sin(k * M_PI / 4), q[2]);
					edge = !safe_ground(r);
				}
				if (edge)
					continue;
			}
		}
		v /= 1 + d / 600;
		if (i == tt.item)
			v *= 1.3f; // keep the one on the way
		if (v > bs)
		{
			bs = v;
			best = i;
			VectorCopy(o, bo);
		}
	}
	if (best != tt.item)
	{
		tt.item = best;
		tt.item_since = now;
		tt.item_best_dist = 1e9f;
		if (best)
			tt.pickups_tried++;
		if (best && cl_touch_autowalk.integer > 1)
			Con_Printf("touch autowalk: fetch %s at %.0f %.0f %.0f, %.0f units\n", sv_string(prog, PRVM_EDICT_NUM(best), prog->fieldoffsets.classname), bo[0], bo[1], bo[2], VectorDistance(bo, tt.my_org));
	}
	VectorCopy(bo, tt.item_org);
}

// Fetching a pickup that gets no nearer for 4 seconds: leave it for 20.
static void item_progress(double now)
{
	float d;
	int i, slot = 0;
	if (!tt.item)
		return;
	d = VectorDistance(tt.item_org, tt.my_org);
	if (d < tt.item_best_dist - 32)
	{
		tt.item_best_dist = d;
		tt.item_since = now;
		return;
	}
	if (now - tt.item_since < 4)
		return;
	for (i = 0; i < 8; i++)
		if (now >= tt.item_ban_until[i])
			slot = i;
	tt.item_ban[slot] = tt.item;
	tt.item_ban_until[slot] = now + 20;
	if (cl_touch_autowalk.integer > 1)
		Con_Printf("touch autowalk: pickup %d out of reach, leaving it\n", tt.item);
	tt.item = 0;
}


// Of 16 directions, the one that goes most where `want` points, has room and
// ground, and keeps the course (no jitter). Standing over lava or slime, the
// way back to the last safe spot beats everything else.
static void choose(const vec3_t want)
{
	int k, best = -1;
	float bs = -1e9f;
	vec3_t bdir = { 0, 0, 0 };
	qboolean bjump = false;
	vec3_t feet, back = { 0, 0, 0 };
	VectorCopy(tt.my_org, feet);
	feet[2] += -24 + 20;
	if (safe_ground(feet))
	{
		VectorCopy(tt.my_org, tt.last_safe);
		tt.have_safe = true;
	}
	else if (tt.have_safe)
	{
		VectorSubtract(tt.last_safe, tt.my_org, back);
		back[2] = 0;
		VectorNormalize(back);
	}
	if (VectorLength2(back) == 0)
	{
		// sliding toward a hazard: where momentum carries the player in 0.35 s must be safe too
		vec3_t ahead;
		VectorMA(feet, 0.35f, tt.my_vel, ahead);
		ahead[2] = feet[2];
		if (!safe_ground(ahead))
		{
			VectorSet(back, -tt.my_vel[0], -tt.my_vel[1], 0);
			VectorNormalize(back);
		}
	}
	for (k = 0; k < TT_DIRS; k++)
	{
		float a = k * (2 * M_PI / TT_DIRS), run, s;
		vec3_t dir = { cos(a), sin(a), 0 };
		qboolean ground, jump;
		run = room(dir, &ground, &jump);
		s = 2 * DotProduct(dir, want) + 1.5f * (jump ? 1 : run / TT_LOOK) + 0.5f * DotProduct(dir, tt.move_dir);
		if (!ground)
			s -= 6;
		s += 5 * DotProduct(dir, back);
		if (run < 24 && !jump)
			s -= 3;
		if (s > bs)
		{
			bs = s;
			best = k;
			VectorCopy(dir, bdir);
			bjump = jump;
		}
	}
	if (best < 0)
		return;
	VectorCopy(bdir, tt.move_dir);
	tt.jump_dir = bjump;
}

static void steer(double now)
{
	float own_f = cl.cmd.forwardmove / max(1.0f, cl_forwardspeed.value), own_s = cl.cmd.sidemove / max(1.0f, cl_sidespeed.value);
	qboolean idle = own_f * own_f + own_s * own_s < 0.01f;
	vec3_t want, ang, fwd, right;
	float speed;

	tt.buttons = 0;
	tt.mode = NULL;
	if (now >= tt.items_next)
	{
		pick_item(now);
		tt.items_next = now + 0.5;
	}
	item_progress(now);
	if (!idle || TouchHook_InUse() || TouchHook_Rescuing() || !tt.item)
	{
		tt.steering = false;
		tt.slow_since = 0;
		return;
	}
	tt.mode = "fetch";
	VectorClear(want);
	go_to(tt.item_org, 24, want);
	if (VectorLength2(want) == 0)
	{
		tt.steering = false;
		tt.slow_since = 0;
		return;
	}

	// stuck against something the traces missed: somewhere else for a moment
	speed = sqrt(tt.my_vel[0] * tt.my_vel[0] + tt.my_vel[1] * tt.my_vel[1]);
	if (tt.steering && speed < 40)
	{
		if (!tt.slow_since)
			tt.slow_since = now;
		else if (now - tt.slow_since > 0.6 && now >= tt.stuck_until)
		{
			float a = lhrandom(0, 2 * M_PI);
			VectorSet(tt.stuck_dir, cos(a), sin(a), 0);
			tt.stuck_until = now + 0.6;
			tt.slow_since = 0;
			tt.stucks++;
		}
	}
	else
		tt.slow_since = 0;
	if (now < tt.stuck_until)
	{
		VectorCopy(tt.stuck_dir, want);
		tt.mode = "unstuck";
	}
	if (now >= tt.eval_next || !tt.steering)
	{
		choose(want);
		tt.eval_next = now + TT_EVAL;
	}
	tt.steering = true;

	VectorSet(ang, 0, cl.viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, NULL);
	cl.cmd.forwardmove = DotProduct(tt.move_dir, fwd) * cl_forwardspeed.value;
	cl.cmd.sidemove = DotProduct(tt.move_dir, right) * cl_sidespeed.value;
	if (tt.jump_dir && cl.onground)
		tt.buttons |= 2;
}

// The player's own state from the local server (the client of this process).
static qboolean read_self(void)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *me;
	if (!sv.active || !prog->loaded || cl.playerentity < 1 || cl.playerentity > svs.maxclients)
		return false;
	tt.me = cl.playerentity;
	me = PRVM_EDICT_NUM(tt.me);
	if (PRVM_serveredictfloat(me, health) <= 0)
	{
		tt.have_safe = false;
		return false;
	}
	VectorCopy(PRVM_serveredictvector(me, origin), tt.my_org);
	VectorCopy(PRVM_serveredictvector(me, velocity), tt.my_vel);
	VectorAdd(tt.my_org, PRVM_serveredictvector(me, view_ofs), tt.my_eye);
	return true;
}

void TouchAutowalk_Frame(void)
{
	double now = realtime;
	tt.buttons = 0;
	tt.open = cl_touch_autowalk.integer && cls.state == ca_connected && !cls.demoplayback
		&& TouchAssist_Allowed(TOUCH_ASSIST_WALK) && !TouchAim_BlockReason() && read_self();
	if (!tt.open)
	{
		tt.item = 0;
		tt.steering = false;
		return;
	}
	wp_check(SVVM_prog, now);
	steer(now);
}

int TouchAutowalk_Buttons(void)
{
	return tt.open ? tt.buttons : 0;
}

static void TouchAutowalk_Status_f(void)
{
	Con_Printf("touch autowalk: cl_touch_autowalk %d, assist %d, open %d; mode %s; waypoints %d, route %d of %d; pickups fetched %d, unstuck %d\n",
		cl_touch_autowalk.integer, TouchAssist_Allowed(TOUCH_ASSIST_WALK), tt.open, tt.mode ? tt.mode : "idle", wpg.n, wpg.pnext, wpg.plen,
		tt.pickups_tried, tt.stucks);
}

void TouchAutowalk_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_autowalk);
	Cmd_AddCommand("touch_autowalk_status", TouchAutowalk_Status_f, "Auto walk: whether it is on, what it fetches, and the waypoint route");
}
