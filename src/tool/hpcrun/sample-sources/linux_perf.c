// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2016, Rice University
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

//
// Linux perf sample source simple oo interface
//


/******************************************************************************
 * system includes
 *****************************************************************************/

#include <assert.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>        // printf
#include <stdlib.h>       // getenv
#include <sys/syscall.h>  // SYS_gettid
#include <time.h>
#include <errno.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <string.h>       // strlen
#include <unistd.h>       // write
#include <fcntl.h>



/******************************************************************************
 * libmonitor
 *****************************************************************************/
#include <monitor.h>

/******************************************************************************
 * local includes
 *****************************************************************************/

#include "simple_oo.h"
#include "sample_source_obj.h"
#include "common.h"

#include <hpcrun/cct_insert_backtrace.h>
#include <hpcrun/hpcrun_stats.h>
#include <hpcrun/loadmap.h>
#include <hpcrun/messages/messages.h>
#include <hpcrun/metrics.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/sample_sources_registered.h>
#include <hpcrun/sample-sources/blame-shift/blame-shift.h>
#include <hpcrun/utilities/tokenize.h>

#include <lib/prof-lean/hpcrun-fmt.h>



//******************************************************************************
// macros
//******************************************************************************

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id  _sigev_un._tid
#endif

#define THREAD_SELF 	 	 0
#define CPU_ANY 	        -1
#define GROUP_FD 	        -1
#define PERF_FLAGS 		 0
#define PERF_REQUEST_0_SKID 	 2
#define PERF_WAKEUP_EACH_SAMPLE  1

#define MMAP_OFFSET_0 0

#define EXCLUDE_CALLCHAIN_USER   1

#define DEFAULT_THRESHOLD  2000000L

// FIXME: why are these not defined in Linux include files?
#define rmb()
#define mb()



//******************************************************************************
// type declarations
//******************************************************************************

struct perf_event_callchain {
	struct perf_event_header header;
	uint64_t   nr;         /* number of IPs */ 
	uint64_t   ips[];     /* vector of IPs */
};



/******************************************************************************
 * external thread-local variables
 *****************************************************************************/

extern __thread bool hpcrun_thread_suppress_sample;



//******************************************************************************
// forward declarations 
//******************************************************************************

void perf_thread_init();
void perf_thread_fini();


//******************************************************************************
// local variables
//******************************************************************************

static int the_signal = SIGIO;

static uint16_t perf_kernel_lm_id;

static bool perf_ksyms_avail;

static int perf_process_state;
static int perf_initialized;

static int metric_id;
static long threshold;

static sigset_t sig_mask;

static int pagesize;
static const char *event_name = "PERF_COUNT_HW_CPU_CYCLES";


// Special case to make perf init a soft failure.
// Make sure that we don't use perf if it won't work.
static int perf_unavail = 0;



//******************************************************************************
// thread local variables
//******************************************************************************

int                  	    __thread perf_thread_initialized;
int                         __thread perf_thread_state;

long                        __thread my_kernel_samples;
long                        __thread my_user_samples;

int                         __thread myid;

int                         __thread perf_threadinit = 0;
long                        __thread perf_started = 0;

struct sigevent             __thread sigev;
struct timespec             __thread real_start; 
struct timespec             __thread cpu_start; 

int   __thread perf_fd;
struct perf_event_mmap_page __thread *perf_mmap;
struct perf_event_callchain __thread *perf_callchain;
int                         __thread perf_mmap_size;



//******************************************************************************
// private operations 
//******************************************************************************

static long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
	       int cpu, int group_fd, unsigned long flags)
{
   int ret;

   ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
   return ret;
}


static void
perf_start()
{
  int ret;

  monitor_real_pthread_sigmask(SIG_UNBLOCK, &sig_mask, NULL);

  ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0);

  fcntl(perf_fd, F_SETFL,  O_ASYNC | O_NONBLOCK);

  struct f_owner_ex owner;
  owner.type = F_OWNER_TID;
  owner.pid  = syscall(SYS_gettid);
  ret = fcntl(perf_fd, F_SETOWN_EX, &owner);

  if (ret == -1) {
    fprintf(stderr, "can't set fcntl(F_SETOWN_EX) on %d: %s\n", 
	    perf_fd, strerror(errno));
  }

  perf_started = 1;
}


static void
perf_stop()
{
  if (!perf_initialized) return;

  monitor_real_pthread_sigmask(SIG_BLOCK, &sig_mask, NULL);
  ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);

  perf_started = 0;
}


void
perf_dump_callchain()
{
  int print;

  char *pc = (char *) perf_callchain;
  struct perf_event_callchain *sd = 
    (struct perf_event_callchain *) (pc + (perf_mmap->data_tail % pagesize));
  int i;

  print = (sd->nr > 0);

  if (print) {
    for (i=0; i < sd->nr; i++) {
      printf("%d 0x%lx\n", myid, sd->ips[i]);
    }
    printf("\n");
  }
  perf_mmap->data_head = 0;
  perf_mmap->data_tail = 0;
}


bool
perf_kernel_syms_avail()
{
  FILE *ksyms = fopen(HPCRUN_FMT_KERNEL_SYMBOLS, "r");
  if (ksyms != NULL) {
    fclose(ksyms);
    return true;
  }
  return false;
}


cct_node_t *
add_kernel_callchain(cct_node_t *leaf)
{
  char *pcc_v = (char *) perf_callchain;
  struct perf_event_callchain *pcc = 
    (struct perf_event_callchain *) (pcc_v + (perf_mmap->data_tail % pagesize));

  cct_node_t *parent = leaf;
  for (int i = pcc->nr - 1; i > 0; i--) {
    ip_normalized_t npc = { .lm_id = perf_kernel_lm_id, .lm_ip = pcc->ips[i] };
    cct_addr_t frm = { .ip_norm = npc };
    cct_node_t *child = hpcrun_cct_insert_addr(parent, &frm);
    parent = child;
  }
  perf_mmap->data_head = 0;
  perf_mmap->data_tail = 0;

  return parent;
}


void
perf_attr_init(struct perf_event_attr *attr)
{
  memset(attr, 0, sizeof(struct perf_event_attr));

  attr->type = PERF_TYPE_HARDWARE;
  attr->size = sizeof(struct perf_event_attr);
  attr->config = PERF_COUNT_HW_CPU_CYCLES;

  attr->sample_period = threshold;
  attr->sample_type = PERF_SAMPLE_CALLCHAIN;
  attr->precise_ip = PERF_REQUEST_0_SKID;
  attr->wakeup_events = PERF_WAKEUP_EACH_SAMPLE;
  attr->sample_stack_user = 4096;


  if (perf_ksyms_avail) {
    attr->sample_type = PERF_SAMPLE_CALLCHAIN;
    attr->exclude_callchain_user = EXCLUDE_CALLCHAIN_USER;
  } else {
    attr->sample_type = PERF_SAMPLE_IP;
  }

}


void
perf_thread_init()
{
  if (perf_thread_initialized == 0) {
    struct perf_event_attr attr;
    perf_attr_init(&attr);
    perf_fd = perf_event_open(&attr, THREAD_SELF, CPU_ANY, 
			      GROUP_FD, PERF_FLAGS);

    perf_mmap_size = pagesize * 2;
    void *map_result = mmap(NULL, perf_mmap_size, PROT_WRITE | PROT_READ, 
			    MAP_SHARED, perf_fd, MMAP_OFFSET_0);

    if (map_result == MAP_FAILED) {
      EMSG("Linux perf mmap failed: %s", strerror(errno));
    }

    char *mm = (char *) map_result;

    perf_mmap  = (struct perf_event_mmap_page *) mm;
    perf_callchain = (struct perf_event_callchain *) (mm + pagesize);

    if (perf_mmap) {
      memset(perf_mmap, 0, sizeof(struct perf_event_mmap_page));
      perf_mmap->version = 0; 
      perf_mmap->compat_version = 0; 
      perf_mmap->data_head = 0; 
      perf_mmap->data_tail = 0; 
    }
    perf_thread_initialized = 1;
  }
}


void
perf_thread_fini()
{
  munmap(perf_mmap, perf_mmap_size);
  close(perf_fd);
  perf_thread_initialized = 0;
}


static int
perf_event_handler(int sig, siginfo_t* siginfo, void* context)
{
  void *pc = 0; // FIXME

  ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);

  // if sampling disabled explicitly for this thread, skip all processing
  if (hpcrun_thread_suppress_sample) 
    return 0; // tell monitor the signal has been handled.

  // If the interrupt came from inside our code, then drop the sample
  // and return and avoid any MSG.
  if (! hpcrun_safe_enter_async(pc)) {
    hpcrun_stats_num_samples_blocked_async_inc();
    return 0; // tell monitor the signal has been handled.
  }

  // perf_dump_callchain();

  sample_val_t sv = hpcrun_sample_callpath(context, metric_id, 1,
					   0/*skipInner*/, 0/*isSync*/);

  // add_kernel_callchain(sv.sample_node);

  blame_shift_apply(metric_id, sv.sample_node, 1 /*metricIncr*/);

  hpcrun_safe_exit();
  return 0; // tell monitor the signal has been handled.
}


static void 
perf_init()
{
  perf_ksyms_avail = perf_kernel_syms_avail();
  pagesize = sysconf(_SC_PAGESIZE);
  sigemptyset(&sig_mask);
  sigaddset(&sig_mask, the_signal);

  if (perf_ksyms_avail) {
    hpcrun_kernel_callpath_register(add_kernel_callchain);
    perf_kernel_lm_id = 
      hpcrun_loadModule_add(HPCRUN_FMT_KERNEL);
  }
 
  monitor_sigaction(the_signal, &perf_event_handler, 0, NULL);
}



/******************************************************************************
 * method functions
 *****************************************************************************/

static void
METHOD_FN(init)
{
  perf_process_state = INIT;
  self->state = INIT;
}


static void
METHOD_FN(thread_init)
{
  TMSG(LINUX_PERF, "thread init");
  if (perf_unavail) { return; }

  perf_thread_state = INIT;

  TMSG(LINUX_PERF, "thread init OK");
}


static void
METHOD_FN(thread_init_action)
{
  TMSG(LINUX_PERF, "thread init action");

  if (perf_unavail) { return; }

  perf_thread_state = INIT;
  perf_thread_init();
}


static void
METHOD_FN(start)
{
  TMSG(LINUX_PERF, "start");

  if (perf_unavail) { 
    return; 
  }

  // make LINUX_PERF start idempotent.  the application can turn on sampling
  // anywhere via the start-stop interface, so we can't control what
  // state LINUX_PERF is in.

  if (perf_thread_state == START) {
    TMSG(LINUX_PERF,"*NOTE* LINUX_PERF start called when already in state START");
    return;
  }

  perf_start();

  perf_thread_state = START;
}

static void
METHOD_FN(thread_fini_action)
{
  TMSG(LINUX_PERF, "unregister thread");
  if (perf_unavail) { return; }

  perf_thread_fini();
}


static void
METHOD_FN(stop)
{
  TMSG(LINUX_PERF, "stop");

  if (perf_unavail) return; 

  if (perf_thread_state == STOP) {
    TMSG(LINUX_PERF,"*NOTE* PERF stop called when already in state STOP");
    return;
  }

  if (perf_thread_state != START) {
    TMSG(LINUX_PERF,"*WARNING* PERF stop called when not in state START");
    return;
  }

  perf_stop();

  perf_thread_state = STOP;
}

static void
METHOD_FN(shutdown)
{
  TMSG(LINUX_PERF, "shutdown");

  if (perf_unavail) { return; }

  METHOD_CALL(self, stop); // make sure stop has been called
  // FIXME: add component shutdown code here

  perf_thread_fini();

  perf_process_state = UNINIT;
}


// Return true if Linux perf recognizes the name, whether supported or not.
// We'll handle unsupported events later.
static bool
METHOD_FN(supports_event, const char *ev_str)
{
  TMSG(LINUX_PERF, "supports event");
  if (perf_unavail) { return false; }

  if (perf_process_state == UNINIT){
    METHOD_CALL(self, init);
  }

  if (strncmp(event_name, ev_str, strlen(event_name)) == 0) return true; 
  
  return false;
}

 
static void
METHOD_FN(process_event_list, int lush_metrics)
{
  metric_desc_properties_t prop = metric_property_none;
  prop = metric_property_cycles;

  TMSG(LINUX_PERF, "process event list");

  if (perf_unavail) { return; }

#if 0
  char* evlist = METHOD_CALL(self, get_event_str);

  char *event;
  for (event = start_tok(evlist); more_tok(); event = next_tok()) {
    char name[1024];

    TMSG(LINUX_PERF,"checking event spec = %s",event);

    hpcrun_extract_ev_thresh(event, sizeof(name), name, &threshold, DEFAULT_THRESHOLD);

  }
#endif
  char *name = "PERF_COUNT_HW_CPU_CYCLES";
  threshold = DEFAULT_THRESHOLD;

  perf_initialized = true;
  perf_init();
  perf_thread_init();

  metric_id = hpcrun_new_metric();

  hpcrun_set_metric_info_and_period(metric_id, name,
				    MetricFlags_ValFmt_Int,
				    threshold, prop);
}


static void
METHOD_FN(gen_event_set, int lush_metrics)
{
}


static void
METHOD_FN(display_events)
{
  printf("===========================================================================\n");
  printf("Available Linux perf events\n");
  printf("===========================================================================\n");
  printf("Name\t\tDescription\n");
  printf("---------------------------------------------------------------------------\n");
  printf("%s\tTotal cycles.\n", 
         "PERF_COUNT_HW_CPU_CYCLES");
  printf("\n");
}



/***************************************************************************
 * object
 ***************************************************************************/

#define ss_name linux_perf
#define ss_cls SS_HARDWARE

#include "ss_obj.h"

