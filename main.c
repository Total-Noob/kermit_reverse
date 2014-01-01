/*
	Full kermit.prx reverse by Total_Noob
*/

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspsysevent.h>

#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("sceKermit_Driver", 0x1007, 1, 0);

#define REG32(ADDR) (*(volatile u32 *)(ADDR))

#define MAX_SEMA_IDS_1 16
#define MAX_SEMA_IDS_2 3
#define MAX_INTR_HANDLERS 16

int sceKernelPowerLock(int a0);
int sceKernelPowerUnlock(int a0);

typedef struct
{
	u32 cmd; //0x0
	SceUID sema_id; //0x4
	u64 *reponse; //0x8
	u32 unk_C; //0xC
} KermitPacket; //0x10

typedef struct
{
	u32 cmd; //0x0
	u32 kermit_addr; //0x4
} KermitCommand; //0x8

typedef struct
{
	u64 result; //0x0
	SceUID sema_id; //0x8
	int unk_C; //0xC
	u64 *reponse; //0x10
	u64 unk_1C; //0x1C
} KermitReponse; //0x24

typedef struct
{
	int unk_0; //0x0
	int unk_4; //0x4
} KermitInterrupt; //0x8

int SysEventHandler(int ev_id, char *ev_name, void *param, int *result);

//0x00000E30
PspSysEventHandler event_handler =
{
	sizeof(PspSysEventHandler),
	"SceKermit",
	0x00FFFF00,
	SysEventHandler
};

SceUID g_sema_ids_1[MAX_SEMA_IDS_1]; //0x00000E70
SceUID g_sema_ids_2[MAX_SEMA_IDS_2]; //0x00000EB0

int g_virtual_intr_handlers[MAX_INTR_HANDLERS]; //0x00000EC0

int g_pipe_id; //0x00000EBC

int g_active_connections; //0x00000F00
int g_enable_kermit; //0x00000F04

inline static unsigned int pspClz(unsigned int a)
{
	unsigned int ret;
	asm volatile ("clz %0, %1\n" : "=r" (ret) : "r" (a));
	return ret;
}

inline static unsigned int pspBitrev(unsigned int a)
{
	unsigned int ret;
	asm volatile ("bitrev %0, %1\n" : "=r" (ret) : "r" (a));
	return ret;
}

inline static int pspMin(int a, int b)
{
	int ret;
	asm volatile ("min %0, %1, %2\n" : "=r" (ret) : "r" (a), "r" (b));
	return ret;
}

inline static void pspSync()
{
	asm volatile ("sync\n");
}

inline static void SetRegister(u32 reg, u32 val)
{
	u32 old_val = REG32(reg);

	volatile u32 volatile_val;
	volatile_val = REG32(reg) = old_val & ~val;
	volatile_val = REG32(reg) = old_val | val;
}

inline static void ResetRegisters()
{
	REG32(0xBC300040) = -1;
	REG32(0xBC300044) = 0;
	REG32(0xBC300048) = -1;
	REG32(0xBC30004C) = 0;
	REG32(0xBC300050) = 0;
}

//0x000006E0
__attribute__((noinline)) void sceKermitWait() 
{
	pspSync();

	REG32(0xBD000004) = 0xF;

	volatile u32 volatile_val;	
	while((volatile_val = REG32(0xBD000004)) != 0);
	while((volatile_val = REG32(0xBD000000)) != 0xF);
}

//0x000009D8
void sceKermitCallVirtualInterruptHandler(unsigned int high_bits) 
{
	if(high_bits != 0)
	{
		unsigned int i = 0;

		do
		{
			unsigned int clz = pspClz(pspBitrev(high_bits));

			i += clz;

			high_bits >>= clz;
			high_bits >>= 1;

			/* Call interrupt handler */
			void (* handler)() = (void *)g_virtual_intr_handlers[i];
			if(handler)
			{
				handler();
			}

			/* Set something */
			KermitInterrupt *kermit_interrupt = (KermitInterrupt *)0xBFC008C0;
			kermit_interrupt[i].unk_0 = 0;

			i++;
		} while(high_bits != 0);
	}
}

//0x00000000
int interrupt_handler()
{
	int intr = sceKernelCpuSuspendIntr();

	/* Unmask */
	unsigned int bits = REG32(0xBC300030) & ~0x4002;
	REG32(0xBC300030) = bits;
	pspSync();

	/* High bits reserved for interrupt handlers */
	unsigned int high_bits = (bits >> 16);
	if(high_bits != 0)
	{
		sceKermitCallVirtualInterruptHandler(high_bits);
	}

	/* Low bits reserved for sema signals */
	unsigned int low_bits = bits & 0xFFFF;

	if(low_bits != 0)
	{
		unsigned int i = 0;

		do
		{
			unsigned int clz = pspClz(pspBitrev(low_bits));

			i += clz;

			low_bits >>= clz;
			low_bits >>= 1;

			if(i >= 4)
			{
				if(i < 7)
				{
					/* Signal for sceKermitSendCommand */
					int num = i - 4;
					sceKernelSignalSema(g_sema_ids_2[num], 1);
				}
				else if(i < 10)
				{
					unsigned int num = i - 7;
					KermitReponse *kermit_reponse = (KermitReponse *)0xBFC00840;

					*kermit_reponse[num].reponse = kermit_reponse[num].result;

					if(sceKernelSignalSema(kermit_reponse[num].sema_id, 1) == 0)
					{
						SetRegister(0xBC300050, 1 << i);
					}
				}
			}

			i++;
		} while(low_bits != 0);
	}

	sceKernelCpuResumeIntr(intr);
	return -1;
}

//0x00000A6C
int GetVramOrScratchpadAddr(void *data, unsigned int size)
{
	u32 physical_address = 0;
	u32 io_base = 0;

	u32 data_addr = ((u32)data & 0x1FFFFFFF);

	if(0x7FFFFF < (data_addr + 0xFC000000))
	{
		if((data_addr + 0xFFFF0000) >= 0x4000)
		{
			return (u32)data;
		}

		/* Scratchpad from 0x00010000-0x00014000 */
		physical_address = ((u32)data & 0x03FFFFFF);
		io_base = 0x1D00000;
	}
	else
	{
		/* Vram from 0x04000000-0x04800000 */
		physical_address = ((u32)data & 0x03FFFFFF);
		io_base = 0x1E00000;
	}

	u32 kermit_addr = (((physical_address + io_base) + 0xAA000000) & 0x9FFFFFFF);

	memcpy((void *)kermit_addr, data, size);
	sceKernelDcacheWritebackInvalidateRange((void *)kermit_addr, size);

	return kermit_addr;
}

//0x0000069C
int sceKermitDisableKermit() 
{
	g_enable_kermit = 0;
	return 0;
}

//0x000006AC
int sceKermitIsActiveConnection() 
{
	return (g_active_connections < 1);
}

//0x000006BC
int sceKermitRegisterVirtualIntrHandler(unsigned int num, int (* handler)())
{
	if(num < MAX_INTR_HANDLERS)
	{
		g_virtual_intr_handlers[num] = (int)handler;
	}

	return 0;
}

//0x000002B8
int sceKermitDisplaySync() 
{
	int intr = sceKernelCpuSuspendIntr();

	/* LCDC clear */
	REG32(0xBE140000) &= ~1;

	/* Wait 64 microseconds */
	unsigned int timelow = sceKernelGetSystemTimeLow();
	while((sceKernelGetSystemTimeLow() - timelow) < 64);

	/* Set register */
	SetRegister(0xBC300050, 0x400);

	/* Wait */
	while((REG32(0xBC300030) & 0x400) == 0);

	/* Set register */
	REG32(0xBC300030) = 0x400;
	pspSync();

	/* LCDC set */
	REG32(0xBE140000) |= 3;

	sceKernelCpuResumeIntr(intr);

	return intr;
}

//not used in any kermit module
//0x00000178
int sceKermitSendNumber(unsigned int num, unsigned int is_callback) 
{
	int res;

	res = sceKernelPowerLock(1);
	if(res != 0)
	{
		return res;
	}

	sceKermitWait();

	if(num >= MAX_SEMA_IDS_1)
	{
		sceKernelPowerUnlock(1);
		return 0x80010016;
	}

	/* Signal */
	int intr = sceKernelCpuSuspendIntr();
	unsigned int val = 1 << num;
	REG32(0xBC300038) |= val;
	SetRegister(0xBC300050, val);
	sceKernelCpuResumeIntr(intr);

	/* Wait for reponse */
	SceUID sema_id = g_sema_ids_1[num];
	if(is_callback)
	{
		res = sceKernelWaitSemaCB(sema_id, 1, NULL);
	}
	else
	{
		res = sceKernelWaitSema(sema_id, 1, NULL);
	}

	if(res != 0)
	{
		sceKernelPowerUnlock(1);
		return pspMin(res, 0);
	}

	sceKernelPowerUnlock(1);
	return 0;
}

//0x00000394
int sceKermitSendCommand(KermitPacket *packet, u32 cmd_mode, u32 cmd, unsigned int argc, unsigned int is_callback, u64 *resp)
{
	int res;
	int intr;
	SceUID sema_id;

	/* Kermit not enabled */
	if(!g_enable_kermit)
	{
		sceKernelDelayThread(10 * 1000);
		*resp = 0;
		return 0;
	}

	/* Kermit active */
	intr = sceKernelCpuSuspendIntr();
	g_active_connections++;
	sceKernelCpuResumeIntr(intr);

	unsigned int num;

	switch(cmd_mode)
	{
		case 5: //KERMIT_MODE_AUDIO
			num = 1;
		break;

		case 6: //KERMIT_MODE_ME
			num = 2;
		break;

		default:
			num = 0;
		break;
	}

	SceUInt timeout = 5 * 1000 * 1000;
	res = sceKernelWaitSema(g_sema_ids_2[num], 1, &timeout);
	if(res != 0)
	{
		goto EXIT;
	}

	res = sceKernelReceiveMsgPipe(g_pipe_id, &sema_id, sizeof(SceUID), 0, 0, NULL);
	if(res != 0)
	{
		goto EXIT;
	}

	KermitCommand *kermit_command = (KermitCommand *)0xBFC00800;
	kermit_command[num].cmd = (cmd_mode << 16) | cmd;

	packet->cmd = cmd;
	packet->sema_id = sema_id;
	packet->reponse = (u64 *)packet;

	unsigned int packet_size = ((argc + sizeof(u64) + 1) & 0xFFFFFFF8) * sizeof(u64);
	kermit_command[num].kermit_addr = GetVramOrScratchpadAddr(packet, packet_size);

	sceKermitWait();

	res = sceKernelPowerLock(1);
	if(res != 0)
	{
		goto EXIT;
	}

	/* Signal */
	intr = sceKernelCpuSuspendIntr();
	SetRegister(0xBC300050, 1 << (num + 4));
	sceKernelCpuResumeIntr(intr);

	/* Wait for reponse */
	if(is_callback)
	{
		sceKernelWaitSemaCB(sema_id, 1, NULL);
	}
	else
	{
		sceKernelWaitSema(sema_id, 1, NULL);
	}

	res = sceKernelSendMsgPipe(g_pipe_id, &sema_id, sizeof(SceUID), 0, NULL, NULL);
	if(res != 0)
	{
		sceKernelPowerUnlock(1);
		goto EXIT;
	}

	/* Copy data to response */
	*resp = ((u64 *)packet)[0];

	res = sceKernelPowerUnlock(1);

EXIT:
	intr = sceKernelCpuSuspendIntr();
	g_active_connections--;
	sceKernelCpuResumeIntr(intr);

	return pspMin(res, 0);
}

//0x00000764
int SysEventHandler(int ev_id, char *ev_name, void *param, int *result)
{
	u32 val;

	switch(ev_id)
	{
		case 0x4000: //suspend
			val = REG32(0xBC300038) & 0x4002;
		break;

		case 0x10000: //resume
			ResetRegisters();
			val = (REG32(0xBC300038) & 0x4002) | 0xFFFF07F0;
		break;

		default:
			return 0;
	}

	/* Set register */
	REG32(0xBC300038) = val;

	return 0;
}

inline static int CreateSemaIds(SceUID *sema_ids, int initVal, int max)
{
	int i;
	for(i = 0; i < max; i++)
	{
		SceUID sema_id = sceKernelCreateSema("sceCompat", 0x100, initVal, 1, NULL);
		if(sema_id <= 0)
		{
			return -1;
		}

		sema_ids[i] = sema_id;
	}

	return 0;
}

//0x00000850
int sceKermitInit()
{
	/* Create semaphore list */
	if(CreateSemaIds(g_sema_ids_1, 0, MAX_SEMA_IDS_1) < 0)
	{
		return -1;
	}

	if(CreateSemaIds(g_sema_ids_2, 1, MAX_SEMA_IDS_2) < 0)
	{
		return -1;
	}

	/* Create message pipe */
	g_pipe_id = sceKernelCreateMsgPipe("sceCompat", PSP_MEMORY_PARTITION_KERNEL, 0x1000, (void *)0x40, NULL);
	if(g_pipe_id < 0)
	{
		return -1;
	}

	if(sceKernelSendMsgPipe(g_pipe_id, g_sema_ids_1, 0x40, 0, NULL, NULL) != 0)
	{
		return -1;
	}

	/* Register interrupt handler */
	if(sceKernelRegisterIntrHandler(PSP_MECODEC_INT, 2, interrupt_handler, NULL, NULL) != 0)
	{
		return -1;
	}

	/* Reset registers */
	ResetRegisters();
	REG32(0xBC300038) = (REG32(0xBC300038) & 0x4002) | 0xFFFF07F0;

	/* Enable interrupt handler */
	if(sceKernelEnableIntr(PSP_MECODEC_INT) != 0)
	{
		return -1;
	}
	
	return 0;
}

//0x000007DC
int sceKermitEnd()
{
	/* Clear register */
	REG32(0xBC300038) = 0;

	/* Release interrupt handler */
	int res = sceKernelReleaseIntrHandler(PSP_MECODEC_INT);
	if(res != 0)
	{
		return res;
	}

	/* Delete semaphores */
	unsigned int i;
	for(i = 0; i < MAX_SEMA_IDS_1; i++)
	{
		int res = sceKernelDeleteSema(g_sema_ids_1[i]);
		if(res != 0)
		{
			return res;
		}
	}

	return 0;
}

//0x00000610
int module_start(SceSize args, void *argp) 
{
	sceKernelUnregisterSysEventHandler(&event_handler);

	int res = sceKermitInit();
	if(res < 0)
	{
		return 1;
	}

	sceKernelRegisterSysEventHandler(&event_handler);

	return 0;
}

//0x0000067C
int module_reboot_before(SceSize args, void *argp) 
{
	sceKermitEnd();
	return 0;
}
