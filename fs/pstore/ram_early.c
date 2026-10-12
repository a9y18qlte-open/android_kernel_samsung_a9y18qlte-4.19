// SPDX-License-Identifier: GPL-2.0
/*
 * Early boot console into the ramoops console zone.
 *
 * Bring-up aid: from early_param time, write printk output straight into
 * a persistent_ram buffer (same header as fs/pstore/ram_core.c). A kernel
 * that dies before ramoops probes then still leaves its log, and the next
 * boot's ramoops exports it as console-ramoops-0.
 *
 * Usage: ramoops_early=<phys>,<size>, e.g. ramoops_early=0x84d40000,0x40000
 * for the console zone of a 1 MiB ramoops region with 256 KiB records.
 * <size> must fit one early_ioremap slot (256 KiB on arm64 4K pages).
 *
 * The console is a boot console, so it stops once a real console (e.g.
 * the pstore console of the regular ramoops driver) registers. ramoops
 * then finds a valid buffer and keeps appending to it.
 *
 * Optionally the same text is drawn into the framebuffer the bootloader
 * left scanning out (continuous splash), for when nothing survives a reset:
 * ramoops_early_fb=<phys>,<width>,<height>,<bytes per pixel>,<stride>[,<scale>]
 * Text wraps around inside the mapped rows instead of scrolling, to keep
 * Device memory writes low; a cleared line marks the current position.
 * The first crash report (panic, oops, stack overflow) restarts at the top
 * and stops when the rows are full, so its beginning stays on screen.
 * ramoops_early_fb_mark=<string> freezes on the first line containing
 * <string> instead.
 */

#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <asm/early_ioremap.h>

#define PERSISTENT_RAM_SIG	0x43474244	/* DBGC */

/* struct persistent_ram_buffer: sig, start, size, data[] */
#define PRB_SIG		0
#define PRB_START	4
#define PRB_SIZE	8
#define PRB_DATA	12

static void __iomem *prb;
static u32 prb_cap, prb_pos, prb_used;

/* Framebuffer text output, one early_ioremap slot per chunk of lines */
#define FB_CHUNKS	6	/* of 7 early_ioremap slots */
#define FB_MAX_COLS	256

static void __iomem *fb_chunk[FB_CHUNKS];
static unsigned int fb_bpp, fb_stride, fb_line_bytes, fb_lines_per_chunk;
static unsigned int fb_lines, fb_cols, fb_x, fb_y, fb_scale;
static bool fb_crash;
static const struct font_desc *fb_font;

static void __iomem *fb_line(unsigned int line)
{
	return fb_chunk[line / fb_lines_per_chunk] +
	       (line % fb_lines_per_chunk) * fb_line_bytes;
}

static void fb_clear_line(unsigned int line)
{
	memset_io(fb_line(line), 0, fb_line_bytes);
}

static void fb_putc(unsigned char c)
{
	const u8 *glyph = (const u8 *)fb_font->data + c * fb_font->height;
	unsigned int cell_w = fb_font->width * fb_scale * fb_bpp;
	void __iomem *cell = fb_line(fb_y) + fb_x * cell_w;
	u8 row[8 * 4 * 4];
	unsigned int r, px, sy;

	for (r = 0; r < fb_font->height; r++) {
		for (px = 0; px < 8 * fb_scale; px++)
			memset(row + px * fb_bpp,
			       (glyph[r] & (0x80 >> (px / fb_scale))) ? 0xff : 0,
			       fb_bpp);
		for (sy = 0; sy < fb_scale; sy++)
			memcpy_toio(cell + (r * fb_scale + sy) * fb_stride,
				    row, cell_w);
	}
}

static void fb_newline(void)
{
	fb_x = 0;
	if (fb_crash && fb_y + 1 == fb_lines) {
		fb_lines = 0;	/* screen full: keep the crash report */
		return;
	}
	fb_y = (fb_y + 1) % fb_lines;
	fb_clear_line(fb_y);
	fb_clear_line((fb_y + 1) % fb_lines);	/* position marker */
}

static const char * const fb_crash_marks[] = {
	"Insufficient stack space",
	"Unable to handle kernel",
	"Internal error:",
	"Kernel panic",
	"kernel BUG at",
	"blocked for more than",
	"soft lockup",
	"hard LOCKUP",
	"Watchdog bark",
};

static char fb_mark[64];

static int __init ramoops_early_fb_mark_setup(char *arg)
{
	if (arg)
		strlcpy(fb_mark, arg, sizeof(fb_mark));
	return 0;
}
early_param("ramoops_early_fb_mark", ramoops_early_fb_mark_setup);

static void fb_check_crash(const char *s, unsigned int n)
{
	unsigned int i;

	if (fb_crash)
		return;
	/* the mark itself is on the command line */
	if (strnstr(s, "command line", n))
		return;
	for (i = 0; i <= ARRAY_SIZE(fb_crash_marks); i++) {
		const char *mark = i < ARRAY_SIZE(fb_crash_marks) ?
				   fb_crash_marks[i] : fb_mark;

		if (fb_mark[0] && i < ARRAY_SIZE(fb_crash_marks))
			continue;	/* a custom mark replaces the crash marks */
		if (mark[0] && strnstr(s, mark, n)) {
			fb_crash = true;
			fb_x = 0;
			fb_y = 0;
			fb_clear_line(0);
			fb_clear_line(1);
			return;
		}
	}
}

static void fb_write(const char *s, unsigned int n)
{
	fb_check_crash(s, n);
	while (n-- && fb_lines) {
		unsigned char c = *s++;

		if (c == '\n') {
			fb_newline();
			continue;
		}
		if (c < ' ' || c > '~')
			c = '?';
		if (fb_x == fb_cols) {
			fb_newline();
			if (!fb_lines)
				break;
		}
		fb_putc(c);
		fb_x++;
	}
}

/*
 * The zone is mapped as Device memory, so stick to single byte and
 * naturally aligned word accesses.
 */
static void ramoops_early_write(struct console *con, const char *s,
				unsigned int n)
{
	if (fb_lines)
		fb_write(s, n);

	if (!prb)
		return;

	while (n--) {
		writeb_relaxed(*s++, prb + PRB_DATA + prb_pos);
		if (++prb_pos == prb_cap)
			prb_pos = 0;
		if (prb_used < prb_cap)
			prb_used++;
	}
	writel_relaxed(prb_pos, prb + PRB_START);
	writel(prb_used, prb + PRB_SIZE);
}

static struct console ramoops_early_console = {
	.name	= "ramoops_early",
	.write	= ramoops_early_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index	= -1,
};

static int __init ramoops_early_setup(char *arg)
{
	unsigned long long phys, size;
	char *end;

	if (!arg)
		return -EINVAL;

	phys = memparse(arg, &end);
	if (*end != ',')
		return -EINVAL;
	size = memparse(end + 1, &end);
	if (size <= PRB_DATA)
		return -EINVAL;

	prb = early_ioremap(phys, size);
	if (!prb)
		return -ENOMEM;

	prb_cap = size - PRB_DATA;
	writel_relaxed(PERSISTENT_RAM_SIG, prb + PRB_SIG);
	writel_relaxed(0, prb + PRB_START);
	writel(0, prb + PRB_SIZE);

	if (!(ramoops_early_console.flags & CON_ENABLED))
		register_console(&ramoops_early_console);
	return 0;
}
early_param("ramoops_early", ramoops_early_setup);

static int __init ramoops_early_fb_setup(char *arg)
{
	unsigned long long phys, width, height, bpp, stride, scale = 1;
	unsigned int i, max_lines;
	char *p = arg;

	if (!p)
		return -EINVAL;
	phys = memparse(p, &p);
	if (*p++ != ',')
		return -EINVAL;
	width = memparse(p, &p);
	if (*p++ != ',')
		return -EINVAL;
	height = memparse(p, &p);
	if (*p++ != ',')
		return -EINVAL;
	bpp = memparse(p, &p);
	if (*p++ != ',')
		return -EINVAL;
	stride = memparse(p, &p);
	if (*p == ',')
		scale = memparse(p + 1, &p);
	if (!width || !height || !bpp || bpp > 4 || stride < width * bpp ||
	    !scale || scale > 4)
		return -EINVAL;

	fb_font = &font_vga_8x16;
	fb_bpp = bpp;
	fb_stride = stride;
	fb_scale = scale;
	fb_line_bytes = fb_font->height * scale * stride;
	/* each slot maps up to 256K, including the in-page offset */
	fb_lines_per_chunk = (SZ_256K - PAGE_SIZE) / fb_line_bytes;
	if (!fb_lines_per_chunk)
		return -EINVAL;

	max_lines = min_t(unsigned int, height / (fb_font->height * scale),
			  FB_CHUNKS * fb_lines_per_chunk);
	for (i = 0; i * fb_lines_per_chunk < max_lines; i++) {
		fb_chunk[i] = early_ioremap(phys +
				i * fb_lines_per_chunk * fb_line_bytes,
				fb_lines_per_chunk * fb_line_bytes);
		if (!fb_chunk[i])
			break;
	}
	fb_lines = min(max_lines, i * fb_lines_per_chunk);
	fb_cols = min_t(unsigned int, width / (fb_font->width * scale),
			FB_MAX_COLS);
	if (fb_lines < 3)
		return -ENOMEM;

	fb_clear_line(0);
	fb_clear_line(1);

	if (!(ramoops_early_console.flags & CON_ENABLED))
		register_console(&ramoops_early_console);
	return 0;
}
early_param("ramoops_early_fb", ramoops_early_fb_setup);
