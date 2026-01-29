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

/* ========= HexaMods console palette =========
   Base  : white on black (readable)
   Accent: orange on black (branding/headings)
   Value : green on black (keys/fuses)
*/
static inline void ui_base(void)   { console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_WHITE); }
static inline void ui_accent(void) { console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_ORANGE); }
static inline void ui_value(void)  { console_set_colors(CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GREEN);  }

void do_asciiart(void)
{
    const char *p = asciiart;      // asciiart is const char* in your new asciiart.h
    while (*p)
        console_putch(*p++);

    printf("%s", asciitail);       // safe
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

    console_init();

    // Set readable base colors first (kills the blue “bars” for our text)
    ui_base();

    // Branding header (accent line + base line)
    ui_accent();
    printf("\nHexaMods XeLL RELOADED - Second Stage " LONGVERSION "\n");
    ui_base();
    printf("Coded by DAYBR3AK // RGH 4 Ever\n\n");

    // ASCII art in base (more readable than orange “thin” lines)
    ui_base();
    do_asciiart();

    xenon_sound_init();

    if (xenon_get_console_type() != REV_CORONA_PHISON) // Not needed for MMC type of consoles! ;)
    {
        ui_accent();
        printf(" * nand init\n");
        ui_base();

        sfcx_init();
        if (sfc.initialized != SFCX_INITIALIZED)
        {
            ui_accent();
            printf(" ! sfcx initialization failure\n");
            printf(" ! nand related features will not be available\n");
            ui_base();
            delay(5);
        }
    }

    xenon_config_init();

#ifndef NO_NETWORKING
    ui_accent();
    printf(" * network init\n");
    ui_base();
    network_init();

    ui_accent();
    printf(" * starting httpd server...");
    ui_base();
    httpd_start();
    ui_accent();
    printf("success\n");
    ui_base();
#endif

    ui_accent();
    printf(" * usb init\n");
    ui_base();
    usb_init();
    usb_do_poll();

    ui_accent();
    printf(" * sata hdd init\n");
    ui_base();
    xenon_ata_init();

    ui_accent();
    printf(" * sata dvd init\n");
    ui_base();
    xenon_atapi_init();

    mount_all_devices();
    (void)findDevices();

    ui_accent();
    printf(" * CPU PVR: %08x\n", mfspr(287));
    ui_base();

#ifndef NO_PRINT_CONFIG
    ui_accent();
    printf(" * FUSES - write them down and keep them safe:\n");
    ui_base();

    char *fusestr = FUSES;
    for (i = 0; i < 12; ++i) {
        u64 line;
        unsigned int hi, lo;

        line = xenon_secotp_read_line(i);
        hi = (unsigned int)(line >> 32);
        lo = (unsigned int)(line & 0xffffffff);

        fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);
    }

    // Values in green like your web theme
    ui_value();
    printf("%s", FUSES);          // safe (no format-string risk)

    // Keys also green
    print_cpu_dvd_keys();

    ui_base();
    network_print_config();
#endif

    LogDeInit();

    ui_accent();
    printf("\n * Looking for files on local media and TFTP...\n\n");
    ui_base();

    for (;;) {
        fileloop();
        tftp_loop(); // less likely to find something...
        console_clrline();
        mount_all_devices();
    }

    return 0;
}
