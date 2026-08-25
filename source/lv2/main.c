#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <xenos/xenos.h>
#include <console/console.h>
#include <time/time.h>
#include <ppc/timebase.h>
#include <usb/usbmain.h>
#include <sys/iosupport.h>
#include <ppc/register.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xenon_nand/xenon_config.h>
#include <xenon_soc/xenon_secotp.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_soc/xenon_io.h>
#include <pci/io.h>	/* read32, for the GPU's PCI config space */
#include <xenon_sound/sound.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_smc/xenon_gpio.h>
#include <xb360/xb360.h>
#include <network/network.h>
#include <lwip/init.h>
#include <lwip/ip.h>
#include <httpd/httpd.h>
#include <diskio/ata.h>
#include <elf/elf.h>
#include <version.h>
#include <byteswap.h>

#include "config.h"
#include "file.h"
#include "discord.h"
#include "logo.h"

#ifndef NO_TFTP
#include "tftp/tftp.h"
#endif

#include "log.h"

static const char *consoleNames[] =
{
	"Xenon",
	"Zephyr",
	"Falcon",
	"Jasper",
	"Trinity",
	"Corona",
	"Corona MMC",
	"Winchester",
	"Winchester MMC",
};

/* The Xenos reports its framebuffer geometry in a fixed structure at
 * 0xec806100. libxenon reads it in console_init() but keeps the type private
 * to console.c, so the layout is repeated here rather than exported. */
struct ati_info
{
	uint32_t unknown1[4];
	uint32_t base;
	uint32_t unknown2[8];
	uint32_t width;
	uint32_t height;
} __attribute__ ((__packed__));

#define ATI_INFO ((const volatile struct ati_info *)0xec806100)

/* Screen palette, packed the way console_set_colors() wants it:
 * (b<<24)|(g<<16)|(r<<8). Charcoal reads softer than pure black on a large
 * display and gives the green/yellow/purple more to push against. */
#define COLOUR_BG 0x1C181800	/* RGB(24,24,28) */

/* Drop shadow for the logo: its own alpha mask drawn black and offset,
 * underneath the real thing. The Discord mark is left flat - at 19x14 a
 * shadow only muddies it - and the handle and temperatures are console text
 * on the 8x16 cell grid, where a few pixel offset isn't expressible. */
#define SHADOW_DX 3
#define SHADOW_DY 3

#define LOGO_MARGIN 0	/* pixels clear of the right edge of the safe area */

/* console_scroll32() shifts the whole framebuffer from row 0, in 32 pixel
 * tile blocks, but console_pset() can only address from offset_y down - so
 * anything a scroll drags above that margin can't be wiped. Starting a full
 * tile block down means a scrolled copy still lands at y >= 0 where the wipe
 * can reach it, instead of stranding a sliver at the top of the screen.
 *
 * That is the whole reason for the gap above the logo. Don't lower it. */
#define LOGO_TOP    32	/* pixels down from the top */

/* Right edge of the panel, in console_pset() coordinates. Deliberately not
 * console_pset_right(): that measures from pixel_max_x while text measures
 * from offset_x, and the two differ by the overscan margin - which would
 * leave the text sticking out past the logo by ~8 columns at 1080p. Text
 * column C starts at pixel C*8 in this system, so both line up exactly. */
static int panel_right(void)
{
	return console_get_cursor_max_x() * 8 - LOGO_MARGIN;
}

/* Column at which a field of len characters sits centred under the logo.
 * Text lands on 8 pixel boundaries so it can be a few pixels off dead
 * centre, which is invisible at this size. */
static int panel_col(int len)
{
	int left = panel_right() - LOGO_WIDTH;

	return (left + (LOGO_WIDTH - len * 8) / 2) / 8;
}

/* Alpha-blend one mask pixel of a solid colour over the background. */
static void blend_pset(int x, int y, unsigned int a, int r, int g, int b)
{
	unsigned int bg = console_color[0];
	int r0 = (bg >>  8) & 0xff;
	int g0 = (bg >> 16) & 0xff;
	int b0 = (bg >> 24) & 0xff;

	console_pset(x, y,
		     r0 + (r - r0) * (int)a / 255,
		     g0 + (g - g0) * (int)a / 255,
		     b0 + (b - b0) * (int)a / 255);
}

/* The four square Microsoft mark. Flat colours in a 2x2 grid, so it's four
 * filled rectangles rather than a bitmap - nothing to store, and it stays
 * crisp instead of picking up the antialiasing a scaled image would.
 *
 * 5px squares put it at 11px, which lands the bottom edge exactly on the text
 * baseline (the font's capitals occupy rows 2..11 of the 16px cell) with the
 * top a pixel above cap height, so it sits on the line rather than hanging
 * into the descender space below it. */
#define MSMARK_SQ   5	/* edge of one square, pixels */
#define MSMARK_GAP  1
#define MSMARK_SIZE (MSMARK_SQ * 2 + MSMARK_GAP)

static void draw_msmark(int x, int y)
{
	/* sampled from the reference artwork, not the 2012 brand palette */
	static const unsigned char square[4][3] =
	{
		{ 0xF1, 0x51, 0x1B },	/* top left     */
		{ 0x80, 0xCC, 0x28 },	/* top right    */
		{ 0x00, 0xAD, 0xEF },	/* bottom left  */
		{ 0xFB, 0xBC, 0x09 },	/* bottom right */
	};
	int i, sx, sy;

	for (i = 0; i < 4; i++)
	{
		int ox = (i & 1) ? MSMARK_SQ + MSMARK_GAP : 0;
		int oy = (i & 2) ? MSMARK_SQ + MSMARK_GAP : 0;

		for (sy = 0; sy < MSMARK_SQ; sy++)
			for (sx = 0; sx < MSMARK_SQ; sx++)
				console_pset(x + ox + sx, y + oy + sy,
					     square[i][0], square[i][1], square[i][2]);
	}
}

/* Two passes: the whole shadow first, then the mark on top. Drawing them per
 * pixel would let the shadow fall over parts of the mark already placed. */
void draw_logo()
{
	int left = panel_right() - LOGO_WIDTH;
	unsigned int x, y;
	int pass;

	for (pass = 0; pass < 2; pass++)
	{
		for (y = 0; y < LOGO_HEIGHT; y++)
		{
			for (x = 0; x < LOGO_WIDTH; x++)
			{
				unsigned int a = logo_alpha[y * LOGO_WIDTH + x];

				if (!a)
					continue; /* transparent, leave the background */

				if (pass == 0)
					blend_pset(left + x + SHADOW_DX,
						   LOGO_TOP + y + SHADOW_DY,
						   a, 0, 0, 0);
				else
					blend_pset(left + x, LOGO_TOP + y,
						   a, 255, 255, 255);
			}
		}
	}
}

/* Right hand panel, stacked under the logo. The logo's last pixel row is
 * LOGO_TOP + LOGO_HEIGHT, so row 8 (pixels 128+) is the first text row clear
 * of it. TEMPS_WIDTH covers "EDRAM 40.0C" plus a column of slack. */
#define DISCORD_TEXT "@socioculture"
#define DISCORD_ROW  9
#define TEMPS_ROW    11
#define TEMPS_LINES  4
#define TEMPS_WIDTH  11

/* The band the panel owns, in pixels from the top. */
#define LOGO_BAND_H ((TEMPS_ROW + TEMPS_LINES) * 16)

static void draw_discord(void);
static void draw_temperatures(void);

/* Set once the console has scrolled, after which any row we memorised points
 * at something else and must not be written to. */
static int screen_scrolled;

/* The console scrolls the whole framebuffer, logo included, so once output
 * reaches the bottom the logo would crawl off the top. Wipe the band it lives
 * in - which also clears whatever a scroll dragged up above it - and draw it
 * again at home. Nothing else uses these columns, so the wipe is safe. */
static void redraw_logo(void)
{
	unsigned int bg = console_color[0];
	unsigned int r0 = (bg >>  8) & 0xff;
	unsigned int g0 = (bg >> 16) & 0xff;
	unsigned int b0 = (bg >> 24) & 0xff;
	int left = panel_right() - LOGO_WIDTH;
	unsigned int x, y;

	/* widened by the shadow offset so its edge gets cleared too */
	for (y = 0; y < LOGO_BAND_H; y++)
		for (x = 0; x < LOGO_WIDTH + SHADOW_DX; x++)
			console_pset(left + x, y, r0, g0, b0);

	draw_logo();
	draw_discord();
	draw_temperatures();
}

/* Called every pass of the main loop. A cursor that moved up means
 * console_scroll32() ran; near the bottom of the screen any movement at all
 * means scrolling is in progress. Idle screens redraw nothing, so there's no
 * flicker while XeLL is just sitting there. */
static void keep_logo_in_place(void)
{
	static int last_y = -1;
	int max_y = console_get_cursor_max_y();
	int y = console_get_cursor_y();

	if (last_y >= 0 && y < last_y)
		screen_scrolled = 1; /* memorised rows are meaningless from here */

	if (last_y >= 0 && y != last_y && (y < last_y || y >= max_y - 3))
		redraw_logo();

	last_y = y;
}

/* defined below, next to the rest of the screen helpers */
static void print_coloured(unsigned int colour, const char *s);

/* The "Network init" line is printed while DHCP is still in flight, so it
 * would otherwise sit at "requested" forever. Remember its row and rewrite it
 * once we know the outcome. Padded, so a shorter result can't leave the tail
 * of the longer one behind. */
static int netstatus_row = -1;

static void set_network_status(const char *state, unsigned int colour)
{
	int x, y, i;

	if (netstatus_row < 0 || screen_scrolled)
		return;

	x = console_get_cursor_x();
	y = console_get_cursor_y();

	console_set_cursor(0, netstatus_row);
	printf("   Network init... ");
	print_coloured(colour, state);
	for (i = strlen(state); i < 32; i++)
		printf(" ");

	console_set_cursor(x, y);
	netstatus_row = -1; /* reported, nothing more to say */
}

/* Printed once, after DHCP has settled, so it sits below the network config
 * instead of above it. Scanning starts as soon as the main loop does either
 * way - this is just the banner. */
static void announce_scan(void)
{
	static int announced;

	if (announced)
		return;
	announced = 1;

#ifndef NO_TFTP
	printf("\nLooking for files on TFTP and local media...\n\n");
#else
	printf("\nLooking for files on local media...\n\n");
#endif
}

void dumpana() {
	int i;
	for (i = 0; i < 0x100; ++i)
	{
		uint32_t v;
		xenon_smc_ana_read(i, &v);
		printf("0x%08x, ", (unsigned int)v);
		if ((i&0x7)==0x7)
			printf(" // %02x\n", (unsigned int)(i &~0x7));
	}
}

char FUSES[350]; /* this string stores the ascii dump of the fuses */

unsigned char stacks[6][0x10000];

#ifndef NO_NETWORKING
/* libxenon's network_init() sits in a 15 second busy loop waiting for a DHCP
 * lease before it returns. We don't want that wait standing between the user
 * and the fuses/keys, so XeLL brings the interface up itself, fires off the
 * DHCP request and carries on booting. network_dhcp_poll() finishes the
 * handshake later on and falls back to the same static address libxenon would
 * have picked, so httpd/tftp/kboot.conf all behave as before. */

/* lives in libxenon, the same init callback network_init() hands to lwip */
extern err_t enet_init(struct netif *netif);

/* how long DHCP gets before we give up on it, matching libxenon (60 * 250ms) */
#define DHCP_TIMEOUT_MSEC 15000

/* one poll slice, short enough to be invisible but long enough for the
 * handshake to step forward every time we drop by */
#define DHCP_POLL_SLICE_MSEC 20

static uint64_t dhcp_started;
static int dhcp_settled;

static int network_start(void)
{
	ip_addr_t ipaddr, netmask, gateway;

	printf(" * initializing lwip 1.4.1...\n");

	IP4_ADDR(&netmask, 255, 255, 255, 255);
	IP4_ADDR(&gateway, 0, 0, 0, 0);
	IP4_ADDR(&ipaddr, 0, 0, 0, 0);

	lwip_init();

	printf(" * initializing NIC\n");
	if (!netif_add(&netif, &ipaddr, &netmask, &gateway, NULL, enet_init, ip_input)){
		printf(" ! netif_add failed!\n");
		dhcp_settled = 1;
		return NETWORK_INIT_FAILURE;
	}
	netif_set_default(&netif);

	printf(" * requesting dhcp in the background\n");
	dhcp_start(&netif);
	dhcp_started = mftb();

	return NETWORK_INIT_SUCCESS;
}

/* libxenon's network_print_config() labels itself " * network config:", the one
 * line on screen that doesn't match the casing of everything else. Same
 * information, printed to match. */
#define IP_OCTETS(ip) (int)(((ip).addr >> 24) & 0xff), (int)(((ip).addr >> 16) & 0xff), \
		      (int)(((ip).addr >>  8) & 0xff), (int)( (ip).addr        & 0xff)

static void print_network_config(void)
{
	printf("Network Config: %d.%d.%d.%d / %d.%d.%d.%d / %02X:%02X:%02X:%02X:%02X:%02X\n",
		IP_OCTETS(netif.ip_addr), IP_OCTETS(netif.netmask),
		netif.hwaddr[0], netif.hwaddr[1], netif.hwaddr[2],
		netif.hwaddr[3], netif.hwaddr[4], netif.hwaddr[5]);
}

static void network_dhcp_poll(void)
{
	ip_addr_t ipaddr, netmask, gateway;
	uint64_t slice;

	if (dhcp_settled)
		return;

	/* give lwip a slice so the handshake can make progress */
	slice = mftb();
	do {
		network_poll();
	} while (netif.ip_addr.addr == 0 &&
		 tb_diff_msec(mftb(), slice) < DHCP_POLL_SLICE_MSEC);

	if (netif.ip_addr.addr){
		dhcp_settled = 1;
		console_clrline();
		set_network_status("DHCP lease acquired", CONSOLE_SUCCESS);
#ifndef NO_PRINT_CONFIG
		print_network_config();
#endif
		return;
	}

	if (tb_diff_msec(mftb(), dhcp_started) < DHCP_TIMEOUT_MSEC)
		return;

	dhcp_settled = 1;
	console_clrline();
	set_network_status("no DHCP, static IP assigned", CONSOLE_WARN);

	IP4_ADDR(&ipaddr, 192, 168, 1, 99);
	IP4_ADDR(&gateway, 192, 168, 1, 1);
	IP4_ADDR(&netmask, 255, 255, 255, 0);
	netif_set_addr(&netif, &ipaddr, &netmask, &gateway);
	netif_set_up(&netif);
#ifndef NO_PRINT_CONFIG
	print_network_config();
#endif
}
#endif

/* libxenon's print_cpu_dvd_keys() draws every line in the one console colour,
 * so XeLL prints these itself instead - same lines and failure messages, plus
 * the extra keyvault fields, with the cpu key's digits in green. Everything
 * here goes through kv_read()/kv_get_key(), both declared in xb360.h. */

/* Die codename per console generation, for the processor line. The console
 * type itself is detected; this table just names the silicon that goes with
 * it. Vejle is the 45nm XCGPU's real name - "Valhalla" was only its working
 * name before the chip was finalised, though it stuck in a lot of places. */
/* GPU die per console generation. Two of these are approximations libxenon
 * can't resolve: Zephyr_C shipped Rhea while earlier Zephyrs were Xenos, and
 * July 2009 Jaspers and every Tonasket carry Kronos rather than Zeus - both
 * report identically here. From Trinity on the GPU shares a die with the CPU,
 * so these match cpuNames. */
static const char *gpuNames[] =
{
	"Xenos",	/* Xenon */
	"Xenos",	/* Zephyr        - Rhea on Zephyr_C */
	"Rhea",		/* Falcon */
	"Zeus/Kronos",	/* Jasper - see dieNodes, the two are indistinguishable */
	"Vejle",	/* Trinity */
	"Vejle",	/* Corona */
	"Vejle",	/* Corona MMC */
	"Oban",		/* Winchester */
	"Oban",		/* Winchester MMC */
};

/* Process node per die, in nanometres, following the console generation. No
 * register reports this - it follows from the board revision, the same way
 * the die codenames do. eDRAM is 10MB on every console ever made; only the
 * node it was fabbed on changed.
 *
 * Jasper's eDRAM is the one soft entry: 80nm normally, but the Kronos boards
 * (July 2009 Jaspers and every Tonasket) carry the 65nm Styx-65, and those
 * report identically to libxenon. */
static const struct
{
	int cpu;
	int gpu;
	const char *edram;
} dieNodes[] =
{
	{ 90, 90, "90nm" },	/* Xenon */
	{ 90, 90, "80nm" },	/* Zephyr */
	{ 65, 80, "80nm" },	/* Falcon */
	{ 65, 65, "80/65nm" },	/* Jasper - Styx-80 on Zeus, Styx-65 on Kronos */
	{ 45, 45, "65nm" },	/* Trinity */
	{ 45, 45, "65nm" },	/* Corona */
	{ 45, 45, "65nm" },	/* Corona MMC */
	{ 45, 45, "65nm" },	/* Winchester */
	{ 45, 45, "65nm" },	/* Winchester MMC */
};

/* " (65nm)", or nothing at all when the console type isn't one we know. One
 * static buffer, so one call per printf. */
static const char *die_node(int type, int which)
{
	static char buf[16];

	if (type < 0 || type > 8)
		return "";

	if (which == 2)
		sprintf(buf, " (%s)", dieNodes[type].edram);
	else
		sprintf(buf, " (%dnm)",
			(which == 0) ? dieNodes[type].cpu : dieNodes[type].gpu);

	return buf;
}

static const char *cpuNames[] =
{
	"Waternoose",	/* Xenon         - 90nm CPU */
	"Waternoose",	/* Zephyr        - 90nm CPU */
	"Loki",		/* Falcon        - 65nm CPU */
	"Loki",		/* Jasper        - 65nm CPU */
	"Vejle",	/* Trinity       - 45nm XCGPU */
	"Vejle",	/* Corona        - 45nm XCGPU */
	"Vejle",	/* Corona MMC    - 45nm XCGPU */
	"Oban",		/* Winchester    - later XCGPU */
	"Oban",		/* Winchester MMC- later XCGPU */
};

/* CONSOLE_COLOR_GREY is RGB(192,192,192) - against white text on black that
 * reads as white. This is dim enough to actually look absent. Packed the way
 * console_set_colors() wants it: (b<<24)|(g<<16)|(r<<8). */
#define COLOUR_DIM 0x70707000	/* RGB(112,112,112) */

/* Print s in colour, then put the console back how we found it. */
static void print_coloured(unsigned int colour, const char *s)
{
	unsigned int bg = console_color[0], fg = console_color[1];

	console_set_colors(bg, colour);
	printf("%s", s);
	console_set_colors(bg, fg);
}

static void status_line(const char *label, const char *state, unsigned int colour)
{
	printf("   %s... ", label);
	print_coloured(colour, state);
	printf("\n");
}

/* Probe the eDRAM's own ID register.
 *
 * Zeus and Kronos are the same GPU die - only the eDRAM daughter die differs
 * (Styx-80 vs Styx-65), so no PCI id or revision separates them. The eDRAM
 * does identify itself though: libxenon reads register 0x2000 in
 * edram_init_state1() and keeps it in edram_id/edram_rev.
 *
 * We can't call that function - it configures the eDRAM, branches on state
 * XeLL doesn't set up, and abort()s on failure, all while the console is live
 * on the same GPU. But the read itself is just a register-indirect access
 * through plain MMIO, so it's replicated here read-only: no configuration
 * writes, and every wait is bounded so a console that doesn't answer times
 * out instead of hanging the boot.
 *
 * Whether 0x2000 reads meaningfully without libxenon's configuration writes
 * is the open question - hence displaying the raw value rather than acting
 * on it. */
/* Plain MMIO accessors, write32n/read32n against 0xec800000 + reg. Non-static
 * in libxenon's xenos.c but absent from xenos.h, so declared here. */
extern void xenos_write32(int reg, uint32_t val);
extern uint32_t xenos_read32(int reg);

#define EDRAM_SPIN 100000

static int edram_idle(void)
{
	int spin = EDRAM_SPIN;

	while (xenos_read32(0x3c4c))
		if (--spin <= 0)
			return 0; /* never went idle */

	return 1;
}

static int edram_probe_id(uint32_t *out)
{
	uint32_t res;

	if (!edram_idle())
		return 0;

	xenos_write32(0x3c44, 0x2000);
	if (!edram_idle())
		return 0;

	res = xenos_read32(0x3c48);
	if (!edram_idle())
		return 0;

	*out = res;
	return 1;
}

/* libxenon exposes the PCI bridge's revision but not the GPU's, though both
 * sit in the same config layout: it reads the low byte of offset 0x08 at the
 * bridge's base 0xd0000000, so the same read at the GPU's base 0xd0010000 -
 * the one xenon_get_XenosID() uses - gives the Xenos die revision. */
static unsigned int xenos_revision(void)
{
	return read32(0xd0010008) & 0xff;
}

/* AV pack IDs as libxenon's own xenos_autoset_mode() reads them - it switches
 * on exactly these values to pick a video mode, so the meanings come from
 * there rather than from guesswork. Anything unlisted prints its raw byte. */
static const char *avpack_name(int avpack)
{
	switch (avpack)
	{
	case 0x13:
	case 0x14:
	case 0x1C:
	case 0x1E:
	case 0x1F: return "HDMI";
	case 0x1B:
	case 0x59:
	case 0x5B: return "VGA";
	case 0x0C:
	case 0x0F: return "Component";
	case 0x4F: return "Composite HD";
	case 0x43:
	case 0x57: return "Composite";
	case 0x54: return "Composite + S-Video";
	case 0x47: return "SCART";
	}

	return NULL;
}

static const char *av_region_name(int region)
{
	switch (region)
	{
	case AVREGION_NTSCM: return "NTSC-M";
	case AVREGION_NTSCJ: return "NTSC-J";
	case AVREGION_PAL50: return "PAL-50";
	case AVREGION_PAL60: return "PAL-60";
	}

	return "Invalid";
}

static const char *nand_type_name(int meta_type)
{
	switch (meta_type)
	{
	case META_TYPE_0: return "small block";
	case META_TYPE_1: return "Jasper 16MB";
	case META_TYPE_2: return "large block";
	}

	return "unknown";
}

/* SMC message 0x07 answers with four sensors. The conventional order is
 * CPU, GPU, EDRAM, board, each fixed point with degrees in the high byte
 * and a 1/256 fraction in the low byte.
 *
 * These live directly under the logo, stacked one per line and right aligned
 * with it, rather than inline with the boot text. That means they sit at a
 * fixed spot on screen instead of on a row the console can scroll away, so
 * they keep updating no matter how much text goes past. Fixed width, so each
 * redraw covers the previous reading exactly. */
/* Sits directly under the logo, above the temperatures. The mark is a solid
 * shape stored as an alpha mask like the logo, tinted to the same purple as
 * the handle beside it, and nudged down so its 14 rows centre in the 16 pixel
 * text row. */
#define PURPLE_R ((CONSOLE_COLOR_PURPLE >>  8) & 0xff)
#define PURPLE_G ((CONSOLE_COLOR_PURPLE >> 16) & 0xff)
#define PURPLE_B ((CONSOLE_COLOR_PURPLE >> 24) & 0xff)
#define DISCORD_GAP 4	/* pixels between the mark and the handle */

static void draw_discord(void)
{
	int len = (int)strlen(DISCORD_TEXT);
	int left = panel_right() - LOGO_WIDTH;
	int combo = DISCORD_WIDTH + DISCORD_GAP + len * 8;
	/* centre the mark and handle together, then hang the mark off the text */
	int col = (left + (LOGO_WIDTH - combo) / 2 + DISCORD_WIDTH + DISCORD_GAP) / 8;
	int iconx = col * 8 - DISCORD_GAP - DISCORD_WIDTH;
	int icony = DISCORD_ROW * 16 + (16 - DISCORD_HEIGHT) / 2;
	int x = console_get_cursor_x();
	int y = console_get_cursor_y();
	unsigned int ix, iy;

	/* no shadow on the mark - it's small enough that one would just muddy it */
	for (iy = 0; iy < DISCORD_HEIGHT; iy++)
	{
		for (ix = 0; ix < DISCORD_WIDTH; ix++)
		{
			unsigned int a = discord_alpha[iy * DISCORD_WIDTH + ix];

			if (!a)
				continue;

			blend_pset(iconx + ix, icony + iy, a,
				   PURPLE_R, PURPLE_G, PURPLE_B);
		}
	}

	console_set_cursor(col, DISCORD_ROW);
	print_coloured(CONSOLE_COLOR_PURPLE, DISCORD_TEXT);

	console_set_cursor(x, y);
}

/* Degrees C at which a reading stops being green. Green up to 68, yellow
 * from 69, red from 80. */
#define TEMP_WARN 69
#define TEMP_HOT  80

static const char *sensorNames[TEMPS_LINES] = { "CPU", "GPU", "EDRAM", "MB" };

static void draw_temperatures(void)
{
	uint16_t sensor[TEMPS_LINES];
	int col = panel_col(TEMPS_WIDTH);
	int x = console_get_cursor_x();
	int y = console_get_cursor_y();
	int i;

	xenon_smc_query_sensors(sensor);

	for (i = 0; i < TEMPS_LINES; i++)
	{
		int deg = sensor[i] >> 8;
		unsigned int colour;
		char value[8];

		if (deg >= TEMP_HOT)
			colour = CONSOLE_COLOR_RED;
		else if (deg >= TEMP_WARN)
			colour = CONSOLE_WARN;
		else
			colour = CONSOLE_SUCCESS;

		sprintf(value, "%2d.%01dC", deg, ((sensor[i] & 0xff) * 10) / 256);

		console_set_cursor(col, TEMPS_ROW + i);
		printf("%-5s ", sensorNames[i]);
		print_coloured(colour, value);
	}

	console_set_cursor(x, y);
}

static void update_temperatures(void)
{
	static uint64_t last_update;

	if (tb_diff_msec(mftb(), last_update) < 2000)
		return;
	last_update = mftb();

	draw_temperatures();
}

/* Fuses blow four bits at a time, so a loader data version is the number of
 * blown nibbles: fuseline 2 carries the CB LDV, lines 7-11 the CF/CG LDV. */
static int fuse_ldv(const u64 *fuseline, int first, int last)
{
	int i, bits = 0;

	for (i = first; i <= last; i++)
	{
		u64 v = fuseline[i];

		while (v)
		{
			bits += v & 1;
			v >>= 1;
		}
	}

	return bits / 4;
}

/* ATA model strings come back space padded. */
static const char *ata_model(struct xenon_ata_device *dev)
{
	static char buf[sizeof(dev->model) + 1];
	int n;

	memcpy(buf, dev->model, sizeof(dev->model));
	buf[sizeof(dev->model)] = '\0';

	/* libxenon terminates at 40 chars for ata and 24 for atapi, so trim back
	 * from the real string end - anything past it is uninitialised. */
	for (n = strlen(buf) - 1; n >= 0 && buf[n] == ' '; n--)
		buf[n] = '\0';

	return buf;
}

static void detect_line(const char *label, int present, struct xenon_ata_device *dev)
{
	printf("   %s... ", label);

	if (present)
		printf("%s\n", ata_model(dev));
	else
	{
		print_coloured(COLOUR_DIM, "None");
		printf("\n");
	}
}

static void print_key_green(char *name, unsigned char *data)
{
	unsigned int bg = console_color[0], fg = console_color[1];
	int i;

	printf("%s: ", name);

	console_set_colors(bg, CONSOLE_COLOR_GREEN);
	for (i = 0; i < 16; i++)
		printf("%02X", data[i]);
	console_set_colors(bg, fg);

	printf("\n");
}

/* Keyvault fields that are plain ascii. kv_get_key() insists the caller's
 * length already matches the table entry, so len has to be exact. */
static void print_kv_ascii(const char *label, unsigned char keyid, int len,
			   unsigned char *kv)
{
	unsigned char buf[0x20];
	char out[0x21];
	int n = len, i;

	if (len >= (int)sizeof(buf))
		return;

	memset(buf, '\0', sizeof(buf));
	if (kv_get_key(keyid, buf, &n, kv) != 0)
		return;

	/* These are fixed length fields, not necessarily terminated, and a blank
	 * one is padded with nulls or spaces rather than being absent. Build a
	 * printable copy instead of trusting %s to stop somewhere sensible. */
	for (i = 0; i < len; i++)
		out[i] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : ' ';
	out[len] = '\0';

	for (i = len - 1; i >= 0 && out[i] == ' '; i--)
		out[i] = '\0';

	/* A serial is plain alphanumerics. Blank fields, padding, and the junk
	 * some consoles carry here are all worth nothing on screen, so drop the
	 * whole line rather than printing a placeholder. */
	if (out[0] == '\0')
		return;

	for (i = 0; out[i]; i++)
		if (!((out[i] >= '0' && out[i] <= '9') ||
		      (out[i] >= 'A' && out[i] <= 'Z') ||
		      (out[i] >= 'a' && out[i] <= 'z')))
			return;

	printf("%s: %s\n", label, out);
}

/* The manufacturing date lives inside the console certificate, past CertSize,
 * ConsoleId, ConsolePartNumber, Reserved, Privileges and ConsoleType - so
 * eight ascii digits at cert offset 0x1C. Only printed if it really is
 * digits, so a wrong offset shows nothing rather than garbage. */
static void print_mfg_date(unsigned char *kv)
{
	unsigned char cert[0x1A8];
	int n = sizeof(cert);
	unsigned char *d;
	int i, digits = 0, printable = 0;

	if (kv_get_key(XEKEY_CONSOLE_CERTIFICATE, cert, &n, kv) != 0)
		return;

	d = &cert[0x1C]; /* ManufacturingDate, 8 bytes */

	for (i = 0; i < 8 && !d[i]; i++)
		;
	if (i == 8)
		return; /* field never programmed */

	for (i = 0; i < 8; i++)
	{
		if (d[i] >= '0' && d[i] <= '9')
			digits++;
		if (d[i] >= 0x20 && d[i] < 0x7f)
			printable++;
	}

	/* Plain YYYYMMDD is the documented form, but not every console carries
	 * it that way, so fall back to showing the bytes rather than silently
	 * dropping the line. */
	if (digits == 8)
		printf("   * Mfg Date: %c%c%c%c-%c%c-%c%c\n",
			d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
	else if (printable == 8)
		printf("   * Mfg Date: %.8s\n", (char *)d);
	else
		printf("   * Mfg Date: %02X%02X%02X%02X%02X%02X%02X%02X\n",
			d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

static void print_console_keys(void)
{
	unsigned char key[0x10];
	unsigned char region[0x02];
	unsigned char *kv;
	int n, r;

	printf("\n");

	memset(key, '\0', sizeof(key));
	if (cpu_get_key(key) == 0)
		print_key_green("   * CPU Key", key);

	if (xenon_logical_nand_data_ok() != 0)
	{
		print_coloured(CONSOLE_ERR, "   ! Unable to read Keyvault data from NAND\n");
		print_coloured(CONSOLE_ERR, "   ! xenon_logical_nand_data_ok error\n");
		printf("\n");
		return;
	}

	if (KV_FLASH_OFFSET == 0 || KV_FLASH_SIZE == 0)
	{
		print_coloured(CONSOLE_ERR, "   ! Unable to read Keyvault data from NAND\n");
		print_coloured(CONSOLE_ERR, "   ! Keyvault size or offset is zero\n");
		printf("\n");
		return;
	}

	memset(key, '\0', sizeof(key));
	if (get_virtual_cpukey(key) == 0)
		print_key("   * Virtual CPU Key", key);

	/* Decrypt the keyvault once and pull every field from the same buffer.
	 * kv_read() is an RC4 pass plus an HMAC check, and the libxenon helpers
	 * each do their own, so this is several of those saved. */
	kv = malloc(KV_FLASH_SIZE);
	if (kv == NULL)
	{
		print_coloured(CONSOLE_ERR, "   ! Out of memory reading the keyvault\n");
		printf("\n");
		return;
	}

	memset(key, '\0', sizeof(key));
	r = kv_read(kv, 0);
	if (r == 2 && get_virtual_cpukey(key) == 0)
		r = kv_read(kv, 1); /* retry against the virtual fuses */

	if (r != 0)
	{
		print_coloured(CONSOLE_ERR, "   ! Unable to decrypt the keyvault\n");
		free(kv);
		printf("\n");
		return;
	}

	memset(key, '\0', sizeof(key));
	n = sizeof(key);
	if (kv_get_key(XEKEY_DVD_KEY, key, &n, kv) == 0)
		print_key("   * DVD Key", key);

	print_kv_ascii("   * Serial", XEKEY_CONSOLE_SERIAL_NUMBER, 0x0C, kv);
	print_kv_ascii("   * Mobo Serial", XEKEY_MOBO_SERIAL_NUMBER, 0x0C, kv);
	print_mfg_date(kv);

	n = sizeof(region);
	if (kv_get_key(XEKEY_GAME_REGION, region, &n, kv) == 0)
		printf("   * Game Region: %04X\n", (region[0] << 8) | region[1]);

	free(kv);

	printf("\n");
}

void reset_timebase_task()
{
	mtspr(284,0); // TBLW
	mtspr(285,0); // TBUW
	mtspr(284,0);
}

void synchronize_timebases()
{
	xenon_thread_startup();
	
	std((void*)0x200611a0,0); // stop timebase
	
	int i;
	for(i=1;i<6;++i){
		xenon_run_thread_task(i,&stacks[i][0xff00],(void *)reset_timebase_task);
		while(xenon_is_thread_task_running(i));
	}
	
	reset_timebase_task(); // don't forget thread 0
			
	std((void*)0x200611a0,0x1ff); // restart timebase
}
	
/* how the nand came up - reported after the fuses and keys, not while it runs */
#define NAND_SKIPPED 0
#define NAND_OK      1
#define NAND_FAILED  2

int main(){
	LogInit();
	int i;
	int consoleType = 0;
	int nandStatus = NAND_SKIPPED;
	int ataPresent = 0, atapiPresent = 0;
	int procRow;
#ifndef NO_NETWORKING
	int netStatus = NETWORK_INIT_FAILURE;
#endif

	printf("ANA Dump before Init:\n");
	dumpana();

	// linux needs this
	synchronize_timebases();
	
	// irqs preinit (SMC related)
	*(volatile uint32_t*)0xea00106c = 0x1000000;
	*(volatile uint32_t*)0xea001064 = 0x10;
	*(volatile uint32_t*)0xea00105c = 0xc000000;

	// Reset the ROL state in case it is corrupted by
	// ARGON_DATA JTAG, a rogue libxenon app, etc.
	xenon_smc_set_led(0, 0);

	xenon_smc_start_bootanim();

	// flush console after each outputted char
	setbuf(stdout,NULL);

	xenos_init(VIDEO_MODE_AUTO);

	printf("ANA Dump after Init:\n");
	dumpana();

#ifdef SWIZZY_THEME
	console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_ORANGE); // Orange text on black bg
#elif defined XTUDO_THEME
	console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_PINK); // Pink text on black bg
#elif defined DEFAULT_THEME
	console_set_colors(COLOUR_BG,CONSOLE_COLOR_WHITE); // White text on charcoal
#else
	console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_GREEN); // Green text on black bg
#endif
	console_init();

	/* console_init() announces the framebuffer the instant it arms the screen
	 * hook, so there's no muting it - wipe the screen instead. clrscr also
	 * puts the cursor back to 0,0, so the splash starts at the top. */
	console_clrscr();
	redraw_logo(); /* paints the backdrop, then everything on it */

	/* The git rev is the only thing that tells you which build actually
	 * booted, which matters when reflashing repeatedly. RELEASE carries the
	 * Makefile's quoting when the repo has no tags, so use GITREV directly. */
	printf("XeLL git-" GITREV " - Copyright (C) 2007-2026 LibXenon.org, Free60.org, Et al.\n\n");

	/* build details go to the log and the uart, not the screen */
	console_close();
	printf("XeLL - Xenon linux loader second stage " LONGVERSION "\n");
	printf("Built with GCC " GCC_VERSION " and Binutils " BINUTILS_VERSION " \n");
	console_open();

	//delay(3); //give the user a chance to see our splash screen <- network init should last long enough...

	xenon_sound_init();

	/* xenon_make_it_faster()/xenon_set_speed() natter about VIDs and cores
	 * waking back up. Drop the screen hook while they run so none of it shows
	 * - the work still happens, and the log still records it. */
	console_close();
	xenon_make_it_faster(XENON_SPEED_FULL);
	console_open();

	/* The nand has to come up before the keyvault can be read, so it runs
	 * here - silently - and gets reported further down with the rest of the
	 * init results, after the fuses and keys the user actually came for. */
	if (xenon_get_console_type() != REV_CORONA_PHISON) //Not needed for MMC type of consoles! ;)
	{
		console_close();
		printf(" * nand init\n");
		sfcx_init();
		console_open();

		nandStatus = (sfc.initialized == SFCX_INITIALIZED) ? NAND_OK : NAND_FAILED;

		if (sfc.initialized != SFCX_INITIALIZED)
		{
			print_coloured(CONSOLE_ERR, " ! sfcx initialization failure\n");
			print_coloured(CONSOLE_ERR, " ! nand related features will not be available\n");
			delay(5);
		}
	}

	xenon_config_init();

	/* Everything worth writing down is readable as soon as the NAND is up, so
	 * print it before we go anywhere near the network - no waiting on a DHCP
	 * server to see your fuses, cpu key, serial and dvd key. */

	/* display some cpu info */
	consoleType = xenon_get_console_type();

	/* There's no register that reports the core clock, so take it from
	 * libxenon's own timebase constant (the timebase runs at core/64) rather
	 * than hardcoding a number here. */
	printf("\n");

	/* Leading spaces make room for the four square mark, which is wider than
	 * the 8px cell a single space would give it. */
	procRow = console_get_cursor_y();

	printf("  Microsoft %s%s %08x %u.%03uGHz Processor\n",
			 (consoleType >= 0 && consoleType <= 8) ? cpuNames[consoleType] : "Unknown",
			 die_node(consoleType, 0),
			 mfspr(287),
			 (unsigned int)((PPC_TIMEBASE_FREQ * 64) / 1000000000LL),
			 (unsigned int)(((PPC_TIMEBASE_FREQ * 64) / 1000000LL) % 1000));

	/* bottom aligned to the text baseline at row 11 */
	draw_msmark(0, procRow * 16 + 1);

	printf("Console Type: %s - %s%s\n",
			 (consoleType >= 0 && consoleType <= 8) ? consoleNames[consoleType] : "Unknown",
			 (consoleType >= 0 && consoleType <= 8) ? gpuNames[consoleType] : "Unknown",
			 die_node(consoleType, 1));

	/* The three IDs libxenon narrows the board down with. Board revisions that
	 * share silicon (Corona / Waitsburg / Stingray) read identically here;
	 * Tonasket differs from Jasper only by its Kronos GPU, so the Xenos ID is
	 * what would tell them apart. */
	printf("GPU ID: %04x rev %02x   PCI Bridge: %02x   DVE: %02x   Video: %ux%u%s\n",
			 xenon_get_XenosID(),
			 xenos_revision(),
			 xenon_get_PCIBridgeRevisionID(),
			 xenon_get_DVE(),
			 (unsigned int)ATI_INFO->width,
			 (unsigned int)ATI_INFO->height,
			 xenos_is_overscan() ? " overscan" : "");

	{
		int avpack = xenon_smc_read_avpack();
		const char *avname = avpack_name(avpack);

		printf("AV Region: %s   AV Pack: ",
			 av_region_name(xenon_config_get_avregion()));

		if (avname)
			printf("%s\n\n", avname);
		else
			printf("%02X\n\n", avpack); /* unknown, show the raw id */
	}

#ifndef NO_PRINT_CONFIG
	printf("Fuses:\n");
	u64 fuseline[12];
	char *fusestr = FUSES;
	for (i=0; i<12; ++i){
		unsigned int hi,lo;

		fuseline[i]=xenon_secotp_read_line(i);
		hi=fuseline[i]>>32;
		lo=fuseline[i]&0xffffffff;

		/* the flat one-per-line dump is what the httpd /FUSE download hands
		 * out, so keep building it exactly as before */
		fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
	}

	/* on screen they go two columns wide, 0-5 on the left, 6-11 on the right */
	for (i=0; i<6; ++i){
		printf("   %02d: %08x%08x     %02d: %08x%08x\n",
			i,
			(unsigned int)(fuseline[i]>>32),
			(unsigned int)(fuseline[i]&0xffffffff),
			i+6,
			(unsigned int)(fuseline[i+6]>>32),
			(unsigned int)(fuseline[i+6]&0xffffffff));
	}

	printf("\n   * LDV: CB %d / CF-CG %d\n",
		fuse_ldv(fuseline, 2, 2), fuse_ldv(fuseline, 7, 11));

	print_console_keys();
#endif

	/* Bring the drivers up with the screen hook off - all of it, lwip, the
	 * PHY, USB enumeration and the ATA probes, is noise. We keep the results
	 * and report them tidily below. The log still records the raw output. */
	console_close();

#ifndef NO_NETWORKING
	printf(" * network init\n");
	netStatus = network_start();

	printf(" * starting httpd server...");
	httpd_start();
	printf("success\n");
#endif

	printf(" * usb init\n");
	usb_init();
	usb_do_poll();

	// FIXME: Not initializing these devices here causes an interrupt storm in
	// linux.
	printf(" * sata hdd init\n");
	ataPresent = (xenon_ata_init() == 0);

#ifndef NO_DVD
	printf(" * sata dvd init\n");
	atapiPresent = (xenon_atapi_init() == 0);
#endif

	mount_all_devices();

	/* back on screen for the dhcp result and the file scan */
	console_open();

	/* Read from the host bridge register HWINIT fills in, so this reflects
	 * what's actually installed rather than assuming the stock 512MB. */
	/* eDRAM is 10MB on every console; only the node it was fabbed on moved */
	printf("   Memory: %uK   eDRAM: 10MB%s",
		xenon_get_ram_size() / 1024, die_node(consoleType, 2));

	{
		uint32_t edramId;

		if (edram_probe_id(&edramId))
			printf("  ID: %08X", (unsigned int)edramId);
	}

	printf("\n");

	if (sfc.initialized == SFCX_INITIALIZED)
		printf("   NAND: %dMB (%s)\n",
			sfc.size_mb, nand_type_name(sfc.meta_type));

	draw_temperatures(); /* drawn under the logo, not inline */
	printf("\n");

	detect_line("Storage", ataPresent, &ata);
	detect_line("Disc Drive", atapiPresent, &atapi);

	/* how the bring-up went */
	printf("\n");
	switch (nandStatus)
	{
	case NAND_OK:
		status_line("NAND init", "success", CONSOLE_SUCCESS);
		break;
	case NAND_FAILED:
		status_line("NAND init", "failed", CONSOLE_ERR);
		break;
	default:
		status_line("NAND init", "MMC console, skipped", CONSOLE_WARN);
		break;
	}

#ifndef NO_NETWORKING
	/* remembered so set_network_status() can rewrite it when DHCP settles */
	netstatus_row = console_get_cursor_y();

	if (netif.ip_addr.addr)
		status_line("Network init", "DHCP lease acquired", CONSOLE_SUCCESS);
	else if (netStatus == NETWORK_INIT_SUCCESS)
		status_line("Network init", "DHCP requested", CONSOLE_WARN);
	else
		status_line("Network init", "failed", CONSOLE_ERR);

	status_line("HTTPD init", "success", CONSOLE_SUCCESS);
#endif

	status_line("USB init", "success", CONSOLE_SUCCESS);

#ifndef NO_NETWORKING
	/* the drive init above gave DHCP plenty of wall clock time to answer,
	 * so check in on it before we start hunting for files */
	network_dhcp_poll();
#endif

	/*int device_list_size = */ // findDevices();

	/* Stop logging and save it to first USB Device found that is writeable */
	LogDeInit();
	//extern char device_list[STD_MAX][10];

	//for (i = 0; i < device_list_size; i++)
	//{
	//	if (strncmp(device_list[i], "ud", 2) == 0)
	//	{
	//		char tmp[STD_MAX + 8];
	//		sprintf(tmp, "%sxell.log", device_list[i]);
	//		if (LogWriteFile(tmp) == 0)
	//			i = device_list_size;
	//	}
	//}
	
	// mount_all_devices();
#ifndef NO_TFTP
	// Set the fallback TFTP address
	ip_addr_t tftp_fallback_address;
	ip4_addr_set_u32(&tftp_fallback_address, 0xC0A8015A); // 192.168.1.90
#endif

	for(;;){
		#ifndef NO_NETWORKING
			// The network needs to be polled for the web interface to
			// function correctly, and it's what finishes off the DHCP
			// handshake network_start() kicked off without blocking.
			network_poll();
			network_dhcp_poll();

			// Held back until DHCP has resolved one way or the other, so
			// this lands under the network config rather than above it.
			// The scan itself has been running since the loop started.
			if (dhcp_settled)
				announce_scan();
		#else
			announce_scan();
		#endif

		// keep the temperature readout live while we sit here, and pin
		// the logo so scrolling text doesn't carry it off the screen
		update_temperatures();
		keep_logo_in_place();

		#ifndef NO_TFTP
			// No point talking to a tftp server before we have an address
			if (dhcp_settled){
				//less likely to find something...
				tftp_loop(boot_server_name());
				tftp_loop(tftp_fallback_address);
			}
		#endif

		fileloop();
		console_clrline();
		usb_do_poll(); // Refresh USB devices
	}

	return 0;
}

