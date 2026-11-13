#ifndef TOUCH_ASSIST_H
#define TOUCH_ASSIST_H

#include "touch_compat.h"

// Which touch assists a server allows. Insanotic has one server permission,
// sv_allow_assist (default 0): a server that does not set it to 1 gets none of
// these, and neither does a stock Xonotic server, which never says anything.
#define TOUCH_ASSIST_AIM        1 // aim pull and friction
#define TOUCH_ASSIST_AUTOSHOOT  2 // auto-shoot
#define TOUCH_ASSIST_AUTOSWITCH 4 // automatic weapon switching
#define TOUCH_ASSIST_MOVE       8 // auto dodge, ledge guard, hook fall rescue
#define TOUCH_ASSIST_WALK       16 // auto walk (cl_touch_autowalk)
#define TOUCH_ASSIST_ALL        31

// Client: once a frame, forget the last server's word once disconnected.
void TouchAssist_Frame(void);
void TouchAssist_Init(void);
// Whether the server allows this assist (sv_allow_assist 1 on that server).
qbool TouchAssist_Allowed(int bit);

#endif
