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
#define FUSE_WIDE_COLS 100

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

static void set_network_status(const char *state, unsigned int colour)
{
	status_line("Network init", state, colour);
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

#define XENOS_ID_ELPIS 0x5821

#define CPUKEY_FUSE_FIRST 3
#define CPUKEY_FUSE_LAST  6

static int zero_fuse(void)
{
	int i;

	for (i = CPUKEY_FUSE_FIRST; i <= CPUKEY_FUSE_LAST; i++)
		if (xenon_secotp_read_line(i) != 0)
			return 0;

	return 1;
}

#define CB_LDV_FUSE     2
#define JTAG_LDV_MAX    5

static int cb_ldv(void)
{
	u64 v = xenon_secotp_read_line(CB_LDV_FUSE);
	int bits = 0;

	while (v)
	{
		bits += v & 1;
		v >>= 1;
	}

	return bits / 4;
}

static int is_elpis(void)
{
	return xenon_get_console_type() == REV_XENON &&
	       xenon_get_XenosID() == XENOS_ID_ELPIS;
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

static void print_sep(void)
{
	print_coloured(COLOUR_DIM, " | ");
}

static void print_bullet(void)
{
	printf("   ");
	print_coloured(COLOUR_DIM, "*");
	printf(" ");
}

#define LDV_FUSE_FIRST 7
#define LDV_FUSE_LAST  11

static void print_fuse_word(u64 value, int highlight)
{
	char hex[17], one[2];
	int n;

	sprintf(hex, "%08x%08x",
		(unsigned int)(value >> 32), (unsigned int)(value & 0xffffffff));

	one[1] = '\0';

	for (n = 0; n < 16; n++)
	{
		one[0] = hex[n];

		if (highlight && hex[n] == 'f')
			print_coloured(CONSOLE_WARN, one);
		else
			printf("%s", one);
	}
}

static void status_line(const char *label, const char *state, unsigned int colour)
{
	if (colour == CONSOLE_SUCCESS)
		return;

	printf("   %s... ", label);
	print_coloured(colour, state);
	printf("\n");
}

static char consoleSerial[13];
static char kvOsig[29];

#define KV_OSIG_OFFSET     0xC92
#define KV_OSIG_LEN        28
#define KV_CONSOLEID_OFF   0x9CA
#define KV_CONSOLEID_LEN   5
#define KV_TYPE_OFFSET     0x1DF8
#define KV_TYPE_LEN        8

static int kv_type(const unsigned char *kv)
{
	int i, ff = 1, zero = 1;

	if (KV_FLASH_SIZE < KV_TYPE_OFFSET + KV_TYPE_LEN)
		return 0;

	for (i = 0; i < KV_TYPE_LEN; i++)
	{
		if (kv[KV_TYPE_OFFSET + i] != 0xFF)
			ff = 0;

		if (kv[KV_TYPE_OFFSET + i] != 0x00)
			zero = 0;
	}

	return (ff || zero) ? 1 : 2;
}

static const char *console_id_friendly(const unsigned char *raw)
{
	static char out[16];
	char hex[11], dec[24];
	uint64_t v = 0;
	int i, n = 0, pad;

	for (i = 0; i < KV_CONSOLEID_LEN; i++)
		sprintf(hex + i * 2, "%02X", raw[i]);

	hex[10] = '\0';

	for (i = 0; i < 9; i++)
		v = v * 16 +
			(uint64_t)((hex[i] <= '9') ? hex[i] - '0' : hex[i] - 'A' + 10);

	if (v == 0)
		dec[n++] = '0';

	while (v)
	{
		dec[n++] = (char)('0' + (int)(v % 10));
		v /= 10;
	}

	pad = 12 - (n + 1);

	if (pad < 0)
		pad = 0;

	for (i = 0; i < pad; i++)
		out[i] = '0';

	for (n--; n >= 0; n--)
		out[i++] = dec[n];

	out[i++] = hex[9];
	out[i] = '\0';

	return out;
}

#define DRIVE_UNKNOWN 0
#define DRIVE_MATCH   1
#define DRIVE_SWAPPED 2

static int drive_match(struct xenon_ata_device *dev)
{
	char product[20];
	int n;

	if (!kvOsig[0])
		return DRIVE_UNKNOWN;

	memcpy(product, dev->model + 8, 16);
	product[16] = '\0';

	for (n = 15; n >= 0 && (product[n] == ' ' || product[n] == '\0'); n--)
		product[n] = '\0';

	if (!product[0])
		return DRIVE_UNKNOWN;

	return strstr(kvOsig, product) ? DRIVE_MATCH : DRIVE_SWAPPED;
}
static int serial_year(void)
{
	int digit, year;

	if (!consoleSerial[0])
		return -1;

	digit = consoleSerial[7] - '0';

	if (digit < 0 || digit > 9)
		return -1;

	year = (digit >= 5) ? 2000 + digit : 2010 + digit;

	if (year < 2010 && xenon_get_console_type() >= REV_TRINITY)
		year += 10;

	return year;
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
static const struct
{
	const char *pkg;
	const char *gpu;
	int gpuNode;
	const char *edram;
	int edramNode;
} gpuDies[] =
{
	{ "Y1",     "Xenos C1", 90, "Edifis",  90 },
	{ "Y2",     "Xenos C2", 90, "Edifis",  90 },
	{ "Rhea",   "Xenos C2", 90, "Styx-90", 90 },
	{ "Elpis",  NULL,       80, "Styx-90", 90 },
	{ "Zeus",   "Gunga",    65, "Styx-90", 90 },
	{ "Kronos", "Gunga",    65, "Styx-65", 65 },
	{ "Vejle",  "XCGPU",    45, "Styx-65", 65 },
	{ "Oban",   NULL,       32, "Oban",     0 },
};

static const char *gpu_package(int type)
{
	if (is_elpis())
		return "Elpis";

	if (type == REV_JASPER)
		return jasper_variant(1);

	if (type >= 0 && type <= 8)
		return gpuNames[type];

	return NULL;
}

static int gpu_die(int type)
{
	const char *pkg = gpu_package(type);
	unsigned int i;

	if (!pkg)
		return -1;

	for (i = 0; i < sizeof(gpuDies) / sizeof(gpuDies[0]); i++)
		if (strcmp(gpuDies[i].pkg, pkg) == 0)
			return (int)i;

	return -1;
}

static const char *gpu_die_name(int type)
{
	int i = gpu_die(type);

	return (i >= 0) ? gpuDies[i].gpu : NULL;
}

static const char *gpu_detail(int type)
{
	static char buf[48];
	int i = gpu_die(type);

	if (i >= 0)
		sprintf(buf, "%dnm%s", gpuDies[i].gpuNode, gpu_underfill(type));
	else
		sprintf(buf, "%dnm%s",
			(type >= 0 && type <= 8) ? dieNodes[type].gpu : 0,
			gpu_underfill(type));

	return buf;
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
	case 0x17:
	case 0x43:
	case 0x57: return "Composite";
	case 0x54: return "Composite + S-Video";
	case 0x47: return "SCART";
	}

	return NULL;
}

static const char *game_region_name(unsigned int region)
{
	switch (region)
	{
	case 0x00FF: return "NTSC/US";
	case 0x0101: return "NTSC/HK";
	case 0x01FC: return "NTSC/KOR";
	case 0x01FE:
	case 0x01FF: return "NTSC/JAP";
	case 0x0201: return "PAL/AUS";
	case 0x02FE: return "PAL/EU";
	case 0x7FFF: return "DEVKIT";
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

#define NAND_BLOCK_SCAN_MAX 4096

static unsigned int nandXellOffset;
static int nandXellBad;

static unsigned int xell_nand_offset(void)
{
	unsigned char footer[XELL_FOOTER_LENGTH];
	int i;

	if (xenon_logical_nand_data_ok() != 0)
		return 0;

	for (i = 0; i < XELL_OFFSET_COUNT; i++)
	{
		if (xenon_get_logical_nand_data(footer,
				xelloffsets[i] + XELL_FOOTER_OFFSET,
				XELL_FOOTER_LENGTH) == -1)
			continue;

		if (memcmp(footer, XELL_FOOTER, XELL_FOOTER_LENGTH) == 0)
			return xelloffsets[i];
	}

	return 0;
}

static int nand_bad_blocks(void)
{
	static unsigned char page[MAX_PAGE_SZ];
	int block, bad = 0, blocks = sfc.size_blocks;
	int first = -1, last = -1;

	nandXellOffset = 0;
	nandXellBad = 0;

	if (sfc.initialized != SFCX_INITIALIZED)
		return -1;

	if (blocks <= 0 || blocks > NAND_BLOCK_SCAN_MAX)
		return -1;

	nandXellOffset = xell_nand_offset();

	if (nandXellOffset && sfc.block_sz > 0)
	{
		first = nandXellOffset / sfc.block_sz;
		last = (nandXellOffset + XELL_SIZE - 1) / sfc.block_sz;
	}

	for (block = 0; block < blocks; block++)
	{
		int status = sfcx_read_page(page, sfcx_block_to_address(block), 1);

		if (SFCX_SUCCESS(status) && sfcx_is_pagevalid(page))
			continue;

		bad++;

		if (first >= 0 && block >= first && block <= last)
			nandXellBad = 1;
	}

	return bad;
}

#define SMC_HDR_LEN_OFF   0x78
#define SMC_HDR_START_OFF 0x7C
#define SMC_SIG_BASE      0x2DC0
#define SMC_SIG_LEN       0xE1
#define SMC_SIG_SITES     5
#define SMC_VAR_WINDOW    0x1500
#define SMC_CHUNK         0x200
#define SMC_MAX_LEN       0x8000

static int smc_read_window(unsigned char *out, uint32_t base, uint32_t want)
{
	unsigned char chunk[SMC_CHUNK];
	unsigned int keys[4] = { 0x42, 0x75, 0x4E, 0x79 };
	uint32_t start = 0, len = 0, off, end = base + want;
	unsigned int i, n, pos = 0;

	if (xenon_logical_nand_data_ok() != 0)
		return -1;

	if (xenon_get_logical_nand_data(&len, SMC_HDR_LEN_OFF, sizeof(len)) == -1 ||
	    xenon_get_logical_nand_data(&start, SMC_HDR_START_OFF, sizeof(start)) == -1)
		return -1;

	if (start == 0 || len > SMC_MAX_LEN || len < end)
		return -1;

	for (off = 0; off < end; off += SMC_CHUNK)
	{
		n = SMC_CHUNK;

		if (off + n > end)
			n = end - off;

		if (xenon_get_logical_nand_data(chunk, start + off, n) == -1)
			return -1;

		for (i = 0; i < n; i++, pos++)
		{
			unsigned int mod = (unsigned int)chunk[i] * 0xFB;
			unsigned char dec =
				(unsigned char)(chunk[i] ^ (keys[pos & 3] & 0xFF));

			keys[(pos + 1) & 3] += mod;
			keys[(pos + 2) & 3] += (mod >> 8);

			if (pos >= base)
				out[pos - base] = dec;
		}
	}

	return (int)len;
}

static const unsigned short smcSigSite[SMC_SIG_SITES] =
	{ 0x03, 0x0B, 0xDE, 0xDF, 0xE0 };

static const struct
{
	unsigned char byte[SMC_SIG_SITES];
	const char *name;
} smcWiring[] = {
	{ { 0xCC, 0xCC, 0xF8, 0xD0, 0xE0 }, "AUD_CLAMP" },
	{ { 0x83, 0x83, 0xD0, 0xE0, 0xF8 }, "ARGON_DATA" },
};

static const char *smc_jtag_wiring(void)
{
	unsigned char sig[SMC_SIG_LEN];
	unsigned int i;
	int k;

	if (smc_read_window(sig, SMC_SIG_BASE, SMC_SIG_LEN) < 0)
		return NULL;

	for (i = 0; i < sizeof(smcWiring) / sizeof(smcWiring[0]); i++)
	{
		for (k = 0; k < SMC_SIG_SITES; k++)
			if (sig[smcSigSite[k]] != smcWiring[i].byte[k])
				break;

		if (k == SMC_SIG_SITES)
			return smcWiring[i].name;
	}

	return NULL;
}

#define SMC_VAR_SITES 8
#define SMC_VAR_DISC  2

static const struct
{
	int type;
	uint32_t len;
	int sites;
	unsigned short site[SMC_VAR_SITES];
	unsigned char byte[SMC_VAR_SITES];
	int disc;
	unsigned short dsite[SMC_VAR_DISC];
	unsigned char plusByte[SMC_VAR_DISC];
	unsigned char altByte[SMC_VAR_DISC];
	const char *alt;
} smcVariant[] =
{
	{ REV_XENON, 0x3000, 8,
		{ 0x0775, 0x0776, 0x0777, 0x0851, 0x0852, 0x0853, 0x1148, 0x1149 },
		{ 0x02, 0x2D, 0xDD, 0x02, 0x2D, 0xC0, 0x02, 0x2D },
		0, { 0 }, { 0 }, { 0 },
		NULL },
	{ REV_FALCON, 0x3000, 8,
		{ 0x0156, 0x13C3, 0x13C8, 0x13CD, 0x13D2, 0x13D7, 0x13DC, 0x13E1 },
		{ 0x12, 0x75, 0x04, 0x17, 0x75, 0x1A, 0x75, 0x80 },
		2, { 0x1276, 0x1284 }, { 0x8A, 0x8A }, { 0xAF, 0xAF },
		"CR4" },
	{ REV_JASPER, 0x3000, 6,
		{ 0x0072, 0x0073, 0x0074, 0x0156, 0x0157, 0x0158 },
		{ 0x12, 0x2D, 0x73, 0x12, 0x2D, 0x7D },
		2, { 0x127B, 0x1292 }, { 0x50, 0x50 }, { 0x54, 0x54 },
		"CR4" },
	{ REV_TRINITY, 0x3000, 8,
		{ 0x00F1, 0x14C0, 0x14C5, 0x14CA, 0x14CF, 0x14D4, 0x14D9, 0x14DE },
		{ 0x12, 0x75, 0x04, 0x6C, 0x75, 0x6F, 0x75, 0x80 },
		2, { 0x1380, 0x1396 }, { 0x41, 0x41 }, { 0x60, 0x60 },
		"CR4" },
	{ REV_CORONA, 0x3800, 6,
		{ 0x0059, 0x005A, 0x005B, 0x00F1, 0x00F2, 0x00F3 },
		{ 0x12, 0x31, 0xF0, 0x12, 0x31, 0xFA },
		2, { 0x1381, 0x1397 }, { 0x41, 0x41 }, { 0x50, 0x50 },
		"CR4" },
	{ REV_CORONA_PHISON, 0x3800, 6,
		{ 0x0059, 0x005A, 0x005B, 0x00F1, 0x00F2, 0x00F3 },
		{ 0x12, 0x31, 0xF0, 0x12, 0x31, 0xFA },
		2, { 0x1381, 0x1397 }, { 0x41, 0x41 }, { 0x50, 0x50 },
		"CR4" },
};

static const char *smc_variant(void)
{
	static unsigned char win[SMC_VAR_WINDOW];
	int type = xenon_get_console_type();
	unsigned int i;
	int k, len;

	for (i = 0; i < sizeof(smcVariant) / sizeof(smcVariant[0]); i++)
	{
		if (smcVariant[i].type != type)
			continue;

		len = smc_read_window(win, 0, SMC_VAR_WINDOW);

		if (len < 0 || (uint32_t)len != smcVariant[i].len)
			return NULL;

		for (k = 0; k < smcVariant[i].sites; k++)
			if (win[smcVariant[i].site[k]] != smcVariant[i].byte[k])
				return NULL;

		if (smcVariant[i].disc == 0)
			return "SMC+";

		for (k = 0; k < smcVariant[i].disc; k++)
			if (win[smcVariant[i].dsite[k]] != smcVariant[i].plusByte[k])
				break;

		if (k == smcVariant[i].disc)
			return "SMC+";

		for (k = 0; k < smcVariant[i].disc; k++)
			if (win[smcVariant[i].dsite[k]] != smcVariant[i].altByte[k])
				return NULL;

		return smcVariant[i].alt;
	}

	return NULL;
}

#define SMC_QUERY_VERSION 0x12

static const char *smc_version(void)
{
	static char text[16];
	unsigned char msg[16];

	memset(msg, 0, sizeof(msg));
	msg[0] = SMC_QUERY_VERSION;

	xenon_smc_send_message(msg);
	xenon_smc_receive_response(msg);

	if (msg[1] >= 1 && msg[1] <= 9)
		sprintf(text, "%u.%02u", msg[1], msg[2]);
	else if (msg[2] >= 1 && msg[2] <= 9)
		sprintf(text, "%u.%02u", msg[2], msg[3]);
	else
		return NULL;

	return text;
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

#define TEMPS_INTERVAL_MSEC 1000

static void update_temperatures(void)
{
	static uint64_t last_update;

	if (tb_diff_msec(mftb(), last_update) < TEMPS_INTERVAL_MSEC)
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

static int security_sectors_present(void)
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
#define SMART_ENABLE_OPS    0xD8
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

#define SMART_READ_DATA       0xD0
#define SMART_READ_THRESHOLDS 0xD1
#define SMART_ATTR_REALLOC 5
#define SMART_ATTR_HOURS   9
#define SMART_ATTR_PENDING 197
#define SMART_ATTR_UNCORR  198
#define SMART_ATTR_CRC     199
#define SMART_ATTR_COUNT   30
#define HOURS_MID          5000
#define HOURS_HIGH         15000

static unsigned int hours_colour(uint32_t hours)
{
	if (hours >= HOURS_HIGH)
		return CONSOLE_COLOR_ORANGE;

	if (hours >= HOURS_MID)
		return CONSOLE_WARN;

	return console_color[1];
}

static int ata_wait_drq(void)
{
	uint64_t t = mftb();
	uint8_t status;

	for (;;)
	{
		status = ata_reg_read(XENON_ATA_REG_STATUS);

		if (status & 0x01)
			return 0;

		if (status & 0x08)
			return 1;

		if (tb_diff_msec(mftb(), t) > SMART_TIMEOUT_MSEC)
			return 0;
	}
}

static int smart_table_ok(const unsigned char *buf)
{
	int n, seen = 0, last = 0;

	for (n = 0; n < SMART_ATTR_COUNT; n++)
	{
		int id = buf[2 + n * 12];

		if (id == 0)
			continue;

		if (id <= last)
			return 0;

		last = id;
		seen++;
	}

	return seen >= 3;
}

static int smart_read_table(unsigned char *buf, uint8_t feature)
{
	int i;

	if (!ata.ioaddress || !ata_wait_not_busy())
		return 0;

	ata_reg_write(XENON_ATA_REG_DISK, 0xE0);
	ata_reg_write(XENON_ATA_REG_FEATURES, feature);
	ata_reg_write(XENON_ATA_REG_SECTORS, 1);
	ata_reg_write(XENON_ATA_REG_LBALOW, 0);
	ata_reg_write(XENON_ATA_REG_LBAMID, 0x4F);
	ata_reg_write(XENON_ATA_REG_LBAHIGH, 0xC2);
	ata_reg_write(XENON_ATA_REG_CMD, ATA_CMD_SMART);

	if (!ata_wait_not_busy() || !ata_wait_drq())
		return 0;

	for (i = 0; i < 128; i++)
		((uint32_t *)buf)[i] =
			*(volatile uint32_t *)(ata.ioaddress + XENON_ATA_REG_DATA);

	if (smart_table_ok(buf))
		return 1;

	for (i = 0; i < 256; i++)
	{
		unsigned char t = buf[i * 2];

		buf[i * 2] = buf[i * 2 + 1];
		buf[i * 2 + 1] = t;
	}

	return smart_table_ok(buf);
}

static int smart_attr(const unsigned char *buf, int id, uint32_t *out)
{
	int n;

	for (n = 0; n < SMART_ATTR_COUNT; n++)
	{
		const unsigned char *e = buf + 2 + n * 12;

		if (e[0] != id)
			continue;

		*out = (uint32_t)e[5] | ((uint32_t)e[6] << 8) |
		       ((uint32_t)e[7] << 16) | ((uint32_t)e[8] << 24);

		return 1;
	}

	return 0;
}

static const char *smart_attr_name(int id)
{
	switch (id)
	{
	case 1:   return "Read Error Rate";
	case 3:   return "Spin-Up Time";
	case 4:   return "Start/Stop Count";
	case 5:   return "Reallocated Sectors";
	case 7:   return "Seek Error Rate";
	case 9:   return "Power-On Hours";
	case 10:  return "Spin Retry Count";
	case 11:  return "Recalibration Retries";
	case 12:  return "Power Cycle Count";
	case 184: return "End-to-End Error";
	case 187: return "Reported Uncorrectable";
	case 188: return "Command Timeout";
	case 190: return "Airflow Temperature";
	case 194: return "Temperature";
	case 196: return "Reallocation Events";
	case 197: return "Pending Sectors";
	case 198: return "Offline Uncorrectable";
	case 199: return "UDMA CRC Errors";
	case 200: return "Write Error Rate";
	}

	return NULL;
}

static const char *smart_failing_attr(const unsigned char *data)
{
	static unsigned char thr[XENON_DISK_SECTOR_SIZE] __attribute__((aligned(128)));
	static char text[32];
	int n, m;

	memset(thr, 0, sizeof(thr));

	if (!smart_read_table(thr, SMART_READ_THRESHOLDS))
		return NULL;

	for (n = 0; n < SMART_ATTR_COUNT; n++)
	{
		const unsigned char *e = data + 2 + n * 12;

		if (e[0] == 0)
			continue;

		for (m = 0; m < SMART_ATTR_COUNT; m++)
		{
			const unsigned char *t = thr + 2 + m * 12;
			const char *name;

			if (t[0] != e[0] || t[1] == 0 || e[3] > t[1])
				continue;

			name = smart_attr_name(e[0]);

			if (name)
				return name;

			sprintf(text, "attribute %d", e[0]);

			return text;
		}
	}

	return NULL;
}

static void print_smart_wear(int health)
{
	static unsigned char buf[XENON_DISK_SECTOR_SIZE] __attribute__((aligned(128)));
	uint32_t hours, bad, count;
	int have_hours, have_bad;
	char text[32];

	memset(buf, 0, sizeof(buf));

	if (!smart_read_table(buf, SMART_READ_DATA))
		return;

	have_hours = smart_attr(buf, SMART_ATTR_HOURS, &hours);
	have_bad   = smart_attr(buf, SMART_ATTR_REALLOC, &bad);

	if (!have_hours && !have_bad)
		return;

	printf("      ");

	if (have_hours)
	{
		sprintf(text, "%u hour%s", hours, (hours == 1) ? "" : "s");
		print_coloured(hours_colour(hours), text);
	}

	if (have_hours && have_bad)
		printf(", ");

	if (have_bad)
	{
		sprintf(text, "%u reallocated", bad);
		print_coloured(bad ? CONSOLE_COLOR_RED : console_color[1], text);
	}

	if (smart_attr(buf, SMART_ATTR_PENDING, &count) && count)
	{
		printf(", ");
		sprintf(text, "%u Pending", count);
		print_coloured(CONSOLE_COLOR_RED, text);
	}

	if (smart_attr(buf, SMART_ATTR_UNCORR, &count) && count)
	{
		printf(", ");
		sprintf(text, "%u Uncorrectable", count);
		print_coloured(CONSOLE_COLOR_RED, text);
	}

	if (smart_attr(buf, SMART_ATTR_CRC, &count) && count)
	{
		printf(", ");
		sprintf(text, "%u CRC Error%s", count, (count == 1) ? "" : "s");
		print_coloured(CONSOLE_WARN, text);
	}

	if (health == HEALTH_FAILING)
	{
		const char *why = smart_failing_attr(buf);

		if (why)
		{
			print_sep();
			print_coloured(CONSOLE_COLOR_RED, why);
		}
	}

	printf("\n");
}

static int drive_health(void)
{
	uint8_t mid, high;

	if (!ata.ioaddress || !ata_wait_not_busy())
		return HEALTH_UNKNOWN;

	ata_reg_write(XENON_ATA_REG_DISK, 0xE0);
	ata_reg_write(XENON_ATA_REG_FEATURES, SMART_ENABLE_OPS);
	ata_reg_write(XENON_ATA_REG_SECTORS, 0);
	ata_reg_write(XENON_ATA_REG_LBALOW, 0);
	ata_reg_write(XENON_ATA_REG_LBAMID, 0x4F);
	ata_reg_write(XENON_ATA_REG_LBAHIGH, 0xC2);
	ata_reg_write(XENON_ATA_REG_CMD, ATA_CMD_SMART);

	if (!ata_wait_not_busy())
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
	int health = HEALTH_UNKNOWN;

	printf("   %s... ", label);

	if (!present)
	{
		print_coloured(COLOUR_DIM, "None");
		printf("\n");
		return;
	}

	printf("%s", drive_name(dev));

	if (dev == &atapi)
	{
		if (drive_match(dev) == DRIVE_SWAPPED)
		{
			print_sep();
			print_coloured(CONSOLE_COLOR_RED, "Not The KV Drive");
		}
	}

	if (dev == &ata)
	{
		if (dev->size >= SECTORS_PER_GB)
			printf(" %uGB", dev->size / SECTORS_PER_GB);

		print_sep();

		if (security_sectors_present())
			print_coloured(CONSOLE_SUCCESS, "Security Sectors");
		else
			print_coloured(COLOUR_DIM, "No Security Sectors");

		printf(", ");
		health = drive_health();

		switch (health)
		{
		case HEALTH_OK:
			print_coloured(CONSOLE_SUCCESS, "S.M.A.R.T OK");
			break;
		case HEALTH_FAILING:
			print_coloured(CONSOLE_COLOR_RED, "S.M.A.R.T FAILING");
			break;
		default:
			print_coloured(COLOUR_DIM, "S.M.A.R.T n/a");
			break;
		}
	}

	printf("\n");

	if (health != HEALTH_UNKNOWN)
		print_smart_wear(health);
}

#define CB_HEADER_OFFSET 0x8000

#define BL_CHAIN_MAX  8
#define BL_SIZE_MIN   0x10
#define BL_SIZE_MAX   0x400000
#define BL_WALK_LIMIT 0x800000

#define BL_STAGE_2BL  2
#define CB_X_RGH13    42069
#define CB_FLAG_MFG   0x01

struct bl_chain
{
	int count;
	int dev;
	int cb_count;
	int cb_mfg;
	unsigned int cb_a;
	unsigned int cb_b;
	unsigned int cb_x;
	unsigned char magic[BL_CHAIN_MAX][2];
	unsigned int build[BL_CHAIN_MAX];
};

static const struct bl_chain *bootloaders(void)
{
	static struct bl_chain chain;
	static int scanned;
	unsigned int cb_build[BL_CHAIN_MAX];
	unsigned char hdr[0x10];
	uint32_t off = CB_HEADER_OFFSET;
	int n;

	if (scanned)
		return &chain;

	scanned = 1;

	if (xenon_logical_nand_data_ok() != 0)
		return &chain;

	for (n = 0; n < BL_CHAIN_MAX; n++)
	{
		uint32_t size;

		if (xenon_get_logical_nand_data(hdr, off, sizeof(hdr)) == -1)
			break;

		if (hdr[1] < 'A' || hdr[1] > 'Z')
			break;

		if ((hdr[1] & 0x0F) < 2 || (hdr[1] & 0x0F) > 7)
			break;

		chain.magic[chain.count][0] =
			(hdr[0] >= 'A' && hdr[0] <= 'Z') ? hdr[0] : 'S';
		chain.magic[chain.count][1] = hdr[1];
		chain.build[chain.count] = (unsigned int)((hdr[2] << 8) | hdr[3]);

		if ((hdr[1] & 0x0F) == BL_STAGE_2BL)
		{
			if (chain.cb_count == 0)
			{
				chain.cb_mfg = (hdr[7] & CB_FLAG_MFG) ? 1 : 0;
				chain.dev = (hdr[0] != 'C') ? 1 : 0;
			}

			cb_build[chain.cb_count++] = chain.build[chain.count];
		}

		chain.count++;

		size = ((uint32_t)hdr[0x0C] << 24) | ((uint32_t)hdr[0x0D] << 16) |
		       ((uint32_t)hdr[0x0E] <<  8) |  (uint32_t)hdr[0x0F];

		size = (size + 0xF) & ~(uint32_t)0xF;

		if (size < BL_SIZE_MIN || size > BL_SIZE_MAX)
			break;

		off += size;

		if (off >= BL_WALK_LIMIT)
			break;
	}

	if (chain.cb_count >= 1)
	{
		chain.cb_a = cb_build[0];
		chain.cb_b = cb_build[chain.cb_count - 1];
	}

	if (chain.cb_count >= 3)
		chain.cb_x = cb_build[1];

	return &chain;
}

static void print_bootloaders(void)
{
	const struct bl_chain *bl = bootloaders();
	int n;

	if (bl->count == 0)
		return;

	print_bullet();
	printf("Bootloaders:");

	for (n = 0; n < bl->count; n++)
		printf("  %c%c %u", bl->magic[n][0], bl->magic[n][1], bl->build[n]);

	printf("\n");
}

#define BL_STAGE_CF      6
#define PATCH_SLOTS_MAX  8
#define PATCH_SCAN_LEN   0x200
#define PATCH_SLOT_TABLE 0x64

static unsigned int kernel_version(void)
{
	unsigned char hdr[0x10];
	uint32_t slot_offset, slot_size;
	uint16_t slot_count;
	unsigned int best = 0;
	int i, n;

	if (xenon_logical_nand_data_ok() != 0)
		return 0;

	if (xenon_get_logical_nand_data(&slot_offset, PATCH_SLOT_TABLE,
					sizeof(slot_offset)) == -1 ||
	    xenon_get_logical_nand_data(&slot_count, PATCH_SLOT_TABLE + 4,
					sizeof(slot_count)) == -1 ||
	    xenon_get_logical_nand_data(&slot_size, PATCH_SLOT_TABLE + 12,
					sizeof(slot_size)) == -1)
		return 0;

	if (slot_size == 0)
		slot_size = 0x10000;

	if (slot_count > PATCH_SLOTS_MAX)
		slot_count = PATCH_SLOTS_MAX;

	for (i = 0; i < (int)slot_count; i++)
	{
		uint32_t base = slot_offset + i * slot_size;

		for (n = 0; n < PATCH_SCAN_LEN; n += 0x10)
		{
			unsigned int build;

			if (xenon_get_logical_nand_data(hdr, base + n, sizeof(hdr)) == -1)
				break;

			if (hdr[0] != 'C' || (hdr[1] & 0x0F) != BL_STAGE_CF)
				continue;

			build = (unsigned int)((hdr[2] << 8) | hdr[3]);

			if (build > best)
				best = build;

			break;
		}
	}

	return best;
}

#define VFUSES_JTAG_OFFSET 0x95000
#define VFUSES_LEN         0x60

static const char *exploit_method(void)
{
	static const unsigned char fuseline0[8] =
		{ 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	unsigned char buf[VFUSES_LEN];
	const struct bl_chain *bl = bootloaders();

	if (zero_fuse())
		return "Zero Fuse";

	if (xenon_logical_nand_data_ok() != 0)
		return NULL;

	if (bl->dev)
		return "DevGL";

	if (bl->cb_x == CB_X_RGH13)
		return "RGH 1.3";

	if (bl->cb_x)
		return "RGH3";

	if (xenon_get_logical_nand_data(buf, VFUSES_JTAG_OFFSET, VFUSES_LEN) == -1)
		return NULL;

	if (memcmp(buf, fuseline0, sizeof(fuseline0)) == 0)
		return (cb_ldv() > JTAG_LDV_MAX) ? "RJTAG" : "JTAG";

	if (xenon_get_console_type() == REV_XENON)
		return "EXT_CLK";

	if (bl->cb_count >= 2)
	{
		if (bl->cb_mfg)
			return "RGH2m";

		return "RGH2";
	}

	if (bl->cb_count == 1)
		return "RGH1";

	return "RGH";
}

static u64 vfuse_word(const unsigned char *vf, int line)
{
	u64 v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | vf[line * 8 + i];

	return v;
}

static int vfuses_valid(const unsigned char *vf)
{
	static const unsigned char line0[8] =
		{ 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

	if (memcmp(vf, line0, sizeof(line0)) != 0)
		return 0;

	if (memcmp(vf + 3 * 8, vf + 4 * 8, 8) != 0)
		return 0;

	if (memcmp(vf + 5 * 8, vf + 6 * 8, 8) != 0)
		return 0;

	return vfuse_word(vf, 3) != 0;
}

static int read_vfuses(unsigned char *out)
{
	uint32_t slot_offset = 0, slot_size = 0;
	uint16_t slot_count = 0;
	int i;

	if (xenon_logical_nand_data_ok() != 0)
		return 0;

	if (xenon_get_logical_nand_data(out, VFUSES_JTAG_OFFSET, VFUSES_LEN) != -1 &&
	    vfuses_valid(out))
		return 1;

	if (xenon_get_logical_nand_data(&slot_offset, PATCH_SLOT_TABLE,
					sizeof(slot_offset)) == -1 ||
	    xenon_get_logical_nand_data(&slot_count, PATCH_SLOT_TABLE + 4,
					sizeof(slot_count)) == -1 ||
	    xenon_get_logical_nand_data(&slot_size, PATCH_SLOT_TABLE + 12,
					sizeof(slot_size)) == -1)
		return 0;

	if (slot_size == 0 || slot_count == 0 || slot_count > PATCH_SLOTS_MAX)
		return 0;

	for (i = 0; i < (int)slot_count; i++)
	{
		uint32_t at = slot_offset + (uint32_t)i * slot_size;

		if (xenon_get_logical_nand_data(out, at, VFUSES_LEN) == -1)
			continue;

		if (vfuses_valid(out))
			return 1;
	}

	return 0;
}

#define KEY_COLOUR CONSOLE_COLOR_RED

#define VFUSE_NONE     0
#define VFUSE_MATCH    1
#define VFUSE_MISMATCH 2

static int vfuse_check(const unsigned char *virt)
{
	unsigned char real[0x10];

	if (zero_fuse())
		return VFUSE_NONE;

	memset(real, '\0', sizeof(real));

	if (cpu_get_key(real) != 0)
		return VFUSE_NONE;

	return (memcmp(real, virt, sizeof(real)) == 0)
		? VFUSE_MATCH : VFUSE_MISMATCH;
}

static void print_key_plain(const char *name, unsigned char *data,
			    const char *note, unsigned int colour)
{
	int i;

	print_bullet();
	printf("%s: ", name);

	for (i = 0; i < 16; i++)
		printf("%02X", data[i]);

	if (note)
	{
		print_sep();
		print_coloured(colour, note);
	}

	printf("\n");
}

static void print_key_hi(const char *name, unsigned char *data)
{
	unsigned int bg = console_color[0], fg = console_color[1];
	int i;

	print_bullet();
	printf("%s: ", name);

	console_set_colors(bg, KEY_COLOUR);
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

	print_bullet();
	printf("%s: %s\n", label, out);
}

static char mfgDate[24];
static int mfgYear, mfgWeek;

static void read_mfg_date(unsigned char *kv)
{
	static const int cum[12] =
		{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	unsigned char cert[0x1A8];
	int n = sizeof(cert);
	unsigned char *d;
	int i, digits = 0, printable = 0, dashed;
	int y = 0, mo = 0, da = 0, doy;

	mfgDate[0] = '\0';
	mfgYear = 0;
	mfgWeek = 0;

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
	{
		y  = 2000 + (d[6] - '0') * 10 + (d[7] - '0');
		mo = (d[0] - '0') * 10 + (d[1] - '0');
		da = (d[3] - '0') * 10 + (d[4] - '0');
	}
	else if (digits == 8)
	{
		y  = (d[0] - '0') * 1000 + (d[1] - '0') * 100 +
		     (d[2] - '0') * 10 + (d[3] - '0');
		mo = (d[4] - '0') * 10 + (d[5] - '0');
		da = (d[6] - '0') * 10 + (d[7] - '0');
	}
	else if (printable == 8)
	{
		sprintf(mfgDate, "%.8s", (char *)d);
		return;
	}
	else
	{
		sprintf(mfgDate, "%02X%02X%02X%02X%02X%02X%02X%02X",
			d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
		return;
	}

	sprintf(mfgDate, "%04d-%02d-%02d", y, mo, da);

	if (mo < 1 || mo > 12 || da < 1 || da > 31)
		return;

	doy = cum[mo - 1] + da;

	if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		doy++;

	mfgYear = y;
	mfgWeek = (doy + 6) / 7;
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
		print_key_hi("CPU Key", key);

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
		switch (vfuse_check(key))
		{
		case VFUSE_MATCH:
			print_key_plain("Virtual CPU Key", key,
					"Matches Fuses", CONSOLE_SUCCESS);
			break;

		case VFUSE_MISMATCH:
			print_key_plain("Virtual CPU Key", key,
					"Wrong Console", CONSOLE_COLOR_RED);
			break;

		default:
			print_key_plain("Virtual CPU Key", key, NULL, 0);
			break;
		}
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
		print_key_plain("DVD Key", key, NULL, 0);

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

	read_mfg_date(kv);

	if (consoleSerial[0])
	{
		const char *factory = serial_factory();
		int drift = mfgWeek - serial_week();

		print_bullet();
		printf("Serial: %s", consoleSerial);

		if (mfgDate[0])
		{
			print_sep();
			printf("%s", mfgDate);
		}

		print_sep();
		printf("Week %d", serial_week());

		if (factory)
			printf(", %s", factory);

		if (drift < 0)
			drift = -drift;

		if (mfgYear && (mfgYear != serial_year() || drift > 1))
		{
			print_sep();
			print_coloured(CONSOLE_COLOR_RED, "Date Mismatch");
		}

		printf("\n");
	}
	else
	{
		print_kv_ascii("Serial", XEKEY_CONSOLE_SERIAL_NUMBER, 0x0C, kv);

		if (mfgDate[0])
		{
			print_bullet();
			printf("Mfg Date: %s\n", mfgDate);
		}
	}

	print_bullet();
	printf("Console ID: %s", console_id_friendly(kv + KV_CONSOLEID_OFF));
	print_sep();
	for (n = 0; n < KV_CONSOLEID_LEN; n++)
		printf("%02X", kv[KV_CONSOLEID_OFF + n]);

	n = kv_type(kv);

	if (n)
	{
		print_sep();
		printf("KV Type %d", n);
	}

	printf("\n");

	memcpy(kvOsig, kv + KV_OSIG_OFFSET, KV_OSIG_LEN);
	kvOsig[KV_OSIG_LEN] = '\0';

	for (n = 0; n < KV_OSIG_LEN; n++)
		if (kvOsig[n] < 0x20 || kvOsig[n] > 0x7e)
		{
			kvOsig[n] = '\0';
			break;
		}

	print_kv_ascii("Mobo Serial", XEKEY_MOBO_SERIAL_NUMBER, 0x0C, kv);

	n = sizeof(region);
	if (kv_get_key(XEKEY_GAME_REGION, region, &n, kv) == 0)
	{
		unsigned int code = (region[0] << 8) | region[1];
		const char *name = game_region_name(code);

		print_bullet();
		printf("Game Region: %04X", code);

		if (name)
			printf(" - %s", name);

		printf("\n");
	}

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

	printf("   Microsoft %s%s %08x %u.%03uGHz Processor\n",
			 (consoleType >= 0 && consoleType <= 8) ? cpuNames[consoleType] : "Unknown",
			 die_node(consoleType, 0),
			 mfspr(287),
			 (unsigned int)((PPC_TIMEBASE_FREQ * 64) / 1000000000LL),
			 (unsigned int)(((PPC_TIMEBASE_FREQ * 64) / 1000000LL) % 1000));

	draw_msmark(8, procRow * 16 + 1);

	if (is_elpis())
		printf("   Console: Xenon - Elpis (%s)", gpu_detail(consoleType));
	else if (consoleType >= 0 && consoleType <= 8)
	{
		const char *name = (consoleType == REV_JASPER)
					? jasper_variant(0)
					: consoleNames[consoleType];
		const char *pkg = (consoleType == REV_JASPER)
					? jasper_variant(1)
					: gpuNames[consoleType];

		if (strcmp(pkg, cpuNames[consoleType]) == 0)
			printf("   Console: %s", name);
		else
			printf("   Console: %s - %s (%s)", name, pkg,
				 gpu_detail(consoleType));
	}
	else
		printf("   Console: Unknown (PVR %08x)", mfspr(287));

	{
		const char *method = exploit_method();

		if (method)
		{
			print_sep();
			print_coloured(CONSOLE_WARN, method);

			if (strstr(method, "JTAG"))
			{
				const char *wiring = smc_jtag_wiring();

				if (wiring)
				{
					printf(" ");
					print_coloured(CONSOLE_WARN, wiring);
				}
			}
		}
	}

	printf("\n");

	printf("\n");

#ifndef NO_PRINT_CONFIG
	u64 fuseline[12];
	char *fusestr = FUSES;
	unsigned char vfuse[VFUSES_LEN];
	int haveVfuse;

	for (i=0; i<12; ++i){
		unsigned int hi,lo;

		fuseline[i]=xenon_secotp_read_line(i);
		hi=fuseline[i]>>32;
		lo=fuseline[i]&0xffffffff;

		fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
	}

	haveVfuse = read_vfuses(vfuse);

	if (!haveVfuse)
	{
		printf("Fuses:\n");

		for (i=0; i<6; ++i){
			printf("   %02d: ", i);
			print_fuse_word(fuseline[i],
				i >= LDV_FUSE_FIRST && i <= LDV_FUSE_LAST);
			printf("     %02d: ", i + 6);
			print_fuse_word(fuseline[i + 6],
				(i + 6) >= LDV_FUSE_FIRST && (i + 6) <= LDV_FUSE_LAST);
			printf("\n");
		}
	}
	else if (console_get_cursor_max_x() >= FUSE_WIDE_COLS)
	{
		printf("Fuses:                                               Virtual Fuses:\n");

		for (i = 0; i < 6; ++i)
		{
			printf("   %02d: ", i);
			print_fuse_word(fuseline[i],
				i >= LDV_FUSE_FIRST && i <= LDV_FUSE_LAST);
			printf("     %02d: ", i + 6);
			print_fuse_word(fuseline[i + 6],
				(i + 6) >= LDV_FUSE_FIRST && (i + 6) <= LDV_FUSE_LAST);
			printf("     %02d: ", i);
			print_fuse_word(vfuse_word(vfuse, i),
				i >= LDV_FUSE_FIRST && i <= LDV_FUSE_LAST);
			printf("     %02d: ", i + 6);
			print_fuse_word(vfuse_word(vfuse, i + 6),
				(i + 6) >= LDV_FUSE_FIRST && (i + 6) <= LDV_FUSE_LAST);
			printf("\n");
		}
	}
	else
	{
		printf("Fuses:                   Virtual Fuses:\n");

		for (i = 0; i < 12; ++i)
		{
			printf("   %02d: ", i);
			print_fuse_word(fuseline[i],
				i >= LDV_FUSE_FIRST && i <= LDV_FUSE_LAST);
			printf("  ");
			print_fuse_word(vfuse_word(vfuse, i),
				i >= LDV_FUSE_FIRST && i <= LDV_FUSE_LAST);
			printf("\n");
		}
	}

	{
		unsigned int kernel = kernel_version();

		printf("\n");
		print_bullet();
		printf("LDV: CB %d / CF-CG %d",
			fuse_ldv(fuseline, 2, 2), fuse_ldv(fuseline, 7, 11));

		if (kernel)
		{
			print_sep();
			printf("Kernel 2.0.%u.0", kernel);
		}

		printf("\n");
		print_bootloaders();
	}

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

	printf("   GPU ID: %04x rev %02x", xenon_get_XenosID(), xenos_revision());

	{
		const char *die = gpu_die_name(consoleType);

		if (die)
		{
			print_sep();
			printf("%s", die);
		}
	}

	print_sep();
	printf("Bridge: %02x", xenon_get_PCIBridgeRevisionID());
	print_sep();
	printf("DVE: %02x", xenon_get_DVE());
	print_sep();
	printf("Video: %ux%u\n",
			 (unsigned int)ATI_INFO->width,
			 (unsigned int)ATI_INFO->height);

	{
		unsigned int mb = xenon_get_ram_size() / (1024 * 1024);

		if (mb >= 1024)
			printf("   Memory: %uGB", mb / 1024);
		else
			printf("   Memory: %uMB", mb);
	}

	print_sep();

	{
		int die = gpu_die(consoleType);

		if (die >= 0 && gpuDies[die].edramNode)
			printf("eDRAM: 10MB (%s, %dnm)\n",
				gpuDies[die].edram, gpuDies[die].edramNode);
		else if (die >= 0)
			printf("eDRAM: 10MB (%s, on-die)\n", gpuDies[die].edram);
		else if (consoleType == REV_JASPER)
			printf("eDRAM: 10MB (%s)\n", jasper_variant(2));
		else
			printf("eDRAM: 10MB%s\n", die_node(consoleType, 2));
	}

	if (sfc.initialized == SFCX_INITIALIZED)
	{
		const char *smc = smc_version();
		int bad;

		console_close();
		bad = nand_bad_blocks();
		console_open();

		printf("   NAND: %dMB", sfc.size_mb);

		if (smc)
		{
			print_sep();
			printf("SMC: %s", smc);

			{
				const char *var = smc_variant();

				if (var)
				{
					printf(" ");
					print_coloured(CONSOLE_WARN, var);
				}
			}
		}

		if (bad >= 0)
		{
			char text[40];

			print_sep();
			sprintf(text, "%d Bad Block%s", bad, (bad == 1) ? "" : "s");
			print_coloured(bad ? CONSOLE_COLOR_RED : console_color[1], text);
		}

		if (nandXellOffset)
		{
			char text[40];

			print_sep();

			if (nandXellBad)
			{
				sprintf(text, "XeLL @ 0x%X Damaged", nandXellOffset);
				print_coloured(CONSOLE_COLOR_RED, text);
			}
			else
			{
				printf("XeLL @ 0x%X", nandXellOffset);
			}
		}

		printf("\n");
	}

	{
		int avpack = xenon_smc_read_avpack();
		const char *avname = avpack_name(avpack);

		printf("   AV Region: %s", av_region_name(xenon_config_get_avregion()));
		print_sep();
		printf("AV Pack: ");

		if (avname)
			printf("%s\n", avname);
		else
			printf("Unknown (%02X)\n", avpack);
	}

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
	if (!netif.ip_addr.addr && netStatus != NETWORK_INIT_SUCCESS)
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

