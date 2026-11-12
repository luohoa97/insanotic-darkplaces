/*
 * Insanotic: RmlUi in the engine.
 *
 * ui_rml_engine.c is the C side (cvars, commands, hooks into the menu, input
 * and drawing, everything that needs the engine's headers). ui_rml.cpp is the
 * C++ side (RmlUi's render, file and system interfaces and the documents).
 * They only talk through this header, and the header uses nothing but plain C
 * types, so the C++ file never has to include an engine header.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_RML_H
#define UI_RML_H

#include <stddef.h>

/* Keys for UIRml_Key: printable keys are their lower case ASCII code (as the
 * engine passes them), the rest are these, numbered above any ASCII code. */
enum {
	UIKEY_ESCAPE = 256, UIKEY_TAB, UIKEY_RETURN, UIKEY_BACKSPACE, UIKEY_DELETE,
	UIKEY_INSERT, UIKEY_HOME, UIKEY_END, UIKEY_PAGEUP, UIKEY_PAGEDOWN,
	UIKEY_LEFT, UIKEY_RIGHT, UIKEY_UP, UIKEY_DOWN
};
/* modifier bits for the input functions */
enum { UIMOD_CTRL = 1, UIMOD_SHIFT = 2, UIMOD_ALT = 4 };

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Implemented in ui_rml_engine.c, used by the rest of the engine
 * --------------------------------------------------------------------- */

#ifdef CONFIG_MENU
/* cvars, commands and the renderer module; once, at startup */
void UI_RML_Init(void);
/* draw whatever documents are shown, on top of the menu; every frame */
void UI_RML_Draw(void);
/* a key or mouse button event for the menu. 1 when a document used it. */
int UI_RML_KeyEvent(int key, int ascii, int down);
/* 1 while a document other than the main menu is up: it gets all input */
int UI_RML_Modal(void);
/* the renderer is going away: drop everything that holds textures */
void UI_RML_Shutdown(void);
#else
/* the dedicated server has no menu and does not link RmlUi */
#define UI_RML_Init() ((void)0)
#define UI_RML_Draw() ((void)0)
#define UI_RML_KeyEvent(key, ascii, down) 0
#define UI_RML_Modal() 0
#define UI_RML_Shutdown() ((void)0)
#endif

/* ------------------------------------------------------------------------
 * Implemented in ui_rml_engine.c, used by ui_rml.cpp
 * --------------------------------------------------------------------- */

/* whole file from the engine's file system (pk3 aware), NULL when missing;
 * release with UIE_Free */
unsigned char *UIE_LoadFile(const char *path, size_t *size);
/* decoded image as premultiplied RGBA, NULL when missing; release with UIE_Free */
unsigned char *UIE_LoadImage(const char *path, int *width, int *height);
void UIE_Free(void *data);

/* a texture the engine can draw; pixels are premultiplied RGBA */
void *UIE_TextureCreate(const unsigned char *rgba, int width, int height, int mipmap);
void UIE_TextureFree(void *texture);

/* draw triangles; positions are window pixels (top left origin), colors are
 * premultiplied RGBA floats 0..1, texture may be NULL */
void UIE_DrawTriangles(const float *xy, const float *uv, const float *rgba, int numvertices, const int *indices, int numindices, void *texture);
/* scissor in window pixels, enable 0 turns it off */
void UIE_Scissor(int enable, int x, int y, int width, int height);

double UIE_Time(void);
/* level: 0 info, 1 warning or worse, 2 debug chatter */
void UIE_Log(int level, const char *text);
/* queue a console command line */
void UIE_Command(const char *command);
/* value of a cvar as text, empty when there is none */
const char *UIE_CvarString(const char *name);
/* whether a file is in the game files (packs, folders, downloaded packs) */
int UIE_FileExists(const char *path);
/* the screen sizes the video code offers */
typedef struct UIE_VideoMode { int width, height; } UIE_VideoMode;
int UIE_VideoModes(UIE_VideoMode *out, int max);
/* the (up to two) keys bound to a command, as names; empty when none */
void UIE_KeysFor(const char *command, char *key1, char *key2, int size);
/* bind a key to a command in the slot (0 or 1) the player picked; key
 * names it freed are unbound. A key of -1 clears the slot. */
void UIE_BindKey(const char *command, int slot, int key);
/* while on, the next key or mouse button press goes to UIRml_KeyCaptured
 * instead of anywhere else (Escape cancels) */
void UIE_CaptureKeys(int on);
/* the servers the network code has heard from, for the server browser; returns how many */
typedef struct UIE_Server {
	char cname[128], name[128], map[32], type[32];
	int ping, numplayers, numhumans, maxplayers, freeslots, isfavorite;
	int mappreview; /* the server can send a picture of its map */
} UIE_Server;
int UIE_ServerList(UIE_Server *out, int max);
/* what one server told besides the list columns; 0 when it is not in the list */
typedef struct UIE_ServerDetail {
	char players[2800]; /* lines of: score ping team "name" */
	char qcstatus[128], mod[32], game[32];
	int numbots, numhumans, maxplayers;
} UIE_ServerDetail;
int UIE_ServerDetail_Get(const char *cname, UIE_ServerDetail *out);
/* ask a server for the picture of its map (see netconn.c): 1 asked, 0 no room now, -1 will not work */
int UIE_MapPreviewRequest(const char *cname, const char *map);
/* ask the master servers again (reset empties the list first) */
void UIE_ServerRefresh(int reset);
/* names of the files matching a pattern, for the campaign list; returns how many */
int UIE_FileList(const char *pattern, char (*out)[128], int max);
/* set a cvar by name; read-only and private ones are left alone */
void UIE_CvarSet(const char *name, const char *value);
/* send a line to the server the way "cmd" does */
void UIE_ServerCommand(const char *line);
/* the on-screen keyboard of touch devices */
void UIE_Keyboard(int show);

/* ------------------------------------------------------------------------
 * Implemented in ui_rml.cpp, used by ui_rml_engine.c
 * --------------------------------------------------------------------- */

/* start RmlUi and make the context; 0 on failure (the reason is logged) */
int UIRml_Create(int width, int height, float dpratio);
void UIRml_Destroy(void);
void UIRml_Resize(int width, int height, float dpratio);

/* documents are named like their file: "mainmenu" is ui/mainmenu.rml.
 * Show loads on first use. */
int UIRml_Show(const char *name);
void UIRml_Hide(const char *name);
void UIRml_HideAll(void);
/* a page a server sent (a path in ui/); NULL when it is showing, else why not */
const char *UIRml_ShowServer(const char *path);
int UIRml_ServerShown(void);
int UIRml_IsShown(const char *name);
int UIRml_AnyShownExcept(const char *name);
/* close and load every document again (stylesheets and images too) */
void UIRml_Reload(void);
/* class on the document's body, for state the stylesheet reacts to */
void UIRml_SetBodyClass(const char *name, const char *cls, int on);
/* ids of the documents that are shown, for rml_list */
void UIRml_List(void (*print)(const char *line));
/* for tests: whether the element with that id is hovered or pressed */
void UIRml_State(const char *id, void (*print)(const char *line));
/* names of the documents that are shown */
void UIRml_EachShown(void (*each)(const char *name));

/* the key that was pressed while capturing, or -1 for Escape */
void UIRml_KeyCaptured(int key);

/* input: coordinates are window pixels. Return 1 when RmlUi used the event. */
/* 1 while the input is a finger: nothing hovers, the pressed state is all the feedback */
void UIRml_SetTouch(int touch);
void UIRml_MouseMove(float x, float y);
int UIRml_MouseButton(int button, int down, int modifiers, int touch);
int UIRml_MouseWheel(float delta, int modifiers);
int UIRml_Key(int key, int down, int modifiers);
int UIRml_Text(unsigned int unicode);
/* the pointer left the window or a finger lifted */
void UIRml_MouseLeave(void);
/* 1 while a text field has the keyboard */
int UIRml_TextFocus(void);
/* 1 while a finger or button is dragging in a document */
int UIRml_Dragging(void);

/* one frame: apply bindings, advance animations, draw everything shown */
void UIRml_Frame(void);

#ifdef __cplusplus
}
#endif

#endif
