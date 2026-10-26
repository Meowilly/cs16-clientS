// cl_dll/cheat_esp.cpp
// Debug-оверлей для тестов на своём сервере с ботами: скелеты игроков.
// Работает только при sv_cheats 1. Аима здесь нет.

#include "hud.h"
#include "cl_util.h"
#include "triangleapi.h"
#include "const.h"
#include "studio.h"
#include "com_model.h"
#include "r_studioint.h"
#include <string.h>
#include <stdlib.h>

extern engine_studio_api_t IEngineStudio;

static cvar_t *ch_esp; // 0 = выкл, 1 = скелет, 2 = скелет + бокс

// ---------------------------------------------------------------------------
// Кэш костей. Заполняется из StudioModelRenderer (см. патч внизу файла).
// ---------------------------------------------------------------------------
float g_EspBones[33][MAXSTUDIOBONES][3][4];
int   g_EspBoneCount[33];
float g_EspBoneTime[33];

void Cheat_Init()
{
	ch_esp = gEngfuncs.pfnRegisterVariable("ch_esp", "0", 0);
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

static void Line2D(int x0, int y0, int x1, int y1, int r, int g, int b)
{
	int dx = abs(x1 - x0), dy = abs(y1 - y0);
	int n = dx > dy ? dx : dy;
	if (n > 2000) return; // защита от гигантских линий
	if (n == 0) n = 1;

	for (int i = 0; i <= n; i++)
	{
		int x = x0 + (x1 - x0) * i / n;
		int y = y0 + (y1 - y0) * i / n;
		gEngfuncs.pfnFillRGBA(x, y, 2, 2, r, g, b, 255);
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

static void DrawSkeleton(int idx, cl_entity_t *e, int r, int g, int b)
{
	if (g_EspBoneCount[idx] <= 0)
		return;

	// кости устарели (игрока не рендерили в этом кадре) - не рисуем
	if (gEngfuncs.GetClientTime() - g_EspBoneTime[idx] > 0.1f)
		return;

	if (!e->model)
		return;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata(e->model);
	if (!hdr)
		return;

	mstudiobone_t *bones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	int count = g_EspBoneCount[idx];
	if (count > hdr->numbones) count = hdr->numbones;

	for (int b2 = 0; b2 < count; b2++)
	{
		int p = bones[b2].parent;
		if (p < 0) continue;

		float a[3] = { g_EspBones[idx][b2][0][3], g_EspBones[idx][b2][1][3], g_EspBones[idx][b2][2][3] };
		float c[3] = { g_EspBones[idx][p][0][3],  g_EspBones[idx][p][1][3],  g_EspBones[idx][p][2][3]  };

		int ax, ay, cx, cy;
		if (W2S(a, ax, ay) && W2S(c, cx, cy))
			Line2D(ax, ay, cx, cy, r, g, b);
	}
}

// ---------------------------------------------------------------------------
void Cheat_Draw()
{
	if (!ch_esp || ch_esp->value == 0.0f)
		return;

	// работаем только там, где сервер разрешил читы
	if (gEngfuncs.pfnGetCvarFloat("sv_cheats") == 0.0f)
		return;

	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	if (!me)
		return;

	int maxPl = gEngfuncs.GetMaxClients();

	for (int i = 1; i <= maxPl; i++)
	{
		cl_entity_t *e = gEngfuncs.GetEntityByIndex(i);
		if (!e || e == me || !e->player)
			continue;

		// игрок не обновлялся сервером (вне PVS) - пропускаем "призраков"
		if (e->curstate.messagenum < me->curstate.messagenum)
			continue;

		// пропускаем своих по команде
		if (g_PlayerExtraInfo[i].teamnumber == g_PlayerExtraInfo[me->index].teamnumber)
			continue;

		// высота зависит от стойки: usehull 0 = стоя, 1 = сидя
		float top = (e->curstate.usehull == 1) ? 14.0f : 28.0f;
		float bot = (e->curstate.usehull == 1) ? -18.0f : -36.0f;

		float head[3] = { e->origin[0], e->origin[1], e->origin[2] + top };
		float feet[3] = { e->origin[0], e->origin[1], e->origin[2] + bot };

		int hx, hy, fx, fy;
		bool boxOk = W2S(head, hx, hy) && W2S(feet, fx, fy);

		// Скелет (режимы 1 и 2)
		DrawSkeleton(i, e, 255, 0, 0);

		// Бокс (режим 2)
		if (ch_esp->value >= 2.0f && boxOk)
			DrawBox(hx, hy, fy, 255, 255, 0);
	}
}

/*
===============================================================================
 ПАТЧ 1: StudioModelRenderer.cpp
 В StudioDrawPlayer (или где у вас вызывается StudioSetupBones для игрока),
 сразу после StudioSetupBones() добавить:

	extern float g_EspBones[33][MAXSTUDIOBONES][3][4];
	extern int   g_EspBoneCount[33];
	extern float g_EspBoneTime[33];

	int idx = m_pCurrentEntity->index;
	if (idx >= 1 && idx <= 32 && m_pCurrentEntity->player)
	{
		memcpy(g_EspBones[idx], *m_pbonetransform, sizeof(float) * MAXSTUDIOBONES * 12);
		g_EspBoneCount[idx] = m_pStudioHeader->numbones;
		g_EspBoneTime[idx]  = gEngfuncs.GetClientTime();
	}

 Если m_pbonetransform у вас объявлен не как указатель на массив, а как
 float[MAXSTUDIOBONES][3][4], уберите звёздочку: memcpy(g_EspBones[idx], m_pbonetransform, ...)

 ПАТЧ 2: hud.cpp / hud_redraw.cpp
 - в CHud::Init()   добавить:  Cheat_Init();
 - в CHud::Redraw() добавить:  Cheat_Draw();
 - вверху файла:               void Cheat_Init(); void Cheat_Draw();

 ПАТЧ 3: CMakeLists.txt (cl_dll)
 - добавить cheat_esp.cpp в список исходников.

 Использование в консоли:
	sv_cheats 1
	ch_esp 1   // скелеты
	ch_esp 2   // скелеты + бокс
===============================================================================
*/
