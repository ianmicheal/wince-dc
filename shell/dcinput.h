//
// dcinput.h - DirectInput layer for the shell: polled keyboard (low-latency,
// replaces WM_KEYDOWN) + the DC controller as a pointer (analog stick moves a
// cursor, A/B/X/Y = click).
//
#ifndef DCINPUT_H
#define DCINPUT_H

#include <windows.h>

BOOL DInInit(HWND hwnd);     // TRUE if a DI keyboard was acquired (use it, not WM_KEYDOWN)
void DInShutdown(void);
void DInUpdate(void);        // poll all devices; call once per loop

int  DInNextKey(DWORD *vk);  // 1 + VK for each queued key-down (edge / auto-repeat)
int  DInHasPointer(void);    // TRUE if a controller pointer is active
void DInCursor(int *x, int *y);
int  DInTookClick(void);     // TRUE once on each click (button press edge)

#endif // DCINPUT_H
