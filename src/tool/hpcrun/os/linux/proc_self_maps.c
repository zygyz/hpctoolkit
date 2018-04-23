// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// $HeadURL$
// $Id$
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2018, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *


//******************************************************************************
// global includes
//******************************************************************************

#include <fcntl.h>   // for O_RDONLY
#include <stdio.h>   // for printf
#include <string.h>  // for memset, memcpy
#include <unistd.h>  // for open/close



//******************************************************************************
// local includes
//******************************************************************************

#include "proc_self_maps.h"



//******************************************************************************
// macros
//******************************************************************************

#define NO_FD -1
#define BUF_LEN 4096

#define BUFSIZE (MAXPATHLEN << 1)
#define LM_EOF -1

#define R_PERM_OFFSET  0
#define W_PERM_OFFSET  1
#define X_PERM_OFFSET  2
#define SP_PERM_OFFSET 3



//******************************************************************************
// types
//******************************************************************************

typedef struct {
  char buffer[BUF_LEN];
  int buf_pos;
  int buf_len;
  int eof;
  int fd;
} proc_self_maps_file_t;


typedef struct {
  proc_self_maps_segment_t *s;
  void *addr;
} proc_self_maps_address_to_segment_helper_t;

typedef struct {
  char perms[5]; // 4 permissions + NULL terminator
} proc_self_maps_segment_perm_string_t;
 


//******************************************************************************
// private operations
//******************************************************************************

static int
proc_self_maps_open
(
  proc_self_maps_file_t *lmf
)
{
  memset(lmf, 0, sizeof(proc_self_maps_file_t));
  lmf->fd = open("/proc/self/maps", O_RDONLY);
  return lmf->fd != NO_FD;
}


static int
proc_self_maps_close
(
  proc_self_maps_file_t *lmf
)
{
  int retval = close(lmf->fd);
  memset(lmf, 0, sizeof(proc_self_maps_file_t));
  lmf->fd = NO_FD;
  return retval;
}


static int
proc_self_maps_getc
(
  proc_self_maps_file_t *lmf
)
{
  if (lmf->eof) return LM_EOF;
  if (lmf->buf_pos == lmf->buf_len) {
    lmf->buf_len = read(lmf->fd, lmf->buffer, sizeof(lmf->buffer)); 
    lmf->buf_pos = 0;
    if (lmf->buf_len == 0) {
      lmf->eof = 1;
      return LM_EOF;
    }
  }
  return lmf->buffer[lmf->buf_pos++]; 
}


static int
proc_self_maps_getline
(
  proc_self_maps_file_t *lmf, 
  char *buffer, 
  size_t len
)
{
  int i = 0;
  for (;;) {
    int c = proc_self_maps_getc(lmf);
    if (c == LM_EOF) break;
    if (c == '\n') {
      buffer[i++] = 0;
      break;
    }
    buffer[i++] = c;
    if (i == len - 1) break;
  }
  return i;
}


static void
skip_separator
(
  char **cursor
)
{
  char *nextc = *cursor;
  while (*nextc == ' ' || *nextc == ':' || *nextc == '-') nextc++;
  *cursor = nextc;
}


static long
gethex(char **cursor)
{
   char *nextc = *cursor;
   long result = 0;
   for(;;) {
     int val;
     int digit = *nextc++;
     if (digit >= 'a' && digit <= 'f') val = digit - 'a' + 10;
     else if (digit >= '0' && digit <= '9') val = digit - '0';
     else break;      
     result <<= 4;
     result += val;
   }
   *cursor = nextc - 1;
   return result;
}


static long
getdec
(
  char **cursor
)
{
   char *nextc = *cursor;
   long result = 0;
   for(;;) {
     int val;
     int digit = *nextc++;
     if (digit >= '0' && digit <= '9') val = digit - '0';
     else break;      
     result *= 10;
     result += val;
   }
   *cursor = nextc - 1;
   return result;
}


static void *
getaddr
(
  char **cursor
)
{
   return (void *) gethex(cursor);
}


static int
getperms
(
  char **cursor
)
{
  char *perms = *cursor;
  int result = 0;
  while(*perms != ' ') {
    switch(*perms) {
    case 'r': result |= proc_self_maps_segment_perm_r; break;
    case 'w': result |= proc_self_maps_segment_perm_w; break;
    case 'x': result |= proc_self_maps_segment_perm_x; break;
    case 'p': result |= proc_self_maps_segment_perm_p; break;
    case 's': result |= proc_self_maps_segment_perm_s; break;
    default: break;
    }
    perms++;
  }
  *cursor = perms;
  return result;
}


static int 
proc_self_maps_segment_contains
(
  proc_self_maps_segment_t *s,
  void *addr
)
{
  return (s->start <= addr) && (addr < s->end);
}


static void
proc_self_maps_segment_parse
(
  proc_self_maps_segment_t *s,
  char *line
)
{
  char *cursor = line;
  s->start = getaddr(&cursor);
  skip_separator(&cursor);
  s->end = getaddr(&cursor);
  skip_separator(&cursor);
  s->perms = getperms(&cursor);
  skip_separator(&cursor);
  s->offset = gethex(&cursor);
  skip_separator(&cursor);
  s->major = gethex(&cursor);
  skip_separator(&cursor);
  s->minor = gethex(&cursor);
  skip_separator(&cursor);
  s->inode = getdec(&cursor);
  skip_separator(&cursor);
  strncpy(s->pathname, cursor, sizeof(s->pathname));
}


static int
proc_self_maps_address_to_segment_callback
(
  proc_self_maps_segment_t *s, 
  void *arg
)
{
  proc_self_maps_address_to_segment_helper_t *helper = (proc_self_maps_address_to_segment_helper_t *) arg;
  int result = proc_self_maps_segment_contains(s, helper->addr);
  if (result) {
    memcpy(helper->s, s, sizeof(*s));
  }
  return result;
}  


static void
proc_self_maps_segment_perm_string_init
(
  proc_self_maps_segment_perm_string_t *p 
)
{
  int bytes = sizeof(p->perms);

  memset(p->perms, '-', bytes - 1); // initialize all but last byte
  p->perms[bytes - 1] = 0;          // initialize last byte
}


static char *
proc_self_maps_get_perm_string
(
  proc_self_maps_segment_t *s, 
  proc_self_maps_segment_perm_string_t *p 
)
{
  proc_self_maps_segment_perm_string_init(p);

  if (s->perms & proc_self_maps_segment_perm_r) p->perms[R_PERM_OFFSET]  = 'r';
  if (s->perms & proc_self_maps_segment_perm_w) p->perms[W_PERM_OFFSET]  = 'w';
  if (s->perms & proc_self_maps_segment_perm_x) p->perms[X_PERM_OFFSET]  = 'x';
  if (s->perms & proc_self_maps_segment_perm_s) p->perms[SP_PERM_OFFSET] = 's';
  if (s->perms & proc_self_maps_segment_perm_p) p->perms[SP_PERM_OFFSET] = 'p';

  return p->perms;
}



//******************************************************************************
// interface operations
//******************************************************************************

int
proc_self_maps_is_pseudo_path
(
  const char *path
)
{
  return path[0] == '[';
}


void
proc_self_maps_segment_print
(
  proc_self_maps_segment_t *s
)
{
  proc_self_maps_segment_perm_string_t p;
  
  printf("%p-%p %s %08lx %02lx:%02lx %ld '%s'\n", 
         s->start, 
         s->end, 
         proc_self_maps_get_perm_string(s, &p),
         s->offset, 
         s->major, s->minor, 
         s->inode, 
         s->pathname);
}


int
proc_self_maps_segment_iterate
(
  proc_self_maps_callback_t callback,
  void *arg
)
{
  int result = 0;
  char buffer[BUFSIZE];
  proc_self_maps_file_t lmf;

  proc_self_maps_open(&lmf);
  while (proc_self_maps_getline(&lmf, buffer, BUFSIZE) != 0) {
    proc_self_maps_segment_t s;
    proc_self_maps_segment_parse(&s, buffer);
    if (callback(&s, arg)) {
       result = 1; 
       break;
    }
  }
  proc_self_maps_close(&lmf);

  return result;
}


int
proc_self_maps_address_to_segment
(
  void *addr,
  proc_self_maps_segment_t *s
)
{
  proc_self_maps_address_to_segment_helper_t helper;
  helper.s = s;
  helper.addr = addr;
  return proc_self_maps_segment_iterate(proc_self_maps_address_to_segment_callback, &helper);
}
