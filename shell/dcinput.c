//
// dcinput.c - DirectInput keyboard + controller (see dcinput.h).
//
// Keyboard: polled each frame, edge-detected (with auto-repeat for nav keys),
// DIK scan codes mapped to VK and queued for the shell's OnKey.
// Controller: analog stick -> cursor delta (deadzone + speed); A/B/X/Y -> click.
//
#define CINTERFACE
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <dinput.h>
#include "dcinput.h"

#define SCRW 640
#define SCRH 480

static LPDIRECTINPUT        g_di  = NULL;
static LPDIRECTINPUTDEVICE2 g_kbd = NULL;
static LPDIRECTINPUTDEVICE2 g_joy = NULL;

static BYTE  g_now[256], g_last[256];
static DWORD g_repeatAt[256];
static int   g_cx = SCRW / 2, g_cy = SCRH / 2;
static int   g_btnLast = 0, g_click = 0;

static DWORD g_q[32];
static int   g_qh = 0, g_qt = 0;

static void Push(DWORD vk) { int n = (g_qh + 1) & 31; if (n != g_qt) { g_q[g_qh] = vk; g_qh = n; } }

static const struct { int dik; DWORD vk; int nav; } g_map[] =
{
    { DIK_UP, VK_UP, 1 }, { DIK_DOWN, VK_DOWN, 1 }, { DIK_LEFT, VK_LEFT, 1 }, { DIK_RIGHT, VK_RIGHT, 1 },
    { DIK_RETURN, VK_RETURN, 0 }, { DIK_ESCAPE, VK_ESCAPE, 0 }, { DIK_TAB, VK_TAB, 0 }, { DIK_BACK, VK_BACK, 1 },
    { DIK_0, '0', 0 }, { DIK_1, '1', 0 }, { DIK_2, '2', 0 }, { DIK_3, '3', 0 }, { DIK_4, '4', 0 },
    { DIK_5, '5', 0 }, { DIK_6, '6', 0 }, { DIK_7, '7', 0 }, { DIK_8, '8', 0 }, { DIK_9, '9', 0 },
    { DIK_C, 'C', 0 },
};
#define NMAP (sizeof(g_map) / sizeof(g_map[0]))

static BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCE di, LPVOID ctx)
{
    LPDIRECTINPUTDEVICE  d1 = NULL;
    LPDIRECTINPUTDEVICE2 d2 = NULL;
    DWORD                t;

    if (IDirectInput_CreateDevice(g_di, &di->guidInstance, &d1, NULL) != DI_OK)
        return DIENUM_CONTINUE;
    if (IDirectInputDevice_QueryInterface(d1, &IID_IDirectInputDevice2, (LPVOID *)&d2) != S_OK)
    {
        IDirectInputDevice_Release(d1);
        return DIENUM_CONTINUE;
    }
    IDirectInputDevice_Release(d1);

    t = GET_DIDEVICE_TYPE(di->dwDevType);
    if (t == DIDEVTYPE_KEYBOARD && !g_kbd)
    {
        IDirectInputDevice2_SetDataFormat(d2, &c_dfDIKeyboard);
        IDirectInputDevice2_Acquire(d2);
        g_kbd = d2;
        OutputDebugStringW(L"DCIN: keyboard acquired\r\n");
    }
    else if (t == DIDEVTYPE_JOYSTICK && !g_joy)
    {
        DIPROPRANGE r;
        IDirectInputDevice2_SetDataFormat(d2, &c_dfDIJoystick);
        memset(&r, 0, sizeof(r));
        r.diph.dwSize       = sizeof(DIPROPRANGE);
        r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        r.diph.dwHow        = DIPH_DEVICE;
        r.lMin = -1000; r.lMax = 1000;
        IDirectInputDevice2_SetProperty(d2, DIPROP_RANGE, &r.diph);
        IDirectInputDevice2_Acquire(d2);
        g_joy = d2;
        OutputDebugStringW(L"DCIN: joystick acquired\r\n");
    }
    else
    {
        IDirectInputDevice2_Release(d2);
    }
    return DIENUM_CONTINUE;
}

BOOL DInInit(HWND hwnd)
{
    if (DirectInputCreate(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &g_di, NULL) != DI_OK)
    {
        OutputDebugStringW(L"DCIN: DirectInputCreate FAILED\r\n");
        return FALSE;
    }
    IDirectInput_EnumDevices(g_di, 0, EnumCb, NULL, DIEDFL_ATTACHEDONLY);
    return (g_kbd != NULL);
}

void DInShutdown(void)
{
    if (g_kbd) { IDirectInputDevice2_Unacquire(g_kbd); IDirectInputDevice2_Release(g_kbd); g_kbd = NULL; }
    if (g_joy) { IDirectInputDevice2_Unacquire(g_joy); IDirectInputDevice2_Release(g_joy); g_joy = NULL; }
    if (g_di)  { IDirectInput_Release(g_di); g_di = NULL; }
}

void DInUpdate(void)
{
    DWORD nowt = GetTickCount();
    int   i;

    if (g_kbd)
    {
        for (i = 0; i < 256; i++) g_last[i] = g_now[i];
        IDirectInputDevice2_Poll(g_kbd);
        if (IDirectInputDevice2_GetDeviceState(g_kbd, 256, g_now) != DI_OK)
            IDirectInputDevice2_Acquire(g_kbd);
        else
        {
            for (i = 0; i < (int)NMAP; i++)
            {
                int  dik  = g_map[i].dik;
                BOOL down = (g_now[dik] & 0x80) != 0;
                BOOL was  = (g_last[dik] & 0x80) != 0;
                if (down && !was)            { Push(g_map[i].vk); g_repeatAt[dik] = nowt + 350; }
                else if (down && was && g_map[i].nav && nowt >= g_repeatAt[dik])
                                             { Push(g_map[i].vk); g_repeatAt[dik] = nowt + 100; }
            }
        }
    }

    if (g_joy)
    {
        DIJOYSTATE js;
        IDirectInputDevice2_Poll(g_joy);
        if (IDirectInputDevice2_GetDeviceState(g_joy, sizeof(js), &js) != DI_OK)
            IDirectInputDevice2_Acquire(g_joy);
        else
        {
            int ax = js.lX < 0 ? -js.lX : js.lX;
            int ay = js.lY < 0 ? -js.lY : js.lY;
            int btn;
            if (ax > 150) g_cx += js.lX / 80;
            if (ay > 150) g_cy += js.lY / 80;
            if (g_cx < 0) g_cx = 0;  if (g_cx >= SCRW) g_cx = SCRW - 1;
            if (g_cy < 0) g_cy = 0;  if (g_cy >= SCRH) g_cy = SCRH - 1;
            btn = (js.rgbButtons[0] | js.rgbButtons[1] | js.rgbButtons[2] | js.rgbButtons[3]) & 0x80;
            if (btn && !g_btnLast) g_click = 1;
            g_btnLast = btn;
        }
    }
}

int DInNextKey(DWORD *vk)
{
    if (g_qt == g_qh) return 0;
    *vk = g_q[g_qt];
    g_qt = (g_qt + 1) & 31;
    return 1;
}

int  DInHasPointer(void)        { return g_joy != NULL; }
void DInCursor(int *x, int *y)  { *x = g_cx; *y = g_cy; }
int  DInTookClick(void)         { int c = g_click; g_click = 0; return c; }
