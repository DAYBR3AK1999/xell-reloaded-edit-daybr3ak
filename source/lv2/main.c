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

#include "asciiart.h"
#include "config.h"
#include "file.h"
#include "tftp/tftp.h"
#include "log.h"

static inline void set_theme_colors_default(void)
{
	/*
	 * HexaMods style default: ORANGE on BLACK
	 * Keeps your existing theme switches intact.
	 */
#ifdef SWIZZY_THEME
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE); // Orange text on black bg
#elif defined XTUDO_THEME
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_PINK);   // Pink text on black bg
#elif defined DEFAULT_THEME
	console_set_colors(CONSOLE_COLOR_BLUE,  CONSOLE_COLOR_WHITE);  // White text on blue bg
#else
	// Default to HexaMods look instead of green
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE);  // Orange text on black bg
#endif
}

static inline void set_value_colors(void)
{
	// “terminal” values like your web page
	console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GREEN);
}

void do_asciiart(void)
{
	// asciiart/asciitail are typically string literals -> treat as const
	const char *p = asciiart;
	while (*p)
		console_putch(*p++);

	// safer than printf(asciitail)
	printf("%s", asciitail);
}

void dumpana(void)
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

char FUSES[350]; /* this string stores the ascii dump of the fuses */

unsigned char stacks[6][0x10000];

void reset_timebase_task(void)
{
	mtspr(284, 0); // TBLW
	mtspr(285, 0); // TBUW
	mtspr(284, 0);
}

void synchronize_timebases(void)
{
	xenon_thread_startup();

	std((void*)0x200611a0, 0); // stop timebase

	int i;
	for (i = 1; i < 6; ++i) {
		xenon_run_thread_task(i, &stacks[i][0xff00], (void*)reset_timebase_task);
		while (xenon_is_thread_task_running(i));
	}

	reset_timebase_task(); // don't forget thread 0

	std((void*)0x200611a0, 0x1ff); // restart timebase
}

int main(void)
{
	LogInit();
	int i;

	printf("ANA Dump before Init:\n");
	dumpana();

	// linux needs this
	synchronize_timebases();

	// irqs preinit (SMC related)
	*(volatile uint32_t*)0xea00106c = 0x1000000;
	*(volatile uint32_t*)0xea001064 = 0x10;
	*(volatile uint32_t*)0xea00105c = 0xc000000;

	xenon_smc_start_bootanim();

	// flush console after each outputted char
	setbuf(stdout, NULL);

	xenos_init(VIDEO_MODE_AUTO);

	printf("ANA Dump after Init:\n");
	dumpana();

	/*
	 * NOTE: some console implementations reset colors during console_init().
	 * So init first, then apply our theme colors.
	 */
	console_init();
	set_theme_colors_default();

	// Branding header
	printf("\nHexaMods XeLL RELOADED - Second Stage " LONGVERSION "\n");
	printf("Coded by DAYBR3AK // RGH 4 Ever\n\n");

	do_asciiart();

	//delay(3); //give the user a chance to see our splash screen <- network init should last long enough...

	xenon_sound_init();

	if (xenon_get_console_type() != REV_CORONA_PHISON) // Not needed for MMC type of consoles! ;)
	{
		set_theme_colors_default();
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

	set_theme_colors_default();
	printf(" * network init\n");
	network_init();

	printf(" * starting httpd server...");
	httpd_start();
	printf("success\n");

#endif

	set_theme_colors_default();
	printf(" * usb init\n");
	usb_init();
	usb_do_poll();

	set_theme_colors_default();
	printf(" * sata hdd init\n");
	xenon_ata_init();

	set_theme_colors_default();
	printf(" * sata dvd init\n");
	xenon_atapi_init();

	mount_all_devices();
	int device_list_size = findDevices();
	(void)device_list_size;

	/* display some cpu info */
	set_theme_colors_default();
	printf(" * CPU PVR: %08x\n", mfspr(287));

#ifndef NO_PRINT_CONFIG
	// Section title in theme color
	set_theme_colors_default();
	printf(" * FUSES - write them down and keep them safe:\n");

	// Build fuse string
	char *fusestr = FUSES;
	for (i = 0; i < 12; ++i) {
		u64 line;
		unsigned int hi, lo;

		line = xenon_secotp_read_line(i);
		hi = (unsigned int)(line >> 32);
		lo = (unsigned int)(line & 0xffffffff);

		fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
	}

	// Print values in green (terminal style)
	set_value_colors();
	printf("%s", FUSES); // safer than printf(FUSES)

	print_cpu_dvd_keys();

	// Back to theme for network info label/output
	set_theme_colors_default();
	network_print_config();
#endif

	/* Stop logging and save it to first USB Device found that is writeable */
	LogDeInit();

	printf("\n");
	set_theme_colors_default();
	printf(" * Looking for files on local media and TFTP...\n\n");

	for (;;) {
		fileloop();
		tftp_loop(); // less likely to find something...
		console_clrline();
		mount_all_devices();
	}

	return 0;
}
