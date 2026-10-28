// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Angelo Di Tomasso
 * scp55 - Roland SCP-55 PCMCIA Sound Canvas driver
 *
 * Programs the socket (I/O base 0x330), un-mutes the CS4231A codec the synth
 * reaches the output through, and exposes the MPU-401 UART as an ALSA
 * rawmidi device (hence a sequencer port).
 *
 * /proc/scp55      status, "echo 1" for the test phrase
 * /proc/scp55mix   live codec mixer: "w <hex idx> <hex val>", "r <hex idx>",
 *                  "i" unmute defaults, "u" mute both channels
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#define DRV_NAME "scp55"

static struct pcmcia_device *scp_dev;
static struct snd_card *card;
static struct snd_rawmidi *rmidi;
static struct snd_rawmidi_substream *tx_sub;
static struct delayed_work tx_work;
static unsigned int io_base;
static int codec_ok, err_state;
static u8 last_read;
static int have_read;
static char status[512] = "not probed\n";

static int beep_on_probe = 1;
module_param(beep_on_probe, int, 0444);

static struct pcmcia_device_id scp55_ids[] = {
	PCMCIA_DEVICE_MANF_CARD(0xc00c, 0x0001),
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, scp55_ids);

/* ---- MPU-401 UART ---- */
static int uart_ready(void)
{
	int i;

	for (i = 0; i < 2000; i++)
		if (!(inb(io_base + 1) & 0x40))
			return 1;
	return 0;
}

static void mo(u8 b)
{
	int i;

	for (i = 0; i < 2000; i++)
		if (!(inb(io_base + 1) & 0x40))
			break;
	outb(b, io_base);
}

/* ---- CS4231A codec at io+8 ---- */
static void cc_wait(void)
{
	int i;

	for (i = 0; i < 200000; i++)
		if (!(inb(io_base + 8) & 0x80))
			return;
}

static void cc_put(u8 idx, u8 val)
{
	cc_wait();
	outb(idx, io_base + 8);
	udelay(200);
	outb(val, io_base + 9);
	udelay(200);
}

static u8 cc_get(u8 idx)
{
	cc_wait();
	outb(idx, io_base + 8);
	udelay(200);
	return inb(io_base + 9);
}

static void beep(void)
{
	static const u8 gs[] = { 0xf0,0x41,0x10,0x42,0x12,0x40,0x00,0x7f,0x00,0x41,0xf7 };
	int i, ch;

	for (ch = 0; ch < 16; ch++) { mo(0xb0 | ch); mo(0x78); mo(0x00); }
	for (i = 0; i < sizeof(gs); i++) {
		mo(gs[i]);
		if (gs[i] == 0xf7)
			msleep(60);
	}
	mo(0xc0); mo(0x00);
	mo(0x90); mo(0x3c); mo(0x64);
	mo(0x90); mo(0x40); mo(0x64);
	mo(0x90); mo(0x43); mo(0x64);
	msleep(1200);
	mo(0x80); mo(0x3c); mo(0x40);
	mo(0x80); mo(0x40); mo(0x40);
	mo(0x80); mo(0x43); mo(0x40);
}

/* ---- ALSA rawmidi ---- */
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
		outb(c, io_base);
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

/* ---- /proc/scp55 ---- */
static int status_show(struct seq_file *m, void *v)
{
	seq_puts(m, status);
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, NULL);
}

static ssize_t status_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	char k[8];

	if (count >= sizeof(k))
		return -EINVAL;
	if (copy_from_user(k, buf, count))
		return -EFAULT;
	k[count] = 0;
	if (io_base && k[0] == '1')
		beep();
	return count;
}

static const struct proc_ops status_ops = {
	.proc_open	= status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= status_write,
};

/* ---- /proc/scp55mix ---- */
static int mix_show(struct seq_file *m, void *v)
{
	if (!io_base)
		seq_puts(m, "card not configured\n");
	else if (have_read)
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

	if (!io_base)
		return -ENODEV;
	if (count >= sizeof(k))
		return -EINVAL;
	if (copy_from_user(k, buf, count))
		return -EFAULT;
	k[count] = 0;

	if (sscanf(k, " w %x %x", &idx, &val) == 2) {
		cc_put((u8)idx, (u8)val);
	} else if (sscanf(k, " r %x", &idx) == 1) {
		last_read = cc_get((u8)idx);
		have_read = 1;
	} else if (sscanf(k, " %c", &cmd) == 1) {
		if (cmd == 'i') {
			cc_put(0x06, 0x00); cc_put(0x07, 0x00);
			cc_put(0x02, 0x08); cc_put(0x03, 0x08);
			cc_put(0x04, 0x08); cc_put(0x05, 0x08);
		} else if (cmd == 'u') {
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

static int card_setup(struct pcmcia_device *link)
{
	int err;

	err = snd_card_new(&link->dev, SNDRV_DEFAULT_IDX1, "SCP55",
			   THIS_MODULE, 0, &card);
	if (err < 0)
		return err;

	strcpy(card->driver, "SCP-55");
	strcpy(card->shortname, "Roland SCP-55");
	snprintf(card->longname, sizeof(card->longname),
		 "Roland SCP-55 Sound Canvas (PCMCIA) at 0x%03x", io_base);

	err = snd_rawmidi_new(card, "SCP-55 MIDI", 0, 1, 0, &rmidi);
	if (err < 0) {
		snd_card_free(card);
		card = NULL;
		return err;
	}
	strcpy(rmidi->name, "SCP-55");
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &out_ops);

	INIT_DELAYED_WORK(&tx_work, tx_work_fn);

	err = snd_card_register(card);
	if (err < 0) {
		snd_card_free(card);
		card = NULL;
		rmidi = NULL;
		return err;
	}
	return 0;
}

static int scp55_conf_check(struct pcmcia_device *link, void *priv)
{
	int ret = pcmcia_request_io(link);

	if (ret)
		return -ENODEV;
	return 0;
}

static int scp55_probe(struct pcmcia_device *link)
{
	unsigned int base;
	u8 aux1;
	int ret;

	io_base = 0;
	codec_ok = err_state = 0;

	link->config_flags |= CONF_AUTO_SET_IO;
	link->io_lines = 10;

	ret = pcmcia_loop_config(link, scp55_conf_check, NULL);
	if (ret) {
		err_state = ret;
		snprintf(status, sizeof(status),
			 "pcmcia_loop_config found no usable I/O window: %d\n", ret);
		return ret;
	}

	base = link->resource[0]->start;
	ret = pcmcia_enable_device(link);
	if (ret) {
		err_state = ret;
		snprintf(status, sizeof(status),
			 "pcmcia_enable_device failed: %d (io %#x)\n", ret, base);
		pcmcia_disable_device(link);
		return ret;
	}

	io_base = base;
	scp_dev = link;

	cc_put(0x06, 0x00); cc_put(0x07, 0x00);
	cc_put(0x02, 0x08); cc_put(0x03, 0x08);
	cc_put(0x04, 0x08); cc_put(0x05, 0x08);
	aux1 = cc_get(0x02);
	codec_ok = ((aux1 & 0x1f) == 0x08);

	proc_create("scp55", 0666, NULL, &status_ops);
	proc_create("scp55mix", 0666, NULL, &mix_ops);

	ret = card_setup(link);
	if (ret) {
		snprintf(status, sizeof(status),
			 "SCP-55 io 0x%03x, ALSA card failed: %d\n", base, ret);
		return ret;
	}

	snprintf(status, sizeof(status),
		 "SCP-55 ready\n  io base   : 0x%03x\n  codec     : %s (aux1 L 0x%02x)\n"
		 "  chip id   : 0x%02x\n  rawmidi   : /dev/snd/midiC%dD0 (%s)\n"
		 "  sequencer : port \"Roland SCP-55\"\n",
		 base, codec_ok ? "unmuted" : "no answer", aux1, cc_get(0x0c),
		 card ? card->number : -1,
		 rmidi ? "registered" : "missing");

	pr_info(DRV_NAME ": io 0x%03x, codec %s, card %d\n",
		base, codec_ok ? "ok" : "silent", card->number);

	if (beep_on_probe)
		beep();
	return 0;
}

static void scp55_remove(struct pcmcia_device *link)
{
	tx_sub = NULL;
	cancel_delayed_work_sync(&tx_work);
	remove_proc_entry("scp55mix", NULL);
	remove_proc_entry("scp55", NULL);
	if (card) {
		snd_card_free(card);
		card = NULL;
		rmidi = NULL;
	}
	pcmcia_disable_device(link);
	scp_dev = NULL;
	io_base = 0;
}

static struct pcmcia_driver scp55_driver = {
	.owner		= THIS_MODULE,
	.name		= DRV_NAME,
	.id_table	= scp55_ids,
	.probe		= scp55_probe,
	.remove		= scp55_remove,
};

static int __init scp55_init(void)
{
	return pcmcia_register_driver(&scp55_driver);
}

static void __exit scp55_exit(void)
{
	pcmcia_unregister_driver(&scp55_driver);
}

module_init(scp55_init);
module_exit(scp55_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Angelo Di Tomasso");
MODULE_DESCRIPTION("Roland SCP-55 PCMCIA Sound Canvas: socket, codec, ALSA rawmidi");
MODULE_VERSION("0.1.3");
