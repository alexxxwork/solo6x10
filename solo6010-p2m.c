/*
 * Copyright (C) 2010 Ben Collins <bcollins@bluecherry.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>

#include "solo6010.h"

// #define SOLO_TEST_P2M

int solo_p2m_dma(struct solo6010_dev *solo_dev, int id, int wr,
		 void *sys_addr, u32 ext_addr, u32 size)
{
	struct solo_p2m_dev *p2m_dev;
	unsigned int timeout = 0;
	dma_addr_t dma_addr;

	if (id >= SOLO_NR_P2M)
                return -EINVAL;

	p2m_dev = &solo_dev->p2m_dev[id];

	down(&p2m_dev->sem);

	dma_addr = pci_map_single(solo_dev->pdev, sys_addr, size,
				  wr ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

start_dma:
	INIT_COMPLETION(p2m_dev->completion);
	p2m_dev->error = 0;
	solo_reg_write(solo_dev, SOLO_P2M_TAR_ADR(id), dma_addr);
	solo_reg_write(solo_dev, SOLO_P2M_EXT_ADR(id), ext_addr);
	solo_reg_write(solo_dev, SOLO_P2M_EXT_CFG(id),
		       SOLO_P2M_COPY_SIZE(size >> 2));
	solo_reg_write(solo_dev, SOLO_P2M_CONTROL(id),
		       SOLO_P2M_BURST_SIZE(SOLO_P2M_BURST_256) |
		       (wr ? SOLO_P2M_WRITE : 0) | SOLO_P2M_TRANS_ON);

	timeout = wait_for_completion_timeout(&p2m_dev->completion, HZ);

	solo_reg_write(solo_dev, SOLO_P2M_CONTROL(id), 0);

	/* XXX Really looks to me like we will get stuck here if a
	 * real PCI P2M error occurs */
	if (p2m_dev->error)
		goto start_dma;

	pci_unmap_single(solo_dev->pdev, dma_addr, size,
			 wr ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE);

	up(&p2m_dev->sem);

	return (timeout == 0) ? -EAGAIN : 0;
}

#ifdef SOLO_TEST_P2M
static int p2m_test(struct solo6010_dev *solo_dev, u8 id, u32 base, int size,
		    unsigned char a)
{
	u8 *wr_buf;
	u8 *rd_buf;
	int i;
	int err_cnt = 0;

	wr_buf = kmalloc(size, GFP_KERNEL);
	if (!wr_buf)
		return -1;

	rd_buf = kmalloc(size, GFP_KERNEL);
	if (!rd_buf) {
		kfree(wr_buf);
		return -1;
	}

	memset(wr_buf, a, size);
	memset(rd_buf, a + 1, size);

	solo_p2m_dma(solo_dev, id, 1, wr_buf, base, size);
	solo_p2m_dma(solo_dev, id, 0, rd_buf, base, size);

	for (i = 0; i < size; i++)
		if (wr_buf[i] != rd_buf[i])
			err_cnt++;

	kfree(wr_buf);
	kfree(rd_buf);

	return err_cnt;
}

static void run_p2m_test(struct solo6010_dev *solo_dev)
{
	int i, j, errs = 0;
	u8 a;

	for (j = 0; j < SOLO_NR_P2M; j++)
		for (i = 0, a = 0xff; i < 256; i++, a--)
			errs += p2m_test(solo_dev, j, 0, 32, a);

	printk(KERN_WARNING "%s: Found %d errors during p2m test\n",
	       SOLO6010_NAME, errs);

	return;
}
#else
#define run_p2m_test(__solo)	do{}while(0)
#endif

void solo_p2m_isr(struct solo6010_dev *solo_dev, int id)
{
	solo_reg_write(solo_dev, SOLO_IRQ_STAT, SOLO_IRQ_P2M(id));
	complete(&solo_dev->p2m_dev[id].completion);
}

void solo_p2m_error_isr(struct solo6010_dev *solo_dev, u32 status)
{
	struct solo_p2m_dev *p2m_dev;
	int i;

	if (!(status & SOLO_PCI_ERR_P2M))
		return;

	for (i = 0; i < SOLO_NR_P2M; i++) {
		p2m_dev = &solo_dev->p2m_dev[i];
		p2m_dev->error = 1;
		solo_reg_write(solo_dev, SOLO_P2M_CONTROL(i), 0);
		complete(&p2m_dev->completion);
	}
}

void solo_p2m_exit(struct solo6010_dev *solo_dev)
{
	int i;

	for (i = 0; i < SOLO_NR_P2M; i++)
		solo6010_irq_off(solo_dev, SOLO_IRQ_P2M(i));
}

int solo_p2m_init(struct solo6010_dev *solo_dev)
{
	struct solo_p2m_dev *p2m_dev;
	int i;

	for (i = 0; i < SOLO_NR_P2M; i++) {
		p2m_dev = &solo_dev->p2m_dev[i];

		init_MUTEX(&p2m_dev->sem);
		init_completion(&p2m_dev->completion);

		solo_reg_write(solo_dev, SOLO_P2M_DES_ADR(i),
			       __pa(p2m_dev->desc));

		solo_reg_write(solo_dev, SOLO_P2M_CONTROL(i), 0);
		solo_reg_write(solo_dev, SOLO_P2M_CONFIG(i),
			       SOLO_P2M_CSC_16BIT_565 |
			       SOLO_P2M_DMA_INTERVAL(0) |
			       SOLO_P2M_PCI_MASTER_MODE);
		solo6010_irq_on(solo_dev, SOLO_IRQ_P2M(i));
	}

	run_p2m_test(solo_dev);

	return 0;
}
