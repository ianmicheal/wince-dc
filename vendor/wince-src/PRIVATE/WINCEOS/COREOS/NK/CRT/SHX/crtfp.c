/*
 * crtfp.c - SH-4 soft-float helper STUBS.
 *
 * OURS. The kernel's FPU-disabled exception handler (shfloat.c
 * HandleHWFloatException) references these compiler soft-float helpers. They are
 * only reached if code executes an FP op while the FPU is trapped. For first
 * boot (no FP) these stubs just satisfy the linker.
 *
 * !!! STUBS - return 0. Implement real IEEE soft-float (or enable the FPU and
 *     drop this path) before relying on any floating-point in the kernel. !!!
 */
double _addd(double a, double b) { return 0.0; }
double _subd(double a, double b) { return 0.0; }
double _muld(double a, double b) { return 0.0; }
double _divd(double a, double b) { return 0.0; }
float  _adds(float a, float b)   { return 0.0f; }
float  _subs(float a, float b)   { return 0.0f; }
float  _muls(float a, float b)   { return 0.0f; }
float  _divs(float a, float b)   { return 0.0f; }

double _itod(int x)     { return 0.0; }
float  _itos(int x)     { return 0.0f; }
int    _dtoi(double x)  { return 0; }
float  _dtos(double x)  { return 0.0f; }
double _stod(float x)   { return 0.0; }
int    _stoi(float x)   { return 0; }

int _eqd(double a, double b) { return 0; }
int _gtd(double a, double b) { return 0; }
int _eqs(float a, float b)   { return 0; }
int _gts(float a, float b)   { return 0; }

double sqrt(double x) { return 0.0; }
