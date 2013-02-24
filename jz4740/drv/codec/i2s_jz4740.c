/*
 * i2s.c
 *
 * JzSOC On-Chip I2S audio driver.
 *
 * Author: Jim Gao, Stephan Qiu
 * e-mail: jgao@ingenic.cn
 *         dsqiu@ingenic.cn
 *
 * Copyright (C) 2006-2007 Ingenic Semiconductor Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <includes.h>
#include <jz4740.h>
#include "pcm.h"
#include "dm.h"
//#include "ao_jz4740_vol.h"

#define CONFIG_I2S_AK4642EN
#define DMAC_DCCSR_DAM DMAC_DCMD_DAI
#define DMAC_DCCSR_SAM DMAC_DCMD_SAI
#define DMAC_DCCSR_RDIL_IGN DMAC_DCMD_RDIL_IGN
#ifndef u8
#define u8 unsigned char
#endif
#ifndef u16
#define u16 unsigned short
#endif
#ifndef u32
#define u32 unsigned int
#endif

#define AUDIO_READ_DMA	2
#define AUDIO_WRITE_DMA	3

#ifdef PHYADDR
#undef PHYADDR
#endif
#define PHYADDR(n)	((n) & 0x1fffffff)
#ifdef KSEG1ADDR
#undef KSEG1ADDR
#endif
#define	KSEG1ADDR(n)	(PHYADDR(n) | 0xa0000000)

#ifdef PAGE_SIZE
#undef PAGE_SIZE
#endif
#define QUEUE_MAX 4
#define PAGE_SIZE	(0x1000 / QUEUE_MAX)

extern void printf(const char *fmt, ...);
extern void HP_turn_off(void);

#define STANDARD_SPEED  48000
#define SYNC_CLK	48000

#define MAX_RETRY       100
extern int HP_on_off_flag;
static unsigned long i2s_clk;
static int		jz_audio_b; 
static int		jz_audio_rate;
static char		jz_audio_format;
static char		jz_audio_channels;
static char		jz_audio_volume;
static u16		jz_audio_factor;

int codec_get_rate(void) {return jz_audio_rate;}
char codec_get_format(void) {return jz_audio_format;}
char codec_get_channels(void) {return jz_audio_channels;}

static void jz_update_filler(int bits, int channels);

static void jz_i2s_replay_dma_irq(unsigned int);
static void jz_i2s_record_dma_irq(unsigned int);

static void (*replay_filler)(unsigned long src_start, int count, int id);
static int (*record_filler)(unsigned long dst_start, int count, int id);

static long old_CDCCR1=0;  
static long new_CDCCR1=0;  
static long old_CDCCR2=0;  
static long new_CDCCR2=0;  
static int flag_CDCCR1=0;  
static int flag_CDCCR2=0;

/*start*****************add volum control -by gaojian***********************/

#if 0
static void get_factor(int level);
static void fill_sample_table(void);
static u16 get_new_sample(int sample);
static int              codec_volue_factor;
static int              codec_volue_hpmute;
static int              codec_volue_hpvol;

static u16 sample_table[32768];

static u16 get_new_sample(int sample)
{
	printf("sample:%d",sample);
	if (sample >= 0)
		return sample_table[sample];
	else
		return (-(sample_table[(-sample)] + 1));
}

static void fill_sample_table(void)
{
	int cnt;
      
	sample_table[0] = 0;
	for (cnt = 1; cnt <= 32767; cnt++)
		//sample_table[cnt] = (u16)((cnt * codec_volue_factor) >> 8);
		sample_table[cnt] = (u16)(cnt * codec_volue_factor / 30);
}

static void get_factor(int level)
{
	int hpmute = 0, hpvol = 0, factor = 0;
	switch (level) {
	case 1 ... 30:
		hpmute = 0;
		hpvol = 0;
		factor = level;
		break;
	case 0:
		hpmute = 1;
		hpvol = 0;
		factor = 0;
		break;
	}
	codec_volue_factor = factor;
	codec_volue_hpvol = hpvol;
	codec_volue_hpmute = hpmute;
}
#endif
/*end*****************add volum control -by gaojian ***********************/
static u16 Factor_table[32] = {
	0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240,256,
	   216, 230, 243, 256, 214,224, 235, 245, 256, 212, 221, 230, 238, 247,256
};

////////////////////////////////////////

typedef struct buffer_queue_s {
	int count;
	int id[QUEUE_MAX];
	int oldid;
} buffer_queue_t;

typedef struct left_right_sample_s
{
	signed short left;
	signed short right;
} left_right_sample_t;

static left_right_sample_t save_last_samples[64];

static unsigned long out_dma_buf[QUEUE_MAX+1];

static unsigned long out_dma_pbuf[QUEUE_MAX+1];

static unsigned long out_dma_buf_data_count[QUEUE_MAX+1];

static unsigned long in_dma_buf[QUEUE_MAX+1];
static unsigned long in_dma_pbuf[QUEUE_MAX+1];
static unsigned long in_dma_buf_data_count[QUEUE_MAX+1];

static buffer_queue_t out_empty_queue;
static buffer_queue_t out_full_queue;
static buffer_queue_t out_busy_queue;

static buffer_queue_t in_empty_queue;
static buffer_queue_t in_full_queue;
static buffer_queue_t in_busy_queue;

static int first_record_call = 0;

//static OS_EVENT *tx_sem;
OS_EVENT *tx_sem;
static OS_EVENT *rx_sem;
static OS_EVENT *pause_sem;

static OS_EVENT *pre_tx_sem;

static int preconvert_control = 0;

int IS_WRITE_PCM;

static inline int get_buffer_id(struct buffer_queue_s *q)
{
	int r;
	unsigned long flags;
	int i;

	if (q->count == 0)
		return -1;
	flags = spin_lock_irqsave();	
	r = q->id[0];
	for (i=0;i < q->count-1;i++)
		q->id[i] = q->id[i+1];
	q->count --;
	spin_unlock_irqrestore(flags);
	return r;
}

static inline void put_buffer_id(struct buffer_queue_s *q, int id)
{
	unsigned long flags;
	flags = spin_lock_irqsave();
	q->id[q->count] = id;
	q->count ++;
	spin_unlock_irqrestore(flags);
}

static  inline int elements_in_queue(struct buffer_queue_s *q)
{
	int r;
	unsigned long flags;

	flags=spin_lock_irqsave();
	r = q->count;
	spin_unlock_irqrestore(flags);

	return r;
}

/****************************************************************************
 * Architecture related routines
 ****************************************************************************/

static void jz_i2s_record_dma_irq (unsigned int arg)
{
	int dma = AUDIO_READ_DMA;
	int id1, id2;

	dma_stop(dma);
//	if(preconvert_control==1)
//	{
//		OSSemPost(pre_tx_sem);
//		return;
//	}
	if (__dmac_channel_address_error_detected(dma)) {
		printf("%s: DMAC address error.\n", __FUNCTION__);
		__dmac_channel_clear_address_error(dma);
	}
	if (__dmac_channel_transmit_end_detected(dma)) {
		__dmac_channel_clear_transmit_end(dma);

		id1 = get_buffer_id(&in_busy_queue);
		put_buffer_id(&in_full_queue, id1);
		//__dcache_invalidate_all();

		OSSemPost(rx_sem);

		if ((id2 = get_buffer_id(&in_empty_queue)) >= 0) {
			put_buffer_id(&in_busy_queue, id2);

			in_dma_buf_data_count[id2] = in_dma_buf_data_count[id1];

			dma_start(dma, PHYADDR(AIC_DR), in_dma_pbuf[id2],
				  in_dma_buf_data_count[id2]);
		} else
			in_busy_queue.count = 0;
	}
}


int pause_flag,control=0,jz_pause_flag =0,restart=0;
int restart_count = 1;

static int restartpcm(unsigned int smp,unsigned char *p)
{
	signed short l_sample,r_sample;
	signed char *mono;
	unsigned int cur_dma_buffer_count = 0;

	int i,l_flag,r_flag,step_len;
	signed int left_sam,right_sam,temp,l_val,r_val;
	unsigned long l_sample_count,r_sample_count,sample_count,temp_convert;

	volatile signed short *stero;
	volatile signed short value;

	//smp  =  smp - jz_audio_channels * 1;
	
	switch(jz_audio_b)
	{
	case 8:
		mono = (signed char *)p;
		break;
		
	case 16:
		stero = (signed short *)p;
		break;
	}
		
	switch(jz_audio_channels)
	{
	case 1:
		l_sample = (unsigned int)mono[smp];
		r_sample = (unsigned int)mono[smp + 1];
		break;
		
	case 2:
		l_sample = (signed short)( *(stero) );
		r_sample = (signed short)( *(stero + 1) );
		//printf("\n{ l_sample:%d ; r_sample:%d }",l_sample,r_sample);

		left_sam = (signed int)l_sample;
		right_sam = (signed int)r_sample;
		sample_count = smp / 4;
                ////////////////////////////////////////
		for(i=0;i <= (sample_count/2);i++) {

			/*if(sample_count/2 > 20) {
				if((i < 10) || (i > (sample_count/2 - 10) ))
					printf("A[ %d : %d---%d ]\n",i,(signed int)(*(stero + i)),(signed int)(*(stero + sample_count - i)));
					}*/

			l_val = (signed int)(*(stero + i));
			*(stero + i) = *(stero + sample_count - i);
			*(stero + sample_count - i) = (signed short)l_val;
		
			/*if(sample_count/2 > 20) {
				if((i < 10) || (i > (sample_count/2 - 10) ))
					printf("B[ %d : %d---%d ]\n",i,(signed int)(*(stero + i)),(signed int)(*(stero + sample_count - i)));
					}*/
		}
		cur_dma_buffer_count = smp;
                ////////////////////////////////////////////
		break;
	}
	 
	return cur_dma_buffer_count;
}


static int insertpcm(unsigned int smp,unsigned char *p)
{
	signed short l_sample,r_sample;
	signed char *mono;
	unsigned int cur_dma_buffer_count = 0;

	int i,l_flag,r_flag,step_len;
	signed int left_sam,right_sam,temp,l_val,r_val;
	unsigned long l_sample_count,r_sample_count,sample_count,temp_convert;

	volatile signed short *stero;
	volatile signed short value;

	//smp  =  smp - jz_audio_channels * 1;
	if(smp == 0) return 0;
	switch(jz_audio_b)
	{
	case 8:
		mono = (signed char *)p;
		break;
		
	case 16:
		stero = (signed short *)p;
		break;
		
	}
	
	switch(jz_audio_channels)
	{
	case 1:
		l_sample = (unsigned int)mono[smp];
		r_sample = (unsigned int)mono[smp + 1];
		break;
		
	case 2:
		l_sample = (signed short)( *(stero + (smp/2) -2) );
		r_sample = (signed short)( *(stero + (smp/2) -1) );

		left_sam = (signed int)l_sample;
		right_sam = (signed int)r_sample;

		if(left_sam == 0 && right_sam == 0)
			return 0;
		//insert some sample here
                ////////////////////////////////////////
		memset(p,0,smp);
		step_len = jz_audio_rate / 10 * 4;
		step_len /= 2;
		step_len = 0x7fff / step_len + 1;

		l_sample_count = 0;
		l_val = left_sam;

		while(1) {
			if(l_val > 0) {

				if(l_val >= step_len) {
					l_val -= step_len;
					l_sample_count ++;
				} else
					break;

			} 
			
			if(l_val < 0) {

				if(l_val <= -step_len) {
					l_val += step_len;
					l_sample_count ++;
				} else
					break;

			}
			
			if(l_val == 0) 
				break;
		}

		r_sample_count = 0;
		r_val = right_sam;
		while(1) {
			if(r_val > 0) {

				if(r_val >= step_len) {
					r_val -= step_len;
					r_sample_count ++;
				} else
					break;
			} 
			
			if(r_val < 0) {

				if(r_val <= -step_len) {
					r_val += step_len;
					r_sample_count ++;
				} else
					break;
			}
			
			if(r_val == 0)
				break;
			
		}

		//fill up
		if(l_sample_count > r_sample_count)
			sample_count = l_sample_count;
		else
			sample_count = r_sample_count;

		l_val = left_sam;
		r_val = right_sam;
		for(i=0;i <= sample_count;i++) {
			
			*stero = (signed short)l_val;
		
	
			stero ++;
			
			if(l_val > step_len) {
				l_val -= step_len;
			} else if(l_val < -step_len) {
				l_val += step_len;
			} else if(l_val >= -step_len && l_val <= step_len) {
				l_val = 0;
			}

			*stero = (signed short)r_val;

			stero ++;

			if(r_val > step_len) {
				r_val -= step_len;
			} else if(r_val < -step_len) {
				r_val += step_len;
			} else if(r_val >= -step_len && r_val <= step_len) {
				r_val = 0;
			}

		}
		
		*stero = 0;
	
		stero ++;
		*stero = 0;
	
		stero ++;
		sample_count += 1;	

		*stero = 0;
	
		stero ++;
		*stero = 0;
	
		stero ++;
		sample_count += 1;

		*stero = 0;
	
		stero ++;
		*stero = 0;
	
		stero ++;
		sample_count += 1;

		cur_dma_buffer_count = sample_count * 4;

		break;
	}
	 
	return cur_dma_buffer_count;
}
static void pcm_pause()
{
	unsigned char err;

	int dma = AUDIO_WRITE_DMA;
	int id;

	if(control==1)
	{
		control = 0;
		
	}else
	{
		control = 1;
	}
	
	
	if(pause_flag == 0)
	{
		pause_flag = 1;
		printf("pause:%d\n",pause_flag);

	}else
	{
		pause_flag = 0;
		printf("pause:%d\n",pause_flag);

		__intc_unmask_irq(20);
		id = out_busy_queue.oldid;
		if(out_dma_buf_data_count[id] != 0) {
			dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
					  out_dma_buf_data_count[id]);
		}
		
	}
}

static void pcm_start(void)
{
	unsigned char err;

	int dma = AUDIO_WRITE_DMA;
	int id;

	__intc_unmask_irq(20);
	id = out_busy_queue.oldid;
	if(out_dma_buf_data_count[id] != 0) {
		dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
			  out_dma_buf_data_count[id]);
	}
	

}

#if 1
static void jz_i2s_replay_dma_irq (unsigned int arg)
{
	int dma = AUDIO_WRITE_DMA;
	int id;
	dma_stop(dma);
	if (__dmac_channel_address_error_detected(dma)) {
		printf("%s: DMAC address error.\n", __FUNCTION__);
		__dmac_channel_clear_address_error(dma);
	}

	if (__dmac_channel_transmit_end_detected(dma)) {
		__dmac_channel_clear_transmit_end(dma);
		
		if ((id = get_buffer_id(&out_busy_queue)) < 0)
			printf("Strange DMA finish interrupt for AIC module\n");
                //set CDCCR1
		if(flag_CDCCR1)
		{
			REG_ICDC_CDCCR1=new_CDCCR1;
			old_CDCCR1=new_CDCCR1;
			flag_CDCCR1=0;
		}
                //set CDCCR2
		if(flag_CDCCR2)
		{
			REG_ICDC_CDCCR2=new_CDCCR2;
			old_CDCCR2=new_CDCCR2;
			flag_CDCCR2=0;
		}

		put_buffer_id(&out_empty_queue, id);
		if ((id = get_buffer_id(&out_full_queue)) >= 0) {
			put_buffer_id(&out_busy_queue, id);
			dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
				  out_dma_buf_data_count[id]);

		} else {
			out_busy_queue.count = 0;
			printf("e\n");
        }

		if (elements_in_queue(&out_empty_queue) > 0)
			OSSemPost(tx_sem);
		//if the queue will be empty, signal semaphore
//		if(elements_in_queue(&out_full_queue)<0)
//		  OSSemPost(tx_sem);
	}
}
#endif

#if 0
void anti_pop_1(void)
{
	unsigned char err;
	int dma = AUDIO_WRITE_DMA;
	int id;
	int i,j,tx_con=0,step_cnt,step_len;
	int d;
	unsigned short left,right,tmp1,tmp2;

	left =((unsigned short) save_last_samples[out_busy_queue.oldid].left)+0x8000;
	right = (( unsigned short)save_last_samples[out_busy_queue.oldid].right)+0x8000;
	
	if(left >= 0x8000)
	{
		tmp1 = left - 0x8000;
	}
	else{
		tmp1 = 0x8000 - left;
	}
	
	if(right >= 0x8000)
	{
		tmp2 = right - 0x8000;
	}
	else{
		tmp2 = 0x8000 - right;
	}
			
	step_len = jz_audio_rate / 10 * 4;
	step_len /= 2;
	step_len = 0x7fff / step_len + 1;
//	step_len = 4;
			
	if(tmp1>=tmp2)
	{
		step_cnt=tmp1/step_len;
	}
	else
	{
		step_cnt=tmp2/step_len;
	}
			
	for(i=0;i<step_cnt;) {		
		tx_con = (REG_AIC_SR & 0x00003f00) >> 8;
		if(tx_con < 30) {
	
				
				if(left - 0x8000 > step_len){
					left = left - step_len;
				}
				else if(left - 0x8000 < -step_len){
					left = left + step_len;
				}else if(left - 0x8000 >= -step_len && left - 0x8000 <= step_len) {
					left = 0x8000;
				}
				
				if(right - 0x8000 > step_len){
					right = right - step_len;
				}
				else if(right - 0x8000 < -step_len){
					right = right + step_len;
				}else if(right - 0x8000 >= -step_len && right - 0x8000 <= step_len) {
					right = 0x8000;
				}
				if(i%2==0){
					REG_AIC_DR = left ;
					REG_AIC_DR = right;
				}
				else{
					
					REG_AIC_DR = 0xffff-left;
					REG_AIC_DR = 0xffff-right;
				}
				
				i++;
				
		}
		
	}
	
}


void anti_pop_2(void)
{
	unsigned char err;
	int dma = AUDIO_WRITE_DMA;
	int id;
	int i,j,tx_con=0,step_cnt,step_len;
	int d;
	unsigned short left,right,tmp1,tmp2;
	

	left =((unsigned short) save_last_samples[out_busy_queue.oldid].left)+0x8000;
	right = (( unsigned short)save_last_samples[out_busy_queue.oldid].right)+0x8000;
	
	if(left >= 0x8000)
	{
		tmp1 = left - 0x8000;
	}
	else{
		tmp1 = 0x8000 - left;
	}
	
	if(right >= 0x8000)
	{
		tmp2 = right - 0x8000;
	}
	else
	{
		tmp2 = 0x8000 - right;
	}
	step_len = 4;
			
	if(tmp1>=tmp2)
	{
		step_cnt=tmp1/step_len;
	}
	else
	{
		step_cnt=tmp2/step_len;
	}

			
	for(i=0;i<step_cnt;)
	{		
		tx_con = (REG_AIC_SR & 0x00003f00) >> 8;
		if(tx_con < 30)
		{
			if(left - 0x8000 > step_len){
				left = left - step_len;
			}
			else if(left - 0x8000 < -step_len){
				left = left + step_len;
			}else if(left - 0x8000 >= -step_len && left - 0x8000 <= step_len) {
				left = 0x8000;
			}
				
			if(right - 0x8000 > step_len){
				right = right - step_len;
			}
			else if(right - 0x8000 < -step_len){
				right = right + step_len;
			}else if(right - 0x8000 >= -step_len && right - 0x8000 <= step_len) {
				right = 0x8000;
			}
			if(i%2==0){
				REG_AIC_DR = left ;
				REG_AIC_DR = right;
			}
			else{
					
				REG_AIC_DR = 0xffff-left;
				REG_AIC_DR = 0xffff-right;
			}
				
			i++;
		}
		
	}
	
}
void anti_pop(void)
{
	unsigned char err;
	int dma = AUDIO_WRITE_DMA;
	int id;
	int i,j,tx_con=0,step_cnt,step_len;
	int d;
	unsigned short left,right,tmp1,tmp2;
	
	left =((unsigned short) save_last_samples[out_busy_queue.oldid].left)+0x8000;
	right = (( unsigned short)save_last_samples[out_busy_queue.oldid].right)+0x8000;

	
	if(left >= 0x8000)
	{
		tmp1 = left - 0x8000;
	}
	else{
		tmp1 = 0x8000 - left;
	}
	
	if(right >= 0x8000)
	{
		tmp2 = right - 0x8000;
	}
	else
	{
		tmp2 = 0x8000 - right;
	}

	step_len = 4;
			
	if(tmp1>=tmp2)
	{
		step_cnt=tmp1/step_len;
		left=left;
		step_len=left/2;
		
	}
	else
	{
		step_cnt=tmp2/step_len;
		left=right;
		step_len=left/2;
	}

			
	for(i=0;i<step_cnt;)
	{		
		tx_con = (REG_AIC_SR & 0x00003f00) >> 8;
		if(tx_con < 30)
		{
			
			if(left - 0x8000 > step_len){
				left = left - step_len;
			}
			else if(left - 0x8000 < -step_len){
				left = left + step_len;
			}else if(left - 0x8000 >= -step_len && left - 0x8000 <= step_len) {
				left = 0x8000;
			}
				
			if(i%2==0){
				REG_AIC_DR = left ;
				REG_AIC_DR = left;
			}
			else{
					
				REG_AIC_DR = 0xffff-left;
				REG_AIC_DR = 0xffff-left;
			}
				
			i++;
		}
		
	}
	
}
static void jz_i2s_replay_dma_irq (unsigned int arg)
{
	unsigned char err;
	int dma = AUDIO_WRITE_DMA;
	int id;
	int i,j,tx_con=0,step_cnt,step_len;
	int d;
	unsigned short left,right,tmp1,tmp2;
	
	dma_stop(dma);
	if (__dmac_channel_address_error_detected(dma)) {
		printf("%s: DMAC address error.\n", __FUNCTION__);
		__dmac_channel_clear_address_error(dma);
	}

	if (__dmac_channel_transmit_end_detected(dma)) {
        __dmac_channel_clear_transmit_end(dma);

 
		if(pause_flag)
		{
			__intc_mask_irq(20);

			anti_pop();
			
			
#if 0 //insert data to DMA
			id = out_busy_queue.oldid;
//			printf("[ id:%d ; left:%d ; right:%d ]",id,save_last_samples[id].left,save_last_samples[id].right);
			out_dma_buf_data_count[id] = insertpcm(out_dma_buf_data_count[id],out_dma_buf[id]);
//			printf("count:%d\n",out_dma_buf_data_count[id]);
			if(out_dma_buf_data_count[id] != 0) {
				dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
				out_dma_buf_data_count[id]);

			}
#endif
			dma_stop(dma);
         
			
		} else {
			
			if ((id = get_buffer_id(&out_busy_queue)) < 0)
				printf("Strange DMA finish interrupt for AIC module\n");

			put_buffer_id(&out_empty_queue, id);
			if ((id = get_buffer_id(&out_full_queue)) >= 0) {
				put_buffer_id(&out_busy_queue, id);
				dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
					  out_dma_buf_data_count[id]);

			} else
				out_busy_queue.count = 0;
			}
		
//		if (elements_in_queue(&out_empty_queue) > 0)
//			OSSemPost(tx_sem);
		
	}
	
	
}
#endif

/*
 * Initialize the onchip I2S controller
 */

static void jz_i2s_initHw(void)
{
	unsigned int aic_cr_val;
	//__i2s_reset();
	
	i2s_clk = __cpm_get_i2sclk();

	__i2s_disable();
	__i2s_as_slave();
	__i2s_set_oss_sample_size(16);
	__i2s_set_iss_sample_size(16);
	//__i2s_enable();


	__i2s_disable_record();
	__i2s_disable_replay();
	__i2s_disable_loopback();

	__i2s_set_transmit_trigger(8);
	__i2s_set_receive_trigger(1);
}

static int jz_audio_set_speed(int rate)
{
/* 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 99999999 ? */
	if (rate > 48000)
		rate = 48000;
	if (rate < 8000)
		rate = 8000;
	jz_audio_rate = rate;

	i2s_codec_set_samplerate(rate);
	return rate;
}

static int record_fill_1x8_u(unsigned long dst_start, int count, int id)
{
	int cnt = 0;
	unsigned short data;
	unsigned long *s = (unsigned long*)(in_dma_buf[id]);
	unsigned char *dp = (unsigned char*)dst_start;
   
	while (count > 0) {
		count -= 4;		/* count in dword */
		cnt += 1;		
		data =(unsigned short) *s++;
		*(dp ++) =(data+0x8080) >> 8;
	}
	return cnt;
}

static int record_fill_2x8_u(unsigned long dst_start, int count, int id)
{
	int cnt = 0;
	unsigned short d1, d2;
//	unsigned long d1, d2;
	volatile unsigned long *s = (unsigned long*)(in_dma_buf[id]);
	volatile unsigned char *dp = (unsigned char*)dst_start;

	while (count > 0) {
		count -= 4;
		cnt += 2;
		s++;
		d1 = (unsigned short)*s;
		*(dp ++) = (d1+0x8000) >> 8;
		d2 = (unsigned short)(*s >> 16);
		*(dp ++) = (d2+0x8000) >> 8;
	}
	return cnt;
}


static int record_fill_1x16_s(unsigned long dst_start, int count, int id)
{
	int cnt = 0;
	unsigned long *s = (unsigned long * )(in_dma_buf[id]);
	unsigned short *dp = (unsigned short *)dst_start;
	
	while (count > 0) {
		count -= 4;		/* count in dword */
		cnt += 2;	/* count in byte */
		*(dp ++) = (*(s++) & 0xffff);
	}
	return cnt;
}

static int record_fill_2x16_s(unsigned long dst_start, int count, int id)
{
		int cnt = 0;
	unsigned long d1, d2;
	unsigned long *s = (unsigned long*)(in_dma_buf[id]);
	unsigned short *dp = (unsigned short *)dst_start;
	while (count > 0) {
		count -= 2;		/* count in dword */
			cnt += 4;	/* count in byte */
			d1 = *(s++);
			*(dp ++) = (d1 + 0x80) >> 8;
			d2 = *(s++);
			*(dp ++) = (d2 + 0x80)>> 8;
	}
	return cnt;

}
static inline int record_fill_all(unsigned long dst_start, int count, int id)
{
	memcpy((void *)dst_start,(void *)in_dma_buf[id],count);
	return count;
}

static void replay_fill_1x8_u(unsigned long src_start, int count, int id)
{
	int cnt = 0;
	unsigned char data;
	unsigned short ddata;
	unsigned char *s = (unsigned char *)src_start;
	unsigned short *dp = (unsigned short*)(out_dma_buf[id]);

	while (count > 0) {
		count -= 1;
		cnt += 2;
		data = *s++;//see ak4642en page 29
//		data = (unsigned char)get_new_sample((int)data);
		ddata = ((unsigned short) data <<8) - 0x8000;
		*(dp ++) = ddata;
	}

	out_dma_buf_data_count[id] = cnt;
}

static void replay_fill_2x8_u(unsigned long src_start, int count, int id)
{
	int cnt = 0;
	unsigned char d1;
	unsigned long dd1;
	volatile unsigned char *s = (unsigned char *)src_start;
	volatile unsigned short *dp = (unsigned short*)(out_dma_buf[id]);
	while (count > 0) {
		count -= 2;
		cnt += 4 ;
		d1 = *s++;
//		d1 = (unsigned char)get_new_sample((int)d1);
		dd1 = ((unsigned short) d1 << 8)- 0x8000;
		*(dp ++) = dd1;
		d1 = *s++;
		dd1 = ((unsigned short) d1 << 8)- 0x8000;
		*(dp ++) = dd1;

	}
	out_dma_buf_data_count[id] = cnt;
}


static void replay_fill_1x16_s(unsigned long src_start, int count, int id)
{
	int cnt = 0;
	unsigned short d1;
	unsigned long l1;
	volatile unsigned short *s = (unsigned short *)src_start;
	volatile unsigned long *dp = (unsigned long*)(out_dma_buf[id]);
	while (count > 0) {
		count -= 2;
		cnt += 2 ;
		d1 = *(s++);
//		d1 = (signed short)get_new_sample(d1);
		l1 = (unsigned long)d1;
		*(dp ++) = l1;
		*(dp ++) = l1;
	}
	cnt = cnt  * 2 *  jz_audio_b;
	out_dma_buf_data_count[id] = cnt;

}


static void replay_fill_2x16_s(unsigned long src_start, int count, int id)
{
	int cnt = 0;
	unsigned short d1;
	signed short d2;
	unsigned long l1;
	unsigned short *s = (unsigned short *)src_start;
    unsigned long *dp = (unsigned long*)(out_dma_buf[id]);
//    printf("replay_fill_2x16_s  dp:%x s:%x\r\n",dp,s);
	
	while (count > 0) {
		count -= 1;
		cnt += 1;
		d1 = *s++;
//		d2=(signed short)d1;
//		d2 = (signed short)get_new_sample(d2);
//		d1=(unsigned short)d2;
		l1 = (unsigned long)d1;
		*dp ++ = l1;
//		*dp ++ = l1;
	}
	out_dma_buf_data_count[id] = cnt;
	
}

int pcm_close(void)
{
	int id;
	printf("replay some mute\n");
	id = out_busy_queue.oldid;
	//printf("[ id:%d ; left:%d ; right:%d ]",id,save_last_samples[id].left,save_last_samples[id].right);
	out_dma_buf_data_count[id] = insertpcm(out_dma_buf_data_count[id],out_dma_buf[id]);
	/*if(out_dma_buf_data_count[id] != 0) {
				dma_start(dma, out_dma_pbuf[id], PHYADDR(AIC_DR),
				out_dma_buf_data_count[id]);
		   
				}*/
}

void (*get_volume)(short *block,int count, int id,unsigned int *out_dma_buf, int jz_audio_factor,int shift);

void SetAudioVolume(void *p)
{
	get_volume=p;
}

static inline void replay_fill_all(unsigned long src_start, int count, int id)
{
	int i;

	if(get_volume)
	{
		//	printf("get_volume11111111111:%d\n",get_volume);
//		volatile signed short *s = (signed short*)src_start;
		get_volume(src_start,count,id,out_dma_buf[id],jz_audio_factor,8);

		unsigned int *p = (unsigned int *)((unsigned int)out_dma_buf[id] & (~0x20000000));
		memcpy((void *)p,(void *)src_start,count);
		dma_cache_wback_inv((unsigned long)p, count);  
		out_dma_buf_data_count[id] = count;
	}
	else
	{
		if (jz_audio_format==AFMT_S16_LE) {
			
			volatile signed short *s = (signed short*)src_start;
			volatile signed short value;
			int i;
/*			for(i= 0;i < (count/2) ; i++) {
				value = *(s + i);
				*(s + i) = (value* jz_audio_factor)>>8;
			}*/

			unsigned int *p = (unsigned int *)((unsigned int)out_dma_buf[id] & (~0x20000000));
			memcpy((void *)p,(void *)src_start,count);
			dma_cache_wback_inv((unsigned long)p, count);  
			out_dma_buf_data_count[id] = count;
			
		} else {
			unsigned int *p = (unsigned int *)((unsigned int)out_dma_buf[id] & (~0x20000000));
			memcpy((void *)p,(void *)src_start,count);
			dma_cache_wback_inv((unsigned long)p, count);  
			out_dma_buf_data_count[id] = count;
		}
	}
	//printf("id %d count = %d src_start = %x\n",id,count,src_start);
#if 0
	//save the last left and right sample data
	volatile signed short *s = (signed short*)src_start;
	volatile signed short value;
//	printf("\nsrc %d %d ",*(s + (count/2) -2) , *(s + (count/2) -1) );
/*
  for(i= (count/2) - 4;i < (count/2) ; i++) {
		value = (signed short) ( *(s + i) );
		printf("\n( i:%d ; val:%d )",i,value);
	}
*/
	save_last_samples[id].left = (signed short) ( *(s + (count/2) -2) );
	save_last_samples[id].right = (signed short) ( *(s + (count/2) -1) );
#endif
//	printf("\n{{ id:%d ; L:%d ; R:%d }}",id,save_last_samples[id].left,save_last_samples[id].right);
}

static unsigned int jz_audio_set_format(unsigned int fmt)
{

	switch (fmt) {
	        case AFMT_U8:
			jz_audio_format = fmt;
			jz_update_filler(fmt, jz_audio_channels);
			break;
		case AFMT_S16_LE:
			jz_audio_format = fmt;
			jz_update_filler(fmt, jz_audio_channels);
		break;
	}
	return jz_audio_format;
}

static short jz_audio_set_channels(short channels)
{
	switch (channels) {
	case 1:
		//i2s_codec_set_channel(1);
		//jz_audio_channels = channels;
		//jz_update_filler(jz_audio_format, jz_audio_channels);
		//break;
	case 2:
		jz_audio_channels = channels;
		jz_update_filler(jz_audio_format, jz_audio_channels);
		break;
	case 0:
		break;
	}
	return jz_audio_channels;
}



static void jz_audio_reset(void)
{
	int i;

	__i2s_disable_replay();
	__i2s_disable_receive_dma();
	__i2s_disable_record();
	__i2s_disable_transmit_dma();
	
	if(HP_on_off_flag == 1) {
		HP_on_off_flag = 0;
		HP_turn_off();
	}

	in_empty_queue.count = QUEUE_MAX;
	out_empty_queue.count = QUEUE_MAX;

	for (i=0;i<QUEUE_MAX;i++) {
		in_empty_queue.id[i] = i;
		out_empty_queue.id[i] = i;
	}

	in_busy_queue.count = 0;
	in_full_queue.count = 0;
	out_busy_queue.count = 0;
	out_full_queue.count = 0;
}


/* I2S codec initialisation. */
static int jz_i2s_codec_init(void)
{
	i2s_codec_init();
	return 0;
}

static void jz_update_filler(int format, int channels)
{
#define TYPE(fmt,ch) (((fmt)<<3) | ((ch)&7))	/* up to 8 chans supported. */

		
	switch (TYPE(format, channels)) {
	case TYPE(AFMT_U8, 1):

		jz_audio_b = 8;
		replay_filler = replay_fill_all;
		record_filler = record_fill_1x8_u;
		__i2s_set_oss_sample_size(8);
		__i2s_set_iss_sample_size(16);
		
		dma_src_size(AUDIO_READ_DMA, 16);
		dma_dest_size(AUDIO_READ_DMA, 32);
		
		dma_src_size(AUDIO_WRITE_DMA, 32);
		dma_dest_size(AUDIO_WRITE_DMA, 8);
		
		dma_block_size(AUDIO_WRITE_DMA,8);
		dma_block_size(AUDIO_READ_DMA,4);
		
		__i2s_set_transmit_trigger(16 - 4);
		__i2s_set_receive_trigger(2);
		__aic_enable_mono2stereo();
		
		break;
	case TYPE(AFMT_U8, 2):
		jz_audio_b = 8;
		replay_filler = replay_fill_all;
		record_filler = record_fill_2x8_u;
		
		dma_src_size(AUDIO_READ_DMA, 16);
		dma_dest_size(AUDIO_READ_DMA, 32);
		
		dma_src_size(AUDIO_WRITE_DMA, 32);
		dma_dest_size(AUDIO_WRITE_DMA, 8);
		
		
		dma_block_size(AUDIO_WRITE_DMA,8);
		dma_block_size(AUDIO_READ_DMA,4);
		
		__i2s_set_oss_sample_size(8);
		__i2s_set_iss_sample_size(16);
		__i2s_set_transmit_trigger(16 - 4);
		__i2s_set_receive_trigger(2);
		__aic_disable_mono2stereo();
		
		break;
	case TYPE(AFMT_S16_LE, 1):
		jz_audio_b = 16;
		replay_filler = replay_fill_all;
		record_filler = record_fill_1x16_s;
		__i2s_set_oss_sample_size(16);
		__i2s_set_iss_sample_size(16);

		dma_src_size(AUDIO_READ_DMA, 16);
		dma_dest_size(AUDIO_READ_DMA, 32);
		
		dma_src_size(AUDIO_WRITE_DMA, 32);
		dma_dest_size(AUDIO_WRITE_DMA, 16);

		dma_block_size(AUDIO_WRITE_DMA,16);
		dma_block_size(AUDIO_READ_DMA,4);
		__i2s_set_transmit_trigger(16 - 8);
		__i2s_set_receive_trigger(2);
		if(IS_WRITE_PCM)
		__aic_enable_mono2stereo();
		
		break;
	case TYPE(AFMT_S16_LE, 2):
		jz_audio_b = 16;
		replay_filler = replay_fill_all;
		record_filler = record_fill_all;
		__i2s_set_oss_sample_size(16);
		__i2s_set_iss_sample_size(16);

		dma_src_size(AUDIO_READ_DMA, 16);
		dma_dest_size(AUDIO_READ_DMA, 32);
		
		dma_src_size(AUDIO_WRITE_DMA, 32);
		dma_dest_size(AUDIO_WRITE_DMA, 16);
		

		dma_block_size(AUDIO_READ_DMA,16);
		dma_block_size(AUDIO_WRITE_DMA,16);
/*
*   DMA settint information for 16-bit sample audio
*   dma transfer unit is 16 bytes, data port width is 16-bit
*   DMA transmitte request threshold is (16-4)*2 samples
*   so the audio buffer size must be n*8 samples
*/
		__i2s_set_transmit_trigger(16 - 4);
		__i2s_set_receive_trigger(4);
		__aic_disable_mono2stereo();
		
		break;
	}
}


static void SetVolumeReg(int val)
{
	long tmp_val;  

	//set REG_ICDC_CDCCR1
	if(val == 0)
	{
		tmp_val = REG_ICDC_CDCCR1;
		tmp_val &= ~(1 << 14);
		tmp_val |= 1 << 14;
		new_CDCCR1== tmp_val;
		if(new_CDCCR1!= old_CDCCR1)
			flag_CDCCR1=1;
//		REG_ICDC_CDCCR1 = tmp_val;
	}else
	{
		tmp_val = REG_ICDC_CDCCR1;
		tmp_val &= ~(1 << 14);
		tmp_val |= 0 << 14;
		new_CDCCR1== tmp_val;
		if(new_CDCCR1 != old_CDCCR1)
			flag_CDCCR1=1;
//		REG_ICDC_CDCCR1 = tmp_val;
	}
	//set REG_ICDC_CDCCR2
	if(val >= 0 && val <= 16 )
	{
		tmp_val = REG_ICDC_CDCCR2;
		tmp_val &= ~(3 << 0);
		tmp_val |= 0 << 0;
		new_CDCCR2 = tmp_val;
		if(new_CDCCR2!= old_CDCCR2)
			flag_CDCCR2=1;
//		REG_ICDC_CDCCR2 = tmp_val;
	}
	else if(val >= 17 && val <= 20 )
	{
		tmp_val = REG_ICDC_CDCCR2;
		tmp_val &= ~(3 << 0);
		tmp_val |= 1 << 0;
		new_CDCCR2 = tmp_val;
		if(new_CDCCR2!= old_CDCCR2)
			flag_CDCCR2=1;
//		REG_ICDC_CDCCR2 = tmp_val;
	}
	else if(val >= 21 && val <= 25 )
	{
		tmp_val = REG_ICDC_CDCCR2;
		tmp_val &= ~(3 << 0);
		tmp_val |= 2 << 0;
		new_CDCCR2 = tmp_val;
		if(new_CDCCR2!= old_CDCCR2)
			flag_CDCCR2=1;
//		REG_ICDC_CDCCR2 = tmp_val;
	}
	else
	{
		tmp_val = REG_ICDC_CDCCR2;
		tmp_val &= ~(3 << 0);
		tmp_val |= 3 << 0;
		new_CDCCR2 = tmp_val;
		if(new_CDCCR2!= old_CDCCR2)
			flag_CDCCR2=1;
//		REG_ICDC_CDCCR2 = tmp_val;
	}

}
int pcm_ioctl(unsigned int cmd, unsigned long arg)
{
	long val;

	val=arg;

	switch (cmd) {
	case PCM_SET_SAMPLE_RATE:
		jz_audio_set_speed(arg);
		break;
	case PCM_SET_CHANNEL:
		jz_audio_set_channels(arg);
		break;
	case PCM_SET_FORMAT:
		jz_audio_set_format(arg);
		break;
	case PCM_SET_VOL:
		if (arg > 100)
			arg = 100;
		if(!IS_WRITE_PCM){
			i2s_codec_set_mic(arg);
		}else{
			i2s_codec_set_volume(arg);
		}

		break;
	case PCM_SET_HP_VOL:

		if(val < 0)
			val = 0;
		if(val > 31)
			val = 31;

		if (val >= 0 && val <= 31) {
			jz_audio_factor = Factor_table[val];
//			printf("jz_audio_factor:%d %d\n",jz_audio_factor,val);
			jz_audio_volume = val;

			SetVolumeReg(val);
//			printf("REG_ICDC_CDCCR1 : 0x%08x\n",REG_ICDC_CDCCR1);
//			printf("REG_ICDC_CDCCR2 : 0x%08x\n",REG_ICDC_CDCCR2);
		}
		break;
	case PCM_SET_PAUSE:
		//pause_flag=1;
		__aic_disable_replay();
		break;
	case PCM_SET_PLAY:
		//pause_flag=0;
		//pcm_start();
		__aic_enable_replay();
		break;
	case PCM_SET_REPLAY:
		jz_audio_reset();
		IS_WRITE_PCM=1;
		first_record_call = 0;
		in_codec_app1();
		break;
	case PCM_SET_RECORD:
		jz_audio_reset();
		IS_WRITE_PCM=0;
		first_record_call = 1;
		in_codec_app12();
		break;
	case PCM_RESET:
		dma_stop();
		__aic_flush_fifo();
		i2s_codec_reset();
		break;
	case PCM_GET_HP_VOL:
		return jz_audio_volume;
	case PCM_GET_VOL:
		return i2s_codec_get_volume();
	case PCM_GET_SPACE:
	{
		int dnum = elements_in_queue(&out_empty_queue);
		//printf("dnum = %d %d\n",dnum,dma_get_count(AUDIO_WRITE_DMA));
		if (dnum > 0)
			return (dnum) * PAGE_SIZE - dma_get_count(AUDIO_WRITE_DMA);
		else
			return 0;
	}					
	default:
		printf("pcm_ioctl:Unsupported I/O command: %08x\n", cmd);
		return -1;
	}
	return 0;
}

int pcm_can_write(void)
{
	if (elements_in_queue(&out_empty_queue) > 0)
		return 1;
	return 0;
}

int pcm_can_read(void)
{
	if (elements_in_queue(&in_full_queue) > 0)
		return 1;
	return 0;
}

int pcm_read(char *buffer, int count)
{
	int id, ret = 0, left_count, copy_count, cnt = 0,ps;
	U8 err;
	// printf("buffer:0x%x,count = 0x%x\r\n",buffer,count);
	if (count <= 0)
		return -1;
	if(jz_audio_format==AFMT_U8)
	{
		ps=PAGE_SIZE/2;
	}
	else{
		ps=PAGE_SIZE;
	}
	if (count < ps) {
		copy_count = count;//*16 / (jz_audio_channels * jz_audio_format);
	} else
		copy_count =  ps;

	//printf("copy_count = 0x%x,PAGE_SIZE = 0x%x\r\n",copy_count,PAGE_SIZE);
	
	left_count = count;

	if (first_record_call) {

		first_record_call = 0;
		if ((id = get_buffer_id(&in_empty_queue)) >= 0) {
			put_buffer_id(&in_busy_queue, id);
			in_dma_buf_data_count[id] = copy_count;//(jz_audio_format / 8);
			//in_dma_buf_data_count[id] = copy_count * (jz_audio_format / 8);
			__i2s_enable_receive_dma();
			__i2s_enable_record();

			dma_start(AUDIO_READ_DMA,
				  PHYADDR(AIC_DR), in_dma_pbuf[id],
				  in_dma_buf_data_count[id]);

			OSSemPend(rx_sem, 0, &err);

		} 
	}

	while (left_count > 0) {

		if (elements_in_queue(&in_full_queue) <= 0)
			OSSemPend(rx_sem, 0, &err);

		if ((id = get_buffer_id(&in_full_queue)) >= 0) {
			/*
			 * FIXME: maybe the buffer is too small.
			 */

			cnt = record_filler((unsigned long)buffer+ret, copy_count, id);
            //printf("ret = 0x%x,copy_count = 0x%x\r\n",ret,copy_count);
			
			put_buffer_id(&in_empty_queue, id);
		}

		if (elements_in_queue(&in_busy_queue) == 0) {
			if ((id = get_buffer_id(&in_empty_queue)) >= 0) {
				put_buffer_id(&in_busy_queue, id);
				in_dma_buf_data_count[id] = copy_count; //(jz_audio_format / 8);
				//in_dma_buf_data_count[id] = copy_count * (jz_audio_format / 8);
				__i2s_enable_receive_dma();
				__i2s_enable_record();

				dma_start(AUDIO_READ_DMA,
					  PHYADDR(AIC_DR), in_dma_pbuf[id],
					  in_dma_buf_data_count[id]);
			}
		}

		if (ret + cnt > count)
			cnt = count - ret;

		ret += cnt;
		left_count -= cnt;
	}

	return ret;
}

int pcm_write(char *buffer, int count)
{
	int id, ret = 0, left_count, copy_count;
//	printf("pcm_write %x %x\n",count, PAGE_SIZE);
	if(count >= PAGE_SIZE)
	{	
		left_count = count / PAGE_SIZE * PAGE_SIZE;
		while (left_count > 0)
		{
	
			if (elements_in_queue(&out_empty_queue) == 0)
		{
		    printf("pf\n");
				return ret;
	    }
		
			if ((id = get_buffer_id(&out_empty_queue)) >= 0) {
		
				replay_filler( (unsigned long)buffer + ret, PAGE_SIZE, id);
				put_buffer_id(&out_full_queue, id);
				
			} 
	 
			left_count -= PAGE_SIZE;
			ret += PAGE_SIZE;
	
			if(elements_in_queue(&out_busy_queue) == 0) {
				if ((id = get_buffer_id(&out_full_queue)) >= 0) {
					put_buffer_id(&out_busy_queue, id);
	
					__i2s_enable_transmit_dma();
					__i2s_enable_replay();
	
					dma_start(AUDIO_WRITE_DMA,
						  out_dma_pbuf[id], PHYADDR(AIC_DR),
						  out_dma_buf_data_count[id]);
				}
			}
		}
	}else
	{
		if (elements_in_queue(&out_empty_queue) == 0)
		{
		    printf("f\n");
				return ret;
	    }
		//for dma data translate
		copy_count = (jz_audio_format == AFMT_S16_LE) ? count / 16*16 : count/8*8;
//		printf("copy_cnt:%d count:%d unsigned long:%d %d %d\n",copy_count,count,sizeof(unsigned long),sizeof(signed long),sizeof(char));
		if ((id = get_buffer_id(&out_empty_queue)) >= 0) {
				replay_filler( (unsigned long)buffer, copy_count, id);
				put_buffer_id(&out_full_queue, id);
				
			}  
		if(elements_in_queue(&out_busy_queue) == 0) {
				if ((id = get_buffer_id(&out_full_queue)) >= 0) {
					put_buffer_id(&out_busy_queue, id);
	
					__i2s_enable_transmit_dma();
					__i2s_enable_replay();
	
					dma_start(AUDIO_WRITE_DMA,
						  out_dma_pbuf[id], PHYADDR(AIC_DR),
						  out_dma_buf_data_count[id]);
				}	
			}
			ret = copy_count;		
	}	
	return ret;
}

static unsigned char dma_pool[PAGE_SIZE * QUEUE_MAX ] __attribute__((aligned(32)));
extern void in_codec_app1(void);
extern void in_codec_app11(void);
int pcm_init()
{
	int i;
	unsigned char *p = dma_pool;
	p = (unsigned char *)KSEG1ADDR((unsigned int )p);
	SetAudioVolume(0);
	jz_i2s_initHw();
	if (jz_i2s_codec_init() < 0)
		return -1;
	tx_sem = OSSemCreate(0);
	rx_sem = OSSemCreate(0);
	pre_tx_sem = OSSemCreate(0);
	pause_sem = OSSemCreate(0);

		
	for(i=0;i < 64;i++) {
		save_last_samples[i].left = 0;
		save_last_samples[i].right = 0;
	}
	
	dma_request(AUDIO_READ_DMA, jz_i2s_record_dma_irq, 0,
		    DMAC_DCCSR_DAM|DMAC_DCCSR_RDIL_IGN,
		    DMAC_DRSR_RS_AICIN);

	dma_request(AUDIO_WRITE_DMA, jz_i2s_replay_dma_irq, 0,
		    DMAC_DCCSR_SAM | DMAC_DCCSR_RDIL_IGN |  DMAC_DCMD_DWDH_16,
		    DMAC_DRSR_RS_AICOUT);//transmite fifo empty request
	
		    //DMAC_DRSR_RS_AUTO);
     
	jz_audio_reset();
   
	
	in_empty_queue.count = QUEUE_MAX;
	out_empty_queue.count = QUEUE_MAX;

	for (i=0;i<QUEUE_MAX;i++) {
		in_empty_queue.id[i] = i;
		out_empty_queue.id[i] = i;
	}

	in_full_queue.count = 0;
	in_busy_queue.count = 0;
	out_busy_queue.count = 0;
	out_full_queue.count = 0;

	/* Aligned in PAGE_SIZE */
	for (i = 0; i < QUEUE_MAX; i++) {
		out_dma_buf[i] = (unsigned long)p;
		out_dma_pbuf[i] = PHYADDR((unsigned int)out_dma_buf[i]);
		in_dma_buf[i] = (unsigned long)p;
		//p += PAGE_SIZE*16;
		in_dma_pbuf[i] = PHYADDR((unsigned int)in_dma_buf[i]);
		p += PAGE_SIZE;
	}

	REG_AIC_SR = 0x00000008;
	REG_AIC_FR |= 0x10; 
	__i2s_enable();

#if 0
	pcm_ioctl(PCM_SET_SAMPLE_RATE, 44100); //48000,44100
	pcm_ioctl(PCM_SET_FORMAT, AFMT_S16_LE);
	pcm_ioctl(PCM_SET_CHANNEL, 1);
	pcm_ioctl(PCM_SET_VOL, 100);	/* 100% */
#endif
    
    pcm_ioctl(PCM_SET_SAMPLE_RATE, 44100);
    pcm_ioctl(PCM_SET_CHANNEL, 2);
    pcm_ioctl(PCM_SET_FORMAT, AFMT_S16_LE);
    
	pcm_ioctl(PCM_SET_REPLAY,0);
	pcm_ioctl(PCM_SET_HP_VOL,10);	/* 0-32 */

#if 0	
	printf("REG_GPIO_PXFUN(3):0x%08x\n",REG_GPIO_PXFUN(3));
	printf("REG_GPIO_PXSEL(3):0x%08x\n",REG_GPIO_PXSEL(3));

	printf("REG_CPM_SCR:0x%08x\n",REG_CPM_SCR);
	printf("REG_CPM_CPCCR:0x%08x\n",REG_CPM_CPCCR);
	printf("REG_CPM_I2SCDR:0x%08x\n",REG_CPM_I2SCDR);
	printf("REG_AIC_FR:0x%08x\n",REG_AIC_FR);
	printf("REG_AIC_CR:0x%08x\n",REG_AIC_CR);
	printf("REG_AIC_SR:0x%08x\n",REG_AIC_SR);
	printf("REG_AIC_I2SCR:0x%08x\n",REG_AIC_I2SCR);
	printf("REG_AIC_I2SSR:0x%08x\n",REG_AIC_I2SSR);
	printf("REG_AIC_I2SDIV:0x%08x\n",REG_AIC_I2SDIV);
	printf("REG_ICDC_CDCCR1:0x%08x\n",REG_ICDC_CDCCR1);
	printf("REG_ICDC_CDCCR2:0x%08x\n",REG_ICDC_CDCCR2);
#endif
	return 0;
}

#if (DM==1)
void  codec_poweron(void)
{
}
void  codec_poweroff(void)
{
}
int  codec_preconvert(void)
{
	__i2s_set_transmit_trigger(2);
#if 0
        u32 ret;
	unsigned long flags;
	flags = spin_lock_irqsave();
//        ret = out_busy_queue.count;
	ret = (*(volatile u32*)0xb0020014 >>8) & 0x3f; 
	spin_unlock_irqrestore(flags);
	   	
	//       while((((*(volatile u32*)0xb0020014) >> 8) & 0x3f) < 0x16);
		//  printf("tfl = %d\r\n",(((*(volatile u32*)0xb0020014) >> 8) & 0x3f));
       
	// printf("ret = %d",ret);
	if( ret > 0x14)
//        if(ret > 0)
#endif	
	return 1;
	//  return 0;
}
void  codec_convert(void)
{
       preconvert_control=0;
       __i2s_set_transmit_trigger(16 - 2);
}
void  mng_init_codec(void)
{
	struct dm_jz4740_t codec_dm;
	codec_dm.name = "Codec driver";	
	codec_dm.init = pcm_init;  
	codec_dm.poweron =  codec_poweron;
	codec_dm.poweroff =  codec_poweroff;
	codec_dm.convert =  codec_convert;
	codec_dm.preconvert =  codec_preconvert;
	dm_register(0,&codec_dm);
}
#endif
void jz_memmove_handler(unsigned int arg)
{
	REG_DMAC_DCCSR(arg) = 0;
}

void codecs_memmove(void *target,void *source,unsigned int len)
{
  //int ch = AUDIO_READ_DMA;
  int ch = 5;
  if(len == 0)
  	return;
 	printf("target = 0x%08x source = 0x%08x\r\n",target,source,len);
#if 0
 	#define PRE_MOVE_SIZE 2048
 	if(len > PRE_MOVE_SIZE)
 	{	
 		memcpy(target,source,PRE_MOVE_SIZE);
 		target += PRE_MOVE_SIZE;
 		source += PRE_MOVE_SIZE;
 		len -= PRE_MOVE_SIZE;
 	}else
 	{
 		memcpy(target,source,len);
 		return;
 	}	
	if(((unsigned int)source < 0xa0000000))
			dma_cache_wback_inv((unsigned long)source - 31, len+32);
	
	
	if(((unsigned int)target < 0xa0000000))
		dma_cache_wback_inv((unsigned long)target - 31, len+32);
#endif
	jz_flush_dcache();
	jz_sync();
	while (REG_DMAC_DTCR(ch));
	REG_DMAC_DSAR(ch) = PHYADDR((unsigned long)source);       
	REG_DMAC_DTAR(ch) = PHYADDR((unsigned long)target);       
	REG_DMAC_DTCR(ch) = len / 4;	            	    
  REG_DMAC_DCMD(ch) = DMAC_DCMD_SAI| DMAC_DCMD_DAI | DMAC_DCMD_SWDH_32 |
												  DMAC_DCMD_DWDH_32|DMAC_DCMD_DS_32BIT | DMAC_DCMD_TIE;
	REG_DMAC_DRSR(ch) = DMAC_DRSR_RS_AUTO;	/* DMA request type */											  
  REG_DMAC_DCCSR(ch) = DMAC_DCCSR_EN | DMAC_DCCSR_NDES;	
  request_irq(IRQ_DMA_0 + ch, jz_memmove_handler,ch);
		    
  //REG_DMAC_DCCSR(ch) = 0;
// memcpy(target,source,len);	
}

#define LED_PIN (32 * 3 + 28)    
void    gsetled()
{
    __gpio_set_pin(LED_PIN);
}

void    gclrled()
{
    __gpio_clear_pin(LED_PIN);
}

