// cl_dll/cheat.cpp  (всё в одном файле)
// Debug-оверлей для тестов на своём сервере с ботами. Только при sv_cheats 1.
//
// Внутри:
//   1. ESP: скелет + круг головы, бокс, дистанция, радиус показа
//   2. Автошут (триггербот): стреляет, когда прицел уже на враге, доводки нет
//   3. Круговое меню (зажми бинд, веди мышью, отпусти) - как закупка CS:GO
//
// Команды / cvar:
//   ch_esp 0/1/2        выкл / скелет + круг головы / то же + бокс
//   ch_dist 0/1         дистанция
//   ch_esp_dist N       радиус показа (1000 по умолчанию, 0 = вся карта)
//   ch_auto 0/1         автошут
//   ch_bhop 0/1         банихоп: держишь пробел - прыгает сам
//   ch_bhop_toggle      вкл/выкл банихоп
//   ch_toggle           вкл/выкл ESP
//   ch_box_toggle       вкл/выкл бокс
//   ch_dist_toggle      вкл/выкл дистанцию
//   ch_auto_toggle      вкл/выкл автошут
//   ch_alloff           выключить всё
//   +ch_menu / -ch_menu круговое меню
//
// Подключение (подробно в конце файла):
//   hud.cpp          CHud::Init()      -> Cheat_Init()
//   hud_redraw.cpp   CHud::Redraw()    -> Cheat_Draw()
//   input.cpp        CL_CreateMove()   -> Cheat_Auto(cmd) + блок стрельбы при открытом меню
//   (хук мыши не нужен: меню берёт дельту из поворота камеры внутри Cheat_Auto)
//   cl_util.h        объявления (см. ниже)

#include "hud.h"
#include "cl_util.h"
#include "triangleapi.h"
#include "const.h"
#include "studio.h"
#include "com_model.h"
#include "r_studioint.h"
#include "usercmd.h"
#include "pm_defs.h"
#include "event_api.h"
#include "in_defs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

extern engine_studio_api_t IEngineStudio;

static cvar_t *ch_esp;
static cvar_t *ch_dist;
static cvar_t *ch_esp_dist;
static cvar_t *ch_auto;
static cvar_t *ch_menu_sens;
static cvar_t *ch_bhop;

static float CV(const char *name)
{
	return gEngfuncs.pfnGetCvarFloat((char *)name);
}

static void SetCV(const char *name, float v)
{
	gEngfuncs.Cvar_SetValue((char *)name, v);
}

static bool CheatsOn()

// ===========================================================================
// 1. ESP
// ===========================================================================

// Кэш костей. Заполняется из GameStudioModelRenderer.cpp (патч в конце файла).
float g_EspBones[33][MAXSTUDIOBONES][3][4];
int   g_EspBoneCount[33];
float g_EspBoneTime[33];

// Упрощённый скелет: 16 суставов вместо всех костей
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

static void DrawEsp()
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

// ===========================================================================
// 2. Автошут
// ===========================================================================

// индекс игрока под прицелом (0 = никого / стена)
static int TraceTarget(cl_entity_t *me)
{
	vec3_t ang, fwd, org, end, eye;
	pmtrace_t tr;

	gEngfuncs.GetViewAngles(ang);
	AngleVectors(ang, fwd, NULL, NULL);

	gEngfuncs.pEventAPI->EV_LocalPlayerViewheight(eye);
	org[0] = me->origin[0] + eye[0];
	org[1] = me->origin[1] + eye[1];
	org[2] = me->origin[2] + eye[2];

	for (int i = 0; i < 3; i++)
		end[i] = org[i] + fwd[i] * 8192.0f;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(false, true);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(me->index - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);
	gEngfuncs.pEventAPI->EV_PlayerTrace(org, end, PM_STUDIO_BOX, me->index, &tr);
	gEngfuncs.pEventAPI->EV_PopPMStates();

	int ent = gEngfuncs.pEventAPI->EV_IndexFromTrace(&tr);
	if (ent < 1 || ent > gEngfuncs.GetMaxClients())
		return 0;
	return ent;
}

// стоит ли игрок на земле (трасса на 2 юнита вниз по хуллу игрока)
static bool OnGround(cl_entity_t *me)
{
	vec3_t org, end;
	pmtrace_t tr;

	org[0] = me->origin[0];
	org[1] = me->origin[1];
	org[2] = me->origin[2];
	end[0] = org[0];
	end[1] = org[1];
	end[2] = org[2] - 2.0f;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(false, true);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(me->index - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(me->curstate.usehull);
	gEngfuncs.pEventAPI->EV_PlayerTrace(org, end, PM_NORMAL, me->index, &tr);
	gEngfuncs.pEventAPI->EV_PopPMStates();

	return tr.fraction < 1.0f && tr.plane.normal[2] >= 0.7f;
}

// ===========================================================================
// 3. Круговое меню
// ===========================================================================

static bool  g_MenuOpen = false;
static float g_mx = 0.0f, g_my = 0.0f; // курсор относительно центра экрана
static float g_SavedAng[3];            // угол обзора на момент открытия меню
static bool  g_HaveAng = false;

static const int   ITEM_COUNT = 7;
static const float R_OUT  = 150.0f;
static const float R_DEAD = 30.0f;
static const float R_ITEM = 105.0f;
static const float PI2    = 6.2831853f;

static const char *OnOff(bool b) { return b ? "ON" : "OFF"; }

static void ItemLabel(int i, char *out, int size)
{
	switch (i)
	{
	case 0: snprintf(out, size, "Skeleton [%s]",  OnOff(CV("ch_esp") >= 1.0f)); break;
	case 1: snprintf(out, size, "Box [%s]",       OnOff(CV("ch_esp") >= 2.0f)); break;
	case 2: snprintf(out, size, "Distance [%s]",  OnOff(CV("ch_dist") != 0.0f)); break;
	case 3: snprintf(out, size, "Autoshoot [%s]", OnOff(CV("ch_auto") != 0.0f)); break;
	case 4:
	{
		float d = CV("ch_esp_dist");
		if (d <= 0.0f) snprintf(out, size, "Range [MAP]");
		else           snprintf(out, size, "Range [%d]", (int)d);
		break;
	}
	case 5: snprintf(out, size, "Bhop [%s]", OnOff(CV("ch_bhop") != 0.0f)); break;
	default: snprintf(out, size, "All OFF"); break;
	}
}

static void ItemApply(int i)
{
	switch (i)
	{
	case 0: SetCV("ch_esp", CV("ch_esp") == 1.0f ? 0.0f : 1.0f); break;
	case 1: SetCV("ch_esp", CV("ch_esp") >= 2.0f ? 1.0f : 2.0f); break;
	case 2: SetCV("ch_dist", CV("ch_dist") != 0.0f ? 0.0f : 1.0f); break;
	case 3: SetCV("ch_auto", CV("ch_auto") != 0.0f ? 0.0f : 1.0f); break;
	case 4:
	{
		float d = CV("ch_esp_dist");
		if (d == 600.0f)       SetCV("ch_esp_dist", 1000.0f);
		else if (d == 1000.0f) SetCV("ch_esp_dist", 0.0f);
		else                   SetCV("ch_esp_dist", 600.0f);
		break;
	}
	case 5: SetCV("ch_bhop", CV("ch_bhop") != 0.0f ? 0.0f : 1.0f); break;
	default:
		SetCV("ch_esp", 0.0f);
		SetCV("ch_dist", 0.0f);
		SetCV("ch_auto", 0.0f);
		SetCV("ch_bhop", 0.0f);
		break;
	}
}

// сектор под курсором, -1 = мёртвая зона в центре
static int MenuSelected()
{
	float len = sqrtf(g_mx * g_mx + g_my * g_my);
	if (len < R_DEAD)
		return -1;

	// угол по часовой от верха, 0..2pi
	float a = atan2f(g_mx, -g_my);
	if (a < 0.0f) a += PI2;

	float step = PI2 / (float)ITEM_COUNT;
	int idx = (int)((a + step * 0.5f) / step);
	return idx % ITEM_COUNT;
}

static void MenuOpen()
{
	if (!CheatsOn())
	{
		gEngfuncs.Con_Printf("ch_menu: need sv_cheats 1\n");
		return;
	}
	g_MenuOpen = true;
	g_mx = g_my = 0.0f;
	g_HaveAng = false;
}

static void MenuClose()
{
	if (!g_MenuOpen)
		return;
	int sel = MenuSelected();
	if (sel >= 0)
		ItemApply(sel);
	g_MenuOpen = false;
}

bool Cheat_MenuIsOpen()
{
	return g_MenuOpen;
}

// true = дельту забрало меню, вид крутить не нужно
bool Cheat_MenuMouse(int dx, int dy)
{
	if (!g_MenuOpen)
		return false;

	g_mx += (float)dx;
	g_my += (float)dy;

	float len = sqrtf(g_mx * g_mx + g_my * g_my);
	if (len > R_OUT)
	{
		g_mx *= R_OUT / len;
		g_my *= R_OUT / len;
	}
	return true;
}

static void Dot(int x, int y, int r, int g, int b, int a)
{
	gEngfuncs.pfnFillRGBA(x - 1, y - 1, 3, 3, r, g, b, a);
}

static void DrawMenu()
{
	if (!g_MenuOpen)
		return;

	// сервер выключил читы - закрываем без применения
	if (!CheatsOn())
	{
		g_MenuOpen = false;
		return;
	}

	int cx = ScreenWidth / 2;
	int cy = ScreenHeight / 2;
	float step = PI2 / (float)ITEM_COUNT;

	// затемнение под меню
	gEngfuncs.pfnFillRGBA(cx - 190, cy - 190, 380, 380, 0, 0, 0, 70);

	// внешнее кольцо
	for (int k = 0; k < 72; k++)
	{
		float a = (float)k * PI2 / 72.0f;
		Dot(cx + (int)(sinf(a) * R_OUT), cy - (int)(cosf(a) * R_OUT), 200, 200, 200, 255);
	}

	// разделители секторов
	for (int i = 0; i < ITEM_COUNT; i++)
	{
		float a = ((float)i + 0.5f) * step;
		for (int d = (int)R_DEAD; d <= (int)R_OUT; d += 6)
			Dot(cx + (int)(sinf(a) * d), cy - (int)(cosf(a) * d), 120, 120, 120, 255);
	}

	int sel = MenuSelected();

	// подписи
	for (int i = 0; i < ITEM_COUNT; i++)
	{
		float a = (float)i * step;
		int px = cx + (int)(sinf(a) * R_ITEM);
		int py = cy - (int)(cosf(a) * R_ITEM);

		char label[32];
		ItemLabel(i, label, sizeof(label));

		int w = 0, h = 0;
		gEngfuncs.pfnDrawConsoleStringLen(label, &w, &h);

		if (i == sel)
			gEngfuncs.pfnFillRGBA(px - w / 2 - 6, py - 10, w + 12, 20, 255, 200, 0, 170);
		else
			gEngfuncs.pfnFillRGBA(px - w / 2 - 6, py - 10, w + 12, 20, 0, 0, 0, 150);

		if (i == sel)
			gEngfuncs.pfnDrawSetTextColor(0.0f, 0.0f, 0.0f);
		else
			gEngfuncs.pfnDrawSetTextColor(1.0f, 1.0f, 1.0f);
		gEngfuncs.pfnDrawConsoleString(px - w / 2, py - 7, label);
	}

	// линия до курсора и сам курсор
	float len = sqrtf(g_mx * g_mx + g_my * g_my);
	int n = (int)(len / 5.0f);
	for (int s = 0; s <= n; s++)
	{
		float t = n > 0 ? (float)s / (float)n : 0.0f;
		Dot(cx + (int)(g_mx * t), cy + (int)(g_my * t), 255, 255, 0, 255);
	}
	gEngfuncs.pfnFillRGBA(cx + (int)g_mx - 3, cy + (int)g_my - 3, 7, 7, 255, 255, 0, 255);
}

// ===========================================================================
// Команды, init, публичные точки входа
// ===========================================================================

static void Cmd_EspToggle()
{
	SetCV("ch_esp", CV("ch_esp") != 0.0f ? 0.0f : 1.0f);
}

static void Cmd_BoxToggle()
{
	SetCV("ch_esp", CV("ch_esp") >= 2.0f ? 1.0f : 2.0f);
}

static void Cmd_DistToggle()
{
	SetCV("ch_dist", CV("ch_dist") != 0.0f ? 0.0f : 1.0f);
}

static void Cmd_AutoToggle()
{
	SetCV("ch_auto", CV("ch_auto") != 0.0f ? 0.0f : 1.0f);
}

static void Cmd_BhopToggle()
{
	SetCV("ch_bhop", CV("ch_bhop") != 0.0f ? 0.0f : 1.0f);
}

static void Cmd_AllOff()
{
	SetCV("ch_esp", 0.0f);
	SetCV("ch_dist", 0.0f);
	SetCV("ch_auto", 0.0f);
	SetCV("ch_bhop", 0.0f);
}

void Cheat_Init()
{
	ch_esp      = gEngfuncs.pfnRegisterVariable("ch_esp", "0", 0);
	ch_dist     = gEngfuncs.pfnRegisterVariable("ch_dist", "0", 0);
	ch_esp_dist = gEngfuncs.pfnRegisterVariable("ch_esp_dist", "1000", 0);
	ch_auto     = gEngfuncs.pfnRegisterVariable("ch_auto", "0", 0);
	ch_menu_sens = gEngfuncs.pfnRegisterVariable("ch_menu_sens", "12", 0);
	ch_bhop     = gEngfuncs.pfnRegisterVariable("ch_bhop", "0", 0);

	gEngfuncs.pfnAddCommand("ch_toggle",      Cmd_EspToggle);
	gEngfuncs.pfnAddCommand("ch_box_toggle",  Cmd_BoxToggle);
	gEngfuncs.pfnAddCommand("ch_dist_toggle", Cmd_DistToggle);
	gEngfuncs.pfnAddCommand("ch_auto_toggle", Cmd_AutoToggle);
	gEngfuncs.pfnAddCommand("ch_bhop_toggle", Cmd_BhopToggle);
	gEngfuncs.pfnAddCommand("ch_alloff",      Cmd_AllOff);
	gEngfuncs.pfnAddCommand("+ch_menu",       MenuOpen);
	gEngfuncs.pfnAddCommand("-ch_menu",       MenuClose);
}

// Вызывать из CHud::Redraw()
void Cheat_Draw()
{
	// меню рисуем первыми: они не зависят от того, включён ли ESP
	DrawMenu();

	// работаем только там, где сервер разрешил читы
	if (!CheatsOn())
		return;

	DrawEsp();
}

// Вызывать в конце CL_CreateMove()
void Cheat_Auto(struct usercmd_s *cmd)
{
	// пока открыто круговое меню: не стреляем, камеру держим на месте,
	// а курсор меню двигаем по тому, насколько мышь пыталась её повернуть
	if (g_MenuOpen)
	{
		cmd->buttons &= ~IN_ATTACK;

		vec3_t ang;
		gEngfuncs.GetViewAngles(ang);

		if (!g_HaveAng)
		{
			g_SavedAng[0] = ang[0];
			g_SavedAng[1] = ang[1];
			g_SavedAng[2] = ang[2];
			g_HaveAng = true;
		}
		else
		{
			float dyaw   = ang[1] - g_SavedAng[1];
			float dpitch = ang[0] - g_SavedAng[0];
			while (dyaw >  180.0f) dyaw -= 360.0f;
			while (dyaw < -180.0f) dyaw += 360.0f;

			// пикселей курсора на градус поворота (ch_menu_sens)
			float k = ch_menu_sens ? ch_menu_sens->value : 12.0f;

			g_mx += -dyaw * k;   // yaw растёт влево
			g_my +=  dpitch * k;

			float len = sqrtf(g_mx * g_mx + g_my * g_my);
			if (len > R_OUT)
			{
				g_mx *= R_OUT / len;
				g_my *= R_OUT / len;
			}
		}

		gEngfuncs.SetViewAngles(g_SavedAng);
		cmd->viewangles[0] = g_SavedAng[0];
		cmd->viewangles[1] = g_SavedAng[1];
		cmd->viewangles[2] = g_SavedAng[2];
		return;
	}

	// банихоп: пока в воздухе, прыжок отпускаем, чтобы при приземлении он нажался заново
	if (ch_bhop && ch_bhop->value != 0.0f && CheatsOn() && (cmd->buttons & IN_JUMP))
	{
		cl_entity_t *pl = gEngfuncs.GetLocalPlayer();
		if (pl && !OnGround(pl))
			cmd->buttons &= ~IN_JUMP;
	}

	if (!ch_auto || ch_auto->value == 0.0f)
		return;
	if (!CheatsOn())
		return;

	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	if (!me)
		return;

	int t = TraceTarget(me);
	if (!t)
		return;

	// своих по команде не трогаем
	if (g_PlayerExtraInfo[t].teamnumber == g_PlayerExtraInfo[me->index].teamnumber)
		return;

	cmd->buttons |= IN_ATTACK;
}

/*
===============================================================================
 ПОДКЛЮЧЕНИЕ

 cl_util.h (объявления):
	void Cheat_Init();
	void Cheat_Draw();
	void Cheat_Auto(struct usercmd_s *cmd);
	bool Cheat_MenuIsOpen();
	bool Cheat_MenuMouse(int dx, int dy);

 hud.cpp:           Cheat_Init() в конце CHud::Init()
 hud_redraw.cpp:    Cheat_Draw() в CHud::Redraw()
 input.cpp:         Cheat_Auto(cmd) в конце CL_CreateMove()
 IN_MouseMove:      хук не нужен. Cheat_Auto(cmd) должен вызываться ПОСЛЕ того, как
                    CL_CreateMove применил мышь и записал cmd->viewangles (в самом конце).
                    Скорость курсора меню: ch_menu_sens (пикселей на градус, 12 по умолчанию).

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

 СЕРВЕР (dlls/client.cpp, AddToFullPack) - вид сквозь стены на всю карту:

	if (ent != host)
	{
		bool force = CVAR_GET_FLOAT("sv_cheats") != 0 && (ent->v.flags & FL_CLIENT);
		if (!force && !ENGINE_CHECK_VISIBILITY((const struct edict_s *)ent, pSet))
			return 0;
	}

 Бинды (один раз, в консоли на английской раскладке):
	bind RSHIFT +ch_menu
	(если RSHIFT неизвестна движку - bind SHIFT +ch_menu)
===============================================================================
*/
