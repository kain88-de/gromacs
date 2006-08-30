/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * GROningen Mixture of Alchemy and Childrens' Stories
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <stdio.h>

#include "gmx_fatal.h"
#include "macros.h"
#include "smalloc.h"
#include "futil.h"
#include "filenm.h"
#include "string2.h"
#include "gmxfio.h"

/* XDR should be available on all platforms now, 
 * but we keep the possibility of turning it off...
 */
#define USE_XDR

typedef struct {
  int  iFTP;
  bool bOpen,bRead,bDouble,bDebug,bStdio;
  char *fn;
  FILE *fp;
  XDR  *xdr;
} t_fileio;

/* These simple lists define the I/O type for these files */
static const int ftpXDR[] = { efTPR, efTRR, efEDR, efXTC, efMTX };
static const int ftpASC[] = { efTPA, efGRO, efPDB };
static const int ftpBIN[] = { efTPB, efTRJ, efENE };
#ifdef HAVE_XML
static const int ftpXML[] = { efXML };
#endif

bool in_ftpset(int ftp,int nset,const int set[])
{
  int i;
  bool bResult;
  
  bResult = FALSE;
  for(i=0; (i<nset); i++)
    if (ftp == set[i])
      bResult = TRUE;
  
  return bResult;    
}

static bool do_dummy(void *item,int nitem,int eio,
		     char *desc,char *srcfile,int line)
{
  gmx_fatal(FARGS,"gmx_fio_select not called!");
  
  return FALSE;
}

/* Global variables */
do_func *do_read  = do_dummy;
do_func *do_write = do_dummy;
char *itemstr[eitemNR] = {
  "[header]",      "[inputrec]",   "[box]",         "[topology]", 
  "[coordinates]", "[velocities]", "[forces]"
};
/* Comment strings for TPA only */
char *comment_str[eitemNR] = {
  "; The header holds information on the number of atoms etc. and on whether\n"
  "; certain items are present in the file or not.\n"
  "; \n"
  ";                             WARNING\n"
  ";                   DO NOT EDIT THIS FILE BY HAND\n"
  "; The GROMACS preprocessor performs a lot of checks on your input that\n"
  "; you ignore when editing this. Your simulation may crash because of this\n",
  
  "; The inputrec holds the parameters for MD such as the number of steps,\n"
  "; the timestep and the cut-offs.\n",
  "; The simulation box in nm.\n",
  "; The topology section describes the topology of the molcecules\n"
  "; i.e. bonds, angles and dihedrals etc. and also holds the force field\n"
  "; parameters.\n",
  "; The atomic coordinates in nm\n",
  "; The atomic velocities in nm/ps\n",
  "; The forces on the atoms in nm/ps^2\n"
};


/* Local variables */
static t_fileio *FIO = NULL;
static t_fileio *curfio = NULL;
static int  nFIO = 0;
static char *eioNames[eioNR] = { "REAL", "INT", "NUCHAR", "USHORT", 
				 "RVEC", "NRVEC", "IVEC", "STRING" };
static char *add_comment = NULL;

static char *dbgstr(char *desc)
{
  static char *null_str="";
  static char buf[STRLEN];
  
  if (!curfio->bDebug)
    return null_str;
  else {
    sprintf(buf,"  ; %s %s",add_comment ? add_comment : "",desc);
    return buf;
  }
}

void set_comment(char *comment)
{
  if (comment)
    add_comment = strdup(comment);
}

void unset_comment(void)
{
  if (add_comment)
    sfree(add_comment);
  add_comment = NULL;
}


static void _check_nitem(int eio,int nitem,char *file,int line)
{
  if ((nitem != 1) && !((eio == eioNRVEC) || (eio == eioNUCHAR)))
    gmx_fatal(FARGS,"nitem (%d) may differ from 1 only for %s or %s, not for %s"
	      "(%s, %d)",nitem,eioNames[eioNUCHAR],eioNames[eioNRVEC],
	      eioNames[eio],file,line);
}

#define check_nitem() _check_nitem(eio,nitem,__FILE__,__LINE__)

static void fe(int eio,char *desc,char *srcfile,int line)
{
  gmx_fatal(FARGS,"Trying to %s %s type %d (%s), src %s, line %d",
	    curfio->bRead ? "read" : "write",desc,eio,
	    ((eio >= 0) && (eio < eioNR)) ? eioNames[eio] : "unknown",
	    srcfile,line);
}

#define FE() fe(eio,desc,__FILE__,__LINE__)


static void encode_string(int maxlen,char dst[],char src[])
{
  int i;
  
  for(i=0; (src[i] != '\0') && (i < maxlen-1); i++)
    if ((src[i] == ' ') || (src[i] == '\t'))
      dst[i] = '_';
    else
      dst[i] = src[i];
  dst[i] = '\0';
  
  if (i == maxlen)
    fprintf(stderr,"String '%s' truncated to '%s'\n",src,dst);
}

static void decode_string(int maxlen,char dst[],char src[])
{
  int i;
  
  for(i=0; (src[i] != '\0') && (i < maxlen-1); i++)
    if (src[i] == '_')
      dst[i] = ' ';
    else
      dst[i] = src[i];
  dst[i] = '\0';
  
  if (i == maxlen)
    fprintf(stderr,"String '%s' truncated to '%s'\n",src,dst);
}


static bool do_ascwrite(void *item,int nitem,int eio,
			char *desc,char *srcfile,int line)
{
  int  i;
  int  res=0,*iptr;
  real *ptr;
  char strbuf[256];
  unsigned char *ucptr;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    res = fprintf(curfio->fp,"%18.10e%s\n",*((real *)item),dbgstr(desc));
    break;
  case eioINT:
    res = fprintf(curfio->fp,"%18d%s\n",*((int *)item),dbgstr(desc));
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++)
      res = fprintf(curfio->fp,"%4d",(int)ucptr[i]);
    fprintf(curfio->fp,"%s\n",dbgstr(desc));
    break;
  case eioUSHORT:
    res = fprintf(curfio->fp,"%18d%s\n",*((unsigned short *)item),
		  dbgstr(desc));
    break;
  case eioRVEC:
    ptr = (real *)item;
    res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		  ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      res = fprintf(curfio->fp,"%18.10e%18.10e%18.10e%s\n",
		    ptr[XX],ptr[YY],ptr[ZZ],dbgstr(desc));
    }
    break;
  case eioIVEC:
    iptr= (int *)item;
    res = fprintf(curfio->fp,"%18d%18d%18d%s\n",
		  iptr[XX],iptr[YY],iptr[ZZ],dbgstr(desc));
    break;
  case eioSTRING:
    encode_string(256,strbuf,(char *)item);
    res = fprintf(curfio->fp,"%-18s%s\n",strbuf,dbgstr(desc));
    break;
  default:
    FE();
  }
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  return (res > 0);
}

/* This is a global variable that is reset when a file is opened. */
static  int  nbuf=0;

static char *next_item(FILE *fp)
{
  /* This routine reads strings from the file fp, strips comment
   * and buffers. If there are multiple strings on a line, they will
   * be stored here, and indices in the line buffer (buf) will be
   * stored in bufindex. This way we can uncomment on the fly,
   * without too much double work. Each string is first read through
   * fscanf in this routine, and then through sscanf in do_read.
   * No unnecessary string copying is done.
   */
#define MAXBUF 20
  static  char buf[STRLEN];
  static  int  bufindex[MAXBUF];
  int     i,j0;
  char    ccc;
  
  if (nbuf) {
    j0 = bufindex[0];
    for(i=1; (i<nbuf); i++)
      bufindex[i-1] = bufindex[i];
    nbuf--;

    return buf+j0;
  }
  else {
    /* First read until we find something that is not comment */
    if (fgets2(buf,STRLEN-1,fp) == NULL)
      gmx_file("End of file");
      
    i = 0;
    do {
      /* Skip over leading spaces */
      while ((buf[i] != '\0') && (buf[i] != ';') && isspace(buf[i]))
	i++;

      /* Store start of something non-space */
      j0 = i;
      
      /* Look for next spaces */
      while ((buf[i] != '\0') && (buf[i] != ';') && !isspace(buf[i]))
	i++;
	
      /* Store the last character in the string */
      ccc = buf[i];
      
      /* If the string is non-empty, add it to the list */
      if (i > j0) {
	buf[i] = '\0';
	bufindex[nbuf++] = j0;
	
	/* We increment i here; otherwise the next test for buf[i] would be 
	 * '\0', since we test the main loop for ccc anyway, we cant go SEGV
	 */
	i++;
      }
    } while ((ccc != '\0') && (ccc != ';'));

    return next_item(fp);
  }
}

static bool do_ascread(void *item,int nitem,int eio,
		       char *desc,char *srcfile,int line)
{
  FILE   *fp = curfio->fp;
  int    i,m,res=0,*iptr,ix;
  double d,x;
  real   *ptr;
  unsigned char *ucptr;
  char   *cptr;
  
  check_nitem();  
  switch (eio) {
  case eioREAL:
    res = sscanf(next_item(fp),"%lf",&d);
    if (item) *((real *)item) = d;
    break;
  case eioINT:
    res = sscanf(next_item(fp),"%d",&i);
    if (item) *((int *)item) = i;
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    for(i=0; (i<nitem); i++) {
      res = sscanf(next_item(fp),"%d",&ix);
      if (item) ucptr[i] = ix;
    }
    break;
  case eioUSHORT:
    res = sscanf(next_item(fp),"%d",&i);
    if (item) *((unsigned short *)item) = i;
    break;
  case eioRVEC:
    ptr = (real *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp),"%lf\n",&x);
      ptr[m] = x;
    }
    break;
  case eioNRVEC:
    for(i=0; (i<nitem); i++) {
      ptr = ((rvec *)item)[i];
      for(m=0; (m<DIM); m++) {
	res = sscanf(next_item(fp),"%lf\n",&x);
	if (item) ptr[m] = x;
      }
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    for(m=0; (m<DIM); m++) {
      res = sscanf(next_item(fp),"%d\n",&ix);
      if (item) iptr[m] = ix;
    }
    break;
  case eioSTRING:
    cptr = next_item(fp);
    if (item) {
      decode_string(strlen(cptr)+1,(char *)item,cptr);
      /* res = sscanf(cptr,"%s",(char *)item);*/
      res = 1;
    }
    break;
  default:
    FE();
  }
  if ((res <= 0) && curfio->bDebug)
    fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  
  return (res > 0);
}

static bool do_binwrite(void *item,int nitem,int eio,
			char *desc,char *srcfile,int line)
{
  size_t size=0,wsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    size = sizeof(real);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
    size = sizeof(rvec);
    break;
  case eioNRVEC:
    size = sizeof(rvec);
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    size = ssize = strlen((char *)item)+1;
    do_binwrite(&ssize,1,eioINT,desc,srcfile,line);
    break;
  default:
    FE();
  }
  wsize = fwrite(item,size,nitem,curfio->fp);
  
  if ((wsize != nitem) && curfio->bDebug) {
    fprintf(stderr,"Error writing %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
    fprintf(stderr,"written size %u bytes, source size %u bytes\n",
	    (unsigned int)wsize,(unsigned int)size);
  }
  return (wsize == nitem);
}

static bool do_binread(void *item,int nitem,int eio,
		       char *desc,char *srcfile,int line)
{
  size_t size=0,rsize;
  int    ssize;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble)
      size = sizeof(double);
    else
      size = sizeof(float);
    break;
  case eioINT:
    size = sizeof(int);
    break;
  case eioNUCHAR:
    size = sizeof(unsigned char);
    break;
  case eioUSHORT:
    size = sizeof(unsigned short);
    break;
  case eioRVEC:
  case eioNRVEC:
    if (curfio->bDouble)
      size = sizeof(double)*DIM;
    else
      size = sizeof(float)*DIM;
    break;
  case eioIVEC:
    size = sizeof(ivec);
    break;
  case eioSTRING:
    do_binread(&ssize,1,eioINT,desc,srcfile,line);
    size = ssize;
    break;
  default:
    FE();
  }
  if (item)
    rsize = fread(item,size,nitem,curfio->fp);
  else {
    /* Skip over it if we have a NULL pointer here */
#ifdef HAVE_FSEEKO
    fseeko(curfio->fp,(off_t)(size*nitem),SEEK_CUR);
#else
    fseek(curfio->fp,(size*nitem),SEEK_CUR);
#endif    
    rsize = nitem;
  }
  if ((rsize != nitem) && (curfio->bDebug))
    fprintf(stderr,"Error reading %s %s from file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
	    
  return (rsize == nitem);
}

#ifdef USE_XDR
static bool do_xdr(void *item,int nitem,int eio,
		   char *desc,char *srcfile,int line)
{
  unsigned char *ucptr;
  bool_t res=0;
  float  fvec[DIM];
  double dvec[DIM];
  int    j,m,*iptr,idum;
  real   *ptr;
  unsigned short us;
  double d=0;
  float  f=0;
  
  check_nitem();
  switch (eio) {
  case eioREAL:
    if (curfio->bDouble) {
      if (item && !curfio->bRead) d = *((real *)item);
      res = xdr_double(curfio->xdr,&d);
      if (item) *((real *)item) = d;
    }
    else {
      if (item && !curfio->bRead) f = *((real *)item);
      res = xdr_float(curfio->xdr,&f);
      if (item) *((real *)item) = f;
    }
    break;
  case eioINT:
    if (item && !curfio->bRead) idum = *(int *)item;
    res = xdr_int(curfio->xdr,&idum);
    if (item) *(int *)item = idum;
    break;
  case eioNUCHAR:
    ucptr = (unsigned char *)item;
    res   = 1;
    for(j=0; (j<nitem) && res; j++) {
      res = xdr_u_char(curfio->xdr,&(ucptr[j]));
    }
    
    break;
  case eioUSHORT:
    if (item && !curfio->bRead) us = *(unsigned short *)item;
    res = xdr_u_short(curfio->xdr,(unsigned short *)&us);
    if (item) *(unsigned short *)item = us;
    break;
  case eioRVEC:
    if (curfio->bDouble) {
      if (item && !curfio->bRead)
	for(m=0; (m<DIM); m++) 
	  dvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)dvec,DIM,(unsigned int)sizeof(double),
		     (xdrproc_t)xdr_double);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = dvec[m];
    }
    else {
      if (item && !curfio->bRead)
	for(m=0; (m<DIM); m++) 
	  fvec[m] = ((real *)item)[m];
      res=xdr_vector(curfio->xdr,(char *)fvec,DIM,(unsigned int)sizeof(float),
		     (xdrproc_t)xdr_float);
      if (item)
	for(m=0; (m<DIM); m++) 
	  ((real *)item)[m] = fvec[m];
    }
    break;
  case eioNRVEC:
    ptr = NULL;
    res = 1;
    for(j=0; (j<nitem) && res; j++) {
      if (item)
	ptr = ((rvec *)item)[j];
      res = do_xdr(ptr,1,eioRVEC,desc,srcfile,line);
    }
    break;
  case eioIVEC:
    iptr = (int *)item;
    res  = 1;
    for(m=0; (m<DIM) && res; m++) {
      if (item && !curfio->bRead) idum = iptr[m];
      res = xdr_int(curfio->xdr,&idum);
      if (item) iptr[m] = idum;
    }
    break;
  case eioSTRING: {
    char *cptr;
    int  slen;
    
    if (item) {
      if (!curfio->bRead) 
	slen = strlen((char *)item)+1;
      else
	slen = 0;
    }
    else
      slen = 0;
    
    if (xdr_int(curfio->xdr,&slen) <= 0)
      gmx_fatal(FARGS,"wrong string length %d for string %s"
		" (source %s, line %d)",slen,desc,srcfile,line);
    if (!item && curfio->bRead)
      snew(cptr,slen);
    else
      cptr=(char *)item;
    if (cptr) 
      res = xdr_string(curfio->xdr,&cptr,slen);
    else
      res = 1;
    if (!item && curfio->bRead)
      sfree(cptr);
    break;
  }
  default:
    FE();
  }
  if ((res == 0) && (curfio->bDebug))
    fprintf(stderr,"Error in xdr I/O %s %s to file %s (source %s, line %d)\n",
	    eioNames[eio],desc,curfio->fn,srcfile,line);
  return (res != 0);
}
#endif

#define gmx_fio_check(fio) range_check(fio,0,nFIO)

/*****************************************************************
 *
 *                     EXPORTED SECTION
 *
 *****************************************************************/
int gmx_fio_open(char *fn,char *mode)
{
  t_fileio *fio=NULL;
  int      i,nfio=0;
  char     *bf,newmode[5];
  bool     bRead;

  
  if (fn2ftp(fn)==efTPA)
    strcpy(newmode,mode);
  else {
    if (mode[0]=='r')
      strcpy(newmode,"r");
    else if (mode[0]=='w')
      strcpy(newmode,"w");
    else if (mode[0]=='a')
      strcpy(newmode,"a");
    else
      gmx_fatal(FARGS,"DEATH HORROR in gmx_fio_open, mode is '%s'",mode);
  }

  /* Check if it should be opened as a binary file */
  if (strncmp(ftp2ftype(fn2ftp(fn)),"ASCII",5)) {
    /* Not ascii, add b to file mode */
    if ((strchr(newmode,'b')==NULL) && (strchr(newmode,'B')==NULL))
      strcat(newmode,"b");
  }

  /* Determine whether we have to make a new one */
  for(i=0; (i<nFIO); i++)
    if (!FIO[i].bOpen) {
      fio  = &(FIO[i]);
      nfio = i;
      break;
    }
  if (i == nFIO) {
    nFIO++;
    srenew(FIO,nFIO);
    fio  = &(FIO[nFIO-1]);
    nfio = nFIO-1;
  }

  bRead = (newmode[0]=='r');
  fio->fp  = NULL;
  fio->xdr = NULL;
  if (fn) {
    fio->iFTP   = fn2ftp(fn);
    fio->fn     = strdup(fn);
    fio->bStdio = FALSE;
    
    /* If this file type is in the list of XDR files, open it like that */
    if (in_ftpset(fio->iFTP,asize(ftpXDR),ftpXDR)) {
      /* First check whether we have to make a backup,
       * only for writing, not for read or append.
       */
      if (newmode[0]=='w') {
	if (fexist(fn)) {
	  bf=(char *)backup_fn(fn);
	  if (rename(fn,bf) == 0) {
	    fprintf(stderr,"\nBack Off! I just backed up %s to %s\n",fn,bf);
	  }
	  else
	    fprintf(stderr,"Sorry, I couldn't backup %s to %s\n",fn,bf);
	}
      }
      else {
	/* Check whether file exists */
	if (!fexist(fn))
	  gmx_open(fn);
      }
      snew(fio->xdr,1);
      if (!xdropen(fio->xdr,fn,newmode))
	gmx_open(fn);
    }
    else {
      /* If it is not, open it as a regular file */
      fio->fp = ffopen(fn,newmode);
    }
  }
  else {
    /* Use stdin/stdout for I/O */
    fio->iFTP   = efTPA;
    fio->fp     = bRead ? stdin : stdout;
    fio->fn     = strdup("STDIO");
    fio->bStdio = TRUE;
  }
  fio->bRead  = bRead;
  fio->bDouble= (sizeof(real) == sizeof(double));
  fio->bDebug = FALSE;
  fio->bOpen  = TRUE;

  return nfio;
}


void gmx_fio_close(int fio)
{
  gmx_fio_check(fio);
  
  if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
    xdrclose(FIO[fio].xdr);
    sfree(FIO[fio].xdr);
  }
  else {
    /* Don't close stdin and stdout! */
    if (!FIO[fio].bStdio)
      fclose(FIO[fio].fp);
  }

  sfree(FIO[fio].fn);
  FIO[fio].bOpen = FALSE;
  do_read  = do_dummy;
  do_write = do_dummy;
}

void gmx_fio_select(int fio)
{
  gmx_fio_check(fio);
#ifdef DEBUG
  fprintf(stderr,"Select fio called with type %d for file %s\n",
	  FIO[fio].iFTP,FIO[fio].fn);
#endif

  if (in_ftpset(FIO[fio].iFTP,asize(ftpXDR),ftpXDR)) {
#ifdef USE_XDR    
    do_read  = do_xdr;
    do_write = do_xdr;
#else
    gmx_fatal(FARGS,"Sorry, no XDR");
#endif
  }
  else if (in_ftpset(FIO[fio].iFTP,asize(ftpASC),ftpASC)) {
    do_read  = do_ascread;
    do_write = do_ascwrite;
  }
  else if (in_ftpset(FIO[fio].iFTP,asize(ftpBIN),ftpBIN)) {
    do_read  = do_binread;
    do_write = do_binwrite;
  }
#ifdef HAVE_XMl
  else if (in_ftpset(FIO[fio].iFTP,asize(ftpXML),ftpXML)) {
    do_read  = do_dummy;
    do_write = do_dummy;
  }
#endif
  else 
    gmx_fatal(FARGS,"Can not read/write topologies to file type %s",
	      ftp2ext(curfio->iFTP));
  
  curfio = &(FIO[fio]);
}

void gmx_fio_setprecision(int fio,bool bDouble)
{
  gmx_fio_check(fio);
  FIO[fio].bDouble = bDouble;
}

bool gmx_fio_getdebug(int fio)
{
  gmx_fio_check(fio);
  return FIO[fio].bDebug;
}

void gmx_fio_setdebug(int fio,bool bDebug)
{
  gmx_fio_check(fio);
  FIO[fio].bDebug = bDebug;
}

char *gmx_fio_getname(int fio)
{
  gmx_fio_check(fio);
  return curfio->fn;
}

void gmx_fio_setftp(int fio,int ftp)
{
  gmx_fio_check(fio);
  FIO[fio].iFTP = ftp;
}

int gmx_fio_getftp(int fio)
{
  gmx_fio_check(fio);
  return FIO[fio].iFTP;
}
 
void gmx_fio_rewind(int fio)
{
  gmx_fio_check(fio);
  if (FIO[fio].xdr) {
    xdrclose(FIO[fio].xdr);
    /* File is always opened as binary by xdropen */
    xdropen(FIO[fio].xdr,FIO[fio].fn,FIO[fio].bRead ? "r" : "w");
  }
  else
    frewind(FIO[fio].fp);
}

void gmx_fio_flush(int fio)
{
  gmx_fio_check(fio);
  if (FIO[fio].fp)
    fflush(FIO[fio].fp);
 if (FIO[fio].xdr)
      (void) fflush ((FILE *) FIO[fio].xdr->x_private);
}
  
off_t gmx_fio_ftell(int fio)
{
  gmx_fio_check(fio);
  if (FIO[fio].fp)
    return ftell(FIO[fio].fp);
  else
    return 0;
}

void gmx_fio_seek(int fio, off_t fpos)
{
  gmx_fio_check(fio);
  if (FIO[fio].fp)
#ifdef HAVE_FSEEKO
    fseeko(FIO[fio].fp,fpos,SEEK_SET);
#else
    fseek(FIO[fio].fp,fpos,SEEK_SET);
#endif
  else
    gmx_file(FIO[fio].fn);
}

FILE *gmx_fio_getfp(int fio)
{
  gmx_fio_check(fio);
  if (FIO[fio].fp)
    return FIO[fio].fp;
  else
    return NULL;
}

XDR *gmx_fio_getxdr(int fio)
{
  gmx_fio_check(fio);
  if (FIO[fio].xdr) 
    return FIO[fio].xdr;
  else
    return NULL;
}

bool gmx_fio_getread(int fio)
{
  gmx_fio_check(fio);
  
  return FIO[fio].bRead;
}
