// SPDX-License-Identifier: GPL-2.0
/*
 * hunitrec.c -- minimal native CCW driver for Hercules-emulated IBM
 * unit-record devices: 1403 printer, 3505 reader, 3525 punch.
 *
 * Real channel I/O via ccw_device_start(), the same API tape_34xx /
 * dasd-eckd / 3270 already use successfully against this Hercules build.
 * No vmur (z/VM-diag-only, doesn't apply to a standalone/LPAR guest --
 * see docs/ubuntu.md #4) and no vfio-ccw (tried, stalls inside the guest
 * kernel's FSM before ever reaching Hercules -- see docs/hunitrec-driver.md).
 *
 * CU/dev types and CCW opcodes below are taken directly from Hercules's
 * own printer.c / cardrdr.c / cardpch.c, not just IBM docs, so they are
 * guaranteed to match this emulation:
 *   1403 printer : CU 0x2821, any dev_type -- write, opcode 0x09
 *                  "Write and Space 1 Line" (printer.c:1956)
 *   3505 reader  : CU 0x3505, dev_type 0x3505 -- read, opcode 0x02
 *                  "Read" (cardrdr.c:840), 80-byte card records
 *   3525 punch   : CU 0x3505, dev_type 0x3525 -- write, opcode 0x01
 *                  "Write, Feed, Select Stacker" (cardpch.c:466)
 *
 * EBCDIC conversion uses Hercules's own cp_819_to_037 / cp_037_to_819
 * tables (copied verbatim from heracles/codepage.c), matching the
 * CODEPAGE 819/037 setting already in ubuntu26.rc.
 *
 * Bind/online like any other ccw device:
 *   modprobe hunitrec
 *   echo 1 > /sys/bus/ccw/devices/0.0.000e/online   # printer
 *   echo 1 > /sys/bus/ccw/devices/0.0.000c/online   # reader
 *   echo 1 > /sys/bus/ccw/devices/0.0.000d/online   # punch
 *   echo "HELLO" > /dev/hprt2
 *   cat /dev/hrdr0 > cards.txt
 *   cat file.txt > /dev/hpch0
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <asm/ccwdev.h>
#include <asm/cio.h>
#include <asm/scsw.h>

#define HPRT_MAX_LINE 132   /* real 1403 line width */
#define CARD_LEN      80    /* real 80-column card */

/* verbatim from heracles/codepage.c: cp_819_to_037[] / cp_037_to_819[] */
static const unsigned char cp_819_to_037[256] = {
	0x00,0x01,0x02,0x03,0x37,0x2D,0x2E,0x2F,0x16,0x05,0x25,0x0B,0x0C,0x0D,0x0E,0x0F,
	0x10,0x11,0x12,0x13,0x3C,0x3D,0x32,0x26,0x18,0x19,0x3F,0x27,0x1C,0x1D,0x1E,0x1F,
	0x40,0x5A,0x7F,0x7B,0x5B,0x6C,0x50,0x7D,0x4D,0x5D,0x5C,0x4E,0x6B,0x60,0x4B,0x61,
	0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0x7A,0x5E,0x4C,0x7E,0x6E,0x6F,
	0x7C,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,
	0xD7,0xD8,0xD9,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xBA,0xE0,0xBB,0xB0,0x6D,
	0x79,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x91,0x92,0x93,0x94,0x95,0x96,
	0x97,0x98,0x99,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xC0,0x4F,0xD0,0xA1,0x07,
	0x20,0x21,0x22,0x23,0x24,0x15,0x06,0x17,0x28,0x29,0x2A,0x2B,0x2C,0x09,0x0A,0x1B,
	0x30,0x31,0x1A,0x33,0x34,0x35,0x36,0x08,0x38,0x39,0x3A,0x3B,0x04,0x14,0x3E,0xFF,
	0x41,0xAA,0x4A,0xB1,0x9F,0xB2,0x6A,0xB5,0xBD,0xB4,0x9A,0x8A,0x5F,0xCA,0xAF,0xBC,
	0x90,0x8F,0xEA,0xFA,0xBE,0xA0,0xB6,0xB3,0x9D,0xDA,0x9B,0x8B,0xB7,0xB8,0xB9,0xAB,
	0x64,0x65,0x62,0x66,0x63,0x67,0x9E,0x68,0x74,0x71,0x72,0x73,0x78,0x75,0x76,0x77,
	0xAC,0x69,0xED,0xEE,0xEB,0xEF,0xEC,0xBF,0x80,0xFD,0xFE,0xFB,0xFC,0xAD,0xAE,0x59,
	0x44,0x45,0x42,0x46,0x43,0x47,0x9C,0x48,0x54,0x51,0x52,0x53,0x58,0x55,0x56,0x57,
	0x8C,0x49,0xCD,0xCE,0xCB,0xCF,0xCC,0xE1,0x70,0xDD,0xDE,0xDB,0xDC,0x8D,0x8E,0xDF,
};

static const unsigned char cp_037_to_819[256] = {
	0x00,0x01,0x02,0x03,0x9C,0x09,0x86,0x7F,0x97,0x8D,0x8E,0x0B,0x0C,0x0D,0x0E,0x0F,
	0x10,0x11,0x12,0x13,0x9D,0x85,0x08,0x87,0x18,0x19,0x92,0x8F,0x1C,0x1D,0x1E,0x1F,
	0x80,0x81,0x82,0x83,0x84,0x0A,0x17,0x1B,0x88,0x89,0x8A,0x8B,0x8C,0x05,0x06,0x07,
	0x90,0x91,0x16,0x93,0x94,0x95,0x96,0x04,0x98,0x99,0x9A,0x9B,0x14,0x15,0x9E,0x1A,
	0x20,0xA0,0xE2,0xE4,0xE0,0xE1,0xE3,0xE5,0xE7,0xF1,0xA2,0x2E,0x3C,0x28,0x2B,0x7C,
	0x26,0xE9,0xEA,0xEB,0xE8,0xED,0xEE,0xEF,0xEC,0xDF,0x21,0x24,0x2A,0x29,0x3B,0xAC,
	0x2D,0x2F,0xC2,0xC4,0xC0,0xC1,0xC3,0xC5,0xC7,0xD1,0xA6,0x2C,0x25,0x5F,0x3E,0x3F,
	0xF8,0xC9,0xCA,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0x60,0x3A,0x23,0x40,0x27,0x3D,0x22,
	0xD8,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0xAB,0xBB,0xF0,0xFD,0xFE,0xB1,
	0xB0,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70,0x71,0x72,0xAA,0xBA,0xE6,0xB8,0xC6,0xA4,
	0xB5,0x7E,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0xA1,0xBF,0xD0,0xDD,0xDE,0xAE,
	0x5E,0xA3,0xA5,0xB7,0xA9,0xA7,0xB6,0xBC,0xBD,0xBE,0x5B,0x5D,0xAF,0xA8,0xB4,0xD7,
	0x7B,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0xAD,0xF4,0xF6,0xF2,0xF3,0xF5,
	0x7D,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,0x50,0x51,0x52,0xB9,0xFB,0xFC,0xF9,0xFA,0xFF,
	0x5C,0xF7,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0xB2,0xD4,0xD6,0xD2,0xD3,0xD5,
	0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0xB3,0xDB,0xDC,0xD9,0xDA,0x9F,
};

enum hu_kind { KIND_PRT, KIND_RDR, KIND_PCH };

struct hu_dev {
	struct ccw_device *cdev;
	struct miscdevice misc;
	struct mutex lock;
	struct completion done;
	struct irb last_irb;
	enum hu_kind kind;
	char name[16];
};

static void hu_int_handler(struct ccw_device *cdev, unsigned long intparm,
			    struct irb *irb)
{
	struct hu_dev *hu = dev_get_drvdata(&cdev->dev);

	if (!hu)
		return;
	memcpy(&hu->last_irb, irb, sizeof(*irb));
	complete(&hu->done);
}

/* Issue one CCW (cmd_code, dma_buf/dma_addr, len) and wait for completion.
 * Caller holds hu->lock and provides DMA-safe dma_buf/dma_addr. Returns 0
 * on normal completion, -ETIMEDOUT, or -EIO (unit check) on failure. */
static int hu_do_io(struct hu_dev *hu, u8 cmd_code, dma32_t dma_addr, u16 len)
{
	struct ccw1 *ccw;
	dma32_t ccw_dma;
	int rc;

	ccw = ccw_device_dma_zalloc(hu->cdev, sizeof(*ccw), &ccw_dma);
	if (!ccw)
		return -ENOMEM;

	ccw->cmd_code = cmd_code;
	ccw->flags = CCW_FLAG_SLI;
	ccw->count = len;
	ccw->cda = dma_addr;

	reinit_completion(&hu->done);
	rc = ccw_device_start(hu->cdev, ccw, (unsigned long)hu, 0xff, 0);
	if (rc)
		goto out;

	if (!wait_for_completion_timeout(&hu->done, 10 * HZ)) {
		rc = -ETIMEDOUT;
		goto out;
	}
	if (scsw_dstat(&hu->last_irb.scsw) & (DEV_STAT_UNIT_CHECK | DEV_STAT_UNIT_EXCEP))
		rc = -EIO;	/* e.g. reader out of cards (cardrdr.c sets UX, "eof" attach flag) */
	else
		rc = 0;

out:
	ccw_device_dma_free(hu->cdev, ccw, sizeof(*ccw));
	return rc;
}

/* ---- printer / punch: write() ---- */

static ssize_t hu_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *pos)
{
	struct hu_dev *hu = container_of(file->private_data, struct hu_dev, misc);
	size_t maxlen = (hu->kind == KIND_PRT) ? HPRT_MAX_LINE : CARD_LEN;
	unsigned char *data = NULL;
	dma32_t data_dma;
	char *kbuf;
	u8 opcode = (hu->kind == KIND_PRT) ? 0x09 : 0x01;
	int rc, i;

	if (count == 0)
		return 0;
	if (count > maxlen)
		count = maxlen;

	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	if (mutex_lock_interruptible(&hu->lock)) {
		kfree(kbuf);
		return -ERESTARTSYS;
	}

	data = ccw_device_dma_zalloc(hu->cdev, count, &data_dma);
	if (!data) {
		rc = -ENOMEM;
		goto out;
	}
	for (i = 0; i < count; i++)
		data[i] = cp_819_to_037[(unsigned char)kbuf[i]];

	rc = hu_do_io(hu, opcode, data_dma, count);
	ccw_device_dma_free(hu->cdev, data, count);
	if (rc == 0)
		rc = count;

out:
	mutex_unlock(&hu->lock);
	kfree(kbuf);
	return rc;
}

/* ---- reader: read() ---- */

static ssize_t hu_read(struct file *file, char __user *ubuf, size_t count,
			loff_t *pos)
{
	struct hu_dev *hu = container_of(file->private_data, struct hu_dev, misc);
	unsigned char *data;
	dma32_t data_dma;
	char *kbuf;
	int rc, i;
	size_t n;

	if (mutex_lock_interruptible(&hu->lock))
		return -ERESTARTSYS;

	data = ccw_device_dma_zalloc(hu->cdev, CARD_LEN, &data_dma);
	if (!data) {
		rc = -ENOMEM;
		goto out;
	}

	rc = hu_do_io(hu, 0x02, data_dma, CARD_LEN);
	if (rc) {
		ccw_device_dma_free(hu->cdev, data, CARD_LEN);
		if (rc == -EIO)
			rc = 0;	/* out of cards -> EOF, not an error */
		goto out;
	}

	kbuf = kmalloc(CARD_LEN, GFP_KERNEL);
	if (!kbuf) {
		ccw_device_dma_free(hu->cdev, data, CARD_LEN);
		rc = -ENOMEM;
		goto out;
	}
	for (i = 0; i < CARD_LEN; i++)
		kbuf[i] = cp_037_to_819[data[i]];
	ccw_device_dma_free(hu->cdev, data, CARD_LEN);

	n = count < CARD_LEN ? count : CARD_LEN;
	if (copy_to_user(ubuf, kbuf, n))
		rc = -EFAULT;
	else
		rc = n;
	kfree(kbuf);

out:
	mutex_unlock(&hu->lock);
	return rc;
}

static const struct file_operations hu_prt_pch_fops = {
	.owner = THIS_MODULE,
	.write = hu_write,
};

static const struct file_operations hu_rdr_fops = {
	.owner = THIS_MODULE,
	.read = hu_read,
};

static int hu_set_online(struct ccw_device *cdev) { return 0; }
static int hu_set_offline(struct ccw_device *cdev) { return 0; }

static int hu_probe(struct ccw_device *cdev)
{
	struct hu_dev *hu;
	static int prt_idx, rdr_idx, pch_idx;
	const char *prefix;
	const struct file_operations *fops;
	int rc;

	hu = kzalloc(sizeof(*hu), GFP_KERNEL);
	if (!hu)
		return -ENOMEM;
	hu->cdev = cdev;
	mutex_init(&hu->lock);
	init_completion(&hu->done);

	if (cdev->id.cu_type == 0x2821) {
		hu->kind = KIND_PRT;
		prefix = "hprt";
		fops = &hu_prt_pch_fops;
		snprintf(hu->name, sizeof(hu->name), "%s%d", prefix, prt_idx++);
	} else if (cdev->id.dev_type == 0x3505) {
		hu->kind = KIND_RDR;
		prefix = "hrdr";
		fops = &hu_rdr_fops;
		snprintf(hu->name, sizeof(hu->name), "%s%d", prefix, rdr_idx++);
	} else {
		hu->kind = KIND_PCH;
		prefix = "hpch";
		fops = &hu_prt_pch_fops;
		snprintf(hu->name, sizeof(hu->name), "%s%d", prefix, pch_idx++);
	}

	hu->misc.minor = MISC_DYNAMIC_MINOR;
	hu->misc.name = hu->name;
	hu->misc.fops = fops;
	hu->misc.parent = &cdev->dev;	/* -> /sys/class/misc/<name>/device links to 0.0.NNNN */
	hu->misc.mode = 0666;		/* regular users can open directly, no sudo/udev rule needed */

	dev_set_drvdata(&cdev->dev, hu);
	cdev->handler = hu_int_handler;

	rc = misc_register(&hu->misc);
	if (rc) {
		kfree(hu);
		return rc;
	}

	dev_info(&cdev->dev, "hunitrec: registered as /dev/%s\n", hu->name);
	return 0;
}

static void hu_remove(struct ccw_device *cdev)
{
	struct hu_dev *hu = dev_get_drvdata(&cdev->dev);

	if (!hu)
		return;
	misc_deregister(&hu->misc);
	kfree(hu);
}

static struct ccw_device_id hu_ids[] = {
	{ CCW_DEVICE(0x2821, 0) },			/* 1403 printer */
	{ CCW_DEVICE_DEVTYPE(0x3505, 0, 0x3505, 0) },	/* 3505 reader */
	{ CCW_DEVICE_DEVTYPE(0x3505, 0, 0x3525, 0) },	/* 3525 punch */
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(ccw, hu_ids);

static struct ccw_driver hu_driver = {
	.driver = {
		.name = "hunitrec",
		.owner = THIS_MODULE,
	},
	.ids = hu_ids,
	.probe = hu_probe,
	.remove = hu_remove,
	.set_online = hu_set_online,
	.set_offline = hu_set_offline,
	.int_class = IRQIO_C15,
};

static int __init hu_init(void)
{
	return ccw_driver_register(&hu_driver);
}

static void __exit hu_exit(void)
{
	ccw_driver_unregister(&hu_driver);
}

module_init(hu_init);
module_exit(hu_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Native CCW driver for Hercules-emulated 1403/3505/3525 unit-record devices");
