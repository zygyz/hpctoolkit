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

//***************************************************************************
//
// File:
//   fake_lock.c
//
// Purpose:
//   inform the cilk race detector that the race detector can pretend 
//   that a code sequence executed under mutual exclusion 
//
// Description:
//   void fake_lock_acquire(void)
//
//   void fake_lock_release(void)
//
// Note:
//   the contents of this file will be ignored unless CILKSCREEN is defined
//
//***************************************************************************

#ifdef CILKSCREEN

//****************************************************************************
// system include files
//****************************************************************************

#ifdef __INTEL_COMPILER
#include <cilktools/cilkscreen.h>
#endif



//****************************************************************************
// local include files
//****************************************************************************

#include "fake_lock.h"



//****************************************************************************
// private data
//****************************************************************************

#ifdef __INTEL_COMPILER
static int fake_lock; // this is a placeholder only 
#endif



//****************************************************************************
// public operations
//****************************************************************************

void fake_lock_acquire(void)
{
#ifdef __INTEL_COMPILER
    __cilkscreen_acquire_lock(&fake_lock);
#endif
}


void fake_lock_release(void)
{
#ifdef __INTEL_COMPILER
    __cilkscreen_release_lock(&fake_lock);
#endif
}

#endif
