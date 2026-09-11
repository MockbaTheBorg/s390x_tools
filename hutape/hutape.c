// SPDX-License-Identifier: GPL-2.0
/*
 * hutape.c -- native CCW driver for Hercules-emulated IBM 3480/3490/3590
 * tape drives, in PARALLEL with the in-tree tape_34xx driver (not a
 * replacement for it).
 *
 * tape_34xx already claims and works fine on the 3490 (0580) in this
 * config. It refuses the 3480 (0560) because Hercules emulates a real
 * historical CU model (0x3480/0x31, "D31") that isn't in tape_34xx's
 * compiled-in match table, and there is no tape_3590 driver at all in
 * this kernel build for the 3590 (0590).
 *
 * This module's match table (ht_ids[]) includes all three CU types
 * (0x3480/0x3490/0x3590, any model), so it's ABLE to drive a 3490 too --
 * but it does not forcibly take 0580 away from tape_34xx. The ccw-bus
 * core binds a device to a driver once (at device-detection time / on
 * each driver's registration, whichever comes first) and skips devices
 * that already have a bound driver -- it never re-matches or steals an
 * already-bound device from another driver.
 *
 * Which driver actually ends up with 0580 depends on module LOAD ORDER
 * at boot, which is not guaranteed: if tape_34xx loads first, it keeps
 * 0580 and hutape's probe for it is simply skipped; if hutape's
 * modules-load.d entry runs before tape_34xx has loaded (observed after
 * a real reboot: tape_34xx not loaded at all yet), hutape binds all
 * three CU types including 0580. Both outcomes are fine -- this driver
 * works correctly for a 3490 too, so the 0x3490 table entry exists
 * specifically to serve as that fallback. hutape-online.sh reflects
 * this: it onlines whatever ends up under
 * /sys/bus/ccw/drivers/hutape/ (see docs/hutape-driver.md), which for
 * 0580 is conditional on this same race.
 *
 * Same proven approach as tools/hunitrec/ (see docs/hunitrec-driver.md):
 * ccw_device_start() + ccw_device_dma_zalloc(), one CCW per call, block
 * on a completion signalled from cdev->handler. CU types/models and CCW
 * opcodes below are taken directly from Hercules's own tapedev.c /
 * tapeccws.c, not just IBM docs:
 *
 *   3480 tape : CU 0x3480, any model (real device is model 0x31 "D31",
 *               tapedev.c:494)
 *   3590 tape : CU 0x3590, any model (real device is model 0x60 "A50",
 *               tapedev.c:496)
 *
 * CCW opcodes (tapeccws.c, generic across all three tape CU types --
 * verified for both 3480 and 3590 code paths, not just 3490):
 *   0x01 Write                         0x02 Read Forward / IPL Read
 *   0x03 Control No-Op                 0x04 Sense
 *   0x07 Rewind                        0x0F Rewind Unload
 *   0x17 Erase Gap                     0x1F Write Tape Mark
 *   0x27 Backspace Block               0x2F Backspace File
 *   0x37 Forward Space Block           0x3F Forward Space File
 *
 * Max block size is capped at 65535 bytes (0xFFFF) -- the CCW count
 * field is only 16 bits wide, so a single (non-chained) CCW can never
 * move more than that in one go, regardless of the 256KB block Hercules
 * would otherwise allow for the 3590 (tapedev.h BLK256). Not an issue
 * for realistic block sizes (tar/dd typically use 10-64KB).
 *
 * No EBCDIC translation here (unlike hunitrec) -- tape data is an opaque
 * byte stream (tar archives, dd images, etc.), passed through as-is.
 *
 * Positioning (rewind/space/weof/erase) is exposed via the standard
 * MTIOCTOP ioctl (linux/mtio.h) so the stock `mt` command works
 * unmodified: `mt -f /dev/htp0 rewind`, `mt -f /dev/htp0 fsf 1`, etc.
 * Bulk I/O is plain read()/write() -- `dd` is the right tool (one
 * read(2)/write(2) per dd block = one CCW = one tape block; use
 * bs=65536 or smaller, see tools/hutape/README.md).
 *
 * Device nodes are 0600 (root-only), unlike hunitrec's 0666: unlike
 * print lines and punched cards, a tape write/erase/rewind is a
 * destructive operation on the mounted volume, so this stays root-only
 * by default rather than following the "no sudo needed" pattern.
 *
 * Bind/online like any other ccw device:
 *   modprobe hutape
 *   chccwdev -e 0.0.0560   # 3480
 *   chccwdev -e 0.0.0590   # 3590
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/mtio.h>
#include <asm/ccwdev.h>
#include <asm/cio.h>
#include <asm/scsw.h>

#define TAPE_MAX_BLK 65535U	/* CCW count field is 16-bit */

/* Tape CCW opcodes (tapeccws.c) */
#define OP_WRITE       0x01
#define OP_READ_FWD    0x02
#define OP_NOP         0x03
#define OP_SENSE       0x04
#define OP_REWIND      0x07
#define OP_REWIND_UL   0x0F
#define OP_ERASE_GAP   0x17
#define OP_WRITE_TM    0x1F
#define OP_BACKSP_BLK  0x27
#define OP_BACKSP_FILE 0x2F
#define OP_FWDSP_BLK   0x37
#define OP_FWDSP_FILE  0x3F

struct ht_dev {
	struct ccw_device *cdev;
	struct miscdevice misc;
	struct mutex lock;
	struct completion done;
	struct irb last_irb;
	char name[16];
};

static void ht_int_handler(struct ccw_device *cdev, unsigned long intparm,
			    struct irb *irb)
{
	struct ht_dev *ht = dev_get_drvdata(&cdev->dev);

	if (!ht)
		return;
	memcpy(&ht->last_irb, irb, sizeof(*irb));
	complete(&ht->done);
}

/* Issue one CCW and wait for completion. Caller holds ht->lock.
 * dma_addr/len may be 0 for control commands (no data phase).
 * On return: *residual = bytes NOT transferred (SCSW count field),
 * *dstat = device status byte. Returns 0 on any status-pending
 * completion (caller inspects *dstat for UNIT_CHECK/UNIT_EXCEP),
 * -ETIMEDOUT, or -ENOMEM. */
static int ht_do_io(struct ht_dev *ht, u8 cmd_code, dma32_t dma_addr, u16 len,
		     u16 *residual, u8 *dstat)
{
	struct ccw1 *ccw;
	dma32_t ccw_dma;
	int rc;

	ccw = ccw_device_dma_zalloc(ht->cdev, sizeof(*ccw), &ccw_dma);
	if (!ccw)
		return -ENOMEM;

	ccw->cmd_code = cmd_code;
	ccw->flags = CCW_FLAG_SLI;
	ccw->count = len;
	ccw->cda = dma_addr;

	reinit_completion(&ht->done);
	rc = ccw_device_start(ht->cdev, ccw, (unsigned long)ht, 0xff, 0);
	if (rc)
		goto out;

	if (!wait_for_completion_timeout(&ht->done, 60 * HZ)) {
		rc = -ETIMEDOUT;
		goto out;
	}
	*dstat = scsw_dstat(&ht->last_irb.scsw);
	*residual = ht->last_irb.scsw.cmd.count;
	rc = 0;

out:
	ccw_device_dma_free(ht->cdev, ccw, sizeof(*ccw));
	return rc;
}

/* Control command with no data phase (rewind/space/weof/erase/nop).
 * Returns 0 on clean completion, -EIO on unit check, -ETIMEDOUT. */
static int ht_control(struct ht_dev *ht, u8 cmd_code)
{
	u16 residual;
	u8 dstat;
	int rc;

	rc = ht_do_io(ht, cmd_code, 0, 0, &residual, &dstat);
	if (rc)
		return rc;
	if (dstat & DEV_STAT_UNIT_CHECK)
		return -EIO;
	return 0;
}

/* ---- write(): one CCW per call, variable-length block ---- */

static ssize_t ht_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *pos)
{
	struct ht_dev *ht = container_of(file->private_data, struct ht_dev, misc);
	unsigned char *data;
	dma32_t data_dma;
	u16 residual;
	u8 dstat;
	int rc;

	if (count == 0)
		return 0;
	if (count > TAPE_MAX_BLK)
		count = TAPE_MAX_BLK;

	if (mutex_lock_interruptible(&ht->lock))
		return -ERESTARTSYS;

	data = ccw_device_dma_zalloc(ht->cdev, count, &data_dma);
	if (!data) {
		rc = -ENOMEM;
		goto out;
	}
	if (copy_from_user(data, ubuf, count)) {
		ccw_device_dma_free(ht->cdev, data, count);
		rc = -EFAULT;
		goto out;
	}

	rc = ht_do_io(ht, OP_WRITE, data_dma, count, &residual, &dstat);
	ccw_device_dma_free(ht->cdev, data, count);
	if (rc)
		goto out;
	if (dstat & DEV_STAT_UNIT_CHECK) {
		rc = -EIO;	/* e.g. write-protected volume */
		goto out;
	}
	rc = count;

out:
	mutex_unlock(&ht->lock);
	return rc;
}

/* ---- read(): one CCW per call, one block per read() regardless of
 * requested size -- always issues the CCW for the full TAPE_MAX_BLK so
 * a block larger than the caller's buffer is still consumed off the
 * tape (truncated to the caller's buffer), matching normal tape
 * semantics of "read() = one block". A 0-length return means tapemark
 * (Unit Exception with no Unit Check), not an error. ---- */

static ssize_t ht_read(struct file *file, char __user *ubuf, size_t count,
			loff_t *pos)
{
	struct ht_dev *ht = container_of(file->private_data, struct ht_dev, misc);
	unsigned char *data;
	dma32_t data_dma;
	u16 residual, xfer, n;
	u8 dstat;
	int rc;

	if (mutex_lock_interruptible(&ht->lock))
		return -ERESTARTSYS;

	data = ccw_device_dma_zalloc(ht->cdev, TAPE_MAX_BLK, &data_dma);
	if (!data) {
		rc = -ENOMEM;
		goto out;
	}

	rc = ht_do_io(ht, OP_READ_FWD, data_dma, TAPE_MAX_BLK, &residual, &dstat);
	if (rc) {
		ccw_device_dma_free(ht->cdev, data, TAPE_MAX_BLK);
		goto out;
	}
	if (dstat & DEV_STAT_UNIT_CHECK) {
		ccw_device_dma_free(ht->cdev, data, TAPE_MAX_BLK);
		rc = -EIO;
		goto out;
	}
	if (dstat & DEV_STAT_UNIT_EXCEP) {
		/* tapemark: not an error, just end-of-file on the volume */
		ccw_device_dma_free(ht->cdev, data, TAPE_MAX_BLK);
		rc = 0;
		goto out;
	}

	xfer = TAPE_MAX_BLK - residual;
	n = count < xfer ? count : xfer;
	if (copy_to_user(ubuf, data, n))
		rc = -EFAULT;
	else
		rc = n;
	ccw_device_dma_free(ht->cdev, data, TAPE_MAX_BLK);

out:
	mutex_unlock(&ht->lock);
	return rc;
}

/* ---- ioctl(MTIOCTOP): standard tape positioning, so the stock `mt`
 * command works unmodified. A tapemark hit mid-space (MTFSR/MTBSR/
 * MTFSF/MTBSF) stops the loop early and returns -EIO -- the caller
 * can't tell from mt(1) how far it actually got; check position with
 * the CCWs directly (a small limitation vs. a real st driver's
 * MTIOCGET, which this module does not implement). ---- */

static long ht_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ht_dev *ht = container_of(file->private_data, struct ht_dev, misc);
	struct mtop mt;
	u8 opcode;
	int repeat = 1, i, rc;

	if (cmd != MTIOCTOP)
		return -ENOTTY;
	if (copy_from_user(&mt, (void __user *)arg, sizeof(mt)))
		return -EFAULT;

	switch (mt.mt_op) {
	case MTRESET:
	case MTNOP:
		opcode = OP_NOP;
		break;
	case MTREW:
		opcode = OP_REWIND;
		break;
	case MTOFFL:
		opcode = OP_REWIND_UL;
		break;
	case MTWEOF:
		opcode = OP_WRITE_TM;
		repeat = mt.mt_count;
		break;
	case MTFSF:
		opcode = OP_FWDSP_FILE;
		repeat = mt.mt_count;
		break;
	case MTBSF:
		opcode = OP_BACKSP_FILE;
		repeat = mt.mt_count;
		break;
	case MTFSR:
		opcode = OP_FWDSP_BLK;
		repeat = mt.mt_count;
		break;
	case MTBSR:
		opcode = OP_BACKSP_BLK;
		repeat = mt.mt_count;
		break;
	case MTERASE:
		opcode = OP_ERASE_GAP;
		break;
	default:
		return -ENOTTY;
	}

	if (repeat < 1)
		repeat = 1;

	if (mutex_lock_interruptible(&ht->lock))
		return -ERESTARTSYS;

	rc = 0;
	for (i = 0; i < repeat; i++) {
		rc = ht_control(ht, opcode);
		if (rc)
			break;
	}

	mutex_unlock(&ht->lock);
	return rc;
}

static const struct file_operations ht_fops = {
	.owner = THIS_MODULE,
	.read = ht_read,
	.write = ht_write,
	.unlocked_ioctl = ht_ioctl,
	.llseek = noop_llseek,	/* tape is inherently non-seekable; some dd
				 * builds treat a real ESPIPE from lseek() as
				 * fatal even though they never use the
				 * result -- noop_llseek avoids that without
				 * claiming real seek support */
};

static int ht_set_online(struct ccw_device *cdev) { return 0; }
static int ht_set_offline(struct ccw_device *cdev) { return 0; }

static int ht_probe(struct ccw_device *cdev)
{
	struct ht_dev *ht;
	static int idx;
	int rc;

	ht = kzalloc(sizeof(*ht), GFP_KERNEL);
	if (!ht)
		return -ENOMEM;
	ht->cdev = cdev;
	mutex_init(&ht->lock);
	init_completion(&ht->done);
	snprintf(ht->name, sizeof(ht->name), "htp%d", idx++);

	ht->misc.minor = MISC_DYNAMIC_MINOR;
	ht->misc.name = ht->name;
	ht->misc.fops = &ht_fops;
	ht->misc.parent = &cdev->dev;	/* -> /sys/class/misc/<name>/device links to 0.0.NNNN */
	ht->misc.mode = 0600;		/* destructive ops (write/rewind/erase): root-only */

	dev_set_drvdata(&cdev->dev, ht);
	cdev->handler = ht_int_handler;

	rc = misc_register(&ht->misc);
	if (rc) {
		kfree(ht);
		return rc;
	}

	dev_info(&cdev->dev, "hutape: registered as /dev/%s\n", ht->name);
	return 0;
}

static void ht_remove(struct ccw_device *cdev)
{
	struct ht_dev *ht = dev_get_drvdata(&cdev->dev);

	if (!ht)
		return;
	misc_deregister(&ht->misc);
	kfree(ht);
}

static struct ccw_device_id ht_ids[] = {
	{ CCW_DEVICE(0x3480, 0) },	/* 3480 tape, any model (real: 0x31 "D31") */
	{ CCW_DEVICE(0x3490, 0) },	/* 3490 tape, any model -- fallback only, tape_34xx wins 0580 */
	{ CCW_DEVICE(0x3590, 0) },	/* 3590 tape, any model (real: 0x60 "A50") */
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(ccw, ht_ids);

static struct ccw_driver ht_driver = {
	.driver = {
		.name = "hutape",
		.owner = THIS_MODULE,
	},
	.ids = ht_ids,
	.probe = ht_probe,
	.remove = ht_remove,
	.set_online = ht_set_online,
	.set_offline = ht_set_offline,
	.int_class = IRQIO_C15,
};

static int __init ht_init(void)
{
	return ccw_driver_register(&ht_driver);
}

static void __exit ht_exit(void)
{
	ccw_driver_unregister(&ht_driver);
}

module_init(ht_init);
module_exit(ht_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Native CCW driver for Hercules-emulated 3480/3590 tape (parallel to tape_34xx)");
