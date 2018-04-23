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

#include <sys/param.h>



//******************************************************************************
// types
//******************************************************************************

typedef enum {
  proc_self_maps_segment_perm_r = 1,
  proc_self_maps_segment_perm_w = 2, 
  proc_self_maps_segment_perm_x = 4, 
  proc_self_maps_segment_perm_p = 8,
  proc_self_maps_segment_perm_s = 16 
} proc_self_maps_segment_perm_t;


typedef struct {
  void *start;               // segment start address   
  void *end;                 // segment end address
  unsigned int perms;        // segment permissions
  unsigned long offset;      // offset into file/whatever
  unsigned long major;       // major device
  unsigned long minor;       // minor device
  unsigned long inode;       // inode of the file that backs the area
  char pathname[MAXPATHLEN]; // the path to the file associated with the segment
} proc_self_maps_segment_t;


typedef int (*proc_self_maps_callback_t) // return zero to continue iteration 
(
  proc_self_maps_segment_t *s, 
  void *arg
);



//******************************************************************************
// interface operations
//******************************************************************************

extern int
proc_self_maps_is_pseudo_path
(
  const char *path
);


extern void
proc_self_maps_segment_print
(
  proc_self_maps_segment_t *s
);


extern int
proc_self_maps_address_to_segment
(
  void *addr,
  proc_self_maps_segment_t *s
);


extern int
proc_self_maps_segment_iterate
(
  proc_self_maps_callback_t callback,
  void *arg
);
