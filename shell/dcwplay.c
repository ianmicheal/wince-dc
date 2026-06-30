//
// dcwplay.c - DCWin music player (WAV + MP3). A Winamp-style window in the desktop: LCD time,
// spectrum analyzer, transport, seek + volume bars - clickable via the analog-stick pointer
// (DCWinGetPointer) plus keyboard shortcuts.
//
// Audio is STREAMED from the file through a small fixed buffer (g_abSbuf, 32 KB) - NOT loaded
// whole - so an 8 MB MP3 costs ~32 KB of RAM, not 8 MB. The decoder pulls bytes on demand: WAV
// straight from the data chunk, MP3 via minimp3 (vendor/minimp3) frame-by-frame. Decoded PCM is
// pushed into a 1-second looping DirectSound ring (primed once, then refilled behind the play
// cursor). Lots of OutputDebugStringW logging (DCWPLAY:) so the SCIF console shows the DirectSound
// + decode path when there's no sound.
//
// Launched with the file path as the command line (DCWinExec / CreateProcess).
//
#define CINTERFACE
#include <windows.h>
#include <dsound.h>
#include <stdarg.h>
#include "dcwlib.h"
#include "minimp3.h" // API only; the implementation is compiled as C++ in mp3impl.cpp

// ---- UI geometry --------------------------------------------------------------------
#define PW       300
#define PH       150
#define C_BG     RGB(28, 31, 26)
#define C_PANEL  RGB(13, 15, 11)
#define C_EDGE   RGB(49, 54, 41)
#define C_LCD    RGB(47, 224, 122)
#define C_LCDDIM RGB(31, 154, 85)
#define C_LCDHI  RGB(155, 232, 74)
#define C_BTN    RGB(20, 23, 17)

// ---- logging ------------------------------------------------------------------------
#define LOG(s) OutputDebugStringW(L"DCWPLAY: " s L"\r\n")
static void Logf(const WCHAR *pszFmt, ...)
{
	WCHAR ab[160];
	va_list ap;
	va_start(ap, pszFmt);
	wvsprintfW(ab, pszFmt, ap);
	va_end(ap);
	OutputDebugStringW(ab);
}

// ---- audio state --------------------------------------------------------------------
static LPDIRECTSOUND g_pDs;
static LPDIRECTSOUNDBUFFER g_pBuf;
static HWND g_hwndHidden;
static DWORD g_dwBufBytes; // DSound ring size
static DWORD g_dwWritePos; // next byte we'll write into the ring
static int g_nRate = 44100, g_nCh = 2;
static volatile int g_nPlaying, g_nEof;
static volatile DWORD g_dwPlayed; // total stereo frames written to the card
static DWORD g_dwTotalFrames;     // est. total frames (for the seek bar)
static int g_nVol = 80;           // 0..100

// ---- streaming source (no whole-file load) ------------------------------------------
static HANDLE g_hFile = INVALID_HANDLE_VALUE;
static DWORD g_dwSize;      // total file size (for the seek bar)
static DWORD g_dwStreamPos; // file bytes consumed by the decoder (seek bar)
#define SBUF (32 * 1024)
static BYTE g_abSbuf[SBUF];
static int g_nSN, g_nSP;  // valid bytes / consume cursor within g_abSbuf
#define MP3_MAXFRAME 2881 // worst-case MPEG frame; keep this much buffered

static int g_nIsMp3;
static DWORD g_dwWavOff, g_dwWavEnd; // WAV PCM byte window (absolute file offsets)
static int g_nWavBits = 16, g_nWavCh = 2;
static mp3dec_t g_mp3;
// One decoded MPEG frame held as stereo 16-bit PCM. mp3dec yields a fixed 1152-frame block, but
// the ring asks for arbitrary frame counts; we drain this across Decode() calls so the ring is
// filled CONTINUOUSLY (no silence padding for the maxFrames-mod-1152 tail -> no clicks/slowdown).
static short g_aspcm[1152 * 2];
static int g_nPcmN, g_nPcmP; // valid frames / consume cursor within g_aspcm

// Output resampler. The AICA's native rate is EXACTLY 44100 Hz (22.5792 MHz / 512), and only that
// rate is reliably pitch-accurate through the DC's mixerless DirectSound (a 48 kHz buffer plays a
// touch flat - the driver clamps to the 44100 mixer rate). So we always run the AICA buffer at
// 44100 and linearly resample the source into it: a 44100 source is a zero-cost 1:1 passthrough;
// 48000 downsamples; 22050 upsamples. Phase is 16.16 fixed-point; the interp weight is 8-bit so the
// (nxt-cur)*weight product can't overflow int32.
#define OUT_RATE 44100
static int g_nSrcRate = OUT_RATE;                        // decoded source sample rate
static unsigned g_dwRsStep;                              // source frames per output frame, 16.16
static unsigned g_dwRsPhase;                             // fractional source position [0,1), 16.16
static short g_sRsCurL, g_sRsCurR, g_sRsNxtL, g_sRsNxtR; // bracketing source frames
static int g_nRsPrimed, g_nSrcEof;                       // resampler loaded? / source exhausted?

static WCHAR g_aszTrack[80] = L"(no file)";

// viz: mono window of the most recent samples (decoder writes, UI reads)
#define VIZN 256
static short g_asViz[VIZN];
static volatile int g_nVizW;

// dst < src (we always slide toward the front), so a forward byte copy is safe (no memmove dep).
static void Slide(BYTE *pbDst, const BYTE *pbSrc, int n)
{
	int i;
	for (i = 0; i < n; i++)
		pbDst[i] = pbSrc[i];
}

// Slide the unconsumed bytes to the front and read more from the file. After this, g_abSbuf
// holds [0, g_nSN) valid bytes and g_nSP is reset toward 0.
static void StreamFill(void)
{
	DWORD dwGot = 0;
	int nRem;
	if (g_hFile == INVALID_HANDLE_VALUE)
		return;
	nRem = g_nSN - g_nSP;
	if (g_nSP > 0)
	{
		if (nRem > 0)
			Slide(g_abSbuf, g_abSbuf + g_nSP, nRem);
		g_nSN = nRem;
		g_nSP = 0;
	}
	if (g_nSN < SBUF)
	{
		ReadFile(g_hFile, g_abSbuf + g_nSN, (DWORD)(SBUF - g_nSN), &dwGot, 0);
		g_nSN += (int)dwGot;
	}
}

// ---- helpers ------------------------------------------------------------------------
static void PushViz(const short *psStereo, int nFrames)
{
	int i, nW = g_nVizW;
	for (i = 0; i < nFrames; i++)
	{
		g_asViz[nW & (VIZN - 1)] = (short)((psStereo[i * 2] + psStereo[i * 2 + 1]) >> 1);
		nW++;
	}
	g_nVizW = nW;
}

// Configure the resampler for a (newly discovered) source rate. Output is always OUT_RATE.
static void SetSrcRate(int nSrcRate)
{
	g_nSrcRate = nSrcRate > 0 ? nSrcRate : OUT_RATE;
	g_nRate = OUT_RATE; // the AICA buffer always runs at 44100
	g_dwRsStep = ((unsigned)g_nSrcRate << 16) / (unsigned)OUT_RATE; // fits u32 for any sane rate
}

// Pull one decoded source frame (stereo 16-bit, at g_nSrcRate). Returns 0 at end of stream.
static int SrcNext(short *psL, short *psR)
{
	if (g_nIsMp3)
	{
		while (g_nPcmN - g_nPcmP <= 0) // need another MPEG frame
		{
			mp3dec_frame_info_t fi;
			short asTmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
			int n, i;
			if (g_nSN - g_nSP < MP3_MAXFRAME)
				StreamFill();
			if (g_nSN - g_nSP <= 0)
				return 0; // end of file
			n = mp3dec_decode_frame(&g_mp3, g_abSbuf + g_nSP, g_nSN - g_nSP, asTmp, &fi);
			if (fi.frame_bytes == 0)
				return 0; // not enough data (EOF)
			g_nSP += fi.frame_bytes;
			g_dwStreamPos += (DWORD)fi.frame_bytes;
			if (n == 0)
				continue; // header/ID3 skip, no PCM
			for (i = 0; i < n; i++)
			{
				short sL = asTmp[i * fi.channels],
				      sR = (fi.channels > 1) ? asTmp[i * fi.channels + 1] : sL;
				g_aspcm[i * 2] = sL;
				g_aspcm[i * 2 + 1] = sR;
			}
			g_nPcmN = n;
			g_nPcmP = 0;
		}
		*psL = g_aspcm[g_nPcmP * 2];
		*psR = g_aspcm[g_nPcmP * 2 + 1];
		g_nPcmP++;
		return 1;
	}
	else
	{
		int nBps = (g_nWavBits / 8) * (g_nWavCh ? g_nWavCh : 1);
		const BYTE *pb;
		if (g_dwStreamPos + (DWORD)nBps > g_dwWavEnd)
			return 0;
		if (g_nSN - g_nSP < nBps)
			StreamFill();
		if (g_nSN - g_nSP < nBps)
			return 0;
		pb = g_abSbuf + g_nSP;
		if (g_nWavBits == 16)
		{
			*psL = (short)(pb[0] | (pb[1] << 8));
			*psR = (g_nWavCh > 1) ? (short)(pb[2] | (pb[3] << 8)) : *psL;
		}
		else
		{
			*psL = (short)((pb[0] - 128) << 8);
			*psR = (g_nWavCh > 1) ? (short)((pb[1] - 128) << 8) : *psL;
		}
		g_nSP += nBps;
		g_dwStreamPos += (DWORD)nBps;
		return 1;
	}
}

// Produce up to maxFrames OUTPUT frames (at OUT_RATE), linearly resampling the source. 0 = EOF.
static int Decode(short *psOut, int nMaxFrames)
{
	int nGot = 0;
	if (g_hFile == INVALID_HANDLE_VALUE || g_nSrcEof)
		return 0;
	if (!g_nRsPrimed) // load the first two source frames
	{
		if (!SrcNext(&g_sRsCurL, &g_sRsCurR))
		{
			g_nSrcEof = 1;
			return 0;
		}
		if (!SrcNext(&g_sRsNxtL, &g_sRsNxtR))
		{
			g_sRsNxtL = g_sRsCurL;
			g_sRsNxtR = g_sRsCurR;
		}
		g_dwRsPhase = 0;
		g_nRsPrimed = 1;
	}
	while (nGot < nMaxFrames)
	{
		int nF = (int)((g_dwRsPhase >> 8) & 0xff); // 8-bit interp weight (no int32 overflow)
		psOut[nGot * 2] = (short)(g_sRsCurL + (((int)(g_sRsNxtL - g_sRsCurL) * nF) >> 8));
		psOut[nGot * 2 + 1] = (short)(g_sRsCurR + (((int)(g_sRsNxtR - g_sRsCurR) * nF) >> 8));
		nGot++;
		g_dwRsPhase += g_dwRsStep;
		while (g_dwRsPhase >= 0x10000) // step past whole source frames
		{
			g_dwRsPhase -= 0x10000;
			g_sRsCurL = g_sRsNxtL;
			g_sRsCurR = g_sRsNxtR;
			if (!SrcNext(&g_sRsNxtL, &g_sRsNxtR))
			{
				g_nSrcEof = 1;
				g_sRsNxtL = g_sRsCurL;
				g_sRsNxtR = g_sRsCurR;
			}
		}
		if (g_nSrcEof)
			break; // source exhausted
	}
	if (nGot)
		PushViz(psOut, nGot);
	return nGot;
}

// Reset the decoder + stream to the start of audio (re-seek the file, drop the buffer).
static void DecoderReset(void)
{
	if (g_hFile == INVALID_HANDLE_VALUE)
		return;
	if (g_nIsMp3)
	{
		mp3dec_init(&g_mp3);
		SetFilePointer(g_hFile, 0, 0, FILE_BEGIN);
		g_dwStreamPos = 0;
	}
	else
	{
		SetFilePointer(g_hFile, (LONG)g_dwWavOff, 0, FILE_BEGIN);
		g_dwStreamPos = g_dwWavOff;
	}
	g_nSN = g_nSP = 0;
	g_nPcmN = g_nPcmP = 0;
	g_nRsPrimed = 0;
	g_dwRsPhase = 0;
	g_nSrcEof = 0;
	g_dwPlayed = 0;
	g_nEof = 0;
	g_nVizW = 0;
}

// Decode the first MP3 frame to learn the true sample rate/channel count BEFORE we size the AICA
// buffer. minimp3 carries the rate in the frame header, not the file header, so without this the
// buffer is built at a guessed rate and the song plays pitch-shifted (a 48 kHz file through a
// 44.1 kHz buffer is ~1.5 semitones flat). Caller must DecoderReset() afterward to rewind.
static void ProbeMp3Rate(void)
{
	short asTmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
	mp3dec_frame_info_t fi;
	int nTries;
	for (nTries = 0; nTries < 16; nTries++)
	{
		int n;
		if (g_nSN - g_nSP < MP3_MAXFRAME)
			StreamFill();
		if (g_nSN - g_nSP <= 0)
			break;
		n = mp3dec_decode_frame(&g_mp3, g_abSbuf + g_nSP, g_nSN - g_nSP, asTmp, &fi);
		if (fi.frame_bytes == 0)
			break;
		g_nSP += fi.frame_bytes;
		if (n > 0 && fi.hz > 0)
		{
			SetSrcRate(fi.hz);
			g_nCh = fi.channels; // output stays at OUT_RATE; resample the rest
			if (g_dwTotalFrames == 0 &&
			    fi.bitrate_kbps > 0) // estimate length (in OUTPUT frames) from CBR
			{
				DWORD dwSecs = (g_dwSize / 125) / (DWORD)fi.bitrate_kbps;
				g_dwTotalFrames = dwSecs * (DWORD)OUT_RATE;
			}
			Logf(L"DCWPLAY: MP3 src=%dHz ch=%d br=%dk -> out %dHz\r\n", fi.hz, fi.channels,
			     fi.bitrate_kbps, OUT_RATE);
			break;
		}
	}
}

// ---- DirectSound --------------------------------------------------------------------
static int DsInit(void)
{
	WNDCLASSW wc;
	HRESULT hr;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(0);
	wc.lpszClassName = L"DCWPLAYDS";
	RegisterClassW(&wc);
	g_hwndHidden =
	    CreateWindowExW(0, L"DCWPLAYDS", L"", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	hr = DirectSoundCreate(NULL, &g_pDs, NULL);
	Logf(L"DCWPLAY: DirectSoundCreate hr=%08x ds=%08x\r\n", (unsigned)hr, (unsigned)(ULONG)g_pDs);
	if (hr != DS_OK || !g_pDs)
		return 0;
	hr = g_pDs->lpVtbl->SetCooperativeLevel(g_pDs, g_hwndHidden, DSSCL_NORMAL);
	Logf(L"DCWPLAY: SetCooperativeLevel hr=%08x\r\n", (unsigned)hr);
	return 1;
}

static void DsClose(void)
{
	if (g_pBuf)
	{
		g_pBuf->lpVtbl->Stop(g_pBuf);
		g_pBuf->lpVtbl->Release(g_pBuf);
		g_pBuf = NULL;
	}
}

// Create the streaming buffer for the current rate/channels.
static int DsOpenBuffer(void)
{
	DSBUFFERDESC d;
	WAVEFORMATEX wf;
	HRESULT hr;
	DsClose();
	memset(&wf, 0, sizeof(wf));
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = 2;
	wf.nSamplesPerSec = g_nRate;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = 4;
	wf.nAvgBytesPerSec = g_nRate * 4;
	// The DC AICA requires the buffer size AND every Lock offset/size to be 32-byte aligned
	// (else Play/Lock return DSERR_NOT32BYTEALIGNED = 0x887800ff). Round the 1-second ring down.
	g_dwBufBytes = (DWORD)(g_nRate * 4) & ~31u; // ~1 second ring, 32-byte aligned
	memset(&d, 0, sizeof(d));
	d.dwSize = sizeof(d);
	// DSBCAPS_STATIC is REQUIRED on the DC: the AICA has no software mixer, so a playable buffer
	// must live in AICA sound RAM (STATIC). The ~1-second ring (176 KB) fits AICA RAM and we
	// still stream into it (Lock/refill behind the cursor).
	d.dwFlags = DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2;
	d.dwBufferBytes = g_dwBufBytes;
	d.lpwfxFormat = &wf;
	hr = g_pDs->lpVtbl->CreateSoundBuffer(g_pDs, &d, &g_pBuf, NULL);
	Logf(L"DCWPLAY: CreateSoundBuffer hr=%08x rate=%d bytes=%u\r\n", (unsigned)hr, g_nRate,
	     g_dwBufBytes);
	if (hr != DS_OK)
		return 0;
	g_dwWritePos = 0;
	return 1;
}

static void DsSetVolume(void)
{
	long lDb = (g_nVol >= 100) ? 0 : (g_nVol <= 0) ? -10000 : (long)((g_nVol - 100) * 45);
	if (g_pBuf)
		g_pBuf->lpVtbl->SetVolume(g_pBuf, lDb);
}

// Prime the whole ring once with decoded audio so playback isn't silent for the first loop.
static void DsPrime(void)
{
	void *pv1, *pv2;
	DWORD dwB1, dwB2;
	HRESULT hr;
	int nGot;
	if (!g_pBuf)
		return;
	hr = g_pBuf->lpVtbl->Lock(g_pBuf, 0, g_dwBufBytes, &pv1, &dwB1, &pv2, &dwB2,
	                          DSBLOCK_ENTIREBUFFER);
	if (hr != DS_OK)
	{
		Logf(L"DCWPLAY: prime Lock hr=%08x\r\n", (unsigned)hr);
		return;
	}
	nGot = Decode((short *)pv1, (int)(dwB1 / 4));
	if (nGot > 0)
		g_dwPlayed += (DWORD)nGot;
	if ((DWORD)nGot * 4 < dwB1)
		memset((BYTE *)pv1 + nGot * 4, 0, dwB1 - (DWORD)nGot * 4);
	if (pv2 && dwB2)
		memset(pv2, 0, dwB2);
	g_pBuf->lpVtbl->Unlock(g_pBuf, pv1, dwB1, pv2, dwB2);
	g_dwWritePos = 0;
	Logf(L"DCWPLAY: primed %d frames\r\n", nGot);
}

// Fill the part of the ring the card has already played with freshly decoded audio.
static void DsPump(void)
{
	DWORD dwPlay, dwWrite, dwFree;
	HRESULT hr;
	void *pv1, *pv2;
	DWORD dwB1, dwB2;
	static int s_nLogged;
	if (!g_pBuf)
		return;
	hr = g_pBuf->lpVtbl->GetCurrentPosition(g_pBuf, &dwPlay, &dwWrite);
	if (hr != DS_OK)
	{
		if (s_nLogged < 3)
		{
			Logf(L"DCWPLAY: GetCurrentPosition hr=%08x\r\n", (unsigned)hr);
			s_nLogged++;
		}
		return;
	}
	dwFree =
	    (dwPlay >= g_dwWritePos) ? (dwPlay - g_dwWritePos) : (g_dwBufBytes - g_dwWritePos + dwPlay);
	if (s_nLogged < 6)
	{
		Logf(L"DCWPLAY: pump play=%u write=%u wp=%u freeb=%u\r\n", dwPlay, dwWrite, g_dwWritePos,
		     dwFree);
		s_nLogged++;
	}
	if (dwFree < 64)
		return;
	dwFree &= ~31u; // 32-byte align (AICA: DSERR_NOT32BYTEALIGNED)
	if (g_pBuf->lpVtbl->Lock(g_pBuf, g_dwWritePos, dwFree, &pv1, &dwB1, &pv2, &dwB2, 0) != DS_OK)
		return;
	{
		short *psSeg;
		DWORD dwSegB, dwDone;
		int nPart;
		for (nPart = 0; nPart < 2; nPart++)
		{
			psSeg = (short *)(nPart ? pv2 : pv1);
			dwSegB = nPart ? dwB2 : dwB1;
			if (!psSeg || !dwSegB)
				continue;
			dwDone = 0;
			while (dwDone < dwSegB)
			{
				int nWant = (int)((dwSegB - dwDone) / 4), nGot;
				if (!g_nPlaying)
					nGot = 0;
				else
					nGot = Decode(psSeg + dwDone / 2, nWant);
				if (nGot <= 0)
				{
					memset((BYTE *)psSeg + dwDone, 0, dwSegB - dwDone);
					if (g_nPlaying && !g_nEof)
					{
						g_nEof = 1;
						LOG(L"EOF");
					}
					break;
				}
				g_dwPlayed += (DWORD)nGot;
				dwDone += (DWORD)nGot * 4;
			}
		}
	}
	g_pBuf->lpVtbl->Unlock(g_pBuf, pv1, dwB1, pv2, dwB2);
	g_dwWritePos = (g_dwWritePos + dwFree) % g_dwBufBytes;
}

// ---- file load ----------------------------------------------------------------------
static DWORD rd32(const BYTE *pb)
{
	return pb[0] | (pb[1] << 8) | (pb[2] << 16) | ((DWORD)pb[3] << 24);
}

static int LoadFile(const WCHAR *pszPath)
{
	BYTE abHdr[512];
	DWORD dwGot = 0;
	const WCHAR *pszBase;
	if (g_hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(g_hFile);
		g_hFile = INVALID_HANDLE_VALUE;
	}
	g_hFile = CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (g_hFile == INVALID_HANDLE_VALUE)
	{
		Logf(L"DCWPLAY: open FAILED err=%u\r\n", GetLastError());
		return 0;
	}
	g_dwSize = GetFileSize(g_hFile, 0);
	ReadFile(g_hFile, abHdr, sizeof(abHdr), &dwGot, 0); // peek the header (then we re-seek)
	Logf(L"DCWPLAY: opened size=%u hdr=%u\r\n", g_dwSize, dwGot);

	pszBase = pszPath;
	{
		const WCHAR *pszQ;
		for (pszQ = pszPath; *pszQ; pszQ++)
			if (*pszQ == L'\\' || *pszQ == L'/')
				pszBase = pszQ + 1;
	}
	{
		int i;
		for (i = 0; i < 79 && pszBase[i]; i++)
			g_aszTrack[i] = pszBase[i];
		g_aszTrack[i] = 0;
	}

	if (dwGot > 12 && abHdr[0] == 'R' && abHdr[1] == 'I' && abHdr[2] == 'F' && abHdr[3] == 'F')
	{ // WAV
		DWORD dwO = 12;
		int nWavRate = OUT_RATE, nBps;
		DWORD dwSrcFrames, dwSecs;
		g_nIsMp3 = 0;
		g_dwWavOff = 0;
		g_dwWavEnd = 0;
		while (dwO + 8 <= dwGot)
		{
			DWORD dwCsz = rd32(abHdr + dwO + 4);
			if (!memcmp(abHdr + dwO, "fmt ", 4))
			{
				g_nWavCh = abHdr[dwO + 10] | (abHdr[dwO + 11] << 8);
				nWavRate = (int)rd32(abHdr + dwO + 12);
				g_nWavBits = abHdr[dwO + 22] | (abHdr[dwO + 23] << 8);
			}
			else if (!memcmp(abHdr + dwO, "data", 4))
			{
				g_dwWavOff = dwO + 8;
				g_dwWavEnd = dwO + 8 + dwCsz;
				if (g_dwWavEnd > g_dwSize)
					g_dwWavEnd = g_dwSize;
				break;
			}
			dwO += 8 + ((dwCsz + 1) & ~1u);
		}
		if (!g_dwWavOff)
		{
			LOG(L"WAV: no data chunk in first 512B");
			return 0;
		}
		SetSrcRate(nWavRate); // output stays at OUT_RATE; resample the source
		nBps = (g_nWavBits / 8) * (g_nWavCh ? g_nWavCh : 1);
		dwSrcFrames =
		    (g_dwWavEnd - g_dwWavOff) / (DWORD)nBps; // length in OUTPUT frames (resampled)
		dwSecs = nWavRate ? dwSrcFrames / (DWORD)nWavRate : 0;
		g_dwTotalFrames = dwSecs * (DWORD)OUT_RATE;
		Logf(L"DCWPLAY: WAV src=%dHz ch=%d bits=%d -> out %dHz data=%u..%u\r\n", nWavRate, g_nWavCh,
		     g_nWavBits, OUT_RATE, g_dwWavOff, g_dwWavEnd);
	}
	else
	{ // assume MP3
		g_nIsMp3 = 1;
		g_nCh = 2;
		g_dwTotalFrames = 0;
		SetSrcRate(OUT_RATE);
	}
	DecoderReset();
	if (g_nIsMp3)
	{
		ProbeMp3Rate();
		DecoderReset();
	} // learn the real rate, then rewind to frame 0
	if (!DsOpenBuffer())
		return 0;
	DsSetVolume();
	DsPrime(); // fill the ring before Play -> no silent gap
	return 1;
}

// ---- transport ----------------------------------------------------------------------
static void Play(void)
{
	if (g_hFile == INVALID_HANDLE_VALUE)
		return;
	if (!g_nPlaying)
	{
		g_nPlaying = 1;
		if (g_pBuf)
		{
			HRESULT hr = g_pBuf->lpVtbl->Play(g_pBuf, 0, 0, DSBPLAY_LOOPING);
			Logf(L"DCWPLAY: Play hr=%08x\r\n", (unsigned)hr);
		}
	}
}
static void Pause(void)
{
	g_nPlaying = 0;
	LOG(L"Pause");
}
static void Stop(void)
{
	g_nPlaying = 0;
	LOG(L"Stop");
	if (g_pBuf)
		g_pBuf->lpVtbl->Stop(g_pBuf);
	DecoderReset();
	g_dwWritePos = 0;
	if (g_pBuf)
		g_pBuf->lpVtbl->SetCurrentPosition(g_pBuf, 0);
}

static void SeekFrac(float fFrac)
{
	if (g_hFile == INVALID_HANDLE_VALUE)
		return;
	if (fFrac < 0)
		fFrac = 0;
	if (fFrac > 1)
		fFrac = 1;
	if (g_nIsMp3)
	{
		g_dwStreamPos = (DWORD)(fFrac * g_dwSize) & ~1u; // approx (CBR-ish)
		SetFilePointer(g_hFile, (LONG)g_dwStreamPos, 0, FILE_BEGIN);
		mp3dec_init(&g_mp3);
		g_nSN = g_nSP = 0;
	}
	else
	{
		DWORD dwBps = (g_nWavBits / 8) * (g_nWavCh ? g_nWavCh : 1);
		g_dwStreamPos = g_dwWavOff + (DWORD)(fFrac * (g_dwWavEnd - g_dwWavOff)) / dwBps * dwBps;
		SetFilePointer(g_hFile, (LONG)g_dwStreamPos, 0, FILE_BEGIN);
		g_nSN = g_nSP = 0;
	}
	g_dwPlayed = (DWORD)(fFrac * (g_dwTotalFrames ? g_dwTotalFrames : 1));
	g_nEof = 0;
}

// ---- UI -----------------------------------------------------------------------------
#define FFTN 128
static float COST[FFTN], SINT[FFTN];
static int g_nTwInit;
static float cosa(float x)
{
	float x2;
	while (x > 3.14159265f)
		x -= 6.28318531f;
	while (x < -3.14159265f)
		x += 6.28318531f;
	x2 = x * x;
	return 1.f - x2 * (0.5f - x2 * (1.f / 24 - x2 * (1.f / 720 - x2 * (1.f / 40320))));
}
static void Bars(int *pnBars, int nBars)
{
	static float re[FFTN], im[FFTN];
	int i, j, k, b, nW = g_nVizW;
	if (!g_nTwInit)
	{
		for (i = 0; i < FFTN; i++)
		{
			COST[i] = cosa(6.28318531f * i / FFTN);
			SINT[i] = cosa(6.28318531f * i / FFTN - 1.57079633f);
		}
		g_nTwInit = 1;
	}
	for (i = 0; i < FFTN; i++)
	{
		short s = g_asViz[(nW - FFTN + i) & (VIZN - 1)];
		float fWin = 0.5f - 0.5f * COST[i]; // Hann
		re[i] = (float)s * fWin * (1.f / 32768);
		im[i] = 0;
	}
	for (i = 1, j = 0; i < FFTN; i++)
	{
		int nBit = FFTN >> 1;
		for (; j & nBit; nBit >>= 1)
			j ^= nBit;
		j ^= nBit;
		if (i < j)
		{
			float t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (k = 1; k < FFTN; k <<= 1)
	{
		int nStep = FFTN / (k << 1);
		for (j = 0; j < FFTN; j += k << 1)
			for (i = 0; i < k; i++)
			{
				int nIdx = (i * nStep) & (FFTN - 1);
				float cr = COST[nIdx], ci = -SINT[nIdx];
				float ar = re[j + i + k], ai = im[j + i + k];
				float ur = ar * cr - ai * ci, ui = ar * ci + ai * cr;
				re[j + i + k] = re[j + i] - ur;
				im[j + i + k] = im[j + i] - ui;
				re[j + i] += ur;
				im[j + i] += ui;
			}
	}
	for (b = 0; b < nBars; b++)
	{
		int nLo = 1 + b * (FFTN / 2 - 1) / nBars, nHi = 1 + (b + 1) * (FFTN / 2 - 1) / nBars;
		float fMag = 0;
		for (i = nLo; i < nHi; i++)
		{
			float m = (re[i] < 0 ? -re[i] : re[i]) + (im[i] < 0 ? -im[i] : im[i]);
			if (m > fMag)
				fMag = m;
		}
		{
			int v = (int)(fMag * 300.f);
			if (v > 100)
				v = 100;
			pnBars[b] = v;
		}
	}
}

static void FmtTime(WCHAR *pszOut, DWORD dwFrames)
{
	DWORD dwS = g_nRate ? dwFrames / g_nRate : 0;
	wsprintfW(pszOut, L"%u:%02u", dwS / 60, dwS % 60);
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *pWin;
	DWORD dwKey, dwLast = 0;
	int nPbtnWas = 0;
	static int anBars[14], anPeak[14];

	LOG(L"WinMain enter");
	pWin = DCWinOpen(70, 70, PW, PH, L"Media Player", ICON_APP);
	if (!pWin)
		return 1;
	if (DsInit() && lpCmd && lpCmd[0])
	{
		WCHAR aszP[260];
		int i, j = 0; // strip surrounding quotes Explorer may add
		for (i = 0; lpCmd[i] && j < 259; i++)
			if (lpCmd[i] != L'"')
				aszP[j++] = lpCmd[i];
		aszP[j] = 0;
		Logf(L"DCWPLAY: cmdline '%s'\r\n", aszP);
		if (LoadFile(aszP))
			Play();
	}
	else
		LOG(L"no file / DsInit failed");

	for (;;)
	{
		int nCw = PW, nCh = PH, nPx, nPy, nPbtn = 0, nOver, nChanged = 0;
		int i, x, y, nBw;

		DsPump();
		DCWinClientSize(pWin, &nCw, &nCh);
		nOver = DCWinGetPointer(pWin, &nPx, &nPy, &nPbtn);

		while (DCWinPollKey(pWin, &dwKey))
		{
			nChanged = 1;
			if (dwKey == L' ')
			{
				if (g_nPlaying)
					Pause();
				else
					Play();
			}
			else if (dwKey == 'S')
				Stop();
			else if (dwKey == VK_LEFT)
				SeekFrac((float)g_dwPlayed / (g_dwTotalFrames ? g_dwTotalFrames : 1) - 0.05f);
			else if (dwKey == VK_RIGHT)
				SeekFrac((float)g_dwPlayed / (g_dwTotalFrames ? g_dwTotalFrames : 1) + 0.05f);
			else if (dwKey == VK_UP)
			{
				if (g_nVol < 100)
					g_nVol += 5;
				DsSetVolume();
			}
			else if (dwKey == VK_DOWN)
			{
				if (g_nVol > 0)
					g_nVol -= 5;
				DsSetVolume();
			}
		}

		// clickable controls: transport row buttons, seek bar, volume bar
		nBw = (nCw - 16) / 5;
		if (nOver && nPbtn && !nPbtnWas) // click edge
		{
			int nTy = nCh - 26;
			if (nPy >= nTy && nPy < nTy + 18) // transport buttons row
			{
				int b = (nPx - 8) / nBw;
				if (b == 0)
					SeekFrac(0);
				else if (b == 1)
					Play();
				else if (b == 2)
					Pause();
				else if (b == 3)
					Stop();
				else if (b == 4)
					SeekFrac(1);
			}
			else if (nPy >= 70 && nPy < 80) // seek bar
				SeekFrac((float)(nPx - 8) / (nCw - 16));
			nChanged = 1;
		}
		if (nOver && nPbtn && nPy >= nCh - 46 && nPy < nCh - 36 &&
		    nPx > nCw / 2) // volume bar (right half)
		{
			int v = (nPx - nCw / 2 - 4) * 100 / (nCw / 2 - 30);
			if (v < 0)
				v = 0;
			if (v > 100)
				v = 100;
			g_nVol = v;
			DsSetVolume();
			nChanged = 1;
		}
		nPbtnWas = nOver ? nPbtn : 0;

		if (GetTickCount() - dwLast > 33)
		{
			dwLast = GetTickCount();
			nChanged = 1;
		} // ~30fps for the viz
		if (DCWinResized(pWin))
			nChanged = 1;

		if (nChanged)
		{
			WCHAR aszT[40], aszTt[16], aszTd[16];
			DCWinBeginFrame(pWin);
			DCWinFillBg(pWin, C_BG);
			DCWinFill(pWin, 6, 6, nCw - 90, 26, C_PANEL);
			DCWinText(pWin, 10, 9, C_LCD, C_PANEL, g_aszTrack);
			DCWinText(pWin, 10, 20, C_LCDDIM, C_PANEL, g_nIsMp3 ? L"MP3" : L"WAV");
			DCWinFill(pWin, nCw - 80, 6, 74, 26, C_PANEL);
			FmtTime(aszTt, g_dwPlayed);
			FmtTime(aszTd, g_dwTotalFrames);
			DCWinText(pWin, nCw - 74, 9, C_LCDHI, C_PANEL, aszTt);
			wsprintfW(aszT, L"/ %s  %s", aszTd, g_nPlaying ? L"|>" : L"||");
			DCWinText(pWin, nCw - 74, 20, C_LCDDIM, C_PANEL, aszT);
			DCWinFill(pWin, 6, 36, nCw - 12, 28, C_PANEL);
			Bars(anBars, 14);
			nBw = (nCw - 16) / 14;
			for (i = 0; i < 14; i++)
			{
				int h = anBars[i] * 24 / 100;
				if (h < 1)
					h = 1;
				if (anBars[i] > anPeak[i])
					anPeak[i] = anBars[i];
				else if (anPeak[i] > 0)
					anPeak[i]--;
				x = 8 + i * nBw;
				DCWinFill(pWin, x, 60 - h, nBw - 2, h, (h > 18) ? C_LCDHI : C_LCD);
				DCWinFill(pWin, x, 60 - anPeak[i] * 24 / 100 - 1, nBw - 2, 1, C_LCDHI);
			}
			DCWinFill(pWin, 8, 70, nCw - 16, 10, C_PANEL);
			{
				int nSw =
				    (g_nIsMp3 ? (int)((float)g_dwStreamPos / (g_dwSize ? g_dwSize : 1) * (nCw - 18))
				              : (g_dwTotalFrames
				                     ? (int)((float)g_dwPlayed / g_dwTotalFrames * (nCw - 18))
				                     : 0));
				DCWinFill(pWin, 9, 71, nSw, 8, C_LCD);
			}
			y = nCh - 26;
			nBw = (nCw - 16) / 5;
			{
				const WCHAR *apszLbl[5] = {L"|<", L">", L"||", L"[]", L">|"};
				for (i = 0; i < 5; i++)
				{
					x = 8 + i * nBw;
					DCWinFill(pWin, x, y, nBw - 3, 18, C_BTN);
					DCWinText(pWin, x + nBw / 2 - 6, y + 4, C_LCD, C_BTN, apszLbl[i]);
				}
			}
			DCWinText(pWin, nCw / 2 - 24, nCh - 44, C_LCDDIM, C_BG, L"VOL");
			DCWinFill(pWin, nCw / 2, nCh - 46, nCw / 2 - 26, 10, C_PANEL);
			DCWinFill(pWin, nCw / 2 + 1, nCh - 45, (nCw / 2 - 28) * g_nVol / 100, 8, C_LCD);
			DCWinText(pWin, nCw - 24, nCh - 44, C_LCDDIM, C_BG, L"O");
			DCWinEndFrame(pWin);
		}
		if (DCWinShouldClose(pWin))
			break;
		Sleep(10);
	}

	Stop();
	DsClose();
	if (g_pDs)
		g_pDs->lpVtbl->Release(g_pDs);
	if (g_hFile != INVALID_HANDLE_VALUE)
		CloseHandle(g_hFile);
	DCWinClose(pWin);
	return 0;
}
