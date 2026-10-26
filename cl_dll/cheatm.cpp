#include "hud.h"
#include "cl_util.h"
#include "triangleapi.h"

static cvar_t *ch_esp; // 0 = выкл, 1 = вкл

void Cheat_Init()
{
	ch_esp = gEngfuncs.pfnRegisterVariable("ch_esp", "0", 0);
}

void Cheat_Draw()
{
	if (!ch_esp || ch_esp->value == 0.0f)
		return;

	cl_entity_t *me = gEngfuncs.GetLocalPlayer();
	if (!me)
		return;

	int maxPl = gEngfuncs.GetMaxClients();

	for (int i = 1; i <= maxPl; i++)
	{
		cl_entity_t *e = gEngfuncs.GetEntityByIndex(i);
		if (!e || e == me || !e->player)
			continue; // пропускаем пустых, себя и не-игроков

		// пропускаем своих по команде
		if (g_PlayerExtraInfo[i].teamnumber == g_PlayerExtraInfo[me->index].teamnumber)
			continue;

		float head[3] = { e->origin[0], e->origin[1], e->origin[2] + 28 };
		float feet[3] = { e->origin[0], e->origin[1], e->origin[2] - 36 };
		float sh[3], sf[3]; // WorldToScreen пишет 3 числа

		// возвращает не 0, если точка за камерой
		if (gEngfuncs.pTriAPI->WorldToScreen(head, sh) ||
		    gEngfuncs.pTriAPI->WorldToScreen(feet, sf))
			continue;

		int cx = (int)((1.0f + sh[0]) * ScreenWidth  * 0.5f);
		int y1 = (int)((1.0f - sh[1]) * ScreenHeight * 0.5f);
		int y2 = (int)((1.0f - sf[1]) * ScreenHeight * 0.5f);
		int h = y2 - y1;
		if (h < 4) continue;
		int w = h / 2;
		int x1 = cx - w / 2;

		// рамка из четырёх полосок (x, y, ширина, высота, r, g, b, a)
		gEngfuncs.pfnFillRGBA(x1,     y1, w, 1, 255, 0, 0, 255); // верх
		gEngfuncs.pfnFillRGBA(x1,     y2, w, 1, 255, 0, 0, 255); // низ
		gEngfuncs.pfnFillRGBA(x1,     y1, 1, h, 255, 0, 0, 255); // лево
		gEngfuncs.pfnFillRGBA(x1 + w, y1, 1, h, 255, 0, 0, 255); // право
	}
}
