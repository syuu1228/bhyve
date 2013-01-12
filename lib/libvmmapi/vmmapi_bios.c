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

#include <machine/specialreg.h>
#include <machine/segments.h>
#include <machine/vmm.h>

#include "vmmapi.h"

/*
 * Setup the 'vcpu' register set such that it will begin execution at
 * 'rip' in long mode.
 */
int
vm_setup_bios_registers(struct vmctx *vmctx, int vcpu)
{
	int error;
	uint64_t rip, cr0, cr3, cr4, efer, rflags, rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp, rsp, desc_base;
	uint32_t desc_access, desc_limit;
	uint16_t gsel;

	rip = 0x7c00;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RIP, rip)) != 0)
		goto done;

	rflags = 0x2;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RFLAGS, rflags))	!= 0)
		goto done;

	cr0 = 0x60000010;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_CR0, cr0)) != 0)
		goto done;

	cr3 = 0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_CR3, cr3)) != 0)
		goto done;

	cr4 = 0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_CR4, cr4)) != 0)
		goto done;

	desc_base = 0x0;
	desc_limit = 0xffff;
	/* PRESENT | DESC_TYPE_CODEDATA | SEG_TYPE_DATA_RW_ACCESSED */
	desc_access = 0x00000093;
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_CS,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	gsel = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_CS, gsel)) != 0)
		goto done;

	desc_base = 0x0;
	desc_limit = 0xffff;
	/* PRESENT | DESC_TYPE_CODEDATA | SEG_TYPE_DATA_RW_ACCESSED */
	desc_access = 0x00000093;
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_SS,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	gsel = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_SS, gsel)) != 0)
		goto done;

	/* same as SS */
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_DS,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	/* same as SS */
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_DS, gsel)) != 0)
		goto done;

	/* same as SS */
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_ES,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	/* same as SS */
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_ES, gsel)) != 0)
		goto done;

	/* same as SS */
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_FS,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	/* same as SS */
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_FS, gsel)) != 0)
		goto done;

	/* same as SS */
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_GS,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	/* same as SS */
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_GS, gsel)) != 0)
		goto done;

	rdx = 0xf00;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RDX, rdx)) != 0)
		goto done;

	rax = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RAX, rax)) != 0)
		goto done;

	rbx = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RBX, rbx)) != 0)
		goto done;

	rcx = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RCX, rcx)) != 0)
		goto done;

	rsi = 0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RSI, rsi)) != 0)
		goto done;

	rdi = 0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RDI, rdi)) != 0)
		goto done;

	rbp = 0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RBP, rbp)) != 0)
		goto done;

	rsp = 0x8000 - 2;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_RSP, rsp)) != 0)
		goto done;

	desc_base = 0x0;
	desc_limit = 0xffff;
	/* PRESENT | DESC_TYPE_CODEDATA | SEG_TYPE_DATA_RW */
	desc_access = 0x00000092;
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_GDTR,
			    desc_base, desc_limit, desc_access);
	if (error != 0)
		goto done;

	/* same as GDTR */
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_IDTR,
			    desc_base, desc_limit, desc_access);
	if (error != 0)
		goto done;

	desc_base = 0x0;
	desc_limit = 0xffff;
	/* PRESENT | SEG_TYPE_16BIT_BUSY_TSS */
	desc_access = 0x00000083;
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_TR,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	gsel = 0x0;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_TR, gsel)) != 0)
		goto done;

	desc_base = 0x0;
	desc_limit = 0xffff;
	/* PRESENT | SEG_TYPE_LDT */
	desc_access = 0x00000082;
	error = vm_set_desc(vmctx, vcpu, VM_REG_GUEST_LDTR,
			    desc_base, desc_limit, desc_access);
	if (error)
		goto done;

	/* same as TR */
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_LDTR, gsel)) != 0)
		goto done;

	efer = 0x9;
	if ((error = vm_set_register(vmctx, vcpu, VM_REG_GUEST_EFER, efer)) != 0)
		goto done;

done:
	return (error);
}
