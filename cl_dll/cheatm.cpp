#include "hud.h"      // базовые определения клиента (gEngfuncs и т.д.)
#include "cl_util.h"  // вспомогательные макросы

static cvar_t *ch_esp; // переключатель ESP: 0 = выкл, 1 = вкл

void Cheat_Init()
{
    // регистрируем cvar, по умолчанию выключен (0)
    ch_esp = gEngfuncs.pfnRegisterVariable("ch_esp", "0", 0);
}

void Cheat_Draw()
{
    if (!ch_esp || ch_esp->value == 0.0f)
        return; // чит выключен, ничего не делаем

    // сюда на следующем шаге добавим рамки ESP
}
