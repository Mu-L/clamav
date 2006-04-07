/*
 *  Copyright (C) 2002 - 2006 Tomasz Kojm <tkojm@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clamav.h"
#include "others.h"
#include "matcher-ac.h"
#include "matcher-bm.h"
#include "md5.h"
#include "filetypes.h"
#include "matcher.h"
#include "pe.h"
#include "elf.h"
#include "execs.h"
#include "special.h"

#define MD5_BLOCKSIZE 4096

static int targettab[CL_TARGET_TABLE_SIZE] = { 0, CL_TYPE_MSEXE, CL_TYPE_MSOLE2, CL_TYPE_HTML, CL_TYPE_MAIL, CL_TYPE_GRAPHICS, CL_TYPE_ELF };

extern short cli_debug_flag;

#ifdef HAVE_HWACCEL
#include <sn_sigscan/sn_sigscan.h>
#define HWBUFFSIZE 32768
#endif

int cli_scanbuff(const char *buffer, unsigned int length, const char **virname, const struct cl_engine *engine, unsigned short ftype)
{
	int ret = CL_CLEAN, i, tid = 0, *partcnt;
	unsigned long int *partoff;
	struct cli_matcher *groot, *troot = NULL;
#ifdef HAVE_HWACCEL
	void *streamhandle;
	void *resulthandle;
	uint32_t datamask[2] = { 0xffffffff, 0xffffffff };
	int count, hret;
	unsigned long long offset;
#endif


    if(!engine) {
	cli_errmsg("cli_scanbuff: engine == NULL\n");
	return CL_ENULLARG;
    }

#ifdef HAVE_HWACCEL
    if(engine->hwaccel) {
	/* TODO: Setup proper data bitmask (need specs) */
	if((hret = sn_sigscan_createstream(engine->hwdb, datamask, 2, &streamhandle)) < 0) {
	    cli_errmsg("cli_scanbuff: can't create new hardware stream: %d\n", hret);
	    return CL_EHWIO;
	}

	if((hret = sn_sigscan_writestream(streamhandle, buffer, length)) < 0) {
	    cli_errmsg("cli_scanbuff: can't write %u bytes to hardware stream: %d\n", length, hret);
	    sn_sigscan_closestream(streamhandle, &resulthandle);
	    return CL_EHWIO;
	}

	if((hret = sn_sigscan_closestream(streamhandle, &resulthandle)) < 0) {
	    cli_errmsg("cli_scanbuff: can't close hardware stream: %d\n", hret);
	    return CL_EHWIO;
	}

	count = sn_sigscan_resultcount(resulthandle);
	cli_dbgmsg("cli_scanbuff: number of results: %d\n", count);

	if(count > 0) {
	    if((hret = sn_sigscan_resultget(resulthandle, 0, virname, &offset)) < 0) {
		cli_errmsg("cli_scanbuff: can't get hardware match result: %d\n", hret);
		sn_sigscan_resultfree(resulthandle);
		return CL_EHWIO;
	    } else {
		cli_dbgmsg("cli_scanbuff: hardware match %s at %u\n", *virname, offset);
		ret = CL_VIRUS;
	    }
	}

	if((hret = sn_sigscan_resultfree(resulthandle)) < 0) {
	    cli_errmsg("cli_scanbuff: can't free results: %d\n", ret);
	    return CL_EHWIO;
	}

	return ret;
    }
#endif /* HAVE_HWACCEL */


    groot = engine->root[0]; /* generic signatures */

    if(ftype) {
	for(i = 0; i < CL_TARGET_TABLE_SIZE; i++) {
	    if(targettab[i] == ftype) {
		tid = i;
		break;
	    }
	}
	if(tid)
	    troot = engine->root[tid];
    }

    if(troot) {

	if((partcnt = (int *) cli_calloc(troot->ac_partsigs + 1, sizeof(int))) == NULL) {
	    cli_dbgmsg("cli_scanbuff(): unable to cli_calloc(%d, %d)\n", troot->ac_partsigs + 1, sizeof(int));
	    return CL_EMEM;
	}

	if((partoff = (unsigned long int *) cli_calloc(troot->ac_partsigs + 1, sizeof(unsigned long int))) == NULL) {
	    cli_dbgmsg("cli_scanbuff(): unable to cli_calloc(%d, %d)\n", troot->ac_partsigs + 1, sizeof(unsigned long int));
	    free(partcnt);
	    return CL_EMEM;
	}

	if(troot->ac_only || (ret = cli_bm_scanbuff(buffer, length, virname, troot, 0, ftype, -1)) != CL_VIRUS)
	    ret = cli_ac_scanbuff(buffer, length, virname, troot, partcnt, 0, 0, partoff, ftype, -1, NULL);

	free(partcnt);
	free(partoff);

	if(ret == CL_VIRUS)
	    return ret;
    }

    if((partcnt = (int *) cli_calloc(groot->ac_partsigs + 1, sizeof(int))) == NULL) {
	cli_dbgmsg("cli_scanbuff(): unable to cli_calloc(%d, %d)\n", groot->ac_partsigs + 1, sizeof(int));
	return CL_EMEM;
    }

    if((partoff = (unsigned long int *) cli_calloc(groot->ac_partsigs + 1, sizeof(unsigned long int))) == NULL) {
	cli_dbgmsg("cli_scanbuff(): unable to cli_calloc(%d, %d)\n", groot->ac_partsigs + 1, sizeof(unsigned long int));
	free(partcnt);
	return CL_EMEM;
    }

    if(groot->ac_only || (ret = cli_bm_scanbuff(buffer, length, virname, groot, 0, ftype, -1)) != CL_VIRUS)
	ret = cli_ac_scanbuff(buffer, length, virname, groot, partcnt, 0, 0, partoff, ftype, -1, NULL);

    free(partcnt);
    free(partoff);
    return ret;
}

static struct cli_md5_node *cli_vermd5(const unsigned char *md5, const struct cl_engine *engine)
{
	struct cli_md5_node *pt;


    if(!(pt = engine->md5_hlist[md5[0] & 0xff]))
	return NULL;

    while(pt) {
	if(!memcmp(pt->md5, md5, 16))
	    return pt;

	pt = pt->next;
    }

    return NULL;
}

static long int cli_caloff(const char *offstr, int fd, unsigned short ftype)
{
	struct cli_exe_info exeinfo;
	int (*einfo)(int, struct cli_exe_info *) = NULL;
	long int offset = -1;
	int n;


    if(ftype == CL_TYPE_MSEXE)
	einfo = cli_peheader;
    else if(ftype == CL_TYPE_ELF)
	einfo = cli_elfheader;

    if(isdigit(offstr[0])) {
	return atoi(offstr);

    } else if(einfo && (!strncmp(offstr, "EP+", 3) || !strncmp(offstr, "EP-", 3))) {
	if((n = lseek(fd, 0, SEEK_CUR)) == -1) {
	    cli_dbgmsg("Invalid descriptor\n");
	    return -1;
	}
	lseek(fd, 0, SEEK_SET);
	if(einfo(fd, &exeinfo)) {
	    lseek(fd, n, SEEK_SET);
	    return -1;
	}
	free(exeinfo.section);
	lseek(fd, n, SEEK_SET);

	if(offstr[2] == '+')
	    return exeinfo.ep + atoi(offstr + 3);
	else
	    return exeinfo.ep - atoi(offstr + 3);

    } else if(einfo && offstr[0] == 'S') {
	if((n = lseek(fd, 0, SEEK_CUR)) == -1) {
	    cli_dbgmsg("Invalid descriptor\n");
	    return -1;
	}
	lseek(fd, 0, SEEK_SET);
	if(einfo(fd, &exeinfo)) {
	    lseek(fd, n, SEEK_SET);
	    return -1;
	}
	lseek(fd, n, SEEK_SET);

	if(!strncmp(offstr, "SL", 2)) {

	    if(sscanf(offstr, "SL+%ld", &offset) != 1) {
		free(exeinfo.section);
		return -1;
	    }

	    offset += exeinfo.section[exeinfo.nsections - 1].raw;

	} else {

	    if(sscanf(offstr, "S%d+%ld", &n, &offset) != 2) {
		free(exeinfo.section);
		return -1;
	    }

	    if(n >= exeinfo.nsections) {
		free(exeinfo.section);
		return -1;
	    }

	    offset += exeinfo.section[n].raw;
	}

	free(exeinfo.section);
	return offset;

    } else if(!strncmp(offstr, "EOF-", 4)) {
	    struct stat sb;

	if(fstat(fd, &sb) == -1)
	    return -1;

	return sb.st_size - atoi(offstr + 4);
    }

    return -1;
}

int cli_checkfp(int fd, const struct cl_engine *engine)
{
	struct cli_md5_node *md5_node;
	unsigned char *digest;


    if(engine->md5_hlist) {

	if(!(digest = cli_md5digest(fd))) {
	    cli_errmsg("cli_checkfp(): Can't generate MD5 checksum\n");
	    return 0;
	}

	if((md5_node = cli_vermd5(digest, engine)) && md5_node->fp) {
		struct stat sb;

	    if(fstat(fd, &sb))
		return CL_EIO;

	    if((unsigned int) sb.st_size != md5_node->size) {
		cli_warnmsg("Detected false positive MD5 match. Please report.\n");
	    } else {
		cli_dbgmsg("Eliminated false positive match (fp sig: %s)\n", md5_node->virname);
		free(digest);
		return 1;
	    }
	}

	free(digest);
    }

    return 0;
}

int cli_validatesig(unsigned short target, unsigned short ftype, const char *offstr, unsigned long int fileoff, int desc, const char *virname)
{

    if(offstr && desc != -1) {
	    long int off = cli_caloff(offstr, desc, ftype);

	if(off == -1) {
	    cli_dbgmsg("Bad offset in signature (%s)\n", virname);
	    return 0;
	}

	if(fileoff != (unsigned long int) off) {
	    cli_dbgmsg("Virus offset: %d, expected: %d (%s)\n", fileoff, off, virname);
	    return 0;
	}
    }

    return 1;
}

int cli_scandesc(int desc, cli_ctx *ctx, unsigned short otfrec, unsigned short ftype, struct cli_matched_type **ftoffset)
{
 	char *buffer, *buff, *endbl, *pt;
	int ret = CL_CLEAN, *gpartcnt, *tpartcnt, type = CL_CLEAN, i, tid = 0, bytes;
	unsigned int buffersize, length, maxpatlen, shift = 0;
	unsigned long int *gpartoff, *tpartoff, offset = 0;
	MD5_CTX md5ctx;
	unsigned char digest[16];
	struct cli_md5_node *md5_node;
	struct cli_matcher *groot, *troot = NULL;
#ifdef HAVE_HWACCEL
	void *streamhandle;
	void *resulthandle;
	unsigned long long hoffset;
	uint32_t datamask[2] = { 0xffffffff, 0xffffffff };
	int count, hret;
#endif


    if(!ctx->engine) {
	cli_errmsg("cli_scandesc: engine == NULL\n");
	return CL_ENULLARG;
    }

#ifdef HAVE_HWACCEL
    if(ctx->engine->hwaccel) {
	/* TODO: Setup proper data bitmask (need specs) */
	if((hret = sn_sigscan_createstream(ctx->engine->hwdb, datamask, 2, &streamhandle)) < 0) {
	    cli_errmsg("cli_scandesc: can't create new hardware stream: %d\n", hret);
	    return CL_EHWIO;
	}

	if(!(buffer = (char *) cli_calloc(HWBUFFSIZE, sizeof(char)))) {
	    cli_dbgmsg("cli_scandesc: unable to cli_calloc(%u)\n", HWBUFFSIZE);
	    return CL_EMEM;
	}

	while((bytes = cli_readn(desc, buffer, HWBUFFSIZE)) > 0) {
	    if((hret = sn_sigscan_writestream(streamhandle, buffer, bytes)) < 0) {
		cli_errmsg("cli_scandesc: can't write to hardware stream: %d\n", hret);
		ret = CL_EHWIO;
		break;
	    } else {
		if(ctx->scanned)
		    *ctx->scanned += bytes / CL_COUNT_PRECISION;
	    }
	}

	free(buffer);

	if((hret = sn_sigscan_closestream(streamhandle, &resulthandle)) < 0) {
	    cli_errmsg("cli_scandesc: can't close hardware stream: %d\n", hret);
	    return CL_EHWIO;
	}

	count = sn_sigscan_resultcount(resulthandle);
	cli_dbgmsg("cli_scandesc: number of results: %d\n", count);

	if(count > 0) {
	    if((hret = sn_sigscan_resultget(resulthandle, 0, ctx->virname, &hoffset)) < 0) {
		cli_errmsg("cli_scandesc: can't get hardware match result: %d\n", hret);
		sn_sigscan_resultfree(resulthandle);
		return CL_EHWIO;
	    } else {
		cli_dbgmsg("cli_scandesc: hardware match %s at %u\n", *ctx->virname, hoffset);
		ret = CL_VIRUS;
	    }
	}

	if((hret = sn_sigscan_resultfree(resulthandle)) < 0) {
	    cli_errmsg("cli_scandesc: can't free results: %d\n", ret);
	    return CL_EHWIO;
	}

	return ret;
    }
#endif /* HAVE_HWACCEL */


    groot = ctx->engine->root[0]; /* generic signatures */

    if(ftype) {
	for(i = 0; i < CL_TARGET_TABLE_SIZE; i++) {
	    if(targettab[i] == ftype) {
		tid = i;
		break;
	    }
	}
	if(tid)
	    troot = ctx->engine->root[tid];
    }

    if(troot)
	maxpatlen = MAX(troot->maxpatlen, groot->maxpatlen);
    else
	maxpatlen = groot->maxpatlen;

    /* prepare the buffer */
    buffersize = maxpatlen + SCANBUFF;
    if(!(buffer = (char *) cli_calloc(buffersize, sizeof(char)))) {
	cli_dbgmsg("cli_scandesc(): unable to cli_calloc(%d)\n", buffersize);
	return CL_EMEM;
    }

    if((gpartcnt = (int *) cli_calloc(groot->ac_partsigs + 1, sizeof(int))) == NULL) {
	cli_dbgmsg("cli_scandesc(): unable to cli_calloc(%d, %d)\n", groot->ac_partsigs + 1, sizeof(int));
	free(buffer);
	return CL_EMEM;
    }

    if((gpartoff = (unsigned long int *) cli_calloc(groot->ac_partsigs + 1, sizeof(unsigned long int))) == NULL) {
	cli_dbgmsg("cli_scandesc(): unable to cli_calloc(%d, %d)\n", groot->ac_partsigs + 1, sizeof(unsigned long int));
	free(buffer);
	free(gpartcnt);
	return CL_EMEM;
    }

    if(troot) {

	if((tpartcnt = (int *) cli_calloc(troot->ac_partsigs + 1, sizeof(int))) == NULL) {
	    cli_dbgmsg("cli_scandesc(): unable to cli_calloc(%d, %d)\n", troot->ac_partsigs + 1, sizeof(int));
	    free(buffer);
	    free(gpartcnt);
	    free(gpartoff);
	    return CL_EMEM;
	}

	if((tpartoff = (unsigned long int *) cli_calloc(troot->ac_partsigs + 1, sizeof(unsigned long int))) == NULL) {
	    cli_dbgmsg("cli_scandesc(): unable to cli_calloc(%d, %d)\n", troot->ac_partsigs + 1, sizeof(unsigned long int));
	    free(buffer);
	    free(gpartcnt);
	    free(gpartoff);
	    free(tpartcnt);
	    return CL_EMEM;
	}
    }

    if(ctx->engine->md5_hlist)
	MD5_Init(&md5ctx);


    buff = buffer;
    buff += maxpatlen; /* pointer to read data block */
    endbl = buff + SCANBUFF - maxpatlen; /* pointer to the last block
						* length of maxpatlen
						*/

    pt = buff;
    while((bytes = cli_readn(desc, buff + shift, SCANBUFF - shift)) > 0) {

	if(ctx->scanned)
	    *ctx->scanned += bytes / CL_COUNT_PRECISION;

	length = shift + bytes;
	if(pt == buffer)
	    length += maxpatlen;

	if(troot) {
	    if(troot->ac_only || (ret = cli_bm_scanbuff(pt, length, ctx->virname, troot, offset, ftype, desc)) != CL_VIRUS)
		ret = cli_ac_scanbuff(pt, length, ctx->virname, troot, tpartcnt, otfrec, offset, tpartoff, ftype, desc, ftoffset);

	    if(ret == CL_VIRUS) {
		free(buffer);
		free(gpartcnt);
		free(gpartoff);
		free(tpartcnt);
		free(tpartoff);

		lseek(desc, 0, SEEK_SET);
		if(cli_checkfp(desc, ctx->engine))
		    return CL_CLEAN;
		else
		    return CL_VIRUS;
	    }
	}

	if(groot->ac_only || (ret = cli_bm_scanbuff(pt, length, ctx->virname, groot, offset, ftype, desc)) != CL_VIRUS)
	    ret = cli_ac_scanbuff(pt, length, ctx->virname, groot, gpartcnt, otfrec, offset, gpartoff, ftype, desc, ftoffset);

	if(ret == CL_VIRUS) {
	    free(buffer);
	    free(gpartcnt);
	    free(gpartoff);
	    if(troot) {
		free(tpartcnt);
		free(tpartoff);
	    }
	    lseek(desc, 0, SEEK_SET);
	    if(cli_checkfp(desc, ctx->engine))
		return CL_CLEAN;
	    else
		return CL_VIRUS;

	} else if(otfrec && ret >= CL_TYPENO) {
	    if(ret >= type)
		type = ret;
	}

	if(ctx->engine->md5_hlist)
	    MD5_Update(&md5ctx, buff + shift, bytes);

	if(bytes + shift == SCANBUFF) {
	    memmove(buffer, endbl, maxpatlen);
	    offset += SCANBUFF;

	    if(pt == buff) {
		pt = buffer;
		offset -= maxpatlen;
	    }

	    shift = 0;

	} else {
	    shift += bytes;
	}

    }

    free(buffer);
    free(gpartcnt);
    free(gpartoff);
    if(troot) {
	free(tpartcnt);
	free(tpartoff);
    }

    if(ctx->engine->md5_hlist) {
	MD5_Final(digest, &md5ctx);

	if(cli_debug_flag) {
		char md5str[33];
		int i;

	    pt = md5str;
	    for(i = 0; i < 16; i++) {
		sprintf(pt, "%02x", digest[i]);
		pt += 2;
	    }
	    md5str[32] = 0;
	}

	if((md5_node = cli_vermd5(digest, ctx->engine)) && !md5_node->fp) {
		struct stat sb;

	    if(fstat(desc, &sb))
		return CL_EIO;

	    if((unsigned int) sb.st_size != md5_node->size) {
		cli_warnmsg("Detected false positive MD5 match. Please report.\n");
	    } else {
		if(ctx->virname)
		    *ctx->virname = md5_node->virname;

		return CL_VIRUS;
	    }
	}
    }

    return otfrec ? type : CL_CLEAN;
}
