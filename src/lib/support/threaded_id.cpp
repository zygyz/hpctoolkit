// -*-Mode: C++;-*-

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
// Copyright ((c)) 2002-2017, Rice University
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


//****************************************************************************
// user include files
//****************************************************************************

#include <lib/prof-lean/stdatomic.h>
#include "threaded_id.hpp"


//****************************************************************************
// race detection
//****************************************************************************

//----------------------------------------------------------------------------
// NOTE: 
//   when accessing thread-local data, use a fake lock to prevent 
//   cilkscreen from reporting a race. cilkscreen doesn't understand 
//   thread-local data.
//----------------------------------------------------------------------------

#ifdef CILKSCREEN
#include <lib/support/fake_lock.h>
#endif



//****************************************************************************
// local variables
//****************************************************************************

static atomic_uint next_thread_id = ATOMIC_VAR_INIT(1); 

// 0 = uninitialized; when positive, value = thread id + 1
static __thread uint thread_id_plus1 = 0;  

static __thread uint unique_cnt; 



//****************************************************************************
// private operations
//****************************************************************************

static uint 
reversebits(uint val)
{
  uint offset = sizeof(uint) << 3; // size of type in bits

  uint result = 0;
     
  while (val) {
      result <<= 1;      // make room for new bit from val
      result |= val & 1; // transfer a bit from val to result
      offset--;          // reduce shift offset needed to complete reversal
      val >>= 1;         // advance to the next bit in val
  }

  return result << offset;
}



//****************************************************************************
// interface operations
//****************************************************************************

uint
threaded_thread_id()
{
  uint result;

#ifdef CILKSCREEN
  fake_lock_acquire();
#endif

  if (!thread_id_plus1) {
    // assign a thread a small unique positive number to be used to compute
    // thread id
    thread_id_plus1 = atomic_fetch_add(&next_thread_id, 1);
  }
  result = thread_id_plus1 - 1; // return a non-negative thread id

#ifdef CILKSCREEN
  fake_lock_release();
#endif

  return result;
}


uint
threaded_unique_id(uint increment)
{

#ifdef CILKSCREEN
  fake_lock_acquire();
#endif

  if (!unique_cnt) {
    unique_cnt = reversebits(threaded_thread_id() + 1);
  }

  uint result = unique_cnt;

  unique_cnt += increment;

#ifdef CILKSCREEN
  fake_lock_release();
#endif

  return result;
}
