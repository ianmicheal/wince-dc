//
// dcboot.c - see dcboot.h. Lazily maps the DCBOOT shared section; subsystems call DcBootSet()
// to publish a stage result, dcwboot reads state[]/result[]. Self-contained (just coredll).
//
#include "dcboot.h"

static DcBootShared *g_pDb;
static HANDLE g_hMap;

DcBootShared *DcBootMap(int create)
{
	if (g_pDb)
		return g_pDb;
	g_hMap = CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(DcBootShared),
	                            DCBOOT_SECTION);
	if (!g_hMap)
		return NULL;
	g_pDb = (DcBootShared *)MapViewOfFile(g_hMap, FILE_MAP_WRITE, 0, 0, sizeof(DcBootShared));
	if (g_pDb && create && g_pDb->magic != DCBOOT_MAGIC)
		g_pDb->magic = DCBOOT_MAGIC;
	return g_pDb;
}

void DcBootSet(int stage, int state, const WCHAR *result)
{
	DcBootShared *pDb = DcBootMap(1);
	int i;
	if (!pDb || stage < 0 || stage >= DCB_STAGES)
		return;
	if (result)
	{
		WCHAR *pszRes = pDb->result[stage];
		for (i = 0; i < DCB_RESLEN - 1 && result[i]; i++)
			pszRes[i] = result[i];
		pszRes[i] = 0;
	}
	pDb->state[stage] = state; // publish state last
}
