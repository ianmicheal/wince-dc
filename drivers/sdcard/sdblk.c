//
// sdblk.c - SD / SDHC card driver in SPI mode, over the reusable dcspi transport.
// Single-block CMD17/CMD24 I/O (simple + correct; multi-block can come later).
// Reference: the standard elm-chan / KOS SPI-mode SD bring-up sequence.
//
#include "../dcspi/dcspi.h"
#include "sdblk.h"
#include "syslog.h"
#include "dcboot.h"

#define BUS DCSPI_BUS_SCIF // SD adapter wired to the serial port (CS on RTS)
#define CSM DCSPI_CS_RTS

// SCIF bit-bang clock periods (inter-edge settle). SD spec: <=400 kHz during init, fast after.
// The W5500's fast value (~32) overclocks SD power-up on real hardware (CMD0 CRC errors,
// ACMD41 stalls), so run init slow then switch to fast for block I/O.
#define SD_INIT_SETTLE 400
#define SD_RUN_SETTLE  32

// SD commands (index, sent as 0x40|idx)
#define CMD0   0  // GO_IDLE_STATE
#define CMD8   8  // SEND_IF_COND
#define CMD9   9  // SEND_CSD
#define CMD16  16 // SET_BLOCKLEN
#define CMD17  17 // READ_SINGLE_BLOCK
#define CMD24  24 // WRITE_BLOCK
#define CMD55  55 // APP_CMD
#define CMD58  58 // READ_OCR
#define ACMD41 41 // SD_SEND_OP_COND

static int g_nInited = 0;
static int g_nByteAddr = 1; // 1 = SDSC (byte address), 0 = SDHC/SDXC (block address)
static unsigned long g_nSectors = 0;

static unsigned char Rb(void)
{
	return SpiRwByte(BUS, 0xFF);
}
static void Wb(unsigned char b)
{
	SpiRwByte(BUS, b);
}
static void Cs(int nAssert)
{
	SpiSetCS(BUS, nAssert);
} // assert=1 -> CS low

// Send a command frame [0x40|cmd][arg32][crc], return the R1 response (0xFF on timeout).
static unsigned char SdCmd(int nCmd, unsigned long dwArg)
{
	unsigned char bCrc = 0x01, bResp;
	int i;
	if (nCmd == CMD0)
		bCrc = 0x95; // valid CRC7 needed before CRC is turned off
	if (nCmd == CMD8)
		bCrc = 0x87;
	Wb((unsigned char)(0x40 | nCmd));
	Wb((unsigned char)(dwArg >> 24));
	Wb((unsigned char)(dwArg >> 16));
	Wb((unsigned char)(dwArg >> 8));
	Wb((unsigned char)(dwArg));
	Wb(bCrc);
	for (i = 0; i < 16; i++)
	{
		bResp = Rb();
		if (!(bResp & 0x80))
			return bResp;
	} // R1: top bit clears
	return 0xFF;
}

static int SdWaitReady(void) // poll until the card releases busy (returns 0xFF)
{
	int i;
	for (i = 0; i < 200000; i++)
		if (Rb() == 0xFF)
			return 0;
	return -1;
}

static void SdReadCsd(void) // CMD9 -> 16-byte CSD -> decode capacity into g_nSectors
{
	unsigned char abCsd[16];
	int i;
	if (SdCmd(CMD9, 0) != 0)
		return;
	for (i = 0; i < 100000; i++)
		if (Rb() == 0xFE)
			break; // data start token
	if (i == 100000)
		return;
	for (i = 0; i < 16; i++)
		abCsd[i] = Rb();
	Rb();
	Rb();                     // CSD CRC16
	if ((abCsd[0] >> 6) == 1) // CSD v2 (SDHC/SDXC)
	{
		unsigned long dwCsize =
		    ((unsigned long)(abCsd[7] & 0x3F) << 16) | ((unsigned long)abCsd[8] << 8) | abCsd[9];
		g_nSectors = (dwCsize + 1) * 1024; // (C_SIZE+1) * 512KB / 512
	}
	else // CSD v1 (SDSC)
	{
		unsigned long dwCsize = ((unsigned long)(abCsd[6] & 0x03) << 10) |
		                        ((unsigned long)abCsd[7] << 2) | (abCsd[8] >> 6);
		int nCsmult = ((abCsd[9] & 0x03) << 1) | (abCsd[10] >> 7);
		int nRdbllen = abCsd[5] & 0x0F;
		unsigned long dwBlocks = (dwCsize + 1) * (1UL << (nCsmult + 2));
		unsigned long dwBlocklen = 1UL << nRdbllen;
		g_nSectors = dwBlocks * (dwBlocklen / 512);
	}
}

int SdInit(void)
{
	unsigned char bResp = 0xFF, abOcr[4];
	int i, nV2 = 0;

	g_nInited = 0;
	g_nByteAddr = 1;
	g_nSectors = 0;
	SysLog(L"sd: SdInit enter");
	if (SpiInit(BUS, CSM))
	{
		SysLog(L"sd: SpiInit FAILED");
		return -1;
	}
	SysLog(L"sd: SpiInit ok (SCIF)");
	SpiSetSettle(SD_INIT_SETTLE); // <=400 kHz for the bring-up sequence

	Cs(0); // CS high during the power-up clocks
	for (i = 0; i < 10; i++)
		Wb(0xFF); // >= 74 clocks
	Cs(1);

	bResp = SdCmd(CMD0, 0);
	SysLog(L"sd: CMD0 -> %02x", bResp);
	if (bResp != 0x01)
	{
		Cs(0);
		return -2;
	} // enter idle/SPI mode

	{
		unsigned char bResp8 = SdCmd(CMD8, 0x000001AA); // v2 card check
		if (bResp8 == 0x01)
		{
			for (i = 0; i < 4; i++)
				abOcr[i] = Rb(); // R7 trailer
			SysLog(L"sd: CMD8 r=01 echo=%02x%02x", abOcr[2], abOcr[3]);
			if (abOcr[2] != 0x01 || abOcr[3] != 0xAA)
			{
				Cs(0);
				return -3;
			}
			nV2 = 1;
		}
		else
			SysLog(L"sd: CMD8 r=%02x (v1/no-CMD8)", bResp8);
	}

	for (i = 0; i < 20000; i++) // ACMD41 init loop
	{
		SdCmd(CMD55, 0);
		bResp = SdCmd(ACMD41, nV2 ? 0x40000000UL : 0); // HCS bit for v2
		if (bResp == 0x00)
			break;
	}
	SysLog(L"sd: ACMD41 r=%02x iters=%d v2=%d", bResp, i, nV2);
	if (bResp != 0x00)
	{
		Cs(0);
		return -4;
	}

	if (nV2 && SdCmd(CMD58, 0) == 0x00) // read OCR -> CCS (bit30)
	{
		for (i = 0; i < 4; i++)
			abOcr[i] = Rb();
		g_nByteAddr = (abOcr[0] & 0x40) ? 0 : 1; // CCS=1 -> block addressing
	}
	SysLog(L"sd: CMD58 byteAddr=%d (1=SDSC 0=SDHC)", g_nByteAddr);
	if (g_nByteAddr)
		SdCmd(CMD16, 512); // fix block length for SDSC

	SdReadCsd();

	Cs(0);
	Rb();
	SpiSetSettle(SD_RUN_SETTLE); // card is up: switch to fast clock for block I/O
	g_nInited = 1;
	SysLog(L"sd: init OK sectors=%u byteAddr=%d", g_nSectors, g_nByteAddr);
	{ // publish capacity to the boot screen (dcwboot reads DCBOOT)
		unsigned long dwMb = g_nSectors / 2048; // 512-byte sectors -> MiB
		WCHAR szCap[DCB_RESLEN];
		if (dwMb >= 1024)
			wsprintfW(szCap, L"%u.%u GB", dwMb / 1024, ((dwMb % 1024) * 10) / 1024);
		else
			wsprintfW(szCap, L"%u MB", dwMb);
		DcBootSet(DCB_STORE, DCB_OK, szCap);
	}
	return 0;
}

int SdReadSectors(unsigned long dwLba, int nCount, void *pvBuf)
{
	unsigned char *pb = (unsigned char *)pvBuf;
	unsigned long dwAddr;
	int i;

	if (!g_nInited)
	{
		SysLog(L"sd: read lba=%u but !inited", dwLba);
		return -1;
	}
	dwAddr = g_nByteAddr ? dwLba * 512 : dwLba;
	SysLog(L"sd: read lba=%u cnt=%d addr=%u", dwLba, nCount, dwAddr);
	Cs(1);
	while (nCount-- > 0)
	{
		unsigned char bTok = 0xFF, bR1;
		bR1 = SdCmd(CMD17, dwAddr);
		if (bR1 != 0)
		{
			SysLog(L"sd: CMD17 lba=%u R1=%02x", dwLba, bR1);
			Cs(0);
			return -1;
		}
		for (i = 0; i < 100000; i++)
		{
			bTok = Rb();
			if (bTok != 0xFF)
				break;
		}
		if (bTok != 0xFE)
		{
			SysLog(L"sd: CMD17 lba=%u tok=%02x", dwLba, bTok);
			Cs(0);
			return -1;
		}
		SpiRwData(BUS, 0, pb, 512); // read 512 (tx NULL -> 0xFF)
		Rb();
		Rb(); // data CRC16
		pb += 512;
		dwAddr += g_nByteAddr ? 512 : 1;
	}
	Cs(0);
	Rb();
	SysLog(L"sd: read ok lba=%u [%02x %02x %02x %02x]", dwLba, ((BYTE *)pvBuf)[0],
	       ((BYTE *)pvBuf)[1], ((BYTE *)pvBuf)[2], ((BYTE *)pvBuf)[510]);
	return 0;
}

int SdWriteSectors(unsigned long dwLba, int nCount, const void *pvBuf)
{
	const unsigned char *pb = (const unsigned char *)pvBuf;
	unsigned long dwAddr;

	if (!g_nInited)
		return -1;
	dwAddr = g_nByteAddr ? dwLba * 512 : dwLba;
	Cs(1);
	while (nCount-- > 0)
	{
		if (SdCmd(CMD24, dwAddr) != 0)
		{
			Cs(0);
			return -1;
		}
		Wb(0xFF);                   // 1-byte gap before token
		Wb(0xFE);                   // data start token
		SpiRwData(BUS, pb, 0, 512); // write 512
		Wb(0xFF);
		Wb(0xFF); // dummy CRC16
		if ((Rb() & 0x1F) != 0x05)
		{
			Cs(0);
			return -1;
		} // data response: 010 = accepted
		if (SdWaitReady())
		{
			Cs(0);
			return -1;
		} // wait out the program-busy
		pb += 512;
		dwAddr += g_nByteAddr ? 512 : 1;
	}
	Cs(0);
	Rb();
	return 0;
}

unsigned long SdSectorCount(void)
{
	return g_nSectors;
}
