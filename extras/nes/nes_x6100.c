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
#define _GNU_SOURCE             /* posix_openpt, ptsname: the pseudo-terminal for bluetoothctl */
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
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include "libretro.h"
#include "font8x8.h"

#define LAND_W   800             /* the screen, as the radio is held */
#define LAND_H   480
#define MAX_DEVS 16
#define MAX_GAMES 2000

/* Where the games, saves and pad layouts are: the NES folder of the card's BOOT partition */
static char roms_dir[256] = "/boot/NES", saves_dir[300], bios_dir[300], pads_file[300], last_file[300];
#define ROMS  roms_dir
#define SAVES saves_dir
#define BIOS  bios_dir
#define PADS  pads_file

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

/* The kernel's buffers to the card. Not sync(): FCEUmm has a global variable of that name, which
 * the static link took for the C library's function (the call jumped into its data) */
static void flush_disks(void)
{
	syscall(SYS_sync);
}

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
static int set_bluetooth = 1, set_aspect = 0, set_en = 0, set_mouse = 100;

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
		flush_disks();
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

static bool save_state(int slot);
static bool game_on;

static void leave(int cmd)
{
	save_sram();
	if (game_on)
		save_state(9);                    /* where it was left: "continue" next time */
	text_mode();
	logmsg(cmd == (int)RB_POWER_OFF ? "POWER: switching off" : "leaving: restart");
	close_log();
	flush_disks();
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
static int menu_mouse;                      /* the mouse's way up and down, for the menus */

static char note_text[96];                  /* a word over the game for a while */
static uint32_t note_until;
static void note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(note_text, sizeof note_text, fmt, ap);
	va_end(ap);
	note_until = now_ms() + 3000;
}

/* Bluetooth's state, for the status line (the session is further down) */
static int bt_powered = -1;                 /* -1: not known / not running */
static void bt_poll(void);
static void test_keys(void);

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
	bind_key(m, J_A, BTN_C);              /* PS4 / Switch pads without their own driver: the right one */
	bind_key(m, J_B, BTN_SOUTH);          bind_key(m, J_B, BTN_TRIGGER);    /* BTN_A; a joystick's 1st */
	bind_key(m, J_X, BTN_NORTH);          bind_key(m, J_X, BTN_THUMB2);
	bind_key(m, J_Y, BTN_WEST);           bind_key(m, J_Y, BTN_TOP);
	bind_key(m, J_SELECT, BTN_SELECT);    bind_key(m, J_SELECT, BTN_BASE3);
	bind_key(m, J_START, BTN_START);      bind_key(m, J_START, BTN_BASE4);
	bind_key(m, J_SELECT, BTN_TL2);       bind_key(m, J_START, BTN_TR2);   /* Share/Options, -/+ as buttons 9, 10 */
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
	flush_disks();
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
	if (devs[i].role != ROLE_RADIO)
		note(T("Odlaczono: %.60s", "Disconnected: %.60s"), devs[i].name);
	close(devs[i].fd);
	for (int j = i; j < n_devs - 1; j++) {
		devs[j] = devs[j + 1];
		pfd[j] = pfd[j + 1];
	}
	n_devs--;
	if (learn_from == i)
		learn_from = -1;
}

/* The PC test: NES_TEST_DEVS="pad:FIFO,kbd:FIFO,mouse:FIFO", events written there by the test */
static void test_inputs(void)
{
	static bool done;
	const char *spec = getenv("NES_TEST_DEVS");
	if (done || !spec)
		return;
	done = true;
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", spec);
	for (char *t = strtok(buf, ","); t && n_devs < MAX_DEVS; t = strtok(NULL, ",")) {
		char *colon = strchr(t, ':');
		if (!colon)
			continue;
		*colon = 0;
		int fd = open(colon + 1, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		struct dev *v = &devs[n_devs];
		memset(v, 0, sizeof *v);
		v->fd = fd;
		v->port = -1;
		snprintf(v->node, sizeof v->node, "test%d", n_devs);
		snprintf(v->name, sizeof v->name, "Test %s", t);
		v->role = !strcmp(t, "pad") ? ROLE_PAD : !strcmp(t, "kbd") ? ROLE_KEYBOARD : ROLE_MOUSE;
		if (v->role == ROLE_PAD) {
			v->amin[ABS_X] = v->amin[ABS_Y] = 0;
			v->amax[ABS_X] = v->amax[ABS_Y] = 255;
			v->abs[ABS_X] = v->abs[ABS_Y] = 128;
			v->amin[ABS_HAT0X] = v->amin[ABS_HAT0Y] = -1;
			v->amax[ABS_HAT0X] = v->amax[ABS_HAT0Y] = 1;
			v->amin[ABS_Z] = 0;                /* a trigger resting at its end */
			v->amax[ABS_Z] = 255;
			if (!load_map(v))
				default_map(v);
			v->port = free_port();
		}
		logmsg("test input %s: %s", v->node, t);
		pfd[n_devs].fd = fd;
		pfd[n_devs].events = POLLIN;
		n_devs++;
	}
}

static void open_inputs(void)
{
	if (test_mode) {
		test_inputs();
		return;
	}
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
			bool known = load_map(v);
			if (!known)
				default_map(v);
			v->port = free_port();
			if (known)
				note(T("Pad gracza %d: %.50s", "Player %d pad: %.50s"), v->port + 1, v->name);
			else
				note(T("Pad gracza %d. Inny uklad? Menu > Przyciski pada", "Player %d pad. Other layout? Menu > Pad buttons"),
				     v->port + 1);
		} else if (v->role == ROLE_KEYBOARD) {
			note(T("Klawiatura: %.60s", "Keyboard: %.60s"), v->name);
		} else if (v->role == ROLE_MOUSE) {
			note(T("Mysz: %.60s", "Mouse: %.60s"), v->name);
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
	bt_poll();
	if (test_mode)
		test_keys();
	while (n_devs && poll(pfd, (nfds_t)n_devs, 0) > 0) {
		bool any = false;
		for (int i = 0; i < n_devs; i++) {
			if ((pfd[i].revents & (POLLERR | POLLNVAL)) || ((pfd[i].revents & POLLHUP) && !test_mode)) {
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
					if (ev.code == KEY_PAGEUP && ev.value) nav_push(N_LEFT);
					if (ev.code == KEY_PAGEDOWN && ev.value) nav_push(N_RIGHT);
					int j = kb_key(ev.code);
					if (j >= 0)
						kb_bits = ev.value ? kb_bits | BIT(j) : kb_bits & ~BIT(j);
				}
				break;
			case ROLE_MOUSE:
				if (ev.type == EV_REL) {
					if (ev.code == REL_X) mouse_dx += ev.value;
					else if (ev.code == REL_Y) {
						mouse_dy += ev.value;
						menu_mouse += ev.value;   /* in the menus: a step per 40 counts */
						for (; menu_mouse >= 40; menu_mouse -= 40) nav_push(N_DOWN);
						for (; menu_mouse <= -40; menu_mouse += 40) nav_push(N_UP);
					}
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
	if (test_mode) {
		static uint32_t was[2];
		if (p[0] != was[0] || p[1] != was[1])
			logmsg("test bits p1=%05x p2=%05x", p[0], p[1]);
		was[0] = p[0];
		was[1] = p[1];
	}
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

/* How far a long selected line has moved: still a moment, then along, then still at its end */
static int marquee(int len, int max)
{
	if (len <= max)
		return 0;
	int span = len - max, phase = (int)((now_ms() / 220) % (unsigned)(span + 14));
	return phase < 7 ? 0 : phase - 7 > span ? span : phase - 7;
}

/* A line max characters wide: cut with "..", or moving along when it is the chosen one */
static void text_fit(int x, int y, int s, uint32_t c, const char *str, int max, bool moving)
{
	int len = text_len(str);
	if (len <= max) {
		text(x, y, s, c, str, max);
	} else if (moving) {
		const char *p = str;
		for (int i = marquee(len, max); i > 0; i--)
			next_char(&p);
		text(x, y, s, c, p, max);
	} else {
		int w = text(x, y, s, c, str, max - 2);
		text(x + w, y, s, C_DIM, "..", 2);
	}
}

/* What is connected: pads, keyboard, mouse, Bluetooth */
static void status_text(char *out, size_t n)
{
	int pads = 0;
	bool kb = false, mouse = false;
	for (int i = 0; i < n_devs; i++) {
		pads += devs[i].role == ROLE_PAD;
		kb |= devs[i].role == ROLE_KEYBOARD;
		mouse |= devs[i].role == ROLE_MOUSE;
	}
	char p[24] = "";
	if (pads)
		snprintf(p, sizeof p, T("PAD x%d  ", "PAD x%d  "), pads);
	snprintf(out, n, "%s%s%s%s", p, kb ? T("KLAW  ", "KEYB  ") : "", mouse ? T("MYSZ  ", "MOUSE  ") : "",
	         bt_powered == 1 ? "BT" : bt_powered == 0 ? T("BT wyl", "BT off") : "");
}

static int panel_y = -1, panel_h;          /* the last box's place: cleared when a smaller one follows */

/* A list on the screen (overlay: a panel over the game) */
struct list_view {
	const char *title;
	const char *const *items;
	int n, sel, top;
	const char *hint, *empty;
	bool overlay;
};

static void draw_list(struct list_view *v)
{
	panel_y = -1;
	int x = v->overlay ? 40 : 0, y = v->overlay ? 8 : 0, w = v->overlay ? 720 : LAND_W, h = v->overlay ? 464 : LAND_H;
	int rows = (h - 110) / ROW;
	if (v->sel < v->top) v->top = v->sel;
	if (v->sel >= v->top + rows) v->top = v->sel - rows + 1;
	if (v->top < 0) v->top = 0;
	fill(x, y, w, h, v->overlay ? C_PANEL : C_BG);
	if (v->overlay) {
		fill(x, y, w, 3, C_ACCENT);
		fill(x, y + h - 2, w, 2, C_LINE);
	}
	int title_max = (w - 48) / 24;
	if (!v->overlay) {                    /* what is connected, top right */
		char st[64];
		status_text(st, sizeof st);
		int sw = text_len(st) * 16;
		if (sw)
			text(x + w - 24 - sw, y + 28, 2, C_DIM, st, 40);
		title_max = (w - 48 - sw - 16) / 24;
	}
	text_fit(x + 24, y + 20, 3, C_ACCENT, v->title, title_max, true);
	fill(x + 24, y + 54, w - 48, 2, C_LINE);
	if (!v->n && v->empty) {              /* nothing yet: the words, wrapped */
		int max = (w - 56) / 16, row = 0;
		const char *p = v->empty;
		while (*p && row < 8) {
			int len = (int)strlen(p), take = len <= max ? len : max;
			if (take < len)
				for (int k = take; k > max / 2; k--)
					if (p[k] == ' ') { take = k; break; }
			char line[128];
			snprintf(line, sizeof line, "%.*s", take, p);
			text(x + 28, y + 70 + row++ * 26, 2, C_DIM, line, max);
			p += take;
			while (*p == ' ') p++;
		}
	}
	for (int r = 0; r < rows && v->top + r < v->n; r++) {
		int i = v->top + r, ry = y + 66 + r * ROW;
		bool on = i == v->sel;
		if (on)
			fill(x + 16, ry - 5, w - 32, ROW - 2, C_ACCENT);
		text_fit(x + 28, ry, 3, on ? C_SEL_T : C_TEXT, v->items[i], (w - 64) / 24, on);
	}
	if (v->n > rows) {                    /* where in the list */
		int track = h - 130, bar = track * rows / v->n;
		if (bar < 12) bar = 12;
		int by = y + 66 + (track - bar) * v->top / (v->n - rows);
		fill(x + w - 10, y + 66, 4, track, C_LINE);
		fill(x + w - 10, by, 4, bar, C_ACCENT);
	}
	if (v->hint)
		text_fit(x + 24, y + h - 30, 2, C_DIM, v->hint, (w - 48) / 16, false);
	present(x, y, w, h);
}

/* One step of a list's choosing: -1 going on, -2 back, else the chosen one */
static int list_step(struct list_view *v, bool *dirty)
{
	int rows = ((v->overlay ? 464 : LAND_H) - 110) / ROW;
	enum nav k;
	while ((k = nav_pop()) != N_NONE) {
		*dirty = true;
		if (!v->n) {
			if (k == N_BACK || k == N_MENU) return -2;
			continue;
		}
		if (k == N_UP) v->sel = v->sel > 0 ? v->sel - 1 : v->n - 1;
		else if (k == N_DOWN) v->sel = v->sel < v->n - 1 ? v->sel + 1 : 0;
		else if (k == N_LEFT) v->sel = v->sel - rows > 0 ? v->sel - rows : 0;
		else if (k == N_RIGHT) v->sel = v->sel + rows < v->n ? v->sel + rows : v->n - 1;
		else if (k == N_OK) return v->sel;
		else if (k == N_BACK || k == N_MENU) return -2;
	}
	return -1;
}

static bool keys_scripted(void);

/* A list to choose from: its index, or -1 (back). overlay: a panel over the game */
static int menu(const char *title, const char *const *items, int n, int sel, const char *hint, bool overlay)
{
	struct list_view v = {title, items, n, sel < 0 || sel >= n ? 0 : sel, 0, hint, NULL, overlay};
	bool dirty = true;
	uint32_t drawn = 0;
	input_settle();
	for (;;) {
		poll_input();
		int c = list_step(&v, &dirty);
		want_menu = false;
		if (c != -1) {
			input_settle();
			return c == -2 ? -1 : c;
		}
		if (dirty || now_ms() - drawn > 400) {   /* now and then: a long line moves, the status */
			dirty = false;
			drawn = now_ms();
			draw_list(&v);
		}
		if (test_mode && !keys_scripted())
			return v.sel;
		idle(10);
	}
}

/* A box with a title and lines (big: the one in large type, a code to type) */
static void panel(const char *title, const char *const *lines, int n, int big)
{
	int x = 20, w = 760, h = 110 + n * 28 + (big >= 0 ? 40 : 0), y = (LAND_H - h) / 2;
	if (panel_y >= 0 && (panel_y < y || panel_y + panel_h > y + h)) {
		fill(x, panel_y, w, panel_h, C_BG);
		present(x, panel_y, w, panel_h);
	}
	panel_y = y;
	panel_h = h;
	fill(x, y, w, h, C_PANEL);
	fill(x, y, w, 3, C_ACCENT);
	fill(x, y + h - 2, w, 2, C_LINE);
	text_fit(x + 24, y + 22, 3, C_ACCENT, title, (w - 48) / 24, true);
	int yy = y + 70;
	for (int i = 0; i < n; i++) {
		if (i == big) {
			text(x + 24, yy, 5, C_WARN, lines[i], 14);
			yy += 48;
		} else {
			text_fit(x + 24, yy, 2, C_TEXT, lines[i], (w - 48) / 16, false);
			yy += 28;
		}
	}
	present(x, y, w, h);
}

/* A message until a key (or for ms milliseconds, if not 0) */
static void message(const char *title, const char *const *lines, int n, unsigned ms)
{
	const char *all[12];
	int k = 0;
	for (int i = 0; i < n && k < 10; i++)
		all[k++] = lines[i];
	if (!ms)
		all[k++] = T("[ F2 / A / Enter: OK ]", "[ F2 / A / Enter: OK ]");
	panel(title, all, k, -1);
	if (test_mode && !keys_scripted())
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
		{"fceumm_mouse_sensitivity", NULL},       /* filled in: mouse= in nes.txt */
	};
	static char rate[16];
	for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
		if (!strcmp(v->key, vars[i].key)) {
			if (vars[i].value) {
				v->value = vars[i].value;
			} else if (!strcmp(v->key, "fceumm_mouse_sensitivity")) {
				snprintf(rate, sizeof rate, "%d", set_mouse);
				v->value = rate;
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
	flush_disks();
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
	flush_disks();
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
	/* the devices forgotten in the emulator: gone again (the lending system's own copy keeps them) */
	"if [ -f /nes-bt/forgotten.txt ]; then for mac in $(cat /nes-bt/forgotten.txt); do rm -rf /var/lib/bluetooth/*/$mac; done; fi\n"
	"dbus-uuidgen --ensure 2>/dev/null\n"
	"dbus-daemon --system\n"
	"sleep 1\n"
	"/usr/libexec/bluetooth/bluetoothd -n >/tmp/bluetoothd.log 2>&1 &\n"
	"sleep 3\n"
	"if [ -f /nes-bt/off ]; then bluetoothctl power off; else bluetoothctl power on; fi\n"
	"touch /tmp/nes-bt-ready\n"
	"[ -f /nes-bt/off ] && exit 0\n"
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

/* The pairings kept on the emulator's partition ($1 save), a device forgotten for good ($1 forget,
 * $2 the device: the lending system's own copy would bring it back at every start) */
static const char bt_save_script[] =
	"exec >>/tmp/nes-save.log 2>&1\n"
	"export PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
	"mkdir -p /nes-bt\n"
	"if [ \"$1\" = forget ]; then echo $2 >> /nes-bt/forgotten.txt; fi\n"
	"if [ \"$1\" = save ] && [ -f /nes-bt/forgotten.txt ]; then\n"
	"  for m in $(ls /var/lib/bluetooth/*/ 2>/dev/null); do grep -v \"$m\" /nes-bt/forgotten.txt > /tmp/f.txt;"
	" cp /tmp/f.txt /nes-bt/forgotten.txt; done\n"
	"fi\n"
	"sleep 2\n"
	"tar -C /var/lib/bluetooth -cf /nes-bt/pairings.tar . && sync\n";

/* A shell script in the lending system's chroot; its pid */
static pid_t run_chroot(const char *script, const char *name, const char *arg, const char *arg2)
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
			execl("/bin/sh", "sh", inner, arg ? arg : "-", arg2, (char *)NULL);
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
	if (run_chroot(bt_script, "nes-bt.sh", NULL, NULL) > 0) {
		bt_started = true;
		logmsg("Bluetooth: starting the donor's BlueZ");
	}
}

/* ------------------------------------------------------------------------------------------ */
/* The emulator's screens                                                                     */
/* ------------------------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------------ */
/* Bluetooth: one bluetoothctl kept open on a pseudo-terminal, its every line read             */
/* ------------------------------------------------------------------------------------------ */
#define MAX_BT 48
struct btdev {
	char mac[18], name[64], icon[32];
	bool paired, trusted, connected, seen;
};
static struct btdev btd[MAX_BT];
static int n_btd;
static int bt_fd = -1;
static pid_t bt_pid = -1;
static char bt_buf[4096];
static size_t bt_len;
static char bt_cur[18];               /* the device an "info" block is about */
static bool bt_in_ctrl;               /* inside a "show" block */
static uint32_t bt_retry;
/* what is under way: a pairing, a connection */
enum { OP_NONE, OP_PAIR, OP_CONNECT, OP_DISCONNECT };
static int op_kind, op_state;         /* op_state: 0 none, 1 waiting, 2 done, 3 failed */
static char op_mac[18], op_err[120], op_code[16];

static bool is_mac(const char *s)
{
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i]))
			return false;
	}
	return true;
}

static struct btdev *bt_dev(const char *mac, bool add)
{
	for (int i = 0; i < n_btd; i++)
		if (!strncasecmp(btd[i].mac, mac, 17))
			return &btd[i];
	if (!add || n_btd >= MAX_BT)
		return NULL;
	struct btdev *d = &btd[n_btd++];
	memset(d, 0, sizeof *d);
	memcpy(d->mac, mac, 17);
	return d;
}

static void bt_send(const char *fmt, ...)
{
	if (bt_fd < 0)
		return;
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0 || n > (int)sizeof line - 2)
		return;
	logmsg("bt> %s", line);
	line[n++] = '\n';
	if (write(bt_fd, line, (size_t)n) < 0)
		logmsg("bt: write: %s", strerror(errno));
}

static void bt_set(struct btdev *d, const char *key, const char *val)
{
	bool yes = !strncmp(val, "yes", 3);
	if (!strcmp(key, "Name"))
		snprintf(d->name, sizeof d->name, "%s", val);
	else if (!strcmp(key, "Alias") && !d->name[0])
		snprintf(d->name, sizeof d->name, "%s", val);
	else if (!strcmp(key, "Icon"))
		snprintf(d->icon, sizeof d->icon, "%s", val);
	else if (!strcmp(key, "Paired") || !strcmp(key, "Bonded")) {
		if (yes) d->paired = true;
		else if (!strcmp(key, "Paired")) d->paired = false;
	} else if (!strcmp(key, "Trusted"))
		d->trusted = yes;
	else if (!strcmp(key, "Connected")) {
		d->connected = yes;
		if (op_state == 1 && !strcasecmp(op_mac, d->mac)) {
			if (op_kind == OP_CONNECT && yes) op_state = 2;
			if (op_kind == OP_DISCONNECT && !yes) op_state = 2;
		}
	} else if (!strcmp(key, "RSSI") || !strcmp(key, "TxPower") || !strcmp(key, "ManufacturerData"))
		d->seen = true;
}

/* Colours, the line editor's moves and prompts out: the words only */
static void bt_clean(char *s)
{
	char *o = s;
	for (char *p = s; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == 0x1b) {                  /* ESC [ ... a letter, or ESC and one more */
			if (p[1] == '[') {
				p += 2;
				while (*p && !(*p >= '@' && *p <= '~'))
					p++;
				if (!*p)
					break;
			} else if (p[1]) {
				p++;
			}
			continue;
		}
		if (c < 0x20 && c != '\t')
			continue;
		*o++ = (char)c;
	}
	*o = 0;
}

static bool bt_answer(const char *t);

static void bt_line(char *line)
{
	bt_clean(line);
	char *s = line, *p;
	while ((p = strstr(s, "]# ")))            /* the prompt, maybe more than once */
		s = p + 3;
	bool indented = *s == '\t' || *s == ' ';
	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s)
		return;
	logmsg("bt< %s", s);
	if (bt_answer(s))
		return;
	char tag = 0;
	if (!strncmp(s, "[NEW] ", 6)) tag = 'N';
	else if (!strncmp(s, "[CHG] ", 6)) tag = 'C';
	else if (!strncmp(s, "[DEL] ", 6)) tag = 'D';
	if (tag) {
		char *r = s + 6;
		if (!strncmp(r, "Controller ", 11)) {
			char *k = strstr(r, "Powered: ");
			if (k) bt_powered = !strncmp(k + 9, "yes", 3);
			return;
		}
		if (strncmp(r, "Device ", 7) || !is_mac(r + 7))
			return;
		char *mac = r + 7, *rest = r + 24;
		while (*rest == ' ') rest++;
		struct btdev *d = bt_dev(mac, tag != 'D');
		if (!d)
			return;
		if (tag == 'N') {
			d->seen = true;
			if (*rest && strncmp(rest, mac, 2))        /* a name, not the address again */
				snprintf(d->name, sizeof d->name, "%s", rest);
			bt_send("info %.17s", mac);                  /* what it is: a pad, a keyboard... */
		} else if (tag == 'D') {
			if (d->paired) {
				d->seen = false;
			} else {
				*d = btd[--n_btd];
			}
		} else {
			char *colon = strstr(rest, ": ");
			if (colon) {
				*colon = 0;
				bt_set(d, rest, colon + 2);
			}
		}
		return;
	}
	if (!strncmp(s, "[agent] ", 8)) {
		char *k = strstr(s, "Passkey: ");
		if (!k) k = strstr(s, "PIN code: ");
		if (k) {
			int j = 0;
			for (k = strchr(k, ':') + 1; *k && j < 15; k++)
				if (isdigit((unsigned char)*k))
					op_code[j++] = *k;
			op_code[j] = 0;
		}
		return;
	}
	if (!indented && !strncmp(s, "Device ", 7) && is_mac(s + 7)) {
		char *rest = s + 24;
		while (*rest == ' ') rest++;
		snprintf(bt_cur, sizeof bt_cur, "%.17s", s + 7);
		bt_in_ctrl = false;
		if (*rest != '(') {                       /* "devices": the address and the name */
			struct btdev *d = bt_dev(bt_cur, true);
			if (d && *rest && !d->name[0])
				snprintf(d->name, sizeof d->name, "%s", rest);
			bt_send("info %s", bt_cur);
		} else {
			bt_dev(bt_cur, true);
		}
		return;
	}
	if (!indented && !strncmp(s, "Controller ", 11)) {
		bt_in_ctrl = true;
		bt_cur[0] = 0;
		return;
	}
	if (indented) {
		char *colon = strstr(s, ": ");
		if (!colon)
			return;
		*colon = 0;
		if (bt_in_ctrl && !strcmp(s, "Powered"))
			bt_powered = !strncmp(colon + 2, "yes", 3);
		else if (bt_cur[0]) {
			struct btdev *d = bt_dev(bt_cur, true);
			if (d) bt_set(d, s, colon + 2);
		}
		return;
	}
	if (op_state == 1) {
		if (op_kind == OP_PAIR && strstr(s, "Pairing successful"))
			op_state = 2;
		else if (op_kind == OP_PAIR && (p = strstr(s, "Failed to pair"))) {
			op_state = strstr(s, "AlreadyExists") ? 2 : 3;
			snprintf(op_err, sizeof op_err, "%.119s", p);
		} else if (op_kind == OP_CONNECT && strstr(s, "Connection successful"))
			op_state = 2;
		else if (op_kind == OP_CONNECT && (p = strstr(s, "Failed to connect"))) {
			op_state = 3;
			snprintf(op_err, sizeof op_err, "%.119s", p);
		} else if (op_kind == OP_DISCONNECT && strstr(s, "Successful disconnected"))
			op_state = 2;
		else if (strstr(s, "not available")) {
			op_state = 3;
			snprintf(op_err, sizeof op_err, "%.119s", s);
		}
	}
	if (!strncmp(s, "Changing power", 14) && strstr(s, "succeeded"))
		bt_powered = strstr(s, " on ") != NULL;
}

/* BlueZ's agent asking: the answer. true: it was a question */
static bool bt_answer(const char *t)
{
	if (strstr(t, "(yes/no)")) {
		bt_send("yes");                      /* confirm the passkey, authorize the service */
	} else if (strstr(t, "Enter PIN code:")) {
		/* an older keyboard: our PIN, typed on it too */
		snprintf(op_code, sizeof op_code, "%04u", (unsigned)(now_ms() * 7919u % 10000u));
		bt_send("%s", op_code);
	} else if (strstr(t, "Enter passkey")) {
		bt_send("0");                        /* a device with its own display: not for us */
	} else {
		return false;
	}
	return true;
}

/* A question without its line's end (the prompt waits there): the answer at once */
static void bt_prompt(void)
{
	if (!bt_len)
		return;
	char tmp[sizeof bt_buf + 1];
	memcpy(tmp, bt_buf, bt_len);
	tmp[bt_len] = 0;
	bt_clean(tmp);
	if (bt_answer(tmp)) {
		logmsg("bt< %s (answered)", tmp);
		bt_len = 0;
	}
}

static void bt_close(void)
{
	if (bt_fd >= 0)
		close(bt_fd);
	bt_fd = -1;
	if (bt_pid > 0) {
		kill(bt_pid, SIGTERM);
		waitpid(bt_pid, NULL, WNOHANG);
	}
	bt_pid = -1;
	bt_retry = now_ms() + 10000;
}

static bool bt_open(void)
{
	if (bt_fd >= 0)
		return true;
	const char *fake = getenv("NES_TEST_BTCTL");      /* the PC test: a pretend bluetoothctl */
	if (!fake && access("/bt/tmp/nes-bt-ready", F_OK) != 0)
		return false;
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) || unlockpt(m)) {
		if (m >= 0) close(m);
		return false;
	}
	const char *sn = ptsname(m);
	struct winsize ws = {.ws_row = 60, .ws_col = 400};
	ioctl(m, TIOCSWINSZ, &ws);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int sl = open(sn, O_RDWR);
		if (sl < 0)
			_exit(1);
		ioctl(sl, TIOCSCTTY, 0);
		struct termios t;
		if (tcgetattr(sl, &t) == 0) {
			t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
			t.c_oflag &= ~(tcflag_t)ONLCR;
			tcsetattr(sl, TCSANOW, &t);
		}
		dup2(sl, 0);
		dup2(sl, 1);
		dup2(sl, 2);
		close(m);
		setenv("TERM", "dumb", 1);
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		if (fake)
			execl(fake, fake, (char *)NULL);
		else if (chroot("/bt") == 0 && chdir("/") == 0)
			execl("/usr/bin/bluetoothctl", "bluetoothctl", (char *)NULL);
		_exit(1);
	}
	if (pid < 0) {
		close(m);
		return false;
	}
	fcntl(m, F_SETFL, O_NONBLOCK);
	bt_fd = m;
	bt_pid = pid;
	bt_len = 0;
	logmsg("bt: bluetoothctl started");
	/* our agent answers the pairing questions; the controller's state; the devices known */
	bt_send("agent KeyboardDisplay");
	bt_send("default-agent");
	bt_send("show");
	bt_send("devices");
	return true;
}

static void bt_poll(void)
{
	if (bt_fd < 0) {
		if ((bt_started || getenv("NES_TEST_BTCTL")) && (int32_t)(now_ms() - bt_retry) >= 0) {
			bt_retry = now_ms() + 3000;
			bt_open();
		}
		return;
	}
	for (;;) {
		if (bt_len >= sizeof bt_buf - 1)
			bt_len = 0;                       /* a line longer than ever: dropped */
		ssize_t r = read(bt_fd, bt_buf + bt_len, sizeof bt_buf - 1 - bt_len);
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (r <= 0) {
			logmsg("bt: bluetoothctl ended");
			bt_close();
			return;
		}
		bt_len += (size_t)r;
		char *nl;
		while ((nl = memchr(bt_buf, '\n', bt_len))) {
			*nl = 0;
			char line[sizeof bt_buf];
			memcpy(line, bt_buf, (size_t)(nl - bt_buf) + 1);
			size_t used = (size_t)(nl - bt_buf) + 1;
			memmove(bt_buf, bt_buf + used, bt_len - used);
			bt_len -= used;
			bt_line(line);
		}
	}
	bt_prompt();
}

/* The pairings to the emulator's partition (a moment after BlueZ wrote them) */
static void bt_keep(const char *what, const char *mac)
{
	if (!getenv("NES_TEST_BTCTL"))
		run_chroot(bt_save_script, "nes-save.sh", what, mac);
}

static const char *kind_of(const char *icon)
{
	if (!icon || !*icon) return "";
	if (strstr(icon, "gaming")) return T(" (pad)", " (pad)");
	if (strstr(icon, "keyboard")) return T(" (klawiatura)", " (keyboard)");
	if (strstr(icon, "mouse")) return T(" (mysz)", " (mouse)");
	if (strstr(icon, "tablet")) return T(" (tablet)", " (tablet)");
	if (strstr(icon, "audio")) return T(" (audio)", " (audio)");
	if (strstr(icon, "phone")) return T(" (telefon)", " (phone)");
	if (strstr(icon, "computer")) return T(" (komputer)", " (computer)");
	return "";
}

static const char *bt_label(const struct btdev *d)
{
	static char out[96];
	snprintf(out, sizeof out, "%s%s", d->name[0] ? d->name : d->mac, kind_of(d->icon));
	return out;
}

/* BlueZ's reason, in words */
static const char *bt_reason(const char *err)
{
	if (strstr(err, "AuthenticationFailed")) return T("Zly kod albo urzadzenie odmowilo.", "A wrong code, or the device refused.");
	if (strstr(err, "AuthenticationCanceled") || strstr(err, "AuthenticationRejected"))
		return T("Urzadzenie odrzucilo parowanie.", "The device refused the pairing.");
	if (strstr(err, "AuthenticationTimeout")) return T("Za dlugo: kod nie zostal wpisany.", "Too long: the code was not typed.");
	if (strstr(err, "ConnectionAttemptFailed") || strstr(err, "Page Timeout") || strstr(err, "not available") ||
	    strstr(err, "Host is down"))
		return T("Nie odpowiada: wlacz w nim parowanie.", "No answer: put it in pairing mode.");
	if (strstr(err, "InProgress")) return T("Juz trwa: sprobuj za chwile.", "Already under way: try again soon.");
	if (strstr(err, "NotReady")) return T("Bluetooth nie jest gotowy.", "Bluetooth is not ready.");
	return err[0] ? err : T("Nieznany blad.", "Unknown error.");
}

static bool bt_ready(void)
{
	if (bt_fd >= 0)
		return true;
	if (bt_started || getenv("NES_TEST_BTCTL")) {     /* it may still be coming up: a few seconds */
		uint32_t until = now_ms() + 8000;
		while (bt_fd < 0 && (int32_t)(now_ms() - until) < 0) {
			const char *l[] = {T("Bluetooth sie uruchamia...", "Bluetooth is starting...")};
			panel("Bluetooth", l, 1, -1);
			bt_retry = 0;
			poll_input();
			idle(200);
		}
		if (bt_fd >= 0)
			return true;
	}
	const char *l[] = {T("Bluetooth nie dziala.", "Bluetooth is not running."),
	                   T("Szczegoly: nes-log.txt na karcie.", "Details: nes-log.txt on the card."),
	                   T("bluetooth=1 w nes.txt?", "bluetooth=1 in nes.txt?")};
	message("Bluetooth", l, 3, 0);
	return false;
}

/* Waiting for what is under way, the words on the screen; back cancels. true: done */
static bool bt_wait(const char *title, const char *what, unsigned secs, bool show_code)
{
	uint32_t start = now_ms();
	input_settle();
	while (op_state == 1) {
		unsigned el = (now_ms() - start) / 1000;
		if (el >= secs) {
			op_state = 3;
			snprintf(op_err, sizeof op_err, T("Za dlugo bez odpowiedzi.", "No answer for too long."));
			break;
		}
		char sec[64];
		snprintf(sec, sizeof sec, "%u s   (F1/B: %s)", el, T("przerwij", "stop"));
		if (show_code && op_code[0]) {
			const char *l[] = {what, T("Wpisz ten kod na klawiaturze i Enter:", "Type this code on the keyboard, then Enter:"),
			                   op_code, sec};
			panel(title, l, 4, 2);
		} else {
			const char *l[] = {what, sec};
			panel(title, l, 2, -1);
		}
		poll_input();
		enum nav k;
		while ((k = nav_pop()) != N_NONE)
			if (k == N_BACK || k == N_MENU) {
				if (op_kind == OP_PAIR)
					bt_send("cancel-pairing %s", op_mac);
				op_state = 3;
				snprintf(op_err, sizeof op_err, T("Przerwane.", "Stopped."));
			}
		idle(100);
	}
	input_settle();
	return op_state == 2;
}

static void bt_start(int kind, const char *mac)
{
	op_kind = kind;
	op_state = 1;
	op_err[0] = op_code[0] = 0;
	snprintf(op_mac, sizeof op_mac, "%s", mac);
}

/* Pairing one: paired, trusted, connected, kept */
static void bt_pair(const char *mac)
{
	struct btdev *d = bt_dev(mac, true);
	char name[96];
	snprintf(name, sizeof name, "%s", bt_label(d));
	bt_send("scan off");                       /* pairing goes better without looking around */
	bt_start(OP_PAIR, mac);
	bt_send("pair %s", mac);
	bool ok = bt_wait(T("Parowanie", "Pairing"), name, 40, true);
	logmsg("bt: pairing %s: %s %s", mac, ok ? "ok" : "failed", op_err);
	if (!ok) {
		const char *l[] = {name, T("Nie udalo sie sparowac:", "Pairing failed:"), bt_reason(op_err)};
		message("Bluetooth", l, 3, 0);
		return;
	}
	d = bt_dev(mac, true);
	if (d) d->paired = true;
	bt_send("trust %s", mac);
	bt_start(OP_CONNECT, mac);
	bt_send("connect %s", mac);
	bool conn = bt_wait(T("Laczenie", "Connecting"), name, 15, false);
	bt_keep("save", mac);
	const char *l[] = {name, T("Sparowane.", "Paired."),
	                   conn ? T("Polaczone: mozna grac.", "Connected: ready to play.")
	                        : T("Polaczy sie samo, gdy je wlaczysz.", "It connects when you switch it on.")};
	message(T("Gotowe", "Done"), l, 3, 0);
}

/* The list of what is around, as it comes in; the chosen one paired */
static void bt_scan(void)
{
	for (int i = 0; i < n_btd; i++)
		btd[i].seen = false;
	bt_send("scan on");
	uint32_t start = now_ms();
	bool scanning = true, dirty = true;
	char macs[MAX_BT][18], labels[MAX_BT][96];
	const char *items[MAX_BT];
	char title[64];
	struct list_view v = {title, items, 0, 0, 0,
	                      T("F2/A: paruj   F1/B: wstecz", "F2/A: pair   F1/B: back"),
	                      T("Wlacz w urzadzeniu tryb parowania (pad: zwykle przytrzymaj jego przycisk parowania "
	                        "kilka sekund). Pojawi sie na tej liscie.",
	                        "Put the device in pairing mode (a pad: usually hold its pairing button a few seconds). "
	                        "It shows up in this list."),
	                      false};
	uint32_t drawn = 0;
	input_settle();
	for (;;) {
		poll_input();
		unsigned el = (now_ms() - start) / 1000;
		if (scanning && el >= 90) {
			bt_send("scan off");
			scanning = false;
		}
		/* pads, keyboards, mice first; then the named; the nameless last */
		int n = 0;
		for (int pass = 0; pass < 3; pass++)
			for (int i = 0; i < n_btd && n < MAX_BT; i++) {
				struct btdev *d = &btd[i];
				if (!d->seen || d->paired)
					continue;
				int rank = !strncmp(d->icon, "input-", 6) ? 0 : d->name[0] ? 1 : 2;
				if (rank != pass)
					continue;
				snprintf(macs[n], sizeof macs[n], "%.17s", d->mac);
				snprintf(labels[n], sizeof labels[n], "%s", bt_label(d));
				items[n] = labels[n];
				n++;
			}
		if (n != v.n) dirty = true;
		v.n = n;
		if (v.sel >= n) v.sel = n ? n - 1 : 0;
		snprintf(title, sizeof title, scanning ? T("Szukam... %u s", "Looking... %u s") : T("Znalezione", "Found"), el);
		int c = list_step(&v, &dirty);
		want_menu = false;
		if (c == -2)
			break;
		if (c >= 0 && c < n) {
			char mac[18];
			snprintf(mac, sizeof mac, "%s", macs[c]);
			bt_pair(mac);
			break;
		}
		if (dirty || now_ms() - drawn > 500) {
			dirty = false;
			drawn = now_ms();
			draw_list(&v);
		}
		if (test_mode && !keys_scripted())
			break;
		idle(20);
	}
	if (scanning)
		bt_send("scan off");
}

/* The paired devices: each one connected, disconnected, forgotten */
static void bt_paired(void)
{
	bt_send("devices");                       /* fresh: each one's info comes back */
	int sel = 0;
	for (;;) {
		char macs[MAX_BT][18], labels[MAX_BT][100];
		const char *items[MAX_BT];
		int n = 0;
		uint32_t settle = now_ms() + 800;
		while ((int32_t)(now_ms() - settle) < 0) {   /* the answers in */
			poll_input();
			idle(20);
		}
		for (int i = 0; i < n_btd && n < MAX_BT; i++) {
			struct btdev *d = &btd[i];
			if (!d->paired)
				continue;
			snprintf(macs[n], sizeof macs[n], "%.17s", d->mac);
			snprintf(labels[n], sizeof labels[n], "%s %s", d->connected ? "*" : "-", bt_label(d));
			items[n] = labels[n];
			n++;
		}
		if (!n) {
			const char *l[] = {T("Brak sparowanych urzadzen.", "No paired devices.")};
			message("Bluetooth", l, 1, 0);
			return;
		}
		int c = menu(T("Sparowane  (* polaczone)", "Paired  (* connected)"), items, n, sel,
		             set_en ? HINT_LIST_EN : HINT_LIST_PL, false);
		if (c < 0)
			return;
		sel = c;
		struct btdev *d = bt_dev(macs[c], false);
		if (!d)
			continue;
		char name[96];
		snprintf(name, sizeof name, "%s", bt_label(d));
		const char *acts[] = {d->connected ? T("Rozlacz", "Disconnect") : T("Polacz", "Connect"),
		                      T("Usun (zapomnij)", "Remove (forget)"), T("Wroc", "Back")};
		int a = menu(name, acts, 3, 0, set_en ? HINT_LIST_EN : HINT_LIST_PL, false);
		if (a == 0 && !d->connected) {
			bt_start(OP_CONNECT, d->mac);
			bt_send("connect %s", d->mac);
			if (!bt_wait(T("Laczenie", "Connecting"), name, 15, false)) {
				const char *l[] = {name, T("Nie polaczono:", "Not connected:"), bt_reason(op_err),
				                   T("Wlacz urzadzenie i sprobuj znowu.", "Switch it on and try again.")};
				message("Bluetooth", l, 4, 0);
			}
		} else if (a == 0) {
			bt_start(OP_DISCONNECT, d->mac);
			bt_send("disconnect %s", d->mac);
			bt_wait(T("Rozlaczanie", "Disconnecting"), name, 8, false);
		} else if (a == 1) {
			char mac[18];
			snprintf(mac, sizeof mac, "%s", d->mac);
			bt_send("remove %s", mac);
			*d = btd[--n_btd];
			bt_keep("forget", mac);
			const char *l[] = {name, T("Usuniete.", "Removed.")};
			message("Bluetooth", l, 2, 1500);
			sel = 0;
		}
	}
}

/* Bluetooth: looking for new devices, the paired ones, on and off */
static void bt_menu(void)
{
	if (!bt_ready())
		return;
	int sel = 0;
	for (;;) {
		bool on = bt_powered != 0;
		int paired = 0, conn = 0;
		for (int i = 0; i < n_btd; i++) {
			paired += btd[i].paired;
			conn += btd[i].paired && btd[i].connected;
		}
		char p_item[64];
		snprintf(p_item, sizeof p_item, T("Sparowane (%d, polaczone %d)", "Paired (%d, %d connected)"),
		         paired, conn);
		const char *items[] = {
			T("Szukaj i paruj nowe", "Look for and pair new"), p_item,
			on ? T("Bluetooth: wlaczony (wylacz)", "Bluetooth: on (switch off)")
			   : T("Bluetooth: wylaczony (wlacz)", "Bluetooth: off (switch on)"),
			T("Wroc", "Back"),
		};
		int c = menu("Bluetooth", items, 4, sel, set_en ? HINT_LIST_EN : HINT_LIST_PL, false);
		if (c < 0 || c == 3)
			break;
		sel = c;
		if (c == 0) {
			if (!on) {
				bt_send("power on");
				unlink("/nes/bt/off");
			}
			bt_scan();
		} else if (c == 1) {
			bt_paired();
		} else {
			bt_send(on ? "power off" : "power on");
			if (on) {
				FILE *f = fopen("/nes/bt/off", "w");    /* kept: the next start too */
				if (f) fclose(f);
			} else {
				unlink("/nes/bt/off");
			}
			bt_powered = !on;
			flush_disks();
		}
	}
	memset(canvas, 0, sizeof canvas);
	present(0, 0, LAND_W, LAND_H);
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
enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_HELP, M_PAD, M_PAIR, M_LIST, M_REBOOT, M_OFF, M_N };

static bool state_exists(int slot)
{
	char ext[16], path[700];
	snprintf(ext, sizeof ext, "state%d", slot);
	save_path(path, sizeof path, ext);
	return access(path, R_OK) == 0;
}

static int game_menu(void)
{
	const char *items[M_N] = {
		T("Wroc do gry", "Back to the game"), T("Zapisz stan", "Save state"),
		state_exists(0) ? T("Wczytaj stan", "Load state") : T("Wczytaj stan (brak)", "Load state (none)"),
		T("Reset gry", "Reset the game"), T("Sterowanie", "Controls"), T("Przyciski pada", "Pad buttons"),
		T("Bluetooth", "Bluetooth"), T("Lista gier", "Game list"),
		T("Restart do menu startowego", "Restart to the boot menu"), T("Wylacz radio", "Switch the radio off"),
	};
	int c = menu(cur_base, items, M_N, 0, T("F2/A: wybierz  GEN: do gry", "F2/A: choose  GEN: back to the game"), true);
	return c < 0 ? M_RESUME : c;
}

/* How to play, on one screen */
static void help(void)
{
	const char *pl[] = {
		"RADIO: BAND/DFN/DFL krzyzak, F1 B, F2 A,",
		"  PTT A, F3 Select, F4 Start, F5 turbo A,",
		"  galki VFO i VOL kierunki, GEN menu",
		"KLAWIATURA: strzalki, X A, Z B, Enter Start,",
		"  Shift Select, C/V turbo, Esc menu,",
		"  F2 zapis stanu, F4 odczyt stanu",
		"PAD: gracz 1 i 2; menu: Home lub Sel+Start",
		"MYSZ: pistolet Zapper (np. Duck Hunt)",
	};
	const char *en[] = {
		"RADIO: BAND/DFN/DFL d-pad, F1 B, F2 A,",
		"  PTT A, F3 Select, F4 Start, F5 turbo A,",
		"  VFO and VOL knobs move, GEN menu",
		"KEYBOARD: arrows, X A, Z B, Enter Start,",
		"  Shift Select, C/V turbo, Esc menu,",
		"  F2 save state, F4 load state",
		"PAD: players 1 and 2; menu: Home or Sel+Start",
		"MOUSE: the Zapper light gun (e.g. Duck Hunt)",
	};
	message(T("Sterowanie", "Controls"), set_en ? en : pl, 8, 0);
}

/* A word over the game, top left */
static void osd(const char *msg)
{
	int w = text_len(msg) * 16 + 32;
	if (w > LAND_W) w = LAND_W;
	fill(0, 0, w, 40, C_PANEL);
	fill(0, 38, w, 2, C_ACCENT);
	text(16, 12, 2, C_TEXT, msg, (LAND_W - 32) / 16);
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
	game_on = true;
	FILE *lf = fopen(last_file, "w");          /* chosen first in the list next time */
	if (lf) {
		fputs(path, lf);
		fclose(lf);
	}
	if (state_exists(9)) {                     /* left in the middle last time */
		const char *it[] = {T("Kontynuuj od miejsca wyjscia", "Continue where you left"),
		                    T("Zacznij od nowa", "Start again")};
		memset(canvas, 0, sizeof canvas);
		int c = menu(cur_base, it, 2, 0, set_en ? HINT_LIST_EN : HINT_LIST_PL, false);
		if (c != 1 && load_state(9))
			note(T("Kontynuacja", "Continuing"));
	}
	frame_data = NULL;
	mapped_w = 0;
	memset(canvas, 0, sizeof canvas);
	present(0, 0, LAND_W, LAND_H);
	input_settle();
	uint32_t last_save = now_ms(), fps_t = now_ms();
	bool osd_shown = false;
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
			case M_HELP: help(); break;
			case M_PAD: learn_pad(); break;
			case M_PAIR: bt_menu(); break;
			case M_LIST: save_state(9); done = true; break;
			case M_REBOOT: leave(RB_AUTOBOOT); break;
			case M_OFF: leave(RB_POWER_OFF); break;
			}
			memset(canvas, 0, sizeof canvas);
			present(0, 0, LAND_W, LAND_H);
			if (msg)
				note("%s", msg);
			if (pcm)
				pcm_prepare(pcm);
			clock_gettime(CLOCK_MONOTONIC, &next);
			if (done)
				break;
		}
		retro_run();
		draw_game();
		if (note_until && (int32_t)(now_ms() - note_until) < 0) {   /* a word for a while, top left */
			osd(note_text);
			osd_shown = true;
		} else if (osd_shown) {
			osd_shown = false;
			note_until = 0;
			fill(0, 0, LAND_W, 40, 0);
			present(0, 0, LAND_W, 40);
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
			if (!test_mode || keys_scripted())
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
		if (test_mode && !keys_scripted() && (int)frames >= test_frames)
			break;
	}
	save_sram();
	game_on = false;
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
	bool first = true;
	for (;;) {
		scan_games();
		if (first) {                              /* the game played last */
			first = false;
			char last[700] = "";
			FILE *lf = fopen(last_file, "r");
			if (lf) {
				if (!fgets(last, sizeof last, lf)) last[0] = 0;
				fclose(lf);
			}
			for (int i = 0; i < n_games; i++)
				if (!strcmp(game_path(i), last))
					sel = i;
		}
		enum { X_PAIR, X_PAD, X_REBOOT, X_OFF, X_N };
		const char *extra[X_N] = {
			T("> Bluetooth", "> Bluetooth"),
			T("> Przyciski pada", "> Pad buttons"),
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
		case X_PAIR: bt_menu(); break;
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
		else if (sscanf(line, " mouse = %d", &v) == 1 && v >= 10 && v <= 500) set_mouse = v;
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
	mkdir("/dev/pts", 0755);                  /* the pseudo-terminal bluetoothctl runs on */
	if (mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620,ptmxmode=666") != 0)
		logmsg("devpts: %s", strerror(errno));
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

/* The PC test: NES_TEST_KEYS walks the menus (u d l r o b m: up down left right ok back menu,
 * w: wait, p: a picture of the screen into NES_TEST_SHOTS/shotNN.raw), then the test ends */
static const char *test_k;
static uint32_t test_next;
static int test_shot;

static bool keys_scripted(void)
{
	return test_k != NULL;
}

static void test_keys(void)
{
	if (!test_k || (int32_t)(now_ms() - test_next) < 0)
		return;
	test_next = now_ms() + 450;
	char c = *test_k;
	if (!c) {
		logmsg("test: keys done");
		bt_close();
		exit(0);
	}
	test_k++;
	switch (c) {
	case 'u': nav_push(N_UP); break;
	case 'd': nav_push(N_DOWN); break;
	case 'l': nav_push(N_LEFT); break;
	case 'r': nav_push(N_RIGHT); break;
	case 'o': nav_push(N_OK); break;
	case 'b': nav_push(N_BACK); break;
	case 'm': nav_push(N_MENU); want_menu = true; break;
	case 'p': {
		char path[600];
		snprintf(path, sizeof path, "%s/shot%02d.raw", getenv("NES_TEST_SHOTS") ? getenv("NES_TEST_SHOTS") : ".", test_shot++);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(fb, fb_stride, fb_h, f);
			fclose(f);
		}
		break;
	}
	default: break;
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
	if (getenv("NES_TEST_ROMS"))
		snprintf(roms_dir, sizeof roms_dir, "%s", getenv("NES_TEST_ROMS"));
	snprintf(saves_dir, sizeof saves_dir, "%s/SAVES", roms_dir);
	snprintf(bios_dir, sizeof bios_dir, "%s/BIOS", roms_dir);
	snprintf(pads_file, sizeof pads_file, "%s/pads.txt", roms_dir);
	snprintf(last_file, sizeof last_file, "%s/last.txt", roms_dir);
	test_k = getenv("NES_TEST_KEYS");
	if (getenv("NES_TEST_LOG"))
		log_fp = fopen(getenv("NES_TEST_LOG"), "w");
	if (getenv("NES_TEST_LANG")) set_en = !strcmp(getenv("NES_TEST_LANG"), "en");
	bool init = getpid() == 1;
	if (init)
		as_init();
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	screen_init();
	/* a splash while the rest comes up */
	fill(0, 0, LAND_W, LAND_H, C_BG);
	fill(220, 150, 360, 160, C_PANEL);
	fill(220, 150, 360, 3, C_ACCENT);
	text(256, 176, 6, C_ACCENT, "NES", 3);
	text(256, 250, 2, C_TEXT, T("emulator dla X6100", "emulator for the X6100"), 22);
	text(256, 276, 2, C_DIM, "FCEUmm", 22);
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
		const char *outs[3] = {getenv("NES_TEST_OUT"), getenv("NES_TEST_MENU_OUT"), getenv("NES_TEST_BT_OUT")};
		for (int k = 0; k < 3; k++) {
			if (k == 2) {                 /* the Bluetooth menu's look, Polish */
				set_en = 0;
				const char *items[] = {"Szukaj i paruj nowe", "Sparowane urzadzenia",
				                       "Bluetooth: wlaczony (wylacz)", "Wroc"};
				menu("Bluetooth", items, 4, 0, HINT_LIST_PL, false);
				const char *l[] = {"Klawiatura BT (klawiatura)", "Wpisz ten kod na klawiaturze i Enter:", "482917",
				                   "4 s   (F1/B: przerwij)"};
				panel("Parowanie", l, 4, 2);
			} else if (k) {                      /* then the menu over it: its look is checked too */
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
