// SPDX-License-Identifier: GPL-2.0
/*
 * Debugfs interface for reverse engineering the chip: raw register access and frame pacing
 * statistics. Lives in /sys/kernel/debug/fl2000-<usb device name>/
 *
 *  reg     write "ADDR" to select a register, "ADDR VAL" to write it; read returns the selected
 *          register, freshly read from the chip
 *  dump    read the whole control register space, bypassing the regmap cache
 *  frames  read frame pacing statistics from stream URB completions; write anything to reset
 *  ring    read the latest raw completion timestamps (ns), one per line, oldest first
 *  sent    read the latest frames sent, oldest first: "ns seq hash new|rep ready_ns", after a
 *          header with the frames rendered, dropped and superseded so far (reset through
 *          "frames")
 *  pokes   bit overrides applied during every modeset, before the chip reset and again at the
 *          end: write "REG SETMASK CLRMASK" to add one, "clear" to drop them all; read lists them.
 *          For bits that only latch on reset, which live register writes cannot reveal
 */

#include "fl2000.h"

/* Register windows worth dumping: USB control bank and VGA control bank */
static const struct {
	u32 start;
	u32 end;
} fl2000_dump_ranges[] = {
	{ 0x0000, 0x00FC },
	{ 0x8000, 0x80FC },
};

#define FL2000_MAX_POKES 8

/* Module wide: there is only ever one dongle under test */
static struct {
	u32 reg;
	u32 set;
	u32 clr;
} fl2000_pokes[FL2000_MAX_POKES];
static unsigned int fl2000_num_pokes;
static DEFINE_MUTEX(fl2000_pokes_lock);

void fl2000_debugfs_apply_pokes(struct usb_device *usb_dev)
{
	mutex_lock(&fl2000_pokes_lock);
	for (unsigned int i = 0; i < fl2000_num_pokes; i++) {
		u32 val;

		if (fl2000_reg_raw_read(usb_dev, fl2000_pokes[i].reg, &val))
			continue;
		val = (val & ~fl2000_pokes[i].clr) | fl2000_pokes[i].set;
		fl2000_reg_raw_write(usb_dev, fl2000_pokes[i].reg, val);
	}
	mutex_unlock(&fl2000_pokes_lock);
}

static int fl2000_debugfs_pokes_show(struct seq_file *s, void *unused)
{
	mutex_lock(&fl2000_pokes_lock);
	for (unsigned int i = 0; i < fl2000_num_pokes; i++)
		seq_printf(s, "0x%04x set 0x%08x clr 0x%08x\n", fl2000_pokes[i].reg,
			   fl2000_pokes[i].set, fl2000_pokes[i].clr);
	mutex_unlock(&fl2000_pokes_lock);

	return 0;
}

static int fl2000_debugfs_pokes_open(struct inode *inode, struct file *file)
{
	return single_open(file, fl2000_debugfs_pokes_show, inode->i_private);
}

static ssize_t fl2000_debugfs_pokes_write(struct file *file, const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	char buf[48];
	u32 reg, set, clr;
	int ret = count;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	mutex_lock(&fl2000_pokes_lock);
	if (!strncmp(buf, "clear", 5)) {
		fl2000_num_pokes = 0;
	} else if (sscanf(buf, "%x %x %x", &reg, &set, &clr) == 3 && reg <= 0xFFFF && !(reg & 3)) {
		if (fl2000_num_pokes < FL2000_MAX_POKES) {
			fl2000_pokes[fl2000_num_pokes].reg = reg;
			fl2000_pokes[fl2000_num_pokes].set = set;
			fl2000_pokes[fl2000_num_pokes].clr = clr;
			fl2000_num_pokes++;
		} else {
			ret = -ENOSPC;
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&fl2000_pokes_lock);

	return ret;
}

static const struct file_operations fl2000_debugfs_pokes_fops = {
	.owner = THIS_MODULE,
	.open = fl2000_debugfs_pokes_open,
	.read = seq_read,
	.write = fl2000_debugfs_pokes_write,
	.llseek = seq_lseek,
	.release = single_release,
};

struct fl2000_debugfs {
	struct usb_device *usb_dev;
	struct dentry *dir;
	struct mutex lock; /* Serializes the selected register */
	u32 reg;
};

static ssize_t fl2000_debugfs_reg_read(struct file *file, char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct fl2000_debugfs *dbg = file->private_data;
	char buf[32];
	u32 reg, val;
	int len, ret;

	mutex_lock(&dbg->lock);
	reg = dbg->reg;
	ret = fl2000_reg_raw_read(dbg->usb_dev, reg, &val);
	mutex_unlock(&dbg->lock);
	if (ret)
		return ret < 0 ? ret : -EIO;

	len = scnprintf(buf, sizeof(buf), "0x%04x 0x%08x\n", reg, val);

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t fl2000_debugfs_reg_write(struct file *file, const char __user *ubuf, size_t count,
					loff_t *ppos)
{
	struct fl2000_debugfs *dbg = file->private_data;
	char buf[32];
	u32 reg, val;
	int n, ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	n = sscanf(buf, "%x %x", &reg, &val);
	if (n < 1 || reg > 0xFFFF || reg & 3)
		return -EINVAL;

	mutex_lock(&dbg->lock);
	dbg->reg = reg;
	if (n == 2)
		ret = fl2000_reg_raw_write(dbg->usb_dev, reg, val);
	mutex_unlock(&dbg->lock);
	if (ret)
		return ret < 0 ? ret : -EIO;

	return count;
}

static const struct file_operations fl2000_debugfs_reg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = fl2000_debugfs_reg_read,
	.write = fl2000_debugfs_reg_write,
	.llseek = default_llseek,
};

static int fl2000_debugfs_dump_show(struct seq_file *s, void *unused)
{
	struct fl2000_debugfs *dbg = s->private;

	for (int r = 0; r < ARRAY_SIZE(fl2000_dump_ranges); r++) {
		for (u32 reg = fl2000_dump_ranges[r].start; reg <= fl2000_dump_ranges[r].end;
		     reg += 4) {
			u32 val;

			if (fl2000_reg_raw_read(dbg->usb_dev, reg, &val))
				seq_printf(s, "0x%04x error\n", reg);
			else
				seq_printf(s, "0x%04x 0x%08x\n", reg, val);
		}
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(fl2000_debugfs_dump);

static int fl2000_debugfs_frames_show(struct seq_file *s, void *unused)
{
	struct fl2000_debugfs *dbg = s->private;
	struct fl2000_frame_stats *stats;
	u64 min_dt = U64_MAX, max_dt = 0, sum_dt = 0, sum_sq = 0, mean, var;
	unsigned int n;
	int ret;

	stats = kvmalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	ret = fl2000_stream_get_stats(dbg->usb_dev, stats);
	if (ret)
		goto out;

	seq_printf(s, "count %llu\n", stats->count);
	if (stats->count > 1) {
		u64 span = stats->last_ns - stats->first_ns;

		/* Whole-session mean: immune to jitter, the number to compare against theory */
		seq_printf(s, "mean_period_ns %llu.%03llu\n",
			   div64_u64(span, stats->count - 1),
			   div64_u64((span % (stats->count - 1)) * 1000, stats->count - 1));
		seq_printf(s, "mean_millihz %llu\n",
			   div64_u64((stats->count - 1) * 1000000000000ull, span));
	}

	/* Spread over the latest completions only */
	n = stats->ring_len;
	if (n > 2) {
		for (unsigned int i = 1; i < n; i++) {
			u64 dt = stats->ring[i] - stats->ring[i - 1];

			min_dt = min(min_dt, dt);
			max_dt = max(max_dt, dt);
			sum_dt += dt;
		}
		mean = div64_u64(sum_dt, n - 1);
		for (unsigned int i = 1; i < n; i++) {
			s64 d = (s64)(stats->ring[i] - stats->ring[i - 1]) - (s64)mean;

			sum_sq += (u64)(d * d);
		}
		var = div64_u64(sum_sq, n - 1);
		seq_printf(s, "ring %u min_ns %llu max_ns %llu stddev_ns %llu\n", n, min_dt, max_dt,
			   (u64)int_sqrt64(var));
	}

out:
	kvfree(stats);
	return ret;
}

static int fl2000_debugfs_frames_open(struct inode *inode, struct file *file)
{
	return single_open(file, fl2000_debugfs_frames_show, inode->i_private);
}

static ssize_t fl2000_debugfs_frames_write(struct file *file, const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct fl2000_debugfs *dbg = s->private;

	fl2000_stream_reset_stats(dbg->usb_dev);

	return count;
}

static const struct file_operations fl2000_debugfs_frames_fops = {
	.owner = THIS_MODULE,
	.open = fl2000_debugfs_frames_open,
	.read = seq_read,
	.write = fl2000_debugfs_frames_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int fl2000_debugfs_ring_show(struct seq_file *s, void *unused)
{
	struct fl2000_debugfs *dbg = s->private;
	struct fl2000_frame_stats *stats;
	int ret;

	stats = kvmalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	ret = fl2000_stream_get_stats(dbg->usb_dev, stats);
	if (!ret)
		for (unsigned int i = 0; i < stats->ring_len; i++)
			seq_printf(s, "%llu\n", stats->ring[i]);

	kvfree(stats);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(fl2000_debugfs_ring);

static int fl2000_debugfs_sent_show(struct seq_file *s, void *unused)
{
	struct fl2000_debugfs *dbg = s->private;
	struct fl2000_sent_frame *sent;
	unsigned int len;
	u64 dropped, superseded;
	u32 frame_seq;
	int ret;

	sent = kvmalloc_array(FL2000_FRAME_RING, sizeof(*sent), GFP_KERNEL);
	if (!sent)
		return -ENOMEM;

	ret = fl2000_stream_get_sent(dbg->usb_dev, sent, &len, &dropped, &superseded, &frame_seq);
	if (!ret) {
		seq_printf(s, "# rendered %u dropped %llu superseded %llu\n", frame_seq, dropped,
			   superseded);
		for (unsigned int i = 0; i < len; i++)
			seq_printf(s, "%llu %u %08x %s %llu\n", sent[i].ns, sent[i].seq, sent[i].hash,
				   sent[i].repeat ? "rep" : "new", sent[i].ready_ns);
	}

	kvfree(sent);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(fl2000_debugfs_sent);

/* Not devres managed on the USB device: that outlives this module, see fl2000_put_devices() */
struct fl2000_debugfs *fl2000_debugfs_init(struct usb_device *usb_dev)
{
	struct fl2000_debugfs *dbg;
	char name[64];

	dbg = kzalloc(sizeof(*dbg), GFP_KERNEL);
	if (!dbg)
		return ERR_PTR(-ENOMEM);
	dbg->usb_dev = usb_dev;
	dbg->reg = FL2000_VGA_STATUS_REG;
	mutex_init(&dbg->lock);

	snprintf(name, sizeof(name), "fl2000-%s", dev_name(&usb_dev->dev));
	dbg->dir = debugfs_create_dir(name, NULL);

	debugfs_create_file("reg", 0600, dbg->dir, dbg, &fl2000_debugfs_reg_fops);
	debugfs_create_file("dump", 0400, dbg->dir, dbg, &fl2000_debugfs_dump_fops);
	debugfs_create_file("frames", 0600, dbg->dir, dbg, &fl2000_debugfs_frames_fops);
	debugfs_create_file("pokes", 0600, dbg->dir, dbg, &fl2000_debugfs_pokes_fops);
	debugfs_create_file("ring", 0400, dbg->dir, dbg, &fl2000_debugfs_ring_fops);
	debugfs_create_file("sent", 0400, dbg->dir, dbg, &fl2000_debugfs_sent_fops);

	return dbg;
}

void fl2000_debugfs_cleanup(struct fl2000_debugfs *dbg)
{
	if (IS_ERR_OR_NULL(dbg))
		return;

	/* Waits for open files to be done with dbg */
	debugfs_remove_recursive(dbg->dir);
	mutex_destroy(&dbg->lock);
	kfree(dbg);
}
