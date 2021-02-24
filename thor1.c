/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <thor1.h>

// ------------------------ Structures ------------------------


#include <sys/time.h>
#include <bootimg.h>
#include <zipfile/zipfile.h>
#include "fastboot.h"

void bootimg_set_cmdline(boot_img_hdr *h, const char *cmdline);
boot_img_hdr *mkbootimg(void *kernel, unsigned kernel_size,
                        void *ramdisk, unsigned ramdisk_size,
                        void *second, unsigned second_size,
                        unsigned page_size, unsigned base,
                        unsigned *bootimg_size);
static usb_handle *usb = 0;
static const char *serial = 0;
static const char *product = 0;
static const char *cmdline = 0;
static int wipe_data = 0;
static unsigned short vendor_id = 0;
static unsigned base_addr = 0x10000000;
void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr,"error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr,"\n");
    va_end(ap);
    exit(1);
}    
void get_my_path(char *path);
char *find_item(const char *item, const char *product)
{
    char *dir;
    char *fn;
    char path[PATH_MAX + 128];
    if(!strcmp(item,"boot")) {
        fn = "boot.img";
    } else if(!strcmp(item,"recovery")) {
        fn = "recovery.img";
    } else if(!strcmp(item,"system")) {
        fn = "system.img";
    } else if(!strcmp(item,"userdata")) {
        fn = "userdata.img";
    } else if(!strcmp(item,"info")) {
        fn = "android-info.txt";
    } else {
        fprintf(stderr,"unknown partition '%s'\n", item);
        return 0;
    }
    if(product) {
        get_my_path(path);
        sprintf(path + strlen(path),
                "../../../target/product/%s/%s", product, fn);
        return strdup(path);
    }
        
    dir = getenv("ANDROID_PRODUCT_OUT");
    if((dir == 0) || (dir[0] == 0)) {
        die("neither -p product specified nor ANDROID_PRODUCT_OUT set");
        return 0;
    }
    
    sprintf(path, "%s/%s", dir, fn);
    return strdup(path);
}
#ifdef _WIN32
void *load_file(const char *fn, unsigned *_sz);
#else
void *load_file(const char *fn, unsigned *_sz)
{
    char *data;
    int sz;
    int fd;
    data = 0;
    fd = open(fn, O_RDONLY);
    if(fd < 0) return 0;
    sz = lseek(fd, 0, SEEK_END);
    if(sz < 0) goto oops;
    if(lseek(fd, 0, SEEK_SET) != 0) goto oops;
    data = (char*) malloc(sz);
    if(data == 0) goto oops;
    if(read(fd, data, sz) != sz) goto oops;
    close(fd);
    if(_sz) *_sz = sz;
    return data;
oops:
    close(fd);
    if(data != 0) free(data);
    return 0;
}
#endif
int match_fastboot(usb_ifc_info *info)
{
    if(!(vendor_id && (info->dev_vendor == vendor_id)) &&
       (info->dev_vendor != 0x18d1) &&  // Google
       (info->dev_vendor != 0x0451) &&
       (info->dev_vendor != 0x22b8) &&  // Motorola
       (info->dev_vendor != 0x413c) &&  // DELL
       (info->dev_vendor != 0x0bb4))    // HTC
            return -1;
    if(info->ifc_class != 0xff) return -1;
    if(info->ifc_subclass != 0x42) return -1;
    if(info->ifc_protocol != 0x03) return -1;
    // require matching serial number if a serial number is specified
    // at the command line with the -s option.
    if (serial && strcmp(serial, info->serial_number) != 0) return -1;
    return 0;
}
int list_devices_callback(usb_ifc_info *info)
{
    if (match_fastboot(info) == 0) {
        char* serial = info->serial_number;
        if (!serial[0]) {
            serial = "????????????";
        }
        // output compatible with "adb devices"
        printf("%s\tfastboot\n", serial);
    }
    return -1;
}
usb_handle *open_device(void)
{
    static usb_handle *usb = 0;
    int announce = 1;
    if(usb) return usb;
    
    for(;;) {
        usb = usb_open(match_fastboot);
        if(usb) return usb;
        if(announce) {
            announce = 0;    
            fprintf(stderr,"< waiting for device >\n");
        }
        sleep(1);
    }
}
void list_devices(void) {
    // We don't actually open a USB device here,
    // just getting our callback called so we can
    // list all the connected devices.
    usb_open(list_devices_callback);
}
void usage(void)
{
    fprintf(stderr,
/*           1234567890123456789012345678901234567890123456789012345678901234567890123456 */
            "usage: fastboot [ <option> ] <command>\n"
            "\n"
            "commands:\n"
            "  update <filename>                        reflash device from update.zip\n"
            "  flashall                                 flash boot + recovery + system\n"
            "  flash <partition> [ <filename> ]         write a file to a flash partition\n"
            "  erase <partition>                        erase a flash partition\n"
            "  getvar <variable>                        display a bootloader variable\n"
            "  boot <kernel> [ <ramdisk> ]              download and boot kernel\n"
            "  flash:raw boot <kernel> [ <ramdisk> ]    create bootimage and flash it\n"
            "  devices                                  list all connected devices\n"
            "  reboot                                   reboot device normally\n"
            "  reboot-bootloader                        reboot device into bootloader\n"
            "\n"
            "options:\n"
            "  -w                                       erase userdata and cache\n"
            "  -s <serial number>                       specify device serial number\n"
            "  -p <product>                             specify product name\n"
            "  -c <cmdline>                             override kernel commandline\n"
            "  -i <vendor id>                           specify a custom USB vendor id\n"
            "  -b <base_addr>                           specify a custom kernel base address\n"
        );
    exit(1);
}
void *load_bootable_image(const char *kernel, const char *ramdisk, 
                          unsigned *sz, const char *cmdline)
{
    void *kdata = 0, *rdata = 0;
    unsigned ksize = 0, rsize = 0;
    void *bdata;
    unsigned bsize;
    if(kernel == 0) {
        fprintf(stderr, "no image specified\n");
        return 0;
    }
    kdata = load_file(kernel, &ksize);
    if(kdata == 0) {
        fprintf(stderr, "cannot load '%s'\n", kernel);
        return 0;
    }
    
        /* is this actually a boot image? */
    if(!memcmp(kdata, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
        if(cmdline) bootimg_set_cmdline((boot_img_hdr*) kdata, cmdline);
        
        if(ramdisk) {
            fprintf(stderr, "cannot boot a boot.img *and* ramdisk\n");
            return 0;
        }
        
        *sz = ksize;
        return kdata;
    }
    if(ramdisk) {
        rdata = load_file(ramdisk, &rsize);
        if(rdata == 0) {
            fprintf(stderr,"cannot load '%s'\n", ramdisk);
            return  0;
        }
    }
    fprintf(stderr,"creating boot image...\n");
    bdata = mkbootimg(kdata, ksize, rdata, rsize, 0, 0, 2048, base_addr, &bsize);
    if(bdata == 0) {
        fprintf(stderr,"failed to create boot.img\n");
        return 0;
    }
    if(cmdline) bootimg_set_cmdline((boot_img_hdr*) bdata, cmdline);
    fprintf(stderr,"creating boot image - %d bytes\n", bsize);
    *sz = bsize;
    
    return bdata;
}
void *unzip_file(zipfile_t zip, const char *name, unsigned *sz)
{
    void *data;
    zipentry_t entry;
    unsigned datasz;
    
    entry = lookup_zipentry(zip, name);
    if (entry == NULL) {
        fprintf(stderr, "archive does not contain '%s'\n", name);
        return 0;
    }
    *sz = get_zipentry_size(entry);
    datasz = *sz * 1.001;
    data = malloc(datasz);
    if(data == 0) {
        fprintf(stderr, "failed to allocate %d bytes\n", *sz);
        return 0;
    }
    if (decompress_zipentry(entry, data, datasz)) {
        fprintf(stderr, "failed to unzip '%s' from archive\n", name);
        free(data);
        return 0;
    }
    return data;
}
static char *strip(char *s)
{
    int n;
    while(*s && isspace(*s)) s++;
    n = strlen(s);
    while(n-- > 0) {
        if(!isspace(s[n])) break;
        s[n] = 0;
    }
    return s;
}
#define MAX_OPTIONS 32
static int setup_requirement_line(char *name)
{
    char *val[MAX_OPTIONS];
    const char **out;
    unsigned n, count;
    char *x;
    int invert = 0;
    
    if (!strncmp(name, "reject ", 7)) {
        name += 7;
        invert = 1;
    } else if (!strncmp(name, "require ", 8)) {
        name += 8;
        invert = 0;
    }
    x = strchr(name, '=');
    if (x == 0) return 0;
    *x = 0;
    val[0] = x + 1;
    for(count = 1; count < MAX_OPTIONS; count++) {
        x = strchr(val[count - 1],'|');
        if (x == 0) break;
        *x = 0;
        val[count] = x + 1;
    }
    
    name = strip(name);
    for(n = 0; n < count; n++) val[n] = strip(val[n]);
    
    name = strip(name);
    if (name == 0) return -1;
        /* work around an unfortunate name mismatch */
    if (!strcmp(name,"board")) name = "product";
    out = malloc(sizeof(char*) * count);
    if (out == 0) return -1;
    for(n = 0; n < count; n++) {
        out[n] = strdup(strip(val[n]));
        if (out[n] == 0) return -1;
    }
    fb_queue_require(name, invert, n, out);
    return 0;
}
static void setup_requirements(char *data, unsigned sz)
{
    char *s;
    s = data;
    while (sz-- > 0) {
        if(*s == '\n') {
            *s++ = 0;
            if (setup_requirement_line(data)) {
                die("out of memory");
            }
            data = s;
        } else {
            s++;
        }
    }
}
void queue_info_dump(void)
{
    fb_queue_notice("--------------------------------------------");
    fb_queue_display("version-bootloader", "Bootloader Version...");
    fb_queue_display("version-baseband",   "Baseband Version.....");
    fb_queue_display("serialno",           "Serial Number........");
    fb_queue_notice("--------------------------------------------");
}
void do_update_signature(zipfile_t zip, char *fn)
{
    void *data;
    unsigned sz;
    data = unzip_file(zip, fn, &sz);
    if (data == 0) return;
    fb_queue_download("signature", data, sz);
    fb_queue_command("signature", "installing signature");
}
void do_update(char *fn)
{
    void *zdata;
    unsigned zsize;
    void *data;
    unsigned sz;
    zipfile_t zip;
    queue_info_dump();
    zdata = load_file(fn, &zsize);
    if (zdata == 0) die("failed to load '%s'", fn);
    zip = init_zipfile(zdata, zsize);
    if(zip == 0) die("failed to access zipdata in '%s'");
    data = unzip_file(zip, "android-info.txt", &sz);
    if (data == 0) {
        char *tmp;
            /* fallback for older zipfiles */
        data = unzip_file(zip, "android-product.txt", &sz);
        if ((data == 0) || (sz < 1)) {
            die("update package has no android-info.txt or android-product.txt");
        }
        tmp = malloc(sz + 128);
        if (tmp == 0) die("out of memory");
        sprintf(tmp,"board=%sversion-baseband=0.66.04.19\n",(char*)data);
        data = tmp;
        sz = strlen(tmp);
    }
    setup_requirements(data, sz);
    data = unzip_file(zip, "boot.img", &sz);
    if (data == 0) die("update package missing boot.img");
    do_update_signature(zip, "boot.sig");
    fb_queue_flash("boot", data, sz);
    data = unzip_file(zip, "recovery.img", &sz);
    if (data != 0) {
        do_update_signature(zip, "recovery.sig");
        fb_queue_flash("recovery", data, sz);
    }
    data = unzip_file(zip, "system.img", &sz);
    if (data == 0) die("update package missing system.img");
    do_update_signature(zip, "system.sig");
    fb_queue_flash("system", data, sz);
}
void do_send_signature(char *fn)
{
    void *data;
    unsigned sz;
    char *xtn;
	
    xtn = strrchr(fn, '.');
    if (!xtn) return;
    if (strcmp(xtn, ".img")) return;
	
    strcpy(xtn,".sig");
    data = load_file(fn, &sz);
    strcpy(xtn,".img");
    if (data == 0) return;
    fb_queue_download("signature", data, sz);
    fb_queue_command("signature", "installing signature");
}
void do_flashall(void)
{
    char *fname;
    void *data;
    unsigned sz;
    queue_info_dump();
    fname = find_item("info", product);
    if (fname == 0) die("cannot find android-info.txt");
    data = load_file(fname, &sz);
    if (data == 0) die("could not load android-info.txt");
    setup_requirements(data, sz);
    fname = find_item("boot", product);
    data = load_file(fname, &sz);
    if (data == 0) die("could not load boot.img");
    do_send_signature(fname);
    fb_queue_flash("boot", data, sz);
    fname = find_item("recovery", product);
    data = load_file(fname, &sz);
    if (data != 0) {
        do_send_signature(fname);
        fb_queue_flash("recovery", data, sz);
    }
    fname = find_item("system", product);
    data = load_file(fname, &sz);
    if (data == 0) die("could not load system.img");
    do_send_signature(fname);
    fb_queue_flash("system", data, sz);   
}
#define skip(n) do { argc -= (n); argv += (n); } while (0)
#define require(n) do { if (argc < (n)) usage(); } while (0)
int do_oem_command(int argc, char **argv)
{
    int i;
    char command[256];
    if (argc <= 1) return 0;
    
    command[0] = 0;
    while(1) {
        strcat(command,*argv);
        skip(1);
        if(argc == 0) break;
        strcat(command," ");
    }
    fb_queue_command(command,"");    
    return 0;
}
int main(int argc, char **argv)
{
    int wants_wipe = 0;
    int wants_reboot = 0;
    int wants_reboot_bootloader = 0;
    void *data;
    unsigned sz;
    skip(1);
    if (argc == 0) {
        usage();
        return 0;
    }
    if (!strcmp(*argv, "devices")) {
        list_devices();
        return 0;
    }
    while (argc > 0) {
        if(!strcmp(*argv, "-w")) {
            wants_wipe = 1;
            skip(1);
        } else if(!strcmp(*argv, "-b")) {
            require(2);
            base_addr = strtoul(argv[1], 0, 16);
            skip(2);
        } else if(!strcmp(*argv, "-s")) {
            require(2);
            serial = argv[1];
            skip(2);
        } else if(!strcmp(*argv, "-p")) {
            require(2);
            product = argv[1];
            skip(2);
        } else if(!strcmp(*argv, "-c")) {
            require(2);
            cmdline = argv[1];
            skip(2);
        } else if(!strcmp(*argv, "-i")) {
            char *endptr = NULL;
            unsigned long val;
            require(2);
            val = strtoul(argv[1], &endptr, 0);
            if (!endptr || *endptr != '\0' || (val & ~0xffff))
                die("invalid vendor id '%s'", argv[1]);
            vendor_id = (unsigned short)val;
            skip(2);
        } else if(!strcmp(*argv, "getvar")) {
            require(2);
            fb_queue_display(argv[1], argv[1]);
            skip(2);
        } else if(!strcmp(*argv, "erase")) {
            require(2);
            fb_queue_erase(argv[1]);
            skip(2);
        } else if(!strcmp(*argv, "signature")) {
            require(2);
            data = load_file(argv[1], &sz);
            if (data == 0) die("could not load '%s'", argv[1]);
            if (sz != 256) die("signature must be 256 bytes");
            fb_queue_download("signature", data, sz);
            fb_queue_command("signature", "installing signature");
            skip(2);
        } else if(!strcmp(*argv, "reboot")) {
            wants_reboot = 1;
            skip(1);
        } else if(!strcmp(*argv, "reboot-bootloader")) {
            wants_reboot_bootloader = 1;
            skip(1);
        } else if (!strcmp(*argv, "continue")) {
            fb_queue_command("continue", "resuming boot");
            skip(1);
        } else if(!strcmp(*argv, "boot")) {
            char *kname = 0;
            char *rname = 0;
            skip(1);
            if (argc > 0) {
                kname = argv[0];
                skip(1);
            }
            if (argc > 0) {
                rname = argv[0];
                skip(1);
            }
            data = load_bootable_image(kname, rname, &sz, cmdline);
            if (data == 0) return 1;
            fb_queue_download("boot.img", data, sz);
            fb_queue_command("boot", "booting");
        } else if(!strcmp(*argv, "flash")) {
            char *pname = argv[1];
            char *fname = 0;
            require(2);
            if (argc > 2) {
                fname = argv[2];
                skip(3);
            } else {
                fname = find_item(pname, product);
                skip(2);
            }
            if (fname == 0) die("cannot determine image filename for '%s'", pname);
            data = load_file(fname, &sz);
            if (data == 0) die("cannot load '%s'\n", fname);
            fb_queue_flash(pname, data, sz);
        } else if(!strcmp(*argv, "flash:raw")) {
            char *pname = argv[1];
            char *kname = argv[2];
            char *rname = 0;
            require(3);
            if(argc > 3) {
                rname = argv[3];
                skip(4);
            } else {
                skip(3);
            }
            data = load_bootable_image(kname, rname, &sz, cmdline);
            if (data == 0) die("cannot load bootable image");
            fb_queue_flash(pname, data, sz);
        } else if(!strcmp(*argv, "flashall")) {
            skip(1);
            do_flashall();
            wants_reboot = 1;
        } else if(!strcmp(*argv, "update")) {
            if (argc > 1) {
                do_update(argv[1]);
                skip(2);
            } else {
                do_update("update.zip");
                skip(1);
            }
            wants_reboot = 1;
        } else if(!strcmp(*argv, "oem")) {
            argc = do_oem_command(argc, argv);
        } else {
            usage();
        }
    }
    if (wants_wipe) {
        fb_queue_erase("userdata");
        fb_queue_erase("cache");
    }
    if (wants_reboot) {
        fb_queue_reboot();
    } else if (wants_reboot_bootloader) {
        fb_queue_command("reboot-bootloader", "rebooting into bootloader");
    }
    usb = open_device();
    fb_execute_queue(usb);
    return 0;
}

// ---------------- Integer Types Definitions -----------------

typedef int64_t int80_t;

// ----------------- Float Types Definitions ------------------

typedef double float64_t;
typedef long double float80_t;

// ------------------------ Structures ------------------------

struct _IO_FILE {
    int32_t e0;
};

struct _LIST_ENTRY {
    struct _LIST_ENTRY * e0;
    struct _LIST_ENTRY * e1;
};

struct _LIST_ENTRY {
    struct _LIST_ENTRY * e0;
    struct _LIST_ENTRY * e1;
};

struct _RTL_CRITICAL_SECTION {
    struct _RTL_CRITICAL_SECTION_DEBUG * e0;
    int32_t e1;
    int32_t e2;
    int32_t * e3;
    int32_t * e4;
    int32_t e5;
};

struct _RTL_CRITICAL_SECTION_DEBUG {
    int16_t e0;
    int16_t e1;
    struct _RTL_CRITICAL_SECTION * e2;
    struct _LIST_ENTRY e3;
    int32_t e4;
    int32_t e5;
    int32_t e6;
    int16_t e7;
    int16_t e8;
};

struct _TYPEDEF_GUID {
    int32_t e0;
    int16_t e1;
    int16_t e2;
    char e3[8];
};

struct _SP_DEVINFO_DATA {
    int32_t e0;
    struct _TYPEDEF_GUID e1;
    int32_t e2;
    int32_t e3;
};

struct lconv {
    char * e0;
    char * e1;
    char * e2;
    char * e3;
    char * e4;
    char * e5;
    char * e6;
    char * e7;
    char * e8;
    char * e9;
    char e10;
    char e11;
    char e12;
    char e13;
    char e14;
    char e15;
    char e16;
    char e17;
    char e18;
    char e19;
    char e20;
    char e21;
    char e22;
    char e23;
};

// ------------------- Function Prototypes --------------------

int32_t ___b2d_D2A(int32_t a1, int32_t a2);
int32_t ___Balloc_D2A(uint32_t a1);
int32_t ___Bfree_D2A(int32_t a1);
int32_t ___chkstk(int32_t * a1);
int32_t ___cmp_D2A(int32_t a1, int32_t a2);
int32_t ___diff_D2A(int32_t a1, int32_t a2);
int32_t ___fpclassify(int32_t a1);
int32_t ___freedtoa(int32_t a1);
int32_t ___gcc_register_frame(int32_t a1, int32_t a2, int32_t a3);
int32_t ___gdtoa(int32_t * a1, int32_t a2, int32_t * a3, int32_t * a4, int32_t a5, int32_t a6, int32_t a7, int32_t * a8);
int32_t ___i2b_D2A(int32_t a1);
int32_t ___lshift_D2A(int32_t a1, uint32_t a2);
int32_t ___main(void);
int32_t ___mbrtowc_cp(int32_t a1, int32_t CodePage, uint32_t a3);
int32_t ___mingw_pformat(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5);
int32_t ___mult_D2A(int32_t a1, int32_t a2);
int32_t ___multadd_D2A(int32_t a1, int32_t a2, int32_t a3);
int32_t ___nrv_alloc_D2A(int32_t a1, int32_t a2, int32_t a3);
int32_t ___pformat_cvt(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t * a6, int32_t * a7, int32_t a8);
int32_t ___pformat_efloat(int32_t a1, int32_t a2, int32_t a3);
int32_t ___pformat_emit_efloat(int32_t a1);
int32_t ___pformat_emit_float(int32_t a1);
int32_t ___pformat_emit_inf_or_nan(void);
int32_t ___pformat_emit_radix_point(void);
int32_t ___pformat_emit_xfloat(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5);
int32_t ___pformat_float(int32_t a1, int32_t a2, int32_t a3);
int32_t ___pformat_gfloat(int32_t a1, int32_t a2, int32_t a3);
int32_t ___pformat_int(void);
int32_t ___pformat_putc(void);
int32_t ___pformat_putchars(void);
int32_t ___pformat_wputchars(void);
int32_t ___pformat_xint(int32_t a1);
int32_t ___pow5mult_D2A(int32_t a1, uint32_t a2);
int32_t ___quorem_D2A(int32_t a1, int32_t a2);
int32_t ___rshift_D2A(int32_t a1, uint32_t a2, int32_t a3, int32_t a4);
int32_t ___rv_alloc_D2A(uint32_t a1);
int32_t ___trailz_D2A(int32_t a1);
int32_t ___udivdi3(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
int32_t ___umoddi3(uint32_t result2, uint32_t a2, uint32_t a3, uint32_t a4);
int32_t ___wcrtomb_cp(int32_t cbMultiByte);
int32_t __get_output_format(void);
int32_t _blank_flash_device(int32_t a1, int32_t a2, int32_t a3, int32_t a4);
int32_t _dtoa_lock(void);
int32_t _dtoa_unlock(void);
int32_t _extract_id(int32_t a1, int32_t str);
int32_t _getopt_long(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5);
int32_t _getopt_parse(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6);
int32_t _list_devices(void);
int32_t _mbrtowc(int32_t * a1, int32_t a2, int32_t a3, int32_t * a4);
int32_t _msleep(int32_t dwMilliseconds);
int32_t _qb_blank_flash(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5);
int32_t _qb_describe_error(int32_t a1);
int32_t _qb_get_version(int32_t * a1, int32_t * a2);
int32_t _serial_enum_devices(int32_t a1);
int32_t _snprintf(int32_t a1, int32_t a2, int32_t a3);
int32_t _strcasestr(int32_t result, int32_t a2);
int32_t _strncasecmp(void);
int32_t _usage(void);
int32_t _version(int32_t a1);
int32_t _vsnprintf(int32_t a1, int32_t a2, int32_t a3, int32_t * a4);
int32_t _wait_for_device(void);
int32_t _wcrtomb(int32_t a1, int32_t a2);
int32_t atexit2(void (*pfn)());

// --------------------- Global Variables ---------------------

int32_t g1 = 0; // 0x408000
int32_t g2 = 1; // 0x408010
int32_t g3 = 1; // 0x408014
char * g4 = "?"; // 0x408018
int32_t g5 = 64; // 0x40801c
int32_t g7 = 0; // 0x408034
char (*g8)[5] = "port"; // 0x4095a0
int32_t g9 = -0x13ffffd0; // 0x4098fd
float64_t * g10 = (float64_t *)0x37e08000; // 0x409980
float64_t * g11 = NULL; // 0x4099f8
float64_t * g12 = NULL; // 0x409a00
int32_t g13 = 0x4480f0cf; // 0x409ab4
int32_t g14 = 0; // 0x40a018
int32_t g15 = 0; // 0x40a028
int32_t g16 = 0; // 0x40a038
int32_t g17 = 0; // 0x40a03c
int32_t g18 = 0; // 0x40a040
int32_t g19 = 0; // 0x40a0c0
int32_t g20 = 0; // 0x40a158
int32_t g21 = 0; // 0x40a15c
int32_t g22 = 0; // 0x40a160
char * g23; // 0x40a170
int32_t g24 = 0; // 0x40a180
int32_t g25 = 0; // 0x40a190
char g26 = 0; // 0x40a1a0
int32_t g27 = 0; // 0x40a1d0
int32_t g28 = 0; // 0x40a1e0
struct _RTL_CRITICAL_SECTION * g29 = NULL; // 0x40a1f0
int32_t g30 = 0; // 0x40a220
int32_t g31 = 0; // 0x40a260
int32_t g32 = 0; // 0x40ab60
int32_t g33;
int32_t g34;
int32_t g35;
int32_t g36;
int32_t * g6 = &g31; // 0x408030

// ------------------------ Functions -------------------------

// From module:   /build/buildd/mingw32-runtime-3.15.2/build_dir/src/mingwrt-3.15.2-mingw32\crt1.c
// Address range: 0x401000 - 0x40100c
// Line range:    280 - 281
int32_t atexit2(void (*pfn)()) {
    // 0x401000
    return atexit((void (*)())&g36);
}

// Address range: 0x401290 - 0x4012dc
int32_t ___gcc_register_frame(int32_t a1, int32_t a2, int32_t a3) {
    // 0x401290
    if (g7 == 0) {
        // 0x4012da
        return 0;
    }
    int32_t * moduleHandle = GetModuleHandleA("libgcj_s.dll"); // 0x4012a7
    if (moduleHandle == NULL) {
        // 0x4012da
        return 0;
    }
    int32_t v1 = 0; // bp-24, 0x4012bb
    int32_t (*func)() = GetProcAddress(moduleHandle, "_Jv_RegisterClasses"); // 0x4012c2
    int32_t result = 0; // 0x4012cb
    if (func != NULL) {
        // 0x4012cd
        *(int32_t *)((int32_t)&v1 - 16) = (int32_t)&g7;
        result = (int32_t)func;
    }
    // 0x4012da
    return result;
}

// Address range: 0x401320 - 0x401395
int32_t ___main(void) {
    // 0x401320
    if (g1 != 0) {
        // 0x40132f
        int32_t result; // 0x401320
        return result;
    }
    int32_t v1 = *(int32_t *)0x407218; // 0x401336
    g1 = 1;
    int32_t v2; // 0x401320
    int32_t v3; // 0x401320
    int32_t v4; // 0x401320
    ___gcc_register_frame(v4, v2, v3);
    int32_t v5 = v1; // 0x40134e
    if (v1 == -1) {
        int32_t v6 = 0;
        int32_t v7 = v6 + 1; // 0x401385
        v5 = v6;
        while (*(int32_t *)(4 * v7 + 0x407218) != 0) {
            // 0x401385
            v6 = v7;
            v7 = v6 + 1;
            v5 = v6;
        }
    }
    // 0x401350
    if (v5 == 0) {
        // 0x401368
        return atexit2((void (*)())0x4012f0);
    }
    int32_t v8 = v5; // 0x401352
    while (v8 != 1) {
        // 0x401360
        v8--;
    }
    // 0x401368
    return atexit2((void (*)())0x4012f0);
}

// Address range: 0x4013a0 - 0x401415
int32_t _version(int32_t a1) {
    int32_t v1 = 0; // bp-8, 0x4013a6
    int32_t v2 = 0; // bp-12, 0x4013ad
    fprintf((struct _IO_FILE *)(*(int32_t *)0x40b1dc + 64), "Motorola qboot utility version %x.%x\n", 2, 4);
    int32_t result = _qb_get_version(&v1, &v2); // 0x4013d9
    if (result != 0) {
        // 0x401413
        return result;
    }
    int32_t chars_printed = 4; // 0x4013eb
    if (v1 == 2 != (v2 == 4)) {
        int32_t v3 = *(int32_t *)0x40b1dc; // 0x4013fb
        chars_printed = fprintf((struct _IO_FILE *)(v3 + 64), "DLL version: %x.%x\n", v1, v2);
    }
    // 0x401413
    return chars_printed;
}

// Address range: 0x401415 - 0x40143a
int32_t _usage(void) {
    int32_t v1 = *(int32_t *)0x40b1dc; // 0x40141b
    return fwrite((int32_t *)"usage: qboot [ <option> ] <command>\n\ncommands:\n  devices                                       list connected devices\n  blank-flash [ <programmer> [ <singleimage> ]] blank flash device\n\noptions:\n  -p <port>, --port=<port>  specify device port\n                            This is needed only when the program does not detect\n                            the device automatically or when multiple devices in\n                            blank flash mode are connected\n\n                            Set --port to be the full or any unambiguous part of\n                            a device pathname. For example:\n                            --port=100\n                            --port=COM100\n                            --port=ttyUSB3\n                            --port=/dev/ttyUSB3\n                            --port=/dev/tty.usbtoserial\n  --debug[=<level>]         enable debugging\n                            1(default): show debug messages\n                            2: also dump raw packets\n  -h, --help                show help screen\n  -v, --version             show version info\n\nexamples:\n  qboot devices             list all connected devices\n  qboot blank-flash         blank flash device\n", 1, 1196, (struct _IO_FILE *)(v1 + 64));
}

// Address range: 0x401502 - 0x40155b
int32_t _blank_flash_device(int32_t a1, int32_t a2, int32_t a3, int32_t a4) {
    int32_t result = _qb_blank_flash(a1, a2, a3, 0x40143a, a4); // 0x40151c
    if (result != 0) {
        int32_t v1 = _qb_describe_error(result); // 0x401533
        fprintf((struct _IO_FILE *)(*(int32_t *)0x40b1dc + 64), "FAILED (%s)\n", (char *)v1);
    }
    // 0x401556
    return result;
}

// Address range: 0x4015eb - 0x401671
int32_t _wait_for_device(void) {
    int32_t result = g15; // 0x4015f8
    if (result != 0) {
        // 0x40166c
        return result;
    }
    // 0x40160b
    int32_t v1; // bp-28, 0x4015eb
    int32_t v2 = &v1; // 0x4015ee
    int32_t * v3 = (int32_t *)(v2 - 16); // 0x40160e
    *v3 = 0x4015cf;
    int32_t result2 = _serial_enum_devices((int32_t)&g36); // 0x401613
    g15 = result2;
    if (result2 != 0) {
        // 0x40166c
        return result2;
    }
    // 0x401633
    *(int32_t *)(v2 - 4) = *(int32_t *)0x40b1dc + 64;
    *(int32_t *)(v2 - 8) = 23;
    *(int32_t *)(v2 - 12) = 1;
    *v3 = (int32_t)"< waiting for device >\n";
    fwrite(NULL, (int32_t)&g36, (int32_t)&g36, (struct _IO_FILE *)&g36);
    *v3 = 500;
    _msleep((int32_t)&g36);
    *v3 = 0x4015cf;
    int32_t result3 = _serial_enum_devices((int32_t)&g36); // 0x401613
    g15 = result3;
    while (result3 == 0) {
        // 0x40165a
        *v3 = 500;
        _msleep((int32_t)&g36);
        *v3 = 0x4015cf;
        result3 = _serial_enum_devices((int32_t)&g36);
        g15 = result3;
    }
    // 0x40166c
    return result3;
}

// Address range: 0x401671 - 0x401687
int32_t _msleep(int32_t dwMilliseconds) {
    // 0x401671
    Sleep(dwMilliseconds);
    return &g36;
}

// Address range: 0x4016e7 - 0x4016ff
int32_t _list_devices(void) {
    // 0x4016e7
    return _serial_enum_devices(0x401687);
}

// Address range: 0x4016ff - 0x40193f
int main(int argc, char ** argv) {
    // 0x4016ff
    ___main();
    setvbuf((struct _IO_FILE *)(*(int32_t *)0x40b1dc + 64), NULL, 4, 0);
    int32_t v1; // bp-64, 0x4016ff
    int32_t v2 = &v1; // 0x40173a
    int32_t * v3 = (int32_t *)(v2 - 16);
    int32_t * v4 = (int32_t *)(v2 - 20); // 0x401742
    int32_t * v5 = (int32_t *)(v2 - 24); // 0x401747
    int32_t v6; // 0x4016ff
    int32_t * v7 = (int32_t *)((int32_t)&v6 + 4);
    int32_t * v8 = (int32_t *)(v2 - 28); // 0x40174f
    int32_t * v9 = (int32_t *)(v2 - 32); // 0x401755
    int32_t * v10 = (int32_t *)(v2 - 8);
    int32_t * v11 = (int32_t *)(v2 - 12);
    *v3 = 0;
    *v4 = (int32_t)&g8;
    *v5 = (int32_t)"p:d:hv";
    *v8 = *v7;
    *v9 = v6;
    int32_t v12 = _getopt_long((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x401757
    char * v13 = (char *)v12;
    char * v14 = v13; // 0x401766
    int32_t v15 = v12; // 0x401766
    int32_t v16 = 0; // 0x401766
    int32_t v17 = 0; // 0x401766
    char * str2 = v13; // 0x401766
    int32_t v18; // 0x4016ff
    if (v12 >= 0) {
        while (true) {
          lab_0x40176c_2:;
            int32_t v19 = v16;
            char * v20 = v14; // 0x4016ff
            int32_t v21 = v15; // 0x4016ff
            char * str; // 0x4016ff
            int32_t v22; // 0x4016ff
            while (true) {
              lab_0x40176c:
                // 0x40176c
                str = v20;
                if (str == (char *)104) {
                    // break (via goto) -> 0x4017f7
                    goto lab_0x4017f7;
                }
                // 0x401778
                v22 = v21;
                if (str <= (char *)104) {
                    // break -> 0x40177e
                    break;
                }
                switch (v22) {
                    case 112: {
                        // 0x4017a4
                        g14 = g20;
                        *v3 = 0;
                        *v4 = (int32_t)&g8;
                        *v5 = (int32_t)"p:d:hv";
                        *v8 = *v7;
                        *v9 = v6;
                        int32_t v23 = _getopt_long((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x401757
                        char * v24 = (char *)v23;
                        v20 = v24;
                        v21 = v23;
                        v17 = v19;
                        str2 = v24;
                        if (v23 < 0) {
                            goto lab_0x40182a_2;
                        }
                        goto lab_0x40176c;
                    }
                    case 118: {
                        // 0x401808
                        _version(118);
                        // 0x401934
                        return 0;
                    }
                    default: {
                        // 0x401825
                        abort();
                        // UNREACHABLE
                    }
                }
            }
            // 0x40177e
            switch (v22) {
                case 63: {
                    return -1;
                }
                case 100: {
                    // 0x4017b0
                    if (g20 == 0) {
                        // .thread
                        v18 = v19 | 1;
                        goto lab_0x40173d;
                    } else {
                        // 0x4017da
                        *v10 = 0;
                        *v11 = 0;
                        *v3 = g20;
                        int32_t str_as_ul = strtoul(str, (char **)&g36, (int32_t)&g36); // 0x4017c6
                        int32_t v25 = v19; // 0x4016ff
                        if (str_as_ul == 0) {
                            // .thread
                            v18 = v25;
                            goto lab_0x40173d;
                        } else {
                            v25 = v19 | 1;
                            v18 = v19 | 3;
                            if (str_as_ul == 1) {
                                // .thread
                                v18 = v25;
                                goto lab_0x40173d;
                            } else {
                                goto lab_0x40173d;
                            }
                        }
                    }
                }
                default: {
                    // 0x401825
                    abort();
                    // UNREACHABLE
                }
            }
        }
      lab_0x4017f7:
        // 0x4017f7
        _usage();
        // 0x401934
        return 0;
    }
  lab_0x40182a_2:
    // 0x40182a
    v6 -= g2;
    int32_t v26 = *v7 + 4 * g2; // 0x40183f
    *v7 = v26;
    if (v6 == 0) {
        // 0x40184a
        _usage();
        // 0x401934
        return -1;
    }
    // 0x40185b
    *v11 = (int32_t)"devices";
    *v3 = *(int32_t *)v26;
    if (strcmp(str2, (char *)&g36) == 0) {
        // 0x401878
        _list_devices();
        // 0x401934
        return 0;
    }
    // 0x401889
    g15 = _wait_for_device();
    *v11 = (int32_t)"blank-flash";
    *v3 = *(int32_t *)*v7;
    if (strcmp((char *)&g36, (char *)&g36) != 0) {
        // 0x401906
        *v10 = *(int32_t *)*v7;
        *v11 = (int32_t)"Invalid command: %s\n";
        *v3 = *(int32_t *)0x40b1dc + 64;
        fprintf((struct _IO_FILE *)&g36, (char *)&g36);
        _usage();
        // 0x401934
        return -1;
    }
    int32_t v27 = 0; // 0x4018c4
    int32_t v28 = 0; // 0x4018c4
    if (v6 >= 2) {
        int32_t v29 = *v7; // 0x4018c9
        int32_t v30 = *(int32_t *)(v29 + 4); // 0x4018cf
        v27 = v30;
        v28 = 0;
        if (v6 != 2) {
            // 0x4018dc
            v27 = v30;
            v28 = *(int32_t *)(v29 + 8);
        }
    }
    // 0x4018ea
    *(int32_t *)(v2 - 4) = v17;
    *v10 = v28;
    *v11 = v27;
    *v3 = g15;
    int32_t result = _blank_flash_device(v27, v28, (int32_t)&g36, (int32_t)&g36); // 0x4018f9
    // 0x401934
    return result;
  lab_0x40173d:
    // 0x40173d
    v16 = v18;
    *v3 = 0;
    *v4 = (int32_t)&g8;
    *v5 = (int32_t)"p:d:hv";
    *v8 = *v7;
    *v9 = v6;
    v15 = _getopt_long((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
    v14 = (char *)v15;
    v17 = v16;
    str2 = v14;
    if (v15 < 0) {
        goto lab_0x40182a_2;
    }
    goto lab_0x40176c_2;
}

// Address range: 0x401940 - 0x4019e2
int32_t _strcasestr(int32_t result, int32_t a2) {
    unsigned char c = *(char *)a2; // 0x401949
    if (c == 0) {
        // 0x4019dd
        return result;
    }
    int32_t str = a2 + 1; // 0x401955
    int32_t v1 = tolower((int32_t)c); // 0x401968
    int32_t len = strlen((char *)str); // 0x401979
    char c2 = *(char *)result; // 0x401987
    if (c2 == 0) {
        // 0x4019dd
        return 0;
    }
    // 0x4019a3
    int32_t v2; // bp-28, 0x401940
    int32_t v3 = &v2; // 0x401943
    int32_t * v4 = (int32_t *)(v3 - 16); // 0x4019ae
    int32_t v5 = result + 1;
    *v4 = (int32_t)c2;
    if ((char)v1 == (char)tolower((int32_t)c2)) {
        // 0x4019bc
        *(int32_t *)(v3 - 8) = len;
        *(int32_t *)(v3 - 12) = str;
        *v4 = v5;
        if (_strncasecmp() == 0) {
            // break -> 0x4019dd
            break;
        }
    }
    char c3 = *(char *)v5; // 0x401987
    int32_t result2 = 0; // 0x401998
    while (c3 != 0) {
        int32_t v6 = v5;
        v5 = v6 + 1;
        *v4 = (int32_t)c3;
        if ((char)v1 == (char)tolower((int32_t)c3)) {
            // 0x4019bc
            *(int32_t *)(v3 - 8) = len;
            *(int32_t *)(v3 - 12) = str;
            *v4 = v5;
            result2 = v6;
            if (_strncasecmp() == 0) {
                // break -> 0x4019dd
                break;
            }
        }
        // 0x401984
        c3 = *(char *)v5;
        result2 = 0;
    }
    // 0x4019dd
    return result2;
}

// Address range: 0x4019e4 - 0x401a36
int32_t _extract_id(int32_t a1, int32_t str) {
    int32_t v1 = _strcasestr(a1, str); // 0x4019f3
    int32_t str_as_ul = 0; // 0x401a02
    if (v1 != 0) {
        // 0x401a04
        str_as_ul = strtoul((char *)(strlen((char *)str) + v1), NULL, 16);
    }
    // 0x401a31
    return str_as_ul;
}

// Address range: 0x401a36 - 0x401c78
int32_t _serial_enum_devices(int32_t a1) {
    // 0x401a36
    int32_t v1; // bp-36, 0x401a36
    int32_t hKey; // bp-40, 0x401a36
    if (!SetupDiClassGuidsFromNameA("PORTS", (struct _TYPEDEF_GUID *)&v1, 1, &hKey)) {
        // 0x401c70
        return 0;
    }
    struct _TYPEDEF_GUID * v2 = (struct _TYPEDEF_GUID *)&v1; // bp-364, 0x401a86
    int32_t * v3 = SetupDiGetClassDevsA((struct _TYPEDEF_GUID *)&v1, NULL, NULL, 10); // 0x401a8c
    if (v3 == (int32_t *)-1) {
        // 0x401c70
        return 0;
    }
    int32_t v4 = (int32_t)v3; // 0x401a8c
    int32_t v5 = (int32_t)&v2; // 0x401a86
    int32_t v6 = 28; // bp-68, 0x401aa6
    int32_t v7 = &v6; // 0x401c21
    *(int32_t *)(v5 - 8) = v7;
    int32_t v8 = v5 - 12; // 0x401c22
    *(int32_t *)v8 = 0;
    *(int32_t *)(v5 - 16) = v4;
    bool v9 = SetupDiEnumDeviceInfo((int32_t *)1, (int32_t)&g36, (struct _SP_DEVINFO_DATA *)&g36); // 0x401c2b
    int32_t result = 0; // 0x401c32
    int32_t v10 = v8; // 0x401c32
    if (v9) {
        int32_t v11 = &hKey;
        int32_t v12; // bp-196, 0x401a36
        int32_t v13 = &v12;
        int32_t v14; // bp-324, 0x401a36
        int32_t v15 = &v14;
        int32_t v16 = 1;
        int32_t * v17 = (int32_t *)(v5 - 20); // 0x401ab5
        *v17 = 264;
        int32_t * v18 = (int32_t *)(v5 - 24); // 0x401aba
        *v18 = 0;
        int32_t * v19 = (int32_t *)(v5 - 28); // 0x401abc
        *v19 = (int32_t)&g16;
        memset(&g36, (int32_t)&g36, (int32_t)&g36);
        *v17 = v11;
        *v18 = 128;
        *v19 = v13;
        *(int32_t *)(v5 - 32) = 0;
        *(int32_t *)(v5 - 36) = 1;
        int32_t v20 = v5 - 40; // 0x401ae3
        *(int32_t *)v20 = v7;
        *(int32_t *)(v5 - 44) = v4;
        bool v21 = SetupDiGetDeviceRegistryPropertyA(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, &g36, (char *)&g36, (int32_t)&g36, &g36); // 0x401aec
        int32_t v22 = v20; // 0x401af3
        int32_t * v23; // 0x401a36
        int32_t * v24; // 0x401afc
        int32_t * v25; // 0x401b07
        int32_t v26; // 0x401b24
        int32_t v27; // 0x401b64
        bool v28; // 0x401b6d
        int32_t v29; // 0x401b86
        int32_t * v30; // 0x401b94
        int32_t v31; // 0x401b94
        int32_t v32; // 0x401bbb
        int32_t * v33; // 0x401bbb
        int32_t * v34; // 0x401bc0
        int32_t v35; // 0x401bc3
        int32_t * v36; // 0x401a36
        if (v21) {
            // 0x401af9
            v24 = (int32_t *)(v5 - 52);
            *v24 = (int32_t)"VID_";
            v25 = (int32_t *)(v5 - 56);
            *v25 = v13;
            g16 = _extract_id((int32_t)&g36, (int32_t)&g36);
            *v24 = (int32_t)"PID_";
            *v25 = v13;
            v26 = _extract_id((int32_t)&g36, (int32_t)&g36);
            g17 = v26;
            v22 = v20;
            if (v26 != 0 && g16 != 0) {
                // 0x401b4b
                *(int32_t *)(v5 - 48) = v11;
                *v24 = 128;
                *v25 = (int32_t)&g18;
                *(int32_t *)(v5 - 60) = 0;
                *(int32_t *)(v5 - 64) = 12;
                v27 = v5 - 68;
                *(int32_t *)v27 = v7;
                *(int32_t *)(v5 - 72) = v4;
                v28 = SetupDiGetDeviceRegistryPropertyA(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, &g36, (char *)&g36, (int32_t)&g36, &g36);
                v22 = v27;
                if (v28) {
                    // 0x401b7a
                    *(int32_t *)(v5 - 80) = 0x20019;
                    *(int32_t *)(v5 - 84) = 1;
                    *(int32_t *)(v5 - 88) = 0;
                    v29 = v5 - 92;
                    *(int32_t *)v29 = 1;
                    *(int32_t *)(v5 - 96) = v7;
                    *(int32_t *)(v5 - 100) = v4;
                    v30 = SetupDiOpenDevRegKey(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                    v22 = v29;
                    if (v30 != (int32_t *)-1) {
                        // 0x401ba2
                        v31 = (int32_t)v30;
                        hKey = 128;
                        *(int32_t *)(v5 - 104) = v11;
                        *(int32_t *)(v5 - 108) = v15;
                        *(int32_t *)(v5 - 112) = 0;
                        *(int32_t *)(v5 - 116) = 0;
                        v32 = v5 - 120;
                        v33 = (int32_t *)v32;
                        *v33 = (int32_t)"PortName";
                        v34 = (int32_t *)(v5 - 124);
                        *v34 = v31;
                        v35 = RegQueryValueExA((int32_t *)hKey, (char *)&g36, &g36, &g36, (char *)&g36, &g36);
                        if (v35 != 0) {
                            // 0x401ba2
                            v23 = (int32_t *)(v5 - 132);
                        } else {
                            // 0x401bcf
                            *v33 = v15;
                            *v34 = (int32_t)"\\\\.\\%s";
                            *(int32_t *)(v5 - 128) = 127;
                            v36 = (int32_t *)(v5 - 132);
                            *v36 = (int32_t)&g19;
                            _snprintf((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                            v23 = v36;
                        }
                        // 0x401bea
                        *v23 = v31;
                        RegCloseKey(&g36);
                        *(int32_t *)(v5 - 136) = (int32_t)&g16;
                        v22 = v32;
                        if (a1 == 0) {
                            // break -> 0x401c38
                            break;
                        }
                    }
                }
            }
        }
        int32_t v37 = v22;
        int32_t v38 = v16 + 1; // 0x401c18
        *(int32_t *)(v37 - 8) = v7;
        int32_t v39 = v37 - 12; // 0x401c22
        *(int32_t *)v39 = v16;
        *(int32_t *)(v37 - 16) = v4;
        bool v40 = SetupDiEnumDeviceInfo((int32_t *)v38, (int32_t)&g36, (struct _SP_DEVINFO_DATA *)&g36); // 0x401c2b
        result = 0;
        v10 = v39;
        while (v40) {
            int32_t v41 = v37;
            v16 = v38;
            v17 = (int32_t *)(v41 - 20);
            *v17 = 264;
            v18 = (int32_t *)(v41 - 24);
            *v18 = 0;
            v19 = (int32_t *)(v41 - 28);
            *v19 = (int32_t)&g16;
            memset(&g36, (int32_t)&g36, (int32_t)&g36);
            *v17 = v11;
            *v18 = 128;
            *v19 = v13;
            *(int32_t *)(v41 - 32) = 0;
            *(int32_t *)(v41 - 36) = 1;
            v20 = v41 - 40;
            *(int32_t *)v20 = v7;
            *(int32_t *)(v41 - 44) = v4;
            v21 = SetupDiGetDeviceRegistryPropertyA(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, &g36, (char *)&g36, (int32_t)&g36, &g36);
            v22 = v20;
            if (v21) {
                // 0x401af9
                v24 = (int32_t *)(v41 - 52);
                *v24 = (int32_t)"VID_";
                v25 = (int32_t *)(v41 - 56);
                *v25 = v13;
                g16 = _extract_id((int32_t)&g36, (int32_t)&g36);
                *v24 = (int32_t)"PID_";
                *v25 = v13;
                v26 = _extract_id((int32_t)&g36, (int32_t)&g36);
                g17 = v26;
                v22 = v20;
                if (v26 != 0 && g16 != 0) {
                    // 0x401b4b
                    *(int32_t *)(v41 - 48) = v11;
                    *v24 = 128;
                    *v25 = (int32_t)&g18;
                    *(int32_t *)(v41 - 60) = 0;
                    *(int32_t *)(v41 - 64) = 12;
                    v27 = v41 - 68;
                    *(int32_t *)v27 = v7;
                    *(int32_t *)(v41 - 72) = v4;
                    v28 = SetupDiGetDeviceRegistryPropertyA(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, &g36, (char *)&g36, (int32_t)&g36, &g36);
                    v22 = v27;
                    if (v28) {
                        // 0x401b7a
                        *(int32_t *)(v41 - 80) = 0x20019;
                        *(int32_t *)(v41 - 84) = 1;
                        *(int32_t *)(v41 - 88) = 0;
                        v29 = v41 - 92;
                        *(int32_t *)v29 = 1;
                        *(int32_t *)(v41 - 96) = v7;
                        *(int32_t *)(v41 - 100) = v4;
                        v30 = SetupDiOpenDevRegKey(&g36, (struct _SP_DEVINFO_DATA *)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                        v22 = v29;
                        if (v30 != (int32_t *)-1) {
                            // 0x401ba2
                            v31 = (int32_t)v30;
                            hKey = 128;
                            *(int32_t *)(v41 - 104) = v11;
                            *(int32_t *)(v41 - 108) = v15;
                            *(int32_t *)(v41 - 112) = 0;
                            *(int32_t *)(v41 - 116) = 0;
                            v32 = v41 - 120;
                            v33 = (int32_t *)v32;
                            *v33 = (int32_t)"PortName";
                            v34 = (int32_t *)(v41 - 124);
                            *v34 = v31;
                            v35 = RegQueryValueExA((int32_t *)hKey, (char *)&g36, &g36, &g36, (char *)&g36, &g36);
                            if (v35 != 0) {
                                // 0x401ba2
                                v23 = (int32_t *)(v41 - 132);
                            } else {
                                // 0x401bcf
                                *v33 = v15;
                                *v34 = (int32_t)"\\\\.\\%s";
                                *(int32_t *)(v41 - 128) = 127;
                                v36 = (int32_t *)(v41 - 132);
                                *v36 = (int32_t)&g19;
                                _snprintf((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                                v23 = v36;
                            }
                            // 0x401bea
                            *v23 = v31;
                            RegCloseKey(&g36);
                            *(int32_t *)(v41 - 136) = (int32_t)&g16;
                            v22 = v32;
                            result = &g19;
                            v10 = v32;
                            if (a1 == 0) {
                                // break -> 0x401c38
                                break;
                            }
                        }
                    }
                }
            }
            // 0x401c15
            v37 = v22;
            v38 = v16 + 1;
            *(int32_t *)(v37 - 8) = v7;
            v39 = v37 - 12;
            *(int32_t *)v39 = v16;
            *(int32_t *)(v37 - 16) = v4;
            v40 = SetupDiEnumDeviceInfo((int32_t *)v38, (int32_t)&g36, (struct _SP_DEVINFO_DATA *)&g36);
            result = 0;
            v10 = v39;
        }
    }
    // 0x401c38
    *(int32_t *)(v10 - 16) = v4;
    SetupDiDestroyDeviceInfoList(&g36);
    // 0x401c70
    return result;
}

// Address range: 0x401df0 - 0x401df9
int32_t _strncasecmp(void) {
    // 0x401df0
    return _strnicmp((char *)&g36, (char *)&g36, (int32_t)&g36);
}

// Address range: 0x401e00 - 0x4027c1
int32_t _getopt_parse(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6) {
    int32_t v1 = g2; // 0x401e09
    int32_t v2 = g21 | (int32_t)(v1 < 1); // 0x401e1f
    g21 = v2;
    int32_t v3; // 0x401e00
    int32_t result; // 0x401e00
    int32_t v4; // bp-76, 0x401e00
    int32_t v5; // 0x401e00
    if (v2 != 0) {
        // 0x402024
        g2 = 1;
        g21 = 0;
        v3 = 1;
        goto lab_0x401e40;
    } else {
        int32_t v6 = g24; // 0x401e2e
        v3 = v1;
        if (v1 < v6) {
            goto lab_0x401e40;
        } else {
            char * v7 = g23; // 0x402066
            if (v7 != NULL) {
                char v8 = *v7; // 0x402073
                if (v8 != 0) {
                    int32_t v9 = (int32_t)v7 + 1; // 0x40207d
                    int32_t result2 = v8; // 0x402081
                    g4 = (char *)result2;
                    char * v10 = (char *)v9; // 0x40208a
                    g23 = v10;
                    char * v11 = (char *)a4; // 0x40208f
                    char v12 = *v11; // 0x40208f
                    int32_t v13 = a4; // 0x402300
                    char v14 = v12; // 0x402301
                    switch (v12) {
                        case 43: {
                        }
                        case 45: {
                            // 0x4022fa
                            v13 = a4 + 1;
                            v14 = *(char *)v13;
                            // break -> 0x4020a3
                            break;
                        }
                    }
                    int32_t v15 = v13; // 0x4020a6
                    char v16 = v14; // 0x4020a6
                    if (v14 == 58) {
                        // 0x40247b
                        v15 = v13 + 1;
                        v16 = *(char *)v15;
                    }
                    char v17 = v16; // 0x4020b5
                    int32_t v18 = v15;
                    while (v8 != v17) {
                        int32_t v19 = v18 + 1; // 0x4020b4
                        v17 = *(char *)v19;
                        if (v17 == 0) {
                            goto lab_0x4020bb;
                        }
                        v18 = v19;
                    }
                    if (v18 == 0) {
                      lab_0x4020bb:
                        // 0x4020bb
                        if (a1 == 2) {
                            if (g3 != 0) {
                                // 0x402611
                                g23 = v7;
                                int32_t v20 = *(int32_t *)a3; // 0x40261b
                                int32_t v21 = *(int32_t *)0x40b1dc; // 0x40261e
                                fprintf((struct _IO_FILE *)(v21 + 64), "%s: unrecognised option `-%s'\n", (char *)v20, v7);
                            }
                            // 0x4022d1
                            g23 = NULL;
                            g4 = NULL;
                        } else {
                            char * v22 = v10; // 0x4020cd
                            if (g3 != 0) {
                                int32_t v23 = *(int32_t *)0x40b1dc; // 0x402315
                                int32_t v24 = *(int32_t *)a3; // 0x402320
                                fprintf((struct _IO_FILE *)(v23 + 64), "%s: invalid option -- %c\n", (char *)v24, v8);
                                v22 = g23;
                            }
                            // 0x4020d3
                            if (v22 != NULL) {
                                // 0x4020e0
                                if (*v22 != 0) {
                                    // 0x4022eb
                                    g2 = g25;
                                    // 0x40200c
                                    return 63;
                                }
                            }
                        }
                        // 0x4022eb
                        g2 = g25 + 1;
                        // 0x40200c
                        return 63;
                    }
                    // 0x402449
                    if (*(char *)(v18 + 1) == 58) {
                        // 0x402490
                        g20 = v9;
                        if (*v10 == 0) {
                            // 0x40249a
                            if (*(char *)(v18 + 2) == 58) {
                                // 0x40271d
                                g20 = 0;
                            } else {
                                // 0x4024a4
                                if (a2 - g25 < 2) {
                                    // 0x4026d0
                                    if (g3 != 0) {
                                        int32_t v25 = *(int32_t *)0x40b1dc; // 0x4026e0
                                        int32_t v26 = *(int32_t *)a3; // 0x4026ec
                                        fprintf((struct _IO_FILE *)(v25 + 64), "%s: option requires an argument -- %c\n", (char *)v26, v8);
                                    }
                                    char v27 = *v11; // 0x402700
                                    char v28 = v27; // 0x40272f
                                    switch (v27) {
                                        case 43: {
                                        }
                                        case 45: {
                                            // 0x40272c
                                            v28 = *(char *)(a4 + 1);
                                            // break -> 0x40270a
                                            break;
                                        }
                                    }
                                    // 0x40270a
                                    result = 4 * (int32_t)(v28 != 58) | (v28 != 58 ? 59 : 58);
                                  lab_0x40200c:
                                    // 0x40200c
                                    return result;
                                }
                                int32_t v29 = g25 + 1; // 0x4024bb
                                g25 = v29;
                                g20 = *(int32_t *)(4 * v29 + a3);
                            }
                        }
                        // 0x4024cc
                        g23 = NULL;
                        g2 = g25 + 1;
                    } else {
                        // 0x40244f
                        g20 = 0;
                        if (v9 != 0) {
                            // 0x402461
                            if (*v10 != 0) {
                                // 0x40246b
                                g2 = g25;
                                // 0x40200c
                                return result2;
                            }
                        }
                    }
                    // 0x40246b
                    g2 = g25 + 1;
                    // 0x40200c
                    return result2;
                }
            }
            int32_t v30 = g22; // 0x4021fd
            int32_t v31 = g25;
            if (v6 < v30) {
                uint32_t v32 = 1 - v30 + v31; // 0x402219
                ___chkstk(&v4);
                int32_t v33; // bp-64, 0x401e00
                v33 = &v33;
                int32_t v34 = g22;
                if (v32 >= 1) {
                    int32_t v35 = 4 * v34 + a3; // 0x40224a
                    int32_t v36 = *(int32_t *)v35; // 0x402253
                    v33 = v36;
                    int32_t v37 = 1; // 0x40225e
                    if (v32 != 1) {
                        int32_t v38 = v35 + 4; // 0x402255
                        *(int32_t *)(4 * v37 + v36) = *(int32_t *)v38;
                        int32_t v39 = v37 + 1; // 0x40225b
                        while (v39 != v32) {
                            // 0x402250
                            v38 += 4;
                            *(int32_t *)(4 * v39 + v33) = *(int32_t *)v38;
                            v39++;
                        }
                    }
                }
                int32_t v40 = v34 - 1; // 0x402263
                g22 = v40;
                if (v40 >= v6) {
                    int32_t v41 = 4 * (v40 + v32) + a3; // 0x40227d
                    int32_t v42 = 4 * v34 + a3; // 0x40227d
                    v42 -= 4;
                    int32_t v43 = v40 - 1; // 0x402283
                    *(int32_t *)v41 = *(int32_t *)v42;
                    int32_t v44 = v43; // 0x40228e
                    v41 -= 4;
                    while (v43 >= v6) {
                        // 0x402280
                        v42 -= 4;
                        v43 = v44 - 1;
                        *(int32_t *)v41 = *(int32_t *)v42;
                        v44 = v43;
                        v41 -= 4;
                    }
                    // 0x402290
                    g22 = v43;
                }
                if (v32 >= 1) {
                    int32_t v45 = 4 * v6 + a3; // 0x4022a0
                    int32_t v46 = 0;
                    int32_t v47 = v46 + 1; // 0x4022a8
                    *(int32_t *)v45 = *(int32_t *)(v33 + 4 * v46);
                    v45 += 4;
                    while (v47 != v32) {
                        // 0x4022a2
                        v46 = v47;
                        v47 = v46 + 1;
                        *(int32_t *)v45 = *(int32_t *)(v33 + 4 * v46);
                        v45 += 4;
                    }
                }
                // 0x4022b2
                v5 = v32 + v6;
                goto lab_0x401e70_2;
            } else {
                // 0x401e5f
                v5 = v31 + 1;
                goto lab_0x401e70_2;
            }
        }
    }
  lab_0x40233d_3:;
    // 0x40233d
    int32_t v48; // 0x401e06
    *(int32_t *)(v48 - 4) = a4;
    *(int32_t *)(v48 - 8) = a3;
    *(int32_t *)(v48 - 12) = a2;
    int32_t v49; // 0x401e00
    int32_t * v50; // 0x401e00
    *v50 = v49;
    int32_t v51 = _getopt_parse((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x40234d
    result = v51;
    return result;
  lab_0x401f41:;
    // 0x401f41
    int32_t v52; // 0x401e00
    int32_t v53 = v52;
    int32_t v54; // 0x401e00
    int32_t v55 = v54;
    int32_t v56; // 0x401e00
    int32_t v57; // 0x401e00
    struct _IO_FILE * v58; // 0x402107
    int32_t v59; // 0x401e00
    int32_t v60; // 0x401e00
    int32_t v61; // 0x401e76
    if (*(char *)v59 == 0) {
        // 0x4020f3
        g23 = NULL;
        v56 = v55;
        v57 = v61;
        v60 = v53;
        if (a6 == 0) {
            goto lab_0x40210c;
        } else {
            // 0x402104
            *(int32_t *)a6 = (int32_t)v58;
            v56 = g20;
            v57 = g25;
            v60 = a6;
            goto lab_0x40210c;
        }
    }
    struct _IO_FILE * v62; // 0x401f4a
    int32_t v63; // 0x401e00
    int32_t v64; // 0x401e70
    int32_t * v65; // 0x401e00
    if (v62 >= NULL) {
        // 0x4024e9
        g4 = NULL;
        g23 = NULL;
        g2 = v64 + 2;
        result = 63;
        if (g3 == 0) {
            goto lab_0x40200c;
        } else {
            // 0x402515
            *(int32_t *)(v48 - 4) = *v65;
            *(int32_t *)(v48 - 8) = *(int32_t *)a3;
            int32_t v66 = v48 - 12; // 0x402524
            *(int32_t *)v66 = (int32_t)"%s: option `%s' is ambiguous\n";
            v63 = v66;
            goto lab_0x402529;
        }
    }
    // 0x401f55
    int32_t v67; // 0x401e00
    struct _IO_FILE * v68; // bp-52, 0x401e00
    *(int32_t *)&v68 = v67;
    struct _IO_FILE * v69 = (struct _IO_FILE *)v67;
    struct _IO_FILE * v70 = v69; // 0x401f5b
    int32_t v71 = v55; // 0x401f5b
    struct _IO_FILE * v72 = v69; // 0x401f5b
    struct _IO_FILE * v73 = v69; // 0x401f5b
    struct _IO_FILE * v74 = v69; // 0x401f5b
    goto lab_0x401f69;
  lab_0x401f69:;
    // 0x401f69
    int32_t v75; // 0x401e00
    int32_t v76 = v75 + 16;
    struct _IO_FILE * v77 = v73; // 0x401f7c
    struct _IO_FILE * v78 = v72; // 0x402370
    int32_t v79 = v71;
    struct _IO_FILE * v80 = v70;
    int32_t v81 = (int32_t)v58 + 1; // 0x401f6f
    struct _IO_FILE * stream2 = (struct _IO_FILE *)v81;
    int32_t v82 = *(int32_t *)v76; // 0x401f73
    struct _IO_FILE * v83 = v80; // 0x401f7a
    int32_t v84 = v79; // 0x401f7a
    struct _IO_FILE * v85 = v78; // 0x401f7a
    struct _IO_FILE * v86 = v77; // 0x401f7a
    struct _IO_FILE * v87 = stream2; // 0x401f7a
    int32_t v88 = v81; // 0x401f7a
    struct _IO_FILE * v89 = v74; // 0x401f7a
    int32_t v90 = v76; // 0x401f7a
    int32_t v91 = v82; // 0x401f7a
    if (v82 == 0) {
        // break -> 0x401f7c
        goto lab_0x401f7c;
    }
    goto lab_0x401f1c;
  lab_0x401f60:;
    // 0x401f60
    int32_t v99; // 0x401e00
    int32_t v112 = v99;
    struct _IO_FILE * v98; // 0x401e00
    v70 = v98;
    int32_t v93; // 0x401e00
    v71 = v93;
    struct _IO_FILE * v97; // 0x401e00
    v72 = v97;
    struct _IO_FILE * v96; // 0x401e00
    v73 = v96;
    v74 = v62;
    int32_t v102; // 0x401e00
    char v101; // 0x401e00
    if (v101 == 61) {
        // 0x4021f2
        g20 = v112;
        v54 = v112;
        v52 = v112;
        v59 = v102;
        goto lab_0x401f41;
    } else {
        goto lab_0x401f69;
    }
  lab_0x401fa0:;
    // 0x401fa0
    char * v113; // 0x401e00
    char v114 = *v113; // 0x401fa3
    if (v114 == 45) {
        // 0x402412
        g23 = NULL;
        g2 = v64 + 2;
        g20 = *v65;
        result = 1;
        goto lab_0x40200c;
    }
    // 0x401fae
    int32_t v115; // 0x401e00
    int32_t v116 = v115;
    char v117 = g26; // 0x401fae
    char v118; // 0x401e00
    char v119; // 0x401e00
    if (v117 != 0) {
        // 0x402016
        v119 = v117;
        if (((int32_t)v114 & 0x1000) == 0) {
            goto lab_0x401ff2;
        } else {
            goto lab_0x40201b;
        }
    } else {
        if (((int32_t)v114 & 0x1000) != 0) {
            goto lab_0x40201b;
        } else {
            // 0x401fc0
            g26 = 0;
            if (v114 == 43) {
                goto lab_0x401fe3;
            } else {
                // 0x401fcf
                *v50 = (int32_t)"POSIXLY_CORRECT";
                char * env_val = getenv((char *)&g36); // 0x401fd7
                v118 = g26;
                v119 = g26;
                if (env_val == NULL) {
                    goto lab_0x401ff2;
                } else {
                    goto lab_0x401fe3;
                }
            }
        }
    }
  lab_0x401ff2:;
    int32_t v120 = v116; // 0x401ff9
    if ((v119 & 16) != 0) {
        // break -> 0x401fff
        goto lab_0x401fff;
    }
    goto lab_0x401e70;
  lab_0x40201b:;
    char v132 = v117 | v114;
    g26 = v132;
    v119 = v132;
    goto lab_0x401ff2;
  lab_0x401ec6:;
    // 0x401ec6
    char * v130; // 0x401e00
    char * v95 = v130; // 0x401f91
    g20 = 0;
    struct _IO_FILE * v133; // 0x401e00
    int32_t v134; // 0x401e00
    int32_t v135; // 0x401e00
    if (a5 != 0) {
        int32_t v136 = *(int32_t *)a5; // 0x401eea
        if (v136 != 0) {
            int32_t v137 = (int32_t)v95; // 0x401ef4
            int32_t v100 = v137 + 1;
            int32_t v109 = v137 + 2; // 0x401f05
            v68 = (struct _IO_FILE *)-1;
            char * v104 = (char *)v100;
            v83 = (struct _IO_FILE *)-1;
            v84 = 0;
            v85 = (struct _IO_FILE *)-1;
            v86 = (struct _IO_FILE *)-1;
            v87 = NULL;
            v88 = 0;
            v89 = (struct _IO_FILE *)-1;
            v90 = a5;
            v91 = v136;
            while (true) {
              lab_0x401f1c:;
                int32_t v92 = v91;
                v75 = v90;
                v62 = v89;
                v67 = v88;
                v58 = v87;
                v93 = v84;
                char v94 = *v95; // 0x401f1c
                if (v94 == 0) {
                    goto lab_0x401f41;
                } else {
                    // 0x401f22
                    v96 = v86;
                    v97 = v85;
                    v98 = v83;
                    v99 = v100;
                    v101 = v94;
                    v102 = v92;
                    if (*(char *)v92 != v94) {
                        goto lab_0x401f60;
                    } else {
                        char v103 = *v104; // 0x401f39
                        int32_t v105 = v92 + 1; // 0x401f3c
                        int32_t v106 = v105; // 0x401f3f
                        char v107 = v103; // 0x401f3f
                        int32_t v108 = v109; // 0x401f3f
                        v54 = v93;
                        v52 = v100;
                        v59 = v105;
                        if (v103 != 0) {
                            int32_t v110 = v108;
                            v99 = v110;
                            v101 = v107;
                            v102 = v106;
                            while (*(char *)v106 == v107) {
                                char v111 = *(char *)v110; // 0x401f39
                                v106++;
                                v107 = v111;
                                if (v111 == 0) {
                                    goto lab_0x401f41;
                                }
                                v110++;
                                v99 = v110;
                                v101 = v107;
                                v102 = v106;
                            }
                            goto lab_0x401f60;
                        } else {
                            goto lab_0x401f41;
                        }
                    }
                }
            }
          lab_0x401f7c:
            if (v77 >= NULL) {
                // 0x40235c
                g23 = NULL;
                v133 = v80;
                v134 = v79;
                v135 = v61;
                if (a6 == 0) {
                    goto lab_0x402375;
                } else {
                    // 0x40236d
                    *(int32_t *)a6 = (int32_t)v78;
                    v133 = v68;
                    v134 = g20;
                    v135 = g25;
                    goto lab_0x402375;
                }
            }
        }
    }
    int32_t v131; // 0x401e00
    if (v131 == 1) {
        // 0x402643
        g4 = NULL;
        g23 = NULL;
        g2 = v64 + 2;
        result = 63;
        if (g3 == 0) {
            goto lab_0x40200c;
        } else {
            // 0x40266b
            *(int32_t *)(v48 - 4) = *v65;
            *(int32_t *)(v48 - 8) = *(int32_t *)a3;
            int32_t v138 = v48 - 12; // 0x40267a
            *(int32_t *)v138 = (int32_t)"%s: unrecognised option `%s'\n";
            v63 = v138;
            goto lab_0x402529;
        }
    }
    // 0x401f91
    v115 = v131;
    v49 = v131;
    if (*v95 != 0) {
        goto lab_0x40233d_3;
    }
    goto lab_0x401fa0;
  lab_0x401fe3:;
    char v139 = v118 | 16;
    g26 = v139;
    v119 = v139;
    goto lab_0x401ff2;
  lab_0x401e40:
    // 0x401e40
    g23 = NULL;
    int32_t v140 = v3 - 1; // 0x401e4f
    g25 = v140;
    g24 = v140;
    g22 = v140;
    // 0x401e5f
    v5 = v140 + 1;
    goto lab_0x401e70_2;
  lab_0x401e70_2:
    // 0x401e70
    v48 = &v4;
    g24 = v5;
    v113 = (char *)a4;
    v50 = (int32_t *)(v48 - 16);
    v120 = a1;
    int32_t v129; // 0x401e00
    int32_t v128; // 0x401e00
    int32_t v123; // 0x401e8f
    int32_t v127; // 0x4025cf
    while (true) {
      lab_0x401e70:
        // 0x401e70
        v64 = g25;
        v61 = v64 + 1;
        g25 = v61;
        if (v61 >= a2) {
            // break -> 0x401fff
            break;
        }
        int32_t v121 = v120;
        g22 = v61;
        int32_t v122 = 4 * v61 + a3;
        v65 = (int32_t *)v122;
        v123 = *v65;
        char * v124 = (char *)v123; // 0x401e92
        g23 = v124;
        v115 = v121;
        if (*v124 != 45) {
            goto lab_0x401fa0;
        } else {
            char * v125 = (char *)(v123 + 1); // 0x401ea4
            g23 = v125;
            v115 = v121;
            switch (*v125) {
                case 0: {
                    goto lab_0x401fa0;
                }
                case 45: {
                    char * v126 = (char *)(v123 + 2); // 0x402040
                    if (*v126 == 0) {
                        // 0x4025cf
                        v127 = g24;
                        v128 = v61;
                        v129 = v122;
                        if (v61 > v127) {
                            goto lab_0x4025e0;
                        } else {
                            goto lab_0x4025fc;
                        }
                    }
                    // 0x40204a
                    v49 = v121;
                    if (v121 < 1) {
                        goto lab_0x40233d_3;
                    }
                    // 0x402055
                    g23 = v126;
                    v130 = v126;
                    v131 = 1;
                    goto lab_0x401ec6;
                }
                default: {
                    // 0x401ebc
                    v130 = v125;
                    v131 = v121;
                    v49 = 0;
                    if (v121 < 2) {
                        goto lab_0x40233d_3;
                    }
                    goto lab_0x401ec6;
                }
            }
        }
    }
  lab_0x401fff:
    // 0x401fff
    g2 = g24;
    // 0x40200c
    return -1;
  lab_0x4025e0:;
    int32_t v141 = v128;
    int32_t v142 = v129 - 4;
    int32_t v143 = v141 - 1; // 0x4025e0
    *(int32_t *)v129 = *(int32_t *)v142;
    v128 = v143;
    v129 = v142;
    if (v127 < v141) {
        goto lab_0x4025e0;
    } else {
        // 0x4025f0
        g22 = v143;
        *(int32_t *)(4 * v127 + a3) = v123;
        goto lab_0x4025fc;
    }
  lab_0x4025fc:;
    int32_t v144 = v127 + 1; // 0x4025fc
    g24 = v144;
    g2 = v144;
    result = -1;
    goto lab_0x40200c;
  lab_0x402529:
    // 0x402529
    *(int32_t *)(v63 - 4) = *(int32_t *)0x40b1dc + 64;
    fprintf((struct _IO_FILE *)&g36, (char *)&g36);
    result = 63;
    goto lab_0x40200c;
  lab_0x40210c:;
    int32_t v145 = v57 + 1; // 0x402118
    g2 = v145;
    int32_t v146 = *(int32_t *)(v75 + 4);
    char v147; // 0x401e00
    struct _IO_FILE * stream; // bp-19, 0x401e00
    if (v56 == 0) {
        if (v146 != 1) {
            goto lab_0x4021d3;
        } else {
            if (v145 < a2) {
                // 0x4021ba
                g25 = v145;
                g20 = *(int32_t *)(4 * v145 + a3);
                g2 = v57 + 2;
                goto lab_0x4021d3;
            } else {
                char v148 = *v113; // 0x402547
                v147 = v148;
                switch (v148) {
                    case 43: {
                        // 0x4025c7
                        v147 = *(char *)(a4 + 1);
                        goto lab_0x402551;
                    }
                    case 45: {
                        // 0x4025c7
                        v147 = *(char *)(a4 + 1);
                        goto lab_0x402551;
                    }
                    default: {
                        goto lab_0x402551;
                    }
                }
            }
        }
    } else {
        if (v146 == 0) {
            // 0x402135
            if (g3 == 0) {
                // 0x40218e
                g4 = (char *)*(int32_t *)(v75 + 12);
                return 63;
            }
            // 0x402144
            stream = (struct _IO_FILE *)0x2d2d;
            *(int32_t *)(v48 - 4) = v60;
            int32_t v149 = *(int32_t *)0x40b1dc + 64; // 0x402167
            *(int32_t *)(v48 - 8) = *(int32_t *)a3;
            *(int32_t *)(v48 - 12) = (int32_t)"%s: ";
            *v50 = v149;
            fprintf(stream, NULL);
            *(int32_t *)(v48 - 20) = *(int32_t *)v75;
            *(int32_t *)(v48 - 24) = (int32_t)&stream;
            *(int32_t *)(v48 - 28) = (int32_t)"option `%s%s' doesn't accept an argument\n";
            *(int32_t *)(v48 - 32) = v149;
            fprintf((struct _IO_FILE *)&g36, (char *)&g36);
            // 0x40218e
            g4 = (char *)*(int32_t *)(v75 + 12);
            return 63;
        }
        goto lab_0x4021d3;
    }
  lab_0x4021d3:;
    int32_t v150 = *(int32_t *)(v75 + 8); // 0x4021d6
    int32_t v151 = *(int32_t *)(v75 + 12);
    result = v151;
    if (v150 != 0) {
        // 0x4021e1
        *(int32_t *)v150 = v151;
        return 0;
    }
    goto lab_0x40200c;
  lab_0x402551:
    // 0x402551
    if (g3 == 0) {
        goto lab_0x4025b7;
    } else {
        // 0x40256d
        stream = (struct _IO_FILE *)0x2d2d;
        *(int32_t *)(v48 - 4) = g3 & -0x10000 | 0x2d00;
        int32_t v152 = *(int32_t *)0x40b1dc + 64; // 0x402590
        *(int32_t *)(v48 - 8) = *(int32_t *)a3;
        *(int32_t *)(v48 - 12) = (int32_t)"%s: ";
        *v50 = v152;
        fprintf(stream, NULL);
        *(int32_t *)(v48 - 20) = *(int32_t *)v75;
        *(int32_t *)(v48 - 24) = (int32_t)&stream;
        *(int32_t *)(v48 - 28) = (int32_t)"option `%s%s' requires an argument\n";
        *(int32_t *)(v48 - 32) = v152;
        fprintf((struct _IO_FILE *)&g36, (char *)&g36);
        goto lab_0x4025b7;
    }
  lab_0x4025b7:
    // 0x4025b7
    g4 = (char *)*(int32_t *)(v75 + 12);
    result = 4 * (int32_t)(v147 != 58) | (v147 != 58 ? 59 : 58);
    goto lab_0x40200c;
  lab_0x402375:;
    int32_t v153 = v135 + 1; // 0x402380
    g2 = v153;
    int32_t v154 = 16 * (int32_t)v133 + a5;
    int32_t v155 = *(int32_t *)(v154 + 4);
    char v156; // 0x401e00
    if (v134 == 0) {
        if (v155 != 1) {
            goto lab_0x4026bd;
        } else {
            if (v153 < a2) {
                // 0x4026a4
                g25 = v153;
                g20 = *(int32_t *)(4 * v153 + a3);
                g2 = v135 + 2;
                goto lab_0x4026bd;
            } else {
                char v157 = *v113; // 0x40273f
                v156 = v157;
                switch (v157) {
                    case 43: {
                        // 0x4027b9
                        v156 = *(char *)(a4 + 1);
                        goto lab_0x402749;
                    }
                    case 45: {
                        // 0x4027b9
                        v156 = *(char *)(a4 + 1);
                        goto lab_0x402749;
                    }
                    default: {
                        goto lab_0x402749;
                    }
                }
            }
        }
    } else {
        if (v155 != 0) {
            goto lab_0x4026bd;
        } else {
            // 0x4023ab
            if (g3 == 0) {
                goto lab_0x402400;
            } else {
                // 0x4023b9
                stream = (struct _IO_FILE *)0x2d2d;
                *(int32_t *)(v48 - 4) = g3 & -0x10000 | 0x2d00;
                int32_t v158 = *(int32_t *)0x40b1dc + 64; // 0x4023dc
                *(int32_t *)(v48 - 8) = *(int32_t *)a3;
                *(int32_t *)(v48 - 12) = (int32_t)"%s: ";
                *v50 = v158;
                fprintf(stream2, (char *)&g36);
                *(int32_t *)(v48 - 20) = *(int32_t *)v154;
                *(int32_t *)(v48 - 24) = (int32_t)&stream;
                *(int32_t *)(v48 - 28) = (int32_t)"option `%s%s' doesn't accept an argument\n";
                *(int32_t *)(v48 - 32) = v158;
                fprintf((struct _IO_FILE *)&g36, (char *)&g36);
                goto lab_0x402400;
            }
        }
    }
  lab_0x4026bd:;
    int32_t v159 = *(int32_t *)(v154 + 8); // 0x4026bd
    int32_t v160 = *(int32_t *)(v154 + 12);
    result = v160;
    if (v159 == 0) {
        goto lab_0x40200c;
    } else {
        // 0x4026c4
        *(int32_t *)v159 = v160;
        result = 0;
        goto lab_0x40200c;
    }
  lab_0x402400:
    // 0x402400
    g4 = (char *)*(int32_t *)(v154 + 12);
    result = 63;
    goto lab_0x40200c;
  lab_0x402749:
    // 0x402749
    if (g3 == 0) {
        goto lab_0x4027ac;
    } else {
        // 0x402765
        stream = (struct _IO_FILE *)0x2d2d;
        *(int32_t *)(v48 - 4) = g3 & -0x10000 | 0x2d00;
        int32_t v161 = *(int32_t *)0x40b1dc + 64; // 0x402788
        *(int32_t *)(v48 - 8) = *(int32_t *)a3;
        *(int32_t *)(v48 - 12) = (int32_t)"%s: ";
        *v50 = v161;
        fprintf(stream2, (char *)&g36);
        *(int32_t *)(v48 - 20) = *(int32_t *)v154;
        *(int32_t *)(v48 - 24) = (int32_t)&stream;
        *(int32_t *)(v48 - 28) = (int32_t)"option `%s%s' requires an argument\n";
        *(int32_t *)(v48 - 32) = v161;
        fprintf((struct _IO_FILE *)&g36, (char *)&g36);
        goto lab_0x4027ac;
    }
  lab_0x4027ac:
    // 0x4027ac
    g4 = (char *)*(int32_t *)(v154 + 12);
    result = 4 * (int32_t)(v156 != 58) | (v156 != 58 ? 59 : 58);
    goto lab_0x40200c;
}

// Address range: 0x402800 - 0x402827
int32_t _getopt_long(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    // 0x402800
    return _getopt_parse(1, a1, a2, a3, a4, a5);
}

// Address range: 0x402850 - 0x402874
int32_t _snprintf(int32_t a1, int32_t a2, int32_t a3) {
    // 0x402850
    int32_t v1; // bp+16, 0x402850
    return _vsnprintf(a1, a2, a3, &v1);
}

// Address range: 0x402880 - 0x4028da
int32_t _vsnprintf(int32_t a1, int32_t a2, int32_t a3, int32_t * a4) {
    int32_t v1 = (int32_t)a4;
    if (a2 == 0) {
        // 0x402899
        return ___mingw_pformat(0, a1, 0, a3, v1);
    }
    uint32_t v2 = a2 - 1; // 0x4028b3
    uint32_t result = ___mingw_pformat(0, a1, v2, a3, v1); // 0x4028bc
    if (result > v2) {
        // 0x4028d2
        *(char *)(v2 + a1) = 0;
    } else {
        // 0x4028ca
        *(char *)(result + a1) = 0;
    }
    // 0x4028ce
    return result;
}

// Address range: 0x4028e0 - 0x4029ce
int32_t ___pformat_cvt(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t * a6, int32_t * a7, int32_t a8) {
    int32_t v1 = a1; // bp-36, 0x4028ea
    int32_t v2 = __asm_fxam((float80_t)(int80_t)a1); // 0x40290a
    __asm_wait();
    int32_t v3; // 0x4028e0
    int32_t v4; // 0x4028e0
    int32_t v5; // bp-12, 0x4028e0
    int32_t v6; // bp-8, 0x4028e0
    int32_t v7; // 0x4028e0
    int32_t result; // 0x402977
    if ((v2 & 256) == 0) {
        if ((v2 & 1024) != 0) {
            int32_t v8 = 0x10000 * a3 / 0x10000;
            if ((v2 & 0x4000) == 0) {
                // 0x4029b0
                v6 = 1;
                v3 = v8 % 0x8000 - 0x403e;
                v4 = v8;
            } else {
                // 0x402986
                v6 = 2;
                v3 = -0x403d;
                v4 = v8;
            }
        } else {
            // 0x40293a
            v6 = 0;
            v3 = 0;
            v4 = 0x10000 * a3 / 0x10000;
        }
    } else {
        if ((v2 & 1024) == 0) {
            // 0x4029a0
            v6 = 4;
            // 0x402951
            *a7 = 0;
            result = ___gdtoa(&g5, 0, &v1, &v6, v7, a5, (int32_t)a6, &v5);
            return result;
        }
        // 0x402924
        v6 = 3;
        v3 = 0;
        v4 = 0x10000 * a3 / 0x10000;
    }
    // 0x402951
    *a7 = v4 & 0x8000;
    result = ___gdtoa(&g5, v3, &v1, &v6, v7, a5, (int32_t)a6, &v5);
    return result;
}

// Address range: 0x4029d0 - 0x402a19
int32_t ___pformat_putc(void) {
    // 0x4029d0
    int32_t v1; // 0x4029d0
    uint32_t v2 = *(int32_t *)(v1 + 4); // 0x4029d8
    int32_t result; // 0x4029d0
    if ((v2 & 0x2000) == 0) {
        int32_t * v3 = (int32_t *)(v1 + 24);
        uint32_t v4 = *v3; // 0x4029e3
        if (*(int32_t *)(v1 + 28) <= v4) {
            // 0x4029f5
            *v3 = v4 + 1;
            return result;
        }
    }
    unsigned char v5 = (char)(v2 / 256) & 16; // 0x4029e8
    int32_t stream = 256 * (int32_t)v5 | v2 & -0xff01; // 0x4029e8
    int32_t c; // 0x4029d0
    if (v5 != 0) {
        // 0x402a00
        fputc(c, (struct _IO_FILE *)stream);
        int32_t * v6 = (int32_t *)(v1 + 24); // 0x402a0b
        *v6 = *v6 + 1;
        return result;
    }
    int32_t * v7 = (int32_t *)(v1 + 24);
    *(char *)(*v7 + stream) = (char)c;
    // 0x4029f5
    *v7 = *v7 + 1;
    return result;
}

// Address range: 0x402a20 - 0x402af9
int32_t ___pformat_emit_radix_point(void) {
    // 0x402a20
    int32_t v1; // 0x402a20
    int32_t * v2 = (int32_t *)(v1 + 16); // 0x402a2b
    int32_t v3; // bp-24, 0x402a20
    if (*v2 == -3) {
        // 0x402ac2
        v3 = 0;
        int32_t v4 = *(int32_t *)localeconv(); // 0x402ad4
        int32_t v5; // bp-18, 0x402a20
        uint32_t v6 = _mbrtowc(&v5, v4, 16, &v3); // 0x402adb
        if (v6 >= 1) {
            // 0x402ae9
            *(int16_t *)(v1 + 20) = (int16_t)v5;
        }
        // 0x402af1
        *v2 = v6;
    }
    int32_t v7 = v1 + 20; // 0x402a35
    if (*(int16_t *)v7 == 0) {
        // 0x402aa0
        return ___pformat_putc();
    }
    // 0x402a3c
    int32_t v8; // bp-44, 0x402a20
    int32_t v9 = &v8; // 0x402a26
    int32_t v10 = ___chkstk(&v8); // 0x402a48
    v3 = 0;
    *(int32_t *)(v9 - 4) = v10;
    *(int32_t *)(v9 - 8) = (int32_t)&v3;
    *(int32_t *)(v9 - 12) = *(int32_t *)v7 % 0x10000;
    *(int32_t *)(v9 - 16) = v9 + 15 & -16;
    int32_t v11 = _wcrtomb(v3, (int32_t)&g36); // 0x402a6a
    if (v11 < 1) {
        // 0x402a91
        return ___pformat_putc();
    }
    int32_t v12 = 0; // 0x402a77
    v12++;
    int32_t result = ___pformat_putc(); // 0x402a8f
    while (v12 != v11) {
        // 0x402a80
        v12++;
        result = ___pformat_putc();
    }
    // 0x402a91
    return result;
}

// Address range: 0x402b00 - 0x402d27
int32_t ___pformat_emit_float(int32_t a1) {
    int32_t * v1 = (int32_t *)(a1 + 8);
    int32_t v2 = *v1;
    int32_t v3; // 0x402b00
    int32_t v4; // 0x402b00
    int32_t v5; // 0x402bd3
    if (v4 < 1) {
        // 0x402cf3
        v5 = v2;
        if (v2 < 1) {
            goto lab_0x402bd8;
        } else {
            int32_t v6 = v2 - 1; // 0x402cfe
            *v1 = v6;
            v3 = v6;
            goto lab_0x402be0;
        }
    } else {
        if (v2 > v4) {
            // 0x402bd3
            v5 = v2 - v4;
            *v1 = v5;
            goto lab_0x402bd8;
        } else {
            // 0x402b24
            *v1 = -1;
            // 0x402b2b
            *v1 = -1;
            goto lab_0x402b32;
        }
    }
  lab_0x402bd8:
    // 0x402bd8
    v3 = v5;
    if (v5 < 0) {
        // 0x402b2b
        *v1 = -1;
        goto lab_0x402b32;
    } else {
        goto lab_0x402be0;
    }
  lab_0x402be0:;
    int32_t * v7 = (int32_t *)(a1 + 12); // 0x402be0
    int32_t v8 = *v7; // 0x402be0
    int32_t v9; // 0x402b00
    int32_t v10; // 0x402beb
    if (v3 > v8) {
        // 0x402beb
        v10 = v3 - v8;
        *v1 = v10;
        if (v10 < 1) {
            goto lab_0x402b32;
        } else {
            // 0x402bf8
            if (*v7 < 1) {
                // 0x402d18
                v9 = v10;
                if ((*(char *)(a1 + 5) & 8) == 0) {
                    goto lab_0x402c0f;
                } else {
                    goto lab_0x402c03;
                }
            } else {
                goto lab_0x402c03;
            }
        }
    } else {
        // 0x402b2b
        *v1 = -1;
        goto lab_0x402b32;
    }
  lab_0x402b32:;
    int32_t v11; // 0x402b00
    if (v11 != 0) {
        // 0x402c61
        ___pformat_putc();
        goto lab_0x402b4e;
    } else {
        goto lab_0x402b3a;
    }
  lab_0x402b3a:;
    int32_t v12 = *(int32_t *)(a1 + 4); // 0x402b3a
    if ((v12 & 256) != 0) {
        // 0x402ce2
        ___pformat_putc();
    } else {
        if ((v12 & 64) != 0) {
            // 0x402d07
            ___pformat_putc();
        }
    }
    goto lab_0x402b4e;
  lab_0x402b4e:;
    int32_t v13 = *v1; // 0x402b4e
    if (v13 < 1) {
        goto lab_0x402b68;
    } else {
        // 0x402b55
        if ((*(int32_t *)(a1 + 4) & 1536) == 512) {
            // 0x402ca6
            *v1 = v13 - 1;
            ___pformat_putc();
            int32_t v14 = *v1; // 0x402cbc
            *v1 = v14 - 1;
            while (v14 >= 0 == (v14 != 0)) {
                // 0x402cb0
                ___pformat_putc();
                v14 = *v1;
                *v1 = v14 - 1;
            }
            if (v4 >= 0 == (v4 != 0)) {
                goto lab_generated_0;
            } else {
                // 0x402cd1
                ___pformat_putc();
                goto lab_0x402b8d;
            }
        } else {
            goto lab_0x402b68;
        }
    }
  lab_0x402c03:;
    int32_t v15 = v10 - 1; // 0x402c03
    *v1 = v15;
    v9 = v15;
    if (v15 == 0) {
        goto lab_0x402b32;
    } else {
        goto lab_0x402c0f;
    }
  lab_0x402b68:
    if (v4 < 1) {
        // 0x402cd1
        ___pformat_putc();
        goto lab_0x402b8d;
    } else {
        goto lab_generated_1;
    }
  lab_0x402c0f:;
    // 0x402c0f
    int32_t v16; // 0x402b00
    int32_t v17; // 0x402b00
    if (v11 != 0) {
        goto lab_0x402c1e;
    } else {
        int32_t v18 = *(int32_t *)(a1 + 4); // 0x402c13
        v16 = v9;
        v17 = v18;
        if ((v18 & 448) == 0) {
            goto lab_0x402c2d;
        } else {
            goto lab_0x402c1e;
        }
    }
  lab_generated_2:
    ___pformat_putc();
    int32_t v19; // 0x402b00
    int32_t v20 = v19 - 1; // 0x402b88
    v19 = v20;
    int32_t v21 = 0; // 0x402b89
    while (v20 != 0) {
        // 0x402b72
        ___pformat_putc();
        v20 = v19 - 1;
        v19 = v20;
        v21 = 0;
    }
    goto lab_0x402b8d;
  lab_0x402c1e:;
    int32_t v30 = v9 - 1; // 0x402c1e
    *v1 = v30;
    if (v30 == 0) {
        goto lab_0x402b32;
    } else {
        // 0x402c2a
        v16 = v30;
        v17 = *(int32_t *)(a1 + 4);
        goto lab_0x402c2d;
    }
  lab_0x402b8d:;
    int32_t * v22 = (int32_t *)(a1 + 12); // 0x402b8d
    int32_t v23 = *v22; // 0x402b8d
    int32_t v24; // 0x402b00
    if (v23 < 1) {
        // 0x402c72
        if ((*(char *)(a1 + 5) & 8) != 0) {
            goto lab_0x402b98;
        } else {
            // 0x402c7c
            v24 = v23;
            if (v21 >= 0) {
                goto lab_0x402bbe;
            } else {
                goto lab_0x402c84;
            }
        }
    } else {
        goto lab_0x402b98;
    }
  lab_0x402c2d:
    // 0x402c2d
    if ((v17 & 1536) != 0) {
        goto lab_0x402b32;
    } else {
        // 0x402c36
        *v1 = v16 - 1;
        ___pformat_putc();
        int32_t v25 = *v1; // 0x402c4c
        *v1 = v25 - 1;
        while (v25 >= 0 == (v25 != 0)) {
            // 0x402c40
            ___pformat_putc();
            v25 = *v1;
            *v1 = v25 - 1;
        }
        if (v11 == 0) {
            goto lab_0x402b3a;
        } else {
            // 0x402c61
            ___pformat_putc();
            goto lab_0x402b4e;
        }
    }
  lab_0x402b98:
    // 0x402b98
    ___pformat_emit_radix_point();
    if (v21 >= 0) {
        goto lab_0x402bbe;
    } else {
        // 0x402b98
        v24 = *v22;
        goto lab_0x402c84;
    }
  lab_0x402bbe:;
    int32_t v26 = *v22; // 0x402bbe
    int32_t result = v26 - 1; // 0x402bc3
    *v22 = result;
    if (v26 >= 0 != v26 != 0) {
        // 0x402bcb
        return result;
    }
    ___pformat_putc();
    int32_t v27 = *v22; // 0x402bbe
    int32_t result2 = v27 - 1; // 0x402bc3
    *v22 = result2;
    while (v27 >= 0 == (v27 != 0)) {
        // 0x402ba8
        ___pformat_putc();
        v27 = *v22;
        result2 = v27 - 1;
        *v22 = result2;
    }
    // 0x402bcb
    return result2;
  lab_0x402c84:
    // 0x402c84
    *v22 = v24 + v21;
    int32_t v28 = v21 + 1; // 0x402c97
    ___pformat_putc();
    int32_t v29 = v28; // 0x402c9f
    while (v28 != 0) {
        // 0x402c90
        v28 = v29 + 1;
        ___pformat_putc();
        v29 = v28;
    }
    goto lab_0x402bbe;
}

// Address range: 0x402d30 - 0x402faa
int32_t ___pformat_xint(int32_t a1) {
    int32_t * v1 = (int32_t *)(a1 + 12); // 0x402d62
    int32_t v2; // 0x402d30
    ___chkstk((int32_t *)v2);
    int32_t v3; // bp-48, 0x402d30
    int32_t v4 = &v3; // 0x402da7
    int32_t v5; // 0x402d30
    int32_t v6; // 0x402d30
    int32_t v7; // 0x402d30
    uint32_t v8; // 0x402d30
    if ((v7 || v2) == 0) {
        goto lab_0x402f7a;
    } else {
        uint32_t v9 = v8 != 111 ? 4 : 3; // 0x402d51
        int32_t v10; // 0x402d30
        char v11 = v10 & (8 * (int32_t)(v8 != 111) | 7);
        unsigned char v12 = v11 | 48; // 0x402dca
        int32_t v13 = v4 + 1; // 0x402dcc
        *(char *)v4 = v12 < 58 ? v12 : v11 + 55 | (char)v8 & 32;
        int32_t v14 = v3 << 32 - v9 | v10 >> v9; // 0x402de3
        int32_t v15 = v3 >> v9; // 0x402de6
        v3 = v15;
        v10 = v14;
        int32_t v16 = v13; // 0x402df9
        while ((v14 || v15) != 0) {
            // 0x402dc2
            v11 = v10 & (8 * (int32_t)(v8 != 111) | 7);
            v12 = v11 | 48;
            v13 = v16 + 1;
            *(char *)v16 = v12 < 58 ? v12 : v11 + 55 | (char)v8 & 32;
            v14 = v3 << 32 - v9 | v10 >> v9;
            v15 = v3 >> v9;
            v3 = v15;
            v10 = v14;
            v16 = v13;
        }
        // 0x402dfb
        v6 = v13;
        v5 = *v1;
        if (v13 == v4) {
            goto lab_0x402f7a;
        } else {
            goto lab_0x402e0a;
        }
    }
  lab_0x402f7a:;
    int32_t * v17 = (int32_t *)(a1 + 4); // 0x402f80
    *v17 = *v17 & -2049;
    v6 = v4;
    v5 = *v1;
    goto lab_0x402e0a;
  lab_0x402e0a:;
    // 0x402e0a
    int32_t v18; // 0x402d30
    if (v5 < 1) {
        goto lab_0x402f28;
    } else {
        if (v4 - v6 + v5 < 1) {
            goto lab_0x402f28;
        } else {
            int32_t v19 = v5 + v4; // 0x402e27
            int32_t v20 = v6; // 0x402e2a
            *(char *)v20 = 48;
            v20++;
            v18 = v19;
            while (v20 != v19) {
                // 0x402e30
                *(char *)v20 = 48;
                v20++;
                v18 = v19;
            }
            goto lab_0x402e3b;
        }
    }
  lab_0x402f28:
    // 0x402f28
    v18 = v6;
    if (v8 == 111) {
        // 0x402f32
        v18 = v6;
        if ((*(char *)(a1 + 5) & 8) != 0) {
            // 0x402f3f
            *(char *)v6 = 48;
            v18 = v6 + 1;
        }
    }
    goto lab_0x402e3b;
  lab_0x402e3b:;
    int32_t v21 = v18; // 0x402e3e
    if (v18 == v4) {
        // 0x402f8e
        v21 = v4;
        if (*v1 != 0) {
            // 0x402f9c
            *(char *)&v3 = 48;
            v21 = v4 | 1;
        }
    }
    int32_t * v22 = (int32_t *)(a1 + 8); // 0x402d89
    int32_t v23 = *v22; // 0x402e4c
    int32_t v24 = v21 - v4; // 0x402e4f
    int32_t v25; // 0x402d30
    int32_t v26; // 0x402d30
    int32_t v27; // 0x402d30
    int32_t v28; // 0x402d30
    int32_t v29; // 0x402d30
    int32_t v30; // 0x402d30
    int32_t v31; // 0x402d30
    if (v23 > v24) {
        int32_t v32 = v23 - v24; // 0x402e5b
        *v22 = v32;
        v25 = v24;
        v30 = v21;
        v28 = v32;
        if (v32 < 1) {
            goto lab_0x402e8c;
        } else {
            // 0x402e64
            v27 = v32;
            if (v8 == 111) {
                goto lab_0x402e7e;
            } else {
                // 0x402e6a
                if ((*(char *)(a1 + 5) & 8) == 0) {
                    goto lab_0x402e7e;
                } else {
                    int32_t v33 = v32 - 2; // 0x402e73
                    v27 = v33;
                    v26 = v24;
                    v31 = v21;
                    v29 = v33;
                    if (v33 < 1) {
                        goto lab_0x402f17;
                    } else {
                        goto lab_0x402e7e;
                    }
                }
            }
        }
    } else {
        // 0x402f05
        *v22 = -1;
        v25 = a1;
        v30 = v21;
        v28 = -1;
        goto lab_0x402e8c;
    }
  lab_0x402e8c:;
    int32_t v34 = v25; // 0x402e90
    int32_t v35 = v30; // 0x402e90
    int32_t v36 = v28; // 0x402e90
    if (v8 == 111) {
        goto lab_0x402e9b;
    } else {
        // 0x402e92
        v34 = v25;
        v35 = v30;
        v36 = v28;
        v26 = v25;
        v31 = v30;
        v29 = v28;
        if ((*(char *)(a1 + 5) & 8) != 0) {
            goto lab_0x402f17;
        } else {
            goto lab_0x402e9b;
        }
    }
  lab_0x402e9b:;
    int32_t v37 = v34; // 0x402e9d
    int32_t v38 = v36; // 0x402e9d
    if (v36 >= 1) {
        // 0x402e9f
        v37 = v34;
        v38 = v36;
        if ((*(char *)(a1 + 5) & 4) == 0) {
            int32_t v39 = 1; // 0x402eb8
            int32_t v40 = v39; // 0x402ec0
            v37 = ___pformat_putc();
            v38 = -1;
            while (v39 != v36) {
                // 0x402eb0
                v39 = v40 + 1;
                v40 = v39;
                v37 = ___pformat_putc();
                v38 = -1;
            }
        }
    }
    int32_t result = v37; // 0x402ece
    if (v35 > v4) {
        int32_t v41 = v35 - 1; // 0x402ed7
        int32_t v42 = v41; // 0x402ee0
        result = ___pformat_putc();
        while (v41 > v4) {
            // 0x402ed0
            v41 = v42 - 1;
            v42 = v41;
            result = ___pformat_putc();
        }
    }
    // 0x402ee2
    if (v38 < 1) {
        // 0x402efa
        return result;
    }
    int32_t v43 = 1; // 0x402ef0
    int32_t v44 = v43; // 0x402ef8
    int32_t result2 = ___pformat_putc(); // 0x402ef8
    while (v43 != v38) {
        // 0x402ee8
        v43 = v44 + 1;
        v44 = v43;
        result2 = ___pformat_putc();
    }
    // 0x402efa
    return result2;
  lab_0x402e7e:
    // 0x402e7e
    v25 = v24;
    v30 = v21;
    v28 = v27;
    if (*v1 < 0) {
        int32_t v45 = *(int32_t *)(a1 + 4) & 1536; // 0x402f52
        v25 = v45;
        v30 = v21;
        v28 = v27;
        if (v45 == 512) {
            int32_t v46 = v21; // 0x402f62
            int32_t v47 = v27 - 1;
            int32_t v48 = v47 - 1; // 0x402f63
            *(char *)v46 = 48;
            v46++;
            v25 = v47;
            v30 = v46;
            v28 = v48;
            while (v47 >= 0 == (v47 != 0)) {
                // 0x402f63
                v47 = v48;
                v48 = v47 - 1;
                *(char *)v46 = 48;
                v46++;
                v25 = v47;
                v30 = v46;
                v28 = v48;
            }
        }
    }
    goto lab_0x402e8c;
  lab_0x402f17:
    // 0x402f17
    *(char *)(v31 + 1) = 48;
    *(char *)v31 = (char)v8;
    v34 = v26 & -256 | v8 % 256;
    v35 = v31 + 2;
    v36 = v29;
    goto lab_0x402e9b;
}

// Address range: 0x402fb0 - 0x403094
int32_t ___pformat_wputchars(void) {
    // 0x402fb0
    int32_t v1; // bp-36, 0x402fb0
    _wcrtomb((int32_t)&v1, 0);
    int32_t v2; // 0x402fb0
    int32_t v3 = *(int32_t *)(v2 + 12); // 0x402fd0
    int32_t v4; // 0x402fb0
    int32_t v5 = v3 > -1 == v4 > v3 ? v3 : v4;
    int32_t * v6 = (int32_t *)(v2 + 8); // 0x402fe2
    int32_t v7 = *v6; // 0x402fe2
    int32_t v8; // 0x402fb0
    if (v7 > v5) {
        int32_t v9 = v7 - v5; // 0x402fed
        *v6 = v9;
        v8 = v9;
        if (v9 >= 1) {
            // 0x402ff6
            v8 = v9;
            if ((*(char *)(v2 + 5) & 4) == 0) {
                // 0x402ffc
                *v6 = v9 - 1;
                ___pformat_putc();
                int32_t v10 = *v6; // 0x40300c
                int32_t v11 = v10 - 1; // 0x40300f
                *v6 = v11;
                v8 = v11;
                while (v10 != 0) {
                    // 0x403000
                    ___pformat_putc();
                    v10 = *v6;
                    v11 = v10 - 1;
                    *v6 = v11;
                    v8 = v11;
                }
            }
        }
    } else {
        // 0x403084
        *v6 = -1;
        v8 = -1;
    }
    int32_t v12 = v8; // 0x40301a
    if (v5 >= 1) {
        // 0x40301c
        int32_t v13; // bp-60, 0x402fb0
        int32_t v14 = &v13; // 0x402fd3
        int32_t * v15 = (int32_t *)(v14 + 8); // 0x403022
        int32_t v16 = v5;
        int32_t v17; // 0x402fb0
        *(int32_t *)(v14 - 4) = v17;
        *(int32_t *)(v14 - 8) = v14 + 40;
        *(int32_t *)(v14 - 12) = (int32_t)*(int16_t *)*v15;
        *(int32_t *)(v14 - 16) = v14 + 24;
        int32_t v18 = _wcrtomb((int32_t)&g36, (int32_t)&g36); // 0x403034
        int32_t v19 = 0; // 0x403040
        while (v18 >= 1) {
            // 0x40301c
            v19++;
            ___pformat_putc();
            while (v19 != v18) {
                // 0x403044
                v19++;
                ___pformat_putc();
            }
            // 0x403055
            *v15 = *v15 + 2;
            if (v16 < 2) {
                // break -> 0x40306f
                break;
            }
            v16--;
            *(int32_t *)(v14 - 4) = v18;
            *(int32_t *)(v14 - 8) = v14 + 40;
            *(int32_t *)(v14 - 12) = (int32_t)*(int16_t *)*v15;
            *(int32_t *)(v14 - 16) = v14 + 24;
            v18 = _wcrtomb((int32_t)&g36, (int32_t)&g36);
            v19 = 0;
        }
        // 0x40306f
        v12 = *v6;
    }
    int32_t v20 = v12; // 0x40306f
    int32_t result = v20 - 1; // 0x403074
    *v6 = result;
    if (v20 >= 0 != v20 != 0) {
        // 0x40307c
        return result;
    }
    ___pformat_putc();
    int32_t v21 = *v6; // 0x40306f
    int32_t result2 = v21 - 1; // 0x403074
    *v6 = result2;
    while (v21 >= 0 == (v21 != 0)) {
        // 0x403063
        ___pformat_putc();
        v21 = *v6;
        result2 = v21 - 1;
        *v6 = result2;
    }
    // 0x40307c
    return result2;
}

// Address range: 0x4030a0 - 0x403138
int32_t ___pformat_putchars(void) {
    // 0x4030a0
    int32_t v1; // 0x4030a0
    int32_t v2 = *(int32_t *)(v1 + 12); // 0x4030a5
    int32_t v3; // 0x4030a0
    int32_t v4 = v2 > -1 == v3 > v2 ? v2 : v3;
    int32_t * v5 = (int32_t *)(v1 + 8); // 0x4030b4
    int32_t v6 = *v5; // 0x4030b4
    int32_t v7; // 0x4030a0
    if (v6 > v4) {
        int32_t v8 = v6 - v4; // 0x4030bb
        *v5 = v8;
        v7 = v8;
        if (v8 >= 1) {
            // 0x4030c4
            v7 = v8;
            if ((*(char *)(v1 + 5) & 4) == 0) {
                // 0x40311a
                *v5 = v8 - 1;
                ___pformat_putc();
                int32_t v9 = *v5; // 0x40312c
                int32_t v10 = v9 - 1; // 0x40312f
                *v5 = v10;
                v7 = v10;
                while (v9 != 0) {
                    // 0x403120
                    ___pformat_putc();
                    v9 = *v5;
                    v10 = v9 - 1;
                    *v5 = v10;
                    v7 = v10;
                }
            }
        }
    } else {
        // 0x40310d
        *v5 = -1;
        v7 = -1;
    }
    int32_t v11 = v7; // 0x4030cc
    if (v4 == 0) {
        goto lab_0x4030fc;
    } else {
        int32_t v12; // 0x4030a0
        int32_t v13 = v12 + 1; // 0x4030d5
        ___pformat_putc();
        v12 = v13;
        // 0x4030ce
        int32_t v14; // 0x4030a0
        while (v13 != v4 + v14) {
            // 0x4030d0
            v13 = v12 + 1;
            ___pformat_putc();
            v12 = v13;
        }
        int32_t v15 = *v5; // 0x4030df
        int32_t result = v15 - 1; // 0x4030e4
        *v5 = result;
        if (v15 < 1) {
            // 0x403109
            return result;
        }
        goto lab_0x4030f0;
    }
  lab_0x4030fc:;
    int32_t v16 = v11; // 0x4030fc
    int32_t v17 = v16 - 1; // 0x403101
    *v5 = v17;
    int32_t result2 = v17; // 0x403107
    if (v16 >= 0 != v16 != 0) {
        // 0x403109
        return result2;
    }
    goto lab_0x4030f0;
  lab_0x4030f0:
    // 0x4030f0
    ___pformat_putc();
    v11 = *v5;
    goto lab_0x4030fc;
}

// Address range: 0x403140 - 0x4031f1
int32_t ___pformat_emit_inf_or_nan(void) {
    // 0x403140
    int32_t v1; // 0x403140
    *(int32_t *)(v1 + 12) = -1;
    int32_t v2; // 0x403140
    int32_t v3; // 0x403140
    int32_t v4; // 0x403140
    int32_t v5; // bp-19, 0x403140
    char v6; // bp-20, 0x403140
    int32_t v7; // 0x403140
    if (v7 == 0) {
        int32_t v8 = *(int32_t *)(v1 + 4); // 0x4031b0
        if ((v8 & 256) == 0) {
            int32_t v9 = &v6;
            v2 = v9;
            v3 = v8;
            v4 = v9;
            if ((v8 & 64) != 0) {
                // 0x4031cf
                v6 = 32;
                v2 = v9;
                v3 = v8;
                v4 = &v5;
            }
        } else {
            // 0x4031b8
            v6 = 43;
            v2 = &v6;
            v3 = v8;
            v4 = &v5;
        }
    } else {
        // 0x403156
        v6 = 45;
        v2 = &v6;
        v3 = *(int32_t *)(v1 + 4);
        v4 = &v5;
    }
    int32_t v10 = v3 & 32; // 0x40316e
    *(char *)v4 = (char)(v10 | v2 & 223);
    int32_t v11 = v4;
    int32_t v12 = v11 + 1;
    int32_t v13; // 0x403140
    int32_t v14 = v13 + 1;
    *(char *)v12 = *(char *)v14 & -33 | (char)v10;
    while (v11 != v4 + 1) {
        // 0x403184
        v11 = v12;
        v12 = v11 + 1;
        v14++;
        *(char *)v12 = *(char *)v14 & -33 | (char)v10;
    }
    // 0x403192
    return ___pformat_putchars();
}

// Address range: 0x403200 - 0x4032d7
int32_t ___pformat_float(int32_t a1, int32_t a2, int32_t a3) {
    // 0x403200
    int32_t v1; // 0x403200
    int32_t * v2 = (int32_t *)(v1 + 12); // 0x403207
    int32_t v3 = *v2; // 0x403207
    int32_t v4 = v3; // 0x40320c
    if (v3 < 0) {
        // 0x4032cb
        *v2 = 6;
        v4 = 6;
    }
    // 0x403212
    int32_t v5; // bp-16, 0x403200
    int32_t v6; // bp-20, 0x403200
    int32_t v7; // 0x403200
    int32_t v8 = ___pformat_cvt(a1, a2, a3, v7, v4, &v6, &v5, a2); // 0x403252
    if (v6 == -0x8000) {
        // 0x4032af
        ___pformat_emit_inf_or_nan();
        return ___freedtoa(v8);
    }
    // 0x403268
    ___pformat_emit_float(v1);
    int32_t * v9 = (int32_t *)(v1 + 8); // 0x403277
    int32_t v10 = *v9; // 0x403277
    *v9 = v10 - 1;
    if (v10 < 1) {
        // 0x4032a0
        return ___freedtoa(v8);
    }
    ___pformat_putc();
    int32_t v11 = *v9; // 0x403293
    *v9 = v11 - 1;
    while (v11 >= 0 == (v11 != 0)) {
        // 0x403287
        ___pformat_putc();
        v11 = *v9;
        *v9 = v11 - 1;
    }
    // 0x4032a0
    return ___freedtoa(v8);
}

// Address range: 0x4032e0 - 0x403517
int32_t ___pformat_int(void) {
    // 0x4032e0
    int32_t v1; // 0x4032e0
    int32_t * v2 = (int32_t *)(v1 + 12); // 0x4032eb
    char * v3; // bp-28, 0x4032e0
    ___chkstk((int32_t *)&v3);
    int32_t v4; // bp-16, 0x4032e0
    v3 = (char *)&v4;
    int32_t * v5 = (int32_t *)(v1 + 4); // 0x403320
    int32_t v6 = *v5; // 0x403320
    int32_t v7 = &v4;
    int32_t v8 = v7; // 0x403325
    int32_t v9; // 0x4032e0
    int32_t v10; // 0x4032e0
    if ((char)v6 < 0) {
        int32_t v11; // 0x4032e0
        if (v11 < 0) {
            // 0x4034f4
            v8 = v7;
            int32_t v12; // 0x4032e0
            v9 = -v12;
            v10 = -((v11 + (int32_t)(v12 != 0)));
        } else {
            // 0x40332f
            *v5 = v6 & -129;
            v8 = (int32_t)v3;
        }
    }
    // 0x403334
    char * v13; // bp-20, 0x4032e0
    *(int32_t *)&v13 = v8;
    int32_t v14 = *v2; // 0x40333e
    if ((v10 || v9) != 0) {
        int32_t v15 = (int32_t)&v3; // 0x4032e6
        int32_t * v16 = (int32_t *)(v15 - 4); // 0x403340
        int32_t * v17 = (int32_t *)(v15 - 8); // 0x403342
        int32_t * v18 = (int32_t *)(v15 - 12); // 0x403344
        int32_t * v19 = (int32_t *)(v15 - 16); // 0x403345
        *v16 = 0;
        *v17 = 10;
        *v18 = v10;
        *v19 = v9;
        int32_t v20 = ___umoddi3((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x403346
        *v13 = (char)v20 + 48;
        int32_t v21 = (int32_t)v13 + 1; // 0x403356
        v13 = (char *)v21;
        *v16 = 0;
        *v17 = 10;
        *v18 = v10;
        *v19 = v9;
        int32_t v22 = ___udivdi3(v21, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x403360
        while ((v22 || v21) != 0) {
            int32_t v23 = v21;
            *v16 = 0;
            *v17 = 10;
            *v18 = v23;
            *v19 = v22;
            v20 = ___umoddi3((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
            *v13 = (char)v20 + 48;
            v21 = (int32_t)v13 + 1;
            v13 = (char *)v21;
            *v16 = 0;
            *v17 = 10;
            *v18 = v23;
            *v19 = v22;
            v22 = ___udivdi3(v21, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
        }
        // 0x403372
        v14 = *v2;
    }
    // 0x403375
    char * v24; // 0x4032e0
    if (v14 < 1) {
        // 0x403375
        v24 = v13;
    } else {
        int32_t v25 = (int32_t)v3; // 0x403379
        int32_t v26 = (int32_t)v13; // 0x40337c
        uint32_t v27 = v25 - v26 + v14; // 0x403383
        v24 = v13;
        if (v27 >= 1) {
            int32_t v28 = v26; // 0x40338f
            *(char *)v28 = 48;
            v28++;
            while (v28 != v14 + v25) {
                // 0x403390
                *(char *)v28 = 48;
                v28++;
            }
            char * v29 = (char *)(v27 + (int32_t)v13); // 0x40339d
            v13 = v29;
            v24 = v29;
        }
    }
    char * v30 = v24; // 0x4033a6
    if (v24 == v3) {
        // 0x403500
        v30 = v24;
        if (*v2 != 0) {
            // 0x40350b
            *v24 = 48;
            char * v31 = (char *)((int32_t)v3 + 1); // 0x40350f
            v13 = v31;
            v30 = v31;
        }
    }
    int32_t * v32 = (int32_t *)(v1 + 8); // 0x403302
    int32_t v33 = *v32; // 0x4033ac
    int32_t v34; // 0x4033ce
    int32_t v35; // 0x4033d9
    if (v33 < 1) {
        goto lab_0x40347a;
    } else {
        int32_t v36 = (int32_t)v3 - (int32_t)v30 + v33; // 0x4033bf
        *v32 = v36;
        if (v36 < 1) {
            goto lab_0x40347a;
        } else {
            // 0x4033ce
            v34 = *v5;
            v35 = v36;
            if ((v34 & 448) != 0) {
                // 0x4033d9
                v35 = v36 - 1;
                *v32 = v35;
            }
            // 0x4033dd
            if (*v2 < 0) {
                if ((v34 & 1536) != 512) {
                    goto lab_0x4033e8;
                } else {
                    // 0x4034ca
                    *v32 = v35 - 1;
                    if (v35 < 1) {
                        goto lab_0x4033ed;
                    } else {
                        *v13 = 48;
                        int32_t v37 = *v32; // 0x4034e2
                        v13 = (char *)((int32_t)v13 + 1);
                        *v32 = v37 - 1;
                        while (v37 >= 0 == (v37 != 0)) {
                            // 0x4034db
                            *v13 = 48;
                            v37 = *v32;
                            v13 = (char *)((int32_t)v13 + 1);
                            *v32 = v37 - 1;
                        }
                        goto lab_0x40347a;
                    }
                }
            } else {
                goto lab_0x4033e8;
            }
        }
    }
  lab_0x40347a:;
    int32_t v38 = *v5; // 0x40347a
    int32_t v39 = v38; // 0x40347f
    if ((char)v38 < 0) {
        goto lab_0x4033f5;
    } else {
        goto lab_0x403485;
    }
  lab_0x4033f5:
    // 0x4033f5
    *v13 = 45;
    char * v40 = (char *)((int32_t)v13 + 1); // 0x4033fc
    v13 = v40;
    char * v41 = v40; // 0x4033fc
    goto lab_0x4033ff;
  lab_0x403485:
    // 0x403485
    if ((v39 & 256) == 0) {
        // 0x403499
        v41 = v13;
        if ((v39 & 64) != 0) {
            // 0x4034a2
            *v13 = 32;
            char * v47 = (char *)((int32_t)v13 + 1); // 0x4034a9
            v13 = v47;
            v41 = v47;
        }
    } else {
        // 0x40348a
        *v13 = 43;
        char * v48 = (char *)((int32_t)v13 + 1); // 0x403491
        v13 = v48;
        v41 = v48;
    }
    goto lab_0x4033ff;
  lab_0x4033ff:
    // 0x4033ff
    if (v3 >= v41) {
        goto lab_0x40343c;
    } else {
        int32_t v42 = (int32_t)v41; // 0x403409
        v42--;
        ___pformat_putc();
        while (v42 > (int32_t)v3) {
            // 0x403410
            v42--;
            ___pformat_putc();
        }
        int32_t v43 = *v32; // 0x403421
        int32_t result = v43 - 1; // 0x403426
        *v32 = result;
        if (v43 < 1) {
            // 0x403449
            return result;
        }
        // 0x403430
        ___pformat_putc();
        goto lab_0x40343c;
    }
  lab_0x4033e8:
    if ((v34 & 1024) == 0) {
        // 0x403454
        *v32 = v35 - 1;
        if (v35 < 1) {
            goto lab_0x4033ed;
        } else {
            ___pformat_putc();
            int32_t v44 = *v32; // 0x40346d
            *v32 = v44 - 1;
            while (v44 >= 0 == (v44 != 0)) {
                // 0x403461
                ___pformat_putc();
                v44 = *v32;
                *v32 = v44 - 1;
            }
            goto lab_0x40347a;
        }
    } else {
        goto lab_0x4033ed;
    }
  lab_0x40343c:;
    int32_t v45 = *v32; // 0x40343c
    int32_t v46 = v45 - 1; // 0x403441
    *v32 = v46;
    int32_t result2 = v46; // 0x403447
    if (v45 >= 0 != v45 != 0) {
        // 0x403449
        return result2;
    }
    // 0x403430
    ___pformat_putc();
    goto lab_0x40343c;
  lab_0x4033ed:
    // 0x4033ed
    v39 = v34;
    if ((char)v34 >= 0) {
        goto lab_0x403485;
    } else {
        goto lab_0x4033f5;
    }
}

// Address range: 0x403520 - 0x40394a
int32_t ___pformat_emit_xfloat(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    int32_t * v1 = (int32_t *)(a5 + 12); // 0x403543
    uint32_t v2 = *v1; // 0x403543
    int32_t v3; // 0x403520
    int32_t v4; // 0x403520
    int32_t v5; // 0x403520
    int32_t v6; // 0x403520
    if (v2 < 15) {
        int32_t v7 = a1; // 0x403561
        int32_t v8 = a2; // 0x403561
        if (a2 >= 0) {
            int32_t v9 = 2 * a2 | (int32_t)(a1 < 0); // 0x403563
            int32_t v10 = 2 * a1; // 0x403567
            int32_t v11 = v10; // 0x40356b
            int32_t v12 = v9; // 0x40356b
            v7 = v10;
            v8 = v9;
            while (v9 >= 0) {
                // 0x403563
                v9 = 2 * v12 | (int32_t)(v11 < 0);
                v10 = 2 * v11;
                v11 = v10;
                v12 = v9;
                v7 = v10;
                v8 = v9;
            }
        }
        int32_t v13 = 4 * v2; // 0x40357c
        int32_t v14 = 56 - v13; // 0x403580
        uint32_t v15 = 0x80000000 * v8 | v7 / 2; // 0x40358a
        uint32_t v16 = v14 & 28; // 0x40358e
        int32_t v17 = 4 << v16;
        int32_t v18 = (v14 & 32) == 0 ? v16 == 0 ? 0 : 4 >> 32 - v16 : v17;
        int32_t v19 = v15 + ((v14 & 32) == 0 ? v17 : 0); // 0x40359e
        int32_t v20 = v8 / 2 + v18 + (int32_t)(v19 < v15); // 0x4035a0
        int32_t v21; // 0x403520
        int32_t v22; // 0x403520
        int32_t v23; // 0x403520
        if (v20 < 0) {
            // 0x40390c
            v23 = v19;
            v22 = v20;
            v21 = (0x10000 * a3 + 0x10000) / 0x10000;
        } else {
            // 0x4035b2
            v23 = 2 * v19;
            v22 = 2 * v20 | (int32_t)(v19 < 0);
            v21 = a3;
        }
        int32_t v24 = 60 - v13; // 0x4035cf
        uint32_t v25 = v24 & 28; // 0x4035d2
        int32_t v26 = v23; // 0x4035d2
        if (v25 != 0) {
            v26 = v22 << 32 - v25 | v23 >> v25;
        }
        int32_t v27 = v22 >> v25;
        int32_t v28 = (v24 & 32) == 0 ? v26 : v27;
        int32_t v29 = (v24 & 32) == 0 ? v27 : 0;
        v3 = v21;
        v6 = v28;
        v5 = v29;
        v4 = v21;
        if ((v28 || v29) != 0) {
            goto lab_0x403654;
        } else {
            goto lab_0x4035ee;
        }
    } else {
        // 0x403646
        v3 = a3;
        v6 = a1;
        v5 = a2;
        v4 = a3;
        if ((a2 || a1) == 0) {
            goto lab_0x4035ee;
        } else {
            goto lab_0x403654;
        }
    }
  lab_0x4036d1:;
    // 0x4036d1
    int32_t v30; // 0x403520
    uint32_t v31 = v30 % 16; // 0x4036b4
    char * v32; // 0x403520
    char * v33 = v32;
    int32_t v34; // 0x403520
    int32_t v35 = v34;
    int32_t v36; // bp-38, 0x403520
    char * v37; // 0x403520
    int32_t v38; // 0x403520
    int32_t v39; // 0x40366a
    if (v31 == 0) {
        if (&v36 < (int32_t *)v33) {
            // 0x403681
            v38 = v31 | 48;
            goto lab_0x403684;
        } else {
            // 0x40367a
            v37 = v33;
            if (*v1 < 0) {
                goto lab_0x40368f;
            } else {
                // 0x403681
                v38 = v31 | 48;
                goto lab_0x403684;
            }
        }
    } else {
        if (v31 < 10) {
            // 0x403681
            v38 = v31 | 48;
            goto lab_0x403684;
        } else {
            // 0x4036da
            v38 = v31 + 55 | v39;
            goto lab_0x403684;
        }
    }
  lab_0x403706:;
    // 0x403706
    int32_t v40; // 0x403520
    int32_t v41 = v40; // 0x40370b
    int32_t v42; // 0x403520
    int32_t v43 = v42; // 0x40370b
    int32_t v44; // 0x403520
    int32_t v45 = v44; // 0x40370b
    if (v42 < 2 != (v44 == 0)) {
        uint32_t v46 = v45;
        uint32_t v47 = v43;
        int32_t v48 = v41;
        int32_t v49 = 0x80000000 * v46 | v47 / 2; // 0x403720
        int32_t v50 = (0x10000 * v48 - 0x10000) / 0x10000; // 0x40372a
        v41 = v50;
        v43 = v49;
        v45 = v46 / 2;
        while (v46 < 2 != v49 < 2) {
            // 0x403720
            v46 = v45;
            v47 = v43;
            v48 = v41;
            v49 = 0x80000000 * v46 | v47 / 2;
            v50 = (0x10000 * v48 - 0x10000) / 0x10000;
            v41 = v50;
            v43 = v49;
            v45 = v46 / 2;
        }
    }
    goto lab_0x4036d1;
  lab_0x403684:
    // 0x403684
    *v33 = (char)v38;
    v37 = (char *)((int32_t)v33 + 1);
    goto lab_0x40368f;
  lab_0x40368f:;
    char * v51 = v37;
    int32_t v52; // 0x403520
    int32_t v53 = 0x10000000 * v52 | v30 / 16; // 0x403697
    int32_t v54 = v52 / 16; // 0x40369b
    v30 = v53;
    v52 = v54;
    v34 = v35;
    v32 = v51;
    if ((v53 || v54) == 0) {
        // break -> 0x40373f
        goto lab_0x40373f;
    }
    goto lab_0x4036b0;
  lab_0x403654:;
    int32_t * v60 = (int32_t *)(a5 + 4);
    int32_t v61 = *v60; // 0x403660
    v39 = v61 & 32;
    v30 = v6;
    v52 = v5;
    v34 = v4;
    v32 = (char *)&v36;
    while (true) {
      lab_0x4036b0:
        // 0x4036b0
        if ((v52 || v30 & -16) == 0) {
            char * v55; // 0x403520
            if (&v36 < (int32_t *)v55) {
                // 0x4036fa
                *v55 = 46;
                goto lab_0x403706;
            } else {
                // 0x4036ed
                char * v56; // 0x403520
                char v57 = *v56; // 0x4036ed
                if ((v57 & 8) != 0) {
                    // 0x4036fa
                    *v55 = 46;
                    goto lab_0x403706;
                } else {
                    int32_t v58 = *v1; // 0x4036f3
                    if (v58 < 1) {
                        goto lab_0x403706;
                    } else {
                        // 0x4036fa
                        *v55 = 46;
                        goto lab_0x403706;
                    }
                }
            }
        } else {
            uint32_t v59 = *v1; // 0x4036c6
            if (v59 >= 1) {
                // 0x4036cd
                *v1 = v59 - 1;
            }
            goto lab_0x4036d1;
        }
    }
  lab_0x40373f:;
    int32_t v62 = &v36; // 0x403658
    char v63 = v61; // 0x403663
    int32_t * v64; // 0x403520
    int32_t v65; // 0x403520
    int32_t v66; // 0x403520
    int32_t v67; // 0x403520
    int32_t v68; // 0x403520
    char * v69; // 0x403520
    char * v70; // 0x403520
    char v71; // 0x403520
    char v72; // 0x403520
    char v73; // 0x403520
    int32_t v74; // 0x403520
    int32_t v75; // 0x403520
    int32_t v76; // 0x403520
    if (&v36 == (int32_t *)v51) {
        // 0x403927
        v67 = v35;
        v74 = v62;
        v71 = v63;
        v66 = *v1;
        goto lab_0x4035fd;
    } else {
        int32_t v77 = *(int32_t *)(a5 + 8); // 0x40374d
        v75 = v62;
        v72 = v63;
        v69 = v51;
        v64 = v60;
        v68 = v35;
        v76 = v62;
        v73 = v63;
        v70 = v51;
        v65 = v77;
        if (v77 < 1) {
            goto lab_0x403624;
        } else {
            goto lab_0x403758;
        }
    }
  lab_0x4035ee:
    // 0x4035ee
    v67 = v3;
    v74 = &v36;
    v71 = (char)*(int32_t *)(a5 + 4);
    v66 = v2;
    goto lab_0x4035fd;
  lab_0x4035fd:;
    char v78 = v71;
    int32_t v79 = v74;
    int32_t v80 = v67;
    int32_t * v81; // 0x403520
    int32_t v82; // bp-37, 0x403520
    int32_t * v83; // 0x403520
    if (v66 < 1) {
        // 0x40392f
        v83 = &v82;
        v81 = &v36;
        if ((*(char *)(a5 + 5) & 8) != 0) {
            goto lab_0x403605;
        } else {
            goto lab_0x403616;
        }
    } else {
        goto lab_0x403605;
    }
  lab_0x403624:;
    int32_t v84 = v75; // 0x40362f
    char * v85 = v69; // 0x40362f
    int32_t v86 = 2; // 0x40362f
    int32_t v87 = v75; // 0x40362f
    char * v88 = v69; // 0x40362f
    int32_t v89 = v72; // 0x40362f
    int32_t v90 = 2; // 0x40362f
    if (v72 >= 0) {
        goto lab_0x4037d4;
    } else {
        goto lab_0x403635;
    }
  lab_0x403758:;
    int32_t v91 = v65;
    char v92 = v73;
    int32_t * v93 = v64;
    int32_t v94 = *v1; // 0x403760
    int32_t v95 = v94 > 0 ? v94 : 0;
    int32_t v96 = *v93 & 448; // 0x403772
    int32_t v97 = 6 - v76 + (int32_t)v70 + v95 + v96 - (v96 | (int32_t)(v96 == 0)); // 0x40377f
    if (0x10000 * v68 >= 0xa0000) {
        int32_t v98; // 0x403520
        int32_t v99 = v98;
        int32_t v100; // 0x403520
        v100++;
        int32_t v101; // 0x403520
        v101++;
        v98 = v99 / 10;
        while (v99 >= 10) {
            // 0x4037a0
            v99 = v98;
            int32_t v102 = v101;
            int32_t v103 = v100;
            int32_t v104 = v103 + 1; // 0x4037ac
            int32_t v105 = v102 + 1; // 0x4037b0
            v100 = v104;
            v101 = v105;
            v98 = v99 / 10;
        }
    }
    int32_t v106 = 2;
    int32_t v107 = v97;
    int32_t v108; // 0x403520
    if (v91 > v107) {
        uint32_t v109 = v91 - v107; // 0x4038ba
        int32_t v110 = v92; // 0x4038bc
        int32_t * v111 = (int32_t *)(a5 + 8); // 0x4038c0
        *v111 = v109;
        v108 = v110;
        if ((v92 >> 7 & 6) == 0) {
            // 0x4038cc
            *v111 = v109 - 1;
            v108 = v110;
            if (v109 >= 1) {
                ___pformat_putc();
                int32_t v112 = *v111; // 0x4038e6
                *v111 = v112 - 1;
                while (v112 >= 0 == (v112 != 0)) {
                    // 0x4038da
                    ___pformat_putc();
                    v112 = *v111;
                    *v111 = v112 - 1;
                }
                // 0x4038f3
                v108 = *v93;
            }
        }
    } else {
        // 0x4037c1
        *(int32_t *)(a5 + 8) = -1;
        v108 = v92;
    }
    // 0x4037cc
    v84 = v76;
    v85 = v70;
    v86 = v106;
    v87 = v76;
    v88 = v70;
    v89 = v108;
    v90 = v106;
    if ((char)v108 < 0) {
        goto lab_0x403635;
    } else {
        goto lab_0x4037d4;
    }
  lab_0x403605:
    // 0x403605
    v36 = 46;
    int32_t v113; // bp-36, 0x403520
    v83 = &v113;
    v81 = &v82;
    goto lab_0x403616;
  lab_0x4037d4:;
    // 0x4037d4
    int32_t v125; // 0x403520
    char * v119; // 0x403520
    int32_t v120; // 0x403520
    if ((v89 & 256) != 0) {
        // 0x4038fb
        ___pformat_putc();
        v120 = v87;
        v119 = v88;
        v125 = v90;
    } else {
        // 0x4037dd
        v120 = v87;
        v119 = v88;
        v125 = v90;
        if ((v89 & 64) != 0) {
            // 0x403916
            ___pformat_putc();
            v120 = v87;
            v119 = v88;
            v125 = v90;
        }
    }
    goto lab_0x4037e6;
  lab_0x403635:
    // 0x403635
    ___pformat_putc();
    v120 = v84;
    v119 = v85;
    v125 = v86;
    goto lab_0x4037e6;
  lab_0x403616:
    // 0x403616
    *(char *)v81 = 48;
    int32_t v114 = *(int32_t *)(a5 + 8); // 0x403619
    v75 = v79;
    v72 = v78;
    v69 = (char *)v83;
    if (v114 >= 0 == (v114 != 0)) {
        // 0x403616
        v64 = (int32_t *)(a5 + 4);
        v68 = v80;
        v76 = v79;
        v73 = v78;
        v70 = (char *)v83;
        v65 = v114;
        goto lab_0x403758;
    } else {
        goto lab_0x403624;
    }
  lab_0x4037e6:
    // 0x4037e6
    ___pformat_putc();
    ___pformat_putc();
    int32_t * v115 = (int32_t *)(a5 + 8); // 0x403802
    uint32_t v116 = *v115; // 0x403802
    if (v116 >= 1) {
        // 0x403809
        if ((*(char *)(a5 + 5) & 2) != 0) {
            // 0x40380f
            *v115 = v116 - 1;
            ___pformat_putc();
            int32_t v117 = *v115; // 0x40381f
            *v115 = v117 - 1;
            while (v117 >= 0 == (v117 != 0)) {
                // 0x403813
                ___pformat_putc();
                v117 = *v115;
                *v115 = v117 - 1;
            }
        }
    }
    int32_t v118 = (int32_t)v119; // 0x40382c
    if (v120 < v118) {
        int32_t v121 = v118 - 1; // 0x403851
        if (*(char *)v121 != 46) {
            // 0x403840
            ___pformat_putc();
        } else {
            // 0x403858
            ___pformat_emit_radix_point();
        }
        int32_t v122 = v121; // 0x40384f
        while (v120 < v121) {
            // 0x403851
            v121 = v122 - 1;
            if (*(char *)v121 != 46) {
                // 0x403840
                ___pformat_putc();
            } else {
                // 0x403858
                ___pformat_emit_radix_point();
            }
            // 0x40384a
            v122 = v121;
        }
    }
    int32_t v123 = *v1; // 0x40386d
    *v1 = v123 - 1;
    ___pformat_putc();
    while (v123 >= 0 == (v123 != 0)) {
        // 0x40386d
        v123 = *v1;
        *v1 = v123 - 1;
        ___pformat_putc();
    }
    int32_t * v124 = (int32_t *)(a5 + 4); // 0x40388a
    *v124 = *v124 | 448;
    *v115 = *v115 + 0x10000 * v125 / 0x10000;
    return ___pformat_int();
}

// Address range: 0x403950 - 0x403a0b
int32_t ___pformat_emit_efloat(int32_t a1) {
    int32_t v1 = 1; // 0x40397e
    int32_t v2; // 0x403950
    int32_t v3 = v2 - 1; // 0x40397e
    int32_t v4 = 1; // 0x40397e
    if (v2 >= 11) {
        v3 /= 10;
        v1++;
        v4 = v1;
        while (v3 >= 10) {
            // 0x403980
            v3 /= 10;
            v1++;
            v4 = v1;
        }
    }
    int32_t v5 = v4;
    int32_t * v6 = (int32_t *)(a1 + 32); // 0x403998
    int32_t v7 = *v6; // 0x403998
    int32_t v8 = v5 < v7 ? v7 : v5;
    int32_t * v9 = (int32_t *)(a1 + 8); // 0x4039a3
    int32_t v10 = *v9; // 0x4039a3
    int32_t v11 = v8 + 2; // 0x4039a6
    *v9 = v10 > v11 ? v10 - v11 : -1;
    ___pformat_emit_float(a1);
    *(int32_t *)(a1 + 12) = *v6;
    int32_t * v12 = (int32_t *)(a1 + 4); // 0x4039cc
    *v12 = *v12 | 448;
    ___pformat_putc();
    *v9 = v8 + 1 + *v9;
    return ___pformat_int();
}

// Address range: 0x403a10 - 0x403ba0
int32_t ___pformat_gfloat(int32_t a1, int32_t a2, int32_t a3) {
    // 0x403a10
    int32_t v1; // 0x403a10
    int32_t * v2 = (int32_t *)(v1 + 12); // 0x403a18
    int32_t v3 = *v2; // 0x403a18
    int32_t v4; // 0x403a10
    if (v3 < 0) {
        // 0x403b32
        *v2 = 6;
        v4 = 6;
    } else {
        // 0x403a24
        v4 = v3;
        if (v3 == 0) {
            // 0x403b21
            *v2 = 1;
            v4 = 1;
        }
    }
    // 0x403a2a
    int32_t v5; // bp-16, 0x403a10
    int32_t v6; // bp-20, 0x403a10
    int32_t v7; // 0x403a10
    int32_t str = ___pformat_cvt(a1, a2, a3, v7, v4, &v6, &v5, a3); // 0x403a67
    if (v6 == -0x8000) {
        // 0x403b83
        ___pformat_emit_inf_or_nan();
        return ___freedtoa(str);
    }
    if (v6 >= 0xfffffffd) {
        uint32_t v8 = *v2; // 0x403a86
        if (v8 >= v6) {
            // 0x403a8d
            if ((*(char *)(v1 + 5) & 8) == 0) {
                int32_t v9 = strlen((char *)str) - v6; // 0x403b5e
                *v2 = v9;
                if (v9 < 0) {
                    int32_t * v10 = (int32_t *)(v1 + 8); // 0x403b6e
                    uint32_t v11 = *v10; // 0x403b6e
                    if (v11 >= 1) {
                        // 0x403b79
                        *v10 = v11 + v9;
                    }
                }
            } else {
                // 0x403a97
                *v2 = v8 - v6;
            }
            // 0x403a9c
            ___pformat_emit_float(v1);
            int32_t * v12 = (int32_t *)(v1 + 8); // 0x403aad
            int32_t v13 = *v12; // 0x403aad
            *v12 = v13 - 1;
            if (v13 < 1) {
                // 0x403b11
                return ___freedtoa(str);
            }
            ___pformat_putc();
            int32_t v14 = *v12; // 0x403acc
            *v12 = v14 - 1;
            while (v14 >= 0 == (v14 != 0)) {
                // 0x403ac0
                ___pformat_putc();
                v14 = *v12;
                *v12 = v14 - 1;
            }
            // 0x403ad9
            return ___freedtoa(str);
        }
    }
    // 0x403af0
    int32_t len; // 0x403a10
    if ((*(char *)(v1 + 5) & 8) == 0) {
        // 0x403b43
        len = strlen((char *)str);
    } else {
        // 0x403af6
        len = *v2;
    }
    // 0x403afd
    *v2 = len - 1;
    ___pformat_emit_efloat(v1);
    // 0x403b11
    return ___freedtoa(str);
}

// Address range: 0x403ba0 - 0x403c5c
int32_t ___pformat_efloat(int32_t a1, int32_t a2, int32_t a3) {
    // 0x403ba0
    int32_t v1; // 0x403ba0
    int32_t * v2 = (int32_t *)(v1 + 12); // 0x403ba7
    int32_t v3 = *v2; // 0x403ba7
    int32_t v4 = v3; // 0x403bac
    if (v3 < 0) {
        // 0x403c50
        *v2 = 6;
        v4 = 6;
    }
    // 0x403bb2
    int32_t v5; // bp-16, 0x403ba0
    int32_t v6; // bp-20, 0x403ba0
    int32_t v7; // 0x403ba0
    int32_t v8 = ___pformat_cvt(a1, a2, a3, v7, v4 + 1, &v6, &v5, a1); // 0x403bf3
    if (v6 == -0x8000) {
        // 0x403c30
        ___pformat_emit_inf_or_nan();
        return ___freedtoa(v8);
    }
    // 0x403c09
    ___pformat_emit_efloat(v1);
    return ___freedtoa(v8);
}

// Address range: 0x403c60 - 0x40462f
int32_t ___mingw_pformat(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    char * env_val = getenv("PRINTF_EXPONENT_DIGITS"); // 0x403c8a
    if (env_val == NULL) {
        // 0x403d70
        __get_output_format();
        goto lab_0x403cae;
    } else {
        // 0x403c9a
        if (*env_val < 51) {
            goto lab_0x403cae;
        } else {
            // 0x403d70
            __get_output_format();
            goto lab_0x403cae;
        }
    }
  lab_0x403e62:;
    // 0x403e62
    int32_t v1; // 0x403c60
    int32_t v2; // 0x403c60
    int32_t v3; // 0x403c60
    int32_t v4; // 0x403c60
    int32_t v5; // 0x403c60
    int32_t v6; // 0x403d1c
    if ((*(char *)v6 & 4) != 0) {
        // 0x404007
        *(int32_t *)(v4 - 4) = v1;
        *(int32_t *)(v4 - 8) = *(int32_t *)(v2 + 8);
        *(int32_t *)(v4 - 12) = *(int32_t *)(v2 + 4);
        int32_t v7 = v4 - 16; // 0x404016
        *(int32_t *)v7 = *(int32_t *)v2;
        v3 = v2 + 12;
        v5 = v7;
    } else {
        int32_t v8 = v4 - 16; // 0x403e6d
        *(float80_t *)v8 = (float80_t)*(float64_t *)v2;
        v3 = v2 + 8;
        v5 = v8;
    }
    // 0x403e79
    ___pformat_efloat((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
    int32_t v9; // 0x403d46
    char * v10 = (char *)v9; // 0x403e89
    int32_t v11 = v5 + 16; // 0x403e89
    int32_t v12 = v3; // 0x403e89
    int32_t v13 = v9; // 0x403e89
    goto lab_0x403d00;
  lab_0x403e2b:;
    // 0x403e2b
    int32_t v39; // 0x403c60
    int32_t v101; // 0x403c60
    int32_t v102; // 0x403c60
    if ((*(char *)v6 & 4) != 0) {
        // 0x40401c
        *(int32_t *)(v4 - 4) = v39;
        *(int32_t *)(v4 - 8) = *(int32_t *)(v2 + 8);
        *(int32_t *)(v4 - 12) = *(int32_t *)(v2 + 4);
        int32_t v103 = v4 - 16; // 0x40402b
        *(int32_t *)v103 = *(int32_t *)v2;
        v101 = v2 + 12;
        v102 = v103;
    } else {
        int32_t v104 = v4 - 16; // 0x403e36
        *(float80_t *)v104 = (float80_t)*(float64_t *)v2;
        v101 = v2 + 8;
        v102 = v104;
    }
    // 0x403e42
    ___pformat_float((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
    v10 = (char *)v9;
    v11 = v102 + 16;
    v12 = v101;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x403df4:;
    // 0x403df4
    int32_t v105; // 0x403c60
    int32_t v106; // 0x403c60
    int32_t v21; // 0x403d08
    if ((*(char *)v6 & 4) != 0) {
        // 0x404031
        *(int32_t *)(v4 - 4) = v21;
        *(int32_t *)(v4 - 8) = *(int32_t *)(v2 + 8);
        *(int32_t *)(v4 - 12) = *(int32_t *)(v2 + 4);
        int32_t v107 = v4 - 16; // 0x404040
        *(int32_t *)v107 = *(int32_t *)v2;
        v105 = v2 + 12;
        v106 = v107;
    } else {
        int32_t v108 = v4 - 16; // 0x403dff
        *(float80_t *)v108 = (float80_t)*(float64_t *)v2;
        v105 = v2 + 8;
        v106 = v108;
    }
    // 0x403e0b
    ___pformat_gfloat((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
    v10 = (char *)v9;
    v11 = v106 + 16;
    v12 = v105;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x403dd2:;
    // 0x403dd2
    int32_t * v24; // 0x403d1c
    *v24 = *v24 | 4;
    int32_t v38; // 0x403c60
    char v42 = *(char *)v38; // 0x403de4
    int32_t v43 = 4; // 0x403de4
    int32_t v44 = v38; // 0x403de4
    goto lab_0x403d3f;
  lab_0x404056:;
    int32_t v109 = *(int32_t *)v2; // 0x404056
    *(int32_t *)(v4 - 16) = v109 == 0 ? (int32_t)L"(null)" : v109;
    wcslen((int16_t *)&g36);
    ___pformat_wputchars();
    v10 = (char *)v9;
    v11 = v4;
    v12 = v2 + 4;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x404181:;
    // 0x404181
    int32_t * v28; // 0x403d33
    int32_t v110 = *v28; // 0x404183
    int32_t v111; // 0x403c60
    if (v110 == 3) {
        // 0x40442a
        *(int32_t *)(v4 + 100) = *(int32_t *)(v2 + 4);
        *(int32_t *)(v4 + 96) = *(int32_t *)v2;
        v111 = v2 + 8;
    } else {
        // 0x40418e
        *(int32_t *)(v4 + 96) = *(int32_t *)v2;
        *(int32_t *)(v4 + 100) = 0;
        v111 = v2 + 4;
        if (v110 != 2) {
            int32_t v112 = *v28; // 0x4041aa
            int32_t v113; // 0x403c60
            v111 = v113;
            int32_t * v114; // 0x403c60
            int32_t * v115; // 0x403c60
            switch (v112) {
                case 1: {
                    uint32_t v116 = *v114; // 0x40457a
                    *v115 = 0;
                    *v114 = v116 % 0x10000;
                    v111 = v113;
                    // break -> 0x4041c0
                    break;
                }
                case 4: {
                    // 0x4045ab
                    *v115 = 0;
                    int32_t v117; // 0x403c60
                    unsigned char v118 = *(char *)v117; // 0x4045b5
                    *v114 = (int32_t)v118;
                    v111 = v113;
                    // break -> 0x4041c0
                    break;
                }
            }
        }
    }
    int32_t v119 = v111;
    char v36; // 0x403c60
    if (v36 == 117) {
        // 0x4044c9
        ___pformat_int();
        v10 = (char *)v9;
        v11 = v4;
        v12 = v119;
        v13 = v9;
    } else {
        // 0x4041c9
        *(int32_t *)(v4 - 16) = v4 + 28;
        ___pformat_xint((int32_t)&g36);
        v10 = (char *)v9;
        v11 = v4;
        v12 = v119;
        v13 = v9;
    }
    goto lab_0x403d00;
  lab_0x404109:
    // 0x404109
    *v24 = *v24 | 128;
    int32_t v120 = *v28; // 0x404114
    int32_t v121; // 0x403c60
    if (v120 == 3) {
        // 0x404451
        *(int32_t *)(v4 + 96) = *(int32_t *)v2;
        *(int32_t *)(v4 + 100) = *(int32_t *)(v2 + 4);
        v121 = v2 + 8;
    } else {
        int32_t v122 = *(int32_t *)v2;
        int32_t v123 = v2 + 4;
        int32_t v124 = v4 + 96;
        int32_t * v125 = (int32_t *)v124;
        *v125 = v122;
        int32_t * v126 = (int32_t *)(v4 + 100);
        *v126 = v122 >> 31;
        v121 = v123;
        if (v120 != 2) {
            // 0x40412a
            v121 = v123;
            switch (*v28) {
                case 1: {
                    int32_t v127 = (int32_t)*(int16_t *)v124; // 0x404563
                    *v125 = v127;
                    *v126 = v127 >> 31;
                    v121 = v123;
                    // break -> 0x404153
                    break;
                }
                case 4: {
                    int32_t v128 = (int32_t)*(char *)v124; // 0x404594
                    *v125 = v128;
                    *v126 = v128 >> 31;
                    v121 = v123;
                    // break -> 0x404153
                    break;
                }
            }
        }
    }
    // 0x404153
    ___pformat_int();
    v10 = (char *)v9;
    v11 = v4;
    v12 = v121;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x40409c:
    // 0x40409c
    *v28 = 3;
    int32_t v40; // 0x403c60
    v42 = *(char *)v40;
    v43 = 4;
    v44 = v40;
    goto lab_0x403d3f;
  lab_0x40416d:
    // 0x40416d
    *v28 = 2;
    v42 = *(char *)v9;
    v43 = 4;
    v44 = v9;
    goto lab_0x403d3f;
  lab_0x403d4e:;
    int32_t v34; // 0x403c60
    char * v26; // 0x403d2c
    if (v34 > 3 || 0x1000000 * (int32_t)v36 > 0x39ffffff) {
        // 0x403d5c
        ___pformat_putc();
        v10 = v26;
        v11 = v4;
        v12 = v2;
        v13 = v21;
        goto lab_0x403d00;
    }
    int32_t v129; // 0x403c60
    if (v34 != 0) {
        // 0x404498
        v129 = v34;
        if (v34 == 2) {
            // 0x4044a1
            v129 = v34 & -256 | 3;
        }
    } else {
        // 0x4042ea
        v129 = v34 & -256 | 1;
    }
    int32_t v130 = v129;
    int32_t * v29; // 0x403d3b
    int32_t v131 = *v29; // 0x4042ec
    int32_t v41 = v130; // 0x4042f2
    if (v131 == 0) {
        goto lab_0x40430f;
    } else {
        int32_t v132 = v36; // 0x403d47
        int32_t * v133 = (int32_t *)v131; // 0x4042f8
        int32_t v134 = *v133; // 0x4042f8
        if (v134 < 0) {
            // 0x4044e1
            *v133 = v132 - 48;
            v42 = *(char *)v9;
            v43 = v130;
            v44 = v9;
            goto lab_0x403d3f;
        } else {
            // 0x404302
            *v133 = v132 - 48 + 10 * v134;
            v41 = v130;
            goto lab_0x40430f;
        }
    }
  lab_0x40430f:
    // 0x40430f
    v42 = *(char *)v9;
    v43 = v41;
    v44 = v9;
    goto lab_0x403d3f;
  lab_0x40427f:
    // 0x40427f
    v42 = *(char *)v9;
    v43 = 4;
    v44 = v9;
    goto lab_0x403d3f;
  lab_0x403d3f:;
    char v30 = v42; // 0x403d41
    int32_t v31 = v44; // 0x403d41
    int32_t v32 = v2; // 0x403d41
    int32_t v33 = v43; // 0x403d41
    int32_t v19 = v4; // 0x403d41
    if (v42 == 0) {
        // break (via goto) -> 0x403da0
        goto lab_0x403da0_3;
    }
    goto lab_0x403d43;
  lab_0x4043e1:
    // 0x4043e1
    *v28 = 2;
    char v52; // 0x404084
    v42 = v52;
    v43 = 4;
    v44 = v9;
    goto lab_0x403d3f;
  lab_0x403d00:;
    char v14 = *v10;
    int32_t v15 = v13; // 0x403d02
    int32_t v16 = v12; // 0x403d02
    int32_t v17 = v11; // 0x403d02
    char v18 = v14; // 0x403d02
    v19 = v11;
    if (v14 == 0) {
        // break -> 0x403da0
        goto lab_0x403da0_3;
    }
    goto lab_0x403d08;
  lab_0x403cae:;
    // 0x403cae
    int32_t v64; // bp-124, 0x403c60
    int32_t v65 = &v64; // 0x403c8f
    char v66 = *(char *)a4; // 0x403cf7
    v15 = a4;
    v16 = a5;
    v17 = v65;
    v18 = v66;
    v19 = v65;
    if (v66 == 0) {
      lab_0x403da0_3:
        // 0x403da0
        return *(int32_t *)(v19 + 52);
    }
    int32_t v51; // 0x403c60
    while (true) {
      lab_0x403d08:
        // 0x403d08
        v4 = v17;
        char v20 = v18; // 0x403d93
        v21 = v15 + 1;
        while (v20 != 37) {
            // 0x403d87
            ___pformat_putc();
            v20 = *(char *)v21;
            v19 = v4;
            if (v20 == 0) {
                // break (via goto) -> 0x403da0
                goto lab_0x403da0_3;
            }
            v21++;
        }
        int32_t v22 = v4 + 40; // 0x403d14
        int32_t * v23 = (int32_t *)v22; // 0x403d14
        *v23 = -1;
        v6 = v4 + 32;
        v24 = (int32_t *)v6;
        *v24 = *(int32_t *)(v4 + 20);
        int32_t v25 = v4 + 36; // 0x403d20
        *(int32_t *)v25 = -1;
        v26 = (char *)v21;
        char v27 = *v26; // 0x403d2c
        v28 = (int32_t *)(v4 + 12);
        *v28 = 0;
        v29 = (int32_t *)(v4 + 16);
        *v29 = v25;
        v30 = v27;
        v31 = v21;
        v32 = v16;
        v33 = 0;
        v19 = v4;
        if (v27 == 0) {
            // break -> 0x403da0
            break;
        }
        while (true) {
          lab_0x403d43:
            // 0x403d43
            v34 = v33;
            v2 = v32;
            int32_t v35 = v31;
            v36 = v30;
            v9 = v35 + 1;
            int32_t v37 = v36 - 32; // 0x403db0
            g33 = v37;
            v38 = v9;
            v39 = v37;
            v1 = v37;
            v40 = v9;
            switch (v36) {
                case 32: {
                    // 0x404316
                    v41 = v34;
                    if (v34 != 0) {
                        goto lab_0x40430f;
                    } else {
                        // 0x40431a
                        *v24 = *v24 | 64;
                        v42 = *(char *)v9;
                        v43 = v34;
                        v44 = v9;
                        goto lab_0x403d3f;
                    }
                }
                case 35: {
                    // 0x4042c3
                    v41 = v34;
                    if (v34 != 0) {
                        goto lab_0x40430f;
                    } else {
                        // 0x4042c7
                        *v24 = *v24 | 2048;
                        v42 = *(char *)v9;
                        v43 = v34;
                        v44 = v9;
                        goto lab_0x403d3f;
                    }
                }
                case 37: {
                    goto lab_0x404395;
                }
                case 42: {
                    // 0x404358
                    if (*v29 == 0) {
                        goto lab_0x40427f;
                    } else {
                        if ((v34 || 2) == 2) {
                            int32_t v45 = *(int32_t *)v2; // 0x40436f
                            int32_t v46; // 0x404358
                            *(int32_t *)v46 = v45;
                            if (v45 < 0) {
                                if (v34 != 0) {
                                    // 0x404622
                                    *v23 = -1;
                                } else {
                                    // 0x40460e
                                    int32_t * v47; // 0x403d20
                                    int32_t v48 = *v47; // 0x40460e
                                    *v47 = -v48;
                                    int32_t v49 = *v24; // 0x404612
                                    *v24 = v49 | 1024;
                                }
                            }
                            char v50 = *(char *)v9; // 0x404384
                            *v29 = 0;
                            v42 = v50;
                            v43 = v34;
                            v44 = v9;
                        } else {
                            // 0x4043fc
                            *v29 = 0;
                            v42 = *(char *)v9;
                            v43 = 4;
                            v44 = v9;
                        }
                        goto lab_0x403d3f;
                    }
                }
                case 43: {
                    // 0x404342
                    v41 = v34;
                    if (v34 != 0) {
                        goto lab_0x40430f;
                    } else {
                        // 0x404346
                        *v24 = *v24 | 256;
                        v42 = *(char *)v9;
                        v43 = v34;
                        v44 = v9;
                        goto lab_0x403d3f;
                    }
                }
                case 45: {
                    // 0x40432c
                    v41 = v34;
                    if (v34 != 0) {
                        goto lab_0x40430f;
                    } else {
                        // 0x404330
                        *v24 = *v24 | 1024;
                        v42 = *(char *)v9;
                        v43 = v34;
                        v44 = v9;
                        goto lab_0x403d3f;
                    }
                }
                case 46: {
                    if (v34 < 2) {
                        // 0x40447c
                        *v23 = 0;
                        *v29 = v22;
                        v42 = *(char *)v9;
                        v43 = 2;
                        v44 = v9;
                        goto lab_0x403d3f;
                    } else {
                        goto lab_0x40427f;
                    }
                }
                case 48: {
                    if (v34 != 0) {
                        goto lab_0x403d4e;
                    } else {
                        // 0x404264
                        *v24 = *v24 | 512;
                        v42 = *(char *)v9;
                        v43 = v34;
                        v44 = v9;
                        goto lab_0x403d3f;
                    }
                }
                case 65: {
                    // 0x403e99
                    v51 = *v24;
                    goto lab_0x403e99_2;
                }
                case 67: {
                    // 0x404222
                    *v23 = -1;
                    goto lab_0x40422a;
                }
                case 69: {
                    goto lab_0x403e62;
                }
                case 70: {
                    goto lab_0x403e2b;
                }
                case 71: {
                    goto lab_0x403df4;
                }
                case 73: {
                    // 0x404084
                    v52 = *(char *)v9;
                    if (v52 != 54) {
                        if (v52 == 51) {
                            // 0x4044f7
                            if (*(char *)(v35 + 2) != 50) {
                                goto lab_0x4043e1;
                            } else {
                                int32_t v53 = v35 + 3; // 0x404501
                                *v28 = 2;
                                v42 = *(char *)v53;
                                v43 = 4;
                                v44 = v53;
                                goto lab_0x403d3f;
                            }
                        } else {
                            goto lab_0x4043e1;
                        }
                    } else {
                        // 0x40408f
                        if (*(char *)(v35 + 2) != 52) {
                            goto lab_0x4043e1;
                        } else {
                            // 0x404099
                            v40 = v35 + 3;
                            goto lab_0x40409c;
                        }
                    }
                }
                case 76: {
                    goto lab_0x403dd2;
                }
                case 83: {
                    goto lab_0x404056;
                }
                case 88: {
                    goto lab_0x404181;
                }
                case 97: {
                    int32_t v54 = *v24 | 32; // 0x403e92
                    *v24 = v54;
                    v51 = v54;
                    goto lab_0x403e99_2;
                }
                case 99: {
                    // 0x4041ea
                    *v23 = -1;
                    if ((*v28 || 1) == 3) {
                        goto lab_0x40422a;
                    } else {
                        // 0x404200
                        *(char *)(v4 + 96) = (char)*(int32_t *)v2;
                        ___pformat_putchars();
                        v10 = (char *)v9;
                        v11 = v4;
                        v12 = v2 + 4;
                        v13 = v9;
                        goto lab_0x403d00;
                    }
                }
                case 100: {
                    goto lab_0x404109;
                }
                case 101: {
                    int32_t v55 = *v24 | 32; // 0x403e5b
                    *v24 = v55;
                    v1 = v55;
                    goto lab_0x403e62;
                }
                case 102: {
                    int32_t v56 = *v24 | 32; // 0x403e24
                    *v24 = v56;
                    v39 = v56;
                    goto lab_0x403e2b;
                }
                case 103: {
                    // 0x403de9
                    *v24 = *v24 | 32;
                    goto lab_0x403df4;
                }
                case 104: {
                    char v57 = *(char *)v9; // 0x4040b0
                    if (v57 == 104) {
                        int32_t v58 = v35 + 2; // 0x404467
                        *v28 = 4;
                        v42 = *(char *)v58;
                        v43 = 4;
                        v44 = v58;
                    } else {
                        // 0x4040bb
                        *v28 = 1;
                        v42 = v57;
                        v43 = 4;
                        v44 = v9;
                    }
                    goto lab_0x403d3f;
                }
                case 105: {
                    goto lab_0x404109;
                }
                case 106: {
                    goto lab_0x40409c;
                }
                case 108: {
                    // 0x403dbc
                    *v28 = 2;
                    v38 = v9;
                    if (*(char *)v9 == 108) {
                        // 0x403dc9
                        *v28 = 3;
                        v38 = v35 + 2;
                    }
                    goto lab_0x403dd2;
                }
                case 110: {
                    // 0x4040cd
                    switch (*v28) {
                        case 4: {
                            int32_t v59 = *(int32_t *)(v4 + 52); // 0x404441
                            *(char *)*(int32_t *)v2 = (char)v59;
                            v10 = (char *)v9;
                            v11 = v4;
                            v12 = v2 + 4;
                            v13 = v9;
                            // break -> 0x403d00
                            break;
                        }
                        case 1: {
                            int32_t v60 = *(int32_t *)(v4 + 52); // 0x404530
                            *(int16_t *)*(int32_t *)v2 = (int16_t)v60;
                            v10 = (char *)v9;
                            v11 = v4;
                            v12 = v2 + 4;
                            v13 = v9;
                            // break -> 0x403d00
                            break;
                        }
                        case 3: {
                            int32_t v61 = *(int32_t *)v2; // 0x4045d9
                            int32_t v62 = *(int32_t *)(v4 + 52); // 0x4045db
                            *(int32_t *)v61 = v62;
                            *(int32_t *)(v61 + 4) = v62 >> 31;
                            v10 = (char *)v9;
                            v11 = v4;
                            v12 = v2 + 4;
                            v13 = v9;
                            // break -> 0x403d00
                            break;
                        }
                        default: {
                            // 0x4040f7
                            *(int32_t *)*(int32_t *)v2 = *(int32_t *)(v4 + 52);
                            v10 = (char *)v9;
                            v11 = v4;
                            v12 = v2 + 4;
                            v13 = v9;
                            // break -> 0x403d00
                            break;
                        }
                    }
                    goto lab_0x403d00;
                }
                case 111: {
                    goto lab_0x404181;
                }
                case 112: {
                    // 0x40428b
                    *v24 = *v24 | 2048;
                    *(int32_t *)(v4 + 100) = 0;
                    *(int32_t *)(v4 + 96) = *(int32_t *)v2;
                    *(int32_t *)(v4 - 16) = v4 + 28;
                    ___pformat_xint((int32_t)&g36);
                    v10 = (char *)v9;
                    v11 = v4;
                    v12 = v2 + 4;
                    v13 = v9;
                    goto lab_0x403d00;
                }
                case 115: {
                    // 0x404046
                    if ((*v28 || 1) == 3) {
                        goto lab_0x404056;
                    } else {
                        int32_t v63 = *(int32_t *)v2; // 0x4043aa
                        *(int32_t *)(v4 - 16) = v63 == 0 ? (int32_t)"(null)" : v63;
                        strlen((char *)&g36);
                        ___pformat_putchars();
                        v10 = (char *)v9;
                        v11 = v4;
                        v12 = v2 + 4;
                        v13 = v9;
                        goto lab_0x403d00;
                    }
                }
                case 116: {
                    goto lab_0x40416d;
                }
                case 117: {
                    goto lab_0x404181;
                }
                case 120: {
                    goto lab_0x404181;
                }
                case 122: {
                    goto lab_0x40416d;
                }
                default: {
                    goto lab_0x403d4e;
                }
            }
        }
      lab_0x404395:
        // 0x404395
        ___pformat_putc();
        v10 = (char *)v9;
        v11 = v4;
        v12 = v2;
        v13 = v9;
        goto lab_0x403d00;
    }
  lab_0x403da0_3:
    // 0x403da0
    return *(int32_t *)(v19 + 52);
  lab_0x403e99_2:;
    int32_t v67 = v51; // 0x403e99
    int32_t * v68; // 0x403c60
    int32_t * v69; // 0x403c60
    int32_t * v70; // 0x403c60
    int32_t * v71; // 0x403c60
    int32_t v72; // 0x403c60
    int32_t v73; // 0x403eaa
    int32_t * v74; // 0x403ec8
    int16_t v75; // 0x403eff
    int16_t * v76; // 0x403eff
    if ((v67 & 4) == 0) {
        float64_t v77 = *(float64_t *)v2; // 0x403f46
        int32_t v78 = v2 + 8; // 0x403f48
        int32_t v79 = v4 + 64; // 0x403f4b
        *(float64_t *)v79 = v77;
        int32_t v80 = __asm_fxam((float80_t)v77); // 0x403f4f
        __asm_wait();
        v72 = v78;
        if ((v80 & 0x4500) == 256) {
            goto lab_0x404410;
        } else {
            int16_t * v81 = (int16_t *)(v4 + 70); // 0x403f64
            uint16_t v82 = *v81; // 0x403f64
            if (v82 <= 0xffff) {
                // 0x403f73
                *v24 = v67 | 128;
            }
            int32_t v83 = v4 - 16; // 0x403f7a
            *(float64_t *)v83 = v77;
            if (___fpclassify((int32_t)&g36) == 1280) {
                // 0x4045f3
                ___pformat_emit_inf_or_nan();
                v10 = (char *)v9;
                v11 = v4;
                v12 = v78;
                v13 = v9;
            } else {
                int32_t * v84 = (int32_t *)v79; // 0x403f93
                uint32_t v85 = *v84; // 0x403f93
                int32_t * v86 = (int32_t *)(v4 + 68); // 0x403f97
                *v86 = 256 * *v86 | v85 / 0x1000000;
                int32_t v87 = 256 * v85; // 0x403fa9
                uint16_t v88 = v82 / 16 % 2048;
                *v81 = *v81 % 0x1000;
                int32_t v89 = v4 + 72; // 0x403fb9
                int16_t * v90 = (int16_t *)v89; // 0x403fb9
                *v90 = v88;
                *v84 = v87;
                if (v88 != 0) {
                    uint16_t v91 = v88 - 1023; // 0x4044b2
                    *v90 = v91;
                    if (v91 >= 0xff83) {
                        // 0x4044bd
                        *v81 = *v81 + 0x1000;
                    }
                } else {
                    // 0x403fcb
                    if ((*v86 || v87) != 0) {
                        // 0x403fd5
                        *v90 = -1023;
                    }
                }
                // 0x403fdc
                *(int32_t *)v83 = v4 + 28;
                *(int32_t *)(v4 - 20) = *(int32_t *)(v4 + 76);
                *(int32_t *)(v4 - 24) = *(int32_t *)v89;
                *(int32_t *)(v4 - 28) = *v86;
                *(int32_t *)(v4 - 32) = *v84;
                ___pformat_emit_xfloat((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                v10 = (char *)v9;
                v11 = v4;
                v12 = v78;
                v13 = v9;
            }
            goto lab_0x403d00;
        }
    } else {
        float80_t v92 = *(float80_t *)v2; // 0x403ea8
        v73 = v2 + 12;
        int32_t v93 = v4 + 80; // 0x403eaf
        *(float80_t *)v93 = v92;
        int32_t v94 = __asm_fxam(v92); // 0x403eb3
        __asm_wait();
        v72 = v73;
        if ((v94 & 0x4500) == 256) {
            goto lab_0x404410;
        } else {
            int32_t v95 = v4 + 88; // 0x403ec8
            v74 = (int32_t *)v95;
            uint32_t v96 = *v74; // 0x403ec8
            if ((int16_t)v96 <= 0xffff) {
                // 0x403ed6
                *v24 = v67 | 128;
            }
            int32_t v97 = __asm_fxam(v92); // 0x403edd
            __asm_wait();
            if ((v97 & 0x4500) == 1280) {
                // 0x4045c2
                ___pformat_emit_inf_or_nan();
                v10 = (char *)v9;
                v11 = v4;
                v12 = v73;
                v13 = v9;
                goto lab_0x403d00;
            } else {
                uint32_t v98 = v96 % 0x8000; // 0x403ef6
                v75 = v98;
                v76 = (int16_t *)v95;
                *v76 = v75;
                if (v98 != 0) {
                    // 0x403ef4
                    v69 = (int32_t *)v93;
                    v68 = (int32_t *)(v4 + 84);
                    goto lab_0x403f10;
                } else {
                    int32_t * v99 = (int32_t *)v93;
                    int32_t * v100 = (int32_t *)(v4 + 84);
                    v69 = v99;
                    v68 = v100;
                    v71 = v99;
                    v70 = v100;
                    if ((*v100 | *v99) == 0) {
                        goto lab_0x403f1b;
                    } else {
                        goto lab_0x403f10;
                    }
                }
            }
        }
    }
  lab_0x404410:
    // 0x404410
    ___pformat_emit_inf_or_nan();
    v10 = (char *)v9;
    v11 = v4;
    v12 = v72;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x403f10:
    // 0x403f10
    *v76 = v75 - 0x3fff;
    v71 = v69;
    v70 = v68;
    goto lab_0x403f1b;
  lab_0x403f1b:
    // 0x403f1b
    *(int32_t *)(v4 - 16) = v4 + 28;
    *(int32_t *)(v4 - 20) = *(int32_t *)(v4 + 92);
    *(int32_t *)(v4 - 24) = *v74;
    *(int32_t *)(v4 - 28) = *v70;
    *(int32_t *)(v4 - 32) = *v71;
    ___pformat_emit_xfloat((int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
    v10 = (char *)v9;
    v11 = v4;
    v12 = v73;
    v13 = v9;
    goto lab_0x403d00;
  lab_0x40422a:
    // 0x40422a
    *(int32_t *)(v4 + 96) = *(int32_t *)v2 % 0x10000;
    *(int32_t *)(v4 + 100) = 0;
    ___pformat_wputchars();
    v10 = (char *)v9;
    v11 = v4;
    v12 = v2 + 4;
    v13 = v9;
    goto lab_0x403d00;
}

// Address range: 0x404630 - 0x405ac8
int32_t ___gdtoa(int32_t * a1, int32_t a2, int32_t * a3, int32_t * a4, int32_t a5, int32_t a6, int32_t a7, int32_t * a8) {
    // 0x404630
    int32_t v1; // bp-156, 0x404630
    int32_t v2 = &v1; // 0x404634
    uint32_t v3 = *a4; // 0x404649
    int32_t v4 = v3 & -49; // 0x40464b
    *a4 = v4;
    int32_t v5 = v3 % 8; // 0x404652
    g34 = v5;
    int32_t * v6; // 0x404630
    int32_t v7; // 0x404630
    int32_t v8; // 0x404630
    switch (v5) {
        case 0: {
            // 0x404630
            v6 = (int32_t *)(v2 - 16);
            goto lab_0x404dec;
        }
        case 1: {
            goto lab_0x404d1f;
        }
        case 2: {
            goto lab_0x404d1f;
        }
        case 3: {
            // 0x404e5c
            *(int32_t *)v8 = -0x8000;
            return ___nrv_alloc_D2A((int32_t)"Infinity", v7, 8);
        }
        case 4: {
            // 0x404e24
            *(int32_t *)v8 = -0x8000;
            return ___nrv_alloc_D2A((int32_t)"NaN", v7, 3);
        }
        default: {
            // 0x404d10
            return *(int32_t *)(v2 + 88);
        }
    }
  lab_0x404d1f:;
    uint32_t v9 = *a1; // 0x404d28
    if (v9 >= 33) {
        int32_t v10; // 0x404630
        while (v9 > 2 * v10) {
            // 0x404d34
            int32_t v11; // 0x404630
            int32_t v12 = v11;
            int32_t v13 = v10;
            int32_t v14 = 2 * v13; // 0x404d34
            int32_t v15 = v12 + 1; // 0x404d36
            v10 = v14;
            v11 = v15;
        }
    }
    int32_t v16 = (int32_t)a3;
    int32_t v17 = ___Balloc_D2A(0); // 0x404d3f
    int32_t v18 = v17 + 20; // 0x404d59
    int32_t v19 = 4 * (v9 - 1) / 32 + v16; // 0x404d61
    int32_t v20 = v16 + 4; // 0x404d66
    *(int32_t *)v18 = *(int32_t *)v16;
    int32_t v21 = v18 + 4; // 0x404d6b
    int32_t v22 = v21; // 0x404d70
    int32_t v23 = v20; // 0x404d70
    while (v19 >= v20) {
        // 0x404d64
        v20 = v23 + 4;
        *(int32_t *)v22 = *(int32_t *)v23;
        v21 = v22 + 4;
        v22 = v21;
        v23 = v20;
    }
    int32_t v24 = v21 - v18; // 0x404d78
    int32_t v25 = v24 / 4;
    int32_t v26 = (v24 & -4) + v18 - 4; // 0x404d81
    while (*(int32_t *)v26 == 0) {
        int32_t v27 = v25 - 1; // 0x404d84
        if (v27 == 0) {
            // 0x404d94
            *(int32_t *)(v17 + 16) = 0;
            goto lab_0x404da7;
        }
        v25 = v27;
        v26 -= 4;
    }
    // 0x404e94
    *(int32_t *)(v17 + 16) = v25;
    goto lab_0x404da7;
  lab_0x404dec:
    // 0x404dec
    *(int32_t *)*(int32_t *)(v2 + 184) = 1;
    *(int32_t *)(v2 - 4) = v4;
    *(int32_t *)(v2 - 8) = 1;
    *(int32_t *)(v2 - 12) = *(int32_t *)(v2 + 188);
    *v6 = (int32_t)&g9;
    int32_t result = ___nrv_alloc_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x404e09
    *(int32_t *)(v2 + 88) = result;
    return result;
  lab_0x404da7:;
    int32_t v28 = ___trailz_D2A(v17); // 0x404daf
    int32_t v29 = a2; // 0x404dcb
    int32_t v30 = v19; // 0x404dcb
    if (v28 != 0) {
        // 0x404ebc
        ___rshift_D2A(v17, v28, v18, v18);
        v29 = v28 + a2;
        v30 = v17;
    }
    int32_t v31 = v29;
    int32_t * v32 = (int32_t *)(v2 + 64); // 0x404dd1
    int32_t v33 = *v32; // 0x404dd1
    char v34; // 0x404630
    int32_t v35; // 0x404630
    int32_t v36; // 0x404630
    int32_t v37; // 0x404630
    int32_t * v38; // 0x404665
    int32_t * v39; // 0x404666
    int32_t * v40; // 0x40466e
    int32_t * v41; // 0x404673
    int32_t * v42; // 0x404679
    int32_t * v43; // 0x40467d
    float64_t * v44; // 0x404681
    int32_t * v45; // 0x4046a1
    int16_t * v46; // 0x4046fc
    int16_t * v47; // 0x404703
    int32_t * v48; // 0x40470c
    int32_t v49; // 0x404630
    int32_t * v50; // 0x404727
    int32_t * v51; // 0x4047a3
    if (*(int32_t *)(v33 + 16) != 0) {
        // 0x404665
        v38 = (int32_t *)(v2 - 4);
        *v38 = v30;
        v39 = (int32_t *)(v2 - 8);
        *v39 = v30;
        int32_t v52 = v2 + 132; // 0x404667
        v40 = (int32_t *)(v2 - 12);
        *v40 = v52;
        v41 = (int32_t *)(v2 - 16);
        *v41 = *v32;
        ___b2d_D2A(v31, (int32_t)&g36);
        v42 = (int32_t *)(v2 + 92);
        v43 = (int32_t *)(v2 + 16);
        v44 = (float64_t *)(v2 + 120);
        float80_t v53; // 0x404630
        *v44 = (float64_t)v53;
        int32_t * v54 = (int32_t *)(v2 + 124); // 0x404688
        int32_t v55 = *v42 - 1 + *v43; // 0x40468f
        int32_t v56 = *v54 % 0x100000 | 0x3ff00000; // 0x40469b
        v45 = (int32_t *)v52;
        *v45 = v55;
        *v54 = v56;
        float64_t v57 = *v44; // 0x4046b2
        *(int32_t *)(v2 - 20) = v55;
        int32_t v58 = (v55 < 0 ? -v55 : v55) - 1077; // 0x4046cc
        float80_t v59 = 0.301029995663980975973L * (float80_t)v55 + 0.289529654602168007305L * ((float80_t)v57 - 1.5L) + 0.176091259055800003486L; // 0x4046e5
        float80_t v60 = v59; // 0x4046e7
        if (v58 >= 1) {
            // 0x4046e9
            *v38 = v58;
            v60 = 7.0e-17L * (float80_t)v58 + v59;
        }
        // 0x4046f8
        v46 = (int16_t *)(v2 + 102);
        v47 = (int16_t *)(v2 + 100);
        *v47 = *v46 % 256 | 3072;
        v48 = (int32_t *)(v2 + 40);
        v49 = v37 & 0x4500;
        *v48 = (int32_t)v60 + (int32_t)(v49 == 0);
        v50 = (int32_t *)(v2 + 44);
        *v50 = 1;
        *v54 = v56 + 0x100000 * v55;
        uint32_t v61 = *v48; // 0x404738
        if (v61 < 23) {
            // 0x40473f
            *v50 = 0;
            if (v49 == 0) {
                // 0x40475f
                *v48 = v61 - 1;
            }
        }
        int32_t v62 = *v43 + -1 - v55; // 0x40476a
        if (v62 < 0) {
            // 0x4051ff
            *(int32_t *)(v2 + 20) = -v62;
            *(int32_t *)(v2 + 56) = 0;
        } else {
            // 0x404771
            *(int32_t *)(v2 + 56) = v62;
            *(int32_t *)(v2 + 20) = 0;
        }
        int32_t v63 = *v48; // 0x40477d
        if (v63 < 0) {
            int32_t * v64 = (int32_t *)(v2 + 20); // 0x4051e2
            *(int32_t *)(v2 + 60) = 0;
            *v64 = *v64 - v63;
            *(int32_t *)(v2 + 24) = -v63;
        } else {
            int32_t * v65 = (int32_t *)(v2 + 56); // 0x40478d
            *(int32_t *)(v2 + 60) = v63;
            *v65 = *v65 + v63;
            *(int32_t *)(v2 + 24) = 0;
        }
        // 0x4047a3
        v51 = (int32_t *)(v2 + 176);
        uint32_t v66 = *v51; // 0x4047a3
        if (v66 < 10) {
            int32_t v67 = 1; // 0x404ef7
            if (v66 > 5) {
                // 0x404fe1
                *v51 = v66 - 4;
                v67 = 0;
            }
            // 0x4050d2
            g35 = v2;
            v36 = v67;
            switch (v2) {
                case 0: {
                    goto lab_0x4047c1;
                }
                case 4: {
                    // 0x405120
                    *(int32_t *)52 = 1;
                    int32_t v68 = *(int32_t *)184; // 0x405128
                    int32_t v69 = v68; // 0x405131
                    if (v68 < 1) {
                        // 0x4058cd
                        *(int32_t *)184 = 1;
                        v69 = 1;
                    }
                    // 0x405137
                    *v45 = v69;
                    *(int32_t *)36 = v69;
                    *(int32_t *)40 = v69;
                    v34 = v69 < 15;
                    v35 = v67;
                    goto lab_0x40480d;
                }
                default: {
                    // 0x404f15
                    *(int32_t *)(v2 + 32) = 0;
                    *(int32_t *)(v2 + 36) = 0;
                    *(int32_t *)(v2 + 48) = 1;
                    v34 = 1;
                    v35 = v67;
                    goto lab_0x40480d;
                }
            }
        } else {
            // 0x4047b1
            *v51 = 0;
            v36 = 1;
            goto lab_0x4047c1;
        }
    } else {
        int32_t * v70 = (int32_t *)(v2 - 16);
        *v70 = v33;
        ___Bfree_D2A(v31);
        v6 = v70;
        goto lab_0x404dec;
    }
  lab_0x4047c1:
    // 0x4047c1
    *v38 = v9;
    *(int32_t *)(v2 + 180) = 0;
    *(int32_t *)(v2 + 32) = -1;
    int32_t * v71 = (int32_t *)(v2 + 96); // 0x4047e7
    *v71 = (int32_t)(0.30103L * (float80_t)v9);
    *(int32_t *)(v2 + 36) = -1;
    *(int32_t *)(v2 + 48) = 1;
    *v45 = *v71 + 3;
    v34 = 0;
    v35 = v36;
    goto lab_0x40480d;
  lab_0x40480d:
    // 0x40480d
    *v41 = *v45;
    int32_t v72 = ___rv_alloc_D2A((int32_t)&g36); // 0x404818
    int32_t * v73 = (int32_t *)(v2 + 160); // 0x40481d
    int32_t v74 = *v73; // 0x40481d
    int32_t * v75 = (int32_t *)(v2 + 88);
    *v75 = v72;
    int32_t v76 = *(int32_t *)(v74 + 12) - 1; // 0x40482b
    int32_t * v77 = (int32_t *)(v2 + 52); // 0x40482c
    *v77 = v76;
    int32_t v78 = 0; // 0x404836
    int32_t v79 = v74; // 0x404836
    if (v76 != 0) {
        int32_t v80 = v76 >= 0 ? v76 : 2; // 0x404847
        int32_t v81 = (v3 & 8) == 0 ? v80 : 3 - v80;
        int32_t v82 = (v3 & 8) == 0 ? v74 : v80;
        v78 = v81;
        v79 = v82;
        if ((v3 & 8) == 0 != v76 >= 0) {
            *v77 = v81;
            v78 = v81;
            v79 = v82;
        }
    }
    int32_t v83 = v79; // 0x404858
    int32_t v84; // 0x404630
    int32_t v85; // 0x404630
    int32_t v86; // 0x404630
    int32_t v87; // 0x404630
    float64_t * v88; // 0x404889
    if (v34 == 0 || (char)v35 == 0) {
        goto lab_0x4049c7;
    } else {
        int32_t v89 = *v48; // 0x40486c
        int32_t v90 = v89 | v78; // 0x40486c
        v83 = v90;
        if (v90 != 0) {
            goto lab_0x4049c7;
        } else {
            float64_t v91 = *v44; // 0x404876
            float80_t v92 = v91; // 0x404876
            *v45 = 0;
            int32_t v93 = v2 + 80; // 0x404889
            v88 = (float64_t *)v93;
            *v88 = v91;
            int32_t v94 = *v48;
            int32_t v95; // 0x404630
            int32_t v96; // 0x404630
            if (v89 < 1) {
                int32_t v97 = -v94; // 0x40570e
                v96 = v97;
                v95 = 2;
                if (v94 != 0) {
                    float64_t v98 = *(float64_t *)((8 * v97 & 120) + (int32_t)&g12); // 0x40594f
                    float64_t v99 = v92 * (float80_t)v98; // 0x40595b
                    *v44 = v99;
                    v96 = v97;
                    v95 = 2;
                    if (-v94 >= 16) {
                        int32_t v100 = 2;
                        int32_t v101 = (int32_t)&g10;
                        int32_t v102 = v97 / 16;
                        float64_t v103 = v99; // 0x405975
                        int32_t v104 = v100; // 0x405975
                        float64_t v105; // 0x40597b
                        if (v102 % 2 != 0) {
                            // 0x405977
                            v105 = *(float64_t *)v101;
                            v103 = (float80_t)v99 * (float80_t)v105;
                            *v44 = v103;
                            v104 = v100 + 1;
                        }
                        int32_t v106 = v104;
                        int32_t v107 = 1; // 0x405982
                        int32_t v108 = v101 + 8; // 0x405983
                        int32_t v109 = v102 / 2; // 0x405988
                        int32_t v110 = v107; // 0x405988
                        while (v102 >= 2) {
                            // 0x405973
                            v100 = v106;
                            v101 = v108;
                            v102 = v109;
                            float64_t v111 = v103; // 0x405977
                            v103 = v111;
                            v104 = v100;
                            if (v102 % 2 != 0) {
                                // 0x405977
                                v105 = *(float64_t *)v101;
                                v103 = (float80_t)v111 * (float80_t)v105;
                                *v44 = v103;
                                v104 = v100 + 1;
                            }
                            // 0x405982
                            v106 = v104;
                            v107 = v110 + 1;
                            v108 = v101 + 8;
                            v109 = v102 / 2;
                            v110 = v107;
                        }
                        // 0x40598a
                        *v45 = v107;
                        v96 = v108;
                        v95 = v106;
                    }
                }
            } else {
                int32_t v112 = v94 / 16; // 0x4048b4
                int32_t v113; // 0x404630
                int32_t v114; // 0x404630
                int32_t v115; // 0x404630
                int32_t v116; // 0x404630
                int32_t v117; // 0x404630
                int32_t v118; // 0x404630
                if ((v94 & 256) == 0) {
                    // 0x40489d
                    v114 = v2 + 12;
                    v113 = v2 + 8;
                    v115 = v112;
                    v116 = v93;
                    v118 = v2 + 84;
                    v117 = 2;
                } else {
                    float64_t v119 = v92 / 1.0e+256L; // 0x4048ce
                    *v44 = v119;
                    int32_t v120 = v2 + 8;
                    *(float64_t *)v120 = v119;
                    int32_t v121 = v2 + 12;
                    v114 = v121;
                    v113 = v120;
                    v115 = v112 % 16;
                    v116 = v120;
                    v118 = v121;
                    v117 = 3;
                }
                int32_t v122 = *(int32_t *)v118;
                int32_t v123 = *(int32_t *)v116;
                int32_t * v124 = (int32_t *)v113; // 0x4048de
                *v124 = v123;
                int32_t * v125 = (int32_t *)v114; // 0x4048e2
                *v125 = v122;
                v96 = v90;
                v95 = v117;
                if (v115 != 0) {
                    int32_t v126 = v115;
                    int32_t v127 = v117 + (int32_t)(v126 % 2 != 0);
                    int32_t v128 = 1; // 0x4048fe
                    int32_t v129 = (int32_t)&g10 + 8; // 0x4048ff
                    int32_t v130 = v126 / 2; // 0x404904
                    int32_t v131 = v128; // 0x404904
                    int32_t v132 = v129; // 0x404904
                    int32_t v133 = v127; // 0x404904
                    while (v126 >= 2) {
                        // 0x4048f7
                        v126 = v130;
                        v127 = v133 + (int32_t)(v126 % 2 != 0);
                        v128 = v131 + 1;
                        v129 = v132 + 8;
                        v130 = v126 / 2;
                        v131 = v128;
                        v132 = v129;
                        v133 = v127;
                    }
                    // 0x404906
                    *v124 = v123;
                    *v125 = v122;
                    *v45 = v128;
                    v96 = v129;
                    v95 = v127;
                }
            }
            int32_t v134 = *(int32_t *)(v2 + 32);
            if (v49 != 0 | *v50 == 0 || v134 < 1) {
                // 0x40527d
                v85 = v134;
                v87 = *v48;
                v84 = v95;
                goto lab_0x404964;
            } else {
                int32_t v135 = *(int32_t *)(v2 + 36); // 0x404944
                v86 = v96;
                if (v135 < 1) {
                    goto lab_0x4049bf;
                } else {
                    // 0x404950
                    *v44 = 10.0;
                    v85 = v135;
                    v87 = *v48 - 1;
                    v84 = v95 + 1;
                    goto lab_0x404964;
                }
            }
        }
    }
  lab_0x4049c7:
    // 0x4049c7
    if (*v42 < 0) {
        goto lab_0x4049da;
    } else {
        // 0x4049cf
        if (*v48 < 15) {
            // 0x404f34
            if (*(int32_t *)(v2 + 180) < 0) {
                int32_t v136 = *(int32_t *)(v2 + 32); // 0x4055c2
                if (v136 >= 0 == (v136 != 0)) {
                    goto lab_0x404f4e;
                } else {
                    if ((v37 & 1280) == 0 || v136 < 0) {
                        // 0x40528c
                        *(int32_t *)(v2 + 72) = 0;
                        *(int32_t *)(v2 + 76) = 0;
                        goto lab_0x40529c;
                    } else {
                        // 0x4055ec
                        *(int32_t *)(v2 + 72) = 0;
                        *(int32_t *)(v2 + 76) = 0;
                        goto lab_0x40559d;
                    }
                }
            } else {
                goto lab_0x404f4e;
            }
        } else {
            goto lab_0x4049da;
        }
    }
  lab_0x4049da:;
    int32_t * v137 = (int32_t *)(v2 + 48); // 0x4049da
    int32_t v138; // 0x404630
    int32_t v139; // 0x404630
    int32_t v140; // 0x404630
    int32_t v141; // 0x404630
    if (*v137 != 0) {
        // 0x405212
        int32_t * v142; // 0x404630
        int32_t v143; // 0x404630
        int32_t v144; // 0x404630
        int32_t v145; // 0x404630
        if (*v51 < 2) {
            int32_t v146 = v9 - *v43; // 0x405776
            int32_t v147 = *(int32_t *)(*v73 + 4); // 0x40577f
            int32_t v148 = v146 + 1; // 0x405782
            *v45 = v148;
            int32_t v149 = *v42; // 0x40578c
            int32_t v150 = v148; // 0x405794
            if (v149 - v146 < v147) {
                int32_t v151 = v149 - v147; // 0x40579a
                *v42 = v151;
                v150 = v151 + 1;
                *v45 = v150;
            }
            int32_t * v152 = (int32_t *)(v2 + 20);
            v142 = v152;
            v143 = v150;
            v145 = *(int32_t *)(v2 + 24);
            v144 = *v152;
        } else {
            int32_t * v153 = (int32_t *)(v2 + 32); // 0x405220
            int32_t v154 = *v153; // 0x405220
            int32_t v155 = v154 - 1; // 0x405224
            int32_t * v156 = (int32_t *)(v2 + 24); // 0x405225
            int32_t v157 = *v156; // 0x405225
            int32_t v158; // 0x404630
            int32_t v159; // 0x404630
            if (v157 < v155) {
                int32_t * v160 = (int32_t *)(v2 + 60); // 0x405602
                *v160 = v155 - v157 + *v160;
                *v156 = v155;
                v158 = *v153;
                v159 = 0;
            } else {
                // 0x40522f
                v158 = v154;
                v159 = v157 - v155;
            }
            int32_t * v161 = (int32_t *)(v2 + 20);
            int32_t v162 = *v161; // 0x405239
            *v45 = v158;
            v142 = v161;
            v143 = v158;
            v145 = v159;
            v144 = v162;
            if (v158 < 0) {
                // 0x4058e8
                *v45 = 0;
                v142 = v161;
                v143 = 0;
                v145 = v159;
                v144 = v162 - v158;
            }
        }
        int32_t * v163 = (int32_t *)(v2 + 56); // 0x405257
        int32_t v164 = *v142 + v143; // 0x40525b
        int32_t v165 = *v163 + v143; // 0x40525d
        *v142 = v164;
        *v163 = v165;
        *v41 = 1;
        *(int32_t *)(v2 + 72) = ___i2b_D2A((int32_t)&g36);
        v140 = v164;
        v138 = v165;
        v141 = v145;
        v139 = v144;
    } else {
        // 0x4049e6
        *(int32_t *)(v2 + 72) = 0;
        v140 = v83;
        v138 = v9;
        v141 = *(int32_t *)(v2 + 24);
        v139 = *(int32_t *)(v2 + 20);
    }
    int32_t v166 = v139;
    int32_t v167 = v140; // 0x4049f8
    int32_t v168 = v166; // 0x4049f8
    if (v166 >= 1) {
        int32_t * v169 = (int32_t *)(v2 + 56); // 0x4049fa
        uint32_t v170 = *v169; // 0x4049fa
        v167 = v140;
        v168 = v166;
        if (v170 >= 1) {
            int32_t v171 = v170 > v166 ? v166 : v170;
            int32_t * v172 = (int32_t *)(v2 + 20); // 0x404a0c
            int32_t v173 = v170 - v171; // 0x404a16
            *v45 = v171;
            *v172 = *v172 - v171;
            *v169 = v173;
            v167 = v173;
            v168 = v166 - v171;
        }
    }
    int32_t * v174 = (int32_t *)(v2 + 24); // 0x404a29
    uint32_t v175 = *v174; // 0x404a29
    int32_t v176 = v167; // 0x404a2f
    int32_t v177 = v141; // 0x404a2f
    if (v175 >= 1) {
        int32_t v178 = *v137; // 0x404a31
        if (v178 == 0) {
            // 0x40553b
            *v38 = v138;
            *v39 = v138;
            int32_t v179 = *v174; // 0x40553d
            *v40 = v179;
            *v41 = *v32;
            *v32 = ___pow5mult_D2A((int32_t)&g36, (int32_t)&g36);
            v176 = v167;
            v177 = v179;
        } else {
            int32_t v180 = v175; // 0x404a3f
            int32_t v181 = v167; // 0x404a3f
            if (v141 >= 1) {
                // 0x404a41
                *v38 = v178;
                *v39 = v178;
                *v40 = v141;
                int32_t * v182 = (int32_t *)(v2 + 72); // 0x404a44
                *v41 = *v182;
                int32_t v183 = ___pow5mult_D2A((int32_t)&g36, (int32_t)&g36); // 0x404a49
                *v182 = v183;
                v181 = *v32;
                *v40 = v181;
                *v41 = v183;
                int32_t v184 = ___mult_D2A((int32_t)&g36, (int32_t)&g36); // 0x404a5a
                *v41 = *v32;
                ___Bfree_D2A((int32_t)&g36);
                *v32 = v184;
                v180 = *v174;
            }
            int32_t v185 = v180 - v141; // 0x404a77
            v176 = v181;
            v177 = v141;
            if (v185 != 0) {
                // 0x40561f
                *v38 = v181;
                *v39 = v181;
                *v40 = v185;
                *v41 = *v32;
                *v32 = ___pow5mult_D2A((int32_t)&g36, (int32_t)&g36);
                v176 = v181;
                v177 = v141;
            }
        }
    }
    // 0x404a7f
    *v41 = 1;
    int32_t v186 = ___i2b_D2A((int32_t)&g36); // 0x404a84
    int32_t * v187 = (int32_t *)(v2 + 76); // 0x404a89
    *v187 = v186;
    int32_t * v188 = (int32_t *)(v2 + 60); // 0x404a90
    if (*v188 >= 1) {
        // 0x404a98
        *v38 = v176;
        *v39 = v176;
        *v40 = *v188;
        *v41 = v186;
        *v187 = ___pow5mult_D2A((int32_t)&g36, (int32_t)&g36);
    }
    int32_t v189 = 0; // 0x404ab4
    int32_t v190 = v177; // 0x404ab4
    if (*v51 < 2) {
        // 0x404ffb
        v189 = 0;
        v190 = v177;
        if (*v43 == 1) {
            // 0x405006
            v189 = 0;
            v190 = v177;
            if (*(int32_t *)(v2 + 164) > *(int32_t *)(*v73 + 4) + 1) {
                int32_t * v191 = (int32_t *)(v2 + 56); // 0x40501e
                int32_t * v192 = (int32_t *)(v2 + 20); // 0x405022
                int32_t v193 = *v192 + 1; // 0x405027
                *v191 = *v191 + 1;
                *v192 = v193;
                v189 = 1;
                v190 = v193;
            }
        }
    }
    int32_t v194 = 1; // 0x404ac7
    if (*v188 != 0) {
        int32_t v195 = *v187 + 16; // 0x4053e8
        int32_t v196 = *(int32_t *)v195; // 0x4053e8
        int32_t v197 = *(int32_t *)(4 * v196 + v195); // 0x4053eb
        v194 = 32 - ((v197 == 0 ? v196 : llvm_ctlz_i32(v197, true) ^ 31) ^ 31);
    }
    int32_t * v198 = (int32_t *)(v2 + 56); // 0x404acd
    int32_t v199 = *v198; // 0x404acd
    uint32_t v200 = (v199 + v194) % 32; // 0x404ad9
    int32_t v201 = v199; // 0x404adc
    int32_t v202 = 28; // 0x404adc
    int32_t * v203; // 0x404630
    int32_t v204; // 0x404630
    int32_t v205; // 0x404630
    int32_t v206; // 0x404630
    if (v200 == 0) {
        goto lab_0x405197;
    } else {
        uint32_t v207 = 32 - v200; // 0x404ae4
        *v45 = v207;
        if (v207 < 5) {
            if (v207 == 4) {
                // 0x40518e
                v203 = (int32_t *)(v2 + 20);
                v205 = 4;
                v206 = v190;
                v204 = v168;
                goto lab_0x404b12;
            } else {
                // 0x405194
                v201 = *v198;
                v202 = v207 + 28;
                goto lab_0x405197;
            }
        } else {
            int32_t v208 = v207 - 4; // 0x404af6
            int32_t * v209 = (int32_t *)(v2 + 20);
            int32_t v210 = *v209 + v208; // 0x404afd
            *v45 = v208;
            *v209 = v210;
            *v198 = v208 + v199;
            v203 = v209;
            v205 = v210;
            v206 = v190;
            v204 = v208 + v168;
            goto lab_0x404b12;
        }
    }
  lab_0x405197:;
    int32_t * v211 = (int32_t *)(v2 + 20);
    int32_t v212 = v202 + v201; // 0x4051a1
    *v45 = v202;
    *v211 = *v211 + v202;
    *v198 = v212;
    v203 = v211;
    v205 = v202;
    v206 = v212;
    v204 = v202 + v168;
    goto lab_0x404b12;
  lab_0x404b12:;
    uint32_t v213 = *v203; // 0x404b12
    if (v213 >= 1) {
        // 0x404b1a
        *v38 = v205;
        *v39 = v205;
        *v40 = *v203;
        *v41 = *v32;
        *v32 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36);
    }
    uint32_t v214 = *v198; // 0x404b32
    if (v214 >= 1) {
        // 0x404b3a
        *v38 = v214;
        *v39 = v214;
        *v40 = *v198;
        *v41 = *v187;
        *v187 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36);
    }
    int32_t v215 = v213; // 0x404b58
    if (*v50 != 0) {
        // 0x40537c
        *v38 = v206;
        *v39 = v206;
        int32_t v216 = *v187; // 0x40537e
        *v40 = v216;
        *v41 = *v32;
        int32_t v217 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x405388
        v215 = v216;
        if (v217 < 0) {
            // 0x405398
            *v38 = v217;
            *v39 = 0;
            *v40 = 10;
            *v41 = *v32;
            int32_t v218 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x4053a2
            *v32 = v218;
            int32_t v219 = *v137; // 0x4053ae
            if (v219 != 0) {
                // 0x4053b6
                *v38 = v219;
                *v39 = 0;
                *v40 = 10;
                int32_t * v220 = (int32_t *)(v2 + 72); // 0x4053bb
                *v41 = *v220;
                int32_t v221 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x4053c0
                *v220 = v221;
            }
            // 0x4053cc
            *v48 = *v48 - 1;
            *(int32_t *)(v2 + 32) = *(int32_t *)(v2 + 36);
            v215 = v216;
        }
    }
    int32_t * v222 = (int32_t *)(v2 + 32); // 0x404b5e
    int32_t v223 = *v222; // 0x404b5e
    if (v223 < 1) {
        // 0x405558
        if (*v51 < 3) {
            goto lab_0x404b6a;
        } else {
            if (v223 < 0) {
                goto lab_0x40529c;
            } else {
                // 0x405572
                *v38 = v206;
                *v39 = 0;
                *v40 = 5;
                *v41 = *v187;
                int32_t v224 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x40557c
                *v187 = v224;
                *v40 = v224;
                *v41 = *v32;
                if (___cmp_D2A((int32_t)&g36, (int32_t)&g36) < 1) {
                    goto lab_0x40529c;
                } else {
                    goto lab_0x40559d;
                }
            }
        }
    } else {
        goto lab_0x404b6a;
    }
  lab_0x404f4e:;
    int32_t v225 = *v75; // 0x404f59
    *v45 = 1;
    *v47 = *v46 % 256 | 3072;
    int32_t v226 = (int32_t)*v44; // 0x404f9f
    int32_t * v227 = (int32_t *)(v2 + 96); // 0x404f9f
    *v227 = v226;
    *v38 = v226;
    *v44 = (float64_t)0.0;
    *(char *)v225 = (char)v226 + 48;
    int32_t v228 = v225 + 1; // 0x404fbf
    int32_t v229 = v228; // 0x404fd4
    int32_t v230 = 0; // 0x404fd4
    int32_t v231; // 0x404630
    if (v49 != 0x4000) {
        int32_t * v232 = (int32_t *)(v2 + 32); // 0x404f77
        int32_t v233 = *v45; // 0x404f70
        int32_t v234 = v228; // 0x404f7b
        if (*v232 != v233) {
            *v45 = v233 + 1;
            *v227 = 0;
            *v38 = 0;
            *v44 = 0.0;
            *(char *)v228 = 48;
            int32_t v235 = v228 + 1; // 0x404fbf
            int32_t v236 = *v45; // 0x404f70
            int32_t v237 = v235; // 0x404f7b
            v234 = v235;
            while (*v232 != v236) {
                // 0x404f81
                *v45 = v236 + 1;
                *v227 = 0;
                *v38 = 0;
                *v44 = 0.0;
                *(char *)v237 = 48;
                v235 = v237 + 1;
                v236 = *v45;
                v237 = v235;
                v234 = v235;
            }
        }
        // 0x4054e7
        v229 = v234;
        v230 = 16;
        v231 = v234;
        switch (*v77) {
            case 0: {
                // 0x40572c
                *v44 = 0.0;
                v229 = v234;
                v230 = 16;
                v231 = v234;
                if (v49 == 0) {
                    goto lab_0x40550c;
                } else {
                    goto lab_0x404ccb;
                }
            }
            case 1: {
                goto lab_0x40550c;
            }
            default: {
                goto lab_0x404ccb;
            }
        }
    } else {
        goto lab_0x404ccb;
    }
  lab_0x404964:
    // 0x404964
    *v38 = v84;
    float80_t v238 = v84; // 0x404969
    float80_t v239 = v238 * v238 + 7.0L; // 0x404971
    float64_t * v240 = (float64_t *)(v2 + 112); // 0x404977
    *v240 = (float64_t)v239;
    int32_t * v241 = (int32_t *)(v2 + 116); // 0x40497b
    *v241 = *v241 - 0x3400000;
    if (v85 != 0) {
        // 0x4053fa
        if (*(int32_t *)(v2 + 48) == 0) {
            float64_t v242 = *(float64_t *)(8 * v85 + (int32_t)&g11); // 0x405640
            *v45 = 1;
            *v47 = *v46 % 256 | 3072;
            *v240 = (float64_t)((float80_t)*v240 * (float80_t)v242);
            int32_t v243 = *v75; // 0x405666
            int32_t v244 = 1.0; // 0x4056a6
            *(int32_t *)(v2 + 96) = v244;
            if (v244 != 0) {
                // 0x405668
                *v38 = v244;
                *v44 = (float64_t)0.0;
            }
            // 0x405677
            *(char *)v243 = (char)v244 + 48;
            int32_t v245 = v243 + 1; // 0x40567c
            int32_t v246 = *v45; // 0x40567d
            while (v246 != v85) {
                // 0x405688
                *v45 = v246 + 1;
                *v44 = (float64_t)(10.0L * (float80_t)*v44);
                v244 = 1.0;
                *(int32_t *)(v2 + 96) = v244;
                if (v244 != 0) {
                    // 0x405668
                    *v38 = v244;
                    *v44 = (float64_t)0.0;
                }
                // 0x405677
                *(char *)v245 = (char)v244 + 48;
                v245++;
                v246 = *v45;
            }
            // 0x4056ba
            v86 = v85;
            if (v49 == 0) {
                // 0x405761
                *v48 = v87;
                v231 = v245;
                goto lab_0x40550c;
            } else {
                goto lab_0x4049bf;
            }
        } else {
            int32_t v247 = *v75; // 0x40541b
            *v45 = 0;
            *v47 = *v46 % 256 | 3072;
            float64_t v248 = *(float64_t *)(8 * v85 + (int32_t)&g11); // 0x40542f
            *v240 = (float64_t)(0.5L * v239 / (float80_t)v248 - (float80_t)*v240);
            int32_t v249 = (int32_t)*v44; // 0x40548a
            int32_t * v250 = (int32_t *)(v2 + 96); // 0x40548a
            *v250 = v249;
            *v38 = v249;
            *v44 = (float64_t)0.0;
            *(char *)v247 = (char)v249 + 48;
            int32_t v251 = v247 + 1; // 0x4054aa
            if (v49 != 0) {
                int32_t v252 = *v45 + 1; // 0x40545c
                *v45 = v252;
                v86 = v85;
                int32_t v253 = v251; // 0x405466
                if (v85 > v252) {
                    *v240 = 100.0;
                    *v250 = 0x2710;
                    *v38 = 0x2710;
                    *v44 = 0.0;
                    *(char *)v253 = 64;
                    uint32_t v254 = *v45 + 1; // 0x40545c
                    *v45 = v254;
                    v86 = v85;
                    v253++;
                    while (v85 > v254) {
                        // 0x40546c
                        *v240 = 100.0;
                        *v250 = 0x2710;
                        *v38 = 0x2710;
                        *v44 = 0.0;
                        *(char *)v253 = 64;
                        v254 = *v45 + 1;
                        *v45 = v254;
                        v86 = v85;
                        v253++;
                    }
                }
                goto lab_0x4049bf;
            } else {
                // 0x4054be
                *v48 = v87;
                v229 = v251;
                v230 = v49 == 0x4000 ? 0 : 16;
                goto lab_0x404ccb;
            }
        }
    } else {
        // 0x40498f
        *v44 = (float64_t)(v239 - 5.0L);
        v86 = v85;
        if (v49 == 0) {
            // 0x4056fb
            *v48 = v87;
            // 0x4055ec
            *(int32_t *)(v2 + 72) = 0;
            *(int32_t *)(v2 + 76) = 0;
            goto lab_0x40559d;
        } else {
            goto lab_0x4049bf;
        }
    }
  lab_0x4049bf:
    // 0x4049bf
    *v44 = *v88;
    v83 = v86;
    goto lab_0x4049c7;
  lab_0x404b6a:;
    // 0x404b6a
    int32_t * v255; // 0x404630
    int32_t v256; // 0x404630
    int32_t v257; // 0x404630
    int32_t v258; // 0x404630
    int32_t v259; // 0x404630
    int32_t v260; // 0x404630
    int32_t v261; // 0x404630
    int32_t v262; // 0x404630
    int32_t v263; // 0x404630
    int32_t v264; // 0x404630
    int32_t v265; // 0x404630
    int32_t v266; // 0x404630
    int32_t * v267; // 0x404ba9
    int32_t * v268; // 0x404630
    char * v269; // 0x404630
    int32_t v270; // 0x4057bb
    int32_t v271; // 0x4059a2
    if (*v137 == 0) {
        int32_t v272 = *v75; // 0x4052cf
        *v45 = 1;
        *v38 = v215;
        *v39 = v215;
        *v40 = *v187;
        *v41 = *v32;
        int32_t v273 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36) + 48; // 0x405307
        *(int32_t *)(v2 + 28) = v273;
        *(char *)v272 = (char)v273;
        int32_t v274 = v272 + 1; // 0x405313
        while (*v222 > *v45) {
            // 0x4052d1
            *v38 = v273;
            *v39 = 0;
            *v40 = 10;
            *v41 = *v32;
            int32_t v275 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x4052db
            *v32 = v275;
            *v45 = *v45 + 1;
            *v38 = v215;
            *v39 = v215;
            *v40 = *v187;
            *v41 = *v32;
            v273 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36) + 48;
            *(int32_t *)(v2 + 28) = v273;
            *(char *)v274 = (char)v273;
            v274++;
        }
        // 0x405321
        *(int32_t *)(v2 + 68) = 0;
        v257 = v223;
        v259 = v274;
      lab_0x405329:
        // 0x405329
        switch (*v77) {
            case 0: {
                // 0x405864
                *v38 = v257;
                *v39 = v257;
                *v40 = 1;
                *v41 = *v32;
                int32_t v276 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36); // 0x40586d
                *v32 = v276;
                *v40 = *v187;
                *v41 = v276;
                int32_t v277 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x40587e
                v260 = v259;
                if (v277 >= 0 == (v277 != 0)) {
                    goto lab_0x40534e;
                } else {
                    if (v277 != 0) {
                        goto lab_0x40589c;
                    } else {
                        // 0x405891
                        v260 = v259;
                        if (*(char *)(v2 + 28) % 2 != 0) {
                            goto lab_0x40534e;
                        } else {
                            goto lab_0x40589c;
                        }
                    }
                }
            }
            case 2: {
                goto lab_0x40589c;
            }
            default: {
                int32_t v278 = *v32; // 0x405340
                v260 = v259;
                if (*(int32_t *)(v278 + 16) < 2) {
                    // 0x405a5c
                    v260 = v259;
                    v265 = 0;
                    if (*(int32_t *)(v278 + 20) != 0) {
                        goto lab_0x40534e;
                    } else {
                        goto lab_0x4058af;
                    }
                } else {
                    goto lab_0x40534e;
                }
            }
        }
    } else {
        int32_t v279; // 0x404630
        if (v204 < 1) {
            int32_t * v280 = (int32_t *)(v2 + 72); // 0x404b8e
            v279 = *v280;
            v255 = v280;
        } else {
            // 0x404b7a
            *v38 = v223;
            *v39 = v223;
            *v40 = v204;
            int32_t * v281 = (int32_t *)(v2 + 72);
            *v41 = *v281;
            int32_t v282 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36); // 0x404b82
            *v281 = v282;
            v279 = v282;
            v255 = v281;
        }
        int32_t v283 = v279; // 0x404b94
        int32_t v284 = v279; // 0x404b94
        if (v189 != 0) {
            // 0x40590a
            *v41 = *(int32_t *)(v279 + 4);
            int32_t v285 = ___Balloc_D2A((int32_t)&g36); // 0x405915
            int32_t v286 = *v255; // 0x40591a
            int32_t v287 = *(int32_t *)(v286 + 16); // 0x40591f
            __asm_rep_movsd_memcpy((char *)(v285 + 12), (char *)(v286 + 12), (4 * v287 + 8) / 4);
            *v40 = 1;
            *v41 = v285;
            v284 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36);
            v283 = *v255;
        }
        // 0x404b9a
        *v45 = 1;
        v267 = (int32_t *)(v2 + 68);
        *v267 = v283;
        *v255 = v284;
        int32_t v288 = v2 + 28;
        v268 = (int32_t *)v288;
        int32_t * v289 = (int32_t *)(v2 + 168);
        v269 = (char *)v288;
        v263 = *v75;
        *v38 = v284;
        *v39 = v284;
        *v40 = *v187;
        *v41 = *v32;
        *v268 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36) + 48;
        *v40 = *v267;
        *v41 = *v32;
        int32_t v290 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x404bde
        *v40 = *v255;
        *v41 = *v187;
        int32_t v291 = ___diff_D2A((int32_t)&g36, (int32_t)&g36); // 0x404bf1
        int32_t v292 = 1; // 0x404c00
        if (*(int32_t *)(v291 + 12) == 0) {
            // 0x4050bb
            *v38 = 1;
            *v39 = 1;
            *v40 = v291;
            *v41 = *v32;
            v292 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36);
        }
        // 0x404c06
        v256 = v292;
        *v41 = v291;
        ___Bfree_D2A((int32_t)&g36);
        int32_t v293 = *v51; // 0x404c12
        int32_t v294 = *v40; // 0x404c1b
        int32_t v295; // 0x405a3e
        int32_t v296; // 0x405aa8
        if ((v293 || v256) == 0) {
            // 0x404c1d
            v294 = *v289;
            if ((*(int32_t *)v294 % 2 | *v77) == 0) {
                // 0x405a3e
                v295 = *v268;
                v262 = v263;
                if (v295 == 57) {
                    goto lab_0x405a2a;
                } else {
                    if (v290 < 1) {
                        // 0x405aa8
                        v296 = *v32;
                        if (*(int32_t *)(v296 + 16) > 1) {
                            // 0x4057dc
                            v264 = 16;
                            v261 = v263;
                            goto lab_0x4057e1;
                        } else {
                            // 0x405ab6
                            v264 = 0;
                            v261 = v263;
                            if (*(int32_t *)(v296 + 20) == 0) {
                                goto lab_0x4057e1;
                            } else {
                                // 0x4057dc
                                v264 = 16;
                                v261 = v263;
                                goto lab_0x4057e1;
                            }
                        }
                    } else {
                        // 0x405a49
                        *v268 = v295 + 1;
                        v264 = 32;
                        v261 = v263;
                        goto lab_0x4057e1;
                    }
                }
            }
        }
        while (v290 >= 0) {
            int32_t v297 = v294; // 0x404c42
            if ((v293 || v290) == 0) {
                // 0x404c44
                v297 = *v289;
                if (*(char *)v297 % 2 == 0) {
                    // break -> 0x4057bb
                    break;
                }
            }
            int32_t v298 = v297;
            if (v256 >= 1) {
                // 0x404c5c
                if (*v77 != 2) {
                    // 0x404c67
                    v262 = v263;
                    if (*v268 == 57) {
                        goto lab_0x405a2a;
                    } else {
                        // 0x404c72
                        *(char *)v263 = *v269 + 1;
                        v258 = v263 + 1;
                        v266 = 32;
                        goto lab_0x404c81;
                    }
                }
            }
            unsigned char v299 = *v269; // 0x405040
            *(char *)v263 = v299;
            int32_t v300 = v263 + 1; // 0x405046
            v257 = v256;
            v259 = v300;
            if (*v45 == *v222) {
                goto lab_0x405329;
            }
            // 0x405058
            *v38 = v291;
            *v39 = 0;
            *v40 = 10;
            *v41 = *v32;
            int32_t v301 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x405062
            *v32 = v301;
            int32_t v302 = *v255; // 0x40506e
            int32_t v303; // 0x404630
            if (*v267 == v302) {
                // 0x405173
                *v38 = v298 & -256 | (int32_t)v299;
                *v39 = 0;
                *v40 = 10;
                *v41 = v302;
                int32_t v304 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x405179
                *v255 = v304;
                *v267 = v304;
                v303 = v304;
            } else {
                // 0x40507c
                *v38 = v302;
                *v39 = 0;
                *v40 = 10;
                *v41 = *v267;
                int32_t v305 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x405086
                *v267 = v305;
                *v39 = 0;
                *v40 = 10;
                *v41 = *v255;
                int32_t v306 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x40509b
                *v255 = v306;
                v303 = v306;
            }
            // 0x4050a7
            *v45 = *v45 + 1;
            v263 = v300;
            *v38 = v303;
            *v39 = v303;
            *v40 = *v187;
            *v41 = *v32;
            *v268 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36) + 48;
            *v40 = *v267;
            *v41 = *v32;
            v290 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36);
            *v40 = *v255;
            *v41 = *v187;
            v291 = ___diff_D2A((int32_t)&g36, (int32_t)&g36);
            v292 = 1;
            if (*(int32_t *)(v291 + 12) == 0) {
                // 0x4050bb
                *v38 = 1;
                *v39 = 1;
                *v40 = v291;
                *v41 = *v32;
                v292 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36);
            }
            // 0x404c06
            v256 = v292;
            *v41 = v291;
            ___Bfree_D2A((int32_t)&g36);
            v293 = *v51;
            v294 = *v40;
            if ((v293 || v256) == 0) {
                // 0x404c1d
                v294 = *v289;
                if ((*(int32_t *)v294 % 2 | *v77) == 0) {
                    // 0x405a3e
                    v295 = *v268;
                    v262 = v263;
                    if (v295 == 57) {
                        goto lab_0x405a2a;
                    } else {
                        if (v290 < 1) {
                            // 0x405aa8
                            v296 = *v32;
                            if (*(int32_t *)(v296 + 16) > 1) {
                                // 0x4057dc
                                v264 = 16;
                                v261 = v263;
                                goto lab_0x4057e1;
                            } else {
                                // 0x405ab6
                                v264 = 0;
                                v261 = v263;
                                if (*(int32_t *)(v296 + 20) == 0) {
                                    goto lab_0x4057e1;
                                } else {
                                    // 0x4057dc
                                    v264 = 16;
                                    v261 = v263;
                                    goto lab_0x4057e1;
                                }
                            }
                        } else {
                            // 0x405a49
                            *v268 = v295 + 1;
                            v264 = 32;
                            v261 = v263;
                            goto lab_0x4057e1;
                        }
                    }
                }
            }
        }
        // 0x4057bb
        v270 = *v77;
        if (v270 == 0) {
            goto lab_0x4059ad;
        } else {
            int32_t v307 = *v32; // 0x4057c7
            v271 = v270;
            if (*(int32_t *)(v307 + 16) < 2) {
                // 0x4059a2
                v271 = *(int32_t *)(v307 + 20);
                if (v271 != 0) {
                    goto lab_0x4057d5;
                } else {
                    goto lab_0x4059ad;
                }
            } else {
                goto lab_0x4057d5;
            }
        }
    }
  lab_0x404ccb:
    // 0x404ccb
    *v41 = *v32;
    ___Bfree_D2A((int32_t)&g36);
    *(char *)v229 = 0;
    *(int32_t *)*(int32_t *)(v2 + 184) = *v48 + 1;
    int32_t v308 = *(int32_t *)(v2 + 188); // 0x404cec
    if (v308 != 0) {
        // 0x404cf7
        *(int32_t *)v308 = v229;
    }
    int32_t * v309 = (int32_t *)*(int32_t *)(v2 + 172); // 0x404d07
    *v309 = *v309 | v230;
    // 0x404d10
    return *v75;
  lab_0x40550c:;
    int32_t v310 = v231 - 1; // 0x40550c
    char * v311 = (char *)v310;
    char v312 = *v311; // 0x40550d
    char * v313; // 0x404630
    char v314; // 0x404630
    int32_t v315; // 0x404630
    while (v312 == 57) {
        int32_t v316 = *v75; // 0x405513
        if (v310 == v316) {
            // 0x405519
            *v48 = *v48 + 1;
            char * v317 = (char *)v316;
            *v317 = 48;
            v313 = v317;
            v314 = 49;
            v315 = v316;
            goto lab_0x40552d;
        }
        v310--;
        v311 = (char *)v310;
        v312 = *v311;
    }
    // 0x40552d
    v313 = v311;
    v314 = v312 + 1;
    v315 = v310;
    goto lab_0x40552d;
  lab_0x40529c:
    // 0x40529c
    *v48 = -1 - *(int32_t *)(v2 + 180);
    *(int32_t *)(v2 + 68) = 0;
    v258 = *v75;
    v266 = 16;
    goto lab_0x404c81;
  lab_0x40589c:;
    int32_t v343 = *v32; // 0x40589c
    if (*(int32_t *)(v343 + 16) < 2) {
        // 0x405a6e
        v265 = 0;
        if (*(int32_t *)(v343 + 20) != 0) {
            // 0x4058aa
            v265 = 16;
            goto lab_0x4058af;
        } else {
            goto lab_0x4058af;
        }
    } else {
        // 0x4058aa
        v265 = 16;
        goto lab_0x4058af;
    }
  lab_0x4059ad:;
    int32_t v328 = 0; // 0x4059b1
    if (v256 < 1) {
        goto lab_0x4059f3;
    } else {
        // 0x4059b3
        *v38 = v256;
        *v39 = v256;
        *v40 = 1;
        *v41 = *v32;
        int32_t v344 = ___lshift_D2A((int32_t)&g36, (int32_t)&g36); // 0x4059bc
        *v32 = v344;
        *v40 = *v187;
        *v41 = v344;
        int32_t v345 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x4059cd
        if (v345 < 1) {
            // 0x405a80
            v328 = 32;
            if (v345 != 0) {
                goto lab_0x4059f3;
            } else {
                // 0x405a86
                v328 = 32;
                if (*v269 % 2 == 0) {
                    goto lab_0x4059f3;
                } else {
                    goto lab_0x4059de;
                }
            }
        } else {
            goto lab_0x4059de;
        }
    }
  lab_0x40559d:;
    int32_t v346 = *v75; // 0x40559d
    *(char *)v346 = 49;
    *(int32_t *)(v2 + 68) = 0;
    *v48 = *v48 + 1;
    v258 = v346 + 1;
    v266 = 32;
    goto lab_0x404c81;
  lab_0x40552d:
    // 0x40552d
    *v313 = v314;
    v229 = v315 + 1;
    v230 = 32;
    goto lab_0x404ccb;
  lab_0x404c81:
    // 0x404c81
    *v41 = *(int32_t *)(v2 + 76);
    ___Bfree_D2A((int32_t)&g36);
    int32_t * v318 = (int32_t *)(v2 + 72); // 0x404c91
    int32_t v319 = *v318; // 0x404c91
    v229 = v258;
    v230 = v266;
    if (v319 != 0) {
        int32_t v320 = *(int32_t *)(v2 + 68); // 0x404c99
        int32_t v321 = v319; // 0x404c9f
        if (v320 != 0 && v320 != v319) {
            // 0x404cab
            *v41 = v320;
            ___Bfree_D2A((int32_t)&g36);
            v321 = *v318;
        }
        // 0x404cbb
        *v41 = v321;
        ___Bfree_D2A((int32_t)&g36);
        v229 = v258;
        v230 = v266;
    }
    goto lab_0x404ccb;
  lab_0x40534e:;
    int32_t v322 = v260;
    int32_t v323 = v322 - 1; // 0x40534e
    char * v324 = (char *)v323;
    char v325 = *v324; // 0x40534f
    while (v325 == 57) {
        // 0x405359
        if (v323 == *v75) {
            // 0x40535f
            *v48 = *v48 + 1;
            int32_t v326 = *v75; // 0x40536d
            *(char *)v326 = 49;
            v258 = v326 + 1;
            v266 = 32;
            goto lab_0x404c81;
        }
        v322 = v323;
        v323 = v322 - 1;
        v324 = (char *)v323;
        v325 = *v324;
    }
    // 0x4058fa
    *v324 = v325 + 1;
    v258 = v322;
    v266 = 32;
    goto lab_0x404c81;
  lab_0x4059f3:;
    int32_t v327 = *v32; // 0x4059f3
    if (*(int32_t *)(v327 + 16) > 1) {
        // 0x4057dc
        v264 = 16;
        v261 = v263;
        goto lab_0x4057e1;
    } else {
        // 0x405a01
        v264 = v328;
        v261 = v263;
        if (*(int32_t *)(v327 + 20) == 0) {
            goto lab_0x4057e1;
        } else {
            // 0x4057dc
            v264 = 16;
            v261 = v263;
            goto lab_0x4057e1;
        }
    }
  lab_0x4057d5:
    if (v270 != 2) {
        // 0x405822
        *v38 = v271;
        *v39 = v271;
        *v40 = *v255;
        *v41 = *v187;
        int32_t v329 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x40582e
        int32_t v330 = v263; // 0x405838
        if (v329 >= 1) {
            unsigned char v331 = *v269; // 0x40583e
            *(char *)v263 = v331;
            *v38 = v329 & -256 | (int32_t)v331;
            *v39 = 0;
            *v40 = 10;
            *v41 = *v255;
            int32_t v332 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x40584e
            if (*v267 == *v255) {
                // 0x405862
                *v267 = v332;
            }
            // 0x4057f3
            *v38 = v332;
            *v39 = 0;
            *v40 = 10;
            int32_t v333 = v263 + 1; // 0x4057f8
            *v41 = *v32;
            int32_t v334 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36); // 0x4057fe
            *v32 = v334;
            *v40 = *v187;
            *v41 = v334;
            int32_t v335 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36); // 0x40580f
            *v255 = v332;
            int32_t v336 = v335 + 48; // 0x405818
            *v268 = v336;
            *v38 = v336;
            *v39 = v336;
            *v40 = *v255;
            *v41 = *v187;
            int32_t v337 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36); // 0x40582e
            int32_t v338 = v333; // 0x405838
            v330 = v333;
            while (v337 >= 1) {
                // 0x40583e
                v331 = *v269;
                *(char *)v338 = v331;
                *v38 = v337 & -256 | (int32_t)v331;
                *v39 = 0;
                *v40 = 10;
                *v41 = *v255;
                v332 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                if (*v267 == *v255) {
                    // 0x405862
                    *v267 = v332;
                }
                // 0x4057f3
                *v38 = v332;
                *v39 = 0;
                *v40 = 10;
                v333 = v338 + 1;
                *v41 = *v32;
                v334 = ___multadd_D2A((int32_t)&g36, (int32_t)&g36, (int32_t)&g36);
                *v32 = v334;
                *v40 = *v187;
                *v41 = v334;
                v335 = ___quorem_D2A((int32_t)&g36, (int32_t)&g36);
                *v255 = v332;
                v336 = v335 + 48;
                *v268 = v336;
                *v38 = v336;
                *v39 = v336;
                *v40 = *v255;
                *v41 = *v187;
                v337 = ___cmp_D2A((int32_t)&g36, (int32_t)&g36);
                v338 = v333;
                v330 = v333;
            }
        }
        int32_t v339 = *v268 + 1; // 0x405a1a
        *v268 = v339;
        v264 = 32;
        v261 = v330;
        v262 = v330;
        if (v339 != 58) {
            goto lab_0x4057e1;
        } else {
            goto lab_0x405a2a;
        }
    } else {
        // 0x4057dc
        v264 = 16;
        v261 = v263;
        goto lab_0x4057e1;
    }
  lab_0x4058af:;
    int32_t v340 = v259 - 1; // 0x4058af
    v258 = v259;
    v266 = v265;
    int32_t v341 = v340; // 0x4058b3
    while (*(char *)v340 == 48) {
        // 0x4058af
        v340 = v341 - 1;
        v258 = v341;
        v266 = v265;
        v341 = v340;
    }
    goto lab_0x404c81;
  lab_0x4059de:;
    int32_t v342 = *v268 + 1; // 0x4059e2
    *v268 = v342;
    v328 = 32;
    v262 = v263;
    if (v342 == 58) {
        goto lab_0x405a2a;
    } else {
        goto lab_0x4059f3;
    }
  lab_0x4057e1:
    // 0x4057e1
    *(char *)v261 = *v269;
    v258 = v261 + 1;
    v266 = v264;
    goto lab_0x404c81;
  lab_0x405a2a:
    // 0x405a2a
    *(char *)v262 = 57;
    v260 = v262 + 1;
    goto lab_0x40534e;
}

// Address range: 0x405ad0 - 0x405bdc
int32_t ___mbrtowc_cp(int32_t a1, int32_t CodePage, uint32_t a3) {
    int32_t result = 0; // 0x405ad8
    int32_t v1; // 0x405ad0
    if (v1 == 0) {
        // 0x405ae2
        return result;
    }
    // 0x405af0
    result = -2;
    uint32_t v2; // 0x405ad0
    if (v2 == 0) {
        // 0x405ae2
        return result;
    }
    // 0x405af9
    result = a1;
    int32_t * v3 = (int32_t *)a1; // 0x405afd
    char v4 = (char)*v3; // 0x405aff
    char v5 = v4; // bp-16, 0x405aff
    *v3 = 0;
    char TestChar = *(char *)&result; // 0x405b09
    int32_t v6; // 0x405ad0
    if (TestChar == 0) {
        // 0x405b0f
        result = 0;
        *(int16_t *)v6 = 0;
        // 0x405ae2
        return result;
    }
    // 0x405b18
    int32_t v7; // bp-28, 0x405ad0
    int32_t * v8 = &v7; // 0x405b1d
    int32_t cbMultiByte; // 0x405ad0
    int32_t dwFlags; // 0x405ad0
    int32_t CodePage2; // bp-52, 0x405ad0
    int32_t * v9; // 0x405ad0
    if (a3 < 2) {
        goto lab_0x405b60;
    } else {
        if (v4 == 0) {
            int32_t v10 = result; // 0x405ba6
            int32_t v11 = v10; // bp-36, 0x405ba6
            bool v12 = IsDBCSLeadByteEx(CodePage, TestChar); // 0x405bad
            v8 = &v11;
            if (!v12) {
                goto lab_0x405b60;
            } else {
                if (v2 < 2) {
                    // 0x405bca
                    int32_t v13; // 0x405ad0
                    *(char *)v13 = (char)v10;
                    result = -2;
                    // 0x405ae2
                    return result;
                }
                // 0x405bbd
                dwFlags = 1;
                cbMultiByte = 0x1000000 * v2 / 0x1000000;
                int32_t v14; // bp-60, 0x405ad0
                v9 = &v14;
                goto lab_0x405b36;
            }
        } else {
            // 0x405b26
            CodePage2 = &v5;
            dwFlags = 2;
            cbMultiByte = 1;
            v9 = &CodePage2;
            goto lab_0x405b36;
        }
    }
  lab_0x405b60:;
    int32_t v15 = (int32_t)v8;
    int32_t * v16 = (int32_t *)(v15 + 36); // 0x405b60
    if (*v16 == 0) {
        // 0x405b68
        result = 1;
        *(int16_t *)v6 = 1;
        // 0x405ae2
        return result;
    }
    // 0x405b79
    *(int32_t *)(v15 - 12) = 1;
    *(int32_t *)(v15 - 20) = 1;
    *(int32_t *)(v15 - 28) = 8;
    *(int32_t *)(v15 - 32) = *v16;
    int32_t v17 = MultiByteToWideChar((int32_t)&g36, (int32_t)&g36, (char *)&g36, (int32_t)&g36, (int16_t *)&g36, (int32_t)&g36); // 0x405b88
    result = 1;
    if (v17 != 0) {
        // 0x405ae2
        return result;
    }
    int32_t * v18 = _errno(); // 0x405b4d
    result = -1;
    *v18 = 42;
    // 0x405ae2
    return result;
  lab_0x405b36:;
    int32_t v19 = (int32_t)v9;
    *(int32_t *)(v19 - 4) = 8;
    *(int32_t *)(v19 - 8) = *(int32_t *)(v19 + 60);
    int32_t lpMultiByteStr; // 0x405ad0
    int32_t v20 = MultiByteToWideChar(CodePage2, dwFlags, (char *)lpMultiByteStr, cbMultiByte, (int16_t *)&g36, (int32_t)&g36); // 0x405b3d
    result = 2;
    if (v20 != 0) {
        // 0x405ae2
        return result;
    }
    // 0x405b4d
    v18 = _errno();
    result = -1;
    *v18 = 42;
    // 0x405ae2
    return result;
}

// Address range: 0x405cd0 - 0x405d27
int32_t _mbrtowc(int32_t * a1, int32_t a2, int32_t a3, int32_t * a4) {
    int32_t v1 = *(int32_t *)*(int32_t *)0x40b1c0; // 0x405cee
    int32_t v2 = a4 == NULL ? (int32_t)&g27 : (int32_t)a4;
    return ___mbrtowc_cp(v2, v1, *(int32_t *)*(int32_t *)0x40b1c4);
}

// Address range: 0x405d80 - 0x405de5
int32_t ___wcrtomb_cp(int32_t cbMultiByte) {
    // 0x405d80
    int32_t v1; // 0x405d80
    int16_t v2 = v1; // 0x405d88
    int16_t lpWideCharStr = v2; // bp-24, 0x405d88
    int32_t CodePage; // 0x405d80
    int32_t lpMultiByteStr; // 0x405d80
    if (CodePage == 0) {
        if (v2 >= 256) {
            // 0x405dd2
            *_errno() = 42;
            return -1;
        }
        // 0x405d96
        *(char *)lpMultiByteStr = (char)v1;
        // 0x405d9d
        return 1;
    }
    int32_t lpUsedDefaultChar = 0; // bp-8, 0x405da6
    int32_t result = WideCharToMultiByte(CodePage, 0, &lpWideCharStr, 1, (char *)lpMultiByteStr, cbMultiByte, NULL, (bool *)&lpUsedDefaultChar); // 0x405dc1
    if (cbMultiByte == 0 == (result != 0)) {
        // 0x405d9d
        return result;
    }
    // 0x405dd2
    *_errno() = 42;
    return -1;
}

// Address range: 0x405ee0 - 0x405f1c
int32_t _wcrtomb(int32_t a1, int32_t a2) {
    // 0x405ee0
    return ___wcrtomb_cp(*(int32_t *)*(int32_t *)0x40b1c4);
}

// Address range: 0x405f20 - 0x406140
int32_t ___quorem_D2A(int32_t a1, int32_t a2) {
    int32_t v1 = *(int32_t *)(a2 + 16); // 0x405f2f
    int32_t v2 = a1 + 16; // 0x405f34
    int32_t * v3 = (int32_t *)v2; // 0x405f34
    if (v1 > *v3) {
        // 0x40605c
        return 0;
    }
    int32_t v4 = v1 - 1; // 0x405f3d
    uint32_t v5 = a1 + 20; // 0x405f42
    int32_t v6 = 4 * v4; // 0x405f49
    int32_t v7 = a2 + 20; // 0x405f49
    uint32_t v8 = v6 + v7; // 0x405f49
    int32_t * v9 = (int32_t *)(v6 + v5); // 0x405f68
    uint32_t v10 = *v9; // 0x405f68
    uint32_t v11 = *(int32_t *)v8 + 1; // 0x405f6c
    uint32_t result = v10 / v11;
    int32_t v12 = v4; // 0x405f81
    if (v11 <= v10) {
        int32_t v13 = v5; // 0x406088
        int32_t v14 = v7; // 0x406088
        int32_t v15 = 0; // 0x406088
        uint64_t v16 = (int64_t)*(int32_t *)v14 * (int64_t)result; // 0x40609a
        uint32_t v17 = (int32_t)v16; // 0x40609a
        uint32_t v18 = v17; // 0x40609e
        v14 += 4;
        int32_t * v19 = (int32_t *)v13; // 0x4060c2
        uint32_t v20 = *v19; // 0x4060c2
        uint32_t v21 = v20 - v18; // 0x4060ce
        *v19 = v21 - v15;
        v13 += 4;
        int32_t v22 = (int32_t)(v18 < v17) + (int32_t)(v16 / 0x100000000); // 0x4060f6
        v15 = ((int32_t)(v20 < v18) - (int32_t)(v21 < v15)) % 2;
        while (v8 >= v14) {
            // 0x406090
            v16 = (int64_t)*(int32_t *)v14 * (int64_t)result;
            v17 = (int32_t)v16;
            v18 = v22 + v17;
            v14 += 4;
            v19 = (int32_t *)v13;
            v20 = *v19;
            v21 = v20 - v18;
            *v19 = v21 - v15;
            v13 += 4;
            v22 = (int32_t)(v18 < v17) + (int32_t)(v16 / 0x100000000);
            v15 = ((int32_t)(v20 < v18) - (int32_t)(v21 < v15)) % 2;
        }
        // 0x4060f8
        v12 = v4;
        if (*v9 == 0) {
            int32_t v23 = v6 + v2; // 0x40610c
            int32_t v24 = v4; // 0x406114
            int32_t v25 = v23; // 0x406114
            int32_t v26 = v4; // 0x406114
            if (v5 < v23) {
                v26 = v24;
                while (*(int32_t *)v25 == 0) {
                    int32_t v27 = v25 - 4; // 0x40611c
                    int32_t v28 = v24 - 1; // 0x40611f
                    v24 = v28;
                    v25 = v27;
                    v26 = v28;
                    if (v5 >= v27) {
                        // break -> 0x406130
                        break;
                    }
                    v26 = v24;
                }
            }
            // 0x406130
            *v3 = v26;
            v12 = v26;
        }
    }
    int32_t v29 = v7; // 0x405f9d
    int32_t v30 = v5; // 0x405f9d
    if (___cmp_D2A(a1, a2) < 0) {
        // 0x40605c
        return result;
    }
    int32_t v31 = 0; // 0x405f9d
    uint32_t v32 = *(int32_t *)v29; // 0x405fca
    uint32_t v33 = v32; // 0x405fcc
    v29 += 4;
    int32_t * v34 = (int32_t *)v30; // 0x405fe7
    uint32_t v35 = *v34; // 0x405fe7
    uint32_t v36 = v35 - v33; // 0x405ff2
    *v34 = v36 - v31;
    int32_t v37 = v33 < v32; // 0x406017
    v30 += 4;
    v31 = ((int32_t)(v35 < v33) - (int32_t)(v36 < v31)) % 2;
    while (v8 >= v29) {
        // 0x405fc4
        v32 = *(int32_t *)v29;
        v33 = v32 + v37;
        v29 += 4;
        v34 = (int32_t *)v30;
        v35 = *v34;
        v36 = v35 - v33;
        *v34 = v36 - v31;
        v37 = v33 < v32;
        v30 += 4;
        v31 = ((int32_t)(v35 < v33) - (int32_t)(v36 < v31)) % 2;
    }
    int32_t result2 = result + 1; // 0x405fab
    int32_t v38 = 4 * v12; // 0x406021
    if (*(int32_t *)(v38 + v5) != 0) {
        // 0x40605c
        return result2;
    }
    int32_t v39 = v38 + v2; // 0x406029
    int32_t v40 = v12; // 0x406031
    int32_t v41 = v39; // 0x406031
    if (v5 >= v39) {
        // 0x40604d
        *v3 = v12;
        // 0x40605c
        return result2;
    }
    while (*(int32_t *)v41 == 0) {
        // 0x406035
        v41 -= 4;
        v40--;
        if (v5 >= v41) {
            // break -> 0x40604d
            break;
        }
    }
    // 0x40604d
    *v3 = v40;
    // 0x40605c
    return result2;
}

// Address range: 0x406140 - 0x406160
int32_t ___freedtoa(int32_t a1) {
    int32_t v1 = a1 - 4; // 0x406144
    uint32_t v2 = *(int32_t *)v1; // 0x406144
    *(int32_t *)a1 = v2;
    *(int32_t *)(a1 + 4) = 1 << v2 % 32;
    return ___Bfree_D2A(v1);
}

// Address range: 0x406160 - 0x406191
int32_t ___rv_alloc_D2A(uint32_t a1) {
    int32_t v1 = 4; // 0x40616d
    int32_t v2 = 0; // 0x40616d
    int32_t v3; // 0x406182
    if (a1 < 20) {
        int32_t v4 = 0;
        v3 = ___Balloc_D2A(v4);
        *(int32_t *)v3 = v4;
        return v3 + 4;
    }
    v1 *= 2;
    v2++;
    int32_t v5 = v2; // 0x40617c
    while (v1 + 16 <= a1) {
        // 0x406174
        v1 *= 2;
        v2++;
        v5 = v2;
    }
    // 0x40617e
    v3 = ___Balloc_D2A(v5);
    *(int32_t *)v3 = v5;
    return v3 + 4;
}

// Address range: 0x4061a0 - 0x4061e7
int32_t ___nrv_alloc_D2A(int32_t a1, int32_t a2, int32_t a3) {
    int32_t result = ___rv_alloc_D2A(a3); // 0x4061b3
    char v1 = *(char *)a1; // 0x4061ba
    *(char *)result = v1;
    int32_t v2 = result; // 0x4061c5
    int32_t v3 = a1; // 0x4061c5
    int32_t v4 = result; // 0x4061c5
    if (v1 != 0) {
        v2++;
        v3++;
        char v5 = *(char *)v3; // 0x4061d1
        *(char *)v2 = v5;
        v4 = v2;
        while (v5 != 0) {
            // 0x4061d0
            v2++;
            v3++;
            v5 = *(char *)v3;
            *(char *)v2 = v5;
            v4 = v2;
        }
    }
    if (a2 != 0) {
        // 0x4061df
        *(int32_t *)a2 = v4;
    }
    // 0x4061e1
    return result;
}

// Address range: 0x4061f0 - 0x406201
int32_t ___fpclassify(int32_t a1) {
    int32_t v1 = __asm_fxam((float80_t)(float64_t)(int64_t)a1); // 0x4061f4
    __asm_wait();
    return v1 & 0x4500;
}

// Address range: 0x406210 - 0x406263
int32_t ___cmp_D2A(int32_t a1, int32_t a2) {
    int32_t v1 = *(int32_t *)(a2 + 16); // 0x40621e
    int32_t result = *(int32_t *)(a1 + 16) - v1; // 0x406223
    if (result != 0) {
        // 0x406252
        return result;
    }
    int32_t v2 = 4 * v1 + 20;
    int32_t v3 = v2 + a2; // 0x406239
    int32_t v4 = v2 + a1; // 0x406239
    v3 -= 4;
    uint32_t v5 = *(int32_t *)v3; // 0x406240
    v4 -= 4;
    uint32_t v6 = *(int32_t *)v4; // 0x406243
    while (v6 == v5) {
        // 0x406248
        if (a1 + 20 >= v4) {
            // 0x406252
            return result;
        }
        v3 -= 4;
        v5 = *(int32_t *)v3;
        v4 -= 4;
        v6 = *(int32_t *)v4;
    }
    // 0x406258
    return v6 < v5 ? -1 : 1;
}

// Address range: 0x406290 - 0x406343
int32_t _dtoa_lock(void) {
    // 0x406290
    int32_t v1; // bp-12, 0x406290
    int32_t v2 = &v1; // 0x406292
    int32_t v3; // 0x406290
    if (g28 == 2) {
        // 0x406311
        *(int32_t *)(v2 - 16) = 24 * v3 + (int32_t)&g29;
        EnterCriticalSection((struct _RTL_CRITICAL_SECTION *)&g36);
        return *(int32_t *)(v2 - 4);
    }
    int32_t v4 = v2; // 0x4062a3
    int32_t v5; // 0x406290
    if (g28 == 0) {
        int32_t v6 = InterlockedExchange(&g28, 1); // 0x4062d0
        int32_t v7; // bp-20, 0x406290
        v4 = &v7;
        int32_t v8 = (int32_t)&g29; // 0x4062d9
        if (v6 == 0) {
            int32_t v9 = v4;
            *(int32_t *)(v9 - 16) = v8;
            InitializeCriticalSection((struct _RTL_CRITICAL_SECTION *)&g36);
            int32_t v10 = v8 + 24; // 0x4062e9
            int32_t v11 = v9 - 4; // 0x4062ec
            v8 = v10;
            while (v10 != (int32_t)&g30) {
                // 0x4062e0
                v9 = v11;
                *(int32_t *)(v9 - 16) = v8;
                InitializeCriticalSection((struct _RTL_CRITICAL_SECTION *)&g36);
                v10 = v8 + 24;
                v11 = v9 - 4;
                v8 = v10;
            }
            // 0x4062f7
            *(int32_t *)(v9 - 20) = 0x406350;
            atexit2((void (*)())&g36);
            g28 = 2;
            v5 = v11;
          lab_0x406311:
            // 0x406311
            *(int32_t *)(v5 - 16) = 24 * v3 + (int32_t)&g29;
            EnterCriticalSection((struct _RTL_CRITICAL_SECTION *)&g36);
            return *(int32_t *)(v5 - 4);
        }
        if (v6 == 2) {
            // 0x406337
            g28 = 2;
            // 0x406311
            *(int32_t *)(v4 - 16) = 24 * v3 + (int32_t)&g29;
            EnterCriticalSection((struct _RTL_CRITICAL_SECTION *)&g36);
            return *(int32_t *)(v4 - 4);
        }
    }
    int32_t v12 = v4; // 0x406290
    int32_t v13; // 0x406290
    while (true) {
      lab_0x4062b9:
        // 0x4062b9
        v13 = v12;
        v5 = v13;
        switch (g28) {
            case 1: {
                // 0x4062a7
                *(int32_t *)(v13 - 16) = 1;
                Sleep((int32_t)&g36);
                v12 = v13 - 4;
                goto lab_0x4062b9;
            }
            case 2: {
                goto lab_0x406311;
            }
            default: {
                return *(int32_t *)v13;
            }
        }
    }
    // 0x4062c3
    return *(int32_t *)v13;
}

// Address range: 0x406390 - 0x4063ba
int32_t _dtoa_unlock(void) {
    // 0x406390
    int32_t result; // 0x406390
    if (g28 != 2) {
        // 0x40639c
        return result;
    }
    // 0x4063a0
    LeaveCriticalSection((struct _RTL_CRITICAL_SECTION *)(24 * result + (int32_t)&g29));
    return &g36;
}

// Address range: 0x4063c0 - 0x4063f4
int32_t ___Bfree_D2A(int32_t a1) {
    // 0x4063c0
    if (a1 == 0) {
        // 0x4063f0
        int32_t result; // 0x4063c0
        return result;
    }
    // 0x4063cc
    _dtoa_lock();
    int32_t v1 = *(int32_t *)(a1 + 4); // 0x4063d3
    int32_t * v2 = (int32_t *)(4 * v1 + (int32_t)&g30); // 0x4063d6
    *(int32_t *)a1 = *v2;
    *v2 = a1;
    return _dtoa_unlock();
}

// Address range: 0x406400 - 0x406496
int32_t ___Balloc_D2A(uint32_t a1) {
    // 0x406400
    _dtoa_lock();
    int32_t * v1 = (int32_t *)(4 * a1 + (int32_t)&g30); // 0x40640e
    int32_t result = *v1; // 0x40640e
    if (result != 0) {
        // 0x406419
        *v1 = *(int32_t *)result;
        // 0x406422
        _dtoa_unlock();
        *(int32_t *)(result + 16) = 0;
        *(int32_t *)(result + 12) = 0;
        return result;
    }
    int32_t v2 = 1 << a1 % 32;
    int32_t v3 = (int32_t)g6; // 0x406449
    uint32_t v4 = 4 * v2 + 27; // 0x406451
    int32_t size = v4 & -8;
    int32_t mem; // 0x406400
    if ((v3 - (int32_t)&g31) / 8 + v4 / 8 < 289) {
        // 0x40646d
        *(int32_t *)&g6 = size + v3;
        mem = v3;
    } else {
        // 0x40647f
        mem = (int32_t)malloc(size);
    }
    // 0x406477
    *(int32_t *)(mem + 4) = a1;
    *(int32_t *)(mem + 8) = v2;
    // 0x406422
    _dtoa_unlock();
    *(int32_t *)(mem + 16) = 0;
    *(int32_t *)(mem + 12) = 0;
    return mem;
}

// Address range: 0x4064a0 - 0x406605
int32_t ___diff_D2A(int32_t a1, int32_t a2) {
    int32_t v1 = ___cmp_D2A(a1, a2); // 0x4064b1
    if (v1 == 0) {
        int32_t result = ___Balloc_D2A(0); // 0x4065cf
        *(int32_t *)(result + 16) = 1;
        *(int32_t *)(result + 20) = 0;
        return result;
    }
    int32_t v2 = v1 >= 0 ? a1 : a2;
    int32_t v3 = v1 >= 0 ? a2 : a1;
    int32_t result2 = ___Balloc_D2A(*(int32_t *)(v2 + 4)); // 0x4064d0
    int32_t v4 = v3 + 20; // 0x4064d9
    *(int32_t *)(result2 + 12) = (int32_t)(v1 < 0);
    int32_t v5 = *(int32_t *)(v2 + 16); // 0x4064df
    int32_t v6 = v2 + 20; // 0x4064e6
    int32_t v7 = v4; // 0x40650f
    int32_t v8 = result2 + 20; // 0x40650f
    int32_t v9 = v6; // 0x40650f
    int32_t v10 = 0; // 0x40650f
    uint32_t v11 = *(int32_t *)v9; // 0x406516
    uint32_t v12 = *(int32_t *)v7; // 0x40651b
    uint32_t v13 = v11 - v12; // 0x40651f
    int32_t v14 = v13 - v10; // 0x406527
    v7 += 4;
    *(int32_t *)v8 = v14;
    v8 += 4;
    v9 += 4;
    v10 = ((int32_t)(v11 < v12) - (int32_t)(v13 < v10)) % 2;
    while (4 * *(int32_t *)(v3 + 16) + v4 > v7) {
        // 0x406512
        v11 = *(int32_t *)v9;
        v12 = *(int32_t *)v7;
        v13 = v11 - v12;
        v14 = v13 - v10;
        v7 += 4;
        *(int32_t *)v8 = v14;
        v8 += 4;
        v9 += 4;
        v10 = ((int32_t)(v11 < v12) - (int32_t)(v13 < v10)) % 2;
    }
    uint32_t v15 = 4 * v5 + v6; // 0x4064e9
    int32_t v16 = v8; // 0x40655f
    int32_t v17 = v9; // 0x40655f
    int32_t v18 = v10; // 0x40655f
    int32_t v19 = v14; // 0x40655f
    int32_t v20 = v8; // 0x40655f
    if (v9 < v15) {
        uint32_t v21 = *(int32_t *)v17; // 0x406561
        int32_t v22 = v21 - v18; // 0x406566
        v17 += 4;
        *(int32_t *)v16 = v22;
        v16 += 4;
        v18 = v21 < v18;
        v19 = v22;
        v20 = v16;
        while (v15 > v17) {
            // 0x406561
            v21 = *(int32_t *)v17;
            v22 = v21 - v18;
            v17 += 4;
            *(int32_t *)v16 = v22;
            v16 += 4;
            v18 = v21 < v18;
            v19 = v22;
            v20 = v16;
        }
    }
    int32_t v23 = v5; // 0x406598
    if (v19 != 0) {
        // 0x4065b3
        *(int32_t *)(result2 + 16) = v5;
        return result2;
    }
    int32_t v24 = v20;
    v23--;
    int32_t v25 = v24 - 4; // 0x4065b1
    while (*(int32_t *)(v24 - 8) == 0) {
        // 0x4065a0
        v24 = v25;
        v23--;
        v25 = v24 - 4;
    }
    // 0x4065b3
    *(int32_t *)(result2 + 16) = v23;
    return result2;
}

// Address range: 0x406610 - 0x406709
int32_t ___lshift_D2A(int32_t a1, uint32_t a2) {
    int32_t v1 = (int32_t)a2 / 32; // 0x406623
    int32_t v2 = *(int32_t *)(a1 + 4); // 0x406626
    int32_t * v3 = (int32_t *)(a1 + 16); // 0x406629
    int32_t v4 = v1 + 1 + *v3; // 0x40662d
    int32_t v5 = *(int32_t *)(a1 + 8); // 0x406630
    int32_t v6 = v5; // 0x406635
    int32_t v7 = v2; // 0x406635
    int32_t v8 = v2; // 0x406635
    if (v4 > v5) {
        v6 *= 2;
        v7++;
        v8 = v7;
        while (v4 > v6) {
            // 0x406637
            v6 *= 2;
            v7++;
            v8 = v7;
        }
    }
    int32_t result = ___Balloc_D2A(v8); // 0x406642
    int32_t v9 = result + 20; // 0x40664b
    int32_t v10 = v9; // 0x406657
    int32_t v11 = 0; // 0x406657
    int32_t v12 = v9; // 0x406657
    if (a2 >= 32) {
        v11++;
        *(int32_t *)v10 = 0;
        v10 += 4;
        while (v11 != v1) {
            // 0x406660
            v11++;
            *(int32_t *)v10 = 0;
            v10 += 4;
        }
        // 0x406674
        v12 = v9 + 4 * v1;
    }
    int32_t v13 = a1 + 20; // 0x406687
    uint32_t v14 = a2 % 32; // 0x40668a
    uint32_t v15 = 4 * *v3 + v13; // 0x406694
    int32_t v16 = v12; // 0x406698
    if (v14 == 0) {
        int32_t v17 = v13 + 4; // 0x4066f8
        *(int32_t *)v16 = *(int32_t *)v13;
        v16 += 4;
        while (v15 > v17) {
            int32_t v18 = v17;
            v17 = v18 + 4;
            *(int32_t *)v16 = *(int32_t *)v18;
            v16 += 4;
        }
        // 0x4066d0
        *(int32_t *)(result + 16) = v4 - 1;
        ___Bfree_D2A(a1);
        return result;
    }
    int32_t v19 = v12; // 0x4066a5
    int32_t v20 = v13; // 0x4066a5
    int32_t * v21 = (int32_t *)v20; // 0x4066a7
    *(int32_t *)v19 = *v21 << v14;
    v19 += 4;
    v20 += 4;
    int32_t v22 = *v21 >> -a2 % 32;
    while (v15 > v20) {
        // 0x4066a7
        v21 = (int32_t *)v20;
        *(int32_t *)v19 = *v21 << v14 | v22;
        v19 += 4;
        v20 += 4;
        v22 = *v21 >> -a2 % 32;
    }
    // 0x4066c9
    *(int32_t *)v19 = v22;
    // 0x4066d0
    *(int32_t *)(result + 16) = v4 + (int32_t)(v22 != 0) - 1;
    ___Bfree_D2A(a1);
    return result;
}

// Address range: 0x406710 - 0x406873
int32_t ___mult_D2A(int32_t a1, int32_t a2) {
    int32_t v1 = *(int32_t *)(a1 + 16); // 0x40671f
    int32_t v2 = *(int32_t *)(a2 + 16); // 0x406722
    int32_t v3 = v1 < v2 ? v1 : v2;
    int32_t v4 = v1 < v2 ? a2 : a1;
    int32_t v5 = v1 < v2 ? v2 : v1;
    int32_t v6 = v3 + v5; // 0x406735
    int32_t v7 = *(int32_t *)(v4 + 8); // 0x406738
    int32_t v8 = v6 - v7; // 0x406738
    int32_t v9 = *(int32_t *)(v4 + 4); // 0x406742
    int32_t result = ___Balloc_D2A(v9 + (int32_t)(v8 < 0 == ((v8 ^ v6) & (v6 ^ v7)) < 0 == (v8 != 0))); // 0x406750
    int32_t v10 = result + 20; // 0x40675b
    uint32_t v11 = v10 + 4 * v6; // 0x406762
    int32_t v12 = v10; // 0x40676f
    if (v10 < v11) {
        *(int32_t *)v12 = 0;
        v12 += 4;
        while (v11 > v12) {
            // 0x406773
            *(int32_t *)v12 = 0;
            v12 += 4;
        }
    }
    int32_t v13 = v4 + 20; // 0x40678a
    int32_t v14 = (v1 < v2 ? a1 : a2) + 20; // 0x40678e
    uint32_t v15 = v14 + 4 * v3; // 0x406791
    int32_t v16 = v14; // 0x40679f
    if (v14 < v15) {
        uint32_t v17 = *(int32_t *)v16; // 0x4067b0
        int32_t v18; // 0x406710
        uint64_t v19; // 0x4067e9
        int32_t * v20; // 0x4067f2
        uint32_t v21; // 0x4067f2
        uint32_t v22; // 0x4067f4
        uint32_t v23; // 0x4067f8
        int32_t v24; // 0x4067fc
        int32_t v25; // 0x406804
        int32_t v26; // 0x406811
        if (v17 != 0) {
            // 0x4067ba
            v25 = v13;
            v19 = (int64_t)*(int32_t *)v25 * (int64_t)v17;
            v20 = (int32_t *)v10;
            v21 = *v20;
            v22 = v21 + (int32_t)v19;
            v23 = v22;
            v24 = (int32_t)(v22 < v21) + (int32_t)(v19 / 0x100000000) + (int32_t)(v23 < v22);
            v25 += 4;
            *v20 = v23;
            v26 = v10 + 4;
            v18 = v26;
            while (v13 + 4 * v5 > v25) {
                // 0x4067e0
                v19 = (int64_t)*(int32_t *)v25 * (int64_t)v17;
                v20 = (int32_t *)v18;
                v21 = *v20;
                v22 = v21 + (int32_t)v19;
                v23 = v22 + v24;
                v24 = (int32_t)(v22 < v21) + (int32_t)(v19 / 0x100000000) + (int32_t)(v23 < v22);
                v25 += 4;
                *v20 = v23;
                v26 = v18 + 4;
                v18 = v26;
            }
            // 0x40681e
            *(int32_t *)v26 = v24;
        }
        // 0x406820
        v16 += 4;
        int32_t v27 = v10 + 4; // 0x406824
        while (v15 > v16) {
            int32_t v28 = v27;
            v17 = *(int32_t *)v16;
            if (v17 != 0) {
                // 0x4067ba
                v25 = v13;
                v19 = (int64_t)*(int32_t *)v25 * (int64_t)v17;
                v20 = (int32_t *)v28;
                v21 = *v20;
                v22 = v21 + (int32_t)v19;
                v23 = v22;
                v24 = (int32_t)(v22 < v21) + (int32_t)(v19 / 0x100000000) + (int32_t)(v23 < v22);
                v25 += 4;
                *v20 = v23;
                v26 = v28 + 4;
                v18 = v26;
                while (v13 + 4 * v5 > v25) {
                    // 0x4067e0
                    v19 = (int64_t)*(int32_t *)v25 * (int64_t)v17;
                    v20 = (int32_t *)v18;
                    v21 = *v20;
                    v22 = v21 + (int32_t)v19;
                    v23 = v22 + v24;
                    v24 = (int32_t)(v22 < v21) + (int32_t)(v19 / 0x100000000) + (int32_t)(v23 < v22);
                    v25 += 4;
                    *v20 = v23;
                    v26 = v18 + 4;
                    v18 = v26;
                }
                // 0x40681e
                *(int32_t *)v26 = v24;
            }
            // 0x406820
            v16 += 4;
            v27 = v28 + 4;
        }
    }
    // 0x406836
    if (v6 < 1) {
        // 0x406860
        *(int32_t *)(result + 16) = v6;
        return result;
    }
    int32_t v29 = v6; // 0x406847
    int32_t v30 = v11; // 0x406847
    int32_t v31 = v6; // 0x406847
    if (*(int32_t *)(v11 - 4) == 0) {
        v29--;
        v31 = v29;
        while (v29 != 0) {
            int32_t v32 = v30;
            v30 = v32 - 4;
            v31 = v29;
            if (*(int32_t *)(v32 - 8) != 0) {
                // break -> 0x406860
                break;
            }
            v29--;
            v31 = v29;
        }
    }
    // 0x406860
    *(int32_t *)(result + 16) = v31;
    return result;
}

// Address range: 0x406880 - 0x40689c
int32_t ___i2b_D2A(int32_t a1) {
    int32_t result = ___Balloc_D2A(1); // 0x406885
    *(int32_t *)(result + 20) = a1;
    *(int32_t *)(result + 16) = 1;
    return result;
}

// Address range: 0x4068a0 - 0x40698b
int32_t ___multadd_D2A(int32_t a1, int32_t a2, int32_t a3) {
    int32_t v1 = a1 + 20; // 0x4068b1
    int32_t * v2 = (int32_t *)(a1 + 16); // 0x4068b4
    uint32_t v3 = *v2; // 0x4068b4
    int32_t v4 = a2 >> 31; // 0x4068d2
    int32_t v5 = a2; // bp-44, 0x4068d5
    int32_t * v6 = (int32_t *)v1; // 0x4068e0
    uint32_t v7 = *v6; // 0x4068e0
    uint64_t v8 = (int64_t)v7 * (int64_t)a2; // 0x4068e9
    uint32_t v9 = (int32_t)v8; // 0x4068e9
    uint32_t v10 = v9 + a3; // 0x4068ec
    int32_t v11 = v7 * v4 + (a3 >> 31) + (int32_t)(v8 / 0x100000000) + (int32_t)(v10 < v9); // 0x4068f5
    *v6 = v10;
    int32_t v12 = 1; // 0x406911
    int32_t v13 = v1; // 0x406911
    int32_t v14 = v11; // 0x406911
    if (v3 > 1) {
        v13 += 4;
        int32_t * v15 = (int32_t *)v13; // 0x4068e0
        uint32_t v16 = *v15; // 0x4068e0
        uint64_t v17 = (int64_t)v16 * (int64_t)v5; // 0x4068e9
        uint32_t v18 = (int32_t)v17; // 0x4068e9
        uint32_t v19 = v11 + v18; // 0x4068ec
        int32_t v20 = v16 * v4 + (int32_t)(v17 / 0x100000000) + (int32_t)(v19 < v18); // 0x4068f5
        v12++;
        *v15 = v19;
        v14 = v20;
        while (v12 != v3) {
            // 0x4068e0
            v13 += 4;
            v15 = (int32_t *)v13;
            v16 = *v15;
            v17 = (int64_t)v16 * (int64_t)v5;
            v18 = (int32_t)v17;
            v19 = v20 + v18;
            v20 = v16 * v4 + (int32_t)(v17 / 0x100000000) + (int32_t)(v19 < v18);
            v12++;
            *v15 = v19;
            v14 = v20;
        }
    }
    int32_t v21 = &v5; // 0x4068a4
    if (v14 == 0) {
        // 0x40693e
        return *(int32_t *)(v21 + 48);
    }
    // 0x406917
    if (v3 >= *(int32_t *)(a1 + 8)) {
        int32_t v22 = ___Balloc_D2A(*(int32_t *)(a1 + 4) + 1); // 0x406956
        int32_t v23 = *v2; // 0x406962
        __asm_rep_movsd_memcpy((char *)(v22 + 12), (char *)(a1 + 12), (4 * v23 + 8) / 4);
        ___Bfree_D2A(a1);
    }
    int32_t v24 = *(int32_t *)(v21 + 12); // 0x406924
    int32_t * v25 = (int32_t *)(v21 + 48);
    int32_t v26 = *v25; // 0x406928
    *(int32_t *)(v26 + 20 + 4 * v24) = *(int32_t *)(v21 + 16);
    *(int32_t *)(v26 + 16) = v24 + 1;
    // 0x40693e
    return *v25;
}

// Address range: 0x406990 - 0x406a8d
int32_t ___pow5mult_D2A(int32_t a1, uint32_t a2) {
    int32_t v1 = (uint32_t)(a2 % 4); // 0x4069a1
    int32_t result = a1; // 0x4069a4
    if (v1 != 0) {
        // 0x406a18
        result = ___multadd_D2A(a1, *(int32_t *)(4 * v1 + (int32_t)&g13), 0);
    }
    // 0x4069a6
    if (a2 < 4) {
        // 0x4069f0
        return result;
    }
    int32_t v2 = result; // 0x4069b5
    int32_t v3 = g32; // 0x4069b5
    if (g32 == 0) {
        // 0x406a33
        _dtoa_lock();
        v3 = g32;
        if (g32 == 0) {
            // 0x406a6e
            v3 = ___i2b_D2A(625);
            g32 = v3;
            *(int32_t *)v3 = 0;
        }
        // 0x406a47
        v2 = _dtoa_unlock();
    }
    // 0x4069b7
    int32_t v4; // bp-28, 0x406990
    int32_t v5 = &v4; // 0x406994
    int32_t v6 = a2 / 4; // 0x4069a8
    int32_t v7 = v2; // 0x4069bd
    int32_t v8 = result; // 0x4069bd
    int32_t v9 = v3; // 0x4069bd
    int32_t v10 = v6; // 0x4069bd
    int32_t v11 = v2; // 0x4069bd
    int32_t v12 = result; // 0x4069bd
    int32_t v13 = v3; // 0x4069bd
    int32_t v14 = v6; // 0x4069bd
    if ((a2 & 4) != 0) {
        goto lab_0x4069d4;
    } else {
        goto lab_0x4069c0;
    }
  lab_0x4069d4:
    // 0x4069d4
    *(int32_t *)(v5 - 4) = v11;
    *(int32_t *)(v5 - 8) = v11;
    *(int32_t *)(v5 - 12) = v13;
    int32_t * v15 = (int32_t *)(v5 - 16); // 0x4069d7
    *v15 = v12;
    int32_t v16 = ___mult_D2A((int32_t)&g36, (int32_t)&g36); // 0x4069d8
    *v15 = v12;
    int32_t v17 = ___Bfree_D2A((int32_t)&g36); // 0x4069ee
    int32_t v18 = v16; // 0x4069ee
    int32_t v19 = v13; // 0x4069ee
    int32_t v20 = v14; // 0x4069ee
    int32_t result2 = v16; // 0x4069ee
    if (v14 < 2) {
        // 0x4069f0
        return result2;
    }
    goto lab_0x4069c4;
  lab_0x4069c0:
    // 0x4069c0
    v17 = v7;
    v18 = v8;
    v19 = v9;
    v20 = v10;
    result2 = v8;
    if (v10 < 2) {
        // 0x4069f0
        return result2;
    }
    goto lab_0x4069c4;
  lab_0x4069c4:;
    int32_t v21 = *(int32_t *)v19; // 0x4069c4
    if (v21 == 0) {
        int32_t v22 = _dtoa_lock(); // 0x4069ff
        int32_t * v23; // 0x4069c4
        int32_t v24 = *v23; // 0x406a04
        if (v24 == 0) {
            // 0x406a56
            *(int32_t *)(v5 - 4) = v22;
            *(int32_t *)(v5 - 8) = v22;
            int32_t v25; // 0x406990
            *(int32_t *)(v5 - 12) = v25;
            *(int32_t *)(v5 - 16) = v25;
            int32_t v26 = ___mult_D2A((int32_t)&g36, (int32_t)&g36); // 0x406a5a
            *v23 = v26;
            *(int32_t *)v26 = 0;
        }
        int32_t v27 = _dtoa_unlock(); // 0x406a16
    }
    int32_t v28 = v20 / 2;
    v7 = v17;
    v8 = v18;
    v9 = v21;
    v10 = v28;
    v11 = v17;
    v12 = v18;
    v13 = v21;
    v14 = v28;
    if (v28 % 2 == 0) {
        goto lab_0x4069c0;
    } else {
        goto lab_0x4069d4;
    }
}

// Address range: 0x406a90 - 0x406bc8
int32_t ___b2d_D2A(int32_t a1, int32_t a2) {
    int32_t v1 = a1 + 16; // 0x406a9b
    uint32_t v2 = a1 + 20; // 0x406a9e
    uint32_t v3 = 4 * *(int32_t *)v1 + v1; // 0x406aa1
    int32_t v4 = *(int32_t *)v3; // 0x406aa7
    uint32_t v5 = (v4 == 0 ? a1 : llvm_ctlz_i32(v4, true) ^ 31) ^ 31; // 0x406aac
    *(int32_t *)a2 = 32 - v5;
    if (v5 <= 10) {
        int32_t v6 = 0; // 0x406aed
        if (v2 < v3) {
            // 0x406ba7
            v6 = *(int32_t *)(v3 - 4) >> (11 - v5) % 32;
        }
        // 0x406af3
        return v6 | v4 << (v5 + 21) % 32;
    }
    int32_t v7; // 0x406a90
    int32_t v8; // 0x406a90
    int32_t v9; // 0x406a90
    if (v2 < v3) {
        int32_t v10 = v3 - 4; // 0x406b74
        int32_t result = *(int32_t *)v10; // 0x406b77
        int32_t v11 = v5 - 11; // 0x406b7a
        v9 = v11;
        v8 = result;
        v7 = v10;
        if (v11 == 0) {
            // 0x406b64
            return result;
        }
    } else {
        int32_t v12 = v5 - 11; // 0x406b20
        v9 = v12;
        v8 = 0;
        v7 = v3;
        if (v12 == 0) {
            // 0x406b64
            return 0;
        }
    }
    uint32_t v13 = v9 % 32; // 0x406b38
    int32_t result2; // 0x406a90
    if (v7 > v2) {
        uint32_t v14 = *(int32_t *)(v7 - 4); // 0x406bb1
        result2 = v14 >> -v9 % 32 | v8 << v13;
    } else {
        // 0x406b54
        result2 = v8 << v13;
    }
    // 0x406b64
    return result2;
}

// Address range: 0x406cd0 - 0x406d96
int32_t ___rshift_D2A(int32_t a1, uint32_t a2, int32_t a3, int32_t a4) {
    int32_t v1 = (int32_t)a2 / 32; // 0x406cdf
    int32_t v2 = a1 + 20; // 0x406ce2
    int32_t v3 = a1 + 16; // 0x406ceb
    int32_t * v4 = (int32_t *)v3; // 0x406ceb
    int32_t v5 = *v4; // 0x406ceb
    int32_t v6 = v2; // 0x406cf0
    if (v1 < v5) {
        uint32_t v7 = 4 * v5 + v2; // 0x406cf2
        int32_t v8 = 4 * v1; // 0x406cfa
        int32_t v9 = v8 + v3; // 0x406cfa
        uint32_t v10 = a2 % 32; // 0x406cfe
        if (v10 == 0) {
            int32_t v11 = v9 + 4;
            v6 = v2;
            if (v7 > v11) {
                *(int32_t *)v2 = *(int32_t *)v11;
                int32_t v12 = v2 + 4; // 0x406d88
                int32_t v13 = v11 + 4;
                v6 = v12;
                int32_t v14 = v13; // 0x406d8d
                int32_t v15 = v12; // 0x406d8d
                while (v7 > v13) {
                    // 0x406d81
                    *(int32_t *)v15 = *(int32_t *)v14;
                    v12 = v15 + 4;
                    v13 = v14 + 4;
                    v6 = v12;
                    v14 = v13;
                    v15 = v12;
                }
            }
        } else {
            int32_t v16 = v9 + 8; // 0x406d12
            int32_t v17 = *(int32_t *)(v8 + v2) >> v10; // 0x406d28
            int32_t v18 = v17; // 0x406d2c
            int32_t v19 = v2; // 0x406d2c
            if (v7 > v16) {
                int32_t v20 = v16; // 0x406d46
                int32_t v21 = v2; // 0x406d49
                int32_t * v22 = (int32_t *)v20; // 0x406d33
                *(int32_t *)v21 = *v22 << -a2 % 32 | v17;
                v20 += 4;
                v21 += 4;
                int32_t v23 = *v22 >> v10;
                v18 = v23;
                v19 = v21;
                while (v7 > v20) {
                    // 0x406d30
                    v22 = (int32_t *)v20;
                    *(int32_t *)v21 = *v22 << -a2 % 32 | v23;
                    v20 += 4;
                    v21 += 4;
                    v23 = *v22 >> v10;
                    v18 = v23;
                    v19 = v21;
                }
            }
            int32_t v24 = v19;
            *(int32_t *)v24 = v18;
            v6 = v18 != 0 ? v24 + 4 : v24;
        }
    }
    int32_t v25 = v6 - v2; // 0x406d66
    int32_t result = v25 / 4; // 0x406d68
    *v4 = result;
    if (v25 < 4) {
        // 0x406d72
        *(int32_t *)v2 = 0;
    }
    // 0x406d79
    return result;
}

// Address range: 0x406da0 - 0x406de2
int32_t ___trailz_D2A(int32_t a1) {
    int32_t v1 = a1 + 20; // 0x406dab
    uint32_t v2 = 4 * *(int32_t *)(a1 + 16) + v1; // 0x406dae
    if (v1 >= v2) {
        // 0x406dd0
        return 0;
    }
    int32_t v3 = *(int32_t *)v1; // 0x406db6
    int32_t v4 = v1; // 0x406dbb
    int32_t result = 0; // 0x406dbb
    int32_t v5; // 0x406da0
    if (v3 != 0) {
        // 0x406dd8
        v5 = v3;
        return v5 == 0 ? 0 : llvm_cttz_i32(v5, true);
    }
    v4 += 4;
    result += 32;
    while (v2 > v4) {
        int32_t v6 = *(int32_t *)v4; // 0x406dc0
        int32_t v7 = v6; // 0x406dc4
        int32_t v8 = result; // 0x406dc4
        if (v6 != 0) {
            // 0x406dd8
            v5 = v7;
            return (v5 == 0 ? 0 : llvm_cttz_i32(v5, true)) + v8;
        }
        v4 += 4;
        result += 32;
    }
    // 0x406dd0
    return result;
}

// Address range: 0x406e78 - 0x406e7b
int32_t __get_output_format(void) {
    // 0x406e78
    return 0;
}

// Address range: 0x406f0c - 0x406f39
int32_t ___chkstk(int32_t * a1) {
    // 0x406f0c
    return (int32_t)a1;
}

// Address range: 0x406f40 - 0x4070be
int32_t ___umoddi3(uint32_t result2, uint32_t a2, uint32_t a3, uint32_t a4) {
    if (a4 == 0) {
        int32_t result; // 0x406f40
        if (a3 > a2) {
            // 0x406f7f
            result = (uint64_t)(0x100000000 * (int64_t)a2 | (int64_t)result2) % (uint64_t)(int64_t)a3;
        } else {
            uint64_t v1 = (int64_t)(a3 != 0 ? a3 : (int32_t)(bool)(a3 == 1)); // 0x407010
            result = (0x100000000 * ((0x100000000 * (int64_t)a4 | (int64_t)a2) % v1) | (int64_t)result2) % v1;
        }
        // 0x406f83
        return result;
    }
    // 0x406fa0
    if (a4 > a2) {
        // 0x406f83
        return result2;
    }
    uint32_t v2 = llvm_ctlz_i32(a4, true); // 0x406fc0
    if (v2 == 0) {
        // 0x406f83
        return result2 - (a2 > a4 | result2 >= a3 ? a3 : 0);
    }
    uint32_t v3 = -v2 % 32; // 0x407039
    uint32_t v4 = a3 >> v3 | a4 << v2; // 0x407043
    uint32_t v5 = a3 << v2;
    uint64_t v6 = (int64_t)v4;
    uint32_t v7 = result2 << v2;
    uint64_t v8 = 0x100000000 * (int64_t)(a2 >> v3) | (int64_t)(result2 >> v3 | a2 << v2); // 0x407064
    uint32_t v9 = (int32_t)(v8 % v6); // 0x407064
    uint64_t v10 = (v8 / v6 & 0xffffffff) * (int64_t)v5; // 0x407069
    uint32_t v11 = (int32_t)v10; // 0x407069
    int32_t v12 = v10 / 0x100000000; // 0x407069
    uint32_t v13; // 0x406f40
    if (v9 >= v12) {
        // 0x40706f
        if (v7 < v11 != (v12 == v9)) {
            // 0x407073
            v13 = v11;
            // 0x406f83
            return v9 - v12 + (int32_t)(v7 < v13) << v3 | v7 - v13 >> v2;
        }
    }
    int32_t v14 = v12 - v4 + (int32_t)(v5 > v11);
    v13 = v11 - v5;
    // 0x406f83
    return v9 - v14 + (int32_t)(v7 < v13) << v3 | v7 - v13 >> v2;
}

// Address range: 0x4070c0 - 0x4071f2
int32_t ___udivdi3(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    if (a4 == 0) {
        int32_t result; // 0x4070c0
        if (a3 > a2) {
            // 0x4070f9
            result = (uint64_t)(0x100000000 * (int64_t)a2 | (int64_t)a1) / (uint64_t)(int64_t)a3;
        } else {
            uint64_t v1 = (int64_t)(a3 != 0 ? a3 : (int32_t)(bool)(a3 == 1)); // 0x40719e
            result = (0x100000000 * ((0x100000000 * (int64_t)a4 | (int64_t)a2) % v1) | (int64_t)a1) / v1;
        }
        // 0x407101
        return result;
    }
    // 0x407115
    if (a4 > a2) {
        // 0x407101
        return 0;
    }
    uint32_t v2 = llvm_ctlz_i32(a4, true); // 0x40711d
    if (v2 == 0) {
        // 0x4071b0
        return a2 > a4 | a1 >= a3;
    }
    uint32_t v3 = -v2 % 32; // 0x407144
    uint64_t v4 = 0x100000000 * (int64_t)(a2 >> v3) | (int64_t)(a1 >> v3 | a2 << v2); // 0x40716c
    uint64_t v5 = (int64_t)(a3 >> v3 | a4 << v2); // 0x40716c
    uint64_t v6 = v4 / v5; // 0x40716c
    int32_t result2 = v6; // 0x40716c
    uint32_t v7 = (int32_t)(v4 % v5); // 0x40716c
    uint64_t v8 = (v6 & 0xffffffff) * (int64_t)(a3 << v2); // 0x407172
    uint32_t v9 = (int32_t)(v8 / 0x100000000); // 0x407172
    if (v9 > v7 || a1 << v2 < (int32_t)v8 == v9 == v7) {
        // 0x407101
        return result2 - 1;
    }
    // 0x407101
    return result2;
}

// Address range: 0x407200 - 0x407206
int32_t _qb_blank_flash(int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5) {
    // 0x407200
    return qb_blank_flash();
}

// Address range: 0x407208 - 0x40720e
int32_t _qb_describe_error(int32_t a1) {
    // 0x407208
    return qb_describe_error();
}

// Address range: 0x407210 - 0x407216
int32_t _qb_get_version(int32_t * a1, int32_t * a2) {
    // 0x407210
    return qb_get_version();
}

// --------------- Dynamically Linked Functions ---------------

// int * __cdecl _errno(void);
// int __cdecl _strnicmp(char const * String1, char const * String2, _In_ size_t MaxCount);
// void abort(void);
// int atexit(void(* func)(void));
// VOID EnterCriticalSection(_Inout_ LPCRITICAL_SECTION lpCriticalSection);
// int fprintf(FILE * restrict stream, const char * restrict format, ...);
// int fputc(int c, FILE * stream);
// size_t fwrite(const void * restrict ptr, size_t size, size_t n, FILE * restrict s);
// char * getenv(const char * name);
// HMODULE GetModuleHandleA(_In_opt_ LPCSTR lpModuleName);
// FARPROC GetProcAddress(_In_ HMODULE hModule, _In_ LPCSTR lpProcName);
// VOID InitializeCriticalSection(_Out_ LPCRITICAL_SECTION lpCriticalSection);
// unsigned InterlockedExchange(_Inout_ unsigned volatile * Target, _In_ unsigned Value);
// BOOL IsDBCSLeadByteEx(_In_ UINT CodePage, _In_ BYTE TestChar);
// VOID LeaveCriticalSection(_Inout_ LPCRITICAL_SECTION lpCriticalSection);
// struct lconv * localeconv(void);
// void * malloc(size_t size);
// void * memset(void * s, int c, size_t n);
// int MultiByteToWideChar(_In_ UINT CodePage, _In_ DWORD dwFlags, LPCCH lpMultiByteStr, _In_ int cbMultiByte, LPWSTR lpWideCharStr, _In_ int cchWideChar);
// int32_t qb_blank_flash(void);
// int32_t qb_describe_error(void);
// int32_t qb_get_version(void);
// LSTATUS RegCloseKey(_In_ HKEY hKey);
// LSTATUS RegQueryValueExA(_In_ HKEY hKey, _In_opt_ LPCSTR lpValueName, LPDWORD lpReserved, _Out_opt_ LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
// BOOL SetupDiClassGuidsFromNameA(_In_ PCSTR ClassName, LPGUID ClassGuidList, _In_ DWORD ClassGuidListSize, _Out_ PDWORD RequiredSize);
// BOOL SetupDiDestroyDeviceInfoList(_In_ HDEVINFO DeviceInfoSet);
// BOOL SetupDiEnumDeviceInfo(_In_ HDEVINFO DeviceInfoSet, _In_ DWORD MemberIndex, _Out_ PSP_DEVINFO_DATA DeviceInfoData);
// HDEVINFO SetupDiGetClassDevsA(_In_opt_ const GUID * ClassGuid, _In_opt_ PCSTR Enumerator, _In_opt_ HWND hwndParent, _In_ DWORD Flags);
// BOOL SetupDiGetDeviceRegistryPropertyA(_In_ HDEVINFO DeviceInfoSet, _In_ PSP_DEVINFO_DATA DeviceInfoData, _In_ DWORD Property, _Out_opt_ PDWORD PropertyRegDataType, PBYTE PropertyBuffer, _In_ DWORD PropertyBufferSize, _Out_opt_ PDWORD RequiredSize);
// HKEY SetupDiOpenDevRegKey(_In_ HDEVINFO DeviceInfoSet, _In_ PSP_DEVINFO_DATA DeviceInfoData, _In_ DWORD Scope, _In_ DWORD HwProfile, _In_ DWORD KeyType, _In_ REGSAM samDesired);
// int setvbuf(FILE * restrict stream, char * restrict buf, int modes, size_t n);
// VOID Sleep(_In_ DWORD dwMilliseconds);
// int strcmp(const char * s1, const char * s2);
// size_t strlen(const char * s);
// unsigned long int strtoul(const char * restrict nptr, char ** restrict endptr, int base);
// int tolower(int c);
// size_t wcslen(const wchar_t * s);
// int WideCharToMultiByte(_In_ UINT CodePage, _In_ DWORD dwFlags, LPCWCH lpWideCharStr, _In_ int cchWideChar, LPSTR lpMultiByteStr, _In_ int cbMultiByte, _In_opt_ LPCCH lpDefaultChar, _Out_opt_ LPBOOL lpUsedDefaultChar);

// --------------------- Meta-Information ---------------------

// Detected compiler/packer: gcc (4.2.1)
// Detected language: C
// Detected functions: 64
