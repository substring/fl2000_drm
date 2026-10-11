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
/* Plus one being converted by fl2000_stream_compress() and one free, so the worker always has a
 * buffer to resend the last frame into
 */
#define FL2000_SB_NUM (FL2000_SB_MIN + 2)

#define FL2000_URB_TIMEOUT 100

/* Transfers kept queued to the chip. Each one beyond the first is a frame of latency; the
 * risk of a single one is the chip running dry between two transfers. Measured on a FL2000DX
 * with all 8 cores saturated plus disk writes: no frame lost at depth 1. Deeper queues remain
 * selectable as a fallback
 */
/* Diagnostics: per-frame content hash and log of what was sent (debugfs "sent") */
static bool fl2000_debug_frames;
module_param_named(debug_frames, fl2000_debug_frames, bool, 0644);
MODULE_PARM_DESC(debug_frames, "Hash every frame and log what is sent, for debugfs \"sent\" "
		 "(default 0)");

static unsigned int fl2000_queue_depth = 1;
module_param_named(queue_depth, fl2000_queue_depth, uint, 0644);
MODULE_PARM_DESC(queue_depth, "Frames queued to the chip, 1-3 (default 1, lowest latency), "
		 "applied when the display is next enabled");

struct fl2000_stream_buf {
	struct list_head list;
	struct sg_table sgt;
	struct page **pages;
	unsigned int nr_pages;
	void *vaddr;
	u32 seq;	/* frame number given by fl2000_stream_compress(), 0 for none yet */
	u32 hash;	/* sampled content hash, to tell frames apart in the debug log */
	u64 ready_ns;	/* when the conversion finished */
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

	/* Page flip event waiting for frame event_seq to reach the screen, under list_lock */
	struct drm_pending_vblank_event *event;
	u32 event_seq;

	/* What was sent, frame by frame, under list_lock: debugfs "sent" */
	u32 frame_seq;
	u64 dropped;
	u64 superseded;
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

/* Under list_lock: take the pending event if frame seq, now starting on screen, carries it */
static struct drm_pending_vblank_event *fl2000_stream_take_event(struct fl2000_stream *stream,
								  u32 seq)
{
	struct drm_pending_vblank_event *event = stream->event;

	if (!event || (s32)(seq - stream->event_seq) < 0)
		return NULL;

	stream->event = NULL;
	return event;
}

static void fl2000_stream_send_event(struct fl2000_stream *stream,
				     struct drm_pending_vblank_event *event)
{
	struct drm_device *drm = stream->crtc->dev;
	unsigned long flags;

	if (!event)
		return;

	spin_lock_irqsave(&drm->event_lock, flags);
	drm_crtc_send_vblank_event(stream->crtc, event);
	spin_unlock_irqrestore(&drm->event_lock, flags);
}

/**
 * fl2000_stream_queue_event() - complete a page flip once its frame reaches the screen
 * @stream: the stream
 * @event: the event of the commit that just converted its frame, or of one that did not
 *	   change the picture
 *
 * The chip has no frame memory: a frame starts on screen when the transfer before it
 * completes. Sending the event then gives userspace honest flip timing, where arming it on
 * the vblank counter needed accurate vblank timestamps this device cannot provide (and WARNed)
 */
void fl2000_stream_queue_event(struct fl2000_stream *stream, struct drm_pending_vblank_event *event)
{
	struct drm_pending_vblank_event *old;
	unsigned long flags;

	spin_lock_irqsave(&stream->list_lock, flags);
	old = stream->event;
	stream->event = event;
	stream->event_seq = stream->frame_seq;
	if (!stream->enabled)
		event = fl2000_stream_take_event(stream, stream->event_seq);
	else
		event = NULL;
	spin_unlock_irqrestore(&stream->list_lock, flags);

	/* Commits wait for flip_done, so there is never an older one; be safe anyway */
	fl2000_stream_send_event(stream, old);
	fl2000_stream_send_event(stream, event);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->transfer_buffer;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_stream *stream = urb->context;

	if (stream) {
		unsigned long flags;

		struct drm_pending_vblank_event *event = NULL;

		spin_lock_irqsave(&stream->list_lock, flags);
		list_move_tail(&cur_sb->list, &stream->render_list);
		/* Whatever goes out next is what the chip shows from now on: the next queued
		 * transfer, or with nothing queued, the newest converted frame the worker is about
		 * to submit. Completing the flip here, from the interrupt, keeps scheduling delays
		 * of the worker out of the timing userspace sees
		 */
		if (!list_empty(&stream->wait_list))
			event = fl2000_stream_take_event(stream,
							 list_first_entry(&stream->wait_list,
									  struct fl2000_stream_buf,
									  list)->seq);
		else if (!list_empty(&stream->transmit_list))
			event = fl2000_stream_take_event(stream,
							 list_last_entry(&stream->transmit_list,
									 struct fl2000_stream_buf,
									 list)->seq);
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
		fl2000_stream_send_event(stream, event);

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

	if (!fl2000_debug_frames)
		return;

	f->ns = ktime_get_ns();
	f->ready_ns = sb->ready_ns;
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
	struct drm_pending_vblank_event *event;
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
		if (list_empty(&stream->transmit_list) && list_empty(&stream->render_list)) {
			/* Every buffer is in flight or being converted: try again shortly rather
			 * than lose this transmission slot for good
			 */
			spin_unlock_irqrestore(&stream->list_lock, flags);
			usleep_range(500, 1000);
			up(&stream->work_sem);
			continue;
		}

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
			cur_sb->ready_ns = last_sb->ready_ns;
			fl2000_stream_record_sent(stream, cur_sb, true);
		} else {
			/* Latest wins: frames that were overtaken before reaching the wire go back
			 * to the free list instead of being shown late
			 */
			cur_sb = list_last_entry(&stream->transmit_list, struct fl2000_stream_buf,
						 list);
			while (list_first_entry(&stream->transmit_list, struct fl2000_stream_buf,
						list) != cur_sb) {
				list_move_tail(stream->transmit_list.next, &stream->render_list);
				stream->superseded++;
			}
			fl2000_stream_record_sent(stream, cur_sb, false);
		}
		/* At the latest when its frame is handed to the chip: with a deeper queue that is a
		 * frame or two before it shows, but waiting longer made the next nonblocking commit
		 * fail with EBUSY, since a blocking one only waits for one vblank
		 */
		event = fl2000_stream_take_event(stream, cur_sb->seq);
		list_move_tail(&cur_sb->list, &stream->wait_list);
		spin_unlock_irqrestore(&stream->list_lock, flags);
		fl2000_stream_send_event(stream, event);

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

void fl2000_stream_compress(struct fl2000_stream *stream, const struct iosys_map *src_map,
			    unsigned int height, unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	struct iosys_map src = *src_map;
	struct iosys_map line_map;
	u32 *line = NULL;
	u8 *dst;
	u32 dst_line_len;
	unsigned long flags;

	/* Take a buffer off the free list and convert into it with the lock dropped and
	 * interrupts on. Converting under the lock stalled URB completions and resubmission for
	 * the whole conversion: 58-68 ms per frame when reading a PRIME buffer rendered by the
	 * GPU, so the chip ran dry and showed every other frame black
	 */
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
	list_del_init(&cur_sb->list);
	spin_unlock_irqrestore(&stream->list_lock, flags);

	dst = cur_sb->vaddr;
	dst_line_len = width * stream->bytes_pix;

	/* The buffer is sized for the mode, the copy for the framebuffer: never write past it */
	if (dst_line_len && height * dst_line_len > stream->buf_size)
		height = stream->buf_size / dst_line_len;

	if (!fl2000_test_fill) {
		line = kmalloc_array(width, sizeof(*line), GFP_KERNEL);
		if (!line) {
			spin_lock_irqsave(&stream->list_lock, flags);
			list_add(&cur_sb->list, &stream->render_list);
			stream->dropped++;
			spin_unlock_irqrestore(&stream->list_lock, flags);
			return;
		}
		iosys_map_set_vaddr(&line_map, line);
	}

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
			/* A buffer rendered by the GPU is write-combined: plain CPU reads of it run
			 * at about 10 MB/s. Pull each line with streaming loads into cached memory
			 * first, then convert from there
			 */
			drm_memcpy_from_wc(&line_map, &src, width * sizeof(*line));
			iosys_map_incr(&src, pitch);

			switch (stream->bytes_pix) {
			case 2:
				fl2000_xrgb888_to_rgb565_line((u16 *)dst, line, width);
				break;
			case 3:
				fl2000_xrgb888_to_rgb888_line(dst, line, width);
				break;
			default: /* Shouldn't happen */
				break;
			}
			dst += dst_line_len;
		}
	}
	kfree(line);

	/* Sampled FNV-1a hash: cheap, and enough to tell frames apart in the debug log */
	cur_sb->hash = 0;
	if (fl2000_debug_frames) {
		cur_sb->hash = 2166136261u;
		for (size_t i = 0; i < stream->buf_size; i += 97)
			cur_sb->hash = (cur_sb->hash ^ ((u8 *)cur_sb->vaddr)[i]) * 16777619u;
	}

	spin_lock_irqsave(&stream->list_lock, flags);
	cur_sb->seq = ++stream->frame_seq;
	cur_sb->ready_ns = ktime_get_ns();
	list_add_tail(&cur_sb->list, &stream->transmit_list);
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
	unsigned int depth = clamp(fl2000_queue_depth, 1U, (unsigned int)FL2000_SB_MIN);

	/* Nothing converted yet is fine: the worker resends the last buffer, as it does whenever
	 * no new frame is ready (this was a BUG_ON)
	 */
	/* The anchor is empty here; re-init clears any poisoned state a previous disable left
	 * behind, which would otherwise auto-reject every URB of the new session
	 */
	init_usb_anchor(&stream->anchor);
	sema_init(&stream->work_sem, 0);
	stream->enabled = true;
	queue_work(stream->work_queue, &stream->work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (unsigned int i = 0; i < depth; i++)
		up(&stream->work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000_stream *stream)
{
	struct drm_pending_vblank_event *event;
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

	/* Nothing will reach the screen any more: complete a flip still waiting for it */
	spin_lock_irq(&stream->list_lock);
	event = stream->event;
	stream->event = NULL;
	spin_unlock_irq(&stream->list_lock);
	fl2000_stream_send_event(stream, event);

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
			   unsigned int *len, u64 *dropped, u64 *superseded, u32 *frame_seq)
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
	*superseded = stream->superseded;
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
	stream->superseded = 0;
	spin_unlock_irq(&stream->list_lock);
}

void fl2000_stream_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_stream_release, NULL, NULL);
}
