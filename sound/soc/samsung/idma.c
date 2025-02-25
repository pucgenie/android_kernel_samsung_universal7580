/*
 * sound/soc/samsung/idma.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * I2S0's Internal DMA driver
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "i2s.h"
#include "idma.h"
#include "dma.h"
#include "i2s-regs.h"
#ifdef CONFIG_SND_SAMSUNG_ALP
#include "srp_alp/srp_alp.h"
#endif

#define ST_RUNNING	(1<<0)
#define ST_OPENED	(1<<1)

#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
#define DMA_SRAM	0
#define DMA_DRAM	1

bool dma_ram;
#endif

static const struct snd_pcm_hardware idma_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		    SNDRV_PCM_INFO_BLOCK_TRANSFER |
		    SNDRV_PCM_INFO_MMAP |
		    SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		    SNDRV_PCM_FMTBIT_U16_LE |
		    SNDRV_PCM_FMTBIT_S24_LE |
		    SNDRV_PCM_FMTBIT_U24_LE |
		    SNDRV_PCM_FMTBIT_U8 |
		    SNDRV_PCM_FMTBIT_S8,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = MAX_IDMA_BUFFER,
	.period_bytes_min = 128,
	.period_bytes_max = MAX_IDMA_PERIOD,
	.periods_min = 1,
	.periods_max = 4,
};

struct idma_ctrl {
	spinlock_t	lock;
	int		state;
	dma_addr_t	start;
	dma_addr_t	pos;
	dma_addr_t	end;
	dma_addr_t	period;
	dma_addr_t	periodsz;
	void		*token;
	void		(*cb)(void *dt, int bytes_xfer);
};

static struct idma_info {
	spinlock_t		lock;
	void __iomem		*regs;
	dma_addr_t		lp_tx_addr;
#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	void			*dma_area;
	struct snd_dma_buffer	sram_buf;
	struct snd_dma_buffer	dram_buf;
#endif
	u32 sample_bit;
} idma;

int check_dram_status(void)
{
#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	return dma_ram;
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(check_dram_status);

static int get_sample_bit(struct snd_pcm_hw_params *params)
{
	/* format */
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_U16_LE:
		return 16;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_U24_LE:
		return 24;
	case SNDRV_PCM_FORMAT_U8:
	case SNDRV_PCM_FORMAT_S8:
		return 8;
	default:
		return 16;
	}
}

static void idma_getpos(dma_addr_t *res, struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd = runtime->private_data;
	u32 maxcnt = snd_pcm_lib_buffer_bytes(substream) >> 2;
	u32 trncnt = readl(idma.regs + I2STRNCNT) & 0xffffff;

	/*
	 * When bus is busy, I2STRNCNT could be increased without dma transfer
	 * in rare cases.
	 */
	if (prtd->state == ST_RUNNING)
		trncnt = trncnt == 0 ? maxcnt - 1 : trncnt - 1;

	if (idma.sample_bit == 24)
		*res = ((trncnt << 2) / prtd->periodsz) * prtd->periodsz;
	else
		*res = trncnt << 2;
}

static int idma_enqueue(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd = substream->runtime->private_data;
	u32 val;

	spin_lock(&prtd->lock);
	prtd->token = (void *) substream;
	spin_unlock(&prtd->lock);

	/* Internal DMA Level0 Interrupt Address */
	val = idma.lp_tx_addr + prtd->periodsz;
	writel(val, idma.regs + I2SLVL0ADDR);

	/* Start address0 of I2S internal DMA operation. */
	val = idma.lp_tx_addr;
	writel(val, idma.regs + I2SSTR0);

	/*
	 * Transfer block size for I2S internal DMA.
	 * Should decide transfer size before start dma operation
	 */
	val = readl(idma.regs + I2SSIZE);
	val &= ~(I2SSIZE_TRNMSK << I2SSIZE_SHIFT);
	val |= (((runtime->dma_bytes >> 2) &
			I2SSIZE_TRNMSK) << I2SSIZE_SHIFT);
	writel(val, idma.regs + I2SSIZE);

	val = readl(idma.regs + I2SAHB);
	val |= AHB_INTENLVL0;
	writel(val, idma.regs + I2SAHB);

	return 0;
}

static void idma_setcallbk(struct snd_pcm_substream *substream,
				void (*cb)(void *, int))
{
	struct idma_ctrl *prtd = substream->runtime->private_data;

	spin_lock(&prtd->lock);
	prtd->cb = cb;
	spin_unlock(&prtd->lock);
}

static void idma_control(int op)
{
	u32 val = readl(idma.regs + I2SAHB);

	spin_lock(&idma.lock);

	switch (op) {
	case LPAM_DMA_START:
		val |= (AHB_INTENLVL0 | AHB_DMAEN);
		break;
	case LPAM_DMA_STOP:
		val &= ~(AHB_INTENLVL0 | AHB_DMAEN);
		break;
	default:
		spin_unlock(&idma.lock);
		return;
	}

	writel(val, idma.regs + I2SAHB);
	spin_unlock(&idma.lock);
}

static void idma_done(void *id, int bytes_xfer)
{
	struct snd_pcm_substream *substream = id;
	struct idma_ctrl *prtd = substream->runtime->private_data;

	if (prtd && (prtd->state & ST_RUNNING))
		snd_pcm_period_elapsed(substream);
}

static int idma_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd = substream->runtime->private_data;
	u32 mod = readl(idma.regs + I2SMOD);
	u32 ahb = readl(idma.regs + I2SAHB);
#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	struct snd_dma_buffer *buf;
	bool sram_avail = false;
#endif

	ahb |= (AHB_DMARLD | AHB_INTMASK);
	mod |= MOD_TXS_IDMA;
	writel(ahb, idma.regs + I2SAHB);
	writel(mod, idma.regs + I2SMOD);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
	runtime->dma_bytes = params_buffer_bytes(params);

#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	if (runtime->dma_bytes <= 32 * 1024) {
		sram_avail = true;
		srp_check(SRP_ENABLE);		/* try to enable srp */
	} else {
		if (srp_check(SRP_DISABLE)) {	/* try to disable srp */
			sram_avail = true;
			srp_check(SRP_FIRM_BROKEN);	/* firmware will be erased */
		}
	}

	if (sram_avail) {		/* sram buffer used */
		buf = &idma.sram_buf;
		idma.lp_tx_addr = idma.sram_buf.addr;
		idma.dma_area   = idma.sram_buf.area;
		dma_ram = DMA_SRAM;
	} else {			/* dram buffer used */
		buf = &idma.dram_buf;
		idma.lp_tx_addr = idma.dram_buf.addr;
		idma.dma_area   = idma.dram_buf.area;
		dma_ram = DMA_DRAM;
	}

	prtd->start = prtd->pos = buf->addr;
	prtd->period = params_periods(params);
	prtd->periodsz = params_period_bytes(params);
	prtd->end = prtd->start + runtime->dma_bytes;
#else
	prtd->start = prtd->pos = runtime->dma_addr;
	prtd->period = params_periods(params);
	prtd->periodsz = params_period_bytes(params);
	prtd->end = runtime->dma_addr + runtime->dma_bytes;
#endif

	idma_setcallbk(substream, idma_done);

	idma.sample_bit = get_sample_bit(params);

	pr_info("I:%s:DmaAddr=@%x Total=%d PrdSz=%d #Prds=%d dma_area=0x%x\n",
		(substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ? "P" : "C",
			prtd->start, runtime->dma_bytes,  prtd->periodsz,
			prtd->period, (unsigned int)runtime->dma_area);

	return 0;
}

static int idma_hw_free(struct snd_pcm_substream *substream)
{
	snd_pcm_set_runtime_buffer(substream, NULL);

	return 0;
}

static int idma_prepare(struct snd_pcm_substream *substream)
{
	struct idma_ctrl *prtd = substream->runtime->private_data;

	prtd->pos = prtd->start;

	/* flush the DMA channel */
	idma_control(LPAM_DMA_STOP);
	idma_enqueue(substream);

	return 0;
}

static int idma_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct idma_ctrl *prtd = substream->runtime->private_data;
	int ret = 0;

	spin_lock(&prtd->lock);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		prtd->state |= ST_RUNNING;
		idma_control(LPAM_DMA_START);
		break;

	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		prtd->state &= ~ST_RUNNING;
		idma_control(LPAM_DMA_STOP);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	spin_unlock(&prtd->lock);

	return ret;
}

static snd_pcm_uframes_t
	idma_pointer(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd = runtime->private_data;
	dma_addr_t res;

	spin_lock(&prtd->lock);

	idma_getpos(&res, substream);

	spin_unlock(&prtd->lock);

	return bytes_to_frames(substream->runtime, res);
}

#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
static int idma_copy(struct snd_pcm_substream *substream, int channel,
	snd_pcm_uframes_t pos, void __user *buf, snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	char *hwbuf = idma.dma_area + frames_to_bytes(runtime, pos);

	if (copy_from_user(hwbuf, buf, frames_to_bytes(runtime, count)))
		return -EFAULT;

	return 0;
}
#else
#define idma_copy	NULL
#endif

static int idma_mmap(struct snd_pcm_substream *substream,
	struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long size, offset;
	int ret;

	/* From snd_pcm_lib_mmap_iomem */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_flags |= VM_IO;
	size = vma->vm_end - vma->vm_start;
	offset = vma->vm_pgoff << PAGE_SHIFT;
	if (offset > runtime->dma_bytes || size > runtime->dma_bytes - offset)
		return -EINVAL;
	ret = io_remap_pfn_range(vma, vma->vm_start,
			(runtime->dma_addr + offset) >> PAGE_SHIFT,
			size, vma->vm_page_prot);

	return ret;
}

static irqreturn_t iis_irq(int irqno, void *dev_id)
{
	struct idma_ctrl *prtd = (struct idma_ctrl *)dev_id;
	u32 iiscon, iisahb, val, addr;

	iisahb  = readl(idma.regs + I2SAHB);
	iiscon  = readl(idma.regs + I2SCON);

	val = (iisahb & AHB_LVL0INT) ? AHB_CLRLVL0INT : 0;

	if (val) {
		iisahb |= val;
		writel(iisahb, idma.regs + I2SAHB);

		addr = readl(idma.regs + I2SLVL0ADDR) - idma.lp_tx_addr;
		addr += prtd->periodsz;
		addr %= (prtd->end - prtd->start);
		addr += idma.lp_tx_addr;

		writel(addr, idma.regs + I2SLVL0ADDR);

		if (prtd->cb)
			prtd->cb(prtd->token, prtd->period);
	}

	return IRQ_HANDLED;
}

static int idma_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &idma_hardware);

	prtd = kzalloc(sizeof(struct idma_ctrl), GFP_KERNEL);
	if (prtd == NULL)
		return -ENOMEM;

        /* Clear AHB register */
	writel(0, idma.regs + I2SAHB);

	ret = request_irq(IRQ_I2S0, iis_irq, 0, "i2s", prtd);
	if (ret < 0) {
		pr_err("fail to claim i2s irq , ret = %d\n", ret);
		kfree(prtd);
		return ret;
	}

	spin_lock_init(&prtd->lock);

	runtime->private_data = prtd;

	return 0;
}

static int idma_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct idma_ctrl *prtd = runtime->private_data;

	free_irq(IRQ_I2S0, prtd);

	if (!prtd)
		pr_err("idma_close called with prtd == NULL\n");

	kfree(prtd);

	return 0;
}

static struct snd_pcm_ops idma_ops = {
	.open		= idma_open,
	.close		= idma_close,
	.ioctl		= snd_pcm_lib_ioctl,
	.trigger	= idma_trigger,
	.pointer	= idma_pointer,
	.copy		= idma_copy,
	.mmap		= idma_mmap,
	.hw_params	= idma_hw_params,
	.hw_free	= idma_hw_free,
	.prepare	= idma_prepare,
};

static void idma_free(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;

	substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	if (!substream)
		return;

	buf = &substream->dma_buffer;
	if (!buf->area)
		return;

	iounmap(buf->area);

	buf->area = NULL;
	buf->addr = 0;
#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	buf = &idma.dram_buf;

	dma_free_writecombine(pcm->card->dev, buf->bytes,
				      buf->area, buf->addr);
	buf->area = NULL;
#endif

}

static int preallocate_idma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;

	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;

	/* Assign PCM buffer pointers */
	buf->dev.type = SNDRV_DMA_TYPE_CONTINUOUS;
	buf->addr = idma.lp_tx_addr;
	buf->bytes = idma_hardware.buffer_bytes_max;
	buf->area = (unsigned char *)ioremap(buf->addr, buf->bytes);
	if (!buf->area)
		return -ENOMEM;
#ifdef CONFIG_SND_SAMSUNG_USE_IDMA_DRAM
	/* info sram_buf */
	memcpy(&idma.sram_buf, buf, sizeof(struct snd_dma_buffer));

	/* info dram_buf */
	memcpy(&idma.dram_buf, buf, sizeof(struct snd_dma_buffer));
	buf = &idma.dram_buf;
	buf->area = dma_alloc_writecombine(pcm->card->dev, buf->bytes,
					   &buf->addr, GFP_KERNEL);
#endif

	return 0;
}

static u64 idma_mask = DMA_BIT_MASK(32);

static int idma_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	int ret = 0;

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &idma_mask;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ret = preallocate_idma_buffer(pcm,
				SNDRV_PCM_STREAM_PLAYBACK);
	}

	return ret;
}

void idma_reg_addr_init(void __iomem *regs, dma_addr_t addr)
{
	spin_lock_init(&idma.lock);
	idma.regs = regs;
	idma.lp_tx_addr = addr;
}
EXPORT_SYMBOL_GPL(idma_reg_addr_init);

static struct snd_soc_platform_driver asoc_idma_platform = {
	.ops = &idma_ops,
	.pcm_new = idma_new,
	.pcm_free = idma_free,
};

int asoc_idma_platform_register(struct device *dev)
{
	return snd_soc_register_platform(dev, &asoc_idma_platform);
}
EXPORT_SYMBOL_GPL(asoc_idma_platform_register);

void asoc_idma_platform_unregister(struct device *dev)
{
	snd_soc_unregister_platform(dev);
}
EXPORT_SYMBOL_GPL(asoc_idma_platform_unregister);

MODULE_AUTHOR("Jaswinder Singh, <jassisinghbrar@gmail.com>");
MODULE_DESCRIPTION("Samsung ASoC IDMA Driver");
MODULE_LICENSE("GPL");
