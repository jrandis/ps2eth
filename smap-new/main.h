#include "common.h"

#define PRE_LWIP_130_COMPAT	1

/* #define SaveGP() \
	void *_ori_gp;	\
	__asm volatile("move %0, $gp\n"	\
	"move $gp, %1" :"=r"(_ori_gp): "r"(_gp))

#define RestoreGP() \
	__asm volatile("move $gp, %0" :: "r"(_ori_gp)) */

#define SaveGP()
#define RestoreGP()

struct SmapDriverData{
	volatile u8 *smap_regbase;
	volatile u8 *emac3_regbase;
	unsigned int TxBufferSpaceAvailable;
	unsigned char NumPacketsInTx;
	unsigned char TxBDIndex;
	unsigned char RxBDIndex;
	unsigned char TxDNVBDIndex;
	int Dev9IntrEventFlag;
	int TxEndEventFlag;
	int IntrHandlerThreadID;
	int TxHandlerThreadID;
	unsigned char SmapIsInitialized;
	unsigned char NetDevStopFlag;
	unsigned char EnableLinkCheckTimer;
	unsigned char LinkStatus;
	iop_sys_clock_t LinkCheckTimer;
};

/* Function prototypes */
int smap_init(void);
int SMAPStart(void);
void SMAPStop(void);
int SMAPGetMACAddress(unsigned char *buffer);

void SMapLowLevelInput(struct pbuf* pBuf);

#include "xfer.h"
