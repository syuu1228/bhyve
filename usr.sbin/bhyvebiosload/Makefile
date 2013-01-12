# $FreeBSD: soc2012/syuu/bhyve-bios/usr.sbin/bhyvebiosload/Makefile 224004 2011-07-06 22:38:09Z neel $

PROG=	bhyvebiosload
SRCS=	bhyvebiosload.c
NO_MAN=

DPADD+=	${LIBVMMAPI}
LDADD+=	-lvmmapi

WARNS?=	3

CFLAGS+=-I${.CURDIR}/../../sys/boot/userboot -g

.include <bsd.prog.mk>
