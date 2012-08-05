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
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>

#include <machine/segments.h>

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include <machine/vmm.h>
#include <vmmapi.h>

#include "fbsdrun.h"
#include "inout.h"
#include "dbgport.h"
#include "mevent.h"
#include "pci_emul.h"
#include "xmsr.h"
#include "instruction_emul.h"
#include "ioapic.h"

#define	DEFAULT_GUEST_HZ	100
#define	DEFAULT_GUEST_TSLICE	200

#define GUEST_NIO_PORT		0x488	/* guest upcalls via i/o port */

#define	VMEXIT_SWITCH		0	/* force vcpu switch in mux mode */
#define	VMEXIT_CONTINUE		1	/* continue from next instruction */
#define	VMEXIT_RESTART		2	/* restart current instruction */
#define	VMEXIT_ABORT		3	/* abort the vm run loop */
#define	VMEXIT_RESET		4	/* guest machine has reset */

#define MB		(1024UL * 1024)
#define GB		(1024UL * MB)

typedef int (*vmexit_handler_t)(struct vmctx *, struct vm_exit *, int *vcpu);

int guest_tslice = DEFAULT_GUEST_TSLICE;
int guest_hz = DEFAULT_GUEST_HZ;
char *vmname;

u_long lomem_sz;
u_long himem_sz;

int guest_ncpus;

static int pincpu = -1;
static int guest_vcpu_mux;
static int guest_vmexit_on_hlt, guest_vmexit_on_pause;

static int foundcpus;

static int strictio;

static char *lomem_addr;
static char *himem_addr;

static char *progname;
static const int BSP = 0;

static int cpumask;

static void *oem_tbl_start;
static int oem_tbl_size;

static void vm_loop(struct vmctx *ctx, int vcpu, uint64_t rip);

struct vm_exit vmexit[VM_MAXCPU];

struct fbsdstats {
        uint64_t        vmexit_bogus;
        uint64_t        vmexit_bogus_switch;
        uint64_t        vmexit_hlt;
        uint64_t        vmexit_pause;
        uint64_t        vmexit_mtrap;
        uint64_t        vmexit_paging;
        uint64_t        cpu_switch_rotate;
        uint64_t        cpu_switch_direct;
        int             io_reset;
} stats;

struct mt_vmm_info {
	pthread_t	mt_thr;
	struct vmctx	*mt_ctx;
	int		mt_vcpu;	
} mt_vmm_info[VM_MAXCPU];

static void
usage(int code)
{

        fprintf(stderr,
                "Usage: %s [-ehBHIP][-g <gdb port>][-z <hz>][-s <pci>]"
		"[-S <pci>][-p pincpu][-n <pci>][-m lowmem][-M highmem] <vm>\n"
		"       -g: gdb port (default is %d and 0 means don't open)\n"
		"       -c: # cpus (default 1)\n"
		"       -p: pin vcpu 'n' to host cpu 'pincpu + n'\n"
		"       -B: inject breakpoint exception on vm entry\n"
		"       -H: vmexit from the guest on hlt\n"
		"       -I: present an ioapic to the guest\n"
		"       -P: vmexit from the guest on pause\n"
		"	-e: exit on unhandled i/o access\n"
		"       -h: help\n"
		"       -z: guest hz (default is %d)\n"
		"       -s: <slot,driver,configinfo> PCI slot config\n"
		"       -S: <slot,driver,configinfo> legacy PCI slot config\n"
		"	-n: <slot,name> PCI slot naming\n"
		"       -m: lowmem in MB\n"
		"       -M: highmem in MB\n"
		"       -x: mux vcpus to 1 hcpu\n"
		"       -t: mux vcpu timeslice hz (default %d)\n",
		progname, DEFAULT_GDB_PORT, DEFAULT_GUEST_HZ,
		DEFAULT_GUEST_TSLICE);
	exit(code);
}

void *
paddr_guest2host(uintptr_t gaddr)
{
	if (lomem_sz == 0)
		return (NULL);

	if (gaddr < lomem_sz) {
		return ((void *)(lomem_addr + gaddr));
	} else if (gaddr >= 4*GB && gaddr < (4*GB + himem_sz)) {
		return ((void *)(himem_addr + gaddr - 4*GB));
	} else
		return (NULL);
}

void
fbsdrun_add_oemtbl(void *tbl, int tblsz)
{
	oem_tbl_start = tbl;
	oem_tbl_size = tblsz;
}

int
fbsdrun_vmexit_on_pause(void)
{

	return (guest_vmexit_on_pause);
}

int
fbsdrun_vmexit_on_hlt(void)
{

	return (guest_vmexit_on_hlt);
}

int
fbsdrun_muxed(void)
{

	return (guest_vcpu_mux);
}

static void *
fbsdrun_start_thread(void *param)
{
	int vcpu;
	struct mt_vmm_info *mtp = param;

	vcpu = mtp->mt_vcpu;
	vm_loop(mtp->mt_ctx, vcpu, vmexit[vcpu].rip);

	/* not reached */
	exit(1);
	return (NULL);
}

void
fbsdrun_addcpu(struct vmctx *ctx, int vcpu, uint64_t rip)
{
	int error;

	if (cpumask & (1 << vcpu)) {
		printf("addcpu: attempting to add existing cpu %d\n", vcpu);
		exit(1);
	}

	cpumask |= 1 << vcpu;
	foundcpus++;

	/*
	 * Set up the vmexit struct to allow execution to start
	 * at the given RIP
	 */
	vmexit[vcpu].rip = rip;
	vmexit[vcpu].inst_length = 0;

	if (vcpu == BSP || !guest_vcpu_mux){
		mt_vmm_info[vcpu].mt_ctx = ctx;
		mt_vmm_info[vcpu].mt_vcpu = vcpu;
	
		error = pthread_create(&mt_vmm_info[vcpu].mt_thr, NULL,
				fbsdrun_start_thread, &mt_vmm_info[vcpu]);
		assert(error == 0);
	}
}

static int
fbsdrun_get_next_cpu(int curcpu)
{

	/*
	 * Get the next available CPU. Assumes they arrive
	 * in ascending order with no gaps.
	 */
	return ((curcpu + 1) % foundcpus);
}

static int
vmexit_catch_reset(void)
{
        stats.io_reset++;
        return (VMEXIT_RESET);
}

static int
vmexit_catch_inout(void)
{
	return (VMEXIT_ABORT);
}

static int
vmexit_handle_notify(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu,
		     uint32_t eax)
{
#if PG_DEBUG /* put all types of debug here */
        if (eax == 0) {
		pause_noswitch = 1;
	} else if (eax == 1) {
		pause_noswitch = 0;
	} else {
		pause_noswitch = 0;
		if (eax == 5) {
			vm_set_capability(ctx, *pvcpu, VM_CAP_MTRAP_EXIT, 1);
		}
	}
#endif
        return (VMEXIT_CONTINUE);
}

static int
vmexit_inout(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	int error;
	int bytes, port, in, out;
	uint32_t eax;
	int vcpu;

	vcpu = *pvcpu;

	port = vme->u.inout.port;
	bytes = vme->u.inout.bytes;
	eax = vme->u.inout.eax;
	in = vme->u.inout.in;
	out = !in;

	/* We don't deal with these */
	if (vme->u.inout.string || vme->u.inout.rep)
		return (VMEXIT_ABORT);

	/* Special case of guest reset */
	if (out && port == 0x64 && (uint8_t)eax == 0xFE)
		return (vmexit_catch_reset());

        /* Extra-special case of host notifications */
        if (out && port == GUEST_NIO_PORT)
                return (vmexit_handle_notify(ctx, vme, pvcpu, eax));

	error = emulate_inout(ctx, vcpu, in, port, bytes, &eax, strictio);
	if (error == 0 && in)
		error = vm_set_register(ctx, vcpu, VM_REG_GUEST_RAX, eax);

	if (error == 0)
		return (VMEXIT_CONTINUE);
	else {
		fprintf(stderr, "Unhandled %s%c 0x%04x\n",
			in ? "in" : "out",
			bytes == 1 ? 'b' : (bytes == 2 ? 'w' : 'l'), port);
		return (vmexit_catch_inout());
	}
}

static int
vmexit_rdmsr(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	printf("vm exit rdmsr 0x%x, cpu %d\n", vme->u.msr.code, *pvcpu);
	return (VMEXIT_ABORT);
}

static int
vmexit_wrmsr(struct vmctx *ctx, struct vm_exit *vme, int *pvcpu)
{
	int newcpu;
	int retval = VMEXIT_CONTINUE;

	newcpu = emulate_wrmsr(ctx, *pvcpu, vme->u.msr.code,vme->u.msr.wval);

	if (guest_vcpu_mux && *pvcpu != newcpu) {
                retval = VMEXIT_SWITCH;
                *pvcpu = newcpu;
        }
        
        return (retval);
}

static int
vmexit_vmx(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	printf("vm exit[%d]\n", *pvcpu);
	printf("\treason\t\tVMX\n");
	printf("\trip\t\t0x%016lx\n", vmexit->rip);
	printf("\tinst_length\t%d\n", vmexit->inst_length);
	printf("\terror\t\t%d\n", vmexit->u.vmx.error);
	printf("\texit_reason\t%u\n", vmexit->u.vmx.exit_reason);
	printf("\tqualification\t0x%016lx\n", vmexit->u.vmx.exit_qualification);

	return (VMEXIT_ABORT);
}

static int bogus_noswitch = 1;

static int
vmexit_bogus(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	stats.vmexit_bogus++;

	if (!guest_vcpu_mux || guest_ncpus == 1 || bogus_noswitch) {
		return (VMEXIT_RESTART);
	} else {
		stats.vmexit_bogus_switch++;
		vmexit->inst_length = 0;
		*pvcpu = -1;		
		return (VMEXIT_SWITCH);
	}
}

static int
vmexit_hlt(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	stats.vmexit_hlt++;
	if (fbsdrun_muxed()) {
		*pvcpu = -1;
		return (VMEXIT_SWITCH);
	} else {
		/*
		 * Just continue execution with the next instruction. We use
		 * the HLT VM exit as a way to be friendly with the host
		 * scheduler.
		 */
		return (VMEXIT_CONTINUE);
	}
}

static int pause_noswitch;

static int
vmexit_pause(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	stats.vmexit_pause++;

	if (fbsdrun_muxed() && !pause_noswitch) {
		*pvcpu = -1;
		return (VMEXIT_SWITCH);
        } else {
		return (VMEXIT_CONTINUE);
	}
}

static int
vmexit_mtrap(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{
	stats.vmexit_mtrap++;

	return (VMEXIT_RESTART);
}

static int
vmexit_paging(struct vmctx *ctx, struct vm_exit *vmexit, int *pvcpu)
{

	stats.vmexit_paging++;

	if (emulate_instruction(ctx, *pvcpu, vmexit->rip, vmexit->u.paging.cr3) != 0) {
		printf("Failed to emulate instruction at 0x%lx\n", vmexit->rip);
		return (VMEXIT_ABORT);
	}

	return (VMEXIT_CONTINUE);
}

static void
sigalrm(int sig)
{
	return;
}

static void
setup_timeslice(void)
{
	struct sigaction sa;
	struct itimerval itv;
	int error;

	/*
	 * Setup a realtime timer to generate a SIGALRM at a
	 * frequency of 'guest_tslice' ticks per second.
	 */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = sigalrm;
	
	error = sigaction(SIGALRM, &sa, NULL);
	assert(error == 0);

	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000000 / guest_tslice;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 1000000 / guest_tslice;
	
	error = setitimer(ITIMER_REAL, &itv, NULL);
	assert(error == 0);
}

static vmexit_handler_t handler[VM_EXITCODE_MAX] = {
	[VM_EXITCODE_INOUT]  = vmexit_inout,
	[VM_EXITCODE_VMX]    = vmexit_vmx,
	[VM_EXITCODE_BOGUS]  = vmexit_bogus,
	[VM_EXITCODE_RDMSR]  = vmexit_rdmsr,
	[VM_EXITCODE_WRMSR]  = vmexit_wrmsr,
	[VM_EXITCODE_MTRAP]  = vmexit_mtrap,
	[VM_EXITCODE_PAGING] = vmexit_paging
};

static void
vm_loop(struct vmctx *ctx, int vcpu, uint64_t rip)
{
	int error, rc, prevcpu;

	if (guest_vcpu_mux)
		setup_timeslice();

	if (pincpu >= 0) {
		error = vm_set_pinning(ctx, vcpu, pincpu + vcpu);
		assert(error == 0);
	}

	while (1) {
		error = vm_run(ctx, vcpu, rip, &vmexit[vcpu]);
		if (error != 0)
			break;

		prevcpu = vcpu;
                rc = (*handler[vmexit[vcpu].exitcode])(ctx, &vmexit[vcpu],
                                                       &vcpu);		
		switch (rc) {
                case VMEXIT_SWITCH:
			assert(guest_vcpu_mux);
			if (vcpu == -1) {
				stats.cpu_switch_rotate++;
				vcpu = fbsdrun_get_next_cpu(prevcpu);
			} else {
				stats.cpu_switch_direct++;
			}
			/* fall through */
		case VMEXIT_CONTINUE:
                        rip = vmexit[vcpu].rip + vmexit[vcpu].inst_length;
			break;
		case VMEXIT_RESTART:
                        rip = vmexit[vcpu].rip;
			break;
		case VMEXIT_RESET:
			exit(0);
		default:
			exit(1);
		}
	}
	fprintf(stderr, "vm_run error %d, errno %d\n", error, errno);
}


int
main(int argc, char *argv[])
{
	int c, error, gdb_port, inject_bkpt, tmp, err, ioapic;
	struct vmctx *ctx;
	uint64_t rip;

	inject_bkpt = 0;
	progname = basename(argv[0]);
	gdb_port = DEFAULT_GDB_PORT;
	guest_ncpus = 1;
	ioapic = 0;

	while ((c = getopt(argc, argv, "ehBHIPxp:g:c:z:s:S:n:m:M:")) != -1) {
		switch (c) {
		case 'B':
			inject_bkpt = 1;
			break;
		case 'x':
			guest_vcpu_mux = 1;
			break;
		case 'p':
			pincpu = atoi(optarg);
			break;
                case 'c':
			guest_ncpus = atoi(optarg);
			break;
		case 'g':
			gdb_port = atoi(optarg);
			break;
		case 'z':
			guest_hz = atoi(optarg);
			break;
		case 't':
			guest_tslice = atoi(optarg);
			break;
		case 's':
			pci_parse_slot(optarg, 0);
			break;
		case 'S':
			pci_parse_slot(optarg, 1);
			break;
		case 'n':
			pci_parse_name(optarg);
			break;
                case 'm':
			lomem_sz = strtoul(optarg, NULL, 0) * MB;
			break;
                case 'M':
			himem_sz = strtoul(optarg, NULL, 0) * MB;
			break;
		case 'H':
			guest_vmexit_on_hlt = 1;
			break;
		case 'I':
			ioapic = 1;
			break;
		case 'P':
			guest_vmexit_on_pause = 1;
			break;
		case 'e':
			strictio = 1;
			break;
		case 'h':
			usage(0);			
		default:
			usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage(1);

	/* No need to mux if guest is uni-processor */
	if (guest_ncpus <= 1)
		guest_vcpu_mux = 0;

	/* vmexit on hlt if guest is muxed */
	if (guest_vcpu_mux) {
		guest_vmexit_on_hlt = 1;
		guest_vmexit_on_pause = 1;
	}

	vmname = argv[0];

	ctx = vm_open(vmname);
	if (ctx == NULL) {
		perror("vm_open");
		exit(1);
	}

	if (fbsdrun_vmexit_on_hlt()) {
		err = vm_get_capability(ctx, BSP, VM_CAP_HALT_EXIT, &tmp);
		if (err < 0) {
			printf("VM exit on HLT not supported\n");
			exit(1);
		}
		vm_set_capability(ctx, BSP, VM_CAP_HALT_EXIT, 1);
		handler[VM_EXITCODE_HLT] = vmexit_hlt;
	}

        if (fbsdrun_vmexit_on_pause()) {
		/*
		 * pause exit support required for this mode
		 */
		err = vm_get_capability(ctx, BSP, VM_CAP_PAUSE_EXIT, &tmp);
		if (err < 0) {
			printf("SMP mux requested, no pause support\n");
			exit(1);
		}
		vm_set_capability(ctx, BSP, VM_CAP_PAUSE_EXIT, 1);
		handler[VM_EXITCODE_PAUSE] = vmexit_pause;
        }

	if (lomem_sz != 0) {
		lomem_addr = vm_map_memory(ctx, 0, lomem_sz);
		if (lomem_addr == (char *) MAP_FAILED) {
			lomem_sz = 0;
		} else if (himem_sz != 0) {
			himem_addr = vm_map_memory(ctx, 4*GB, himem_sz);
			if (himem_addr == (char *) MAP_FAILED) {
				lomem_sz = 0;
				himem_sz = 0;
			}
		}
	}

	init_inout();
	init_pci(ctx);
	if (ioapic)
		ioapic_init(0);

	if (gdb_port != 0)
		init_dbgport(gdb_port);

	error = vm_get_register(ctx, BSP, VM_REG_GUEST_RIP, &rip);
	assert(error == 0);

	if (inject_bkpt) {
		error = vm_inject_event(ctx, BSP, VM_HW_EXCEPTION, IDT_BP);
		assert(error == 0);
	}

	/*
	 * build the guest tables, MP etc.
	 */
	vm_build_tables(ctx, guest_ncpus, ioapic, oem_tbl_start, oem_tbl_size);

	/*
	 * Add CPU 0
	 */
	fbsdrun_addcpu(ctx, BSP, rip);

	/*
	 * Head off to the main event dispatch loop
	 */
	mevent_dispatch();

	exit(1);
}
