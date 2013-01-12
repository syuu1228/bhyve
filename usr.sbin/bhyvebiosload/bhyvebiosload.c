/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: soc2012/syuu/bhyve-bios/usr.sbin/bhyveload/bhyveload.c 234984 2012-04-26 07:52:28Z grehan $
 */

/*-
 * Copyright (c) 2011 Google, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: soc2012/syuu/bhyve-bios/usr.sbin/bhyveload/bhyveload.c 234984 2012-04-26 07:52:28Z grehan $
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: soc2012/syuu/bhyve-bios/usr.sbin/bhyveload/bhyveload.c 234984 2012-04-26 07:52:28Z grehan $");

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <machine/specialreg.h>
#include <machine/vmm.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <vmmapi.h>

#define	MB	(1024 * 1024UL)
#define	GB	(1024 * 1024 * 1024UL)
#define	BSP	0

static struct termios term, oldterm;
#if 0
static int disk_fd = -1;
#endif

static char *vmname, *progname, *membase;
static uint64_t lowmem, highmem;
static struct vmctx *ctx;

static void
usage(void)
{

	printf("usage: %s [-d <disk image path>] [-h <host filesystem path>] "
	       "[-m <lowmem>][-M <highmem>] "
	       "<vmname>\n", progname);
	exit(1);
}

int
main(int argc, char** argv)
{
	int opt, error;
#if 0
	char *disk_image;
	int i, addr;
#endif

	progname = argv[0];

	lowmem = 768 * MB;
	highmem = 0;
#if 0
	disk_image = NULL;
#endif

	while ((opt = getopt(argc, argv, "d:m:M:")) != -1) {
		switch (opt) {
#if 0
		case 'd':
			disk_image = optarg;
			break;
#endif
		case 'm':
			lowmem = strtoul(optarg, NULL, 0) * MB;
			break;
		
		case 'M':
			highmem = strtoul(optarg, NULL, 0) * MB;
			break;

		case '?':
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

#if 0
	if (!disk_image) {
		printf("disk image is required.\n");
		exit(1);
	}
#endif

	vmname = argv[0];

	error = vm_create(vmname);
	if (error != 0 && errno != EEXIST) {
		perror("vm_create");
		exit(1);

	}

	ctx = vm_open(vmname);
	if (ctx == NULL) {
		perror("vm_open");
		exit(1);
	}

	error = vm_setup_memory(ctx, 0, lowmem, &membase);
	if (error) {
		perror("vm_setup_memory(lowmem)");
		exit(1);
	}

	if (highmem != 0) {
		error = vm_setup_memory(ctx, 4 * GB, highmem, NULL);
		if (error) {
			perror("vm_setup_memory(highmem)");
			exit(1);
		}
	}

	tcgetattr(0, &term);
	oldterm = term;
	term.c_lflag &= ~(ICANON|ECHO);
	term.c_iflag &= ~ICRNL;
	tcsetattr(0, TCSAFLUSH, &term);

#if 0
	disk_fd = open(disk_image, O_RDONLY);
	if (read(disk_fd, &membase[0x7c00], 512) != 512) {
		perror("read ");
		return (1);
	}
	close(disk_fd);

	addr = 0x400;
	for (i = 0x0; i < 0x400; i += 0x4) {
		uint16_t *p = (uint16_t *)&membase[i];
#if 1 /* XXX: need to detect CPU vendor */
		membase[addr + 0] = 0x0f;	/* vmcall(3byte) */
		membase[addr + 1] = 0x01;
		membase[addr + 2] = 0xc1;
#else /* for AMD-V */
		membase[addr + 0] = 0x0f;	/* vmmcall(3byte) */
		membase[addr + 1] = 0x01;
		membase[addr + 2] = 0xd9;
#endif
		membase[addr + 3] = 0xcf;	/* iret */
		*p = addr;
		p = (uint16_t *)&membase[i + 0x2];
		*p = 0x0;
		addr += 4;
	}
#endif
	error = vm_setup_bios_registers(ctx, BSP);
	if (error) {
		perror("vm_setup_freebsd_registers");
		return (-1);
	}

	tcsetattr(0, TCSAFLUSH, &oldterm);

	return (0);
}
