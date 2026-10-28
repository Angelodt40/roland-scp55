// SPDX-License-Identifier: GPL-2.0
/* scp55beep - replay a test phrase on an already-configured Roland SCP-55 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>

#define MB 0x330

static int mpu_ready(void)
{
	int i;

	for (i = 0; i < 20000; i++)
		if (!(inb(MB + 1) & 0x40))
			return 1;
	return 0;
}

static void mo(u8 b)
{
	if (mpu_ready())
		outb(b, MB);
}

static void panic_all(void)
{
	int ch;

	for (ch = 0; ch < 16; ch++) {
		mo(0xb0 | ch); mo(0x78); mo(0x00);
	}
}

static const u8 gs[] = { 0xf0,0x41,0x10,0x42,0x12,0x40,0x00,0x7f,0x00,0x41,0xf7 };

static int __init beep_init(void)
{
	int i;

	panic_all();
	for (i = 0; i < sizeof(gs); i++) {
		mo(gs[i]);
		if (gs[i] == 0xf7)
			msleep(60);
	}
	/* ch1 = acoustic grand piano */
	mo(0xc0); mo(0x00);
	/* C major triad, then a held single note */
	mo(0x90); mo(0x3c); mo(0x64);
	mo(0x90); mo(0x40); mo(0x64);
	mo(0x90); mo(0x43); mo(0x64);
	msleep(1200);
	mo(0x80); mo(0x3c); mo(0x40);
	mo(0x80); mo(0x40); mo(0x40);
	mo(0x80); mo(0x43); mo(0x40);
	msleep(400);
	mo(0x90); mo(0x48); mo(0x6e);
	msleep(900);
	mo(0x80); mo(0x48); mo(0x40);
	pr_info("scp55beep: phrase sent\n");
	return 0;
}

static void __exit beep_exit(void)
{
	panic_all();
}

module_init(beep_init);
module_exit(beep_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCP-55 test phrase player");
