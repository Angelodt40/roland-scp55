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
static int card_mode = 1;
module_param(card_mode, int, 0644);
MODULE_PARM_DESC(card_mode, "select the card's audio routing mode (3) at probe instead of relying on the codec loopback");

static int use_loopback = 0;
module_param(use_loopback, int, 0644);
MODULE_PARM_DESC(use_loopback, "close the codec's ADC to DAC loopback: the workaround 0.2.0 relied on, unnecessary once the card mode is selected");

static int mpu_init_on_open = 0;
module_param(mpu_init_on_open, int, 0644);
MODULE_PARM_DESC(mpu_init_on_open, "re-initialise the MPU-401 UART on every open (off by default: the card is already in UART mode after power-on, and a lost 0x3F leaves it in intelligent mode, where it discards every note)");

static int reset_on_open = 0;
module_param(reset_on_open, int, 0644);
MODULE_PARM_DESC(reset_on_open, "send GS reset and all-notes-off when the output opens (off by default: clients that open and close the port repeatedly would silence playback mid-file, which is exactly what happened with aplaymidi)");

static int reset_on_close = 1;
module_param(reset_on_close, int, 0644);
MODULE_PARM_DESC(reset_on_close, "send all-notes-off and all-sound-off when the output closes");

/* Serialises every port access: the delayed work, the procfs handlers and the
 * test phrase all talk to the same registers.
 */
static DEFINE_MUTEX(io_mutex);

/* Protects the substream pointer and the counters, which are touched from
 * contexts that may not sleep.
 */
static DEFINE_SPINLOCK(state_lock);
static bool pending_reset, pending_panic;
static unsigned int reset_open_count, reset_close_count, tx_byte_count;

static void mpu_init(void);
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

/* The card answers its own signature: after power-on base+5 reads 0xAA
 * (SCPENA.EXE writes 0 to base+4 first, SCP95.VXD does the same in its
 * identify routine). At probe time nothing has touched those ports yet, so a
 * plain read is enough - and a read cannot put the card into a wrong mode,
 * which a write to base+4/base+5 demonstrably can.
 */
static int card_signature(void)
{
	return inb(io_base + 5);
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
/* --- transmit helpers, caller must hold io_mutex --- */
static void send_bytes(const u8 *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		mo(buf[i]);
}

static void send_gs_reset(void)
{
	static const u8 gs[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f,
				 0x00, 0x41, 0xf7 };
	int ch;

	/* controllers first: a reset alone can leave notes hanging */
	for (ch = 0; ch < 16; ch++) {
		u8 notes_off[3] = { 0xb0 | ch, 0x7b, 0x00 };

		send_bytes(notes_off, 3);
	}
	send_bytes(gs, sizeof(gs));
	msleep(60);
}

static void send_panic(void)
{
	int ch;

	for (ch = 0; ch < 16; ch++) {
		u8 msg[3] = { 0xb0 | ch, 0x78, 0x00 };
		u8 notes_off[3] = { 0xb0 | ch, 0x7b, 0x00 };

		/* all sound off, then all notes off */
		send_bytes(msg, 3);
		send_bytes(notes_off, 3);
	}
}

static void beep_locked(void)
{
	mutex_lock(&io_mutex);
	if (io_base)
		beep();
	mutex_unlock(&io_mutex);
}

/* ---- MIDI input: MPU-401 UART receive ---- */
static struct snd_rawmidi_substream *rx_sub;
static bool rx_running;
static struct delayed_work rx_work;

static void rx_work_fn(struct work_struct *w)
{
	unsigned char buf[64];
	struct snd_rawmidi_substream *sub;
	unsigned long flags;
	bool again;
	int n = 0;

	spin_lock_irqsave(&state_lock, flags);
	sub = rx_sub;
	spin_unlock_irqrestore(&state_lock, flags);
	if (!sub)
		return;

	mutex_lock(&io_mutex);
	if (io_base) {
		/* DRR (bit 7) stays set while the UART holds a byte for us */
		while (n < (int)sizeof(buf) && (inb(io_base + 1) & 0x80))
			buf[n++] = inb(io_base);
	}
	mutex_unlock(&io_mutex);

	if (n)
		snd_rawmidi_receive(sub, buf, n);

	spin_lock_irqsave(&state_lock, flags);
	again = rx_running && rx_sub;
	spin_unlock_irqrestore(&state_lock, flags);

	if (again)
		schedule_delayed_work(&rx_work, msecs_to_jiffies(1));
}

static int in_open(struct snd_rawmidi_substream *substream) { return 0; }
static int in_close(struct snd_rawmidi_substream *substream) { return 0; }

static void in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	unsigned long flags;

	/* Atomic context: flag and schedule only. */
	spin_lock_irqsave(&state_lock, flags);
	rx_sub = up ? substream : NULL;
	rx_running = up ? true : false;
	spin_unlock_irqrestore(&state_lock, flags);

	if (up)
		schedule_delayed_work(&rx_work, 0);
}

static const struct snd_rawmidi_ops in_ops = {
	.open		= in_open,
	.close		= in_close,
	.trigger	= in_trigger,
};

static void tx_work_fn(struct work_struct *w)
{
	unsigned char c;
	struct snd_rawmidi_substream *sub;
	bool do_reset, do_panic;
	unsigned long flags;
	int again = 0;

	spin_lock_irqsave(&state_lock, flags);
	sub = tx_sub;
	do_reset = pending_reset;
	do_panic = pending_panic;
	pending_reset = false;
	pending_panic = false;
	spin_unlock_irqrestore(&state_lock, flags);

	mutex_lock(&io_mutex);
	if (!io_base) {
		mutex_unlock(&io_mutex);
		return;
	}

	if (do_reset && reset_on_open) {
		if (mpu_init_on_open)
			mpu_init();
		send_gs_reset();
		spin_lock_irqsave(&state_lock, flags);
		reset_open_count++;
		spin_unlock_irqrestore(&state_lock, flags);
	}
	if (do_panic && reset_on_close) {
		send_panic();
		spin_lock_irqsave(&state_lock, flags);
		reset_close_count++;
		spin_unlock_irqrestore(&state_lock, flags);
	}

	while (sub && snd_rawmidi_transmit_peek(sub, &c, 1) == 1) {
		if (!uart_ready()) {
			again = 1;
			break;
		}
		outb(c, io_base);
		snd_rawmidi_transmit_ack(sub, 1);
		spin_lock_irqsave(&state_lock, flags);
		tx_byte_count++;
		spin_unlock_irqrestore(&state_lock, flags);
	}
	mutex_unlock(&io_mutex);

	if (again)
		schedule_delayed_work(&tx_work, msecs_to_jiffies(1));
}

static int out_open(struct snd_rawmidi_substream *substream) { return 0; }
static int out_close(struct snd_rawmidi_substream *substream) { return 0; }

static void out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	unsigned long flags;

	/* Atomic context: flag and schedule only, never sleep here. */
	spin_lock_irqsave(&state_lock, flags);
	if (up) {
		tx_sub = substream;
		pending_reset = true;
	} else {
		tx_sub = NULL;
		pending_panic = true;
	}
	spin_unlock_irqrestore(&state_lock, flags);

	schedule_delayed_work(&tx_work, 0);
}
static const struct snd_rawmidi_ops out_ops = {
	.open		= out_open,
	.close		= out_close,
	.trigger	= out_trigger,
};

/* ---- /proc/scp55 ---- */
static int status_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned int ro, rc, tb;

	seq_puts(m, status);
	if (!io_base)
		return 0;

	spin_lock_irqsave(&state_lock, flags);
	ro = reset_open_count;
	rc = reset_close_count;
	tb = tx_byte_count;
	spin_unlock_irqrestore(&state_lock, flags);

	seq_printf(m, "  resets    : %u on open, %u on close\n", ro, rc);
	seq_printf(m, "  sent      : %u bytes\n", tb);
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
	if (k[0] == '1')
		beep_locked();
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
	ssize_t ret = count;

	if (!io_base)
		return -ENODEV;
	if (count >= sizeof(k))
		return -EINVAL;
	if (copy_from_user(k, buf, count))
		return -EFAULT;
	k[count] = 0;

	mutex_lock(&io_mutex);
	if (!io_base) {
		ret = -ENODEV;
		goto out;
	}
	if (sscanf(k, " w %x %x", &idx, &val) == 2) {
		if (idx > 0xff || val > 0xff)
			ret = -EINVAL;
		else
			cc_put((u8)idx, (u8)val);
	} else if (sscanf(k, " r %x", &idx) == 1) {
		if (idx > 0xff) {
			ret = -EINVAL;
		} else {
			last_read = cc_get((u8)idx);
			have_read = 1;
		}
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
out:
	mutex_unlock(&io_mutex);
	return ret;
}
static const struct proc_ops mix_ops = {
	.proc_open	= mix_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= mix_write,
};


/* ---- codec bring-up ----
 * The routing itself is selected by card_mode_select(); the loopback below is
 * kept only as a switch for cards or machines where the mode write does not
 * take.
 * The synth reaches the output through the CS4231A's analogue input, so the
 * codec has to digitise it and hand it back to its own DAC: unmuting the
 * input gains is not enough, the ADC -> DAC loopback path (LOOPBACK bit 1)
 * has to be closed as well. Without it the card is silent while every status
 * register still reports a healthy codec, which is exactly how 0.1.5 behaved
 * after a cold power-up.
 *
 * Verified on hardware 2026-10-28: with 0x0E = 0x02 the test phrase is
 * audible, with 0x0E = 0x00 the card is silent.
 */
/* Select the card's audio routing. The vendor wave path writes 3 to the
 * control register and 1 to its neighbour before playing
 * (SCP95.DRV, segment 7) - with that mode the synth reaches the codec's DAC
 * without the ADC loopback, verified on hardware: loopback off, mode 3,
 * test phrase audible.
 */
static void card_mode_select(void)
{
	outb(0x03, io_base + 4);
	udelay(50);
	outb(0x01, io_base + 5);
	udelay(50);
}

static void codec_init(void)
{
	u8 aux1;

	/* Mode Change Enable: the format and loopback registers only accept
	 * writes while MCE is set.
	 */
	cc_put(0x09, 0x40);

	cc_put(0x08, 0x00);			/* playback format: 8-bit linear */
	cc_put(0x00, 0x08); cc_put(0x01, 0x08);	/* ADC input */
	cc_put(0x02, 0x08); cc_put(0x03, 0x08);	/* auxiliary 1 */
	cc_put(0x04, 0x08); cc_put(0x05, 0x08);	/* auxiliary 2 */
	cc_put(0x06, 0x00); cc_put(0x07, 0x00);	/* DAC: no attenuation */
	cc_put(0x13, 0x08); cc_put(0x14, 0x08);	/* line in */
	cc_put(0x15, 0x08); cc_put(0x16, 0x08);	/* mic */
	cc_put(0x1b, 0x08);			/* mono input */
	if (use_loopback)
		cc_put(0x0e, 0x02);		/* ADC -> DAC loopback */

	cc_put(0x09, 0x00);			/* leave mode change */

	aux1 = cc_get(0x02);
	codec_ok = ((aux1 & 0x1f) == 0x08);
}

/* ---- MPU-401 UART initialisation ---- */
static int mpu_wait_read(u8 *out, int ms)
{
	int i;

	for (i = 0; i < ms * 100; i++) {
		if (inb(io_base + 1) & 0x80) {
			*out = inb(io_base);
			return 1;
		}
		udelay(10);
	}
	return 0;
}

static int mpu_ready(int ms)
{
	int i;

	for (i = 0; i < ms * 100; i++) {
		if (!(inb(io_base + 1) & 0x40))
			return 1;
		udelay(10);
	}
	return 0;
}

/* Put the UART back into UART mode. The command byte is only written once the
 * card says it is ready, and 0x3F is retried until it is acknowledged: a lost
 * 0x3F leaves the card in intelligent mode, where it silently discards every
 * note that arrives. 0xFF (full reset) is a last resort, because it is what
 * puts the card in intelligent mode in the first place.
 */
static void mpu_init(void)
{
	u8 ack = 0;
	int i, attempt;

	/* Drop whatever the card queued, but never loop forever: DRR can stay
	 * high with a byte that never pops.
	 */
	for (i = 0; i < 8 && (inb(io_base + 1) & 0x80); i++)
		inb(io_base);

	for (attempt = 0; attempt < 5; attempt++) {
		if (!mpu_ready(500))
			continue;
		outb(0x3f, io_base + 1);
		if (mpu_wait_read(&ack, 300) && ack == 0xfe) {
			pr_info(DRV_NAME ": MPU-401 in UART mode\n");
			return;
		}
	}

	/* Still not answering: full reset, then UART mode again. */
	pr_info(DRV_NAME ": UART mode not acknowledged, resetting the MPU\n");
	if (mpu_ready(500))
		outb(0xff, io_base + 1);
	mpu_wait_read(&ack, 1000);
	if (mpu_ready(500))
		outb(0x3f, io_base + 1);
	if (mpu_wait_read(&ack, 1000) && ack == 0xfe)
		pr_info(DRV_NAME ": UART mode restored after reset\n");
	else
		pr_info(DRV_NAME ": MPU-401 still silent\n");
}

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

	err = snd_rawmidi_new(card, "SCP-55 MIDI", 0, 1, 1, &rmidi);
	if (err < 0) {
		snd_card_free(card);
		card = NULL;
		return err;
	}
	strcpy(rmidi->name, "SCP-55");
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &out_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &in_ops);

	INIT_DELAYED_WORK(&tx_work, tx_work_fn);
	INIT_DELAYED_WORK(&rx_work, rx_work_fn);

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
	u8 aux1, signature;
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

	/* Identify first: selecting the card mode overwrites the signature. */
	signature = card_signature();
	if (card_mode)
		card_mode_select();
	codec_init();
	aux1 = cc_get(0x02);
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
		 "  signature : 0x%02x (%s)\n  chip id   : 0x%02x\n"
		 "  rawmidi   : /dev/snd/midiC%dD0 (%s)\n"
		 "  sequencer : port \"Roland SCP-55\"\n",
		 base, codec_ok ? "unmuted" : "no answer", aux1,
		 signature, signature == 0xaa ? "SCP-55 confirmed" : "unexpected",
		 cc_get(0x0c),
		 card ? card->number : -1,
		 rmidi ? "registered" : "missing");

	pr_info(DRV_NAME ": io 0x%03x, codec %s, card %d\n",
		base, codec_ok ? "ok" : "silent", card->number);

	if (beep_on_probe)
		beep_locked();
	return 0;
}

static void scp55_remove(struct pcmcia_device *link)
{
	remove_proc_entry("scp55mix", NULL);
	remove_proc_entry("scp55", NULL);

	/* Stop both directions, then wait for any in-flight register access
	 * before the card goes away underneath it.
	 */
	cancel_delayed_work_sync(&rx_work);
	cancel_delayed_work_sync(&tx_work);
	mutex_lock(&io_mutex);
	io_base = 0;
	mutex_unlock(&io_mutex);

	spin_lock_irq(&state_lock);
	tx_sub = NULL;
	rx_sub = NULL;
	rx_running = false;
	spin_unlock_irq(&state_lock);

	if (card) {
		snd_card_free(card);
		card = NULL;
		rmidi = NULL;
	}
	pcmcia_disable_device(link);
	scp_dev = NULL;
}
/* ---- power management ---- */
static int scp55_suspend(struct pcmcia_device *link)
{
	cancel_delayed_work_sync(&rx_work);
	cancel_delayed_work_sync(&tx_work);

	mutex_lock(&io_mutex);
	if (io_base)
		send_panic();
	mutex_unlock(&io_mutex);

	pr_info(DRV_NAME ": suspended\n");
	return 0;
}

static int scp55_resume(struct pcmcia_device *link)
{
	int ret;

	ret = pcmcia_enable_device(link);
	if (ret) {
		pr_err(DRV_NAME ": resume: pcmcia_enable_device failed: %d\n", ret);
		return ret;
	}

	mutex_lock(&io_mutex);
	io_base = link->resource[0]->start;

	/* after a power cycle the codec comes back with the loopback open
	 * and the inputs muted: bring it up again, then put the MPU-401 back
	 * into UART mode, exactly as probe does
	 */
	codec_init();
	mpu_init();
	mutex_unlock(&io_mutex);

	pr_info(DRV_NAME ": resumed, io 0x%03x, codec %s\n",
		io_base, codec_ok ? "ok" : "silent");
	return 0;
}

static struct pcmcia_driver scp55_driver = {
	.owner		= THIS_MODULE,
	.name		= DRV_NAME,
	.id_table	= scp55_ids,
	.probe		= scp55_probe,
	.remove		= scp55_remove,
	.suspend	= scp55_suspend,
	.resume		= scp55_resume,
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
MODULE_VERSION("0.2.1");
