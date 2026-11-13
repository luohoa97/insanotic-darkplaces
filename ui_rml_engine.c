/*
 * Insanotic: RmlUi in the engine, C side. See ui_rml.h for how the two halves
 * fit together.
 *
 * The main menu is the one document the engine shows by itself. The QuakeC
 * menu keeps running underneath and tells the engine (cvar _menu_overview)
 * when it is showing its main screen, the one with the big buttons. Then, if
 * menu_rmlui is on, this file shows ui/mainmenu.rml instead and tells the
 * QuakeC menu (cvar _menu_rml_active) to leave out its own buttons and
 * pointer. Buttons of the document run console commands such as
 * "menu_cmd directmenu Settings", which makes the QuakeC menu open that
 * dialog; the document then goes away until the main screen is back.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "quakedef.h"

#include "ui_rml.h"

#include "cl_lua.h"
#include "draw.h"
#include "image.h"
#include "keys.h"
#include "menu.h"

const char *Key_KeynumToString(int keynum, char *tinystr, size_t tinystrlength);
int Key_StringToKeynum(const char *str);

#define MAINMENU "mainmenu"

cvar_t menu_rmlui = {CVAR_SAVE, "menu_rmlui", "1", "show the main menu as an RmlUi document (ui/mainmenu.rml) instead of the QuakeC one"};
cvar_t rml_scale = {CVAR_SAVE, "rml_scale", "0", "size of RmlUi documents: how many pixels a dp is; 0 picks one from the window height (and a bigger one on touch screens)"};
// QuakeC sets this each frame: 1 while its main screen (the overview of
// Singleplayer, Multiplayer, ...) is showing and no dialog is open
static cvar_t menu_overview = {0, "_menu_overview", "0", "set by the menu: 1 while the menu's main screen is up"};
// and reads this one to know it should not draw that screen itself
static cvar_t menu_rml_active = {0, "_menu_rml_active", "0", "set by the engine: 1 while an RmlUi document draws the menu's main screen"};

// Pages a server sends (see docs/MODDING.md). They are untrusted: they can
// only talk back to the server, never run commands here.
cvar_t cl_allow_server_pages = {CVAR_SAVE, "cl_allow_server_pages", "1", "1 lets a server open its own pages (RmlUi documents from its pk3) on this client; they can only answer the server, never run commands here"};
cvar_t cl_server_page_timeout = {CVAR_SAVE, "cl_server_page_timeout", "600", "seconds a page from a server stays open before it is closed; 0 = no limit"};

static qboolean ui_created;
static qboolean ui_mainmenu_failed;
static unsigned int ui_texture_serial;

// documents shown with rml_show, to bring back after a renderer restart
#define MAX_REMEMBERED 8
static char ui_remembered[MAX_REMEMBERED][MAX_QPATH];
static int ui_num_remembered;

// which keys and buttons went to a document when they went down, so the
// release goes there too
static unsigned char ui_routed[MAX_KEYS];

static float ui_mouse_sent[2] = {-1, -1};

/*
================================================================================
The engine side of RmlUi's interfaces (see ui_rml.h)
================================================================================
*/

unsigned char *UIE_LoadFile(const char *path, size_t *size)
{
	fs_offset_t len = 0;
	unsigned char *data = FS_LoadFile(path, tempmempool, true, &len);
	if (!data)
		return NULL;
	*size = (size_t)len;
	return data;
}

void UIE_Free(void *data)
{
	if (data)
		Mem_Free(data);
}

unsigned char *UIE_LoadImage(const char *path, int *width, int *height)
{
	unsigned char *bgra;
	unsigned char *rgba;
	int i, n;

	while (*path == '/')
		path++;
	bgra = loadimagepixelsbgra(path, false, false, false, NULL);
	if (!bgra)
	{
		Con_Printf("RmlUi: image %s not found\n", path);
		return NULL;
	}
	*width = image_width;
	*height = image_height;
	// RmlUi wants premultiplied alpha, the engine decodes straight BGRA
	n = image_width * image_height;
	rgba = (unsigned char *)Mem_Alloc(tempmempool, (size_t)n * 4);
	for (i = 0; i < n; i++)
	{
		unsigned int a = bgra[i * 4 + 3];
		rgba[i * 4 + 0] = (unsigned char)((bgra[i * 4 + 2] * a + 127) / 255);
		rgba[i * 4 + 1] = (unsigned char)((bgra[i * 4 + 1] * a + 127) / 255);
		rgba[i * 4 + 2] = (unsigned char)((bgra[i * 4 + 0] * a + 127) / 255);
		rgba[i * 4 + 3] = (unsigned char)a;
	}
	Mem_Free(bgra);
	return rgba;
}

void *UIE_TextureCreate(const unsigned char *rgba, int width, int height, int mipmap)
{
	char name[32];
	int flags = TEXF_ALPHA | TEXF_CLAMP | TEXF_FORCELINEAR;

	if (!drawtexturepool)
		return NULL;
	if (mipmap)
		flags |= TEXF_MIPMAP;
	// a name of its own, or the texture code hands back an earlier one that
	// happens to have the same size
	dpsnprintf(name, sizeof(name), "rmlui%u", ++ui_texture_serial);
	return R_LoadTexture2D(drawtexturepool, name, width, height, rgba, TEXTYPE_RGBA, flags, -1, NULL);
}

void UIE_TextureFree(void *texture)
{
	if (texture)
		R_FreeTexture((rtexture_t *)texture);
}

void UIE_DrawTriangles(const float *xy, const float *uv, const float *rgba, int numvertices, const int *indices, int numindices, void *texture)
{
	static float *vertex3f;
	static int capacity;
	drawqueuemesh_t mesh;
	float sx, sy;
	int i;

	if (numvertices > capacity)
	{
		capacity = numvertices + 256;
		if (vertex3f)
			Mem_Free(vertex3f);
		vertex3f = (float *)Mem_Alloc(cls.permanentmempool, sizeof(float) * 3 * capacity);
	}
	// DrawQ works in console units; the document is laid out in window pixels
	sx = (float)vid_conwidth.integer / (float)vid.width;
	sy = (float)vid_conheight.integer / (float)vid.height;
	for (i = 0; i < numvertices; i++)
	{
		vertex3f[i * 3 + 0] = xy[i * 2 + 0] * sx;
		vertex3f[i * 3 + 1] = xy[i * 2 + 1] * sy;
		vertex3f[i * 3 + 2] = 0;
	}
	mesh.texture = (rtexture_t *)texture;
	mesh.num_triangles = numindices / 3;
	mesh.num_vertices = numvertices;
	mesh.data_element3i = (int *)indices;
	mesh.data_element3s = NULL;
	mesh.data_vertex3f = vertex3f;
	mesh.data_texcoord2f = (float *)uv;
	mesh.data_color4f = (float *)rgba;
	DrawQ_MeshPremultiplied(&mesh);
}

void UIE_Scissor(int enable, int x, int y, int width, int height)
{
	if (enable)
		DrawQ_SetClipAreaPixels(x, y, width, height);
	else
		DrawQ_ResetClipArea();
}

double UIE_Time(void)
{
	return realtime;
}

void UIE_Log(int level, const char *text)
{
	if (level >= 2)
		Con_DPrintf("RmlUi: %s\n", text);
	else
		Con_Printf("RmlUi: %s\n", text);
}

void UIE_Command(const char *command)
{
	Con_DPrintf("RmlUi: command %s\n", command);
	Cbuf_AddText(command);
	Cbuf_AddText("\n");
}

const char *UIE_CvarString(const char *name)
{
	return Cvar_VariableString(name);
}

int UIE_FileExists(const char *path)
{
	return FS_FileExists(path) != 0;
}

int UIE_VideoModes(UIE_VideoMode *out, int max)
{
	int i, n = 0;
	for (i = 0; i < video_resolutions_count && n < max; i++)
	{
		out[n].width = video_resolutions[i].width;
		out[n].height = video_resolutions[i].height;
		n++;
	}
	return n;
}

void UIE_KeysFor(const char *command, char *key1, char *key2, int size)
{
	int keys[2], bindmap = 0;
	char name[32];
	Key_GetBindMap(&bindmap, NULL);
	Key_FindKeysForCommand(command, keys, 2, bindmap);
	*key1 = *key2 = 0;
	if (keys[0] >= 0)
		strlcpy(key1, Key_KeynumToString(keys[0], name, sizeof(name)), size);
	if (keys[1] >= 0)
		strlcpy(key2, Key_KeynumToString(keys[1], name, sizeof(name)), size);
}

void UIE_BindKey(const char *command, int slot, int key)
{
	int keys[2], bindmap = 0;
	Key_GetBindMap(&bindmap, NULL);
	Key_FindKeysForCommand(command, keys, 2, bindmap);
	// the slot's old key is freed; the same key in the other slot moves
	if (slot >= 0 && slot < 2 && keys[slot] >= 0)
		Key_SetBinding(keys[slot], bindmap, "");
	if (key >= 0)
	{
		if (keys[1 - slot] == key)
			keys[1 - slot] = -1;
		Key_SetBinding(key, bindmap, command);
	}
}

static qboolean ui_capture_keys;

void UIE_CaptureKeys(int on)
{
	ui_capture_keys = on != 0;
}

int UIE_ServerList(UIE_Server *out, int max)
{
	int i, n = 0;
	for (i = 0; i < serverlist_cachecount && n < max; i++)
	{
		const serverlist_entry_t *e = ServerList_GetCacheEntry(i);
		UIE_Server *o = out + n;
		if (e->query != SQS_QUERIED)
			continue;
		strlcpy(o->cname, e->info.cname, sizeof(o->cname));
		strlcpy(o->name, e->info.name, sizeof(o->name));
		strlcpy(o->map, e->info.map, sizeof(o->map));
		// the game mode is the first field of the status the game code sends
		strlcpy(o->type, e->info.qcstatus, sizeof(o->type));
		if (strchr(o->type, ':'))
			*strchr(o->type, ':') = 0;
		o->ping = e->info.ping;
		o->numplayers = e->info.numplayers;
		o->numhumans = e->info.numhumans;
		o->maxplayers = e->info.maxplayers;
		o->freeslots = e->info.freeslots;
		o->isfavorite = e->info.isfavorite != 0;
		o->mappreview = e->info.mappreview != 0;
		n++;
	}
	return n;
}

int UIE_ServerDetail_Get(const char *cname, UIE_ServerDetail *out)
{
	int i;
	for (i = 0; i < serverlist_cachecount; i++)
	{
		const serverlist_entry_t *e = ServerList_GetCacheEntry(i);
		if (e->query != SQS_QUERIED || strcmp(e->info.cname, cname))
			continue;
		strlcpy(out->players, e->info.players, sizeof(out->players));
		strlcpy(out->qcstatus, e->info.qcstatus, sizeof(out->qcstatus));
		strlcpy(out->mod, e->info.mod, sizeof(out->mod));
		strlcpy(out->game, e->info.game, sizeof(out->game));
		out->numbots = e->info.numbots;
		out->numhumans = e->info.numhumans;
		out->maxplayers = e->info.maxplayers;
		return 1;
	}
	return 0;
}

int UIE_MapPreviewRequest(const char *cname, const char *map)
{
	return NetConn_MapPreviewRequest(cname, map);
}

void UIE_ServerRefresh(int reset)
{
	ServerList_QueryList(reset != 0, true, false, false);
}

int UIE_FileList(const char *pattern, char (*out)[128], int max)
{
	int i, n = 0;
	fssearch_t *s = FS_Search(pattern, true, true);
	if (!s)
		return 0;
	for (i = 0; i < s->numfilenames && n < max; i++)
		strlcpy(out[n++], s->filenames[i], 128);
	FS_FreeSearch(s);
	return n;
}

void UIE_CvarSet(const char *name, const char *value)
{
	cvar_t *var = Cvar_FindVar(name);
	if (!var || (var->flags & (CVAR_READONLY | CVAR_PRIVATE)))
		return;
	Cvar_Set(name, value);
}

void UIE_ServerCommand(const char *line)
{
	if (cls.state != ca_connected || cls.demoplayback)
		return;
	Con_DPrintf("RmlUi: to the server: %s\n", line);
	Cmd_ForwardStringToServer(line);
}

void UIE_ClipboardSet(const char *text)
{
	Sys_SetClipboardData(text);
}

void UIE_ClipboardGet(char *out, int size)
{
	char *text = Sys_GetClipboardData();
	out[0] = 0;
	if (text)
	{
		strlcpy(out, text, size);
		Z_Free(text);
	}
}

void UIE_Keyboard(int show)
{
	VID_ShowKeyboard(show != 0);
}

/*
================================================================================
Life cycle
================================================================================
*/

static float UI_RML_DPRatio(void)
{
	if (rml_scale.value > 0)
		return rml_scale.value;
	// 720 dp on the short side on a monitor; touch screens are held close and used
	// with a finger, so they get fewer, bigger dps (a phone in landscape
	// ends up about 450 dp high). The short side counts, so a phone held
	// upright is about 450 dp wide instead of 210.
	return (float)(vid.height < vid.width ? vid.height : vid.width) / (vid_touchscreen.integer ? 450.0f : 720.0f);
}

static void UI_RML_Create(void)
{
	int i;

	if (ui_created || !vid.width || !vid.height)
		return;
	if (!UIRml_Create(vid.width, vid.height, UI_RML_DPRatio()))
	{
		ui_mainmenu_failed = true;
		return;
	}
	ui_created = true;
	ui_mainmenu_failed = false;
	// load the main menu now rather than on first use, so a document that
	// does not load is found out before the QuakeC menu leaves its own out
	if (UIRml_Show(MAINMENU))
		UIRml_Hide(MAINMENU);
	else
	{
		Con_Printf("RmlUi: ui/" MAINMENU ".rml did not load, the QuakeC main menu stays\n");
		ui_mainmenu_failed = true;
	}
	for (i = 0; i < ui_num_remembered; i++)
		UIRml_Show(ui_remembered[i]);
	ui_num_remembered = 0;
	ui_mouse_sent[0] = ui_mouse_sent[1] = -1;
	memset(ui_routed, 0, sizeof(ui_routed));
}

static void UI_RML_Remember(const char *name)
{
	// pages from a server are gone with the renderer; the server may send them again
	if (strcmp(name, MAINMENU) && strncmp(name, "server:", 7) && ui_num_remembered < MAX_REMEMBERED)
		dpsnprintf(ui_remembered[ui_num_remembered++], sizeof(ui_remembered[0]), "%s", name);
}

static void UI_RML_Destroy(void)
{
	if (!ui_created)
		return;
	ui_num_remembered = 0;
	UIRml_EachShown(UI_RML_Remember);
	UIRml_Destroy();
	ui_created = false;
}

// The renderer is restarting or the engine is quitting. The textures RmlUi
// holds are in the draw module's pool, which is about to be freed, so give
// them back first; everything is made again on the next frame drawn.
void UI_RML_Shutdown(void)
{
	UI_RML_Destroy();
}

/*
================================================================================
Console commands
================================================================================
*/

static void UI_RML_PrintLine(const char *line)
{
	Con_Printf("  %s\n", line);
}

static void RML_Show_f(void)
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("usage: rml_show <document>   shows ui/<document>.rml\n");
		return;
	}
	Host_StartVideo();
	UI_RML_Create();
	if (!ui_created)
		return;
	if (!UIRml_Show(Cmd_Argv(1)))
		Con_Printf("rml_show: ui/%s.rml did not load\n", Cmd_Argv(1));
}

// The paths a page may have: ui/<folder>/<name>.rml, plain file name
// characters. The server checks this too; the client does not trust it.
static qboolean UI_RML_ServerPathOK(const char *path)
{
	size_t n = strlen(path), i;
	if (n < 8 || n > 120 || strncmp(path, "ui/", 3) || strcmp(path + n - 4, ".rml") || strstr(path, ".."))
		return false;
	for (i = 0; i < n; i++)
	{
		char c = path[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
			return false;
	}
	// ui/<file>.rml directly in ui/ belongs to the game, a page needs a folder
	return strchr(path + 3, '/') != NULL;
}

static void RML_ShowServer_f(void)
{
	static double last;
	const char *path, *why;

	if (Cmd_Argc() != 2)
	{
		Con_Printf("usage: rml_show_server <ui/folder/page.rml>   (a server sends this)\n");
		return;
	}
	path = Cmd_Argv(1);
	if (!cl_allow_server_pages.integer)
		return;
	if (cls.state != ca_connected || cls.demoplayback)
		return;
	if (!UI_RML_ServerPathOK(path))
	{
		Con_Printf("The server asked for a page with a path that is not allowed: %s\n", path);
		return;
	}
	// a server cannot make the screen flicker with pages
	if (realtime - last < 0.5)
		return;
	last = realtime;
	Host_StartVideo();
	UI_RML_Create();
	if (!ui_created)
		return;
	why = UIRml_ShowServer(path);
	if (why)
		Con_Printf("Could not show the server's page %s: %s\n", path, why);
}

static void RML_CloseServer_f(void)
{
	if (ui_created && UIRml_ServerShown())
		UIRml_HideAll();
}

static void RML_Hide_f(void)
{
	if (Cmd_Argc() < 2)
	{
		UIRml_HideAll();
		return;
	}
	UIRml_Hide(Cmd_Argv(1));
}

static void RML_Reload_f(void)
{
	if (!ui_created)
	{
		Con_Printf("rml_reload: nothing loaded yet\n");
		return;
	}
	UIRml_Reload();
	// a main menu that failed to load may work now
	ui_mainmenu_failed = false;
	if (UIRml_Show(MAINMENU))
	{
		if (!Cvar_VariableValue("_menu_overview") || !menu_rmlui.integer)
			UIRml_Hide(MAINMENU);
	}
	else
		ui_mainmenu_failed = true;
	Con_Printf("RmlUi documents reloaded\n");
}

// A click the way the window system would deliver it, for tests: the pointer
// moves, the button goes down and up through Key_Event, so it takes the same
// route into the menu as a real one.
static void RML_Click_f(void)
{
	if (Cmd_Argc() < 3)
	{
		Con_Printf("usage: rml_click <x> <y>   click the primary button at a window pixel\n");
		return;
	}
	in_windowmouse_x = atoi(Cmd_Argv(1));
	in_windowmouse_y = atoi(Cmd_Argv(2));
	Key_Event(K_MOUSE1, 0, true);
	Key_Event(K_MOUSE1, 0, false);
}

// A drag, for tests of drag scrolling: the button goes down at the start, the
// pointer takes ten frames to the end, then the button goes up.
static struct { qboolean active; int frame; int x0, y0, x1, y1; } ui_drag;

// A finger by hand, for tests: rml_touch down|move|up <x> <y>. The window
// system would keep reporting the real pointer, so the position is set again
// every frame while the finger is down.
static struct { qboolean down; int x, y; } ui_finger;

static void RML_Touch_f(void)
{
	const char *what = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
	if (!strcmp(what, "up"))
	{
		ui_finger.down = false;
		Key_Event(K_MOUSE1, 0, false);
		return;
	}
	if (Cmd_Argc() < 4 || (strcmp(what, "down") && strcmp(what, "move")))
	{
		Con_Printf("usage: rml_touch down|move <x> <y>, rml_touch up   (for tests)\n");
		return;
	}
	ui_finger.x = in_windowmouse_x = atoi(Cmd_Argv(2));
	ui_finger.y = in_windowmouse_y = atoi(Cmd_Argv(3));
	if (!strcmp(what, "down"))
	{
		ui_finger.down = true;
		Key_Event(K_MOUSE1, 0, true);
	}
}

// A key press and release by name, for tests (the window system is not asked)
static void RML_Key_f(void)
{
	int key;
	const char *name;
	qboolean ctrl = false;
	if (Cmd_Argc() < 2)
		name = "";
	else
		name = Cmd_Argv(1);
	// "ctrl+v": the key with control held down
	if (!strncasecmp(name, "ctrl+", 5))
	{
		ctrl = true;
		name += 5;
	}
	if ((key = Key_StringToKeynum(name)) < 0)
	{
		Con_Printf("usage: rml_key [ctrl+]<key name>   press and release a key (for tests)\n");
		return;
	}
	if (ctrl)
		Key_Event(K_CTRL, 0, true);
	Key_Event(key, 0, true);
	Key_Event(key, 0, false);
	if (ctrl)
		Key_Event(K_CTRL, 0, false);
}

// typed text as one input event (what an input method or a paste delivers),
// and the system clipboard, for tests
static void RML_Text_f(void)
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("usage: rml_text <text>   deliver text as one input event (for tests)\n");
		return;
	}
	Key_TextInput(Cmd_Argv(1));
}

static void RML_Clip_f(void)
{
	if (Cmd_Argc() < 2)
		return;
	Sys_SetClipboardData(Cmd_Argv(1));
}

static void RML_State_f(void)
{
	if (Cmd_Argc() < 2 || !ui_created)
		return;
	UIRml_State(Cmd_Argv(1), UI_RML_PrintLine);
}

static void RML_Drag_f(void)
{
	if (Cmd_Argc() < 5)
	{
		Con_Printf("usage: rml_drag <x0> <y0> <x1> <y1>   press at the first pixel, drag to the second, release\n");
		return;
	}
	ui_drag.x0 = atoi(Cmd_Argv(1));
	ui_drag.y0 = atoi(Cmd_Argv(2));
	ui_drag.x1 = atoi(Cmd_Argv(3));
	ui_drag.y1 = atoi(Cmd_Argv(4));
	ui_drag.frame = 0;
	ui_drag.active = true;
}

static void UI_RML_DragStep(void)
{
	float t;

	if (ui_finger.down)
	{
		in_windowmouse_x = ui_finger.x;
		in_windowmouse_y = ui_finger.y;
	}

	if (!ui_drag.active)
		return;
	t = ui_drag.frame > 10 ? 1.0f : ui_drag.frame / 10.0f;
	in_windowmouse_x = (int)(ui_drag.x0 + (ui_drag.x1 - ui_drag.x0) * t);
	in_windowmouse_y = (int)(ui_drag.y0 + (ui_drag.y1 - ui_drag.y0) * t);
	if (ui_drag.frame == 0)
		Key_Event(K_MOUSE1, 0, true);
	else if (ui_drag.frame == 12)
	{
		Key_Event(K_MOUSE1, 0, false);
		ui_drag.active = false;
	}
	ui_drag.frame++;
}

static void RML_List_f(void)
{
	if (!ui_created)
	{
		Con_Printf("RmlUi is not running\n");
		return;
	}
	Con_Printf("RmlUi documents:\n");
	UIRml_List(UI_RML_PrintLine);
}

void UI_RML_Init(void)
{
	Cvar_RegisterVariable(&menu_rmlui);
	Cvar_RegisterVariable(&rml_scale);
	Cvar_RegisterVariable(&cl_allow_server_pages);
	Cvar_RegisterVariable(&cl_server_page_timeout);
	Cvar_RegisterVariable(&menu_overview);
	Cvar_RegisterVariable(&menu_rml_active);
	Cmd_AddCommand("rml_show", RML_Show_f, "show an RmlUi document: rml_show <name> loads ui/<name>.rml and draws it above the menu");
	Cmd_AddCommand("rml_hide", RML_Hide_f, "hide an RmlUi document (all of them without a name)");
	Cmd_AddCommand("rml_show_server", RML_ShowServer_f, "open a page the server sent (ui/<folder>/<name>.rml); servers use this");
	Cmd_AddCommand("rml_close_server", RML_CloseServer_f, "close the page a server opened");
	Cmd_AddCommand("rml_reload", RML_Reload_f, "load the RmlUi documents, stylesheets and images again (for editing them)");
	Cmd_AddCommand("rml_click", RML_Click_f, "click at a window pixel as if with the mouse (for tests)");
	Cmd_AddCommand("rml_drag", RML_Drag_f, "press, drag and release the primary button between window pixels (for tests)");
	Cmd_AddCommand("rml_touch", RML_Touch_f, "a finger by hand: rml_touch down|move <x> <y>, rml_touch up (for tests)");
	Cmd_AddCommand("rml_key", RML_Key_f, "press and release a key by name (for tests)");
	Cmd_AddCommand("rml_text", RML_Text_f, "deliver text as one input event, as an input method does (for tests)");
	Cmd_AddCommand("rml_clip", RML_Clip_f, "put text on the system clipboard (for tests)");
	Cmd_AddCommand("rml_state", RML_State_f, "print whether the element with an id is hovered or pressed (for tests)");
	Cmd_AddCommand("rml_list", RML_List_f, "list the RmlUi documents");
}

/*
================================================================================
Drawing
================================================================================
*/

static int UI_RML_Modifiers(void)
{
	int m = 0;
	if (Key_IsDown(K_CTRL))
		m |= UIMOD_CTRL;
	if (Key_IsDown(K_SHIFT))
		m |= UIMOD_SHIFT;
	if (Key_IsDown(K_ALT))
		m |= UIMOD_ALT;
	return m;
}

int UI_RML_Modal(void)
{
	return ui_created && UIRml_AnyShownExcept(MAINMENU);
}

static qboolean UI_RML_MainMenuShown(void)
{
	return ui_created && UIRml_IsShown(MAINMENU);
}

static void UI_RML_DrawCursor(void)
{
	cachepic_t *pic;
	char name[MAX_QPATH];
	const char *skin = Cvar_VariableString("menu_skin");
	float x, y;

	if (vid_touchscreen.integer || VID_InputIsTouch())
		return; // a finger is no pointer
	dpsnprintf(name, sizeof(name), "gfx/menu/%s/cursor", *skin ? skin : "luminos");
	pic = Draw_CachePic_Flags(name, CACHEPICFLAG_QUIET);
	if (!pic || !Draw_GetPicTexture(pic))
		pic = Draw_CachePic_Flags("gfx/menu/luminos/cursor", CACHEPICFLAG_QUIET);
	if (!pic || !Draw_GetPicTexture(pic))
		return;
	x = in_windowmouse_x * (float)vid_conwidth.integer / (float)vid.width;
	y = in_windowmouse_y * (float)vid_conheight.integer / (float)vid.height;
	// the skin's cursor is 32x32 console units
	DrawQ_Pic(x, y, pic, 32, 32, 1, 1, 1, 1, 0);
}

/*
The band across the top of a page from a server. The engine draws it, not the
page, so the page cannot style it away, cover it or move it: it says where
the page comes from and has a close button. (Escape closes the page too.)
*/
#define SERVER_BAND_DP 40.0f

static float UI_RML_BandHeight(void)
{
	return SERVER_BAND_DP * UI_RML_DPRatio(); // window pixels
}

static void UI_RML_DrawServerBand(void)
{
	float sx, sy, h, w, bw;
	const char *text = "Page from the server. Esc closes it.";

	if (!UIRml_ServerShown())
		return;
	sx = (float)vid_conwidth.integer / (float)vid.width;
	sy = (float)vid_conheight.integer / (float)vid.height;
	h = UI_RML_BandHeight() * sy;
	w = (float)vid_conwidth.integer;
	bw = 72.0f * UI_RML_DPRatio() * sx;
	DrawQ_Fill(0, 0, w, h, 0, 0, 0, 0.9f, 0);
	DrawQ_Fill(w - bw, 0, bw, h, 0.69f, 0.19f, 0.19f, 1, 0);
	DrawQ_String(36, h * 0.5f - 4, text, 0, 8, 8, 1, 0.8f, 0.4f, 1, 0, NULL, true, FONT_DEFAULT);
	DrawQ_String(w - bw + bw * 0.5f - 20, h * 0.5f - 4, "Close", 0, 8, 8, 1, 1, 1, 1, 0, NULL, true, FONT_DEFAULT);
}

void UI_RML_Draw(void)
{
	qboolean want_main;
	qboolean active;

	if (!menu_rmlui.integer && !ui_created)
	{
		if (menu_rml_active.integer)
			Cvar_SetValueQuick(&menu_rml_active, 0);
		return;
	}
	if (!ui_created)
		UI_RML_Create();
	if (!ui_created)
		return;
	UI_RML_DragStep();

	// the QuakeC menu draws its own main screen unless it is told not to
	active = menu_rmlui.integer && !ui_mainmenu_failed;
	if (menu_rml_active.integer != (active ? 1 : 0))
		Cvar_SetValueQuick(&menu_rml_active, active);

	want_main = active && Cvar_VariableValue("_menu_overview") != 0 && (key_dest == key_menu || key_dest == key_menu_grabbed);
	if (want_main != UI_RML_MainMenuShown())
	{
		if (want_main)
		{
			UIRml_Show(MAINMENU);
			// the finger or pointer that went down on the dialog's close
			// button must not click what is now under it
			UIRml_MouseLeave();
			ui_mouse_sent[0] = ui_mouse_sent[1] = -1;
		}
		else
			UIRml_Hide(MAINMENU);
	}
	if (!UI_RML_MainMenuShown() && !UIRml_AnyShownExcept(MAINMENU))
		return;

	UIRml_Resize(vid.width, vid.height, UI_RML_DPRatio());
	UIRml_SetTouch(vid_touchscreen.integer || VID_InputIsTouch());
	UIRml_SetBodyClass(MAINMENU, "touch", vid_touchscreen.integer != 0);
	UIRml_SetBodyClass(MAINMENU, "ingame", cls.state == ca_connected);
	// state the in-game menu's stylesheet reacts to: which words and buttons it shows
	UIRml_SetBodyClass("gamemenu", "demo", cls.demoplayback != 0);
	UIRml_SetBodyClass("gamemenu", "campaign", Cvar_VariableValue("g_campaign") != 0);
	UIRml_SetBodyClass("gamemenu", "solo", sv.active && !strcmp(Cvar_VariableString("net_address"), "127.0.0.1"));
	{
		int teams = (int)Cvar_VariableValue("_teams_available");
		UIRml_SetBodyClass("teamselect", "red", (teams & 1) != 0);
		UIRml_SetBodyClass("teamselect", "blue", (teams & 2) != 0);
		UIRml_SetBodyClass("teamselect", "yellow", (teams & 4) != 0);
		UIRml_SetBodyClass("teamselect", "pink", (teams & 8) != 0);
	}
	// a finger is only a pointer while it is down
	if (!vid_touchscreen.integer && !VID_InputIsTouch())
	{
		if (ui_mouse_sent[0] != in_windowmouse_x || ui_mouse_sent[1] != in_windowmouse_y)
		{
			ui_mouse_sent[0] = (float)in_windowmouse_x;
			ui_mouse_sent[1] = (float)in_windowmouse_y;
			UIRml_MouseMove(ui_mouse_sent[0], ui_mouse_sent[1]);
		}
	}
	else if (Key_IsDown(K_MOUSE1) && (ui_mouse_sent[0] != in_windowmouse_x || ui_mouse_sent[1] != in_windowmouse_y))
	{
		ui_mouse_sent[0] = (float)in_windowmouse_x;
		ui_mouse_sent[1] = (float)in_windowmouse_y;
		UIRml_MouseMove(ui_mouse_sent[0], ui_mouse_sent[1]);
	}

	UIRml_Frame();
	DrawQ_ResetClipArea();
	UI_RML_DrawServerBand();
	UI_RML_DrawCursor();
}

/*
================================================================================
Input
================================================================================
*/

static int UI_RML_TranslateKey(int key)
{
	if (key >= 'a' && key <= 'z')
		return key;
	if (key >= '0' && key <= '9')
		return key;
	switch (key)
	{
	case K_SPACE: return ' ';
	case K_ESCAPE: return UIKEY_ESCAPE;
	case K_TAB: return UIKEY_TAB;
	case K_ENTER: case K_KP_ENTER: return UIKEY_RETURN;
	case K_BACKSPACE: return UIKEY_BACKSPACE;
	case K_DEL: return UIKEY_DELETE;
	case K_INS: return UIKEY_INSERT;
	case K_HOME: return UIKEY_HOME;
	case K_END: return UIKEY_END;
	case K_PGUP: return UIKEY_PAGEUP;
	case K_PGDN: return UIKEY_PAGEDOWN;
	case K_LEFTARROW: return UIKEY_LEFT;
	case K_RIGHTARROW: return UIKEY_RIGHT;
	case K_UPARROW: return UIKEY_UP;
	case K_DOWNARROW: return UIKEY_DOWN;
	default: return 0;
	}
}

int UI_RML_KeyEvent(int key, int ascii, int down)
{
	int mods, k;
	qboolean modal, main_shown;

	if (!ui_created || key < 0 || key >= MAX_KEYS)
		return 0;
	// binding a key: the next press goes to the page that asked, whatever it is
	if (ui_capture_keys && down && key != K_TEXT)
	{
		ui_capture_keys = false;
		ui_routed[key] = 1; // the release is not the game's either
		UIRml_KeyCaptured(key == K_ESCAPE ? -1 : key);
		return 1;
	}
	main_shown = UI_RML_MainMenuShown();
	modal = UI_RML_Modal();
	// a release goes where the press went, shown or not
	if (!down && ui_routed[key])
	{
		ui_routed[key] = 0;
		if (key >= K_MOUSE1 && key <= K_MOUSE3)
			UIRml_MouseButton(key - K_MOUSE1, 0, UI_RML_Modifiers(), vid_touchscreen.integer || VID_InputIsTouch());
		else if (key == K_MWHEELUP || key == K_MWHEELDOWN)
			;
		else if ((k = UI_RML_TranslateKey(key)) != 0)
			UIRml_Key(k, 0, UI_RML_Modifiers());
		return 1;
	}
	if (!main_shown && !modal)
		return 0;
	if (!down)
		return 0;
	mods = UI_RML_Modifiers();

	if (key >= K_MOUSE1 && key <= K_MOUSE3)
	{
		// the close button of the band of a page from a server
		if (UIRml_ServerShown() && in_windowmouse_y < UI_RML_BandHeight() && in_windowmouse_x > vid.width - 72.0f * UI_RML_DPRatio())
		{
			UIRml_HideAll();
			ui_routed[key] = 1;
			return 1;
		}
		UIRml_SetTouch(vid_touchscreen.integer || VID_InputIsTouch());
		UIRml_MouseMove((float)in_windowmouse_x, (float)in_windowmouse_y);
		ui_mouse_sent[0] = (float)in_windowmouse_x;
		ui_mouse_sent[1] = (float)in_windowmouse_y;
		UIRml_MouseButton(key - K_MOUSE1, 1, mods, vid_touchscreen.integer || VID_InputIsTouch());
		ui_routed[key] = 1;
		return 1;
	}
	if (key == K_MWHEELUP || key == K_MWHEELDOWN)
	{
		UIRml_MouseWheel(key == K_MWHEELUP ? 1.0f : -1.0f, mods);
		return 1;
	}
	if (key == K_TEXT)
	{
		if (UIRml_TextFocus() && ascii)
		{
			UIRml_Text((unsigned int)ascii);
			return 1;
		}
		return 0;
	}
	if (key == K_ESCAPE)
	{
		// escape closes a document shown with rml_show; on the main menu it is
		// the QuakeC menu's (it leaves the menu when a game is running)
		if (modal)
		{
			UIRml_HideAll();
			ui_routed[key] = 1;
			return 1;
		}
		return 0;
	}
	k = UI_RML_TranslateKey(key);
	if (!k)
		return 0;
	// with a text field focused, typing belongs to it entirely
	if (UIRml_TextFocus())
	{
		UIRml_Key(k, 1, mods);
		ui_routed[key] = 1;
		return 1;
	}
	// otherwise only what moves between or presses buttons; letters stay
	// with the QuakeC menu's shortcuts
	if (k == UIKEY_TAB || k == UIKEY_RETURN || k == ' ' || k == UIKEY_LEFT || k == UIKEY_RIGHT || k == UIKEY_UP || k == UIKEY_DOWN || modal)
		if (UIRml_Key(k, 1, mods))
		{
			ui_routed[key] = 1;
			return 1;
		}
	return modal ? 1 : 0;
}
