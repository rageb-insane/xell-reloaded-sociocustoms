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

#include "asciiart.h"
#include "config.h"
#include "file.h"

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

void do_asciiart()
{
	char *p = asciiart;
	while (*p)
		console_putch(*p++);
	printf(asciitail);
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
		printf(" * dhcp lease acquired\n");
#ifndef NO_PRINT_CONFIG
		network_print_config();
#endif
		return;
	}

	if (tb_diff_msec(mftb(), dhcp_started) < DHCP_TIMEOUT_MSEC)
		return;

	dhcp_settled = 1;
	console_clrline();
	printf(" * dhcp failed - now assigning a static ip\n");

	IP4_ADDR(&ipaddr, 192, 168, 1, 99);
	IP4_ADDR(&gateway, 192, 168, 1, 1);
	IP4_ADDR(&netmask, 255, 255, 255, 0);
	netif_set_addr(&netif, &ipaddr, &netmask, &gateway);
	netif_set_up(&netif);
#ifndef NO_PRINT_CONFIG
	network_print_config();
#endif
}
#endif

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
	
int main(){
	LogInit();
	int i;
	int consoleType = 0;

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
	console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_WHITE); // White text on black bg
#else
	console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_GREEN); // Green text on black bg
#endif
	console_init();

	/* console_init() announces the framebuffer the instant it arms the screen
	 * hook, so there's no muting it - wipe the screen instead. clrscr also
	 * puts the cursor back to 0,0, so the splash starts at the top. */
	console_clrscr();

	/* version banner goes to the log and the uart, not the screen */
	console_close();
	printf("\nXeLL - Xenon linux loader second stage " LONGVERSION "\n");
    printf("\nBuilt with GCC " GCC_VERSION " and Binutils " BINUTILS_VERSION " \n");
	console_open();

	do_asciiart();

	//delay(3); //give the user a chance to see our splash screen <- network init should last long enough...
	
	xenon_sound_init();

	/* xenon_make_it_faster()/xenon_set_speed() natter about VIDs and cores
	 * waking back up. Drop the screen hook while they run so none of it shows
	 * - the work still happens, and the log still records it. */
	console_close();
	xenon_make_it_faster(XENON_SPEED_FULL);
	console_open();

	if (xenon_get_console_type() != REV_CORONA_PHISON) //Not needed for MMC type of consoles! ;)
	{
		/* nand bring-up is only worth showing when it goes wrong */
		console_close();
		printf(" * nand init\n");
		sfcx_init();
		console_open();

		if (sfc.initialized != SFCX_INITIALIZED)
		{
			printf(" ! sfcx initialization failure\n");
			printf(" ! nand related features will not be available\n");
			delay(5);
		}
	}

	xenon_config_init();

	/* Everything worth writing down is readable as soon as the NAND is up, so
	 * print it before we go anywhere near the network - no waiting on a DHCP
	 * server to see your fuses, cpu key, serial and dvd key. */

	/* display some cpu info */
	consoleType = xenon_get_console_type();

	printf("\n * Console Type: %s (PVR %08x)\n\n",
			 (consoleType >= 0 && consoleType <= 7) ? consoleNames[consoleType] : "Unknown",
			 mfspr(287));

#ifndef NO_PRINT_CONFIG
	printf(" * FUSES - write them down and keep them safe:\n");
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
		printf("   fuseset %02d: %08x%08x     fuseset %02d: %08x%08x\n",
			i,
			(unsigned int)(fuseline[i]>>32),
			(unsigned int)(fuseline[i]&0xffffffff),
			i+6,
			(unsigned int)(fuseline[i+6]>>32),
			(unsigned int)(fuseline[i+6]&0xffffffff));
	}

	print_cpu_dvd_keys();
#endif

	/* Everything from here to the file scan is driver bring-up noise - XeLL's
	 * own status lines plus whatever lwip, the PHY, USB and ATA feel like
	 * saying. Keep the whole run off the screen; the log still records it. */
	console_close();

#ifndef NO_NETWORKING

	printf(" * network init\n");
	network_start();

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
	xenon_ata_init();

#ifndef NO_DVD
	printf(" * sata dvd init\n");
	xenon_atapi_init();
#endif

	mount_all_devices();

	/* back on screen for the dhcp result and the file scan */
	console_open();

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

	printf("\n * Looking for files on TFTP and local media...\n\n");
#else
	printf("\n * Looking for files on local media...\n\n");
#endif

	for(;;){
		#ifndef NO_NETWORKING
			// The network needs to be polled for the web interface to
			// function correctly, and it's what finishes off the DHCP
			// handshake network_start() kicked off without blocking.
			network_poll();
			network_dhcp_poll();
		#endif

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

