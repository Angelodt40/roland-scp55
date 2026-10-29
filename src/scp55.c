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
/* Research switch: the memory window is reserved and walked only when asked.
 * The synth does not need it, and reserving it changes the card's state, so
 * the normal load stays exactly as it was before.
 */
static int mem_probe;
module_param(mem_probe, int, 0644);
MODULE_PARM_DESC(mem_probe, "reserve and walk the 1 MB memory window (research, default 0)");

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

	if (do_reset && mpu_init_on_open)
		mpu_init();
	if (do_reset && reset_on_open) {
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
/* ---- the vendor's control-interface handshake ----
 * SCPENA.EXE and the Windows VxD both program the card's control pair
 * (base+4 select, base+5 value) before they touch the MPU: selector 2, value
 * 1, then they wait until base+5 reads back zero. After a suspend the card
 * comes back deaf to MPU commands - 0x3F and 0xFF both go unanswered, which
 * prints as "MPU-401 still silent" - and this is the step our resume was
 * missing. The vendor also zeroes base+10 (0x33A) in the same routine.
 */
static void card_control_init(void)
{
	int i;

	outb(0x02, io_base + 4);
	udelay(50);
	outb(0x01, io_base + 5);
	udelay(50);
	for (i = 0; i < 1000; i++) {
		outb(0x02, io_base + 4);
		udelay(20);
		if (!inb(io_base + 5))
			break;
		udelay(100);
	}
	outb(0x00, io_base + 10);
}

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

	/* The caller must have run card_control_init() before this: the card
	 * only answers MPU commands once its control interface is programmed.
	 * It must also have run codec_init() *after* that handshake, because
	 * writing base+4/base+5 resets the codec state and silences the card.
	 * Order, measured on hardware: card_control_init, mode select,
	 * codec_init, mpu_init.
	 */

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

static struct resource *win_res, *attr_res;
static void __iomem *win_map, *attr_map;
static int win_tried, win_ret, attr_ret;
static struct pcmcia_device *scp55_dev;

static int scp55_window_reserve(struct pcmcia_device *link, int which);

static int scp55_conf_check(struct pcmcia_device *link, void *priv)
{
	int ret = pcmcia_request_io(link);

	if (ret)
		return -ENODEV;

	/* The window has to be reserved while the configuration is still
	 * open: pcmcia_enable_device() programs the socket with the windows
	 * that were requested before it, so asking afterwards is too late.
	 * A refusal is not fatal - the MIDI path does not need it.
	 */
	if (mem_probe && !win_tried) {
		win_tried = 1;
		win_ret = scp55_window_reserve(link, 0);
		attr_ret = scp55_window_reserve(link, 1);
	}
	return 0;
}


/* ---- memory window ---- */
/* The CIS asks for 1 MB of common memory and the socket has no 1 MB to give
 * (the bridge window is exactly 1 MB and its first 64 KB is excluded), so
 * pcmcia_loop_config with CONF_AUTO_SET_IOMEM fails with ENOSPC. A 16-bit
 * memory card does not need the whole thing at once anyway: a window is a
 * 64 KB aperture that slides over the card's pages, so ask for one page and
 * walk it. Nothing here is fatal: a failure keeps the MIDI-only behaviour.
 *
 * Two traps cost a session each and are worth remembering:
 *  - in pcmcia_request_window() @res->end is the size to reserve, not the
 *    last address (the core does "res->end += res->start - 1" afterwards),
 *    and it must be a multiple of s->map_size;
 *  - pcmcia_map_mem_page() wants an address in the card's own memory space,
 *    so consecutive pages are 64 KB apart, not "page" bytes apart.
 */
#define WIN_SIZE	0x10000		/* the aperture; the card owns sixteen */
#define WIN_PAGES	16
/* These are the bits the core copies from res->flags into the socket window
 * (WIN_FLAGS_MAP = 0x63). Without MAP_ACTIVE the window is reserved but never
 * enabled, and every read comes back 0xff - which is exactly what happened
 * until this was spotted. Careful: IORESOURCE_MEM_16BIT (0x08) is NOT the
 * same thing, it sits in WIN_FLAGS_REQ and would mean "window 1".
 */
#define SCP_ACTIVE	0x01		/* MAP_ACTIVE */
#define SCP_16BIT	0x02		/* MAP_16BIT */
#define SCP_ATTRIB	0x20		/* MAP_ATTRIB */

static struct resource *win_res, *attr_res;
static void __iomem *win_map, *attr_map;
static int win_tried, win_ret, attr_ret;

static int scp55_window_reserve(struct pcmcia_device *link, int which)
{
	struct resource *res = link->resource[which ? 3 : 2];
	int ret;

	if (!res)
		return -ENODEV;

	res->flags = IORESOURCE_MEM | SCP_ACTIVE | SCP_16BIT;
	if (which)
		res->flags |= SCP_ATTRIB;
	res->start = 0;
	res->end = WIN_SIZE;

	ret = pcmcia_request_window(link, res, 0);
	if (ret) {
		pr_info(DRV_NAME ": %s window refused (%d)\n",
			which ? "attribute" : "common", ret);
		return ret;
	}
	if (which)
		attr_res = res;
	else
		win_res = res;
	pr_info(DRV_NAME ": %s window %pR reserved\n",
		which ? "attribute" : "common", res);
	return 0;
}

static void scp55_port_snapshot(const char *when)
{
	mutex_lock(&io_mutex);
	pr_info(DRV_NAME ": %s: base+2=%02x base+3=%02x base+8=%02x base+9=%02x\n",
		when, inb(io_base + 2), inb(io_base + 3),
		inb(io_base + 8), inb(io_base + 9));
	mutex_unlock(&io_mutex);
}

static void scp55_dump_window(struct resource *res, void __iomem **map,
			      const char *what, int step, int count, int bytes)
{
	int page, i;

	*map = ioremap(res->start, WIN_SIZE);
	pr_info(DRV_NAME ": %s %pR mapped %s\n", what, res, *map ? "yes" : "no");
	if (!*map)
		return;

	for (page = 0; page < count; page++) {
		char buf[3 * 32 + 1];
		int n = 0;

		if (pcmcia_map_mem_page(scp55_dev, res, page * step)) {
			pr_info(DRV_NAME ": %s offset %#x not mappable\n",
				what, page * step);
			continue;
		}
		for (i = 0; i < bytes; i++)
			n += scnprintf(buf + n, sizeof(buf) - n, "%02x ",
				       readb(*map + i));
		pr_info(DRV_NAME ": %s %05x: %s\n", what, page * step, buf);
	}
}

static void scp55_dump_cis_via_socket(struct pcmcia_device *link)
{
	char buf[3 * 32 + 1];
	int n = 0, i;
	u8 v;

	for (i = 0; i < 32; i++) {
		v = 0;
		if (pcmcia_read_config_byte(link, i, &v))
			n += scnprintf(buf + n, sizeof(buf) - n, "?? ");
		else
			n += scnprintf(buf + n, sizeof(buf) - n, "%02x ", v);
	}
	pr_info(DRV_NAME ": attribute via socket: %s\n", buf);
}

/* Walk the whole attribute space in 32-byte lines: the CIS and any vendor
 * tuple live there, and the card's memory descriptor is what tells us how the
 * 1 MB is laid out.
 */
static void scp55_dump_attr_long(struct resource *res)
{
	int off, i;

	if (!res || !attr_map)
		return;
	for (off = 0; off < 0x400; off += 32) {
		char buf[3 * 32 + 1];
		int n = 0;
		int page = off / WIN_SIZE;

		/* The card pages are 64 KB, so the offset inside the page is
		 * what actually moves the read - the earlier dump mapped the
		 * window again but always read from its start.
		 */
		if (pcmcia_map_mem_page(scp55_dev, res, page * WIN_SIZE))
			continue;
		for (i = 0; i < 32; i++)
			n += scnprintf(buf + n, sizeof(buf) - n, "%02x ",
				       readb(attr_map + (off % WIN_SIZE) + i));
		pr_info(DRV_NAME ": attr %03x: %s\n", off, buf);
	}
}

/* Does any of the 1 MB common memory carry something? The wave engine is fed
 * from the host, so empty is expected - but "empty" and "not reachable" look
 * the same if you only sample the first bytes.
 */
static void scp55_scan_cmem(struct resource *res)
{
	int page, off, i, used;
	u32 sum;

	if (!res || !win_map)
		return;
	for (page = 0; page < WIN_PAGES; page++) {
		if (pcmcia_map_mem_page(scp55_dev, res, page * WIN_SIZE))
			continue;
		used = 0;
		sum = 0;
		for (off = 0; off < WIN_SIZE; off += 512) {
			for (i = 0; i < 512; i++) {
				u8 v = readb(win_map + off + i);
				if (v) {
					used++;
					sum += v;
				}
			}
		}
		pr_info(DRV_NAME ": cmem page %2d: %d/65536 non-zero (sum %u)\n",
			page, used, sum);
	}
}

/* Is the 1 MB real RAM or just an empty decode? Write a pattern into the
 * first bytes of page 0 and read it back. mem_probe=2 asks for this.
 */
static void scp55_write_test(struct resource *res, void __iomem *map)
{
	static const u8 pattern[16] = {
		0xa5, 0x5a, 0x0f, 0xf0, 0x33, 0xcc, 0x01, 0x80,
		0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0
	};
	u8 back[16];
	char before[3 * 16 + 1], after[3 * 16 + 1];
	int i, n;
	u8 zero = 0;

	if (!res || !map)
		return;
	if (pcmcia_map_mem_page(scp55_dev, res, 0))
		return;

	n = 0;
	for (i = 0; i < 16; i++)
		n += scnprintf(before + n, sizeof(before) - n, "%02x ", readb(map + i));
	for (i = 0; i < 16; i++)
		writeb(pattern[i], map + i);
	for (i = 0; i < 16; i++)
		back[i] = readb(map + i);
	n = 0;
	for (i = 0; i < 16; i++)
		n += scnprintf(after + n, sizeof(after) - n, "%02x ", back[i]);

	pr_info(DRV_NAME ": write test before: %s\n", before);
	pr_info(DRV_NAME ": write test after : %s\n", after);
	pr_info(DRV_NAME ": write test result: %s\n",
		memcmp(back, pattern, 16) ? "pattern NOT retained" : "RAM accepts writes");

	/* Put the card back the way we found it. */
	for (i = 0; i < 16; i++)
		writeb(zero, map + i);
}

/* Same test, but with 16-bit accesses: the 8-bit write came back with 0xf0 in
 * every second byte and shifted, which looks like a 16-bit memory behind a
 * byte-wide read. mem_probe=3 asks for this one.
 */
static void scp55_write_test16(struct resource *res, void __iomem *map)
{
	static const u16 words[8] = {
		0xa55a, 0x0ff0, 0x33cc, 0x0180,
		0x1234, 0x5678, 0x9abc, 0xdef0
	};
	u16 back[8];
	char before[6 * 8 + 1], after[6 * 8 + 1];
	int i, n;

	if (!res || !map)
		return;
	if (pcmcia_map_mem_page(scp55_dev, res, 0))
		return;

	n = 0;
	for (i = 0; i < 8; i++)
		n += scnprintf(before + n, sizeof(before) - n, "%04x ",
			       readw(map + i * 2));
	for (i = 0; i < 8; i++)
		writew(words[i], map + i * 2);
	for (i = 0; i < 8; i++)
		back[i] = readw(map + i * 2);
	n = 0;
	for (i = 0; i < 8; i++)
		n += scnprintf(after + n, sizeof(after) - n, "%04x ", back[i]);

	pr_info(DRV_NAME ": 16-bit before: %s\n", before);
	pr_info(DRV_NAME ": 16-bit after : %s\n", after);
	pr_info(DRV_NAME ": 16-bit result: %s\n",
		memcmp(back, words, sizeof(words)) ?
		"pattern NOT retained" : "RAM accepts 16-bit writes");

	for (i = 0; i < 8; i++)
		writew(0, map + i * 2);
}

/* Every address read back the last word written, so the window may be a
 * single latch that mirrors all addresses - or the address lines simply are
 * not wired. Write one word and read it back from several offsets and from a
 * different card page. mem_probe=4 asks for this.
 */
static void scp55_alias_test(struct resource *res, void __iomem *map)
{
	static const unsigned int offsets[] = { 0, 2, 4, 6, 8, 0x100, 0x1000 };
	u16 v;
	int i;

	if (!res || !map)
		return;

	if (pcmcia_map_mem_page(scp55_dev, res, 0))
		return;
	writew(0x1234, map);
	pr_info(DRV_NAME ": alias test, wrote 1234 at page 0 offset 0\n");

	for (i = 0; i < (int)ARRAY_SIZE(offsets); i++) {
		v = readw(map + offsets[i]);
		pr_info(DRV_NAME ": alias page 0 offset %#06x -> %04x\n",
			offsets[i], v);
	}

	if (pcmcia_map_mem_page(scp55_dev, res, 0x10000))
		return;
	pr_info(DRV_NAME ": alias page 1 offset 0 -> %04x\n", readw(map));

	if (pcmcia_map_mem_page(scp55_dev, res, 0))
		return;
	writew(0, map);
}

/* The 1996 driver reaches the wave/mixer side through base+2 (index) and
 * base+3 (data), the CS4231 idiom. Read-only on purpose: we only select a
 * register and read it back, we never write a value. mem_probe=5 asks for it.
 */
static void scp55_codec_port_probe(void)
{
	static const u8 idx[] = { 0x00, 0x04, 0x09, 0x0a, 0x0e, 0x10 };
	char buf[16 * 6 + 1];
	int i, n = 0;

	if (!io_base)
		return;

	mutex_lock(&io_mutex);
	for (i = 0; i < (int)ARRAY_SIZE(idx); i++) {
		u8 data, status;

		outb(idx[i], io_base + 2);
		udelay(50);
		status = inb(io_base + 3);
		data = inb(io_base + 3);
		n += scnprintf(buf + n, sizeof(buf) - n, "%02x:%02x/%02x ",
			       idx[i], status, data);
	}
	mutex_unlock(&io_mutex);
	pr_info(DRV_NAME ": codec port base+2/base+3: %s\n", buf);
}

static void scp55_window_map(struct pcmcia_device *link)
{
	/* The card is not a blank CD: 64 KB of attribute memory hold the CIS.
	 * If the CIS comes back as 0x01 0x03 ..., the window really talks to
	 * the card and an all-0xff common memory means empty, not broken.
	 */
	scp55_dump_cis_via_socket(link);
	if (attr_res) {
		scp55_dump_window(attr_res, &attr_map, "attr", WIN_SIZE, 1, 64);
		scp55_dump_attr_long(attr_res);
	}
	if (win_res) {
		scp55_dump_window(win_res, &win_map, "cmem", WIN_SIZE,
				  WIN_PAGES, 16);
		scp55_scan_cmem(win_res);
		if (mem_probe == 2)
			scp55_write_test(win_res, win_map);
		if (mem_probe == 3)
			scp55_write_test16(win_res, win_map);
		if (mem_probe >= 4)
			scp55_alias_test(win_res, win_map);
	}

	scp55_port_snapshot("window up");
}

static int scp55_probe(struct pcmcia_device *link)
{
	unsigned int base;
	u8 aux1, signature;
	int ret;

	io_base = 0;
	codec_ok = err_state = 0;

	/* CONF_AUTO_SET_IOMEM makes pcmcia_loop_config parse the memory
	 * descriptor of the CIS entry into link->resource[2]: the window has
	 * to be negotiated, not invented (asking for 1 MB by hand failed with
	 * -22).
	 */
	link->config_flags |= CONF_AUTO_SET_IO;
	if (mem_probe)
		link->config_flags |= CONF_AUTO_SET_IOMEM;
	link->io_lines = 10;

	ret = pcmcia_loop_config(link, scp55_conf_check, NULL);
	if (ret) {
		/* Never trade the working I/O path for the experiment. */
		pr_info(DRV_NAME ": config with the memory window failed (%d), retrying I/O only\n",
			ret);
		link->config_flags &= ~CONF_AUTO_SET_IOMEM;
		ret = pcmcia_loop_config(link, scp55_conf_check, NULL);
	}
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
	scp55_dev = link;

	/* Identify first, then program the control interface: both the
	 * handshake and the mode selection write base+4/base+5, and both
	 * overwrite the signature. The codec has to be initialised last,
	 * because those writes reset it.
	 */
	/* Leave the control pair alone at probe. Measured on hardware: a
	 * fresh card plays as it is, and writing base+4/base+5 here leaves
	 * the MPU unacknowledged and the synth mute. The handshake is only
	 * for recovering a card that has already lost its state - see
	 * scripts/scp55-vendor-probe.sh - and even then it does not always
	 * work.
	 */
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

	/* Never initialise the MPU here. The card is already in UART mode
	 * after power-on and does not acknowledge 0x3F, so mpu_init() would
	 * fall through to a 0xFF reset and leave the synth deaf. That is what
	 * broke 0.2.5/0.2.12 at boot: the test phrase is discarded either way,
	 * because the probe does not need the MPU to be touched at all.
	 */
	if (beep_on_probe)
		beep_locked();

	if (mem_probe) {
		scp55_port_snapshot("before window");
		scp55_window_map(link);
	}
	if (mem_probe >= 5)
		scp55_codec_port_probe();
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
	if (win_map) {
		iounmap(win_map);
		win_map = NULL;
	}
	win_res = NULL;
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
	u8 signature;

	ret = pcmcia_enable_device(link);
	if (ret) {
		/* S3 can leave the device in a state the core refuses to
		 * re-enable (-13) even though the card is still bound to the
		 * socket. Give up on the stale configuration and rebuild it
		 * exactly as probe does, instead of returning the error with
		 * the card unusable.
		 */
		pr_info(DRV_NAME ": resume: core enable failed (%d), re-configuring\n", ret);
		pcmcia_disable_device(link);
		link->config_flags |= CONF_AUTO_SET_IO;
		link->io_lines = 10;
		ret = pcmcia_loop_config(link, scp55_conf_check, NULL);
		if (ret) {
			pr_err(DRV_NAME ": resume: pcmcia_loop_config failed: %d\n", ret);
			return ret;
		}
		ret = pcmcia_enable_device(link);
		if (ret) {
			pr_err(DRV_NAME ": resume: pcmcia_enable_device failed: %d\n", ret);
			return ret;
		}
	}

	mutex_lock(&io_mutex);
	io_base = link->resource[0]->start;

	/* A suspend is a power cycle as far as the card is concerned: it loses
	 * the codec state *and* the routing mode. Read the signature first,
	 * because writing the mode register destroys it, then select the mode
	 * again before the codec bring-up. Skipping the mode write was why a
	 * resumed machine had working MIDI and a silent synth: the codec came
	 * up healthy, but the synth never reached the DAC.
	 */
	/* Read the signature before any write: both the control handshake and
	 * the mode selection overwrite base+5, and with it the 0xAA.
	 */
	signature = card_signature();
	card_control_init();
	if (card_mode)
		card_mode_select();
	codec_init();
	mpu_init();
	mutex_unlock(&io_mutex);

	pr_info(DRV_NAME ": resumed, io 0x%03x, signature 0x%02x, codec %s\n",
		io_base, signature, codec_ok ? "ok" : "silent");
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
MODULE_VERSION("0.2.26");
