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
#include <httpd/httpd.h>
#include <diskio/ata.h>
#include <elf/elf.h>
#include <version.h>
#include <byteswap.h>

#include <lwip/ip_addr.h> // for ipaddr_ntoa (Web UI line)

#include "asciiart.h"
#include "config.h"
#include "file.h"
#include "tftp/tftp.h"
#include "log.h"

/* Uncomment for verbose ANA dumps (it spams the screen) */
// #define HEXAMODS_DEBUG_ANA 1

/* Optional: build with -DHEXAMODS_THEME to force your colors */
// #define HEXAMODS_THEME 1

static void do_asciiart(void)
{
	char *p = asciiart;
	while (*p)
		console_putch(*p++);
	/* safer than printf(asciitail); */
	printf("%s", asciitail);
}

static void dumpana(void)
{
	int i;
	for (i = 0; i < 0x100; ++i)
	{
		uint32_t v;
		xenon_smc_ana_read(i, &v);
		printf("0x%08x, ", (unsigned int)v);
		if ((i & 0x7) == 0x7)
			printf(" // %02x\n", (unsigned int)(i & ~0x7));
	}
}

static void apply_console_theme(void)
{
	/*
	 * NOTE: call AFTER console_init() so colors "stick" reliably.
	 */
#ifdef HEXAMODS_THEME
	/* Purple-ish background + orange text (ARGB: 0xAARRGGBB) */
	console_set_colors(0xFF020014, 0xFFFF4500);
#elif defined(SWIZZY_THEME)
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE);
#elif defined(XTUDO_THEME)
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_PINK);
#elif defined(DEFAULT_THEME)
	console_set_colors(CONSOLE_COLOR_BLUE, CONSOLE_COLOR_WHITE);
#else
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GREEN);
#endif
}

static void print_header(void)
{
	printf("========================================\n");
	printf("  HexaMods Reloaded  -  by DAYBR3AK\n");
	printf("  XeLL RELOADED 2nd Stage " LONGVERSION "\n");
	printf("========================================\n\n");
}

char FUSES[350]; /* this string stores the ascii dump of the fuses */

unsigned char stacks[6][0x10000];

static void reset_timebase_task(void)
{
	mtspr(284, 0); // TBLW
	mtspr(285, 0); // TBUW
	mtspr(284, 0);
}

static void synchronize_timebases(void)
{
	xenon_thread_startup();

	std((void*)0x200611a0, 0); // stop timebase

	int i;
	for (i = 1; i < 6; ++i) {
		xenon_run_thread_task(i, &stacks[i][0xff00], (void *)reset_timebase_task);
		while (xenon_is_thread_task_running(i));
	}

	reset_timebase_task(); // don't forget thread 0
	std((void*)0x200611a0, 0x1ff); // restart timebase
}

int main(void)
{
	LogInit();
	int i;

	/* flush console after each outputted char */
	setbuf(stdout, NULL);

#ifdef HEXAMODS_DEBUG_ANA
	printf("ANA Dump before Init:\n");
	dumpana();
#endif

	// linux needs this
	synchronize_timebases();

	// irqs preinit (SMC related)
	*(volatile uint32_t*)0xea00106c = 0x1000000;
	*(volatile uint32_t*)0xea001064 = 0x10;
	*(volatile uint32_t*)0xea00105c = 0xc000000;

	xenon_smc_start_bootanim();

	xenos_init(VIDEO_MODE_AUTO);

#ifdef HEXAMODS_DEBUG_ANA
	printf("ANA Dump after Init:\n");
	dumpana();
#endif

	/* Init console first, then set colors */
	console_init();
	apply_console_theme();
	console_clrscr();

	print_header();
	do_asciiart();

	/* Give the splash a moment to be seen (optional) */
	delay(1);

	xenon_sound_init();
	xenon_make_it_faster(XENON_SPEED_FULL);

	if (xenon_get_console_type() != REV_CORONA_PHISON) // Not needed for MMC type of consoles
	{
		printf(" * nand init\n");
		sfcx_init();
		if (sfc.initialized != SFCX_INITIALIZED)
		{
			printf(" ! sfcx initialization failure\n");
			printf(" ! nand related features will not be available\n");
			delay(5);
		}
	}

	xenon_config_init();

#ifndef NO_NETWORKING
	printf(" * network init\n");
	network_init();

	printf(" * starting httpd server...");
	httpd_start();
	printf("success\n");

	/* Nice “pro” line */
	printf(" * Web UI: http://%s/\n", ipaddr_ntoa(&netif.ip_addr));
#endif

	printf(" * usb init\n");
	usb_init();
	usb_do_poll();

	// FIXME: Not initializing these devices here causes an interrupt storm in linux.
	printf(" * sata hdd init\n");
	xenon_ata_init();

#ifndef NO_DVD
	printf(" * sata dvd init\n");
	xenon_atapi_init();
#endif

	mount_all_devices();

	/* display some cpu info */
	printf(" * CPU PVR: %08x\n", mfspr(287));

#ifndef NO_PRINT_CONFIG
	printf(" * FUSES - write them down and keep them safe:\n");
	char *fusestr = FUSES;

	for (i = 0; i < 12; ++i) {
		u64 line;
		unsigned int hi, lo;

		line = xenon_secotp_read_line(i);
		hi = (unsigned int)(line >> 32);
		lo = (unsigned int)(line & 0xffffffff);

		fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
	}
	printf("%s", FUSES);

	print_cpu_dvd_keys();
	network_print_config();
#endif

	/* Stop logging */
	LogDeInit();

	/* TFTP fallback */
	ip_addr_t fallback_address;
	ip4_addr_set_u32(&fallback_address, 0xC0A8015A); // 192.168.1.90

	printf("\n * Looking for files on local media and TFTP...\n\n");

	/* Small status spinner without spamming newlines */
	static const char spin[] = "|/-\\";
	int si = 0;

	for (;;)
	{
		/* Try local media first, then TFTP */
		fileloop();
		tftp_loop(boot_server_name());
		tftp_loop(fallback_address);

		console_clrline();
		printf(" %c waiting... (USB/TFTP)\r", spin[(si++) & 3]);

		usb_do_poll();
	}

	return 0;
}
