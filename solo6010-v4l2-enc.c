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
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-common.h>

#include "solo6010.h"
#include "solo6010-tw28.h"
#include "solo6010-jpeg.h"

#define MIN_VID_BUFFERS		4
#define FRAME_BUF_SIZE		(512 * 1024)
#define MP4_QS			16

static int solo_enc_thread(void *data);

extern unsigned video_nr;

struct solo_enc_fh {
	struct			solo_enc_dev *enc;
	u32			fmt;
	u16			rd_idx;
	u8			enc_on;
	struct videobuf_queue	vidq;
	struct list_head	vidq_active;
	struct task_struct	*kthread;
};

static unsigned char vid_vop_header[] = {
	0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20,
	0x02, 0x48, 0x05, 0xc0, 0x00, 0x40, 0x00, 0x40,
	0x00, 0x40, 0x00, 0x80, 0x00, 0x97, 0x53, 0x04,
	0x1f, 0x4c, 0x58, 0x10, 0x78, 0x51, 0x18, 0x3e,
};

/*
 * Things we can change around:
 *
 * byte  10,        4-bits 01111000                   aspect
 * bytes 21,22,23  16-bits 000x1111 11111111 1111x000 fps/res
 * bytes 23,24,25  15-bits 00000n11 11111111 11111x00 interval
 * bytes 25,26,27  13-bits 00000x11 11111111 111x0000 width
 * bytes 27,28,29  13-bits 000x1111 11111111 1x000000 height
 * byte  29         1-bit  0x100000                   interlace
 */

/* For aspect */
#define XVID_PAR_43_PAL		2
#define XVID_PAR_43_NTSC	3

static const u32 solo_user_ctrls[] = {
	V4L2_CID_BRIGHTNESS,
	V4L2_CID_CONTRAST,
	V4L2_CID_SATURATION,
	V4L2_CID_HUE,
	0
};

static const u32 solo_mpeg_ctrls[] = {
	V4L2_CID_MPEG_VIDEO_ENCODING,
	V4L2_CID_MPEG_VIDEO_GOP_SIZE,
	0
};

static const u32 *solo_ctrl_classes[] = {
	solo_user_ctrls,
	solo_mpeg_ctrls,
	NULL
};

struct vop_header {
	/* VD_IDX0 */
	u32 size:20, sync_start:1, page_stop:1, vop_type:2, channel:4,
		nop0:1, source_fl:1, interlace:1, progressive:1;

	/* VD_IDX1 */
	u32 vsize:8, hsize:8, frame_interop:1, nop1:7, win_id:4, scale:4;

	/* VD_IDX2 */
	u32 base_addr:16, nop2:15, hoff:1;

	/* VD_IDX3 - User set macros */
	u32 sy:12, sx:12, nop3:1, hzoom:1, read_interop:1, write_interlace:1,
		scale_mode:4;

	/* VD_IDX4 - User set macros continued */
	u32 write_page:8, nop4:24;

	/* VD_IDX5 */
	u32 next_code_addr;

	u32 end_nops[10];
} __attribute__((packed));

/* Should be called with solo_enc->lock held */
static void solo_update_mode(struct solo_enc_dev *solo_enc)
{
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;

	/* XXX GOP needs to be handled better */
	solo_enc->gop = max(solo_dev->fps / solo_enc->interval, 1);
	solo_enc->interlaced = (solo_enc->mode & 0x08) ? 1 : 0;
	solo_enc->bw_weight = max(solo_dev->fps / solo_enc->interval, 1);

	switch (solo_enc->mode) {
	case SOLO_ENC_MODE_CIF:
		solo_enc->width = solo_dev->video_hsize >> 1;
		solo_enc->height = solo_dev->video_vsize;
		break;
	case SOLO_ENC_MODE_D1:
		solo_enc->width = solo_dev->video_hsize;
		solo_enc->height = solo_dev->video_vsize << 1;
		solo_enc->bw_weight <<= 2;
		break;
	default:
		WARN(1, "mode is unknown");
	}
}

/* Should be called with solo_enc->lock held */
static int solo_enc_on(struct solo_enc_fh *fh)
{
	struct solo_enc_dev *solo_enc = fh->enc;
	u8 ch = solo_enc->ch;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	u8 interval;

	if (fh->enc_on)
		return 0;

	solo_update_mode(solo_enc);

	/* Make sure to bw check on first reader */
	if (!atomic_read(&solo_enc->readers)) {
		if (solo_enc->bw_weight > solo_dev->enc_bw_remain)
			return -EBUSY;
		else
			solo_dev->enc_bw_remain -= solo_enc->bw_weight;
	}

	fh->kthread = kthread_run(solo_enc_thread, fh, SOLO6010_NAME "_enc");

	if (IS_ERR(fh->kthread))
		return PTR_ERR(fh->kthread);

	fh->enc_on = 1;
	fh->rd_idx = solo_enc->solo_dev->enc_wr_idx;

	if (atomic_inc_return(&solo_enc->readers) > 1)
		return 0;

	/* Disable all encoding for this channel */
	solo_reg_write(solo_dev, SOLO_CAP_CH_SCALE(ch), 0);
	solo_reg_write(solo_dev, SOLO_CAP_CH_COMP_ENA_E(ch), 0);

	/* Common for both std and ext encoding */
	solo_reg_write(solo_dev, SOLO_VE_CH_INTL(ch),
		       solo_enc->interlaced ? 1 : 0);

	if (solo_enc->interlaced)
		interval = solo_enc->interval - 1;
	else
		interval = solo_enc->interval;

	/* Standard encoding only */
	solo_reg_write(solo_dev, SOLO_VE_CH_GOP(ch), solo_enc->gop);
	solo_reg_write(solo_dev, SOLO_VE_CH_QP(ch), solo_enc->qp);
	solo_reg_write(solo_dev, SOLO_CAP_CH_INTV(ch), interval);

	/* Extended encoding only */
	solo_reg_write(solo_dev, SOLO_VE_CH_GOP_E(ch), solo_enc->gop);
	solo_reg_write(solo_dev, SOLO_VE_CH_QP_E(ch), solo_enc->qp);
	solo_reg_write(solo_dev, SOLO_CAP_CH_INTV_E(ch), interval);

	/* Enables the standard encoder */
	solo_reg_write(solo_dev, SOLO_CAP_CH_SCALE(ch), solo_enc->mode);

	/* Settle down Beavis... */
	mdelay(10);

	return 0;
}

static void solo_enc_off(struct solo_enc_fh *fh)
{
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;

	if (!fh->enc_on)
		return;

	if (fh->kthread) {
		kthread_stop(fh->kthread);
		fh->kthread = NULL;
	}

	solo_dev->enc_bw_remain += solo_enc->bw_weight;
	fh->enc_on = 0;

	if (atomic_dec_return(&solo_enc->readers) > 0)
		return;

	solo_reg_write(solo_dev, SOLO_CAP_CH_SCALE(solo_enc->ch), 0);
}

static void enc_reset_gop(struct solo6010_dev *solo_dev, u8 ch)
{
	BUG_ON(ch >= solo_dev->nr_chans);
	solo_reg_write(solo_dev, SOLO_VE_CH_GOP(ch), 1);
	solo_dev->v4l2_enc[ch]->reset_gop = 1;
}

static int enc_gop_reset(struct solo6010_dev *solo_dev, u8 ch, u8 vop)
{
	BUG_ON(ch >= solo_dev->nr_chans);
	if (!solo_dev->v4l2_enc[ch]->reset_gop)
		return 0;
	if (vop)
		return 1;
	solo_dev->v4l2_enc[ch]->reset_gop = 0;
	solo_reg_write(solo_dev, SOLO_VE_CH_GOP(ch),
		       solo_dev->v4l2_enc[ch]->gop);
	return 0;
}

static int enc_get_mpeg_dma(struct solo6010_dev *solo_dev, void *buf,
			    unsigned int off, unsigned int size)
{
	int ret;

	if (off > SOLO_MP4E_EXT_SIZE(solo_dev))
		return -EINVAL;

	if (off + size <= SOLO_MP4E_EXT_SIZE(solo_dev))
		return solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_MP4E, 0, buf,
				    SOLO_MP4E_EXT_ADDR(solo_dev) + off, size);

	/* Buffer wrap */
	ret = solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_MP4E, 0, buf,
			   SOLO_MP4E_EXT_ADDR(solo_dev) + off,
			   SOLO_MP4E_EXT_SIZE(solo_dev) - off);

	ret |= solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_MP4E, 0,
			    buf + SOLO_MP4E_EXT_SIZE(solo_dev) - off,
			    SOLO_MP4E_EXT_ADDR(solo_dev),
			    size + off - SOLO_MP4E_EXT_SIZE(solo_dev));

	return ret;
}

static int enc_get_jpeg_dma(struct solo6010_dev *solo_dev, void *buf,
			    unsigned int off, unsigned int size)
{
	int ret;

	if (off > SOLO_JPEG_EXT_SIZE(solo_dev))
		return -EINVAL;

	if (off + size <= SOLO_JPEG_EXT_SIZE(solo_dev))
		return solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_JPEG, 0, buf,
				    SOLO_JPEG_EXT_ADDR(solo_dev) + off, size);

	/* Buffer wrap */
	ret = solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_JPEG, 0, buf,
			   SOLO_JPEG_EXT_ADDR(solo_dev) + off,
			   SOLO_JPEG_EXT_SIZE(solo_dev) - off);

	ret |= solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_JPEG, 0,
			    buf + SOLO_JPEG_EXT_SIZE(solo_dev) - off,
			    SOLO_JPEG_EXT_ADDR(solo_dev),
			    size + off - SOLO_JPEG_EXT_SIZE(solo_dev));

	return ret;
}

static int solo_fill_jpeg(struct solo_enc_dev *solo_enc,
			  struct solo_enc_buf *enc_buf,
			  struct videobuf_buffer *vb, void *vbuf)
{
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;

	jpeg_set_header(vbuf, solo_enc->width, solo_enc->height);
	vbuf += JPEG_HEADER_SIZE;
	vb->size = enc_buf->jpeg_size + JPEG_HEADER_SIZE;

	return enc_get_jpeg_dma(solo_dev, vbuf, enc_buf->jpeg_off,
			       enc_buf->jpeg_size);
}

static int solo_fill_mpeg(struct solo_enc_dev *solo_enc,
			  struct solo_enc_buf *enc_buf,
			  struct videobuf_buffer *vb, void *vbuf)
{
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	struct vop_header vh;
	u8 *p = vbuf;
	int ret;

	/* First get the hardware vop header (not real mpeg) */
	ret = enc_get_mpeg_dma(solo_dev, &vh, enc_buf->off, sizeof(vh));
	if (ret)
		return -1;

	if (vh.size > enc_buf->size)
		return -1;

	vb->width = vh.hsize << 4;
	vb->height = vh.vsize << 4;
	vb->size = vh.size;

	if (!enc_buf->vop) {
		vb->size += sizeof(vid_vop_header);
		p += sizeof(vid_vop_header);
	}

	/* Now get the actual mpeg payload */
	enc_buf->off = (enc_buf->off + sizeof(vh)) %
			SOLO_MP4E_EXT_SIZE(solo_dev);
	enc_buf->size -= sizeof(vh);

	ret = enc_get_mpeg_dma(solo_dev, p, enc_buf->off, enc_buf->size);
	if (ret)
		return -1;

	/* Check for valid mpeg data */
	if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01 || p[3] != 0xb6)
		return -1;

	/* If this is a key frame, add extra m4v header */
	if (!enc_buf->vop) {
		u16 fps = solo_dev->fps * 1000;
		u16 interval = solo_enc->interval * 1000;

		p = vbuf;
		memcpy(p, vid_vop_header, sizeof(vid_vop_header));

		if (solo_dev->video_type == SOLO_VO_FMT_TYPE_NTSC)
			p[10] |= ((XVID_PAR_43_NTSC << 3) & 0x78);
		else
			p[10] |= ((XVID_PAR_43_PAL << 3) & 0x78);

		/* Frame rate and interval */
		p[22] = fps >> 4;
		p[23] = ((fps << 4) & 0xf0) | 0x0c | ((interval >> 13) & 0x3);
		p[24] = (interval >> 5) & 0xff;
		p[25] = ((interval << 3) & 0xf8) | 0x04;

		/* Width and height */
		p[26] = (vb->width >> 3) & 0xff;
		p[27] = ((vb->height >> 9) & 0x0f) | 0x10;
		p[28] = (vb->height >> 1) & 0xff;

		/* Interlace */
		if (vh.interlace)
			p[29] |= 0x20;
	}

	/* TODO Mark keyframe in vb? */

	return 0;
}

/* On successful return (0), leaves solo_enc->lock unlocked */
static int solo_enc_fillbuf(struct solo_enc_fh *fh,
			    struct videobuf_buffer *vb,
			    unsigned long flags)
{
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	struct solo_enc_buf *enc_buf = NULL;
	void *vbuf;
	int ret;

	while (fh->rd_idx != solo_dev->enc_wr_idx) {
		enc_buf = &solo_dev->enc_buf[fh->rd_idx];
		fh->rd_idx = (fh->rd_idx + 1) % SOLO_NR_RING_BUFS;
		if (enc_buf->ch == solo_enc->ch)
			break;
		enc_buf = NULL;
	}

	if (!enc_buf)
		return -1;

	if ((fh->fmt == V4L2_PIX_FMT_MPEG &&
	     vb->bsize < enc_buf->size) ||
	    (fh->fmt == V4L2_PIX_FMT_MJPEG &&
	     vb->bsize < (enc_buf->jpeg_size + JPEG_HEADER_SIZE))) {
		return -1;
	}

	if (!(vbuf = videobuf_to_vmalloc(vb)))
		return -1;

	/* Now that we know we have a valid buffer we care about. At this
	 * point, if we fail, we have to show the buffer in an ERROR state */
	list_del(&vb->queue);

	/* Is it ok that we mess with this buffer out of lock? */
	spin_unlock_irqrestore(&solo_enc->lock, flags);

	if (fh->fmt == V4L2_PIX_FMT_MPEG)
		ret = solo_fill_mpeg(solo_enc, enc_buf, vb, vbuf);
	else
		ret = solo_fill_jpeg(solo_enc, enc_buf, vb, vbuf);

	if (!ret) {
		vb->field_count++;
		vb->ts = enc_buf->ts;
		vb->state = VIDEOBUF_DONE;
	} else
		vb->state = VIDEOBUF_ERROR;

	wake_up(&vb->done);

	return 0;
}

static void solo_enc_thread_try(struct solo_enc_fh *fh)
{
	struct solo_enc_dev *solo_enc = fh->enc;
	struct videobuf_buffer *vb;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&solo_enc->lock, flags);

		if (list_empty(&fh->vidq_active))
			break;

		vb = list_first_entry(&fh->vidq_active,
				      struct videobuf_buffer, queue);

		if (!waitqueue_active(&vb->done))
			break;

		/* On success, returns with solo_enc->lock unlocked */
		if (solo_enc_fillbuf(fh, vb, flags))
			break;
	}

	spin_unlock_irqrestore(&solo_enc->lock, flags);
}

static int solo_enc_thread(void *data)
{
	struct solo_enc_fh *fh = data;
	struct solo_enc_dev *solo_enc = fh->enc;
	DECLARE_WAITQUEUE(wait, current);

	set_freezable();
	add_wait_queue(&solo_enc->thread_wait, &wait);

	for (;;) {
		long timeout = schedule_timeout_interruptible(HZ);
		if (timeout == -ERESTARTSYS || kthread_should_stop())
			break;
		solo_enc_thread_try(fh);
		try_to_freeze();
	}

	remove_wait_queue(&solo_enc->thread_wait, &wait);

        return 0;
}

void solo_motion_isr(struct solo6010_dev *solo_dev)
{
	u32 status, sec, usec;
	int i;

	solo_reg_write(solo_dev, SOLO_IRQ_STAT, SOLO_IRQ_MOTION);

	sec = solo_reg_read(solo_dev, SOLO_TIMER_SEC);
	usec = solo_reg_read(solo_dev, SOLO_TIMER_USEC);
	status = solo_reg_read(solo_dev, SOLO_VI_MOT_STATUS);

	for (i = 0; i < solo_dev->nr_chans; i++) {
		struct solo_enc_dev *solo_enc = solo_dev->v4l2_enc[i];
		if (!(status & (1 << i)) || solo_enc->motion_detected)
			continue;
		solo_enc->motion_detected = 1;
		solo_enc->motion_sec = sec;
		solo_enc->motion_usec = usec;
	}
}

void solo_enc_v4l2_isr(struct solo6010_dev *solo_dev)
{
	struct solo_enc_buf *enc_buf;
	struct videnc_status vstatus;
	u32 mpeg_current, mpeg_next, mpeg_size;
	u32 jpeg_current, jpeg_next, jpeg_size;
	u32 reg_mpeg_size;
	u8 cur_q, vop_type;
	u8 ch;

	solo_reg_write(solo_dev, SOLO_IRQ_STAT, SOLO_IRQ_ENCODER);

	vstatus.status11 = solo_reg_read(solo_dev, SOLO_VE_STATE(11));
	cur_q = (vstatus.status11_st.last_queue + 1) % MP4_QS;

	vstatus.status0 = solo_reg_read(solo_dev, SOLO_VE_STATE(0));
	reg_mpeg_size = (vstatus.status0_st.mp4_enc_code_size + 64 + 32) &
			(~31);

	while (solo_dev->enc_idx != cur_q) {
		mpeg_current = solo_reg_read(solo_dev,
					SOLO_VE_MPEG4_QUE(solo_dev->enc_idx));
		jpeg_current = solo_reg_read(solo_dev,
					SOLO_VE_JPEG_QUE(solo_dev->enc_idx));
		solo_dev->enc_idx = (solo_dev->enc_idx + 1) % MP4_QS;
		mpeg_next = solo_reg_read(solo_dev,
					SOLO_VE_MPEG4_QUE(solo_dev->enc_idx));
		jpeg_next = solo_reg_read(solo_dev,
					SOLO_VE_JPEG_QUE(solo_dev->enc_idx));

		ch = (mpeg_current >> 24) & 0x1f;
		vop_type = (mpeg_current >> 29) & 3;

		mpeg_current &= 0x00ffffff;
		mpeg_next    &= 0x00ffffff;
		jpeg_current &= 0x00ffffff;
		jpeg_next    &= 0x00ffffff;

		mpeg_size = (SOLO_MP4E_EXT_SIZE(solo_dev) +
			     mpeg_next - mpeg_current) %
			    SOLO_MP4E_EXT_SIZE(solo_dev);

		jpeg_size = (SOLO_JPEG_EXT_SIZE(solo_dev) +
			     jpeg_next - jpeg_current) %
			    SOLO_JPEG_EXT_SIZE(solo_dev);

		/* XXX I think this means we had a ring overflow? */
		if (mpeg_current > mpeg_next && mpeg_size != reg_mpeg_size) {
			enc_reset_gop(solo_dev, ch);
			continue;
		}

		/* When resetting the GOP, skip frames until I-frame */
		if (enc_gop_reset(solo_dev, ch, vop_type))
			continue;

		enc_buf = &solo_dev->enc_buf[solo_dev->enc_wr_idx];

		enc_buf->vop = vop_type;
		enc_buf->ch = ch;
		enc_buf->off = mpeg_current;
		enc_buf->size = mpeg_size;
		enc_buf->jpeg_off = jpeg_current;
		enc_buf->jpeg_size = jpeg_size;

		do_gettimeofday(&enc_buf->ts);

		solo_dev->enc_wr_idx = (solo_dev->enc_wr_idx + 1) %
					SOLO_NR_RING_BUFS;

		wake_up_interruptible(&solo_dev->v4l2_enc[ch]->thread_wait);
	}

	return;
}

static int solo_enc_buf_setup(struct videobuf_queue *vq, unsigned int *count,
			      unsigned int *size)
{
        *size = FRAME_BUF_SIZE;

        if (*count < MIN_VID_BUFFERS)
		*count = MIN_VID_BUFFERS;

        return 0;
}

static int solo_enc_buf_prepare(struct videobuf_queue *vq,
				struct videobuf_buffer *vb,
				enum v4l2_field field)
{
	struct solo_enc_fh *fh = vq->priv_data;
	struct solo_enc_dev *solo_enc = fh->enc;

	vb->size = FRAME_BUF_SIZE;
	if (vb->baddr != 0 && vb->bsize < vb->size)
		return -EINVAL;

	/* These properties only change when queue is idle */
	vb->width = solo_enc->width;
	vb->height = solo_enc->height;
	vb->field  = field;

	if (vb->state == VIDEOBUF_NEEDS_INIT) {
		int rc = videobuf_iolock(vq, vb, NULL);
		if (rc < 0) {
			videobuf_vmalloc_free(vb);
			vb->state = VIDEOBUF_NEEDS_INIT;
			return rc;
		}
	}
	vb->state = VIDEOBUF_PREPARED;

	return 0;
}

static void solo_enc_buf_queue(struct videobuf_queue *vq,
			       struct videobuf_buffer *vb)
{
	struct solo_enc_fh *fh = vq->priv_data;

	vb->state = VIDEOBUF_QUEUED;
	list_add_tail(&vb->queue, &fh->vidq_active);
	wake_up_interruptible(&fh->enc->thread_wait);
}

static void solo_enc_buf_release(struct videobuf_queue *vq,
				 struct videobuf_buffer *vb)
{
	videobuf_vmalloc_free(vb);
	vb->state = VIDEOBUF_NEEDS_INIT;
}

static struct videobuf_queue_ops solo_enc_video_qops = {
	.buf_setup	= solo_enc_buf_setup,
	.buf_prepare	= solo_enc_buf_prepare,
	.buf_queue	= solo_enc_buf_queue,
	.buf_release	= solo_enc_buf_release,
};

static unsigned int solo_enc_poll(struct file *file,
				  struct poll_table_struct *wait)
{
	struct solo_enc_fh *fh = file->private_data;

	return videobuf_poll_stream(file, &fh->vidq, wait);
}

static int solo_enc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct solo_enc_fh *fh = file->private_data;

	return videobuf_mmap_mapper(&fh->vidq, vma);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static int solo_enc_open(struct file *file)
#else
static int solo_enc_open(struct inode *ino, struct file *file)
#endif
{
	struct solo_enc_dev *solo_enc = video_drvdata(file);
	unsigned long flags;
	struct solo_enc_fh *fh;

	if ((fh = kzalloc(sizeof(*fh), GFP_KERNEL)) == NULL)
		return -ENOMEM;

	spin_lock_irqsave(&solo_enc->lock, flags);

	fh->enc = solo_enc;
	file->private_data = fh;
	INIT_LIST_HEAD(&fh->vidq_active);
	fh->fmt = V4L2_PIX_FMT_MPEG;

	videobuf_queue_vmalloc_init(&fh->vidq, &solo_enc_video_qops,
				    NULL, &solo_enc->lock,
				    V4L2_BUF_TYPE_VIDEO_CAPTURE,
				    V4L2_FIELD_INTERLACED,
				    sizeof(struct videobuf_buffer), fh);

	spin_unlock_irqrestore(&solo_enc->lock, flags);

	return 0;
}

static ssize_t solo_enc_read(struct file *file, char __user *data,
			     size_t count, loff_t *ppos)
{
	struct solo_enc_fh *fh = file->private_data;
	struct solo_enc_dev *solo_enc = fh->enc;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&solo_enc->lock, flags);
	if ((ret = solo_enc_on(fh))) {
		spin_unlock_irqrestore(&solo_enc->lock, flags);
		return ret;
	}
        spin_unlock_irqrestore(&solo_enc->lock, flags);

	return videobuf_read_stream(&fh->vidq, data, count, ppos, 0,
				    file->f_flags & O_NONBLOCK);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static int solo_enc_release(struct file *file)
#else
static int solo_enc_release(struct inode *ino, struct file *file)
#endif
{
	struct solo_enc_fh *fh = file->private_data;
	struct solo_enc_dev *solo_enc = fh->enc;
	unsigned long flags;

	spin_lock_irqsave(&solo_enc->lock, flags);

	videobuf_stop(&fh->vidq);
	videobuf_mmap_free(&fh->vidq);
	solo_enc_off(fh);
	kfree(fh);

	spin_unlock_irqrestore(&solo_enc->lock, flags);

	return 0;
}

static int solo_enc_querycap(struct file *file, void  *priv,
			     struct v4l2_capability *cap)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;

	strcpy(cap->driver, SOLO6010_NAME);
	snprintf(cap->card, sizeof(cap->card), "Softlogic 6010 Enc %d",
		 solo_enc->ch);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI %s",
		 pci_name(solo_dev->pdev));
	cap->version = SOLO6010_VER_NUM;
	cap->capabilities =     V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_READWRITE |
				V4L2_CAP_STREAMING;
	return 0;
}

static int solo_enc_enum_input(struct file *file, void *priv,
			       struct v4l2_input *input)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;

	if (input->index)
		return -EINVAL;

	snprintf(input->name, sizeof(input->name), "Encoder %d",
		 solo_enc->ch + 1);
	input->type = V4L2_INPUT_TYPE_CAMERA;

	if (solo_dev->video_type == SOLO_VO_FMT_TYPE_NTSC)
		input->std = V4L2_STD_NTSC_M;
	else
		input->std = V4L2_STD_PAL_M;

	/* TODO Should check for signal status on this camera */
	input->status = 0;

	return 0;
}

static int solo_enc_set_input(struct file *file, void *priv, unsigned int index)
{
	if (index)
		return -EINVAL;

	return 0;
}

static int solo_enc_get_input(struct file *file, void *priv,
			      unsigned int *index)
{
	*index = 0;

	return 0;
}

static int solo_enc_enum_fmt_cap(struct file *file, void *priv,
				 struct v4l2_fmtdesc *f)
{
	if (f->index > 1)
		return -EINVAL;

	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	switch (f->index) {
	case 0:
		f->pixelformat = V4L2_PIX_FMT_MPEG;
		strcpy(f->description, "MPEG-4 AVC");
		break;
	case 1:
		f->pixelformat = V4L2_PIX_FMT_MJPEG;
		strcpy(f->description, "MJPEG");
		break;
	}

	return 0;
}

static int solo_enc_try_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (pix->pixelformat != V4L2_PIX_FMT_MPEG &&
	    pix->pixelformat != V4L2_PIX_FMT_MJPEG)
		return -EINVAL;

	/* We cannot change width/height in mid read */
	if (atomic_read(&solo_enc->readers) > 0) {
		if (pix->width != solo_enc->width ||
		    pix->height != solo_enc->height)
			return -EBUSY;
	} else if (!(pix->width == solo_dev->video_hsize &&
	      pix->height == solo_dev->video_vsize << 1) &&
	    !(pix->width == solo_dev->video_hsize >> 1 &&
	      pix->height == solo_dev->video_vsize)) {
		/* Default to CIF 1/2 size */
		pix->width = solo_dev->video_hsize >> 1;
		pix->height = solo_dev->video_vsize;
	}

	if (pix->field == V4L2_FIELD_ANY)
		pix->field = V4L2_FIELD_INTERLACED;
	else if (pix->field != V4L2_FIELD_INTERLACED) {
		pix->field = V4L2_FIELD_INTERLACED;
	}

	/* Just set these */
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
	pix->sizeimage = FRAME_BUF_SIZE;

	return 0;
}

static int solo_enc_set_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&solo_enc->lock, flags);

	if ((ret = solo_enc_try_fmt_cap(file, priv, f))) {
		spin_unlock_irqrestore(&solo_enc->lock, flags);
		return ret;
	}

	if (pix->width == solo_dev->video_hsize)
		solo_enc->mode = SOLO_ENC_MODE_D1;
	else
		solo_enc->mode = SOLO_ENC_MODE_CIF;

	/* This does not change the encoder at all */
	fh->fmt = pix->pixelformat;

	ret = solo_enc_on(fh);

	spin_unlock_irqrestore(&solo_enc->lock, flags);

	return ret;
}

static int solo_enc_get_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->width = solo_enc->width;
	pix->height = solo_enc->height;
	pix->pixelformat = fh->fmt;
	pix->field = solo_enc->interlaced ? V4L2_FIELD_INTERLACED :
		     V4L2_FIELD_NONE;
	pix->sizeimage = FRAME_BUF_SIZE;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int solo_enc_reqbufs(struct file *file, void *priv, 
			    struct v4l2_requestbuffers *req)
{
	struct solo_enc_fh *fh = priv;

	return videobuf_reqbufs(&fh->vidq, req);
}

static int solo_enc_querybuf(struct file *file, void *priv,
			     struct v4l2_buffer *buf)
{
	struct solo_enc_fh *fh = priv;

	return videobuf_querybuf(&fh->vidq, buf);
}

static int solo_enc_qbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct solo_enc_fh *fh = priv;

	return videobuf_qbuf(&fh->vidq, buf);
}

static int solo_enc_dqbuf(struct file *file, void *priv,
			  struct v4l2_buffer *buf)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&solo_enc->lock, flags);
	if ((ret = solo_enc_on(fh))) {
		spin_unlock_irqrestore(&solo_enc->lock, flags);
		return ret;
	}
	spin_unlock_irqrestore(&solo_enc->lock, flags);

	ret = videobuf_dqbuf(&fh->vidq, buf, file->f_flags & O_NONBLOCK);
	if (ret)
		return ret;

	/* Fake bframe to signal motion detection */
	if (solo_enc->motion_detected) {
		buf->flags |= V4L2_BUF_FLAG_BFRAME;
		solo_reg_write(solo_enc->solo_dev, SOLO_VI_MOT_CLEAR,
			       1 << solo_enc->ch);
		solo_enc->motion_detected = 0;
	}

	/* XXX Set key-frame/p-frame flag */
	// buf->flags |= V4L2_BUF_FLAG_KEYFRAME;
	// buf->flags |= V4L2_BUF_FLAG_PFRAME;

	return 0;
}

static int solo_enc_streamon(struct file *file, void *priv,
			     enum v4l2_buf_type i)
{
	struct solo_enc_fh *fh = priv;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return videobuf_streamon(&fh->vidq);
}

static int solo_enc_streamoff(struct file *file, void *priv,
			      enum v4l2_buf_type i)
{
	struct solo_enc_fh *fh = priv;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return videobuf_streamoff(&fh->vidq);
}

static int solo_enc_s_std(struct file *file, void *priv, v4l2_std_id *i)
{
	return 0;
}

static int solo_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fsize)
{
	struct solo_enc_fh *fh = priv;
	struct solo6010_dev *solo_dev = fh->enc->solo_dev;

	if (fsize->pixel_format != V4L2_PIX_FMT_MPEG)
		return -EINVAL;

	switch (fsize->index) {
	case 0:
		fsize->discrete.width = solo_dev->video_hsize >> 1;
		fsize->discrete.height = solo_dev->video_vsize;
		break;
	case 1:
		fsize->discrete.width = solo_dev->video_hsize;
		fsize->discrete.height = solo_dev->video_vsize << 1;
		break;
	default:
		return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;

	return 0;
}

static int solo_enum_frameintervals(struct file *file, void *priv,
				    struct v4l2_frmivalenum *fintv)
{
	struct solo_enc_fh *fh = priv;
	struct solo6010_dev *solo_dev = fh->enc->solo_dev;

	if (fintv->pixel_format != V4L2_PIX_FMT_MPEG || fintv->index)
		return -EINVAL;

	fintv->type = V4L2_FRMIVAL_TYPE_STEPWISE;

	fintv->stepwise.min.numerator = solo_dev->fps;
	fintv->stepwise.min.denominator = 1;

	fintv->stepwise.max.numerator = solo_dev->fps;
	fintv->stepwise.max.denominator = 15;

	fintv->stepwise.step.numerator = 1;
	fintv->stepwise.step.denominator = 1;

	return 0;
}

static int solo_g_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *sp)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	struct v4l2_captureparm *cp = &sp->parm.capture;

	cp->capability = V4L2_CAP_TIMEPERFRAME;
	cp->timeperframe.numerator = solo_enc->interval;
	cp->timeperframe.denominator = solo_dev->fps;
	cp->capturemode = 0;
	/* XXX: Shouldn't we be able to get/set this from videobuf? */
	cp->readbuffers = 2;

        return 0;
}

static int solo_s_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *sp)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	unsigned long flags;
	struct v4l2_captureparm *cp = &sp->parm.capture;

	spin_lock_irqsave(&solo_enc->lock, flags);

	if (atomic_read(&solo_enc->readers) > 0) {
		spin_unlock_irqrestore(&solo_enc->lock, flags);
		return -EBUSY;
	}

	if ((cp->timeperframe.numerator == 0) ||
	    (cp->timeperframe.denominator == 0)) {
		/* reset framerate */
		cp->timeperframe.numerator = 1;
		cp->timeperframe.denominator = solo_dev->fps;
	}

	if (cp->timeperframe.denominator != solo_dev->fps)
		cp->timeperframe.denominator = solo_dev->fps;

	if (cp->timeperframe.numerator > 15)
		cp->timeperframe.numerator = 15;

	solo_enc->interval = cp->timeperframe.numerator;

	cp->capability = V4L2_CAP_TIMEPERFRAME;

	solo_update_mode(solo_enc);

	spin_unlock_irqrestore(&solo_enc->lock, flags);

        return 0;
}

static int solo_queryctrl(struct file *file, void *priv,
			  struct v4l2_queryctrl *qc)
{
	struct solo_enc_fh *fh = priv;
	struct solo6010_dev *solo_dev = fh->enc->solo_dev;

	qc->id = v4l2_ctrl_next(solo_ctrl_classes, qc->id);
	if (!qc->id)
		return -EINVAL;

	if (V4L2_CTRL_ID2CLASS(qc->id) == V4L2_CTRL_CLASS_USER)
		return v4l2_ctrl_query_fill(qc, 0x0, 0xff, 1, 0x80);

	if (V4L2_CTRL_ID2CLASS(qc->id) != V4L2_CTRL_CLASS_MPEG)
		return -EINVAL;

	switch (qc->id) {
	case V4L2_CID_MPEG_VIDEO_ENCODING:
		return v4l2_ctrl_query_fill(
			qc, V4L2_MPEG_VIDEO_ENCODING_MPEG_1,
			V4L2_MPEG_VIDEO_ENCODING_MPEG_4_AVC, 1,
			V4L2_MPEG_VIDEO_ENCODING_MPEG_4_AVC);
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		return v4l2_ctrl_query_fill(qc, 1, 255, 1, solo_dev->fps);
	}

        return -EINVAL;
}

static int solo_querymenu(struct file *file, void *priv,
			  struct v4l2_querymenu *qmenu)
{
	struct v4l2_queryctrl qctrl;
	int err;

	qctrl.id = qmenu->id;
	if ((err = solo_queryctrl(file, priv, &qctrl)))
		return err;
	return v4l2_ctrl_query_menu(qmenu, &qctrl, NULL);
}

static int solo_g_ctrl(struct file *file, void *priv,
		       struct v4l2_control *ctrl)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	int ret = 0;

	if (V4L2_CTRL_ID2CLASS(ctrl->id) == V4L2_CTRL_CLASS_USER)
		return tw28_get_ctrl_val(solo_dev, ctrl->id, solo_enc->ch,
					 &ctrl->value);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_ENCODING:
		ctrl->value = V4L2_MPEG_VIDEO_ENCODING_MPEG_4_AVC;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctrl->value = solo_enc->gop;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int solo_s_ctrl(struct file *file, void *priv,
		       struct v4l2_control *ctrl)
{
	struct solo_enc_fh *fh = priv;
	struct solo_enc_dev *solo_enc = fh->enc;
	struct solo6010_dev *solo_dev = solo_enc->solo_dev;
	int ret = 0;

	if (V4L2_CTRL_ID2CLASS(ctrl->id) == V4L2_CTRL_CLASS_USER)
		return tw28_set_ctrl_val(solo_dev, ctrl->id, solo_enc->ch,
					 ctrl->value);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_ENCODING:
		if (ctrl->value != V4L2_MPEG_VIDEO_ENCODING_MPEG_4_AVC)
			return -ERANGE;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		if (ctrl->value < 1 || ctrl->value > 255)
			return -ERANGE;
		solo_enc->gop = ctrl->value;
		solo_reg_write(solo_dev, SOLO_VE_CH_GOP(solo_enc->ch),
			       solo_enc->gop);
		solo_reg_write(solo_dev, SOLO_VE_CH_GOP_E(solo_enc->ch),
			       solo_enc->gop);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,28)
static const struct v4l2_file_operations solo_enc_fops = {
#else
static const struct file_operations solo_enc_fops = {
#endif
	.owner			= THIS_MODULE,
	.open			= solo_enc_open,
	.release		= solo_enc_release,
	.read			= solo_enc_read,
	.poll			= solo_enc_poll,
	.mmap			= solo_enc_mmap,
	.ioctl			= video_ioctl2,
};

static const struct v4l2_ioctl_ops solo_enc_ioctl_ops = {
	.vidioc_querycap		= solo_enc_querycap,
	.vidioc_s_std			= solo_enc_s_std,
	/* Input callbacks */
	.vidioc_enum_input		= solo_enc_enum_input,
	.vidioc_s_input			= solo_enc_set_input,
	.vidioc_g_input			= solo_enc_get_input,
	/* Video capture format callbacks */
	.vidioc_enum_fmt_vid_cap	= solo_enc_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap		= solo_enc_try_fmt_cap,
	.vidioc_s_fmt_vid_cap		= solo_enc_set_fmt_cap,
	.vidioc_g_fmt_vid_cap		= solo_enc_get_fmt_cap,
	/* Streaming I/O */
	.vidioc_reqbufs			= solo_enc_reqbufs,
	.vidioc_querybuf		= solo_enc_querybuf,
	.vidioc_qbuf			= solo_enc_qbuf,
	.vidioc_dqbuf			= solo_enc_dqbuf,
	.vidioc_streamon		= solo_enc_streamon,
	.vidioc_streamoff		= solo_enc_streamoff,
	/* Frame size and interval */
	.vidioc_enum_framesizes		= solo_enum_framesizes,
	.vidioc_enum_frameintervals	= solo_enum_frameintervals,
	/* Video capture parameters */
	.vidioc_s_parm			= solo_s_parm,
	.vidioc_g_parm			= solo_g_parm,
	/* Controls */
	.vidioc_queryctrl		= solo_queryctrl,
	.vidioc_querymenu		= solo_querymenu,
	.vidioc_g_ctrl			= solo_g_ctrl,
	.vidioc_s_ctrl			= solo_s_ctrl,
};

static struct video_device solo_enc_template = {
	.name			= SOLO6010_NAME,
	.fops			= &solo_enc_fops,
	.ioctl_ops		= &solo_enc_ioctl_ops,
	.minor			= -1,
	.release		= video_device_release,

	.tvnorms		= V4L2_STD_NTSC_M | V4L2_STD_PAL_M,
	.current_norm		= V4L2_STD_NTSC_M,
};

static struct solo_enc_dev *solo_enc_alloc(struct solo6010_dev *solo_dev, u8 ch)
{
	struct solo_enc_dev *solo_enc;
	int ret;

	solo_enc = kzalloc(sizeof(*solo_enc), GFP_KERNEL);
	if (!solo_enc)
		return ERR_PTR(-ENOMEM);

	solo_enc->vfd = video_device_alloc();
	if (!solo_enc->vfd) {
		kfree(solo_enc);
		return ERR_PTR(-ENOMEM);
	}

	solo_enc->solo_dev = solo_dev;
	solo_enc->ch = ch;

	*solo_enc->vfd = solo_enc_template;
	solo_enc->vfd->parent = &solo_dev->pdev->dev;
	ret = video_register_device(solo_enc->vfd, VFL_TYPE_GRABBER,
				    video_nr);
	if (ret < 0) {
		video_device_release(solo_enc->vfd);
		kfree(solo_enc);
		return ERR_PTR(ret);
	}

	video_set_drvdata(solo_enc->vfd, solo_enc);

	snprintf(solo_enc->vfd->name, sizeof(solo_enc->vfd->name),
		 "%s-enc (%i/%i)", SOLO6010_NAME, solo_dev->vfd->num,
		 solo_enc->vfd->num);

	if (video_nr >= 0)
		video_nr++;

	spin_lock_init(&solo_enc->lock);
	init_waitqueue_head(&solo_enc->thread_wait);
	atomic_set(&solo_enc->readers, 0);

	solo_enc->qp = SOLO_DEFAULT_QP;
        solo_enc->gop = solo_dev->fps;
	solo_enc->interval = 1;
	solo_enc->mode = SOLO_ENC_MODE_CIF;
	solo_update_mode(solo_enc);

	return solo_enc;
}

static void solo_enc_free(struct solo_enc_dev *solo_enc)
{
	video_unregister_device(solo_enc->vfd);
	kfree(solo_enc);
}

int solo_enc_v4l2_init(struct solo6010_dev *solo_dev)
{
	int i;

	for (i = 0; i < solo_dev->nr_chans; i++) {
		solo_dev->v4l2_enc[i] = solo_enc_alloc(solo_dev, i);
		if (IS_ERR(solo_dev->v4l2_enc[i]))
			break;
	}

	if (i != solo_dev->nr_chans) {
		int ret = PTR_ERR(solo_dev->v4l2_enc[i]);
		while (i--)
			solo_enc_free(solo_dev->v4l2_enc[i]);
		return ret;
	}

	/* D1@MAX-FPS * 4 */
	solo_dev->enc_bw_remain = solo_dev->fps * 4 * 4;

	dev_info(&solo_dev->pdev->dev, "Encoders as /dev/video%d-%d\n",
		 solo_dev->v4l2_enc[0]->vfd->num,
		 solo_dev->v4l2_enc[solo_dev->nr_chans - 1]->vfd->num);

	solo6010_irq_on(solo_dev, SOLO_IRQ_MOTION);

	return 0;
}

void solo_enc_v4l2_exit(struct solo6010_dev *solo_dev)
{
	int i;

	solo6010_irq_off(solo_dev, SOLO_IRQ_MOTION);

	for (i = 0; i < solo_dev->nr_chans; i++)
		solo_enc_free(solo_dev->v4l2_enc[i]);
}
