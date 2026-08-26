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
#include <pci/io.h>
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

struct ati_info
{
	uint32_t unknown1[4];
	uint32_t base;
	uint32_t unknown2[8];
	uint32_t width;
	uint32_t height;
} __attribute__ ((__packed__));

#define ATI_INFO ((const volatile struct ati_info *)0xec806100)

extern DISC_INTERFACE xenon_ata_ops;

#define COLOUR_BG 0x1C181800

#define SHADOW_DX 3
#define SHADOW_DY 3

#define LOGO_MARGIN 0

#define LOGO_TOP    32

static int panel_right(void)
{
	return console_get_cursor_max_x() * 8 - LOGO_MARGIN;
}

#define PANEL_MIN_COLS 100

static int panel_fits(void)
{
	return console_get_cursor_max_x() >= PANEL_MIN_COLS;
}

static int panel_col(int len)
{
	int left = panel_right() - LOGO_WIDTH;

	return (left + (LOGO_WIDTH - len * 8) / 2) / 8;
}

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

#define MSMARK_SQ   5
#define MSMARK_GAP  1
#define MSMARK_SIZE (MSMARK_SQ * 2 + MSMARK_GAP)

static void draw_msmark(int x, int y)
{
	static const unsigned char square[4][3] =
	{
		{ 0xF1, 0x51, 0x1B },
		{ 0x80, 0xCC, 0x28 },
		{ 0x00, 0xAD, 0xEF },
		{ 0xFB, 0xBC, 0x09 },
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

void draw_logo()
{
	int left, pass;
	unsigned int x, y;

	if (!panel_fits())
		return;

	left = panel_right() - LOGO_WIDTH;

	for (pass = 0; pass < 2; pass++)
	{
		for (y = 0; y < LOGO_HEIGHT; y++)
		{
			for (x = 0; x < LOGO_WIDTH; x++)
			{
				unsigned int a = logo_alpha[y * LOGO_WIDTH + x];

				if (!a)
					continue;

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

#define DISCORD_TEXT   "@socioculture"
#define DISCORD_COLOUR 0xF2655800
#define DISCORD_ROW  9
#define TEMPS_ROW    11
#define TEMPS_LINES  4
#define TEMPS_WIDTH  11

#define LOGO_BAND_H ((TEMPS_ROW + TEMPS_LINES) * 16)

static void draw_discord(void);
static void draw_temperatures(void);

static int overscan_top(void)
{
	return xenos_is_overscan() ? (int)(ATI_INFO->height / 28) : 0;
}

static void redraw_logo(void)
{
	unsigned int bg, r0, g0, b0;
	int left, top, x, y;

	if (!panel_fits())
		return;

	bg = console_color[0];
	r0 = (bg >>  8) & 0xff;
	g0 = (bg >> 16) & 0xff;
	b0 = (bg >> 24) & 0xff;
	left = panel_right() - LOGO_WIDTH;
	top  = -overscan_top();

	for (y = top; y < LOGO_BAND_H; y++)
		for (x = 0; x < LOGO_WIDTH + SHADOW_DX; x++)
			console_pset(left + x, y, r0, g0, b0);

	draw_logo();
	draw_discord();
	draw_temperatures();
}

static void keep_logo_in_place(void)
{
	static int last_y = -1;
	int max_y = console_get_cursor_max_y();
	int y = console_get_cursor_y();

	if (last_y >= 0 && (y != last_y || y >= max_y - 3))
		redraw_logo();

	last_y = y;
}

static void print_coloured(unsigned int colour, const char *s);
static void status_line(const char *label, const char *state, unsigned int colour);

static int netstatus_row = -1;
static int netstatus_last = -1;

static void netstatus_anchor(void)
{
	netstatus_row = console_get_cursor_y();
	netstatus_last = netstatus_row;
}

static void netstatus_track(void)
{
	int y = console_get_cursor_y();

	if (netstatus_row >= 0 && netstatus_last >= 0 && y <= netstatus_last)
		netstatus_row -= netstatus_last + 1 - y;

	netstatus_last = y;
}

static void set_network_status(const char *state, unsigned int colour)
{
	int x, y, i;

	if (netstatus_row < 0)
	{
		status_line("Network init", state, colour);
		return;
	}

	x = console_get_cursor_x();
	y = console_get_cursor_y();

	console_set_cursor(0, netstatus_row);
	printf("   Network init... ");
	print_coloured(colour, state);
	for (i = strlen(state); i < 32; i++)
		printf(" ");

	console_set_cursor(x, y);
	netstatus_row = -1;
}

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

extern err_t enet_init(struct netif *netif);

#define DHCP_TIMEOUT_MSEC 15000

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


static const char *gpuNames[] =
{
	"Y1",

	"Y1/Y2/Rhea",
	"Rhea",
	"Zeus/Kronos",
	"Vejle",
	"Vejle",
	"Vejle",
	"Oban",
	"Oban",
};

static const struct
{
	int cpu;
	int gpu;
	const char *edram;
} dieNodes[] =
{
	{ 90, 90, "90nm" },
	{ 90, 90, "80nm" },
	{ 65, 80, "80nm" },
	{ 65, 65, "80/65nm" },
	{ 45, 45, "65nm" },
	{ 45, 45, "65nm" },
	{ 45, 45, "65nm" },
	{ 45, 45, "65nm" },
	{ 45, 45, "65nm" },
};

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
	"Waternoose",
	"Waternoose",
	"Loki",
	"Loki",
	"Vejle",
	"Vejle",
	"Vejle",
	"Oban",
	"Oban",
};

#define COLOUR_DIM 0x70707000

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
	netstatus_track();
}

static char consoleSerial[13];
static int serial_year(void)
{
	if (!consoleSerial[0])
		return -1;

	return (consoleSerial[7] == '0') ? 2010 : 2000 + (consoleSerial[7] - '0');
}

static int serial_week(void)
{
	if (!consoleSerial[0])
		return -1;

	return (consoleSerial[8] - '0') * 10 + (consoleSerial[9] - '0');
}

static const char *serial_factory(void)
{
	static char buf[16];

	if (!consoleSerial[0])
		return NULL;

	if (consoleSerial[10] == '0')
		switch (consoleSerial[11])
		{
		case '2': return "Mexico";
		case '3': return "Hungary";
		case '5': return "China";
		case '6': return "Taiwan";
		}

	sprintf(buf, "factory %c%c", consoleSerial[10], consoleSerial[11]);

	return buf;
}

#define GPU_FIX_YEAR 2008
#define GPU_FIX_WEEK 26

static const char *gpu_underfill(int type)
{
	int year = serial_year();
	int week = serial_week();

	if (type != REV_XENON && type != REV_ZEPHYR && type != REV_FALCON)
		return "";

	if (year < 0)
		return "";

	if (year > GPU_FIX_YEAR || (year == GPU_FIX_YEAR && week >= GPU_FIX_WEEK))
		return ", fixed";

	return ", pre-fix";
}

#define TONASKET_WEEK 29

static const char *jasper_variant(int which)
{
	static const char *before[3] = { "Jasper",   "Zeus",   "80nm" };
	static const char *after[3]  = { "Tonasket", "Kronos", "65nm" };
	static const char *either[3] =
		{ "Jasper/Tonasket", "Zeus/Kronos", "80/65nm" };
	int year = serial_year();
	int week = serial_week();

	if (year < 0)
		return either[which];

	if (year > 2009)
		return after[which];

	if (year < 2009 || week < 27)
		return before[which];

	if (week >= TONASKET_WEEK)
		return after[which];

	return either[which];
}

static unsigned int xenos_revision(void)
{
	return read32(0xd0010008) & 0xff;
}

static const char *avpack_name(int avpack)
{
	switch (avpack)
	{
	case 0x13:
	case 0x14:
	case 0x1C:
	case 0x1E:
	case 0x1F:
	case 0x5F: return "HDMI";
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

#define BLURPLE_R ((DISCORD_COLOUR >>  8) & 0xff)
#define BLURPLE_G ((DISCORD_COLOUR >> 16) & 0xff)
#define BLURPLE_B ((DISCORD_COLOUR >> 24) & 0xff)
#define DISCORD_GAP 4

static void draw_discord(void)
{
	int len, left, combo, col, iconx, icony, x, y;
	unsigned int ix, iy;

	if (!panel_fits())
		return;

	len   = (int)strlen(DISCORD_TEXT);
	left  = panel_right() - LOGO_WIDTH;
	combo = DISCORD_WIDTH + DISCORD_GAP + len * 8;
	col   = (left + (LOGO_WIDTH - combo) / 2 + DISCORD_WIDTH + DISCORD_GAP) / 8;
	iconx = col * 8 - DISCORD_GAP - DISCORD_WIDTH;
	icony = DISCORD_ROW * 16 + (16 - DISCORD_HEIGHT) / 2;
	x     = console_get_cursor_x();
	y     = console_get_cursor_y();

	for (iy = 0; iy < DISCORD_HEIGHT; iy++)
	{
		for (ix = 0; ix < DISCORD_WIDTH; ix++)
		{
			unsigned int a = discord_alpha[iy * DISCORD_WIDTH + ix];

			if (!a)
				continue;

			blend_pset(iconx + ix, icony + iy, a,
				   BLURPLE_R, BLURPLE_G, BLURPLE_B);
		}
	}

	console_set_cursor(col, DISCORD_ROW);
	print_coloured(DISCORD_COLOUR, DISCORD_TEXT);

	console_set_cursor(x, y);
}

#define TEMP_WARN 69
#define TEMP_HOT  80

static const char *sensorNames[TEMPS_LINES] = { "CPU", "GPU", "EDRAM", "MB" };

static unsigned int temp_colour(int deg)
{
	if (deg >= TEMP_HOT)
		return CONSOLE_COLOR_RED;

	if (deg >= TEMP_WARN)
		return CONSOLE_WARN;

	return CONSOLE_SUCCESS;
}

static void print_temperatures_inline(void)
{
	uint16_t sensor[TEMPS_LINES];
	int i;

	xenon_smc_query_sensors(sensor);

	printf("   Temps:");
	for (i = 0; i < TEMPS_LINES; i++)
	{
		char value[10];
		int deg = sensor[i] >> 8;

		sprintf(value, " %d.%01dC", deg, ((sensor[i] & 0xff) * 10) / 256);

		printf(" %s", sensorNames[i]);
		print_coloured(temp_colour(deg), value);
	}
	printf("\n");
}

static void draw_temperatures(void)
{
	uint16_t sensor[TEMPS_LINES];
	int col, x, y, i;

	if (!panel_fits())
		return;

	col = panel_col(TEMPS_WIDTH);
	x = console_get_cursor_x();
	y = console_get_cursor_y();

	xenon_smc_query_sensors(sensor);

	for (i = 0; i < TEMPS_LINES; i++)
	{
		int deg = sensor[i] >> 8;
		char value[8];

		sprintf(value, "%2d.%01dC", deg, ((sensor[i] & 0xff) * 10) / 256);

		console_set_cursor(col, TEMPS_ROW + i);
		printf("%-5s ", sensorNames[i]);
		print_coloured(temp_colour(deg), value);
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

static const char *ata_model(struct xenon_ata_device *dev)
{
	static char buf[sizeof(dev->model) + 1];
	int n;

	memcpy(buf, dev->model, sizeof(dev->model));
	buf[sizeof(dev->model)] = '\0';

	for (n = strlen(buf) - 1; n >= 0 && buf[n] == ' '; n--)
		buf[n] = '\0';

	return buf;
}

#define SECURITY_SECTOR_LBA 16
#define SS_MODEL_OFF        0x1C
#define SS_MODEL_LEN        40

static int security_sector_present(void)
{
	static unsigned char sec[XENON_DISK_SECTOR_SIZE] __attribute__((aligned(128)));
	char model[sizeof(ata.model) + 1];
	char plain[SS_MODEL_LEN + 1], swapped[SS_MODEL_LEN + 1];
	int i, n;

	strcpy(model, ata_model(&ata));
	if (!model[0])
		return 0;

	memset(sec, 0, sizeof(sec));
	xenon_ata_ops.readSectors(SECURITY_SECTOR_LBA, 1, sec);

	memcpy(plain, sec + SS_MODEL_OFF, SS_MODEL_LEN);
	plain[SS_MODEL_LEN] = '\0';

	for (i = 0; i < SS_MODEL_LEN; i += 2)
	{
		swapped[i]     = sec[SS_MODEL_OFF + i + 1];
		swapped[i + 1] = sec[SS_MODEL_OFF + i];
	}
	swapped[SS_MODEL_LEN] = '\0';

	for (n = SS_MODEL_LEN - 1; n >= 0 && plain[n] == ' '; n--)
		plain[n] = '\0';
	for (n = SS_MODEL_LEN - 1; n >= 0 && swapped[n] == ' '; n--)
		swapped[n] = '\0';

	return strcmp(plain, model) == 0 || strcmp(swapped, model) == 0;
}

#define SECTORS_PER_GB 1953125

#define HEALTH_UNKNOWN 0
#define HEALTH_OK      1
#define HEALTH_FAILING 2

#define SMART_TIMEOUT_MSEC 2000
#define SMART_RETURN_STATUS 0xDA
#define ATA_CMD_SMART       0xB0

static void ata_reg_write(int reg, uint8_t val)
{
	*(volatile uint8_t *)(ata.ioaddress + reg) = val;
}

static uint8_t ata_reg_read(int reg)
{
	return *(volatile uint8_t *)(ata.ioaddress + reg);
}

static int ata_wait_not_busy(void)
{
	uint64_t t = mftb();

	while (ata_reg_read(XENON_ATA_REG_STATUS) & 0x80)
		if (tb_diff_msec(mftb(), t) > SMART_TIMEOUT_MSEC)
			return 0;

	return 1;
}

static int drive_health(void)
{
	uint8_t mid, high;

	if (!ata.ioaddress || !ata_wait_not_busy())
		return HEALTH_UNKNOWN;

	ata_reg_write(XENON_ATA_REG_DISK, 0xE0);
	ata_reg_write(XENON_ATA_REG_FEATURES, SMART_RETURN_STATUS);
	ata_reg_write(XENON_ATA_REG_SECTORS, 0);
	ata_reg_write(XENON_ATA_REG_LBALOW, 0);
	ata_reg_write(XENON_ATA_REG_LBAMID, 0x4F);
	ata_reg_write(XENON_ATA_REG_LBAHIGH, 0xC2);
	ata_reg_write(XENON_ATA_REG_CMD, ATA_CMD_SMART);

	if (!ata_wait_not_busy())
		return HEALTH_UNKNOWN;

	if (ata_reg_read(XENON_ATA_REG_STATUS) & 0x01)
		return HEALTH_UNKNOWN;

	mid  = ata_reg_read(XENON_ATA_REG_LBAMID);
	high = ata_reg_read(XENON_ATA_REG_LBAHIGH);

	if (mid == 0x4F && high == 0xC2)
		return HEALTH_OK;

	if (mid == 0xF4 && high == 0x2C)
		return HEALTH_FAILING;

	return HEALTH_UNKNOWN;
}

static const char *drive_name(struct xenon_ata_device *dev)
{
	static const char *const brands[][2] =
	{
		{ "PLDS",     "Philips/Lite-On" },
		{ "Lite-On",  "Lite-On"         },
		{ "LITE-ON",  "Lite-On"         },
		{ "LITEON",   "Lite-On"         },
		{ "TSSTcorp", "Toshiba/Samsung" },
		{ "TSSTCORP", "Toshiba/Samsung" },
		{ "HL-DT-ST", "Hitachi/LG"      },
		{ "HITACHI",  "Hitachi"         },
		{ "SAMSUNG",  "Samsung"         },
		{ "TOSHIBA",  "Toshiba"         },
		{ "PHILIPS",  "Philips"         },
		{ "BENQ",     "BenQ"            },
		{ "BenQ",     "BenQ"            },
	};
	static char buf[sizeof(dev->model) + 24];
	char model[sizeof(dev->model) + 1];
	const char *rest;
	unsigned int i;

	strcpy(model, ata_model(dev));

	for (i = 0; i < sizeof(brands) / sizeof(brands[0]); i++)
	{
		size_t n = strlen(brands[i][0]);

		if (strncmp(model, brands[i][0], n))
			continue;

		rest = model + n;
		while (*rest == ' ')
			rest++;

		if (*rest)
			sprintf(buf, "%s %s", brands[i][1], rest);
		else
			strcpy(buf, brands[i][1]);

		return buf;
	}

	strcpy(buf, model);

	return buf;
}

static void detect_line(const char *label, int present, struct xenon_ata_device *dev)
{
	printf("   %s... ", label);

	if (!present)
	{
		print_coloured(COLOUR_DIM, "None");
		printf("\n");
		return;
	}

	printf("%s", drive_name(dev));

	if (dev == &ata)
	{
		if (dev->size >= SECTORS_PER_GB)
			printf(" %uGB", dev->size / SECTORS_PER_GB);

		printf(" - ");
		if (security_sector_present())
			print_coloured(CONSOLE_SUCCESS, "Security Sector");
		else
			print_coloured(COLOUR_DIM, "No Security Sector");

		switch (drive_health())
		{
		case HEALTH_OK:
			printf(", ");
			print_coloured(CONSOLE_SUCCESS, "SMART OK");
			break;
		case HEALTH_FAILING:
			printf(", ");
			print_coloured(CONSOLE_ERR, "SMART FAILING");
			break;
		}
	}

	printf("\n");
}

#define VFUSES_JTAG_OFFSET 0x95000
#define VFUSES_LEN         0x60
#define PATCH_SLOTS_MAX    8

static const char *exploit_method(int zerofuse)
{
	static const unsigned char fuseline0[8] =
		{ 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	unsigned char buf[VFUSES_LEN];
	uint32_t slot_offset, slot_size;
	uint16_t slot_count;
	int i;

	if (zerofuse)
		return "Zero Fuse";

	if (xenon_get_logical_nand_data(buf, VFUSES_JTAG_OFFSET, VFUSES_LEN) != -1 &&
	    memcmp(buf, fuseline0, sizeof(fuseline0)) == 0)
		return "JTAG image";

	if (xenon_get_logical_nand_data(&slot_offset, 0x64, sizeof(slot_offset)) == -1 ||
	    xenon_get_logical_nand_data(&slot_count, 0x68, sizeof(slot_count)) == -1 ||
	    xenon_get_logical_nand_data(&slot_size, 0x70, sizeof(slot_size)) == -1)
		return NULL;

	if (slot_size == 0)
		slot_size = 0x10000;

	if (slot_count > PATCH_SLOTS_MAX)
		slot_count = PATCH_SLOTS_MAX;

	for (i = 0; i < (int)slot_count; i++)
	{
		if (xenon_get_logical_nand_data(buf, slot_offset + i * slot_size,
						VFUSES_LEN) == -1)
			return NULL;

		if (memcmp(buf, fuseline0, sizeof(fuseline0)) == 0)
			return "Glitch image";
	}

	return NULL;
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

	for (i = 0; i < len; i++)
		out[i] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : ' ';
	out[len] = '\0';

	for (i = len - 1; i >= 0 && out[i] == ' '; i--)
		out[i] = '\0';

	if (out[0] == '\0')
		return;

	for (i = 0; out[i]; i++)
		if (!((out[i] >= '0' && out[i] <= '9') ||
		      (out[i] >= 'A' && out[i] <= 'Z') ||
		      (out[i] >= 'a' && out[i] <= 'z')))
			return;

	printf("%s: %s\n", label, out);
}

static void print_mfg_date(unsigned char *kv)
{
	unsigned char cert[0x1A8];
	int n = sizeof(cert);
	unsigned char *d;
	int i, digits = 0, printable = 0, dashed;

	if (kv_get_key(XEKEY_CONSOLE_CERTIFICATE, cert, &n, kv) != 0)
		return;

	d = &cert[0x1C];

	for (i = 0; i < 8 && !d[i]; i++)
		;
	if (i == 8)
		return;

	for (i = 0; i < 8; i++)
	{
		if (d[i] >= '0' && d[i] <= '9')
			digits++;
		if (d[i] >= 0x20 && d[i] < 0x7f)
			printable++;
	}

	dashed = (digits == 6 && d[2] == '-' && d[5] == '-');

	if (dashed)
		printf("   * Mfg Date: 20%c%c-%c%c-%c%c\n",
			d[6], d[7], d[0], d[1], d[3], d[4]);
	else if (digits == 8)
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
	int n, r, zerofuse = 0;

	printf("\n");

	memset(key, '\0', sizeof(key));
	if (cpu_get_key(key) == 0)
	{
		print_key_green("   * CPU Key", key);

		for (n = 0; n < (int)sizeof(key) && key[n] == 0; n++)
			;

		zerofuse = (n == (int)sizeof(key));
	}

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
	{
		const char *method = exploit_method(zerofuse);
		int n;

		printf("   * Virtual CPU Key: ");
		for (n = 0; n < 16; n++)
			printf("%02X", key[n]);

		if (method)
		{
			printf("  ");
			print_coloured(CONSOLE_WARN, method);
		}

		printf("\n");
	}

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
		r = kv_read(kv, 1);

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

	n = 0x0C;
	if (kv_get_key(XEKEY_CONSOLE_SERIAL_NUMBER, key, &n, kv) == 0)
	{
		int d;

		for (d = 0; d < 0x0C; d++)
			if (key[d] < '0' || key[d] > '9')
				break;

		if (d == 0x0C)
		{
			memcpy(consoleSerial, key, 0x0C);
			consoleSerial[0x0C] = '\0';
		}
	}

	if (consoleSerial[0])
	{
		const char *factory = serial_factory();

		printf("   * Serial: %s  (%d week %d", consoleSerial,
			serial_year(), serial_week());

		if (factory)
			printf(", %s", factory);

		printf(")\n");
	}
	else
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

	console_clrscr();
	redraw_logo();

	if (panel_fits())
		printf("XeLL git-" GITREV " - (C) 2007-2026 LibXenon.org, Free60.org\n\n");
	else
	{
		printf("SocioCustoms ");
		print_coloured(DISCORD_COLOUR, DISCORD_TEXT);
		printf(" - XeLL git-" GITREV " - LibXenon.org, Free60.org\n\n");
	}

	console_close();
	printf("XeLL - Xenon linux loader second stage " LONGVERSION "\n");
	printf("Built with GCC " GCC_VERSION " and Binutils " BINUTILS_VERSION " \n");
	console_open();

	//delay(3); //give the user a chance to see our splash screen <- network init should last long enough...

	xenon_sound_init();

	console_close();
	xenon_make_it_faster(XENON_SPEED_FULL);
	console_open();

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


	/* display some cpu info */
	consoleType = xenon_get_console_type();

	printf("\n");

	procRow = console_get_cursor_y();

	printf("  Microsoft %s%s %08x %u.%03uGHz Processor\n",
			 (consoleType >= 0 && consoleType <= 8) ? cpuNames[consoleType] : "Unknown",
			 die_node(consoleType, 0),
			 mfspr(287),
			 (unsigned int)((PPC_TIMEBASE_FREQ * 64) / 1000000000LL),
			 (unsigned int)(((PPC_TIMEBASE_FREQ * 64) / 1000000LL) % 1000));

	draw_msmark(0, procRow * 16 + 1);

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
			printf("%02X\n\n", avpack);
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

		fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
	}

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

	console_open();

	if (consoleType >= 0 && consoleType <= 8)
		printf("   Console: %s - %s (%dnm%s)\n",
			 (consoleType == REV_JASPER) ? jasper_variant(0)
						     : consoleNames[consoleType],
			 (consoleType == REV_JASPER) ? jasper_variant(1)
						     : gpuNames[consoleType],
			 dieNodes[consoleType].gpu,
			 gpu_underfill(consoleType));
	else
		printf("   Console: Unknown (PVR %08x)\n", mfspr(287));

	if (consoleType == REV_JASPER)
		printf("   Memory: %uK   eDRAM: 10MB (%s)\n",
			xenon_get_ram_size() / 1024, jasper_variant(2));
	else
		printf("   Memory: %uK   eDRAM: 10MB%s\n",
			xenon_get_ram_size() / 1024, die_node(consoleType, 2));

	if (sfc.initialized == SFCX_INITIALIZED)
		printf("   NAND: %dMB (%s)\n",
			sfc.size_mb, nand_type_name(sfc.meta_type));

	if (panel_fits())
		draw_temperatures();
	else
		print_temperatures_inline();
	printf("\n");

	detect_line("Storage", ataPresent, &ata);
	detect_line("Disc Drive", atapiPresent, &atapi);

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
	netstatus_anchor();

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
	network_dhcp_poll();
#endif

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
			network_poll();
			network_dhcp_poll();

			if (dhcp_settled)
				announce_scan();
		#else
			announce_scan();
		#endif

		update_temperatures();
		keep_logo_in_place();

		#ifndef NO_TFTP
			if (dhcp_settled){
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

