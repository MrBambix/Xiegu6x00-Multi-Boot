// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A NES emulator on the Xiegu X6100: a small libretro front end with the FCEUmm core (GPL-2.0)
 * linked in, and the system's whole userland, as Doom's (extras/doom): /sbin/init on a root
 * filesystem of its own, with the kernel of a system already on the card.
 *
 * The games: the user's own .nes / .fds / .unf / .nsf files in the NES folder of the card's BOOT
 * partition (any computer sees it); none comes with it. Saves (battery RAM, states) go to NES/SAVES,
 * pad layouts to NES/pads.txt, the Famicom Disk System's disksys.rom is looked for in NES/BIOS.
 *
 * Picture: the NES's 256x240 at 2x (512x480, sharp) or 4:3 (640x480), centred on the landscape
 * 800x480 screen, turned into the portrait panel's framebuffer as fbcon=rotate:3 does.
 * Input: the radio's keys and knobs, USB and Bluetooth keyboards, mice (the Zapper, for the games
 * that use it) and pads (HID: two players). Sound: the SoC's codec to the radio's speaker.
 * Leaving restarts the radio into the boot menu; POWER switches it off.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/reboot.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "libretro.h"
#include "font8x8.h"

#define LAND_W   800             /* the screen, as the radio is held */
#define LAND_H   480
#define MAX_DEVS 16
#define MAX_GAMES 2000

#define ROMS     "/boot/NES"
#define SAVES    ROMS "/SAVES"
#define BIOS     ROMS "/BIOS"
#define PADS     ROMS "/pads.txt"

/* The X6100's front keys: BTN_TRIGGER_HAPPY1..30 from the key matrix (OpenX6X00 src/front_keys.c) */
#define HAPPY(n) (BTN_TRIGGER_HAPPY1 + (n) - 1)

/* libretro's joypad ids, and ours */
enum {
	J_B = RETRO_DEVICE_ID_JOYPAD_B, J_Y = RETRO_DEVICE_ID_JOYPAD_Y, J_SELECT = RETRO_DEVICE_ID_JOYPAD_SELECT,
	J_START = RETRO_DEVICE_ID_JOYPAD_START, J_UP = RETRO_DEVICE_ID_JOYPAD_UP,
	J_DOWN = RETRO_DEVICE_ID_JOYPAD_DOWN, J_LEFT = RETRO_DEVICE_ID_JOYPAD_LEFT,
	J_RIGHT = RETRO_DEVICE_ID_JOYPAD_RIGHT, J_A = RETRO_DEVICE_ID_JOYPAD_A, J_X = RETRO_DEVICE_ID_JOYPAD_X,
	J_MENU = 16,                      /* ours: the emulator's menu */
};
#define BIT(j) (1u << (j))

/* What the menus hear */
enum nav { N_NONE, N_UP, N_DOWN, N_LEFT, N_RIGHT, N_OK, N_BACK, N_MENU };

static struct timespec t0;
static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}

/* ------------------------------------------------------------------------------------------ */
/* Settings: /boot/nes.txt                                                                    */
/* ------------------------------------------------------------------------------------------ */
static int set_volume = 100, set_rxvol = 20, set_rxmute = 0, set_headphone = 47, set_record = 1;
static int set_bluetooth = 1, set_aspect = 0, set_en = 0;

/* The user's words, in Polish or English (lang=en in nes.txt; the app writes it) */
#define T(pl, en) (set_en ? (en) : (pl))

/* ------------------------------------------------------------------------------------------ */
/* The log: nes-log.txt on the card's BOOT partition                                          */
/* ------------------------------------------------------------------------------------------ */
static FILE *log_fp;
static bool boot_rw;

static void logmsg(const char *fmt, ...)
{
	if (!log_fp)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(log_fp, "[%5ld.%01ld] ", (long)ts.tv_sec, ts.tv_nsec / 100000000);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(log_fp, fmt, ap);
	va_end(ap);
	fputc('\n', log_fp);
	fflush(log_fp);
}

static void log_file(const char *title, const char *path, size_t max)
{
	FILE *f = fopen(path, "r");
	if (!log_fp)
		return;
	fprintf(log_fp, "----- %s (%s)%s\n", title, path, f ? "" : ": none");
	if (f) {
		char buf[512];
		size_t n, total = 0;
		while (total < max && (n = fread(buf, 1, sizeof buf, f)) > 0) {
			fwrite(buf, 1, n, log_fp);
			total += n;
		}
		fclose(f);
	}
	fflush(log_fp);
}

static void log_dmesg(void)
{
	if (!log_fp)
		return;
	int len = klogctl(10, NULL, 0);
	if (len <= 0)
		return;
	char *buf = malloc((size_t)len);
	if (!buf)
		return;
	int n = klogctl(3, buf, len);
	if (n > 0) {
		int from = n > 16000 ? n - 16000 : 0;
		fprintf(log_fp, "----- kernel messages (last %d bytes)\n", n - from);
		fwrite(buf + from, 1, (size_t)(n - from), log_fp);
		fputc('\n', log_fp);
	}
	free(buf);
	fflush(log_fp);
}

static void save_sram(void);

/* Everything said so far kept, the card's BOOT left clean before the radio restarts or stops */
static void close_log(void)
{
	if (log_fp) {
		log_file("Bluetooth script", "/bt/tmp/nes-bt.log", 8000);
		log_file("input devices", "/proc/bus/input/devices", 12000);
		log_dmesg();
		logmsg("end");
		fclose(log_fp);
		log_fp = NULL;
	}
	if (boot_rw) {
		sync();
		umount2("/boot", MNT_DETACH);
		boot_rw = false;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* The radio's base board (I2C bus 0, address 0x72), as its own software drives it            */
/* ------------------------------------------------------------------------------------------ */
static bool base_write(int fd, unsigned reg, uint32_t val)
{
	uint16_t addr = (uint16_t)(reg * 4);
	uint8_t buf[6] = {(uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)val, (uint8_t)(val >> 8),
	                  (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
	struct i2c_msg msg = {.addr = 0x72, .flags = 0, .len = sizeof buf, .buf = buf};
	struct i2c_rdwr_ioctl_data d = {.msgs = &msg, .nmsgs = 1};
	if (ioctl(fd, I2C_RDWR, &d) < 0) {
		logmsg("base board register %u: %s", reg, strerror(errno));
		return false;
	}
	return true;
}

/* Register 12's power-off bit: the base board cuts the power (Linux's power-off alone left it on) */
static bool radio_poweroff(void)
{
	int fd = open("/dev/i2c-0", O_RDWR);
	if (fd < 0)
		return false;
	bool ok = base_write(fd, 12, (set_record ? 0x8u : 0u) | 0x10000u);
	close(fd);
	return ok;
}

/* The speaker playing the SoC's sound, as OpenX6X00's AUDIO_PLAY_ON (see extras/doom/x6100_sound.c) */
static void radio_audio(void)
{
	int vol = set_rxvol < 0 || set_rxvol > 55 ? 20 : set_rxvol;
	int fd = open("/dev/i2c-0", O_RDWR);
	if (fd < 0) {
		logmsg("sound: /dev/i2c-0: %s", strerror(errno));
		return;
	}
	bool ok = base_write(fd, 20, 10u | (10u << 8)) &&
	          base_write(fd, 12, set_record ? 0x8 : 0) &&
	          base_write(fd, 14, (uint32_t)vol);
	if (ok && set_rxmute)
		ok = base_write(fd, 15, 10u << 8);
	logmsg("sound: radio speaker %s (AF volume %d)", ok ? "set" : "NOT set", vol);
	close(fd);
}

/* ------------------------------------------------------------------------------------------ */
/* Leaving: back to the boot menu, or off                                                    */
/* ------------------------------------------------------------------------------------------ */
static int tty_fd = -1;
static bool test_mode;

static void text_mode(void)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void leave(int cmd)
{
	save_sram();
	text_mode();
	logmsg(cmd == (int)RB_POWER_OFF ? "POWER: switching off" : "leaving: restart");
	close_log();
	sync();
	if (getpid() != 1)
		exit(0);
	if (cmd == (int)RB_POWER_OFF) {
		radio_poweroff();
		sleep(1);
	}
	reboot(cmd);
	/* still here: the other way */
	radio_poweroff();
	sleep(1);
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}

static void on_signal(int sig)
{
	(void)sig;
	leave(RB_AUTOBOOT);
}

/* ------------------------------------------------------------------------------------------ */
/* The screen                                                                                 */
/* ------------------------------------------------------------------------------------------ */
static uint8_t *fb;
static int fb_fd = -1;
static unsigned fb_w, fb_h, fb_stride, fb_bpp;
static struct fb_var_screeninfo vinfo;
static int rotate = 3;                          /* 3: as fbcon=rotate:3; 1: the other way; 0: none */
static bool fb_native;                          /* 32 bpp XRGB: pixels as they are */
static uint32_t canvas[LAND_H * LAND_W];        /* the menus are drawn here, landscape */

static inline void put(unsigned fx, unsigned fy, uint32_t p)
{
	uint8_t *d = fb + fy * fb_stride + fx * fb_bpp;
	if (fb_native) {
		*(uint32_t *)d = p;
		return;
	}
	uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
	if (fb_bpp == 4)
		*(uint32_t *)d = (r << vinfo.red.offset) | (g << vinfo.green.offset) | (b << vinfo.blue.offset);
	else
		*(uint16_t *)d = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* landscape (lx, ly) -> the framebuffer's (fx, fy) */
static inline void land_to_fb(unsigned lx, unsigned ly, unsigned *fx, unsigned *fy)
{
	if (rotate == 3) {
		*fx = ly;
		*fy = fb_h - 1 - lx;
	} else if (rotate == 1) {
		*fx = fb_w - 1 - ly;
		*fy = lx;
	} else {
		*fx = lx;
		*fy = ly;
	}
}

/* A part of the canvas onto the screen */
static void present(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > LAND_W) w = LAND_W - x;
	if (y + h > LAND_H) h = LAND_H - y;
	for (int lx = x; lx < x + w; lx++)
		for (int ly = y; ly < y + h; ly++) {
			unsigned fx, fy;
			land_to_fb((unsigned)lx, (unsigned)ly, &fx, &fy);
			if (fx < fb_w && fy < fb_h)
				put(fx, fy, canvas[ly * LAND_W + lx]);
		}
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
	for (int yy = y < 0 ? 0 : y; yy < y + h && yy < LAND_H; yy++)
		for (int xx = x < 0 ? 0 : x; xx < x + w && xx < LAND_W; xx++)
			canvas[yy * LAND_W + xx] = c;
}

/* Polish letters (UTF-8) as their plain ones: the font is ASCII */
static int next_char(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	if (p[0] < 0x80) {
		(*s)++;
		return p[0];
	}
	if ((p[0] & 0xe0) == 0xc0 && p[1]) {
		unsigned cp = ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu);
		*s += 2;
		static const struct { unsigned cp; char c; } pl[] = {
			{0x105, 'a'}, {0x107, 'c'}, {0x119, 'e'}, {0x142, 'l'}, {0x144, 'n'}, {0xf3, 'o'},
			{0x15b, 's'}, {0x17a, 'z'}, {0x17c, 'z'}, {0x104, 'A'}, {0x106, 'C'}, {0x118, 'E'},
			{0x141, 'L'}, {0x143, 'N'}, {0xd3, 'O'}, {0x15a, 'S'}, {0x179, 'Z'}, {0x17b, 'Z'},
		};
		for (size_t i = 0; i < sizeof pl / sizeof pl[0]; i++)
			if (pl[i].cp == cp)
				return pl[i].c;
		return '?';
	}
	int n = (p[0] & 0xf0) == 0xe0 ? 3 : (p[0] & 0xf8) == 0xf0 ? 4 : 1;
	while (n-- && **s)
		(*s)++;
	return '?';
}

/* Text at (x, y), each font pixel s x s; how wide it came out. max: characters at most */
static int text(int x, int y, int s, uint32_t c, const char *str, int max)
{
	int x0 = x, n = 0;
	while (*str && n < max) {
		int ch = next_char(&str);
		if (ch < 32 || ch > 126)
			ch = '?';
		const unsigned char *g = font8x8_basic[ch];
		for (int r = 0; r < 8; r++)
			for (int b = 0; b < 8; b++)
				if (g[r] & (1 << b))
					fill(x + b * s, y + r * s, s, s, c);
		x += 8 * s;
		n++;
	}
	return x - x0;
}

static int text_len(const char *s)
{
	int n = 0;
	while (*s) {
		next_char(&s);
		n++;
	}
	return n;
}

/* The palette: the app's flat look, green accent */
#define C_BG     0x0b1014u
#define C_PANEL  0x151d24u
#define C_LINE   0x2a3640u
#define C_TEXT   0xe8eef2u
#define C_DIM    0x8a99a6u
#define C_ACCENT 0x2fbf71u
#define C_SEL_T  0x08130du
#define C_WARN   0xf0b429u

/* ------------------------------------------------------------------------------------------ */
/* Input                                                                                      */
/* ------------------------------------------------------------------------------------------ */
enum role { ROLE_RADIO, ROLE_MOUSE, ROLE_KEYBOARD, ROLE_PAD };

/* A pad's binding: a key, or an axis pushed one way */
struct bind {
	uint8_t kind;            /* 0 none, 1 key, 2 axis */
	int8_t sign;             /* the axis's way: -1, +1 */
	uint16_t code;
};
#define SLOTS 17             /* libretro's ids 0..11, our menu at 16 */
#define ALTS  4
struct padmap {
	struct bind b[SLOTS][ALTS];
};

struct dev {
	int fd, role, port;
	char node[24], name[80];
	uint8_t keys[KEY_MAX / 8 + 1];     /* the keys held */
	int32_t abs[ABS_MAX + 1];
	int32_t amin[ABS_MAX + 1], amax[ABS_MAX + 1];
	struct padmap map;
	uint32_t held;                     /* libretro bits */
};

static struct dev devs[MAX_DEVS];
static int n_devs;
static struct pollfd pfd[MAX_DEVS];
static uint32_t last_scan;

static uint32_t radio_bits, kb_bits;       /* port 0 */
static uint32_t knob_bits;                  /* port 0: a knob's turn held for a while */
static uint32_t knob_until[3];
static uint32_t knob_bit[3];
static int mouse_dx, mouse_dy, mouse_btn;   /* gathered until the core reads them */
static int frame_mdx, frame_mdy;
static uint32_t frame_bits[2];
static bool want_menu;                      /* the emulator's menu asked for */
static enum nav navq[32];
static unsigned nav_w, nav_r;
static uint32_t prev_nav_bits;
static uint32_t repeat_at;
static bool quick_save, quick_load;

/* learning a pad: what came last */
static int learn_dev = -1;                  /* -2: any pad; >= 0: that one */
static struct bind learn_got;
static int learn_from = -1;
static int32_t learn_base[MAX_DEVS][ABS_MAX + 1];    /* the axes as they were: triggers rest at one end */

static void nav_push(enum nav n)
{
	navq[nav_w] = n;
	nav_w = (nav_w + 1) % 32;
	if (nav_w == nav_r)
		nav_r = (nav_r + 1) % 32;
}

static enum nav nav_pop(void)
{
	if (nav_r == nav_w)
		return N_NONE;
	enum nav n = navq[nav_r];
	nav_r = (nav_r + 1) % 32;
	return n;
}

#define TEST_BIT(bits, b) ((bits)[(b) / 8] & (1 << ((b) % 8)))

/* The radio's keys on the NES pad (player 1) */
static int radio_key(unsigned code)
{
	switch (code) {
	case HAPPY(6):  return J_UP;       /* BAND up */
	case HAPPY(5):  return J_DOWN;     /* BAND down */
	case HAPPY(8):  return J_LEFT;     /* DFN */
	case HAPPY(9):  return J_RIGHT;    /* DFL */
	case HAPPY(13): return J_B;        /* F1 */
	case HAPPY(14): return J_A;        /* F2 */
	case HAPPY(4):  return J_A;        /* PTT */
	case HAPPY(15): return J_SELECT;   /* F3 */
	case HAPPY(19): return J_START;    /* F4 */
	case HAPPY(20): return J_X;        /* F5: A turbo */
	case HAPPY(27): return J_START;    /* MFK pressed */
	case HAPPY(1):  return J_MENU;     /* GEN */
	case HAPPY(21): return J_MENU;     /* VOL pressed */
	default:        return -1;
	}
}

/* A keyboard: arrows, X/K = A, Z/J = B, Enter = Start, Shift/Space = Select, Esc = the menu */
static int kb_key(unsigned code)
{
	switch (code) {
	case KEY_UP: case KEY_W: return J_UP;
	case KEY_DOWN: case KEY_S: return J_DOWN;
	case KEY_LEFT: case KEY_A: return J_LEFT;
	case KEY_RIGHT: case KEY_D: return J_RIGHT;
	case KEY_X: case KEY_K: return J_A;
	case KEY_Z: case KEY_J: return J_B;
	case KEY_C: case KEY_L: return J_X;
	case KEY_V: case KEY_H: return J_Y;
	case KEY_ENTER: case KEY_KPENTER: return J_START;
	case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_SPACE: case KEY_BACKSPACE: return J_SELECT;
	case KEY_ESC: case KEY_F1: return J_MENU;
	default: return -1;
	}
}

static void bind_key(struct padmap *m, int slot, unsigned code)
{
	for (int i = 0; i < ALTS; i++)
		if (!m->b[slot][i].kind) {
			m->b[slot][i] = (struct bind){1, 0, (uint16_t)code};
			return;
		}
}

static void bind_axis(struct padmap *m, int slot, unsigned code, int sign)
{
	for (int i = 0; i < ALTS; i++)
		if (!m->b[slot][i].kind) {
			m->b[slot][i] = (struct bind){2, (int8_t)sign, (uint16_t)code};
			return;
		}
}

/* Most HID pads: the d-pad as a hat or the left stick, A on the right of the face buttons */
static void default_map(struct dev *d)
{
	struct padmap *m = &d->map;
	memset(m, 0, sizeof *m);
	bind_axis(m, J_UP, ABS_HAT0Y, -1);    bind_axis(m, J_UP, ABS_Y, -1);
	bind_axis(m, J_DOWN, ABS_HAT0Y, 1);   bind_axis(m, J_DOWN, ABS_Y, 1);
	bind_axis(m, J_LEFT, ABS_HAT0X, -1);  bind_axis(m, J_LEFT, ABS_X, -1);
	bind_axis(m, J_RIGHT, ABS_HAT0X, 1);  bind_axis(m, J_RIGHT, ABS_X, 1);
	bind_key(m, J_UP, BTN_DPAD_UP);       bind_key(m, J_DOWN, BTN_DPAD_DOWN);
	bind_key(m, J_LEFT, BTN_DPAD_LEFT);   bind_key(m, J_RIGHT, BTN_DPAD_RIGHT);
	bind_key(m, J_A, BTN_EAST);           bind_key(m, J_A, BTN_THUMB);      /* BTN_B; a joystick's 2nd */
	bind_key(m, J_B, BTN_SOUTH);          bind_key(m, J_B, BTN_TRIGGER);    /* BTN_A; a joystick's 1st */
	bind_key(m, J_X, BTN_NORTH);          bind_key(m, J_X, BTN_THUMB2);
	bind_key(m, J_Y, BTN_WEST);           bind_key(m, J_Y, BTN_TOP);
	bind_key(m, J_SELECT, BTN_SELECT);    bind_key(m, J_SELECT, BTN_BASE3);
	bind_key(m, J_START, BTN_START);      bind_key(m, J_START, BTN_BASE4);
	bind_key(m, J_MENU, BTN_MODE);
}

/* NES/pads.txt: one line per pad, its name, a tab, then SLOT=K<code> or SLOT=A<code>+ / - */
static const char *slot_name[SLOTS] = {
	[J_B] = "B", [J_Y] = "TB", [J_SELECT] = "SELECT", [J_START] = "START", [J_UP] = "UP",
	[J_DOWN] = "DOWN", [J_LEFT] = "LEFT", [J_RIGHT] = "RIGHT", [J_A] = "A", [J_X] = "TA",
	[J_MENU] = "MENU",
};

static bool load_map(struct dev *d)
{
	FILE *f = fopen(PADS, "r");
	char line[1024];
	bool found = false;
	while (f && fgets(line, sizeof line, f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		if (strcmp(line, d->name))
			continue;
		struct padmap m;
		memset(&m, 0, sizeof m);
		for (char *t = strtok(tab + 1, " \r\n"); t; t = strtok(NULL, " \r\n")) {
			char *eq = strchr(t, '=');
			if (!eq)
				continue;
			*eq = 0;
			for (int s = 0; s < SLOTS; s++) {
				if (!slot_name[s] || strcmp(slot_name[s], t))
					continue;
				int code = atoi(eq + 2);
				if (eq[1] == 'K')
					bind_key(&m, s, (unsigned)code);
				else if (eq[1] == 'A')
					bind_axis(&m, s, (unsigned)code, strchr(eq + 2, '-') ? -1 : 1);
			}
		}
		/* the menu: the guide button, whatever the layout */
		bind_key(&m, J_MENU, BTN_MODE);
		d->map = m;
		found = true;
	}
	if (f)
		fclose(f);
	return found;
}

static void save_map(const struct dev *d)
{
	char lines[64][1024];
	int n = 0;
	FILE *f = fopen(PADS, "r");
	while (f && n < 63 && fgets(lines[n], sizeof lines[n], f)) {
		char *tab = strchr(lines[n], '\t');
		if (tab && (size_t)(tab - lines[n]) == strlen(d->name) && !strncmp(lines[n], d->name, strlen(d->name)))
			continue;                      /* this pad's old line goes */
		n++;
	}
	if (f)
		fclose(f);
	char *l = lines[n++];
	int k = snprintf(l, 1024, "%s\t", d->name);
	for (int s = 0; s < SLOTS; s++) {
		const struct bind *b = &d->map.b[s][0];
		if (!slot_name[s] || !b->kind || s == J_MENU)
			continue;
		if (b->kind == 1)
			k += snprintf(l + k, (size_t)(1024 - k), "%s=K%u ", slot_name[s], b->code);
		else
			k += snprintf(l + k, (size_t)(1024 - k), "%s=A%u%c ", slot_name[s], b->code, b->sign < 0 ? '-' : '+');
	}
	snprintf(l + k, (size_t)(1024 - k), "\n");
	f = fopen(PADS, "w");
	if (!f) {
		logmsg("pads.txt: %s", strerror(errno));
		return;
	}
	for (int i = 0; i < n; i++)
		fputs(lines[i], f);
	fclose(f);
	sync();
}

static bool axis_on(const struct dev *d, unsigned code, int sign)
{
	int32_t lo = d->amin[code], hi = d->amax[code], v = d->abs[code];
	if (hi <= lo)
		return false;
	if (hi - lo <= 2)                     /* a hat: -1, 0, 1 */
		return sign < 0 ? v < 0 : v > 0;
	int32_t mid = lo + (hi - lo) / 2, half = (hi - lo) / 2;
	return sign < 0 ? v < mid - half / 2 : v > mid + half / 2;
}

static uint32_t pad_bits(const struct dev *d)
{
	uint32_t bits = 0;
	for (int s = 0; s < SLOTS; s++)
		for (int i = 0; i < ALTS; i++) {
			const struct bind *b = &d->map.b[s][i];
			if ((b->kind == 1 && b->code <= KEY_MAX && TEST_BIT(d->keys, b->code)) ||
			    (b->kind == 2 && b->code <= ABS_MAX && axis_on(d, b->code, b->sign))) {
				bits |= BIT(s);
				break;
			}
		}
	/* Select and Start together: the menu, on pads without a guide button */
	if ((bits & BIT(J_SELECT)) && (bits & BIT(J_START)))
		bits = (bits & ~(BIT(J_SELECT) | BIT(J_START))) | BIT(J_MENU);
	return bits;
}

static int classify(int fd)
{
	unsigned char keys[KEY_MAX / 8 + 1], abs[ABS_MAX / 8 + 1];
	memset(keys, 0, sizeof keys);
	memset(abs, 0, sizeof abs);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
	if (TEST_BIT(keys, BTN_GAMEPAD) || TEST_BIT(keys, BTN_TRIGGER) || TEST_BIT(keys, BTN_EAST) ||
	    TEST_BIT(keys, BTN_START) || ((TEST_BIT(abs, ABS_X) || TEST_BIT(abs, ABS_HAT0X)) && !TEST_BIT(keys, BTN_TOUCH) &&
	                                  !TEST_BIT(keys, BTN_LEFT) && !TEST_BIT(keys, KEY_A)))
		return ROLE_PAD;
	if (TEST_BIT(keys, BTN_LEFT))
		return ROLE_MOUSE;
	if (TEST_BIT(keys, KEY_A) && TEST_BIT(keys, KEY_ENTER))
		return ROLE_KEYBOARD;
	return ROLE_RADIO;                    /* the key matrix, the knobs, the POWER key */
}

/* Which player a new pad is: the first free of 1 and 2 */
static int free_port(void)
{
	bool used[2] = {false, false};
	for (int i = 0; i < n_devs; i++)
		if (devs[i].role == ROLE_PAD && devs[i].port >= 0 && devs[i].port < 2)
			used[devs[i].port] = true;
	return !used[0] ? 0 : !used[1] ? 1 : 1;
}

static void drop_input(int i)
{
	logmsg("input %s (\"%s\") gone", devs[i].node, devs[i].name);
	close(devs[i].fd);
	for (int j = i; j < n_devs - 1; j++) {
		devs[j] = devs[j + 1];
		pfd[j] = pfd[j + 1];
	}
	n_devs--;
	if (learn_from == i)
		learn_from = -1;
}

static void open_inputs(void)
{
	if (test_mode)
		return;
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d)) && n_devs < MAX_DEVS) {
		char path[300];
		bool known = false;
		if (strncmp(e->d_name, "event", 5))
			continue;
		for (int i = 0; i < n_devs; i++)
			if (!strcmp(devs[i].node, e->d_name))
				known = true;
		if (known)
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGRAB, 1);
		struct dev *v = &devs[n_devs];
		memset(v, 0, sizeof *v);
		v->fd = fd;
		v->port = -1;
		snprintf(v->node, sizeof v->node, "%.23s", e->d_name);
		strcpy(v->name, "?");
		ioctl(fd, EVIOCGNAME(sizeof v->name), v->name);
		for (char *c = v->name; *c; c++)
			if (*c == '\t' || *c == '\n')
				*c = ' ';
		v->role = classify(fd);
		if (v->role == ROLE_PAD) {
			unsigned char abs[ABS_MAX / 8 + 1];
			memset(abs, 0, sizeof abs);
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
			for (unsigned a = 0; a <= ABS_MAX; a++)
				if (TEST_BIT(abs, a)) {
					struct input_absinfo ai;
					if (ioctl(fd, EVIOCGABS(a), &ai) == 0) {
						v->amin[a] = ai.minimum;
						v->amax[a] = ai.maximum;
						v->abs[a] = ai.value;
					}
				}
			if (!load_map(v))
				default_map(v);
			v->port = free_port();
		}
		static const char *roles[] = {"radio", "mouse", "keyboard", "pad"};
		logmsg("input %s: \"%s\" -> %s%s", e->d_name, v->name, roles[v->role],
		       v->role == ROLE_PAD ? (v->port ? " (player 2)" : " (player 1)") : "");
		pfd[n_devs].fd = fd;
		pfd[n_devs].events = POLLIN;
		n_devs++;
	}
	closedir(d);
}

/* A knob turned: its direction held for a while per detent (in the game), a step (in the menus) */
static void knob(int axis, int value)
{
	if (axis == 0)
		nav_push(value > 0 ? N_DOWN : N_UP);
	else
		nav_push(value > 0 ? N_UP : N_DOWN);
	uint32_t bit = axis == 0 ? (value > 0 ? BIT(J_RIGHT) : BIT(J_LEFT))
	                         : (value > 0 ? BIT(J_UP) : BIT(J_DOWN));
	uint32_t t = now_ms();
	if (knob_bit[axis] != bit)
		knob_until[axis] = t;
	knob_bit[axis] = bit;
	if ((int32_t)(knob_until[axis] - t) < 0)
		knob_until[axis] = t;
	knob_until[axis] += 60u * (uint32_t)(value < 0 ? -value : value);
	if (knob_until[axis] > t + 400)
		knob_until[axis] = t + 400;
}

static void learn_event(int i, const struct input_event *ev)
{
	struct dev *d = &devs[i];
	if (learn_dev == -1 || d->role != ROLE_PAD || (learn_dev >= 0 && learn_dev != i))
		return;
	if (ev->type == EV_KEY && ev->value == 1) {
		learn_got = (struct bind){1, 0, ev->code};
		learn_from = i;
	} else if (ev->type == EV_ABS && ev->code <= ABS_MAX) {
		struct dev before = *d;
		before.abs[ev->code] = learn_base[i][ev->code];
		for (int s = -1; s <= 1; s += 2)
			if (axis_on(d, ev->code, s) && !axis_on(&before, ev->code, s)) {
				learn_got = (struct bind){2, (int8_t)s, ev->code};
				learn_from = i;
			}
	}
}

static void poll_input(void)
{
	struct input_event ev;
	uint32_t t = now_ms();
	if (t - last_scan > 1000) {           /* a mouse, keyboard or pad plugged in meanwhile */
		last_scan = t;
		open_inputs();
	}
	while (n_devs && poll(pfd, (nfds_t)n_devs, 0) > 0) {
		bool any = false;
		for (int i = 0; i < n_devs; i++) {
			if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				drop_input(i--);
				continue;
			}
			if (!(pfd[i].revents & POLLIN))
				continue;
			ssize_t r = read(devs[i].fd, &ev, sizeof ev);
			if (r < 0 && errno == ENODEV) {
				drop_input(i--);
				continue;
			}
			if (r != (ssize_t)sizeof ev)
				continue;
			any = true;
			struct dev *d = &devs[i];
			if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
				leave(RB_POWER_OFF);
			switch (d->role) {
			case ROLE_RADIO:
				if (ev.type == EV_KEY && (ev.value == 0 || ev.value == 1)) {
					int j = radio_key(ev.code);
					if (j >= 0)
						radio_bits = ev.value ? radio_bits | BIT(j) : radio_bits & ~BIT(j);
				} else if (ev.type == EV_REL && ev.value) {
					if (ev.code == REL_X) knob(0, ev.value);          /* main knob */
					else if (ev.code == REL_Y) knob(1, ev.value);     /* VOL */
					else if (ev.code == REL_Z) knob(2, ev.value);     /* MFK */
				}
				break;
			case ROLE_KEYBOARD:
				if (ev.type == EV_KEY && (ev.value == 0 || ev.value == 1)) {
					if (ev.code == KEY_F2 && ev.value) quick_save = true;
					if (ev.code == KEY_F4 && ev.value) quick_load = true;
					int j = kb_key(ev.code);
					if (j >= 0)
						kb_bits = ev.value ? kb_bits | BIT(j) : kb_bits & ~BIT(j);
				}
				break;
			case ROLE_MOUSE:
				if (ev.type == EV_REL) {
					if (ev.code == REL_X) mouse_dx += ev.value;
					else if (ev.code == REL_Y) mouse_dy += ev.value;
					else if (ev.code == REL_WHEEL && ev.value) nav_push(ev.value > 0 ? N_UP : N_DOWN);
				} else if (ev.type == EV_KEY && (ev.value == 0 || ev.value == 1)) {
					int b = ev.code == BTN_LEFT ? 1 : ev.code == BTN_RIGHT ? 2 : ev.code == BTN_MIDDLE ? 4 : 0;
					mouse_btn = ev.value ? mouse_btn | b : mouse_btn & ~b;
					if (ev.value && b == 4) {      /* the middle button: the menu */
						nav_push(N_MENU);
						want_menu = true;
					}
				}
				break;
			case ROLE_PAD:
				if (ev.type == EV_KEY && ev.code <= KEY_MAX && (ev.value == 0 || ev.value == 1)) {
					if (ev.value) d->keys[ev.code / 8] |= (uint8_t)(1 << (ev.code % 8));
					else d->keys[ev.code / 8] &= (uint8_t)~(1 << (ev.code % 8));
				} else if (ev.type == EV_ABS && ev.code <= ABS_MAX) {
					d->abs[ev.code] = ev.value;
				}
				learn_event(i, &ev);
				break;
			}
		}
		if (!any)
			break;
	}
	t = now_ms();
	knob_bits = 0;
	for (int a = 0; a < 2; a++)
		if (knob_bit[a] && (int32_t)(knob_until[a] - t) > 0)
			knob_bits |= knob_bit[a];
	/* the players' pads */
	uint32_t p[2] = {radio_bits | kb_bits | knob_bits, 0};
	for (int i = 0; i < n_devs; i++)
		if (devs[i].role == ROLE_PAD && devs[i].port >= 0) {
			devs[i].held = learn_dev == -1 ? pad_bits(&devs[i]) : 0;
			p[devs[i].port] |= devs[i].held;
		}
	frame_bits[0] = p[0] & 0xfff;
	frame_bits[1] = p[1] & 0xfff;
	/* the menus: a press, and a held direction again and again */
	uint32_t nb = p[0] | p[1];
	uint32_t pressed = nb & ~prev_nav_bits;
	if (pressed & BIT(J_UP)) nav_push(N_UP);
	if (pressed & BIT(J_DOWN)) nav_push(N_DOWN);
	if (pressed & BIT(J_LEFT)) nav_push(N_LEFT);
	if (pressed & BIT(J_RIGHT)) nav_push(N_RIGHT);
	if (pressed & (BIT(J_A) | BIT(J_START) | BIT(J_X))) nav_push(N_OK);
	if (pressed & (BIT(J_B) | BIT(J_Y))) nav_push(N_BACK);
	if (pressed & BIT(J_MENU)) {
		nav_push(N_MENU);
		want_menu = true;             /* a press, not a key still held from a menu */
	}
	uint32_t dirs = nb & (BIT(J_UP) | BIT(J_DOWN));
	if (pressed & dirs)
		repeat_at = t + 400;
	else if (dirs && (int32_t)(t - repeat_at) >= 0) {
		repeat_at = t + 110;
		nav_push(dirs & BIT(J_UP) ? N_UP : N_DOWN);
	}
	prev_nav_bits = nb;
	static int prev_btn;
	if ((mouse_btn & 1) && !(prev_btn & 1)) nav_push(N_OK);
	if ((mouse_btn & 2) && !(prev_btn & 2)) nav_push(N_BACK);
	prev_btn = mouse_btn;
}

/* Everything let go: a menu's last key does not go on into the game */
static void input_settle(void)
{
	nav_r = nav_w;
	want_menu = false;
	quick_save = quick_load = false;
	mouse_dx = mouse_dy = 0;
}

/* Sleep a little, reaping the children (we are init) */
static void idle(unsigned ms)
{
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
	usleep(ms * 1000);
}

/* ------------------------------------------------------------------------------------------ */
/* Menus                                                                                      */
/* ------------------------------------------------------------------------------------------ */
#define ROW 34

/* A list to choose from: its index, or -1 (back). overlay: a panel over the game */
static int menu(const char *title, const char *const *items, int n, int sel, const char *hint, bool overlay)
{
	int x = overlay ? 40 : 0, y = overlay ? 24 : 0, w = overlay ? 720 : LAND_W, h = overlay ? 432 : LAND_H;
	int rows = (h - 110) / ROW, top = 0;
	bool dirty = true;
	if (sel < 0 || sel >= n)
		sel = 0;
	input_settle();
	for (;;) {
		poll_input();
		enum nav v;
		while ((v = nav_pop()) != N_NONE) {
			dirty = true;
			if (v == N_UP) sel = sel > 0 ? sel - 1 : n - 1;
			else if (v == N_DOWN) sel = sel < n - 1 ? sel + 1 : 0;
			else if (v == N_LEFT) sel = sel - rows > 0 ? sel - rows : 0;
			else if (v == N_RIGHT) sel = sel + rows < n ? sel + rows : n - 1;
			else if (v == N_OK) { input_settle(); return sel; }
			else if (v == N_BACK || v == N_MENU) { input_settle(); return -1; }
		}
		want_menu = false;
		if (dirty) {
			dirty = false;
			if (sel < top) top = sel;
			if (sel >= top + rows) top = sel - rows + 1;
			fill(x, y, w, h, overlay ? C_PANEL : C_BG);
			if (overlay) {
				fill(x, y, w, 3, C_ACCENT);
				fill(x, y + h - 2, w, 2, C_LINE);
			}
			text(x + 24, y + 20, 3, C_ACCENT, title, (w - 48) / 24);
			fill(x + 24, y + 54, w - 48, 2, C_LINE);
			for (int r = 0; r < rows && top + r < n; r++) {
				int i = top + r, ry = y + 66 + r * ROW;
				bool on = i == sel;
				if (on)
					fill(x + 16, ry - 5, w - 32, ROW - 2, C_ACCENT);
				text(x + 28, ry, 3, on ? C_SEL_T : C_TEXT, items[i], (w - 60) / 24);
			}
			if (n > rows) {                   /* where in the list */
				int bar = (h - 130) * rows / n, by = y + 66 + (h - 130 - bar) * top / (n - rows > 0 ? n - rows : 1);
				fill(x + w - 10, y + 66, 4, h - 130, C_LINE);
				fill(x + w - 10, by, 4, bar < 12 ? 12 : bar, C_ACCENT);
			}
			if (hint)
				text(x + 24, y + h - 30, 2, C_DIM, hint, (w - 48) / 16);
			present(x, y, w, h);
		}
		if (test_mode)
			return sel;
		idle(10);
	}
}

/* A message until a key (or for ms milliseconds, if not 0) */
static void message(const char *title, const char *const *lines, int n, unsigned ms)
{
	int x = 100, y = 120, w = 600, h = 110 + n * 26;
	fill(x, y, w, h, C_PANEL);
	fill(x, y, w, 3, C_ACCENT);
	text(x + 24, y + 22, 3, C_ACCENT, title, 23);
	for (int i = 0; i < n; i++)
		text(x + 24, y + 70 + i * 26, 2, C_TEXT, lines[i], 34);
	present(x, y, w, h);
	if (test_mode)
		return;
	input_settle();
	uint32_t until = now_ms() + ms;
	for (;;) {
		poll_input();
		enum nav v = nav_pop();
		if (ms ? (int32_t)(now_ms() - until) >= 0 : (v == N_OK || v == N_BACK || v == N_MENU))
			break;
		idle(10);
	}
	input_settle();
}

static const char *HINT_LIST_PL = "F2/A: wybierz  F1/B: wstecz  GEN: menu";
static const char *HINT_LIST_EN = "F2/A: choose  F1/B: back  GEN: menu";

/* ------------------------------------------------------------------------------------------ */
/* Sound                                                                                      */
/* ------------------------------------------------------------------------------------------ */
static struct pcm *pcm;
static unsigned snd_rate = 48000;
static FILE *test_pcm;
static int16_t abuf[48000 / 30 * 2 * 4];
static size_t abuf_n;                 /* frames */
static unsigned xruns;

static void set_mixer(void)
{
	struct mixer *m = mixer_open(0);
	if (!m) {
		logmsg("sound: no mixer");
		return;
	}
	static const struct { const char *name, *text; int value; } s[] = {
		{"AIF1 DA0 Playback Volume", NULL, 161},
		{"AIF1 Slot 0 Digital DAC Playback Switch", NULL, 1},
		{"AIF1 DA0 Stereo Playback Route", "Mix Mono", 0},
		{"Headphone Source Playback Route", "DAC", 0},
		{"Headphone Playback Switch", NULL, 1},
		{"Headphone Playback Volume", NULL, -1},
	};
	for (size_t i = 0; i < sizeof s / sizeof s[0]; i++) {
		struct mixer_ctl *c = mixer_get_ctl_by_name(m, s[i].name);
		if (!c) {
			logmsg("sound: no control '%s'", s[i].name);
			continue;
		}
		int v = s[i].value >= 0 ? s[i].value : set_headphone < 0 ? 0 : set_headphone > 63 ? 63 : set_headphone;
		if (s[i].text)
			mixer_ctl_set_enum_by_string(c, s[i].text);
		else
			for (unsigned k = 0; k < mixer_ctl_get_num_values(c); k++)
				mixer_ctl_set_value(c, k, v);
	}
	mixer_close(m);
}

static void sound_open(void)
{
	if (test_mode) {
		const char *p = getenv("NES_TEST_PCM");
		if (p)
			test_pcm = fopen(p, "wb");
		return;
	}
	set_mixer();
	static const unsigned rates[] = {48000, 44100, 32000};
	for (size_t i = 0; i < 3 && !pcm; i++) {
		struct pcm_config cfg = {
			.channels = 2, .rate = rates[i], .period_size = 512, .period_count = 4,
			.format = PCM_FORMAT_S16_LE, .start_threshold = 1024,
		};
		struct pcm *p = pcm_open(0, 0, PCM_OUT, &cfg);
		if (p && pcm_is_ready(p)) {
			pcm = p;
			snd_rate = rates[i];
		} else if (p) {
			logmsg("sound: %u Hz: %s", rates[i], pcm_get_error(p));
			pcm_close(p);
		}
	}
	logmsg(pcm ? "sound: playing at %u Hz" : "sound: no output (%u)", snd_rate);
}

static void cb_audio_sample(int16_t l, int16_t r)
{
	if (abuf_n < sizeof abuf / sizeof abuf[0] / 2) {
		abuf[abuf_n * 2] = l;
		abuf[abuf_n * 2 + 1] = r;
		abuf_n++;
	}
}

static size_t cb_audio_batch(const int16_t *data, size_t frames)
{
	size_t room = sizeof abuf / sizeof abuf[0] / 2 - abuf_n, n = frames < room ? frames : room;
	memcpy(abuf + abuf_n * 2, data, n * 4);
	abuf_n += n;
	return frames;
}

/* The frame's sound out: one speaker, so both channels the same. The card's buffer filling up
 * keeps the game at its speed */
static bool sound_flush(void)
{
	for (size_t i = 0; i < abuf_n; i++) {
		int32_t v = ((int32_t)abuf[2 * i] + abuf[2 * i + 1]) / 2 * set_volume / 100;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		abuf[2 * i] = abuf[2 * i + 1] = (int16_t)v;
	}
	bool paced = false;
	if (test_pcm) {
		fwrite(abuf, 4, abuf_n, test_pcm);
	} else if (pcm && abuf_n) {
		if (pcm_writei(pcm, abuf, (unsigned)abuf_n) < 0) {
			xruns++;
			pcm_prepare(pcm);
		} else {
			paced = true;
		}
	}
	abuf_n = 0;
	return paced;
}

/* ------------------------------------------------------------------------------------------ */
/* The core                                                                                   */
/* ------------------------------------------------------------------------------------------ */
static const void *frame_data;
static unsigned frame_w = 256, frame_h = 240;
static size_t frame_pitch;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint16_t map_x[LAND_W], map_y[LAND_H];
static int gx0, gy0, gw, gh;                 /* the game's place on the landscape screen */
static unsigned mapped_w, mapped_h;
static double fps = 60.0988;

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data)
		return;                       /* the same picture again */
	frame_data = data;
	frame_w = w;
	frame_h = h;
	frame_pitch = pitch;
}

static void setup_maps(void)
{
	gh = LAND_H;
	gw = set_aspect ? LAND_H * 4 / 3 : (int)(frame_w * (LAND_H / frame_h ? LAND_H / frame_h : 1));
	if (gw > LAND_W)
		gw = LAND_W;
	if (!set_aspect && frame_h * 2 <= LAND_H)
		gh = (int)frame_h * 2;
	gx0 = (LAND_W - gw) / 2;
	gy0 = (LAND_H - gh) / 2;
	for (int x = 0; x < gw; x++)
		map_x[x] = (uint16_t)((unsigned)x * frame_w / (unsigned)gw);
	for (int y = 0; y < gh; y++)
		map_y[y] = (uint16_t)((unsigned)y * frame_h / (unsigned)gh);
	mapped_w = frame_w;
	mapped_h = frame_h;
	memset(canvas, 0, sizeof canvas);
	present(0, 0, LAND_W, LAND_H);
}

static inline uint32_t src_px(unsigned sx, unsigned sy)
{
	const uint8_t *row = (const uint8_t *)frame_data + sy * frame_pitch;
	if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888)
		return ((const uint32_t *)row)[sx] & 0xffffff;
	uint16_t v = ((const uint16_t *)row)[sx];
	if (pixfmt == RETRO_PIXEL_FORMAT_RGB565)
		return ((v & 0xf800u) << 8) | ((v & 0x07e0u) << 5) | ((v & 0x1fu) << 3);
	return ((v & 0x7c00u) << 9) | ((v & 0x03e0u) << 6) | ((v & 0x1fu) << 3);
}

/* The game's picture straight onto the screen */
static void draw_game(void)
{
	if (!frame_data)
		return;
	if (frame_w != mapped_w || frame_h != mapped_h)
		setup_maps();
	for (int x = 0; x < gw; x++) {
		unsigned sx = map_x[x];
		for (int y = 0; y < gh; y++) {
			unsigned fx, fy;
			land_to_fb((unsigned)(gx0 + x), (unsigned)(gy0 + y), &fx, &fy);
			put(fx, fy, src_px(sx, map_y[y]));
		}
	}
}

/* The core's settings: what we answer, the core's own default otherwise */
static bool get_var(struct retro_variable *v)
{
	static const struct { const char *key, *value; } vars[] = {
		{"fceumm_zapper_mode", "mouse"},          /* the Zapper: a mouse */
		{"fceumm_show_crosshair", "enabled"},
		{"fceumm_turbo_enable", "Both"},          /* X / Y: A and B fired again and again */
		{"fceumm_sndrate_hint", NULL},            /* filled in: the output's rate */
		{"fceumm_overscan_v_top", "8"},
		{"fceumm_overscan_v_bottom", "8"},
		{"fceumm_overscan_h_left", "0"},
		{"fceumm_overscan_h_right", "0"},
		{"fceumm_palette", "default"},
		{"fceumm_ntsc_filter", "disabled"},
	};
	static char rate[16];
	for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
		if (!strcmp(v->key, vars[i].key)) {
			if (vars[i].value) {
				v->value = vars[i].value;
			} else {
				snprintf(rate, sizeof rate, "%u", snd_rate);
				v->value = rate;
			}
			return true;
		}
	v->value = NULL;
	return false;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	if (!log_fp || level < RETRO_LOG_INFO)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	buf[strcspn(buf, "\n")] = 0;
	logmsg("core: %s", buf);
}

static bool cb_env(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
		if (f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565 || f == RETRO_PIXEL_FORMAT_0RGB1555) {
			pixfmt = f;
			return true;
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = BIOS;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = SAVES;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		return get_var((struct retro_variable *)data);
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE:
		*(unsigned *)data = snd_rate;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;            /* the plain variables */
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		mapped_w = 0;                     /* the picture's size again at the next frame */
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message *m = data;
		logmsg("core message: %s", m->msg);
		return true;
	}
	default:
		return false;
	}
}

static void cb_input_poll(void)
{
	frame_mdx = mouse_dx;
	frame_mdy = mouse_dy;
	mouse_dx = mouse_dy = 0;
}

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port > 1)
		return 0;
	switch (device & RETRO_DEVICE_MASK) {
	case RETRO_DEVICE_JOYPAD:
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return (int16_t)frame_bits[port];
		return (int16_t)((frame_bits[port] >> id) & 1);
	case RETRO_DEVICE_MOUSE:          /* the Zapper: any mouse, on either port */
		if (id == RETRO_DEVICE_ID_MOUSE_X) return (int16_t)frame_mdx;
		if (id == RETRO_DEVICE_ID_MOUSE_Y) return (int16_t)frame_mdy;
		if (id == RETRO_DEVICE_ID_MOUSE_LEFT) return mouse_btn & 1;
		if (id == RETRO_DEVICE_ID_MOUSE_RIGHT) return (mouse_btn >> 1) & 1;
		return 0;
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* Games and saves                                                                            */
/* ------------------------------------------------------------------------------------------ */
static char *games[MAX_GAMES];
static char *game_names[MAX_GAMES];
static int n_games;
static char cur_base[300];             /* the running game's name without its extension */
static size_t sram_size;
static uint32_t sram_check;

static bool is_game(const char *n)
{
	const char *dot = strrchr(n, '.');
	if (!dot || n[0] == '.')
		return false;
	return !strcasecmp(dot, ".nes") || !strcasecmp(dot, ".fds") || !strcasecmp(dot, ".unf") ||
	       !strcasecmp(dot, ".unif") || !strcasecmp(dot, ".nsf");
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *dir, const char *prefix, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && n_games < MAX_GAMES) {
		char path[600];
		struct stat st;
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2 && strcasecmp(e->d_name, "SAVES") && strcasecmp(e->d_name, "BIOS")) {
				char pre[300];
				snprintf(pre, sizeof pre, "%s%s/", prefix, e->d_name);
				scan_dir(path, pre, depth + 1);
			}
		} else if (is_game(e->d_name)) {
			char shown[400];
			snprintf(shown, sizeof shown, "%s%s", prefix, e->d_name);
			char *dot = strrchr(shown, '.');
			if (dot)
				*dot = 0;
			/* "name\npath": sorted by the name shown */
			size_t n = strlen(shown) + strlen(path) + 2;
			char *s = malloc(n);
			snprintf(s, n, "%s\n%s", shown, path);
			games[n_games++] = s;
		}
	}
	if (d)
		closedir(d);
}

static void scan_games(void)
{
	for (int i = 0; i < n_games; i++)
		free(games[i]);
	n_games = 0;
	mkdir(ROMS, 0755);
	mkdir(SAVES, 0755);
	mkdir(BIOS, 0755);
	scan_dir(ROMS, "", 0);
	qsort(games, (size_t)n_games, sizeof games[0], by_name);
	for (int i = 0; i < n_games; i++) {
		char *nl = strchr(games[i], '\n');
		*nl = 0;                          /* games[i]: the name, then the path after it */
		game_names[i] = games[i];
	}
	logmsg("games: %d in %s", n_games, ROMS);
}

static const char *game_path(int i)
{
	return games[i] + strlen(games[i]) + 1;
}

static uint32_t checksum(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void save_path(char *out, size_t n, const char *ext)
{
	snprintf(out, n, "%s/%s.%s", SAVES, cur_base, ext);
}

static void load_sram(void)
{
	sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size || !p)
		return;
	char path[700];
	save_path(path, sizeof path, "srm");
	FILE *f = fopen(path, "rb");
	if (f) {
		size_t r = fread(p, 1, sram_size, f);
		fclose(f);
		logmsg("battery save %s: %zu bytes", path, r);
	}
	sram_check = checksum(p, sram_size);
}

/* The battery RAM to its file, when it changed */
static void save_sram(void)
{
	if (!cur_base[0] || !sram_size)
		return;
	uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!p)
		return;
	uint32_t c = checksum(p, sram_size);
	if (c == sram_check)
		return;
	char path[700], tmp[710];
	save_path(path, sizeof path, "srm");
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		logmsg("battery save %s: %s", path, strerror(errno));
		return;
	}
	fwrite(p, 1, sram_size, f);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, path);
	sync();
	sram_check = c;
	logmsg("battery save written: %s", path);
}

static bool save_state(int slot)
{
	size_t n = retro_serialize_size();
	void *buf = n ? malloc(n) : NULL;
	if (!buf || !retro_serialize(buf, n)) {
		free(buf);
		return false;
	}
	char ext[16], path[700];
	snprintf(ext, sizeof ext, "state%d", slot);
	save_path(path, sizeof path, ext);
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(buf, 1, n, f) == n;
	if (f) {
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
	free(buf);
	sync();
	logmsg("state %d saved: %s", slot, ok ? "ok" : strerror(errno));
	return ok;
}

static bool load_state(int slot)
{
	char ext[16], path[700];
	snprintf(ext, sizeof ext, "state%d", slot);
	save_path(path, sizeof path, ext);
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	size_t n = retro_serialize_size();
	void *buf = malloc(n ? n : 1);
	size_t r = fread(buf, 1, n, f);
	fclose(f);
	bool ok = r == n && retro_unserialize(buf, n);
	free(buf);
	return ok;
}

/* ------------------------------------------------------------------------------------------ */
/* Bluetooth: the lending system's BlueZ in a chroot (as Doom's), pads paired from here kept   */
/* ------------------------------------------------------------------------------------------ */
static bool bt_started;

static const char bt_script[] =
	"exec >/tmp/nes-bt.log 2>&1\n"
	"set -x\n"
	"export PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
	"mkdir -p /run/dbus /var/lock/subsys /tmp/dbus /var/run\n"
	"if [ -x /sbin/udevd ]; then\n"
	"  mkdir -p /run/udev\n"
	"  /sbin/udevd -d\n"
	"  udevadm trigger --type=subsystems --action=add\n"
	"  udevadm trigger --type=devices --action=add\n"
	"  udevadm settle --timeout=30\n"
	"fi\n"
	"modprobe btusb 2>/dev/null; modprobe hidp 2>/dev/null; modprobe uhid 2>/dev/null\n"
	"for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
	"  [ -d /sys/class/bluetooth/hci0 ] && break\n"
	"  sleep 1\n"
	"done\n"
	"[ -x /etc/init.d/S39btpairings ] && /etc/init.d/S39btpairings start >/dev/null 2>&1\n"
	/* the pads paired in the emulator: kept on its own root filesystem */
	"if [ -f /nes-bt/pairings.tar ]; then mkdir -p /var/lib/bluetooth; tar -C /var/lib/bluetooth -xf /nes-bt/pairings.tar; fi\n"
	"dbus-uuidgen --ensure 2>/dev/null\n"
	"dbus-daemon --system\n"
	"sleep 1\n"
	"/usr/libexec/bluetooth/bluetoothd -n >/tmp/bluetoothd.log 2>&1 &\n"
	"sleep 3\n"
	"bluetoothctl power on\n"
	"touch /tmp/nes-bt-ready\n"
	"paired() { { bluetoothctl devices Paired 2>/dev/null; bluetoothctl paired-devices 2>/dev/null; } |"
	" awk '/^Device /{print $2}' | sort -u; }\n"
	"paired\n"
	"i=0\n"
	"while [ $i -lt 14 ]; do\n"
	"  [ -f /tmp/nes-bt-busy ] || for mac in $(paired); do\n"
	"    bluetoothctl info $mac | grep -q 'Connected: yes' && continue\n"
	"    bluetoothctl --timeout 6 connect $mac\n"
	"  done\n"
	"  sleep 4\n"
	"  i=$((i+1))\n"
	"done\n"
	"wait\n";

/* Looking around: the devices seen, "MAC|name|icon|paired" one per line */
static const char bt_scan_script[] =
	"exec >/tmp/nes-scan.log 2>&1\n"
	"export PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
	"touch /tmp/nes-bt-busy\n"
	"bluetoothctl --timeout 15 scan on\n"
	"for mac in $(bluetoothctl devices | awk '/^Device /{print $2}'); do\n"
	"  info=$(bluetoothctl info $mac)\n"
	"  name=$(echo \"$info\" | sed -n 's/^[[:space:]]*Name: //p' | head -1 | tr '|' ' ')\n"
	"  icon=$(echo \"$info\" | sed -n 's/^[[:space:]]*Icon: //p' | head -1)\n"
	"  paired=$(echo \"$info\" | sed -n 's/^[[:space:]]*Paired: //p' | head -1)\n"
	"  echo \"$mac|$name|$icon|$paired\"\n"
	"done > /tmp/nes-scan.txt\n"
	"rm -f /tmp/nes-bt-busy\n";

/* Pairing one ($1): BlueZ's own agent with no keys to type (pads ask for none), trusted, connected,
 * and the pairings kept */
static const char bt_pair_script[] =
	"exec >/tmp/nes-pair.log 2>&1\n"
	"export PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
	"mac=$1\n"
	"touch /tmp/nes-bt-busy\n"
	"rm -f /tmp/nes-pair-result\n"
	"{ echo 'agent NoInputNoOutput'; sleep 1; echo 'default-agent'; sleep 1; echo 'scan on'; sleep 4;\n"
	"  echo \"pair $mac\"; sleep 12; echo \"trust $mac\"; sleep 1; echo \"connect $mac\"; sleep 6;\n"
	"  echo 'scan off'; sleep 1; echo quit; } | bluetoothctl\n"
	"if bluetoothctl info $mac | grep -q 'Paired: yes'; then\n"
	"  bluetoothctl trust $mac\n"
	"  mkdir -p /nes-bt && tar -C /var/lib/bluetooth -cf /nes-bt/pairings.tar . && sync\n"
	"  echo ok > /tmp/nes-pair-result\n"
	"else\n"
	"  echo no > /tmp/nes-pair-result\n"
	"fi\n"
	"rm -f /tmp/nes-bt-busy\n";

/* A shell script in the lending system's chroot; its pid */
static pid_t run_chroot(const char *script, const char *name, const char *arg)
{
	char path[200];
	snprintf(path, sizeof path, "/bt/tmp/%s", name);
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(script, f);
	fclose(f);
	pid_t pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		char inner[200];
		snprintf(inner, sizeof inner, "/tmp/%s", name);
		if (chroot("/bt") == 0 && chdir("/") == 0)
			execl("/bin/sh", "sh", inner, arg, (char *)NULL);
		_exit(1);
	}
	return pid;
}

/* ------------------------------------------------------------------------------------------ */
/* The system: modules, the WiFi/BT chip's USB port, Bluetooth (as Doom's: extras/doom)        */
/* ------------------------------------------------------------------------------------------ */
static char mod_dir[300];

static void load_module(const char *rel)
{
	char path[700];
	snprintf(path, sizeof path, "%s/%s", mod_dir, rel);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("module %s: missing (%s)", rel, strerror(errno));
		return;
	}
	if (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST)
		logmsg("module %s: %s", rel, strerror(errno));
	else
		logmsg("module %s: loaded", rel);
	close(fd);
}

static void load_with_deps(const char *suffix)
{
	char depfile[400], line[2048];
	snprintf(depfile, sizeof depfile, "%s/modules.dep", mod_dir);
	FILE *f = fopen(depfile, "r");
	bool found = false;
	while (f && fgets(line, sizeof line, f)) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = '\0';
		size_t ln = strlen(line), ls = strlen(suffix);
		if (ln < ls || strcmp(line + ln - ls, suffix))
			continue;
		char *deps[16];
		int n = 0;
		for (char *t = strtok(colon + 1, " \n"); t && n < 16; t = strtok(NULL, " \n"))
			deps[n++] = t;
		for (int i = n - 1; i >= 0; i--)
			load_module(deps[i]);
		load_module(line);
		found = true;
		break;
	}
	if (f)
		fclose(f);
	if (!found)
		logmsg("module *%s: not in this kernel", suffix);
}

static bool mount_donor(void)
{
	const char *dev = getenv("doom_modules");     /* the extras' kernel's system (image.py) */
	struct utsname u;
	if (!dev || uname(&u) != 0) {
		logmsg("no doom_modules= on the command line: no USB HOST, no Bluetooth");
		return false;
	}
	mkdir("/donor", 0755);
	if (mount(dev, "/donor", "ext4", MS_RDONLY, NULL) != 0) {
		logmsg("donor %s: mount failed: %s", dev, strerror(errno));
		return false;
	}
	snprintf(mod_dir, sizeof mod_dir, "/donor/lib/modules/%s", u.release);
	logmsg("donor %s mounted; modules %s: %s", dev, mod_dir, access(mod_dir, R_OK) == 0 ? "there" : "MISSING");
	return true;
}

static bool musb_has_device(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	bool found = false;
	while (d && (e = readdir(d)))
		if (!strncmp(e->d_name, "3-", 2) && !strchr(e->d_name, ':'))
			found = true;
	if (d)
		closedir(d);
	return found;
}

static void write_sysfs(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fputs(val, f);
	fclose(f);
}

/* The WiFi/BT chip's power: gpiochip0 line 5, active low (off, then on, as the radio's software) */
static int wifi_line_fd = -1;

static void wifi_chip_on(void)
{
	int chip = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
	if (chip < 0)
		return;
	struct gpiohandle_request req;
	memset(&req, 0, sizeof req);
	req.lineoffsets[0] = 5;
	req.flags = GPIOHANDLE_REQUEST_OUTPUT;
	req.default_values[0] = 1;
	req.lines = 1;
	snprintf(req.consumer_label, sizeof req.consumer_label, "nes_wifi");
	if (ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &req) >= 0) {
		wifi_line_fd = req.fd;
		usleep(500000);
		struct gpiohandle_data on = {.values = {0}};
		ioctl(wifi_line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &on);
		logmsg("WiFi/BT chip restarted");
	}
	close(chip);
}

static void *musb_watch(void *arg)
{
	(void)arg;
	static const char *mode = "/sys/devices/platform/soc/1c19000.usb/musb-hdrc.2.auto/mode";
	static const char *drv = "/sys/bus/platform/drivers/musb-sunxi";
	sleep(6);
	if (musb_has_device())
		return NULL;
	write_sysfs(mode, "host");
	sleep(4);
	if (musb_has_device())
		return NULL;
	char p[200];
	snprintf(p, sizeof p, "%s/unbind", drv);
	write_sysfs(p, "1c19000.usb");
	sleep(1);
	snprintf(p, sizeof p, "%s/bind", drv);
	write_sysfs(p, "1c19000.usb");
	return NULL;
}

static void start_bluetooth(void)
{
	if (!set_bluetooth) {
		logmsg("Bluetooth: off in nes.txt");
		return;
	}
	FILE *fw = fopen("/sys/module/firmware_class/parameters/path", "w");
	if (fw) {
		fputs("/donor/lib/firmware", fw);
		fclose(fw);
	}
	load_with_deps("/fs/overlayfs/overlay.ko");
	mkdir("/ovl", 0755);
	mkdir("/bt", 0755);
	if (mount("tmpfs", "/ovl", "tmpfs", 0, "size=16m") != 0)
		return;
	mkdir("/ovl/up", 0755);
	mkdir("/ovl/work", 0755);
	if (mount("overlay", "/bt", "overlay", 0, "lowerdir=/donor,upperdir=/ovl/up,workdir=/ovl/work") != 0) {
		logmsg("Bluetooth: overlay: %s", strerror(errno));
		return;
	}
	mount("/proc", "/bt/proc", NULL, MS_BIND, NULL);
	mount("/sys", "/bt/sys", NULL, MS_BIND, NULL);
	mount("/dev", "/bt/dev", NULL, MS_BIND, NULL);
	mount("tmpfs", "/bt/run", "tmpfs", 0, "size=4m");
	mount("tmpfs", "/bt/tmp", "tmpfs", 0, "size=8m");
	mount("/dev/mmcblk0p3", "/bt/mnt", "vfat", MS_RDONLY, NULL);
	mkdir("/nes/bt", 0700);
	mkdir("/bt/nes-bt", 0700);
	mount("/nes/bt", "/bt/nes-bt", NULL, MS_BIND, NULL);
	if (run_chroot(bt_script, "nes-bt.sh", NULL) > 0) {
		bt_started = true;
		logmsg("Bluetooth: starting the donor's BlueZ");
	}
}

/* ------------------------------------------------------------------------------------------ */
/* The emulator's screens                                                                     */
/* ------------------------------------------------------------------------------------------ */
/* Pairing a Bluetooth pad: a look around, a choice, the pairing */
static void pair_pad(bool overlay)
{
	(void)overlay;
	if (!bt_started || access("/bt/tmp/nes-bt-ready", F_OK) != 0) {
		const char *l[] = {T("Bluetooth nie dziala.", "Bluetooth is not running."),
		                   T("Szczegoly: nes-log.txt na karcie.", "Details: nes-log.txt on the card."),
		                   T("bluetooth=1 w nes.txt?", "bluetooth=1 in nes.txt?")};
		message("Bluetooth", l, 3, 0);
		return;
	}
	pid_t pid = run_chroot(bt_scan_script, "nes-scan.sh", NULL);
	uint32_t start = now_ms();
	while (pid > 0 && waitpid(pid, NULL, WNOHANG) == 0) {
		char sec[64];
		snprintf(sec, sizeof sec, T("Szukam... %u s", "Searching... %u s"), (now_ms() - start) / 1000);
		const char *l[] = {T("Wlacz na padzie tryb parowania", "Put the pad in pairing mode"),
		                   T("(zwykle przytrzymaj przycisk", "(usually hold its pairing"),
		                   T("parowania kilka sekund).", "button a few seconds)."), "", sec};
		int x = 100, y = 120, w = 600, h = 240;
		fill(x, y, w, h, C_PANEL);
		fill(x, y, w, 3, C_ACCENT);
		text(x + 24, y + 22, 3, C_ACCENT, T("Parowanie pada", "Pairing a pad"), 23);
		for (int i = 0; i < 5; i++)
			text(x + 24, y + 70 + i * 26, 2, i == 4 ? C_WARN : C_TEXT, l[i], 34);
		present(x, y, w, h);
		poll_input();
		usleep(200000);
	}
	/* what was seen: input devices first */
	char macs[40][18], labels[40][80];
	const char *items[41];
	int n = 0;
	FILE *f = fopen("/bt/tmp/nes-scan.txt", "r");
	char line[300];
	for (int pass = 0; pass < 2; pass++) {
		if (f)
			rewind(f);
		while (f && n < 40 && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = 0;
			char *mac = strtok(line, "|"), *name = strtok(NULL, "|"), *icon = strtok(NULL, "|"),
			     *paired = strtok(NULL, "|");
			if (!mac || strlen(mac) != 17)
				continue;
			bool input = icon && !strncmp(icon, "input-", 6);
			if (input != (pass == 0))
				continue;
			snprintf(macs[n], sizeof macs[n], "%s", mac);
			snprintf(labels[n], sizeof labels[n], "%s%s%s", name && *name ? name : mac,
			         input ? (strstr(icon, "gaming") ? T(" (pad)", " (pad)") : "") : "",
			         paired && !strcmp(paired, "yes") ? T(" *sparowany", " *paired") : "");
			items[n] = labels[n];
			n++;
		}
	}
	if (f)
		fclose(f);
	if (!n) {
		const char *l[] = {T("Nie znaleziono urzadzen.", "No device found."),
		                   T("Wlacz parowanie na padzie", "Put the pad in pairing mode"),
		                   T("i sprobuj jeszcze raz.", "and try again.")};
		message("Bluetooth", l, 3, 0);
		return;
	}
	int c = menu(T("Wybierz pad", "Choose the pad"), items, n, 0, set_en ? HINT_LIST_EN : HINT_LIST_PL, false);
	if (c < 0)
		return;
	unlink("/bt/tmp/nes-pair-result");
	pid = run_chroot(bt_pair_script, "nes-pair.sh", macs[c]);
	start = now_ms();
	while (pid > 0 && waitpid(pid, NULL, WNOHANG) == 0) {
		char sec[64];
		snprintf(sec, sizeof sec, T("Paruje... %u s", "Pairing... %u s"), (now_ms() - start) / 1000);
		const char *l[] = {labels[c], "", sec};
		int x = 100, y = 120, w = 600, h = 190;
		fill(x, y, w, h, C_PANEL);
		fill(x, y, w, 3, C_ACCENT);
		text(x + 24, y + 22, 3, C_ACCENT, T("Parowanie pada", "Pairing a pad"), 23);
		for (int i = 0; i < 3; i++)
			text(x + 24, y + 70 + i * 26, 2, i == 2 ? C_WARN : C_TEXT, l[i], 34);
		present(x, y, w, h);
		poll_input();
		usleep(200000);
	}
	char res[8] = "";
	f = fopen("/bt/tmp/nes-pair-result", "r");
	if (f) {
		if (!fgets(res, sizeof res, f))
			res[0] = 0;
		fclose(f);
	}
	log_file("pairing", "/bt/tmp/nes-pair.log", 6000);
	bool ok = !strncmp(res, "ok", 2);
	const char *ok_l[] = {labels[c], T("Sparowany: laczy sie sam", "Paired: it connects by itself"),
	                      T("po wlaczeniu emulatora.", "when the emulator starts.")};
	const char *no_l[] = {labels[c], T("Nie udalo sie sparowac.", "Pairing failed."),
	                      T("Sprobuj jeszcze raz.", "Try again.")};
	message(ok ? T("Gotowe", "Done") : "Bluetooth", ok ? ok_l : no_l, 3, 0);
}

/* A pad's buttons, one by one */
static void learn_snapshot(void)
{
	for (int i = 0; i < n_devs; i++)
		memcpy(learn_base[i], devs[i].abs, sizeof learn_base[i]);
}

static void learn_pad(void)
{
	static const int order[] = {J_UP, J_DOWN, J_LEFT, J_RIGHT, J_A, J_B, J_SELECT, J_START, J_X, J_Y};
	const char *names_pl[] = {"GORA", "DOL", "LEWO", "PRAWO", "A", "B", "SELECT", "START",
	                          "A TURBO (lub GEN: pomin)", "B TURBO (lub GEN: pomin)"};
	const char *names_en[] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "SELECT", "START",
	                          "TURBO A (or GEN: skip)", "TURBO B (or GEN: skip)"};
	bool any_pad = false;
	for (int i = 0; i < n_devs; i++)
		any_pad |= devs[i].role == ROLE_PAD;
	if (!any_pad) {
		const char *l[] = {T("Nie widze zadnego pada.", "No pad found."),
		                   T("Podlacz pad USB albo sparuj", "Plug in a USB pad or pair"),
		                   T("pad Bluetooth.", "a Bluetooth pad.")};
		message(T("Przyciski pada", "Pad buttons"), l, 3, 0);
		return;
	}
	struct padmap m;
	memset(&m, 0, sizeof m);
	learn_dev = -2;
	learn_from = -1;
	int dev = -1;
	for (size_t k = 0; k < sizeof order / sizeof order[0]; k++) {
		learn_got.kind = 0;
		bool skipped = false;
		int x = 100, y = 120, w = 600, h = 200;
		fill(x, y, w, h, C_PANEL);
		fill(x, y, w, 3, C_ACCENT);
		text(x + 24, y + 22, 3, C_ACCENT, T("Przyciski pada", "Pad buttons"), 23);
		text(x + 24, y + 74, 2, C_TEXT, T("Nacisnij na padzie:", "Press on the pad:"), 34);
		text(x + 24, y + 110, 3, C_WARN, set_en ? names_en[k] : names_pl[k], 23);
		text(x + 24, y + 160, 2, C_DIM, T("GEN na radiu: przerwij", "GEN on the radio: stop"), 34);
		present(x, y, w, h);
		input_settle();
		learn_snapshot();
		uint32_t prev_radio = radio_bits;
		for (;;) {
			poll_input();
			if ((radio_bits & BIT(J_MENU)) && !(prev_radio & BIT(J_MENU))) {
				if (k >= 8) { skipped = true; break; }
				learn_dev = -1;
				input_settle();
				return;
			}
			prev_radio = radio_bits;
			if (learn_got.kind && learn_from >= 0)
				break;
			idle(10);
		}
		if (skipped) {
			while (radio_bits & BIT(J_MENU)) { poll_input(); idle(10); }
			continue;
		}
		if (dev < 0) {
			dev = learn_from;
			learn_dev = dev;          /* the rest from this pad only */
		}
		m.b[order[k]][0] = learn_got;
		/* let go first */
		uint32_t t = now_ms();
		while (now_ms() - t < 3000) {
			poll_input();
			bool held = false;
			if (dev >= 0 && dev < n_devs) {
				const struct dev *d = &devs[dev];
				held = learn_got.kind == 1 ? TEST_BIT(d->keys, learn_got.code)
				                           : axis_on(d, learn_got.code, learn_got.sign);
			}
			if (!held)
				break;
			idle(10);
		}
		learn_got.kind = 0;
		learn_from = -1;
	}
	learn_dev = -1;
	if (dev >= 0 && dev < n_devs) {
		bind_key(&m, J_MENU, BTN_MODE);
		devs[dev].map = m;
		save_map(&devs[dev]);
		logmsg("pad \"%s\": buttons set", devs[dev].name);
		const char *l[] = {devs[dev].name, T("Zapisane w NES/pads.txt.", "Saved in NES/pads.txt.")};
		message(T("Gotowe", "Done"), l, 2, 2500);
	}
	input_settle();
}

/* What the in-game menu can do */
enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_PAD, M_PAIR, M_LIST, M_REBOOT, M_OFF };

static int game_menu(void)
{
	const char *items[] = {
		T("Wroc do gry", "Back to the game"), T("Zapisz stan", "Save state"), T("Wczytaj stan", "Load state"),
		T("Reset gry", "Reset the game"), T("Przyciski pada", "Pad buttons"),
		T("Paruj pad Bluetooth", "Pair a Bluetooth pad"), T("Lista gier", "Game list"),
		T("Restart do menu startowego", "Restart to the boot menu"), T("Wylacz radio", "Switch the radio off"),
	};
	int c = menu(cur_base, items, 9, 0, T("F2/A: wybierz  GEN: do gry", "F2/A: choose  GEN: back to the game"), true);
	return c < 0 ? M_RESUME : c;
}

static void osd(const char *msg)
{
	int w = text_len(msg) * 16 + 32;
	fill(0, 0, w, 40, C_PANEL);
	text(16, 12, 2, C_ACCENT, msg, 48);
	present(0, 0, w, 40);
}

/* One game, until the list or leaving is asked for */
static void play(int idx)
{
	const char *path = game_path(idx);
	snprintf(cur_base, sizeof cur_base, "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	char *dot = strrchr(cur_base, '.');
	if (dot)
		*dot = 0;
	logmsg("game: %s", path);
	FILE *f = fopen(path, "rb");
	struct stat st;
	void *data = NULL;
	size_t size = 0;
	if (f && fstat(fileno(f), &st) == 0 && st.st_size > 0 && st.st_size < 16 * 1024 * 1024) {
		size = (size_t)st.st_size;
		data = malloc(size);
		if (data && fread(data, 1, size, f) != size) {
			free(data);
			data = NULL;
		}
	}
	if (f)
		fclose(f);
	struct retro_game_info gi = {path, data, size, NULL};
	if (!data || !retro_load_game(&gi)) {
		free(data);
		logmsg("game: did not load");
		const char *l[] = {cur_base, T("Tej gry nie da sie uruchomic", "This game does not start"),
		                   T("(uszkodzony plik albo FDS bez", "(a damaged file, or FDS without"),
		                   T("NES/BIOS/disksys.rom).", "NES/BIOS/disksys.rom).")};
		message(T("Blad", "Error"), l, 4, 0);
		cur_base[0] = 0;
		return;
	}
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	fps = av.timing.fps > 1 ? av.timing.fps : 60.0988;
	logmsg("game: %ux%u, %.3f frames/s, sound %.0f Hz", av.geometry.base_width, av.geometry.base_height,
	       fps, av.timing.sample_rate);
	retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);   /* "auto": the core's own choice */
	retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	load_sram();
	frame_data = NULL;
	mapped_w = 0;
	memset(canvas, 0, sizeof canvas);
	present(0, 0, LAND_W, LAND_H);
	input_settle();
	uint32_t last_save = now_ms(), osd_until = 0, fps_t = now_ms();
	const char *osd_msg = NULL;
	unsigned frames = 0, fps_frames = 0;
	int test_frames = test_mode ? (getenv("NES_TEST_FRAMES") ? atoi(getenv("NES_TEST_FRAMES")) : 300) : 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	bool done = false;
	while (!done) {
		poll_input();
		if (want_menu || quick_save || quick_load) {
			int c = M_RESUME;
			if (quick_save) c = M_SAVE;
			else if (quick_load) c = M_LOAD;
			else c = game_menu();
			input_settle();
			const char *msg = NULL;
			switch (c) {
			case M_SAVE: msg = save_state(0) ? T("Stan zapisany", "State saved") : T("Nie zapisano", "Not saved"); break;
			case M_LOAD: msg = load_state(0) ? T("Stan wczytany", "State loaded") : T("Brak zapisu", "No saved state"); break;
			case M_RESET: retro_reset(); break;
			case M_PAD: learn_pad(); break;
			case M_PAIR: pair_pad(true); break;
			case M_LIST: done = true; break;
			case M_REBOOT: leave(RB_AUTOBOOT); break;
			case M_OFF: leave(RB_POWER_OFF); break;
			}
			memset(canvas, 0, sizeof canvas);
			present(0, 0, LAND_W, LAND_H);
			if (msg) {
				osd_msg = msg;
				osd_until = now_ms() + 2000;
			}
			if (pcm)
				pcm_prepare(pcm);
			clock_gettime(CLOCK_MONOTONIC, &next);
			if (done)
				break;
		}
		retro_run();
		draw_game();
		if (osd_until) {                  /* a word for 2 s, top left */
			if ((int32_t)(now_ms() - osd_until) < 0) {
				osd(osd_msg);
			} else {
				osd_until = 0;
				fill(0, 0, 400, 40, 0);
				present(0, 0, 400, 40);
			}
		}
		bool paced = sound_flush();
		frames++;
		if (!paced) {                     /* no sound to keep time: the clock */
			long step = (long)(1e9 / fps);
			next.tv_nsec += step;
			while (next.tv_nsec >= 1000000000L) {
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			if (!test_mode)
				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		}
		uint32_t t = now_ms();
		if (t - last_save > 30000) {
			last_save = t;
			save_sram();
		}
		if (t - fps_t >= 10000) {
			logmsg("%.1f frames/s, sound gaps so far: %u", (frames - fps_frames) * 1000.0 / (t - fps_t), xruns);
			fps_t = t;
			fps_frames = frames;
		}
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		if (test_mode && (int)frames >= test_frames)
			break;
	}
	save_sram();
	retro_unload_game();
	free(data);
	cur_base[0] = 0;
	sram_size = 0;
	memset(canvas, 0, sizeof canvas);
}

/* The list of games, and what else there is to do */
static void game_list(void)
{
	int sel = 0;
	for (;;) {
		scan_games();
		enum { X_PAIR, X_PAD, X_REBOOT, X_OFF, X_N };
		const char *extra[X_N] = {
			T("> Paruj pad Bluetooth", "> Pair a Bluetooth pad"), T("> Przyciski pada", "> Pad buttons"),
			T("> Restart do menu startowego", "> Restart to the boot menu"), T("> Wylacz radio", "> Switch the radio off"),
		};
		static const char *items[MAX_GAMES + X_N + 1];
		int n = 0;
		if (!n_games)
			items[n++] = T("(brak gier: wgraj pliki .nes", "(no games: put .nes files");
		for (int i = 0; i < n_games; i++)
			items[n++] = game_names[i];
		for (int i = 0; i < X_N; i++)
			items[n++] = extra[i];
		int base = n_games ? n_games : 1;
		char title[64];
		snprintf(title, sizeof title, T("Gry NES (%d)", "NES games (%d)"), n_games);
		const char *hint = n_games ? (set_en ? HINT_LIST_EN : HINT_LIST_PL)
		                           : T("Gry: folder NES na partycji BOOT karty", "Games: the NES folder on the card's BOOT");
		int c = menu(title, items, n, sel, hint, false);
		if (c < 0)
			continue;
		sel = c;
		if (c < n_games) {
			play(c);
			continue;
		}
		if (c < base) {
			const char *l[] = {T("Skopiuj gry (.nes, .fds, .unf)", "Copy games (.nes, .fds, .unf)"),
			                   T("do folderu NES na partycji", "to the NES folder on the card's"),
			                   T("BOOT karty (widac ja na", "BOOT partition (any computer"),
			                   T("kazdym komputerze), albo dodaj", "sees it), or add them in"),
			                   T("je w aplikacji przy karcie.", "the app with the card.")};
			message(T("Brak gier", "No games"), l, 5, 0);
			continue;
		}
		switch (c - base) {
		case X_PAIR: pair_pad(false); break;
		case X_PAD: learn_pad(); break;
		case X_REBOOT: leave(RB_AUTOBOOT); break;
		case X_OFF: leave(RB_POWER_OFF); break;
		}
	}
}

/* ------------------------------------------------------------------------------------------ */
/* Start                                                                                      */
/* ------------------------------------------------------------------------------------------ */
static void read_settings(void)
{
	mkdir("/boot", 0755);
	if (mount("/dev/mmcblk0p1", "/boot", "vfat", MS_NOATIME, "utf8") == 0 ||
	    mount("/dev/mmcblk0p1", "/boot", "vfat", MS_NOATIME, NULL) == 0) {
		boot_rw = true;
		log_fp = fopen("/boot/nes-log.txt", "w");
		if (log_fp)
			dup2(fileno(log_fp), 2);
	}
	struct utsname u;
	if (uname(&u) == 0)
		logmsg("NES emulator for the X6100, kernel %s", u.release);
	FILE *f = fopen("/boot/nes.txt", "r");
	char line[128];
	while (f && fgets(line, sizeof line, f)) {
		int v;
		char s[16];
		if (sscanf(line, " volume = %d", &v) == 1 && v >= 0 && v <= 200) set_volume = v;
		else if (sscanf(line, " rxvol = %d", &v) == 1) set_rxvol = v;
		else if (sscanf(line, " rxmute = %d", &v) == 1) set_rxmute = v != 0;
		else if (sscanf(line, " headphone = %d", &v) == 1) set_headphone = v;
		else if (sscanf(line, " record = %d", &v) == 1) set_record = v != 0;
		else if (sscanf(line, " bluetooth = %d", &v) == 1) set_bluetooth = v != 0;
		else if (sscanf(line, " aspect = %15s", s) == 1) set_aspect = !strcmp(s, "4:3");
		else if (sscanf(line, " lang = %15s", s) == 1) set_en = !strcmp(s, "en");
	}
	if (f)
		fclose(f);
	logmsg("settings: volume=%d rxvol=%d bluetooth=%d aspect=%s lang=%s", set_volume, set_rxvol,
	       set_bluetooth, set_aspect ? "4:3" : "2x", set_en ? "en" : "pl");
}

static void as_init(void)
{
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("", "/", "", MS_REMOUNT, NULL);
	setenv("HOME", "/nes", 1);
	read_settings();
	radio_audio();
	if (mount_donor()) {
		/* pads on USB: the kernel's HID drivers that are modules */
		wifi_chip_on();
		load_with_deps("/usb/musb/sunxi.ko");
		load_with_deps("/drivers/hid/uhid.ko");
		load_with_deps("/drivers/input/joydev.ko");
		pthread_t t;
		pthread_create(&t, NULL, musb_watch, NULL);
		pthread_detach(t);
		start_bluetooth();
	}
}

static bool test_fb(void)
{
	const char *t = getenv("NES_TEST_FB");
	if (!t || sscanf(t, "%ux%u", &fb_w, &fb_h) != 2)
		return false;
	fb_bpp = 4;
	fb_stride = fb_w * 4;
	fb_native = true;
	fb = calloc(fb_stride, fb_h);
	test_mode = true;
	return true;
}

static void screen_init(void)
{
	if (test_fb())
		return;
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		logmsg("no /dev/fb0: %s", strerror(errno));
		leave(RB_AUTOBOOT);
	}
	struct fb_fix_screeninfo finfo;
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
	fb_w = vinfo.xres;
	fb_h = vinfo.yres;
	fb_bpp = vinfo.bits_per_pixel / 8;
	fb_stride = ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 ? finfo.line_length : fb_w * fb_bpp;
	if (fb_w > fb_h)
		rotate = 0;                   /* a landscape framebuffer needs no turning */
	fb_native = fb_bpp == 4 && vinfo.red.offset == 16 && vinfo.green.offset == 8 && vinfo.blue.offset == 0;
	fb = mmap(NULL, fb_stride * fb_h, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED) {
		logmsg("mmap /dev/fb0: %s", strerror(errno));
		leave(RB_AUTOBOOT);
	}
	tty_fd = open("/dev/tty0", O_RDWR);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);   /* no console text over the picture */
	logmsg("screen %ux%u, %u bpp, rotate %d", fb_w, fb_h, fb_bpp * 8, rotate);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bool init = getpid() == 1;
	if (init)
		as_init();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	screen_init();
	/* a splash while the rest comes up */
	fill(0, 0, LAND_W, LAND_H, C_BG);
	text(250, 200, 5, C_ACCENT, "NES", 3);
	text(250, 260, 2, C_DIM, T("emulator na X6100", "emulator for the X6100"), 30);
	present(0, 0, LAND_W, LAND_H);
	sound_open();
	retro_set_environment(cb_env);
	retro_set_video_refresh(cb_video);
	retro_set_audio_sample(cb_audio_sample);
	retro_set_audio_sample_batch(cb_audio_batch);
	retro_set_input_poll(cb_input_poll);
	retro_set_input_state(cb_input_state);
	retro_init();
	open_inputs();

	const char *test_rom = getenv("NES_TEST_ROM");
	if (test_mode && test_rom) {          /* the PC test: one game, some frames, the last one kept */
		char *g = malloc(strlen(test_rom) * 2 + 2);
		sprintf(g, "%s%c%s", test_rom, 0, test_rom);
		games[0] = g;
		game_names[0] = g;
		n_games = 1;
		play(0);
		const char *outs[2] = {getenv("NES_TEST_OUT"), getenv("NES_TEST_MENU_OUT")};
		for (int k = 0; k < 2; k++) {
			if (k) {                      /* then the menu over it: its look is checked too */
				set_en = 1;
				snprintf(cur_base, sizeof cur_base, "Test game");
				game_menu();
			}
			FILE *f = outs[k] ? fopen(outs[k], "wb") : NULL;
			if (f) {
				fwrite(fb, fb_stride, fb_h, f);
				fclose(f);
			}
		}
		if (test_pcm)
			fclose(test_pcm);
		retro_deinit();
		return 0;
	}
	game_list();
	return 0;
}
