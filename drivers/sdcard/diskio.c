//
// diskio.c - FatFs low-level disk glue: maps ChaN FatFs's disk_* contract onto the SD
// block driver (single physical drive 0 = the SD card). Also provides get_fattime().
//
#include <windows.h>
#include "fatfs/diskio.h"
#include "sdblk.h"

DSTATUS disk_initialize(BYTE bPdrv)
{
	(void)bPdrv;
	return SdInit() ? STA_NOINIT : 0;
}

DSTATUS disk_status(BYTE bPdrv)
{
	(void)bPdrv;
	return 0;
}

DRESULT disk_read(BYTE bPdrv, BYTE *pbBuff, DWORD dwSector, DWORD dwCount)
{
	(void)bPdrv;
	return SdReadSectors(dwSector, (int)dwCount, pbBuff) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE bPdrv, const BYTE *pbBuff, DWORD dwSector, DWORD dwCount)
{
	(void)bPdrv;
	return SdWriteSectors(dwSector, (int)dwCount, pbBuff) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE bPdrv, BYTE bCmd, void *pvBuff)
{
	(void)bPdrv;
	switch (bCmd)
	{
		case CTRL_SYNC:
			return RES_OK;
		case GET_SECTOR_COUNT:
			*(DWORD *)pvBuff = SdSectorCount();
			return RES_OK;
		case GET_SECTOR_SIZE:
			*(WORD *)pvBuff = 512;
			return RES_OK;
		case GET_BLOCK_SIZE:
			*(DWORD *)pvBuff = 1;
			return RES_OK;
	}
	return RES_PARERR;
}

// Packed DOS date/time: bits 31..25 year-1980, 24..21 month, 20..16 day,
// 15..11 hour, 10..5 minute, 4..0 second/2.
DWORD get_fattime(void)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	return ((DWORD)(t.wYear - 1980) << 25) | ((DWORD)t.wMonth << 21) | ((DWORD)t.wDay << 16) |
	       ((DWORD)t.wHour << 11) | ((DWORD)t.wMinute << 5) | ((DWORD)t.wSecond >> 1);
}
