/*
 * Copyright (c) 1992, 1993, 1996
 *	Berkeley Software Design, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Berkeley Software
 *	Design, Inc.
 *
 * THIS SOFTWARE IS PROVIDED BY Berkeley Software Design, Inc. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Berkeley Software Design, Inc. BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	BSDI port.c,v 2.2 1996/04/08 19:33:03 bostic Exp
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: projects/doscmd/port.c,v 1.9 2002/07/19 13:38:43 markm Exp $");

#include <sys/ioctl.h>
#include <machine/sysarch.h>

#include "doscmd.h"
#include "tty.h"

#define	MINPORT		0x000
#define	MAXPORT_MASK	(MAXPORT - 1)

static __inline int
in(u_int port)
{
        int _inb_result;

#ifdef __GNUC__
        __asm __volatile ("xorl %%eax,%%eax; inb %%dx,%%al" :
            "=a" (_inb_result) : "d" (port));
#endif
        return _inb_result;
}

static __inline void
out(u_int port, int data)
{
#ifdef __GNUC__
        __asm __volatile ("outb %%al,%%dx" : : "a" (data), "d" (port));
#endif
}

FILE *iolog = 0;
u_int32_t ioports[MAXPORT/32];
#if 0
#ifdef __FreeBSD__
static void
iomap(int port, int cnt)
{
    if (port + cnt >= MAXPORT) {
	errno = ERANGE;
	goto bad;
    }
    if (i386_set_ioperm(port, cnt, 1) < 0) {
    bad:
	perror("iomap");
	quit(1);
    }
}

static void
iounmap(int port, int cnt)
{
    if (port + cnt >= MAXPORT) {
	errno = ERANGE;
	goto bad;
    }
    if (i386_set_ioperm(port, cnt, 0) < 0) {
    bad:
	perror("iounmap");
	quit(1);
    }
}
#else
static void
iomap(int port, int cnt)
{

    if (port + cnt >= MAXPORT) {
	errno = ERANGE;
	goto bad;
    }
    while (cnt--) {
	ioports[port/32] |= (1 << (port%32));
	port++;
    }
    if (i386_set_ioperm(ioports) < 0) {
    bad:
	perror("iomap");
	quit(1);
    }
}

static void
iounmap(int port, int cnt)
{

    if (port + cnt >= MAXPORT) {
	errno = ERANGE;
	goto bad;
    }
    while (cnt--) {
	ioports[port/32] &= ~(1 << (port%32));
	port++;
    }
    if (i386_set_ioperm(ioports) < 0) {
    bad:
	perror("iounmap");
	quit(1);
    }
}
#endif
void
outb_traceport(int port, unsigned char byte)
{
/*
    if (!iolog && !(iolog = fopen("/tmp/iolog", "a")))
	iolog = stderr;

    fprintf(iolog, "0x%03X -> %02X\n", port, byte);
 */

    iomap(port, 1);
    out(port, byte);
    iounmap(port, 1);
}

unsigned char
inb_traceport(int port)
{
    unsigned char byte;

/*
    if (!iolog && !(iolog = fopen("/tmp/iolog", "a")))
	iolog = stderr;
 */

    iomap(port, 1);
    byte = in(port);
    iounmap(port, 1);

/*
    fprintf(iolog, "0x%03X <- %02X\n", port, byte);
    fflush(iolog);
 */
    return(byte);
}
#endif

/*
 * Real input/output to (hopefully) iomapped port
 */
void
outb_port(int port, unsigned char byte)
{
    out(port, byte);
}

unsigned char
inb_port(int port)
{
    return in(port);
}

/* 
 * Fake input/output ports
 */

static void
outb_nullport(int port __unused, unsigned char byte __unused)
{
/*
    debug(D_PORT, "outb_nullport called for port 0x%03X = 0x%02X.\n",
		   port, byte);
 */
}

static unsigned char
inb_nullport(int port __unused)
{
/*
    debug(D_PORT, "inb_nullport called for port 0x%03X.\n", port);
 */
    return(0xff);
}

/*
 * configuration table for ports' emulators
 */

struct portsw {
	unsigned char	(*p_inb)(int port);
	void		(*p_outb)(int port, unsigned char byte);
} portsw[MAXPORT];

void
init_io_port_handlers(void)
{
    int i;

    for (i = 0; i < MAXPORT; i++) {
	if (portsw[i].p_inb == 0)
	    portsw[i].p_inb = inb_nullport;
	if (portsw[i].p_outb == 0)
	    portsw[i].p_outb = outb_nullport;
    }

}

void
define_input_port_handler(int port, unsigned char (*p_inb)(int port))
{
	if ((port >= MINPORT) && (port < MAXPORT)) {
		portsw[port].p_inb = p_inb;
	} else
		fprintf (stderr, "attempt to handle invalid port 0x%04x", port);
}

void
define_output_port_handler(int port, void (*p_outb)(int port, unsigned char byte))
{
	if ((port >= MINPORT) && (port < MAXPORT)) {
		portsw[port].p_outb = p_outb;
	} else
		fprintf (stderr, "attempt to handle invalid port 0x%04x", port);
}


void
inb(regcontext_t *REGS, int port)
{
	unsigned char (*in_handler)(int);

	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port].p_inb;
	else
		in_handler = inb_nullport;
	R_AL = (*in_handler)(port);
	debug(D_PORT, "IN  on port %02x -> %02x\n", port, R_AL);
}

void
insb(regcontext_t *REGS, int port)
{
	unsigned char (*in_handler)(int);
	unsigned char data;

	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port].p_inb;
	else
		in_handler = inb_nullport;
	data = (*in_handler)(port);
	*(u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI)) = data;
	debug(D_PORT, "INS on port %02x -> %02x\n", port, data);

	if (R_FLAGS & PSL_D)
	    R_DI--;
	else
	    R_DI++;
}

void
inx(regcontext_t *REGS, int port)
{
	unsigned char (*in_handler)(int);

	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port].p_inb;
	else
		in_handler = inb_nullport;
	R_AL =  (*in_handler)(port);
	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port + 1].p_inb;
	else
		in_handler = inb_nullport;
	R_AH = (*in_handler)(port + 1);
	debug(D_PORT, "IN  on port %02x -> %04x\n", port, R_AX);
}

void
insx(regcontext_t *REGS, int port)
{
	unsigned char (*in_handler)(int);
	unsigned char data;

	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port].p_inb;
	else
		in_handler = inb_nullport;
	data = (*in_handler)(port);
	*(u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI)) = data;
	debug(D_PORT, "INS on port %02x -> %02x\n", port, data);

	if ((port >= MINPORT) && (port < MAXPORT))
		in_handler = portsw[port + 1].p_inb;
	else
		in_handler = inb_nullport;
	data = (*in_handler)(port + 1);
	((u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI)))[1] = data;
	debug(D_PORT, "INS on port %02x -> %02x\n", port, data);

	if (R_FLAGS & PSL_D)
	    R_DI -= 2;
	else
	    R_DI += 2;
}

void
outb(regcontext_t *REGS, int port)
{
	void (*out_handler)(int, unsigned char);

	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port].p_outb;
	else
		out_handler = outb_nullport;
	(*out_handler)(port, R_AL);
	debug(D_PORT, "OUT on port %02x <- %02x\n", port, R_AL);
/*
  if (port == 0x3bc && R_AL == 0x55)
    tmode = 1;
*/
}

void
outx(regcontext_t *REGS, int port)
{
	void (*out_handler)(int, unsigned char);

	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port].p_outb;
	else
		out_handler = outb_nullport;
	(*out_handler)(port, R_AL);
	debug(D_PORT, "OUT on port %02x <- %02x\n", port, R_AL);
	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port + 1].p_outb;
	else
		out_handler = outb_nullport;
	(*out_handler)(port + 1, R_AH);
	debug(D_PORT, "OUT on port %02x <- %02x\n", port + 1, R_AH);
}

void
outsb(regcontext_t *REGS, int port)
{
	void (*out_handler)(int, unsigned char);
	unsigned char value;

	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port].p_outb;
	else
		out_handler = outb_nullport;
	value = *(u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI));
	debug(D_PORT, "OUT on port %02x <- %02x\n", port, value);
	(*out_handler)(port, value);

	if (R_FLAGS & PSL_D)
	    R_DI--;
	else
	    R_DI++;
}

void
outsx(regcontext_t *REGS, int port)
{
	void (*out_handler)(int, unsigned char);
	unsigned char value;

	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port].p_outb;
	else
		out_handler = outb_nullport;
	value = *(u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI));
	debug(D_PORT, "OUT on port %02x <- %02x\n", port, value);
	(*out_handler)(port, value);

	if ((port >= MINPORT) && (port < MAXPORT))
		out_handler = portsw[port + 1].p_outb;
	else
		out_handler = outb_nullport;
	value = ((u_char *)(lomem_addr + MAKEPTR(R_ES, R_DI)))[1];
	debug(D_PORT, "OUT on port %02x <- %02x\n", port+1, value);
	(*out_handler)(port + 1, value);

	if (R_FLAGS & PSL_D)
	    R_DI -= 2;
	else
	    R_DI += 2;
}

bool io_port_defined(int in, int port)
{
	if ((port < MINPORT) && (port > MAXPORT))
		return false;

	if (in)
		return (((void *)portsw[port].p_inb) != inb_nullport);
	else
		return (((void *)portsw[port].p_outb) != inb_nullport);
}

