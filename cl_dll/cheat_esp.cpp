// cl_dll/cheat_esp.cpp  (v3, всё в одном)
// Debug-оверлей для тестов на своём сервере с ботами.
// Работает только при sv_cheats 1. Аима здесь нет.
//
// Команды:
//   ch_esp 0 / 1 / 2   выкл / скелет + круг головы / то же + бокс
//   ch_dist 0 / 1      дистанция отдельно
//   ch_esp_dist N      радиус показа в юнитах (1000 по умолчанию, 0 = без лимита)
//   ch_toggle          вкл/выкл ch_esp (для бинда)

#include "hud.h"
#include "cl_util.h"
#include "triangleapi.h"
#include "const.h"
#include "studio.h"
#include "com_model.h"
#include "r_studioint.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

extern engine_studio_api_t IEngineStudio;

static cvar_t *ch_esp;
static cvar_t *ch_dist;
static cvar_t *ch_esp_dist;

// ---------------------------------------------------------------------------
// Кэш костей. Заполняется из GameStudioModelRenderer.cpp (патч в конце файла).
// ---------------------------------------------------------------------------
float g_EspBones[33][MAXSTUDIOBONES][3][4];
int   g_EspBoneCount[33];
float g_EspBoneTime[33];

// ---------------------------------------------------------------------------
// Упрощённый скелет: 16 суставов вместо всех костей
// ---------------------------------------------------------------------------
enum
{
	J_PELVIS, J_CHEST, J_NECK, J_HEAD,
	J_LARM1, J_LARM2, J_LHAND,
	J_RARM1, J_RARM2, J_RHAND,
	J_LLEG1, J_LLEG2, J_LFOOT,
	J_RLEG1, J_RLEG2, J_RFOOT,
	J_COUNT
};

static const char *g_JointNames[J_COUNT] =
{
	"Bip01 Pelvis", "Bip01 Spine3", "Bip01 Neck", "Bip01 Head",
	"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand",
	"Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
	"Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot",
	"Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot"
};

// пары суставов, между которыми рисуем линию
static const int g_Links[][2] =
{
	{ J_PELVIS, J_CHEST }, { J_CHEST, J_NECK }, { J_NECK, J_HEAD },
	{ J_NECK, J_LARM1 }, { J_LARM1, J_LARM2 }, { J_LARM2, J_LHAND },
	{ J_NECK, J_RARM1 }, { J_RARM1, J_RARM2 }, { J_RARM2, J_RHAND },
	{ J_PELVIS, J_LLEG1 }, { J_LLEG1, J_LLEG2 }, { J_LLEG2, J_LFOOT },
	{ J_PELVIS, J_RLEG1 }, { J_RLEG1, J_RLEG2 }, { J_RLEG2, J_RFOOT }
};
static const int g_LinkCount = sizeof(g_Links) / sizeof(g_Links[0]);

// Кэш: для каждой модели индексы костей ищем по именам один раз
struct JointMap
{
	studiohdr_t *hdr;
	int idx[J_COUNT];
};
static JointMap g_JointCache[16];
static int g_JointCacheCount = 0;

static const JointMap *GetJointMap(studiohdr_t *hdr)
{
	for (int i = 0; i < g_JointCacheCount; i++)
		if (g_JointCache[i].hdr == hdr)
			return &g_JointCache[i];

	JointMap *m;
	if (g_JointCacheCount < 16)
		m = &g_JointCache[g_JointCacheCount++];
	else
		m = &g_JointCache[0];

	m->hdr = hdr;
	mstudiobone_t *bones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	for (int j = 0; j < J_COUNT; j++)
	{
		m->idx[j] = -1;
		for (int b = 0; b < hdr->numbones; b++)
		{
			if (!strcmp(bones[b].name, g_JointNames[j]))
			{
				m->idx[j] = b;
				break;
			}
		}
	}
	return m;
}

// ---------------------------------------------------------------------------
static void Cheat_Toggle()
{
	if (ch_esp)
		gEngfuncs.Cvar_SetValue("ch_esp", ch_esp->value != 0.0f ? 0.0f : 1.0f);
}

void Cheat_Init()
{
	ch_esp      = gEngfuncs.pfnRegisterVariable("ch_esp", "0", 0);
	ch_dist     = gEngfuncs.pfnRegisterVariable("ch_dist", "0", 0);
	ch_esp_dist = gEngfuncs.pfnRegisterVariable("ch_esp_dist", "1000", 0);
	gEngfuncs.pfnAddCommand("ch_toggle", Cheat_Toggle);
}

// ---------------------------------------------------------------------------
static bool W2S(const float *p, int &x, int &y)
{
	float s[3];
	// возвращает не 0, если точка за камерой
	if (gEngfuncs.pTriAPI->WorldToScreen((float *)p, s))
		return false;
	x = (int)((1.0f + s[0]) * ScreenWidth  * 0.5f);
	y = (int)((1.0f - s[1]) * ScreenHeight * 0.5f);
	return true;
}

// Линия точками с шагом 3 px: в разы меньше вызовов FillRGBA, нет лагов
static void Line2D(int x0, int y0, int x1, int y1, int r, int g, int b)
{
	int dx = abs(x1 - x0), dy = abs(y1 - y0);
	int n = dx > dy ? dx : dy;
	if (n > 1500) return;
	int steps = n / 3;
	if (steps < 1) steps = 1;

	for (int i = 0; i <= steps; i++)
	{
		int x = x0 + (x1 - x0) * i / steps;
		int y = y0 + (y1 - y0) * i / steps;
		gEngfuncs.pfnFillRGBA(x - 1, y - 1, 3, 3, r, g, b, 255);
	}
}

static void Circle2D(int cx, int cy, int rad, int r, int g, int b)
{
	if (rad < 3)  rad = 3;
	if (rad > 60) rad = 60;

	const int seg = 14;
	for (int k = 0; k < seg; k++)
	{
		float a = (float)k * 6.2831853f / (float)seg;
		int x = cx + (int)(cosf(a) * rad);
		int y = cy + (int)(sinf(a) * rad);
		gEngfuncs.pfnFillRGBA(x - 1, y - 1, 3, 3, r, g, b, 255);
	}
}

static void DrawBox(int cx, int y1, int y2, int r, int g, int b)
{
	int h = y2 - y1;
	if (h < 4) return;
	int w  = h / 2;
	int x1 = cx - w / 2;

	gEngfuncs.pfnFillRGBA(x1,     y1, w, 1, r, g, b, 255);
	gEngfuncs.pfnFillRGBA(x1,     y2, w, 1, r, g, b, 255);
	gEngfuncs.pfnFillRGBA(x1,     y1, 1, h, r, g, b, 255);
	gEngfuncs.pfnFillRGBA(x1 + w, y1, 1, h, r, g, b, 255);
}

static inline void JointPos(int idx, int bone, float *out)
{
	out[0] = g_EspBones[idx][bone][0][3];
	out[1] = g_EspBones[idx][bone][1][3];
	out[2] = g_EspBones[idx][bone][2][3];
}

// Скелет + круг головы
static void DrawSkeleton(int idx, cl_entity_t *e, int r, int g, int b)
{
	if (g_EspBoneCount[idx] <= 0 || !e->model)
		return;

	// кости устарели (игрока не рендерили в этом кадре)
	if (gEngfuncs.GetClientTime() - g_EspBoneTime[idx] > 0.1f)
		return;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata(e->model);
	if (!hdr)
		return;

	const JointMap *map = GetJointMap(hdr);

	// экранные позиции суставов (считаем один раз на сустав)
	int  sx[J_COUNT], sy[J_COUNT];
	bool ok[J_COUNT];

	for (int j = 0; j < J_COUNT; j++)
	{
		ok[j] = false;
		int bone = map->idx[j];
		if (bone < 0 || bone >= g_EspBoneCount[idx])
			continue;

		float p[3];
		JointPos(idx, bone, p);
		ok[j] = W2S(p, sx[j], sy[j]);
	}

	// линии скелета
	for (int l = 0; l < g_LinkCount; l++)
	{
		int a = g_Links[l][0], c = g_Links[l][1];
		if (ok[a] && ok[c])
			Line2D(sx[a], sy[a], sx[c], sy[c], r, g, b);
	}

	// круг головы
	int hb = map->idx[J_HEAD];
	if (hb >= 0 && hb < g_EspBoneCount[idx])
	{
		float hp[3];
		JointPos(idx, hb, hp);
		hp[2] += 3.0f; // центр головы чуть выше кости

		float hp2[3] = { hp[0], hp[1], hp[2] + 4.5f }; // радиус головы ~4.5 юнита

		int cx, cy, tx, ty;
		if (W2S(hp, cx, cy) && W2S(hp2, tx, ty))
			Circle2D(cx, cy, abs(cy - ty), 255, 255, 0);
	}
}

// Дистанция отдельно, над головой
static void DrawDist(cl_entity_t *e, float dist)
{
	float top = (e->curstate.usehull == 1) ? 14.0f : 28.0f;
	float p[3] = { e->origin[0], e->origin[1], e->origin[2] + top + 6.0f };

	int x, y;
	if (!W2S(p, x, y))
		return;

	char buf[16];
	snprintf(buf, sizeof(buf), "%dm", (int)(dist * 0.0254f));
	gEngfuncs.pfnDrawSetTextColor(1.0f, 1.0f, 0.0f);
	gEngfuncs.pfnDrawConsoleString(x - 8, y - 12, buf);
}

// ---------------------------------------------------------------------------
void Cheat_Draw()
{
	bool skel  = ch_esp  && ch_esp->value  != 0.0f;
	bool showd = ch_dist && ch_dist->value != 0.0f;

	if (!skel && !showd)
		return;

	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	if (!me)
		return;

	float maxDist = ch_esp_dist ? ch_esp_dist->value : 0.0f;
	int maxPl = gEngfuncs.GetMaxClients();

	for (int i = 1; i <= maxPl; i++)
	{
		cl_entity_t *e = gEngfuncs.GetEntityByIndex(i);
		if (!e || e == me || !e->player)
			continue;

		// игрок не обновлялся сервером (вне PVS)
		if (e->curstate.messagenum < me->curstate.messagenum)
			continue;

		// пропускаем своих по команде
		if (g_PlayerExtraInfo[i].teamnumber == g_PlayerExtraInfo[me->index].teamnumber)
			continue;

		// расстояние
		float dx = e->origin[0] - me->origin[0];
		float dy = e->origin[1] - me->origin[1];
		float dz = e->origin[2] - me->origin[2];
		float dist = sqrtf(dx * dx + dy * dy + dz * dz);

		if (maxDist > 0.0f && dist > maxDist)
			continue;

		if (skel)
			DrawSkeleton(i, e, 255, 0, 0);

		// бокс (режим 2)
		if (skel && ch_esp->value >= 2.0f)
		{
			float top = (e->curstate.usehull == 1) ? 14.0f : 28.0f;
			float bot = (e->curstate.usehull == 1) ? -18.0f : -36.0f;
			float head[3] = { e->origin[0], e->origin[1], e->origin[2] + top };
			float feet[3] = { e->origin[0], e->origin[1], e->origin[2] + bot };

			int hx, hy, fx, fy;
			if (W2S(head, hx, hy) && W2S(feet, fx, fy))
				DrawBox(hx, hy, fy, 255, 255, 0);
		}

		if (showd)
			DrawDist(e, dist);
	}
}

/*
===============================================================================
 ПАТЧ в cl_dll/studio/GameStudioModelRenderer.cpp, функция _StudioDrawPlayer,
 сразу после строк:

	StudioSetupBones();
	StudioSaveBones();

 должен стоять блок:

	{
		extern float g_EspBones[33][MAXSTUDIOBONES][3][4];
		extern int   g_EspBoneCount[33];
		extern float g_EspBoneTime[33];

		int idx = m_nPlayerIndex + 1;
		if (idx >= 1 && idx <= 32)
		{
			memcpy(g_EspBones[idx], *m_pbonetransform, sizeof(float) * 12 * m_pStudioHeader->numbones);
			g_EspBoneCount[idx] = m_pStudioHeader->numbones;
			g_EspBoneTime[idx]  = gEngfuncs.GetClientTime();
		}
	}

 hud.cpp: Cheat_Init() в конце CHud::Init()      (уже сделано)
 hud_redraw.cpp: Cheat_Draw() в CHud::Redraw()   (уже сделано)

 Использование:
	sv_cheats 1
	ch_esp 1
	ch_dist 1
	ch_esp_dist 600
	bind F1 ch_toggle
===============================================================================
*/
