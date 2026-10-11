// SPDX-License-Identifier: GPL-2.0
/*
 * Original driver uses default altsetting (#0) of streaming interface, which allows bursts of bulk
 * transfers of 15x1024 bytes on output. But the HW actually works incorrectly here: it uses same
 * endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification: endpoint
 * addresses can be shared only between alternate settings, not interfaces. In order to workaround
 * this we use isochronous transfers instead of bulk. There is a possibility that we still can use
 * bulk transfers with interface 0, but this is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

/* Triple buffering:
 *  - one buffer for HDMI rendering
 *  - one buffer for USB transmission
 *  - one buffer for DRM/KMS data copy
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

#define FL2000_URB_TIMEOUT 100

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
	u32 seq;	/* frame number given by fl2000_stream_compress(), 0 for none yet */
	u32 hash;	/* sampled content hash, to tell frames apart in the debug log */
};

struct fl2000_stream {
	struct usb_device *usb_dev;
	struct drm_crtc *crtc;
	/* Each buffer journey: render->transmit->wait->... */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock; /* List access from bh and interrupt contexts */
	size_t buf_size;
	u32 bytes_pix;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	struct semaphore work_sem;
	bool enabled;
	struct usb_anchor anchor;

	/* Frame pacing statistics, under list_lock. The chip has no frame memory, so a frame URB
	 * completes only as fast as the timing generator consumes it: completions tick at the
	 * real refresh rate
	 */
	u64 stat_count;
	u64 stat_first_ns;
	u64 stat_last_ns;
	unsigned int stat_head;
	u64 stat_ring[FL2000_FRAME_RING];

	/* What was sent, frame by frame, under list_lock: debugfs "sent" */
	u32 frame_seq;
	u64 dropped;
	unsigned int sent_head;
	u64 sent_count;
	struct fl2000_sent_frame sent[FL2000_FRAME_RING];
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (int i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	kfree(sb->pages);

	kfree(sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *sb;
	unsigned int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;

	sb->pages = kcalloc(nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (int i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);

	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *temp_sb;

	list_for_each_entry_safe(cur_sb, temp_sb, &stream->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000_stream *stream, unsigned int size)
{
	int ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&stream->render_list));

	for (int i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &stream->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(stream);
	return ret;
}

static void fl2000_stream_release(struct device *dev, void *res)
{
	struct fl2000_stream *stream = res;

	UNUSED(dev);

	fl2000_stream_disable(stream);
	destroy_workqueue(stream->work_queue);
	fl2000_stream_put_buffers(stream);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;

	if (stream) {
		unsigned long flags;

		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		if (!urb->status) {
			u64 now = ktime_get_ns();

			if (!stream->stat_count)
				stream->stat_first_ns = now;
			stream->stat_last_ns = now;
			stream->stat_ring[stream->stat_head] = now;
			stream->stat_head = (stream->stat_head + 1) % FL2000_FRAME_RING;
			stream->stat_count++;
		}
		spin_unlock_irqrestore(&stream->list_lock, flags);

		drm_crtc_handle_vblank(stream->crtc);

		/* Kick transmit workqueue */
		up(&stream->work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

/* Under list_lock */
static void fl2000_stream_record_sent(struct fl2000_stream *stream,
				      struct fl2000_stream_buf *sb, bool repeat)
{
	struct fl2000_sent_frame *f = &stream->sent[stream->sent_head];

	f->ns = ktime_get_ns();
	f->seq = sb->seq;
	f->hash = sb->hash;
	f->repeat = repeat;
	stream->sent_head = (stream->sent_head + 1) % FL2000_FRAME_RING;
	stream->sent_count++;
}

static void fl2000_stream_zlp_completion(struct urb *urb)
{
	usb_free_urb(urb);
}

/* A frame that is not a whole number of packets ends in a short packet, and URB_ZERO_PACKET then
 * sends nothing more. The chip does not take a short packet as the end of a frame (measured: a
 * 224x240 RGB888 frame, 157.5 packets, ran at 88 Hz instead of 60), so follow such frames with an
 * explicit zero length packet
 */
static void fl2000_stream_send_zlp(struct fl2000_stream *stream)
{
	struct usb_device *usb_dev = stream->usb_dev;
	unsigned int pipe = usb_sndbulkpipe(usb_dev, 1);
	struct urb *zlp_urb;

	if (!(stream->buf_size % usb_maxpacket(usb_dev, pipe)))
		return;

	zlp_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!zlp_urb)
		return;

	usb_fill_bulk_urb(zlp_urb, usb_dev, pipe, NULL, 0, fl2000_stream_zlp_completion, NULL);
	usb_anchor_urb(zlp_urb, &stream->anchor);
	if (fl2000_submit_urb(zlp_urb)) {
		usb_unanchor_urb(zlp_urb);
		usb_free_urb(zlp_urb);
	}
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000_stream *stream = container_of(work, struct fl2000_stream, work);
	struct usb_device *usb_dev = stream->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct fl2000_stream_buf *last_sb;
	struct urb *data_urb;

	while (stream->enabled) {
		unsigned long flags;

		ret = down_interruptible(&stream->work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			stream->enabled = false;
			return;
		}

		/* Disable wakes us through the semaphore precisely so we can exit: URB
		 * completions are the only other source of ups, and they stop (or never come,
		 * on hot-unplug) once streaming winds down. Without this check the worker sat
		 * parked in down_interruptible() forever and drain_workqueue() in
		 * fl2000_stream_disable() deadlocked inside an atomic commit that held every
		 * modeset lock - freezing all displays system-wide
		 */
		if (!stream->enabled)
			return;

		spin_lock_irqsave(&stream->list_lock, flags);

		/* If no buffers are available for immediate transmission - then copy latest
		 * transmission data
		 */
		if (list_empty(&stream->transmit_list)) {
			if (list_empty(&stream->wait_list))
				last_sb = list_last_entry(&stream->render_list,
							  struct fl2000_stream_buf, list);
			else
				last_sb = list_last_entry(&stream->wait_list,
							  struct fl2000_stream_buf, list);
			cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf,
						  list);
			memcpy(cur_sb->vaddr, last_sb->vaddr, stream->buf_size);
			cur_sb->seq = last_sb->seq;
			cur_sb->hash = last_sb->hash;
			fl2000_stream_record_sent(stream, cur_sb, true);
		} else {
			cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf,
						  list);
			fl2000_stream_record_sent(stream, cur_sb, false);
		}
		list_move_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			stream->enabled = false;
			return;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), cur_sb,
				  (int)stream->buf_size, fl2000_stream_data_completion, stream);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &stream->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			/* Must unanchor before dropping the reference: a failed-submit URB left
			 * in the anchor is never completed and never given back, and teardown's
			 * kill loop chokes on it - observed as a kworker live-spinning in
			 * usb_kill_anchored_urbs() inside disconnect for tens of minutes while
			 * holding the USB device mutex, wedging the whole port
			 */
			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);
			stream->enabled = false;
		} else {
			fl2000_stream_send_zlp(stream);
		}
	}
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int xx = 0;

	for (unsigned int x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	void *dst;
	u32 dst_line_len;
	unsigned long flags;

	spin_lock_irqsave(&stream->list_lock, flags);

	/* No buffer to render into: the stream is being torn down or rebuilt (seen when the device
	 * was unbound and rebound while fbdev restored its mode). Drop the frame; a BUG_ON here
	 * killed the kworker with every modeset lock held and wedged the machine
	 */
	if (list_empty(&stream->render_list)) {
		stream->dropped++;
		spin_unlock_irqrestore(&stream->list_lock, flags);
		return;
	}

	cur_sb = list_first_entry(&stream->render_list, struct fl2000_stream_buf, list);
	dst = cur_sb->vaddr;
	dst_line_len = width * stream->bytes_pix;

	/* The buffer is sized for the mode, the copy for the framebuffer: never write past it */
	if (dst_line_len && height * dst_line_len > stream->buf_size)
		height = stream->buf_size / dst_line_len;

	if (fl2000_test_fill) {
		/* Diagnostic: ignore the framebuffer and transmit solid white. The chip paces
		 * bulk URBs at frame rate, so it demonstrably consumes what we send; a white
		 * screen here proves the chip->DAC->monitor data path and isolates a black
		 * screen to the framebuffer copy path, while a black screen with this set
		 * means the chip is discarding/blanking pixel data internally
		 */
		memset(dst, 0xff, stream->buf_size);
	} else {
		for (unsigned int y = 0; y < height; y++) {
			switch (stream->bytes_pix) {
			case 2:
				fl2000_xrgb888_to_rgb565_line(dst, src, width);
				break;
			case 3:
				fl2000_xrgb888_to_rgb888_line(dst, src, width);
				break;
			default: /* Shouldn't happen */
				break;
			}
			src += pitch;
			dst += dst_line_len;
		}
	}

	/* Sampled FNV-1a hash: cheap, and enough to tell frames apart in the debug log */
	cur_sb->hash = 2166136261u;
	for (size_t i = 0; i < stream->buf_size; i += 97)
		cur_sb->hash = (cur_sb->hash ^ ((u8 *)cur_sb->vaddr)[i]) * 16777619u;
	cur_sb->seq = ++stream->frame_seq;

	list_move_tail(&cur_sb->list, &stream->transmit_list);
	spin_unlock_irqrestore(&stream->list_lock, flags);
}

int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix)
{
	int ret;
	unsigned int size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~7U;

	stream->bytes_pix = bytes_pix;

	/* If there are buffers with same size - keep them */
	if (stream->buf_size == size)
		return 0;

	/* Destroy wrong size buffers if they exist */
	if (!list_empty(&stream->render_list))
		fl2000_stream_put_buffers(stream);

	/* Allocate new buffers */
	ret = fl2000_stream_get_buffers(stream, size);
	if (ret) {
		fl2000_stream_put_buffers(stream);
		stream->buf_size = 0;
		return ret;
	}

	stream->buf_size = size;

	return 0;
}

int fl2000_stream_enable(struct fl2000_stream *stream)
{
	BUG_ON(list_empty(&stream->transmit_list));

	/* The anchor is empty here; re-init clears any poisoned state a previous disable left
	 * behind, which would otherwise auto-reject every URB of the new session
	 */
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (int i = 0; i < FL2000_SB_MIN; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct fl2000_stream_buf *cur_sb;

	stream->enabled = false;

	/* Unblock a worker parked on the semaphore so drain can complete; the worker checks
	 * 'enabled' on wake and exits
	 */
	up(&stream->work_sem);

	drain_workqueue(stream->work_queue);

	/* Poison rather than kill: poisoning also rejects any URB that races in afterwards.
	 * Anything still anchored after that (e.g. a failed-submit straggler that can never
	 * complete) is scuttled - dropped from the anchor without waiting - because waiting on
	 * a URB the HCD will never give back spins forever on a glitched/dead port. The next
	 * fl2000_stream_enable() starts from a clean semaphore; re-init the anchor state too
	 */
	if (!usb_wait_anchor_empty_timeout(&stream->anchor, 1000)) {
		usb_poison_anchored_urbs(&stream->anchor);
		usb_scuttle_anchored_urbs(&stream->anchor);
	}

	spin_lock_irq(&stream->list_lock);
	while (!list_empty(&stream->transmit_list)) {
		cur_sb = list_first_entry(&stream->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	while (!list_empty(&stream->wait_list)) {
		cur_sb = list_first_entry(&stream->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &stream->render_list);
	}
	spin_unlock(&stream->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 *
 * Return: Operation result
 */
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc)
{
	int ret;
	struct fl2000_stream *stream;

	/* Altsettung 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers");
		return ERR_PTR(ret);
	}

	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		dev_err(&usb_dev->dev, "Cannot allocate stream");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, stream);

	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_LIST_HEAD(&stream->render_list);
	INIT_LIST_HEAD(&stream->transmit_list);
	INIT_LIST_HEAD(&stream->wait_list);
	spin_lock_init(&stream->list_lock);
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->usb_dev = usb_dev;
	stream->crtc = crtc;

	stream->work_queue = create_workqueue("fl2000_stream");
	if (!stream->work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	return stream;
}

static struct fl2000_stream *fl2000_stream_find(struct usb_device *usb_dev)
{
	return devres_find(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}

int fl2000_stream_get_stats(struct usb_device *usb_dev, struct fl2000_frame_stats *stats)
{
	struct fl2000_stream *stream = fl2000_stream_find(usb_dev);
	unsigned int len;

	if (!stream)
		return -ENODEV;

	spin_lock_irq(&stream->list_lock);
	stats->count = stream->stat_count;
	stats->first_ns = stream->stat_first_ns;
	stats->last_ns = stream->stat_last_ns;
	len = min_t(u64, stream->stat_count, FL2000_FRAME_RING);
	stats->ring_len = len;
	for (unsigned int i = 0; i < len; i++)
		stats->ring[i] = stream->stat_ring[(stream->stat_head + FL2000_FRAME_RING - len + i) %
						   FL2000_FRAME_RING];
	spin_unlock_irq(&stream->list_lock);

	return 0;
}

int fl2000_stream_get_sent(struct usb_device *usb_dev, struct fl2000_sent_frame *out,
			   unsigned int *len, u64 *dropped, u32 *frame_seq)
{
	struct fl2000_stream *stream = fl2000_stream_find(usb_dev);
	unsigned int n;

	if (!stream)
		return -ENODEV;

	spin_lock_irq(&stream->list_lock);
	n = min_t(u64, stream->sent_count, FL2000_FRAME_RING);
	for (unsigned int i = 0; i < n; i++)
		out[i] = stream->sent[(stream->sent_head + FL2000_FRAME_RING - n + i) %
				     FL2000_FRAME_RING];
	*len = n;
	*dropped = stream->dropped;
	*frame_seq = stream->frame_seq;
	spin_unlock_irq(&stream->list_lock);

	return 0;
}

void fl2000_stream_reset_stats(struct usb_device *usb_dev)
{
	struct fl2000_stream *stream = fl2000_stream_find(usb_dev);

	if (!stream)
		return;

	spin_lock_irq(&stream->list_lock);
	stream->stat_count = 0;
	stream->stat_head = 0;
	stream->sent_count = 0;
	stream->sent_head = 0;
	stream->dropped = 0;
	spin_unlock_irq(&stream->list_lock);
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
