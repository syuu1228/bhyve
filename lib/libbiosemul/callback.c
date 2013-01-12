/* No copyright?! */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: projects/doscmd/callback.c,v 1.7 2002/03/07 12:52:26 obrien Exp $");

#include <sys/queue.h>
#include "doscmd.h"

/*
** Callbacks are used for chaining interrupt handlers 
** off interrupt vectors
*/

struct callback {
    LIST_ENTRY(callback) chain;
    u_int32_t vec;
    callback_t func;
    const char *name;
};

LIST_HEAD(cbhead , callback) cbhead[127];

#define	CBHASH(x)	(((x) * 17) % 127)

/*
** Register (func) as a handler for (vec)
*/
void
register_callback(u_int32_t vec, callback_t func, const char *name)
{
    struct cbhead *head;
    struct callback *elm;

    elm = malloc(sizeof(struct callback));
    elm->vec = vec;
    elm->func = func;
    elm->name = name;
    
    head = &cbhead[CBHASH(vec)];
    LIST_INSERT_HEAD(head, elm, chain);
}

/*
** Find a handler for (vec)
*/
callback_t
find_callback(u_int32_t vec)
{
    struct cbhead *head;
    struct callback *elm;

    head = &cbhead[CBHASH(vec)];
    LIST_FOREACH(elm, head, chain)
	if (elm->vec == vec)
	    break;
    if (elm) {
	debug(D_TRAPS2, "callback %s\n", elm->name);
	return (elm->func);
    } else
	return ((callback_t)0);
}

u_int32_t trampoline_rover = 0xF1000000;

/*
 * Interrupts are disabled on an INTn call, so we must restore interrupts
 * before via STI returning.  IRET is not used here because 1) some DOS
 * calls want to return status via the FLAGS register, and 2) external
 * routines which hook INTn calls do not always put a FLAGS image on the
 * stack which re-enables interrupts.
 */
u_char softint_trampoline[] = {
    0x0f, 0x01, 0xc1, /* VMCALL */
    0xfb,	/* STI */
    0xca,	/* RETF 2 */
    2,
    0,
};
/*
 * From the FOSSIL spec:
 * The driver has a "signature" that can be used to determine whether it is
 * present in memory. At offset 6 in the INT 14h service routine is a word,
 * 1954h,  followed  by a  byte that  specifies the maximum function number
 * supported by the driver. This is to make it possible to determine when a
 * driver is present and what level of functionality it provides.
 */
u_char fossil_softint_trampoline[] = {
    0x0f, 0x01, 0xc1, /* VMCALL */
    0xfb,	/* STI */
    0xca,	/* RETF 2 */
    2,
    0,
    0,
    0x54,
    0x19,
    0x1b,	/* Max. Supported FOSSIL AH */
};
u_char hardint_trampoline[] = {
    0x0f, 0x01, 0xc1, /* VMCALL */
    0xcf,	/* IRET */
};
u_char null_trampoline[] = {
    0xcf,	/* IRET */
};

u_int32_t
insert_generic_trampoline(size_t len, u_char *p)
{
    u_char *q;
    u_int32_t where;

    where = trampoline_rover;
    q = (u_char *)(lomem_addr + VECPTR(where));
    memcpy(q, p, len);
    trampoline_rover += len;
    return (where);
}

u_int32_t
insert_softint_trampoline(void)
{
    return (insert_generic_trampoline(
	sizeof(softint_trampoline), softint_trampoline));
}

u_int32_t
insert_fossil_softint_trampoline(void)
{
    return (insert_generic_trampoline(
	sizeof(fossil_softint_trampoline), fossil_softint_trampoline));
}

u_int32_t
insert_hardint_trampoline(void)
{
    return (insert_generic_trampoline(
	sizeof(hardint_trampoline), hardint_trampoline));
}

u_int32_t
insert_null_trampoline(void)
{
    return (insert_generic_trampoline(
	sizeof(null_trampoline), null_trampoline));
}
