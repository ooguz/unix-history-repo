/*
 * Copyright (c) 1982 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 *
 *	@(#)saio.h	7.5 (Berkeley) %G%
 */

/*
 * Header file for standalone package
 */

#include "saerrno.h"

#define	UNIX	"/vmunix"

/*
 * Io block: includes an inode, cells for the use of seek, etc.,
 * and a buffer.
 */
struct	iob {
	int	i_flgs;		/* see F_ below */
	int	i_adapt;	/* adapter */
	int	i_ctlr;		/* controller */
	int	i_unit;		/* pseudo device unit */
	int	i_part;		/* disk partition */
	daddr_t	i_boff;		/* block offset on device */
	struct	inode i_ino;	/* inode, if file */
	daddr_t	i_cyloff;	/* cylinder offset on device */
	off_t	i_offset;	/* seek offset in file */
	daddr_t	i_bn;		/* 1st block # of next read */
	char	*i_ma;		/* memory address of i/o buffer */
	int	i_cc;		/* character count of transfer */
	int	i_error;	/* error # return */
	int	i_errcnt;	/* error count for driver retries */
	int	i_errblk;	/* block # in error for error reporting */
	char	i_buf[MAXBSIZE];/* i/o buffer */
	union {
		struct fs ui_fs;	/* file system super block info */
		char dummy[SBSIZE];
	} i_un;
};
#define i_fs i_un.ui_fs
#define NULL 0

#define F_READ		0x1	/* file opened for reading */
#define F_WRITE		0x2	/* file opened for writing */
#define F_ALLOC		0x4	/* buffer allocated */
#define F_FILE		0x8	/* file instead of device */
#define F_NBSF		0x10	/* no bad sector forwarding */
#define F_SSI		0x40	/* set skip sector inhibit */

/* io types */
#define	F_RDDATA	0x0100	/* read data */
#define	F_WRDATA	0x0200	/* write data */
#define F_HDR		0x0400	/* include header on next i/o */
#define F_CHECK		0x0800	/* perform check of data read/write */
#define F_HCHECK	0x1000	/* perform check of header and data */

#define	F_TYPEMASK	0xff00

/*
 * Lseek call.
 */
#define	L_SET		0	/* absolute offset */

/*
 * Device switch.
 */
struct devsw {
	char	*dv_name;
	int	(*dv_strategy)();
	int	(*dv_open)();
	int	(*dv_close)();
	int	(*dv_ioctl)();
};

struct devsw devsw[];
int ndevs;

#ifdef COMPAT_42
/*
 * Old drive description table.
 * still used by some drivers for now.
 */
struct st {
	short	nsect;		/* # sectors/track */
	short	ntrak;		/* # tracks/surfaces/heads */
	short	nspc;		/* # sectors/cylinder */
	short	ncyl;		/* # cylinders */
	short	*off;		/* partition offset table (cylinders) */
};
#endif

/*
 * Request codes. Must be the same as F_XXX above
 */
#define	READ	F_READ
#define	WRITE	F_WRITE

#define	NBUFS	4

char	b[NBUFS][MAXBSIZE];
daddr_t	blknos[NBUFS];

#define	NFILES	4
struct	iob iob[NFILES];

/* ioctl's -- for disks just now */
#define	SAIOHDR		(('d'<<8)|1)	/* next i/o includes header */
#define	SAIOCHECK	(('d'<<8)|2)	/* next i/o checks data */
#define	SAIOHCHECK	(('d'<<8)|3)	/* next i/o checks header & data */
#define	SAIONOBAD	(('d'<<8)|4)	/* inhibit bad sector forwarding */
#define	SAIODOBAD	(('d'<<8)|5)	/* enable bad sector forwarding */
#define	SAIOECCLIM	(('d'<<8)|6)	/* set limit to ecc correction, bits */
#define	SAIORETRIES	(('d'<<8)|7)	/* set retry count for unit */
#define	SAIODEVDATA	(('d'<<8)|8)	/* get pointer to pack label */
#define	SAIOSSI		(('d'<<8)|9)	/* set skip sector inhibit */
#define	SAIONOSSI	(('d'<<8)|10)	/* inhibit skip sector handling */
#define	SAIOSSDEV	(('d'<<8)|11)	/* is device skip sector type? */
#define	SAIODEBUG	(('d'<<8)|12)	/* enable/disable debugging */
#define	SAIOGBADINFO	(('d'<<8)|13)	/* get bad-sector table */
