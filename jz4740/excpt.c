/*
 * excpt.c
 *
 * Handle exceptions, dump all generic registers for debug.
 *
 * Author: Seeger Chin
 * e-mail: seeger.chin@gmail.com
 *
 * Copyright (C) 2006 Ingenic Semiconductor Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <mipsregs.h>
#include <jz4740.h>

/*
const static char *regstr[] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};
*/

#define MIPS_EXP_NUM    32

unsigned int Process_RA = 0;
unsigned int Process_SP = 0;

typedef void (*PFun_Exception_Handler)(unsigned int);

typedef struct _Except_Struct
{
	unsigned int sp;
	unsigned int ra;
    unsigned int arg;
 	PFun_Exception_Handler except_handle;
}Except_Struct, *PExcept_Struct;

static Except_Struct g_pExcept_info[MIPS_EXP_NUM];

static unsigned int g_MaxExceptNum = 0;
static unsigned int g_CurExcept = 0;

#define malloc(x) alloc(x)
#define free(x) deAlloc(x)

void default_exept_handle(unsigned int *sp, unsigned int arg)
{
    printf("Exception %d\n", arg);
	printf("CAUSE=%08x EPC=%08x\n", read_c0_cause(), read_c0_epc());
	printf("SP= %08x\n", sp);
	printf("AT= %08x  ra= %08x  fp= %08x  gp= %08x\n",sp[27],sp[0],sp[1],sp[2]);
    printf("t9= %08x  t8= %08x  s7= %08x  S6= %08x\n",sp[3],sp[4],sp[5],sp[6]);
    printf("s5= %08x  s4= %08x  s3= %08x  s2= %08x\n",sp[7],sp[8],sp[9],sp[10]);
    printf("s1= %08x  s0= %08x  t7= %08x  t6= %08x\n",sp[11],sp[12],sp[13],sp[14]);
    printf("t5= %08x  t4= %08x  t3= %08x  t2= %08x\n",sp[15],sp[16],sp[17],sp[18]);
    printf("t1= %08x  t0= %08x  a3= %08x  a2= %08x\n",sp[19],sp[20],sp[21],sp[22]);
    printf("a1= %08x  a0= %08x  v1= %08x  v0= %08x\n",sp[23],sp[24],sp[25], sp[26]);
	printf("\n");
    printf("c_except_handler: while(1)");
    printf("\n");

    while(1);
}

void InitExcept()
{
    int i;

    for(i= 0; i < MIPS_EXP_NUM; i++)
    {
        g_pExcept_info[i].sp = 0;
        g_pExcept_info[i].ra = 0;
        g_pExcept_info[i].arg = 0;
        g_pExcept_info[i].except_handle = 0;
    }
}

int Setup_except_handle(unsigned int except_index, PFun_Exception_Handler except_handle, unsigned int arg)
{
    if(except_index >= MIPS_EXP_NUM)
        return -1;

    g_pExcept_info[except_index].arg = arg;
    g_pExcept_info[except_index].except_handle = except_handle;

    return 0;
}

/*
void DeinitExcept()
{
	if(g_pExcept)
		free(g_pExcept);
}
*/

/*
int AddExcept(PFun_Exception_Modal_Handler pFun)
{
	
	if(g_CurExcept >= g_MaxExceptNum)
	{
		g_CurExcept++;
		return 0;
	}
	g_pExcept->sp = Process_SP;
	g_pExcept->ra = Process_RA;
	g_pExcept->except_handle = pFun;
	g_CurExcept++;
	return 1; 
}
*/

/*
void DecExcept(void)
{
    if(g_CurExcept <= 0) return;
    g_CurExcept--;
    g_pExcept[g_CurExcept].except_handle = 0;
    g_pExcept[g_CurExcept].sp            = 0;
    g_pExcept[g_CurExcept].ra            = 0;
}
*/

void c_except_handler(unsigned int *sp, unsigned int except_num)
{
    if(g_pExcept_info[except_num].except_handle == 0)
        default_exept_handle(sp, except_num);

    g_pExcept_info[except_num].except_handle((unsigned int)sp);

	write_32bit_cp0_register(CP0_EPC, g_pExcept_info[except_num].ra);
	__asm__ __volatile__("eret\n\t");    
}

/*
void c_except_handler(unsigned int *sp)
{
	unsigned int i;
	printf("CAUSE=%08x EPC=%08x\n", read_c0_cause(), read_c0_epc());
//	for (i=0;i<32;i++) {
//		if (i % 4 == 0)
//			printf("\n");
//		printf("%4s %08x ", regstr[i], sp[i]);
//	}

	printf("SP= %08x\n", sp);
	printf("AT= %08x  ra= %08x  fp= %08x  gp= %08x\n",sp[27],sp[0],sp[1],sp[2]);
    printf("t9= %08x  t8= %08x  s7= %08x  S6= %08x\n",sp[3],sp[4],sp[5],sp[6]);
    printf("s5= %08x  s4= %08x  s3= %08x  s2= %08x\n",sp[7],sp[8],sp[9],sp[10]);
    printf("s1= %08x  s0= %08x  t7= %08x  t6= %08x\n",sp[11],sp[12],sp[13],sp[14]);
    printf("t5= %08x  t4= %08x  t3= %08x  t2= %08x\n",sp[15],sp[16],sp[17],sp[18]);
    printf("t1= %08x  t0= %08x  a3= %08x  a2= %08x\n",sp[19],sp[20],sp[21],sp[22]);
    printf("a1= %08x  a0= %08x  v1= %08x  v0= %08x\n",sp[23],sp[24],sp[25], sp[26]);
	printf("\n");
	g_CurExcept--;
	if(g_CurExcept < g_MaxExceptNum)
	{
		i = (unsigned int)g_pExcept[g_CurExcept].except_handle;
		if(i)
		{
			Process_SP = g_pExcept[g_CurExcept].sp;
			Process_RA = g_pExcept[g_CurExcept].ra;
	  }
    }else if(main_except_handle){
		i = main_except_handle;	
    }else{
        printf("c_except_handler: while(1)");
		while(1);
    }
	write_32bit_cp0_register(CP0_EPC,i);
	__asm__ __volatile__("eret\n\t");	
}
*/

void watch_except(unsigned int arg)
{
    unsigned int *sp;

    sp = (unsigned int*)arg;

    printf("Watch Exception\n");
	printf("CAUSE=%08x EPC=%08x\n", read_c0_cause(), read_c0_epc());
	printf("SP= %08x\n", sp);
	printf("AT= %08x  ra= %08x  fp= %08x  gp= %08x\n",sp[27],sp[0],sp[1],sp[2]);
    printf("t9= %08x  t8= %08x  s7= %08x  S6= %08x\n",sp[3],sp[4],sp[5],sp[6]);
    printf("s5= %08x  s4= %08x  s3= %08x  s2= %08x\n",sp[7],sp[8],sp[9],sp[10]);
    printf("s1= %08x  s0= %08x  t7= %08x  t6= %08x\n",sp[11],sp[12],sp[13],sp[14]);
    printf("t5= %08x  t4= %08x  t3= %08x  t2= %08x\n",sp[15],sp[16],sp[17],sp[18]);
    printf("t1= %08x  t0= %08x  a3= %08x  a2= %08x\n",sp[19],sp[20],sp[21],sp[22]);
    printf("a1= %08x  a0= %08x  v1= %08x  v0= %08x\n",sp[23],sp[24],sp[25], sp[26]);
	printf("\n");
    printf("c_except_handler: while(1)");
    printf("\n");

    while(1);
}

void add_watch_point(unsigned int addr)
{
    unsigned int tmp;

    addr = (addr & (~0x7)) | 0x1;
    tmp = 1 << 30;

    __asm__ __volatile__("mtc0 %0, $18\n\t"
                        "mtc0 %1, $19\n\t"
                        :
                        : "r"(addr), "r"(tmp)
                        );
    
    Setup_except_handle(23, watch_except, 0);
}

void rsv_ins_except(unsigned int arg)
{
    unsigned int cause;
    unsigned int epc;
    unsigned int *ins;
    unsigned int i;

    cause = read_c0_cause();
    epc = read_c0_epc();

    printf("Reserved Instruction Exception\n");
	printf("CAUSE=%08x EPC=%08x\n", cause, epc);

    ins = (unsigned int*)epc -5;
    for(i= 0; i < 10; i++)
    {
        printf("%08x:  %08x\n", ins, *ins++);
    }

	printf("\n");
    printf("c_except_handler: while(1)");
    printf("\n");

    while(1);
}

void add_rsv_ins_except()
{
dgprintf("Add ins exception\n");
    Setup_except_handle(10, rsv_ins_except, 0);
}



