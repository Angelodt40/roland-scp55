// SPDX-License-Identifier: GPL-2.0
/* scp55midi - SCP-55 MPU-401 as ALSA rawmidi + live codec mixer console */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#define MB 0x330
#define CB 0x338
#define DRV "scp55midi"

static struct platform_device *pdev;
static struct snd_card *card;
static struct snd_rawmidi *rmidi;
static struct snd_rawmidi_substream *tx_sub;
static struct delayed_work tx_work;
static u8 last_read;
static int have_read;

static int uart_ready(void)
{
	int i;

	for (i = 0; i < 2000; i++)
		if (!(inb(MB + 1) & 0x40))
			return 1;
	return 0;
}

static void cc_wait(void)
{
	int i;

	for (i = 0; i < 200000; i++)
		if (!(inb(CB) & 0x80))
			return;
}

static void cc_put(u8 idx, u8 val)
{
	cc_wait();
	outb(idx, CB);
	udelay(200);
	outb(val, CB + 1);
	udelay(200);
}

static u8 cc_get(u8 idx)
{
	cc_wait();
	outb(idx, CB);
	udelay(200);
	return inb(CB + 1);
}

static void tx_work_fn(struct work_struct *w)
{
	unsigned char c;
	struct snd_rawmidi_substream *sub = tx_sub;
	int again = 0;

	if (!sub)
		return;
	while (snd_rawmidi_transmit_peek(sub, &c, 1) == 1) {
		if (!uart_ready()) {
			again = 1;
			break;
		}
		outb(c, MB);
		snd_rawmidi_transmit_ack(sub, 1);
	}
	if (again)
		schedule_delayed_work(&tx_work, msecs_to_jiffies(1));
}

static int out_open(struct snd_rawmidi_substream *sub) { return 0; }
static int out_close(struct snd_rawmidi_substream *sub) { return 0; }

static void out_trigger(struct snd_rawmidi_substream *sub, int up)
{
	if (up) {
		tx_sub = sub;
		schedule_delayed_work(&tx_work, 0);
	} else {
		tx_sub = NULL;
		cancel_delayed_work_sync(&tx_work);
	}
}

static const struct snd_rawmidi_ops out_ops = {
	.open		= out_open,
	.close		= out_close,
	.trigger	= out_trigger,
};

/* ---- /proc/scp55mix: "w <hex idx> <hex val>" or "r <hex idx>" ---- */
static int mix_show(struct seq_file *m, void *v)
{
	if (have_read)
		seq_printf(m, "last read: 0x%02x\n", last_read);
	else
		seq_puts(m, "no reads yet\n");
	return 0;
}

static int mix_open(struct inode *inode, struct file *file)
{
	return single_open(file, mix_show, NULL);
}

static ssize_t mix_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	char k[32];
	unsigned int idx, val;
	char cmd;

	if (count >= sizeof(k))
		return -EINVAL;
	if (copy_from_user(k, buf, count))
		return -EFAULT;
	k[count] = 0;

	if (sscanf(k, " w %x %x", &idx, &val) == 2) {
		cc_put((u8)idx, (u8)val);
		pr_info(DRV ": write 0x%02x=0x%02x\n", idx, val);
	} else if (sscanf(k, " r %x", &idx) == 1) {
		last_read = cc_get((u8)idx);
		have_read = 1;
		pr_info(DRV ": read 0x%02x->0x%02x\n", idx, last_read);
	} else if (sscanf(k, " %c", &cmd) == 1 && (cmd == 'i' || cmd == 'u')) {
		/* i = unmute defaults, u = full mute both channels */
		if (cmd == 'i') {
			cc_put(0x06, 0x00); cc_put(0x07, 0x00);
			cc_put(0x02, 0x08); cc_put(0x03, 0x08);
			cc_put(0x04, 0x08); cc_put(0x05, 0x08);
		} else {
			cc_put(0x02, 0x88); cc_put(0x03, 0x88);
			cc_put(0x04, 0x88); cc_put(0x05, 0x88);
		}
	}
	return count;
}

static const struct proc_ops mix_ops = {
	.proc_open	= mix_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= mix_write,
};

static int __init scp55midi_init(void)
{
	int err;

	proc_create("scp55mix", 0666, NULL, &mix_ops);

	pdev = platform_device_register_simple(DRV, -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	err = snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, "SCP55", THIS_MODULE, 0, &card);
	if (err < 0) {
		platform_device_unregister(pdev);
		return err;
	}
	strcpy(card->driver, "SCP-55");
	strcpy(card->shortname, "Roland SCP-55");
	snprintf(card->longname, sizeof(card->longname),
		 "Roland SCP-55 Sound Canvas (PCMCIA) at 0x%03x", MB);

	err = snd_rawmidi_new(card, "SCP-55 MIDI", 0, 1, 0, &rmidi);
	if (err < 0) {
		snd_card_free(card);
		platform_device_unregister(pdev);
		return err;
	}
	strcpy(rmidi->name, "SCP-55");
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &out_ops);

	INIT_DELAYED_WORK(&tx_work, tx_work_fn);

	err = snd_card_register(card);
	if (err < 0) {
		snd_card_free(card);
		platform_device_unregister(pdev);
		return err;
	}

	pr_info(DRV ": rawmidi registered on 0x%03x, mixer at /proc/scp55mix\n", MB);
	return 0;
}

static void __exit scp55midi_exit(void)
{
	tx_sub = NULL;
	cancel_delayed_work_sync(&tx_work);
	if (card)
		snd_card_free(card);
	if (pdev)
		platform_device_unregister(pdev);
	remove_proc_entry("scp55mix", NULL);
}

module_init(scp55midi_init);
module_exit(scp55midi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ALSA rawmidi + mixer console for the Roland SCP-55");
