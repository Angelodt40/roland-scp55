// SPDX-License-Identifier: GPL-2.0
/* scp55play - play arbitrary GM notes on a configured Roland SCP-55
 * insmod scp55play.ko prog=48 notes=60,64,67 dur=1200 vel=100
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>

#define MB 0x330

static int prog = 0;
static int dur = 900;
static int vel = 100;
static char *notes = "60";
module_param(prog, int, 0444);
module_param(dur, int, 0444);
module_param(vel, int, 0444);
module_param(notes, charp, 0444);

static int ready(void)
{
	int i;

	for (i = 0; i < 20000; i++)
		if (!(inb(MB + 1) & 0x40))
			return 1;
	return 0;
}

static void mo(u8 b)
{
	if (ready())
		outb(b, MB);
}

static const u8 gs[] = { 0xf0,0x41,0x10,0x42,0x12,0x40,0x00,0x7f,0x00,0x41,0xf7 };

static int __init play_init(void)
{
	u8 list[32];
	int n = 0, i;
	char *p = notes;

	while (*p && n < 32) {
		int v = 0;

		while (*p >= '0' && *p <= '9')
			v = v * 10 + (*p++ - '0');
		list[n++] = (u8)v;
		if (*p == ',')
			p++;
		else if (*p)
			break;
	}

	for (i = 0; i < 16; i++) {
		mo(0xb0 | i); mo(0x78); mo(0x00);
	}
	for (i = 0; i < sizeof(gs); i++) {
		mo(gs[i]);
		if (gs[i] == 0xf7)
			msleep(60);
	}
	mo(0xc0); mo((u8)prog);
	pr_info("scp55play: prog %d, %d notes, vel %d, %d ms\n", prog, n, vel, dur);

	for (i = 0; i < n; i++) {
		mo(0x90); mo(list[i]); mo((u8)vel);
	}
	msleep(dur);
	for (i = 0; i < n; i++) {
		mo(0x80); mo(list[i]); mo(0x40);
	}
	msleep(100);
	return 0;
}

static void __exit play_exit(void)
{
	int i;

	for (i = 0; i < 16; i++) {
		mo(0xb0 | i); mo(0x78); mo(0x00);
	}
}

module_init(play_init);
module_exit(play_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCP-55 note player");
