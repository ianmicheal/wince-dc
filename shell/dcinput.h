//
// dcinput.h - DirectInput layer for the shell: polled keyboard (low-latency,
// replaces WM_KEYDOWN) + a pointer from either a mouse (DC Maple / host mouse,
// relative deltas) or the controller (analog stick moves the cursor, buttons =
// click).
//
#ifndef DCINPUT_H
#define DCINPUT_H

#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

	BOOL DInInit(HWND hwnd); // TRUE if a DI keyboard was acquired (use it, not WM_KEYDOWN)
	void DInShutdown(void);
	void DInUpdate(void); // poll all devices; call once per loop

	void DInRelease(void);   // hand input to a launched full-screen app (drop our acquire)
	void DInReacquire(void); // app exited: take input back (re-primed, no phantom edge)

	int DInNextKey(DWORD *vk); // 1 + VK for each queued key-down (edge / auto-repeat)
	int DInHasPointer(void);   // TRUE if a mouse or controller pointer is active
	void DInCursor(int *x, int *y);
	int DInTookClick(void);    // TRUE once per mouse click (cursor paradigm)
	int DInTookActivate(void); // TRUE once per controller face-button press (-> Enter)
	int DInPointerDown(
	    void); // TRUE while the pointer button is held (mouse-L / controller-A) - for drag

// Per-button controller edges, for apps that need the raw face/Start buttons (e.g. the
// browser maps B=back, Y=forward, X=exit, Start=address). 1 once per press; the shell's
// own cursor/activate model (DInTookActivate) is unaffected.
#define DIN_BTN_A     0
#define DIN_BTN_B     1
#define DIN_BTN_X     2
#define DIN_BTN_Y     3
#define DIN_BTN_START 4
	int DInButtonEdge(int btn); // 1 once when DIN_BTN_* transitions to pressed

	// GWES WM_MOUSE* fallback feed (the shell forwards window mouse messages here in
	// case the DC mouse reaches WinCE through the window queue, not DirectInput).
	void DInSetCursor(int x, int y); // absolute client coords
	void DInPostClick(void);

#ifdef __cplusplus
}
#endif

#endif // DCINPUT_H
