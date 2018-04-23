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

//*****************************************************************************
// system includes
//*****************************************************************************

#include <stdlib.h>
#include <string.h>
#include <limits.h>

//#define GNU_SOURCE
#include <link.h>  // dl_iterate_phdr
#include <dlfcn.h> // dladdr


//*****************************************************************************
// local includes
//*****************************************************************************

#include "dylib.h"
#include "fnbounds_interface.h"

#include "proc_self_maps.h"

#include <messages/messages.h>



//*****************************************************************************
// private operations
//*****************************************************************************

static int
proc_self_maps_map_open_dsos
(
  proc_self_maps_segment_t *s, 
  void *arg
)
{
  // if the segment is executable 
  if (s->perms & proc_self_maps_segment_perm_x) {
    // ensurre that it is entered into hpcrun's loadmap
    fnbounds_ensure_mapped_dso(s->pathname, s->start, s->end);
  }
  return 0; // continue the iteration invoking this callback
}



//*****************************************************************************
// interface operations
//*****************************************************************************

int
dylib_is_pseudo_path(const char *path)
{
  return proc_self_maps_is_pseudo_path(path);
}


//------------------------------------------------------------------
// ensure bounds information computed for all open shared libraries
//------------------------------------------------k-----------------
void 
dylib_map_open_dsos()
{
  proc_self_maps_segment_iterate(proc_self_maps_map_open_dsos, 0);
}


//------------------------------------------------------------------
// ensure bounds information computed for the executable
//------------------------------------------------------------------

void 
dylib_map_executable()
{
  const char *executable_name = "/proc/self/exe";
  fnbounds_ensure_mapped_dso(executable_name, NULL, NULL);
}


int 
dylib_addr_is_mapped(void *addr) 
{
  proc_self_maps_segment_t s;
  return proc_self_maps_address_to_segment(addr, &s); 
}


int 
dylib_find_module_containing_addr
(
  void *addr, 
  char *module_name,
  void **start, 
  void **end
)
{
  int retval = 0; // not found
  proc_self_maps_segment_t s;

  if (proc_self_maps_address_to_segment(addr, &s)) {
    strcpy(module_name, s.pathname);
    *start = s.start;
    *end = s.end;
    retval = 1;
  }
  return retval;
}


int 
dylib_find_proc(void* pc, void* *proc_beg, void* *mod_beg)
{
  Dl_info dli;
  int ret = dladdr(pc, &dli); // cf. glibc's _dl_addr
  if (ret) {
    //printf("dylib_find_proc: lm: %s (%p); sym: %s (%p)\n", dli.dli_fname, dli.dli_fbase, dli.dli_sname, dli.dli_saddr);
    *proc_beg = dli.dli_saddr;
    *mod_beg  = dli.dli_fbase;
    return 0;
  }
  else {
    *proc_beg = NULL;
    *mod_beg = NULL;
    return -1;
  }
}


bool
dylib_isin_start_func(void* pc)
{
  extern int __libc_start_main(void); // start of a process
  extern int __clone(void);           // start of a thread (extern)
  extern int clone(void);             // start of a thread (weak)

  void* proc_beg = NULL, *mod_beg = NULL;
  dylib_find_proc(pc, &proc_beg, &mod_beg);
  return (proc_beg == __libc_start_main || 
	  proc_beg == clone || proc_beg == __clone);
}


const char* 
dylib_find_proc_name(const void* pc)
{
  Dl_info dli;
  int ret = dladdr(pc, &dli);
  if (ret) {
    return (dli.dli_sname) ? dli.dli_sname : dli.dli_fname;
  }
  else {
    return NULL;
  }
}

