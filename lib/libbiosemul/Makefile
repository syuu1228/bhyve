# from BSDI Makefile,v 2.6 1996/04/08 20:06:40 bostic Exp
#
# $FreeBSD: projects/doscmd/Makefile,v 1.39 2004/03/30 17:10:01 des Exp $

LIB=	biosemul
WARNS?=	2
SRCS=	biosemul.c \
	bios.c callback.c cpu.c \
	debug.c i386-pinsn.c \
	int.c int10.c int13.c int16.c \
	mouse.c port.c trap.c \
	tty.c video.c ${FONTHDRS}
INCS=	biosemul.h
CFLAGS+=	-I. -DDISASSEMBLER
FONTFILES=	cp437-8x8.pcf.gz cp437-8x14.pcf.gz cp437-8x16.pcf.gz
FONTHDRS=	font8x8.h font8x14.h font8x16.h
CLEANFILES=	${FONTFILES} ${FONTHDRS}

CFLAGS+=	-DNO_X

cp437-8x8.pcf.gz: cp437-8x8.pcf.gz.uu
	uudecode ${.CURDIR}/cp437-8x8.pcf.gz.uu

cp437-8x14.pcf.gz: cp437-8x14.pcf.gz.uu
	uudecode ${.CURDIR}/cp437-8x14.pcf.gz.uu

cp437-8x16.pcf.gz: cp437-8x16.pcf.gz.uu
	uudecode ${.CURDIR}/cp437-8x16.pcf.gz.uu

font8x8.h: cp437-8x8.fnt.uu
	uudecode -p ${.ALLSRC} | \
		file2c 'u_int8_t font8x8[] = {' '};' > ${.TARGET}

font8x14.h: cp437-8x14.fnt.uu
	uudecode -p ${.ALLSRC} | \
		file2c 'u_int8_t font8x14[] = {' '};' > ${.TARGET}

font8x16.h: cp437-8x16.fnt.uu
	uudecode -p ${.ALLSRC} | \
		file2c 'u_int8_t font8x16[] = {' '};' > ${.TARGET}

.include <bsd.lib.mk>
