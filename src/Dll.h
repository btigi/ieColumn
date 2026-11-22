#pragma once

#include <windows.h>

extern HINSTANCE g_hInst;

void DllAddRef();
void DllRelease();
HRESULT RegisterHandler();
HRESULT UnregisterHandler();
