// Ported from Xonotic Touch (dixonSolutions, commit b73fe57); see NOTICE-touch.md.
// Auto walk: navigation only (nearest useful pickup over safe ground, along
// the bot waypoint graph). Never reads other players. See touch_autowalk.c.
#ifndef TOUCH_AUTOWALK_H
#define TOUCH_AUTOWALK_H

#include "touch_compat.h"

void TouchAutowalk_Init(void);
// Once a client frame, from the hook's frame before auto dodge.
void TouchAutowalk_Frame(void);
// Button bits for the outgoing move (a jump over a low obstacle).
int TouchAutowalk_Buttons(void);

#endif
