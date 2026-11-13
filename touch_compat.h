// Backport shims: Xonotic Touch's engine code is written against current
// Xonotic DarkPlaces; Insanotic's engine is the older 0.8.6 DarkPlaces.
// Original code: Xonotic Touch (https://github.com/dixonSolutions/Xonotic-Touch).
#ifndef TOUCH_COMPAT_H
#define TOUCH_COMPAT_H
#include "qtypes.h"
typedef qboolean qbool;
#define CF_CLIENT 0
#define CF_SERVER 0
#define CF_CLIENT_FROM_SERVER 0
#define CF_ARCHIVE CVAR_SAVE
#define CF_READONLY CVAR_READONLY
#define dp_strlcpy strlcpy
#define model_t dp_model_t
#define mdef_t ddef_t
#endif
