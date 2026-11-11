/*
Insanotic: load time accounting (developer_loadtimes 1).
SPDX-License-Identifier: GPL-3.0-or-later
*/
#ifndef LOADTIMES_H
#define LOADTIMES_H

typedef struct loadtimes_s
{
	double map_start;      // Host_Map_f / connect
	double spawnserver;    // SV_SpawnServer total
	double models;         // Mod_LoadModel total
	int models_count;
	double sounds;         // S_PrecacheSound total
	int sounds_count;
	double csqc_init;      // CL_VM_Init
	double world_setup;    // CL_SetupWorldModel
	double video;          // Host_StartVideo
} loadtimes_t;

extern loadtimes_t loadtimes;
extern cvar_t developer_loadtimes;
void LoadTimes_Report(void);
void Host_PreloadRecordStart(void);
void Host_PreloadGameReady(void);

#endif
