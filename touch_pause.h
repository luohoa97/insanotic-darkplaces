// Ported from Xonotic Touch (https://github.com/dixonSolutions/Xonotic-Touch, commit b73fe57) by dixonSolutions;
// adapted to Insanotic's DarkPlaces base (touch_compat.h). See NOTICE-touch.md.
#ifndef TOUCH_PAUSE_H
#define TOUCH_PAUSE_H

// Xonotic Touch: what holds a local game's pause (touch_pause_status), and
// a per-frame record of its changes.
void TouchPause_Init(void);
void TouchPause_Frame(void);

#endif
