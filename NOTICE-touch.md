# Code from Xonotic Touch

The `touch_*.c` / `touch_*.h` files in this directory are ported from
**Xonotic Touch** (https://github.com/dixonSolutions/Xonotic-Touch, commit
`b73fe57`), copyright its authors (dixonSolutions and contributors). Xonotic
Touch declares GPL-3.0-or-later (its Flatpak metainfo and launcher sources);
several of its headers (`touch_altfire.h`, `touch_dodge.h`, `touch_ledge.h`,
`touch_rescue.h`, `touch_weapons.h`) say GPL-2.0-or-later. We use all of it under
GPL-3.0-or-later, like the rest of Insanotic. Their notices stay in the files.

What was changed for Insanotic:

- `touch_compat.h` adapts their code to this engine's older DarkPlaces API
  (`qbool`, command and cvar flags, command signatures).
- `touch_assist.c` is rewritten: every assist (aim pull, auto-shoot, automatic
  weapon switching, auto dodge, ledge guard, hook fall rescue) runs only on a
  server with `sv_allow_assist 1`, which is off by default.
- `touch_tactics.c` is a stub. Their offline tactics read every enemy's health,
  weapon and position from the local server; that is not ported. The
  see-through, full-360 and instant-turn "practice" helpers are switched off.
- `touch_wepquery.c` holds only the part of their `touch_hud.c` the assists need
  (the CSQC weapon registry). Their engine-side HUD is not part of Insanotic.
- `touch_autowalk.c` is the navigation half of their offline autopilot
  (`touch_tactics.c`): pickup choice, safe-ground checks, 16-direction steering
  and the bot waypoint route. It never reads another player, never fights, and
  runs only with `cl_touch_autowalk 1` (default 0) and `sv_allow_assist 1`.
