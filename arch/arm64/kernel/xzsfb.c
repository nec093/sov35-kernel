// SPDX-License-Identifier: GPL-2.0
/*
 * Debug-only boot console that draws printk output as text onto the
 * framebuffer the bootloader left behind (continuous splash), so a hang
 * can be diagnosed on a device that has no reachable UART.
 *
 * Enable with:  xzsfb=<phys>,<stride bytes>,<width>,<height>  keep_bootcon
 * (all four numbers optional; defaults are the Xperia XZs values)
 *
 * Text is drawn white-on-black in 16x32 cells (VGA 8x16 font, 2x scaled).
 * Drawing stops once userspace starts, because the splash memory is handed
 * back to the allocator by then.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/font.h>
#include <linux/io.h>
#include <asm/cacheflush.h>
#include <asm/early_ioremap.h>

/* provides font_vga_8x16 without needing CONFIG_FONT_SUPPORT */
#include "../../../lib/fonts/font_8x16.c"

#define XZSFB_SCALE	2
#define XZSFB_CELL_W	(8 * XZSFB_SCALE)
#define XZSFB_CELL_H	(16 * XZSFB_SCALE)

static phys_addr_t xzsfb_base = 0x83401000;
static unsigned int xzsfb_stride = 4352;
static unsigned int xzsfb_w = 1080;
static unsigned int xzsfb_h = 1920;
static unsigned int xzsfb_row, xzsfb_col;
static bool xzsfb_linear;	/* linear map usable (paging_init done) */
static bool xzsfb_dead;

static size_t xzsfb_rowbytes(void)
{
	return (size_t)xzsfb_stride * XZSFB_CELL_H;
}

static void *xzsfb_map(unsigned int row)
{
	phys_addr_t p = xzsfb_base + (phys_addr_t)row * xzsfb_rowbytes();

	if (xzsfb_linear)
		return __va(p);
	return (void __force *)early_ioremap(p, xzsfb_rowbytes());
}

static void xzsfb_unmap(void *m)
{
	if (!m)
		return;
	if (xzsfb_linear)
		__flush_dcache_area(m, xzsfb_rowbytes());
	else
		early_iounmap((void __iomem *)m, xzsfb_rowbytes());
}

static void xzsfb_clear_row(void *m)
{
	unsigned int y, x;

	/* not memset(): DC ZVA faults on the Device mapping used early on */
	for (y = 0; y < XZSFB_CELL_H; y++) {
		u32 *p = (u32 *)((u8 *)m + (size_t)y * xzsfb_stride);

		for (x = 0; x < xzsfb_w; x++)
			__raw_writel(0, (void __iomem *)p++);
	}
}

static void xzsfb_glyph(void *m, unsigned int col, unsigned char c)
{
	const u8 *g = font_vga_8x16.data + (unsigned int)c * 16;
	unsigned int y, x, sy, sx;

	for (y = 0; y < 16; y++) {
		u8 bits = g[y];

		for (sy = 0; sy < XZSFB_SCALE; sy++) {
			u32 *p = (u32 *)((u8 *)m +
				(size_t)(y * XZSFB_SCALE + sy) * xzsfb_stride) +
				col * XZSFB_CELL_W;

			for (x = 0; x < 8; x++) {
				u32 v = (bits & (0x80 >> x)) ? 0xffffffffu : 0;

				for (sx = 0; sx < XZSFB_SCALE; sx++)
					__raw_writel(v, (void __iomem *)p++);
			}
		}
	}
}

static void xzsfb_write(struct console *con, const char *s, unsigned int n)
{
	unsigned int cols = xzsfb_w / XZSFB_CELL_W;
	unsigned int rows = xzsfb_h / XZSFB_CELL_H;
	void *m;

	if (xzsfb_dead)
		return;
	if (system_state >= SYSTEM_RUNNING) {
		xzsfb_dead = true;
		return;
	}

	m = xzsfb_map(xzsfb_row);
	if (!m) {
		xzsfb_dead = true;
		return;
	}

	while (n--) {
		unsigned char c = *s++;

		if (c == '\r')
			continue;
		if (c == '\n' || xzsfb_col >= cols) {
			xzsfb_unmap(m);
			xzsfb_col = 0;
			if (++xzsfb_row >= rows)
				xzsfb_row = 0;
			m = xzsfb_map(xzsfb_row);
			if (!m) {
				xzsfb_dead = true;
				return;
			}
			xzsfb_clear_row(m);
			if (c == '\n')
				continue;
		}
		if (c < 0x20 || c > 0x7e)
			c = (c == '\t') ? ' ' : '?';
		xzsfb_glyph(m, xzsfb_col++, c);
	}
	xzsfb_unmap(m);
}

static struct console xzsfb_console = {
	.name	= "xzsfb",
	.write	= xzsfb_write,
	.flags	= CON_ENABLED | CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index	= -1,
};

static int __init xzsfb_setup(char *arg)
{
	unsigned long long v;
	char *end;

	if (arg && *arg) {
		v = simple_strtoull(arg, &end, 0);
		if (end != arg)
			xzsfb_base = v;
		if (*end == ',') {
			arg = end + 1;
			v = simple_strtoull(arg, &end, 0);
			if (end != arg)
				xzsfb_stride = v;
			if (*end == ',') {
				arg = end + 1;
				v = simple_strtoull(arg, &end, 0);
				if (end != arg)
					xzsfb_w = v;
				if (*end == ',') {
					arg = end + 1;
					v = simple_strtoull(arg, &end, 0);
					if (end != arg)
						xzsfb_h = v;
				}
			}
		}
	}

	/* blank the first row so the first flush lands on a clean line */
	{
		void *m = xzsfb_map(0);

		if (m) {
			xzsfb_clear_row(m);
			xzsfb_unmap(m);
		}
	}
	register_console(&xzsfb_console);
	return 0;
}
early_param("xzsfb", xzsfb_setup);

/* early_ioremap() is __init: switch to the linear map before init is freed */
static int __init xzsfb_switch_to_linear(void)
{
	console_lock();
	xzsfb_linear = true;
	console_unlock();
	return 0;
}
core_initcall(xzsfb_switch_to_linear);

/*
 * Debug aid #2: "xzpanic=<secs>" on the command line. <secs> after boot,
 * dump uninterruptible tasks and PID 1, then panic() so panic=N warm-reboots
 * and the whole log survives in ramoops (a power-key reset loses DRAM).
 */
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>

static unsigned int xzpanic_secs;

static void xzpanic_fn(struct work_struct *w)
{
	struct task_struct *t;
	int i;

	pr_err("XZDBG: timed dump, blocked tasks follow\n");
	show_state_filter(TASK_UNINTERRUPTIBLE);
	/* several samples of PID 1: a livelock shows the same spot every time */
	for (i = 0; i < 5; i++) {
		rcu_read_lock();
		t = find_task_by_vpid(1);
		if (t) {
			pr_err("XZDBG: pid 1 sample %d (%s) state %ld\n", i,
			       t->comm, t->state);
			sched_show_task(t);
		}
		rcu_read_unlock();
		msleep(300);
	}
	panic("XZDBG timed panic");
}
static DECLARE_DELAYED_WORK(xzpanic_work, xzpanic_fn);

static int __init xzpanic_setup(char *s)
{
	return kstrtouint(s, 0, &xzpanic_secs) == 0;
}
__setup("xzpanic=", xzpanic_setup);

static int __init xzpanic_init(void)
{
	if (xzpanic_secs)
		schedule_delayed_work(&xzpanic_work, xzpanic_secs * HZ);
	return 0;
}
late_initcall(xzpanic_init);
