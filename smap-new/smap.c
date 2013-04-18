#include <errno.h>
#include <stdio.h>
#include <dev9.h>
#include <intrman.h>
#include <loadcore.h>
#include <modload.h>
#include <stdio.h>
#include <sysclib.h>
#include <thbase.h>
#include <thevent.h>
#include <thsemap.h>
#include <irx.h>

#include <smapregs.h>
#include <speedregs.h>

#include <ps2ip.h>

#include "main.h"
#include "xfer.h"

#define DEV9_SMAP_ALL_INTR_MASK	(SMAP_INTR_EMAC3|SMAP_INTR_RXEND|SMAP_INTR_TXEND|SMAP_INTR_RXDNV|SMAP_INTR_TXDNV)
#define DEV9_SMAP_INTR_MASK	(SMAP_INTR_RXEND|SMAP_INTR_TXEND)

/* Event flag bits */
#define SMAP_EVENT_START	0x01
#define SMAP_EVENT_STOP		0x02
#define SMAP_EVENT_INTR		0x04
#define SMAP_EVENT_XMIT		0x08
#define SMAP_EVENT_LINK_CHECK	0x10

struct SmapDriverData SmapDriverData;

const char VersionString[]="Version 2.25.0";
static unsigned int ThreadPriority=0x28;
static unsigned int ThreadStackSize=0x1000;
static unsigned int EnableVerboseOutput=0;
static unsigned int EnableAutoNegotiation=1;
static unsigned int EnablePinStrapConfig=0;
static unsigned int SmapConfiguration=0x5E0;

extern void *_gp;

static void _smap_write_phy(volatile u8 *emac3_regbase, unsigned char address, unsigned short int value){
	unsigned int i, PHYRegisterValue;

	PHYRegisterValue=(address&SMAP_E3_PHY_REG_ADDR_MSK)|SMAP_E3_PHY_WRITE|((SMAP_DsPHYTER_ADDRESS&SMAP_E3_PHY_ADDR_MSK)<<SMAP_E3_PHY_ADDR_BITSFT);
	PHYRegisterValue|=((unsigned int)value)<<SMAP_E3_PHY_DATA_BITSFT;

	i=0;
	SMAP_EMAC3_SET(SMAP_R_EMAC3_STA_CTRL, PHYRegisterValue);

	for(; !(SMAP_EMAC3_GET(SMAP_R_EMAC3_STA_CTRL)&SMAP_E3_PHY_OP_COMP); i++){
		DelayThread(1000);
		if(i>=100) break;
	}

	if(i>=100) printf("smap: %s: > %d ms\n", "_smap_write_phy", i);
}

static int _smap_read_phy(volatile u8 *emac3_regbase, unsigned int address){
	unsigned int i, PHYRegisterValue;
	int result;

	PHYRegisterValue=(address&SMAP_E3_PHY_REG_ADDR_MSK)|SMAP_E3_PHY_READ|((SMAP_DsPHYTER_ADDRESS&SMAP_E3_PHY_ADDR_MSK)<<SMAP_E3_PHY_ADDR_BITSFT);

	i=0;
	result=0;
	SMAP_EMAC3_SET(SMAP_R_EMAC3_STA_CTRL, PHYRegisterValue);

	do{
		if(SMAP_EMAC3_GET(SMAP_R_EMAC3_STA_CTRL)&SMAP_E3_PHY_OP_COMP){
			if(SMAP_EMAC3_GET(SMAP_R_EMAC3_STA_CTRL)&SMAP_E3_PHY_OP_COMP){
				if((result=SMAP_EMAC3_GET(SMAP_R_EMAC3_STA_CTRL))&SMAP_E3_PHY_OP_COMP){
					result>>=SMAP_E3_PHY_DATA_BITSFT;
					break;
				}
			}
		}

		DelayThread(1000);
		i++;
	}while(i<100);

	if(i>=100) printf("smap: %s: > %d ms\n", "_smap_read_phy", i);

	return result;
}

static inline void RestartAutoNegotiation(volatile u8 *emac3_regbase, unsigned short int bmsr){
	if(EnableVerboseOutput) DEBUG_PRINTF("smap: restarting auto nego (BMCR=0x%x, BMSR=0x%x)\n", _smap_read_phy(emac3_regbase, SMAP_DsPHYTER_BMCR), bmsr);
	_smap_write_phy(emac3_regbase, SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_ANEN|SMAP_PHY_BMCR_RSAN);
}

static int InitPHY(struct SmapDriverData *SmapDrivPrivData){
	int i, result;
	unsigned int value, value2, LinkSpeed100M, LinkFDX, FlowControlEnabled, AutoNegoRetries;
	unsigned short int RegDump[6];
	volatile u8 *emac3_regbase;

	LinkSpeed100M=0;
	if(EnableVerboseOutput!=0) DEBUG_PRINTF("smap: Resetting PHY\n");

	_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_RST);
	for(i=0; _smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR)&SMAP_PHY_BMCR_RST; i++){
		if(i<=0){
			DEBUG_PRINTF("smap: PHY reset error\n");
			return -1;
		}
		if(SmapDrivPrivData->NetDevStopFlag) return 0;

		DelayThread(1000);
	}

	if(!EnableAutoNegotiation){
		if(EnableVerboseOutput!=0) DEBUG_PRINTF("smap: no auto mode (conf=0x%x)\n", SmapConfiguration);

		LinkSpeed100M=0<(SmapConfiguration&0x180);	/* Toggles between SMAP_PHY_BMCR_10M and SMAP_PHY_BMCR_100M. */
		value=LinkSpeed100M<<13;
		if(SmapConfiguration&0x140) value|=SMAP_PHY_BMCR_DUPM;
		_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, value);

WaitLink:
		DEBUG_PRINTF("smap: Waiting Valid Link for %dMbps\n", LinkSpeed100M?100:10);

		i=0;
		while(1){
			DelayThread(200000);
			if(SmapDrivPrivData->NetDevStopFlag) return 0;
			i++;
			if(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&SMAP_PHY_BMSR_LINK) break;
			if(i>=5) SmapDrivPrivData->LinkStatus=0;
		}

		SmapDrivPrivData->LinkStatus=1;
	}
	else{
		if(!EnablePinStrapConfig){
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, 0);
			value=_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&0xFFFF;
			if(!(value&0x4000)) SmapConfiguration=SmapConfiguration&0xFFFFFEFF;	/* 100Base-TX FDX */
			if(!(value&0x2000)) SmapConfiguration=SmapConfiguration&0xFFFFFF7F;	/* 100Base-TX HDX */
			if(!(value&0x1000)) SmapConfiguration=SmapConfiguration&0xFFFFFFBF;	/* 10Base-TX FDX */
			if(!(value&0x0800)) SmapConfiguration=SmapConfiguration&0xFFFFFFDF;	/* 10Base-TX HDX */

			DEBUG_PRINTF("smap: no strap mode (conf=0x%x, bmsr=0x%x)\n", SmapConfiguration, value);

			value=_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_ANAR)&0xFFFF;
			value=(SmapConfiguration&0x5E0)|(value&0x1F);
			DEBUG_PRINTF("smap: anar=0x%x\n", value);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_ANAR, value);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_ANEN|SMAP_PHY_BMCR_RSAN);
		}
		else{
			if(!(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR)&SMAP_PHY_BMCR_ANEN)){
				goto WaitLink;
			}
		}

		DEBUG_PRINTF("smap: auto mode (BMCR=0x%x ANAR=0x%x)\n", _smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR)&0xFFFF, _smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_ANAR)&0xFFFF);

RepeatAutoNegoProcess:
		for(AutoNegoRetries=0; AutoNegoRetries<3; AutoNegoRetries++){
			for(i=0; i<3; i++){
				DelayThread(1000000);
				if(SmapDrivPrivData->NetDevStopFlag) return 0;
			}

			value=_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&0xFFFF;
			if((value&(SMAP_PHY_BMSR_ANCP|0x10))==SMAP_PHY_BMSR_ANCP){	/* 0x30: SMAP_PHY_BMSR_ANCP and Remote fault. */
				/* This seems to be checking for the link-up status. */
				for(i=0; !(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&SMAP_PHY_BMSR_LINK); i++){
					DelayThread(200000);
					if(SmapDrivPrivData->NetDevStopFlag) return 0;
					if(i>=20) break;
				}

				if(i<20){
					/* Auto negotiaton completed successfully. */
					SmapDrivPrivData->LinkStatus=1;
					break;
				}
				else RestartAutoNegotiation(SmapDrivPrivData->emac3_regbase, value);
			}
			else RestartAutoNegotiation(SmapDrivPrivData->emac3_regbase, value);
		}

		/* If automatic negotiation fails, manually figure out which speed and duplex mode to use. */
		if(AutoNegoRetries>=3){
			if(EnableVerboseOutput) DEBUG_PRINTF("smap: waiting valid link for 100Mbps Half-Duplex\n");

			_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_100M);
			DelayThread(1000000);
			if(SmapDrivPrivData->NetDevStopFlag) return 0;

			for(i=0; !(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&SMAP_PHY_BMSR_LINK); i++){
				DelayThread(100000);
				if(SmapDrivPrivData->NetDevStopFlag) return 0;
				if(i>=30) break;
			}

			if(i>=30){
				if(EnableVerboseOutput) DEBUG_PRINTF("smap: waiting valid link for 10Mbps Half-Duplex\n");

				_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, SMAP_PHY_BMCR_10M);
				DelayThread(1000000);
				if(SmapDrivPrivData->NetDevStopFlag) return 0;

				for(i=0; !(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&SMAP_PHY_BMSR_LINK); i++){
					DelayThread(100000);
					if(SmapDrivPrivData->NetDevStopFlag) return 0;
					if(i>=30) break;
				}

				if(i>=0x1E) goto RepeatAutoNegoProcess;
			}
		}
	}

	for(i=0; i<6; i++) RegDump[i]=_smap_read_phy(SmapDrivPrivData->emac3_regbase, i);

	if(EnableVerboseOutput) DEBUG_PRINTF("smap: PHY: %04x %04x %04x %04x %04x %04x\n", RegDump[SMAP_DsPHYTER_BMCR], RegDump[SMAP_DsPHYTER_BMSR], RegDump[SMAP_DsPHYTER_PHYIDR1], RegDump[SMAP_DsPHYTER_PHYIDR2], RegDump[SMAP_DsPHYTER_ANAR], RegDump[SMAP_DsPHYTER_ANLPAR]);

	/* if SMAP_DsPHYTER_PHYIDR1 == SMAP_PHY_IDR1_VAL && SMAP_DsPHYTER_PHYIDR2 == SMAP_PHY_IDR2_VAL */
	if(RegDump[SMAP_DsPHYTER_PHYIDR1]==SMAP_PHY_IDR1_VAL && (RegDump[SMAP_DsPHYTER_PHYIDR2]&SMAP_PHY_IDR2_MSK)==SMAP_PHY_IDR2_VAL){
		if(EnableAutoNegotiation){
			_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_FCSCR);
			_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_RECR);
			DelayThread(500000);
			value=_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_FCSCR);
			value2=_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_RECR);
			if((value2&0xFFFF)!=0 || (value&0xFFFF)>=0x11){
				if(EnableVerboseOutput) DEBUG_PRINTF("smap: FCSCR=%d RECR=%d\n", value&0xFFFF, value2&0xFFFF);
				_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMCR, 0);
				goto WaitLink;
			}
		}

		DEBUG_PRINTF("smap: PHY chip: DP83846A%d\n", RegDump[SMAP_DsPHYTER_PHYIDR2]&SMAP_PHY_IDR2_REV_MSK);

		if(!EnableAutoNegotiation){
			if(RegDump[SMAP_DsPHYTER_BMCR]&(SMAP_PHY_BMCR_DUPM|SMAP_PHY_BMCR_100M)){	/* if SMAP_DsPHYTER_BMCR&0x2100 */
				_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_10BTSCR, 0x104);
			}
		}
		else{
			if((RegDump[SMAP_DsPHYTER_ANAR]&0x1E0)==0x20){	/* SMAP_DsPHYTER_ANAR */
				_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_10BTSCR, 0x104);
			}
		}

		if((RegDump[SMAP_DsPHYTER_PHYIDR2]&SMAP_PHY_IDR2_REV_MSK)==0){	/* SMAP_DsPHYTER_PHYIDR2 */
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, 0x13, 1);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_PHYCTRL, 0x1819);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, 0x1F, 0);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, 0x1D, 0x5040);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, 0x1E, 0x8C);
			_smap_write_phy(SmapDrivPrivData->emac3_regbase, 0x13, 0);
		}
	}

	FlowControlEnabled=0;
	if(RegDump[SMAP_DsPHYTER_BMCR]&SMAP_PHY_BMCR_ANEN){
		value=RegDump[SMAP_DsPHYTER_ANAR]&RegDump[SMAP_DsPHYTER_ANLPAR];
		LinkSpeed100M=0<(value&0x180);
		LinkFDX=0<(value&0x140);
		if(LinkFDX) FlowControlEnabled=0<(value&0x400);
	}
	else{
		LinkSpeed100M=RegDump[SMAP_DsPHYTER_BMCR]>>13&1;
		LinkFDX=RegDump[SMAP_DsPHYTER_BMCR]>>8&1;
		FlowControlEnabled=SmapConfiguration>>10&1;
	}

	if(LinkSpeed100M) result=LinkFDX?8:4;
	else result=LinkFDX?2:1;

	DEBUG_PRINTF("smap: %s %s Duplex Mode %s Flow Control\n", LinkSpeed100M?"100BaseTX":"10BaseT", LinkFDX?"Full":"Half", FlowControlEnabled?"with":"without");

	emac3_regbase=SmapDrivPrivData->emac3_regbase;
	value=SMAP_EMAC3_GET(SMAP_R_EMAC3_MODE1)&0x67FFFFFF;
	if(LinkFDX) value|=SMAP_E3_FDX_ENABLE;
	if(FlowControlEnabled) value|=SMAP_E3_FLOWCTRL_ENABLE|SMAP_E3_ALLOW_PF;
	SMAP_EMAC3_SET(SMAP_R_EMAC3_MODE1, value);

	return 0;
}

static unsigned int LinkCheckTimerCB(struct SmapDriverData *SmapDrivPrivData){
	iSetEventFlag(SmapDrivPrivData->Dev9IntrEventFlag, SMAP_EVENT_LINK_CHECK);
	return SmapDrivPrivData->LinkCheckTimer.lo;
}

static int HandleTxIntr(struct SmapDriverData *SmapDrivPrivData){
	int result, OldState;
	volatile u8 *smap_regbase;
	USE_SMAP_TX_BD;

	smap_regbase=SmapDrivPrivData->smap_regbase;
	result=0;
	if(SmapDrivPrivData->NumPacketsInTx>0){
		do{
			if(tx_bd[SmapDrivPrivData->TxDNVBDIndex&0x3F].ctrl_stat&SMAP_BD_TX_READY) break;

			CpuSuspendIntr(&OldState);
			SmapDrivPrivData->TxBufferSpaceAvailable+=(tx_bd[SmapDrivPrivData->TxDNVBDIndex&0x3F].length+3)&~3;
			SmapDrivPrivData->NumPacketsInTx--;
			CpuResumeIntr(OldState);

			result++;
			SmapDrivPrivData->TxDNVBDIndex++;
		}while(SmapDrivPrivData->NumPacketsInTx>0);
	}

	SetEventFlag(SmapDrivPrivData->TxEndEventFlag, 1);

	return result;
}

static void CheckLinkStatus(struct SmapDriverData *SmapDrivPrivData){
	if(!(_smap_read_phy(SmapDrivPrivData->emac3_regbase, SMAP_DsPHYTER_BMSR)&SMAP_PHY_BMSR_LINK)){
		SmapDrivPrivData->LinkStatus=0;
		InitPHY(SmapDrivPrivData);
	}
}

static void IntrHandlerThread(struct SmapDriverData *SmapDrivPrivData){
	unsigned int ResetCounterFlag, IntrReg;
	u32 EFBits;
	int result, counter;
	volatile u8 *smap_regbase, *emac3_regbase;
	USE_SPD_REGS;

	counter=3;
	emac3_regbase=SmapDrivPrivData->emac3_regbase;
	smap_regbase=SmapDrivPrivData->smap_regbase;
	while(1){
		WaitEventFlag(SmapDrivPrivData->Dev9IntrEventFlag, SMAP_EVENT_START|SMAP_EVENT_STOP|SMAP_EVENT_INTR|SMAP_EVENT_XMIT|SMAP_EVENT_LINK_CHECK, WEF_OR|WEF_CLEAR, &EFBits);

		if(EFBits&SMAP_EVENT_STOP){
			if(!SmapDrivPrivData->SmapIsInitialized){
				dev9IntrDisable(DEV9_SMAP_INTR_MASK);
				SMAP_EMAC3_SET(SMAP_R_EMAC3_MODE0, 0);
				SmapDrivPrivData->NetDevStopFlag=0;
				SmapDrivPrivData->SmapIsInitialized=0;
			}
		}
		if(EFBits&SMAP_EVENT_START){
			if(!SmapDrivPrivData->SmapIsInitialized){
				dev9IntrEnable(DEV9_SMAP_INTR_MASK);
				if((result=InitPHY(SmapDrivPrivData))!=0) break;
				if(SmapDrivPrivData->NetDevStopFlag){
					SmapDrivPrivData->NetDevStopFlag=0;
					continue;
				}

				SMAP_EMAC3_SET(SMAP_R_EMAC3_MODE0, SMAP_E3_TXMAC_ENABLE|SMAP_E3_RXMAC_ENABLE);
				DelayThread(10000);
				SmapDrivPrivData->SmapIsInitialized=1;

				if(!SmapDrivPrivData->EnableLinkCheckTimer){
					USec2SysClock(1000000, &SmapDrivPrivData->LinkCheckTimer);
					SetAlarm(&SmapDrivPrivData->LinkCheckTimer, (void*)&LinkCheckTimerCB, SmapDrivPrivData);
					SmapDrivPrivData->EnableLinkCheckTimer=1;
				}
			}
		}

		if(SmapDrivPrivData->SmapIsInitialized){
			ResetCounterFlag=0;
			if(EFBits&SMAP_EVENT_INTR){
				while((IntrReg=SPD_REG16(SPD_R_INTR_STAT)&SMAP_INTR_RXEND)!=0){
					SMAP_REG16(SMAP_R_INTR_CLR)=SMAP_INTR_RXEND;
					ResetCounterFlag=HandleRxIntr(SmapDrivPrivData);
				}

				dev9IntrEnable(SMAP_INTR_RXEND);
			}

			if(ResetCounterFlag) counter=3;

			if(EFBits&SMAP_EVENT_LINK_CHECK){
				if(--counter<=0){
					CheckLinkStatus(SmapDrivPrivData);
				}
			}
		}
	}
}

static int Dev9IntrCb(int flag){
	SaveGP();

	dev9IntrDisable(SMAP_INTR_RXEND);
	iSetEventFlag(SmapDriverData.Dev9IntrEventFlag, SMAP_EVENT_INTR);

	RestoreGP();

	return 0;
}

static int Dev9TXEndIntrHandler(int flag){
	SaveGP();

	dev9IntrDisable(SMAP_INTR_TXEND);
	iWakeupThread(SmapDriverData.TxHandlerThreadID);
	RestoreGP();

	return 0;
}

/* static int Dev9TXDNVIntrHandler(int flag){
	USE_SMAP_REGS;

	SaveGP();
	SMAP_REG16(SMAP_R_INTR_CLR)=SMAP_INTR_TXDNV;
	HandleTxIntr(&SmapDriverData);
	RestoreGP();

	return 0;
} */

/* Note: The GP register does not need to be loaded and restored in these two functions as DEV9 does not use the GP register.
	But is it a good idea? Sony doesn't bother with the GP register within these two functions, but what if DEV9 gets modified to use the GP register? */
static void Dev9PreDmaCbHandler(int bcr, int dir){
	volatile u8 *smap_regbase;
	unsigned short int SliceCount;

	smap_regbase=SmapDriverData.smap_regbase;
	SliceCount=bcr>>16;
	if(dir!=0){
		SMAP_REG16(SMAP_R_TXFIFO_SIZE)=SliceCount;
		SMAP_REG8(SMAP_R_TXFIFO_CTRL)=SMAP_TXFIFO_DMAEN;
	}
	else{
		SMAP_REG16(SMAP_R_RXFIFO_SIZE)=SliceCount;
		SMAP_REG8(SMAP_R_RXFIFO_CTRL)=SMAP_RXFIFO_DMAEN;
	}
}

static void Dev9PostDmaCbHandler(int bcr, int dir){
	volatile u8 *smap_regbase;

	smap_regbase=SmapDriverData.smap_regbase;
	if(dir!=0){
		while(SMAP_REG8(SMAP_R_TXFIFO_CTRL)&SMAP_TXFIFO_DMAEN){};
	}
	else{
		while(SMAP_REG8(SMAP_R_RXFIFO_CTRL)&SMAP_RXFIFO_DMAEN){};
	}
}

int SMAPStart(void){
	SaveGP();
	SetEventFlag(SmapDriverData.Dev9IntrEventFlag, SMAP_EVENT_START);
	RestoreGP();

	return 0;
}

void SMAPStop(void){
	SaveGP();
	SetEventFlag(SmapDriverData.Dev9IntrEventFlag, SMAP_EVENT_STOP);
	SmapDriverData.NetDevStopFlag=1;
	RestoreGP();
}

static void TxHandlerThread(void *arg){
	USE_SPD_REGS;
	volatile u8 *smap_regbase;

	smap_regbase=((struct SmapDriverData *)arg)->smap_regbase;
	while(1){
		SleepThread();

		while(SPD_REG16(SPD_R_INTR_STAT)&SMAP_INTR_TXEND){
			SMAP_REG16(SMAP_R_INTR_CLR)=SMAP_INTR_TXEND;

			HandleTxIntr(arg);
		}

		dev9IntrEnable(SMAP_INTR_TXEND);
	}
}

static inline int initialize(void){
	int result;
	iop_event_t EventFlagData;
	iop_thread_t ThreadData;
	volatile u8 *emac3_regbase;

	emac3_regbase=SmapDriverData.emac3_regbase;

	EventFlagData.attr=0;
	EventFlagData.option=0;
	EventFlagData.bits=0;

	if((result=SmapDriverData.Dev9IntrEventFlag=CreateEventFlag(&EventFlagData))<0){
		DEBUG_PRINTF("smap: CreateEventFlag -> %d\n", result);
		return -6;
	}
	if((result=SmapDriverData.TxEndEventFlag=CreateEventFlag(&EventFlagData))<0){
		DEBUG_PRINTF("smap: CreateEventFlag -> %d\n", result);
		DeleteEventFlag(SmapDriverData.Dev9IntrEventFlag);
		return -6;
	}

	ThreadData.attr=TH_C;
	ThreadData.thread=(void*)&IntrHandlerThread;
	ThreadData.option=0;
	ThreadData.priority=ThreadPriority;
	ThreadData.stacksize=ThreadStackSize;
	if((result=SmapDriverData.IntrHandlerThreadID=CreateThread(&ThreadData))<0){
		DEBUG_PRINTF("smap: CreateThread -> %d\n", result);
		DeleteEventFlag(SmapDriverData.Dev9IntrEventFlag);
		return result;
	}

	ThreadData.priority=ThreadPriority;
	ThreadData.thread=(void*)&TxHandlerThread;
	if((result=SmapDriverData.TxHandlerThreadID=CreateThread(&ThreadData))<0){
		DEBUG_PRINTF("smap: CreateThread -> %d\n", result);
		DeleteEventFlag(SmapDriverData.Dev9IntrEventFlag);
		return result;
	}

	// Checks removed as calls to StartThread() shouldn't fail.
	StartThread(SmapDriverData.IntrHandlerThreadID, &SmapDriverData);
	StartThread(SmapDriverData.TxHandlerThreadID, &SmapDriverData);

	return 0;
}

int smap_init(void){
	int result, i;
	unsigned short int eeprom_data[4], checksum16;
	volatile u8 *emac3_regbase;
	unsigned int mac_address;
	USE_SPD_REGS;
	USE_SMAP_REGS;
	USE_SMAP_TX_BD;
	USE_SMAP_RX_BD;

	checksum16=0;

	SmapDriverData.smap_regbase=(volatile u8 *)SMAP_REGBASE;
	emac3_regbase=SmapDriverData.emac3_regbase=(volatile u8 *)(SMAP_REGBASE + SMAP_EMAC3_REGBASE);
	if(!SPD_REG16(SPD_R_REV_3)&SPD_CAPS_SMAP) return -1;
	if(SPD_REG16(SPD_R_REV_1)<0x11) return -6;	// Minimum: revision 17, ES2.

	dev9IntrDisable(DEV9_SMAP_ALL_INTR_MASK);

	/* Reset FIFOs. */
	SMAP_REG8(SMAP_R_TXFIFO_CTRL)=SMAP_TXFIFO_RESET;
	for(i=9; SMAP_REG8(SMAP_R_TXFIFO_CTRL)&SMAP_TXFIFO_RESET; i--){
		if(i<=0) return -2;
		DelayThread(1000);
	}

	SMAP_REG8(SMAP_R_RXFIFO_CTRL)=SMAP_RXFIFO_RESET;
	for(i=9; SMAP_REG8(SMAP_R_RXFIFO_CTRL)&SMAP_RXFIFO_RESET; i--){
		if(i<=0) return -3;
		DelayThread(1000);
	}

	SMAP_EMAC3_SET(SMAP_R_EMAC3_MODE0, SMAP_E3_SOFT_RESET);
	SMAP_EMAC3_GET(SMAP_R_EMAC3_MODE0);
	for(i=9; SMAP_EMAC3_GET(SMAP_R_EMAC3_MODE0)&SMAP_E3_SOFT_RESET; i--){
		if(i<=0) return -4;
		DelayThread(1000);
	}

	for(i=0; i<SMAP_BD_MAX_ENTRY; i++){
		tx_bd[i].ctrl_stat=0;
		tx_bd[i].reserved=0;
		tx_bd[i].length=0;
		tx_bd[i].pointer=0;
	}

	for(i=0; i<SMAP_BD_MAX_ENTRY; i++){
		rx_bd[i].ctrl_stat=SMAP_BD_RX_EMPTY;
		rx_bd[i].reserved=0;
		rx_bd[i].length=0;
		rx_bd[i].pointer=0;
	}

	SmapDriverData.TxBufferSpaceAvailable=SMAP_TX_BUFSIZE;

	SMAP_REG16(SMAP_R_INTR_CLR)=DEV9_SMAP_ALL_INTR_MASK;

	/* Retrieve the MAC address and verify it's integrity. */
	bzero(eeprom_data, 8);
	if((result=dev9GetEEPROM(eeprom_data))<0){
		return(result==-1?-7:-4);
	}

	for(i=0; i<3; i++) checksum16+=eeprom_data[i];
	if(eeprom_data[0]==0 && eeprom_data[1]==0 && eeprom_data[2]==0){
		return -5;
	}
	if(checksum16!=eeprom_data[3]) return -5;

	SMAP_EMAC3_SET(SMAP_R_EMAC3_MODE1, SMAP_E3_FDX_ENABLE|SMAP_E3_IGNORE_SQE|SMAP_E3_MEDIA_100M|SMAP_E3_RXFIFO_2K|SMAP_E3_TXFIFO_1K|SMAP_E3_TXREQ0_MULTI|SMAP_E3_TXREQ1_SINGLE);	/* SMAP_R_EMAC3_MODE1=0x80008164 */
	SMAP_EMAC3_SET(SMAP_R_EMAC3_TxMODE1, 7<<SMAP_E3_TX_LOW_REQ_BITSFT | 0xF<<SMAP_E3_TX_URG_REQ_BITSFT);	/* SMAP_R_EMAC3_TxMODE1=0x0000380F */
	SMAP_EMAC3_SET(SMAP_R_EMAC3_RxMODE, SMAP_E3_RX_STRIP_PAD|SMAP_E3_RX_STRIP_FCS|SMAP_E3_RX_INDIVID_ADDR|SMAP_E3_RX_BCAST|SMAP_E3_RX_MCAST);	/* SMAP_R_EMAC3_RxMODE=0x0000C058 */
	SMAP_EMAC3_SET(SMAP_R_EMAC3_INTR_STAT, SMAP_E3_INTR_TX_ERR_0|SMAP_E3_INTR_SQE_ERR_0|SMAP_E3_INTR_DEAD_0);	/* SMAP_R_EMAC3_INTR_STAT=0x01c00000 */
//	SMAP_EMAC3_SET(SMAP_R_EMAC3_INTR_ENABLE, SMAP_E3_INTR_TX_ERR_0|SMAP_E3_INTR_SQE_ERR_0|SMAP_E3_INTR_DEAD_0);	/* SMAP_R_EMAC3_INTR_ENABLE=0x01c00000 */
	SMAP_EMAC3_SET(SMAP_R_EMAC3_INTR_ENABLE, 0);

	mac_address=eeprom_data[0]>>8 | eeprom_data[0]<<8;
	SMAP_EMAC3_SET(SMAP_R_EMAC3_ADDR_HI, mac_address);

	mac_address=eeprom_data[1]>>8|eeprom_data[1]<<8;
	mac_address=mac_address<<16|eeprom_data[2]>>8|eeprom_data[2]<<8;
	SMAP_EMAC3_SET(SMAP_R_EMAC3_ADDR_LO, mac_address);

	SMAP_EMAC3_SET(SMAP_R_EMAC3_PAUSE_TIMER, 0xFFFF);	/* SMAP_R_EMAC3_PAUSE_TIMER=0xFFFF0000 */

	SMAP_EMAC3_SET(SMAP_R_EMAC3_GROUP_HASH1, 0);
	SMAP_EMAC3_SET(SMAP_R_EMAC3_GROUP_HASH2, 0);
	SMAP_EMAC3_SET(SMAP_R_EMAC3_GROUP_HASH3, 0);
	SMAP_EMAC3_SET(SMAP_R_EMAC3_GROUP_HASH4, 0);

	SMAP_EMAC3_SET(SMAP_R_EMAC3_INTER_FRAME_GAP, 4);
	SMAP_EMAC3_SET(SMAP_R_EMAC3_TX_THRESHOLD, 0xC<<SMAP_E3_TX_THRESHLD_BITSFT);
	SMAP_EMAC3_SET(SMAP_R_EMAC3_RX_WATERMARK, 0x8<<SMAP_E3_RX_LO_WATER_BITSFT|0x40<<SMAP_E3_RX_HI_WATER_BITSFT);

//	dev9RegisterIntrCb(2, &Dev9TXDNVIntrHandler);	/* TXDNV */
	dev9RegisterIntrCb(4, &Dev9TXEndIntrHandler);	/* TXEND */
//	dev9RegisterIntrCb(6, &Dev9IntrCb);		/* EMAC3 */
//	dev9RegisterIntrCb(3, &Dev9RXDNVIntrHandler);	/* RXDNV */
	dev9RegisterIntrCb(5, &Dev9IntrCb);		/* RXEND */

	dev9RegisterPreDmaCb(1, &Dev9PreDmaCbHandler);
	dev9RegisterPostDmaCb(1, &Dev9PostDmaCbHandler);

	return initialize();
}

int SMAPGetMACAddress(unsigned char *buffer){
	unsigned int mac_address_lo, mac_address_hi;
	volatile u8 *emac3_regbase;

	emac3_regbase=SmapDriverData.emac3_regbase;

	mac_address_hi=SMAP_EMAC3_GET(SMAP_R_EMAC3_ADDR_HI);
	mac_address_lo=SMAP_EMAC3_GET(SMAP_R_EMAC3_ADDR_LO);
	buffer[0]=mac_address_hi>>8;
	buffer[1]=mac_address_hi;
	buffer[2]=mac_address_lo>>24;
	buffer[3]=mac_address_lo>>16;
	buffer[4]=mac_address_lo>>8;
	buffer[5]=mac_address_lo;

	return 0;
}

