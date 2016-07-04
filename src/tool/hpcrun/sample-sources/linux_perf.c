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
// Linux perf sample source interface
//


/******************************************************************************
 * system includes
 *****************************************************************************/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/perf_event.h>

#include <sys/syscall.h> 
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>



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
#include <hpcrun/utilities/arch/context-pc.h>

#include <include/linux_info.h> 



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

#define PERF_SIGNAL SIGIO

#define PERF_DATA_PAGE_EXP        0      // use 2^PERF_DATA_PAGE_EXP pages
#define PERF_DATA_PAGES           (1 << PERF_DATA_PAGE_EXP)  

#define BUFFER_FRONT              ((char *) perf_mmap + pagesize)
#define BUFFER_SIZE               (tail_mask + 1)
#define BUFFER_OFFSET(tail)       ((tail) & tail_mask)

#define PERF_MMAP_SIZE(pagesz)    ((pagesz) * (PERF_DATA_PAGES + 1)) 
#define PERF_TAIL_MASK(pagesz)    (((pagesz) * PERF_DATA_PAGES) - 1) 

#define PERF_EVENT_AVAILABLE_UNKNOWN 0
#define PERF_EVENT_AVAILABLE_NO      1
#define PERF_EVENT_AVAILABLE_YES     2

#define mp_none { }
#define mp_cycles { .cycles = 1 }

#define ETABLE_ENTRY(type, name, properties, value_format) \
    { type, #name, name, properties, value_format}

#define ETABLE_ENTRY_HW(name) \
   ETABLE_ENTRY(PERF_TYPE_HARDWARE, name, mp_none, MetricFlags_ValFmt_Int)

#define ETABLE_ENTRY_HW_CYC(name) \
   ETABLE_ENTRY(PERF_TYPE_HARDWARE, name, mp_cycles, \
					MetricFlags_ValFmt_Int)

#define ETABLE_ENTRY_SW(name) \
   ETABLE_ENTRY(PERF_TYPE_SOFTWARE, name, mp_none, MetricFlags_ValFmt_Int)


//******************************************************************************
// type declarations
//******************************************************************************

typedef struct perf_event_header pe_header_t;

typedef struct perf_event_callchain_s {
  uint64_t    nr;        /* number of IPs */ 
  uint64_t    ips[];     /* vector of IPs */
} pe_callchain_t;

typedef struct perf_event_mmap_page pe_mmap_t;

typedef struct etable_entry_s {
  int perf_type;
  const char *event_name;
  int perf_config;
  metric_desc_properties_t hpcrun_properties;
  MetricFlags_ValFmt_t hpcrun_value_format;
} etable_entry_t;

typedef struct selected_entry_s {
  etable_entry_t *event;
  int metric_id;
  int threshold;
  struct selected_entry_s *next;
} selected_entry_t;



/******************************************************************************
 * external thread-local variables
 *****************************************************************************/

extern __thread bool hpcrun_thread_suppress_sample;



//******************************************************************************
// local variables
//******************************************************************************


static uint16_t perf_kernel_lm_id;

static bool perf_ksyms_avail;

static int perf_process_state;
static int perf_initialized;

#if 0
static int metric_id;
static long threshold;
static const char *event_name = "PERF_COUNT_HW_CPU_CYCLES";
#endif

static sigset_t sig_mask;

static int pagesize;
static size_t tail_mask;


static const char * dashes_separator = 
  "---------------------------------------------------------------------------\n";
static const char * equals_separator =
  "===========================================================================\n";

// Special case to make perf init a soft failure.
// Make sure that we don't use perf if it won't work.
static int perf_unavail = 0;

static
etable_entry_t etable[] = {
  // hardware events
  ETABLE_ENTRY_HW_CYC(PERF_COUNT_HW_CPU_CYCLES),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_INSTRUCTIONS),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_CACHE_REFERENCES),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_CACHE_MISSES),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_BRANCH_MISSES),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_BUS_CYCLES),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_STALLED_CYCLES_FRONTEND),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_STALLED_CYCLES_BACKEND),
  ETABLE_ENTRY_HW(PERF_COUNT_HW_REF_CPU_CYCLES),

  // software events
  ETABLE_ENTRY_SW(PERF_COUNT_SW_CPU_CLOCK),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_TASK_CLOCK),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_PAGE_FAULTS),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_CONTEXT_SWITCHES),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_CPU_MIGRATIONS),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_PAGE_FAULTS_MIN),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_PAGE_FAULTS_MAJ),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_ALIGNMENT_FAULTS),
  ETABLE_ENTRY_SW(PERF_COUNT_SW_EMULATION_FAULTS)
};

static selected_entry_t *event_list = 0;



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

int                         __thread perf_thread_fd;
pe_mmap_t                   __thread *perf_mmap;



//******************************************************************************
// forward declarations 
//******************************************************************************

static bool 
perf_thread_init();

static void 
perf_thread_fini();

static cct_node_t *
perf_add_kernel_callchain(
  cct_node_t *leaf
);

static int perf_event_handler(
  int sig, 
  siginfo_t* siginfo, 
  void* context
);



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

  ioctl(perf_thread_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(perf_thread_fd, PERF_EVENT_IOC_ENABLE, 0);

  fcntl(perf_thread_fd, F_SETFL,  O_ASYNC | O_NONBLOCK);

  struct f_owner_ex owner;
  owner.type = F_OWNER_TID;
  owner.pid  = syscall(SYS_gettid);
  ret = fcntl(perf_thread_fd, F_SETOWN_EX, &owner);

  if (ret == -1) {
    fprintf(stderr, "can't set fcntl(F_SETOWN_EX) on %d: %s\n", 
	    perf_thread_fd, strerror(errno));
  }

  perf_started = 1;
}


static void
perf_stop()
{
  if (!perf_initialized) return;

  monitor_real_pthread_sigmask(SIG_BLOCK, &sig_mask, NULL);
  ioctl(perf_thread_fd, PERF_EVENT_IOC_DISABLE, 0);

  perf_started = 0;
}


//----------------------------------------------------------
// event support
//----------------------------------------------------------

static etable_entry_t *
perf_event_lookup(const char *ename)
{
  int n_entries = sizeof(etable)/sizeof(etable_entry_t);
  for (int i =0; i < n_entries; i++) {
    if (strncmp(etable[i].event_name, ename, strlen(etable[i].event_name)) == 0) {
      return &etable[i];
    }
  }
  return 0;
}

void
perf_event_add(selected_entry_t *se)
{
  se->next = event_list;
  event_list = se;
}


//----------------------------------------------------------
// read from perf_events mmap'ed buffer
//----------------------------------------------------------

static int 
perf_read(
  void *buf, 
  size_t bytes_wanted
)
{
  // front of the circular data buffer
  char *data = BUFFER_FRONT; 

  // compute bytes available in the circular buffer 
  size_t bytes_available = perf_mmap->data_head - perf_mmap->data_tail;

  if (bytes_wanted > bytes_available) return -1;

  // compute offset of tail in the circular buffer
  unsigned long tail = BUFFER_OFFSET(perf_mmap->data_tail);

  long bytes_at_right = BUFFER_SIZE - tail;

  // bytes to copy to the right of tail
  size_t right = bytes_at_right < bytes_wanted ? bytes_at_right : bytes_wanted;

  // copy bytes from tail position
  memcpy(buf, data + tail, right);

  // if necessary, wrap and continue copy from left edge of buffer
  if (bytes_wanted > right) {
    size_t left = bytes_wanted - right;
    memcpy(buf + right, data, left);
  }

  // update tail after consuming bytes_wanted
  perf_mmap->data_tail += bytes_wanted;

  return 0;
}


static inline int
perf_read_header(
  pe_header_t *hdr
)
{
  return perf_read(hdr, sizeof(pe_header_t));
}


static inline int
perf_read_uint64_t(
  uint64_t *val
)
{
  return perf_read(val, sizeof(uint64_t));
}


//----------------------------------------------------------
// predicates that test perf availability
//----------------------------------------------------------

static bool
perf_kernel_syms_avail()
{
  FILE *ksyms = fopen(LINUX_KERNEL_SYMBOL_FILE, "r");
  bool success = (ksyms != NULL);
  if (success) fclose(ksyms);
  return success;
}


static bool
perf_event_avail()
{
  static int checked = PERF_EVENT_AVAILABLE_UNKNOWN;
  
  switch (checked) {
  case PERF_EVENT_AVAILABLE_UNKNOWN:
    {
      struct stat buf;
      int rc = stat("/proc/sys/kernel/perf_event_paranoid", &buf);
      checked = (rc == 0) ? PERF_EVENT_AVAILABLE_YES : PERF_EVENT_AVAILABLE_NO; 
    }
  case PERF_EVENT_AVAILABLE_NO:      
  case PERF_EVENT_AVAILABLE_YES:     
    break;
  }

  return (checked == PERF_EVENT_AVAILABLE_YES);     
}


//----------------------------------------------------------
// initialization and finalization
//----------------------------------------------------------

static void 
perf_init()
{
  perf_ksyms_avail = perf_kernel_syms_avail();

  pagesize = sysconf(_SC_PAGESIZE);
  tail_mask = PERF_TAIL_MASK(pagesize);

  // initialize mask to block PERF_SIGNAL 
  sigemptyset(&sig_mask);
  sigaddset(&sig_mask, PERF_SIGNAL);

  // if kernel symbols are available, we will attempt to collect kernel 
  // callchains and add them to our call paths 
  if (perf_ksyms_avail) {
    hpcrun_kernel_callpath_register(perf_add_kernel_callchain);
    perf_kernel_lm_id = 
      hpcrun_loadModule_add(LINUX_KERNEL_NAME);
  }
 
  monitor_sigaction(PERF_SIGNAL, &perf_event_handler, 0, NULL);
}


static void
perf_attr_init(
  struct perf_event_attr *attr,
  selected_entry_t *se
)
{
  memset(attr, 0, sizeof(struct perf_event_attr));

  attr->type = se->event->perf_type; 
  attr->size = sizeof(struct perf_event_attr);
  attr->disabled = 1;
  attr->config = se->event->perf_config; 

  attr->sample_period = se->threshold;
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


static bool
perf_thread_init()
{
  if (perf_thread_initialized == 0) {
    struct perf_event_attr attr;
    selected_entry_t *se = event_list;

    perf_attr_init(&attr, se);
    perf_thread_fd = perf_event_open(&attr, THREAD_SELF, CPU_ANY, 
			      GROUP_FD, PERF_FLAGS);

    if (perf_thread_fd == -1) {
      fprintf(stderr, "failed to open perf_thread_fd: %s\n", strerror(errno));
    }

    void *map_result = 
      mmap(NULL, PERF_MMAP_SIZE(pagesize), PROT_WRITE | PROT_READ, 
	   MAP_SHARED, perf_thread_fd, MMAP_OFFSET_0);

    if (map_result == MAP_FAILED) {
      EMSG("Linux perf mmap failed: %s", strerror(errno));
      return false;
    }

    perf_mmap  = (pe_mmap_t *) map_result;

    if (perf_mmap) {
      memset(perf_mmap, 0, sizeof(pe_mmap_t));
      perf_mmap->version = 0; 
      perf_mmap->compat_version = 0; 
      perf_mmap->data_head = 0; 
      perf_mmap->data_tail = 0; 
    }

    perf_thread_initialized = 1;
  }
  return true;
}


void
perf_thread_fini()
{
  if (perf_thread_initialized) {
    munmap(perf_mmap, PERF_MMAP_SIZE(pagesize));
    close(perf_thread_fd);
  }
  perf_thread_initialized = 0;
}


//----------------------------------------------------------
// signal handling and processing of kernel callchains
//----------------------------------------------------------

// extend a user-mode callchain with kernel frames (if any)
static cct_node_t *
perf_add_kernel_callchain(
  cct_node_t *leaf
)
{
  cct_node_t *parent = leaf;
  pe_header_t hdr; 

  if (perf_read_header(&hdr) == 0) {
    if (hdr.type == PERF_RECORD_SAMPLE) {
      uint64_t n_frames;
      // determine how many frames in the call chain 
      if (perf_read_uint64_t(&n_frames) == 0) {

	if (n_frames > 0) {
	  // allocate space to receive IPs for kernel callchain 
	  uint64_t *ips = alloca(n_frames * sizeof(uint64_t));

	  // read the IPs for the frames 
	  if (perf_read(ips, n_frames * sizeof(uint64_t)) == 0) {

	    // add kernel IPs to the call chain top down, which is the 
	    // reverse of the order in which they appear in ips
	    for (int i = n_frames - 1; i > 0; i--) {
	      ip_normalized_t npc = 
		{ .lm_id = perf_kernel_lm_id, .lm_ip = ips[i] };
	      cct_addr_t frm = { .ip_norm = npc };
	      cct_node_t *child = hpcrun_cct_insert_addr(parent, &frm);
	      parent = child;
	    }
	  } else {
	    TMSG(LINUX_PERF, "unable to read all frames on fd %d", 
		 perf_thread_fd);
	  }
	}
      } else {
	TMSG(LINUX_PERF, "unable to read number of frames on fd %d", 
	     perf_thread_fd);
      }
    }
  }

  return parent;
}


static void
perf_restart()
{
  int rc = ioctl(perf_thread_fd, PERF_EVENT_IOC_REFRESH, 1);
  if (rc == -1) {
    TMSG(LINUX_PERF, "error in IOC_REFRESH");
  }
}


static int
perf_event_handler(
  int sig, 
  siginfo_t* siginfo, 
  void* context
)
{
  if (siginfo->si_code < 0) {
    // signal not generated by kernel for profiling
    TMSG(LINUX_PERF, "signal si_code %d >= 0 indicates not from kernel", 
	 siginfo->si_code);
    return 1; // tell monitor the signal has not been handled.
  }

#if 0
  if (siginfo->si_code != POLL_HUP) {
    // expect POLL_HUP and not POLL_IN
    TMSG(LINUX_PERF, "signal for si_code %d is not POLL_HUP", siginfo->si_code);
    return 1; // tell monitor the signal has not been handled.
  }

  if (siginfo->si_fd != perf_thread_fd) {
    TMSG(LINUX_PERF, "signal for fd %d is not for perf_thread_fd %d", 
	 siginfo->si_fd, perf_thread_fd);
    return 1; // tell monitor the signal has not been handled.
  }
#endif

  void *pc = hpcrun_context_pc(context); 

  // if sampling disabled explicitly for this thread, skip all processing
  if (hpcrun_thread_suppress_sample) {
    perf_restart();
    return 0; // tell monitor the signal has been handled.
  }

  // if the interrupt came while inside our code, then drop the sample
  // and return and avoid the potential for deadlock.
  if (! hpcrun_safe_enter_async(pc)) {
    hpcrun_stats_num_samples_blocked_async_inc();
    perf_restart();
    return 0; // tell monitor the signal has been handled.
  }

  selected_entry_t *se = event_list;

  sample_val_t sv = hpcrun_sample_callpath(context, se->metric_id, 1,
					   0/*skipInner*/, 0/*isSync*/);

  blame_shift_apply(se->metric_id, sv.sample_node, 1 /*metricIncr*/);

  hpcrun_safe_exit();

  perf_restart();
  return 0; // tell monitor the signal has been handled.
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


// return true if Linux perf recognizes the name, whether supported or not.
// we'll handle unsupported events later.
static bool
METHOD_FN(supports_event, const char *ev_str)
{
  TMSG(LINUX_PERF, "supports event");
  if (perf_unavail) { return false; }

  if (perf_process_state == UNINIT){
    METHOD_CALL(self, init);
  }

  if (perf_event_lookup(ev_str)) return true;
  
  return false;
}

 
static void
METHOD_FN(process_event_list, int lush_metrics)
{
#if 0
  metric_desc_properties_t prop = metric_property_none;
  prop = metric_property_cycles;
MetricFlags_ValFmt_Int,
#endif

  TMSG(LINUX_PERF, "process event list");

  if (perf_unavail) { return; }

  char* evlist = METHOD_CALL(self, get_event_str);
#if 0
  char *event = start_tok(evlist); 
  char name[1024];
  hpcrun_extract_ev_thresh(event, sizeof(name), name, &threshold, 
			   DEFAULT_THRESHOLD);
#else
  char *event;
  for (event = start_tok(evlist); more_tok(); event = next_tok()) {
    char name[1024];
    long threshold;

    TMSG(LINUX_PERF,"checking event spec = %s",event);

    hpcrun_extract_ev_thresh(event, sizeof(name), name, &threshold, 
			     DEFAULT_THRESHOLD);

    selected_entry_t *se = (selected_entry_t *) malloc(sizeof(selected_entry_t));

    se->event = perf_event_lookup(name);
    se->metric_id = hpcrun_new_metric();
    se->threshold = threshold;

    perf_event_add(se);
    
    hpcrun_set_metric_info_and_period(se->metric_id, se->event->event_name,
				      se->event->hpcrun_value_format, 
				      se->threshold, se->event->hpcrun_properties);
  }
#endif

#if 0 
  char *name = "PERF_COUNT_HW_CPU_CYCLES";
  threshold = DEFAULT_THRESHOLD;
#endif

  perf_initialized = true;
  perf_init();
  perf_thread_init();
}


static void
METHOD_FN(gen_event_set, int lush_metrics)
{
}


static void
METHOD_FN(display_events)
{
  if (perf_event_avail()) {

    printf(equals_separator);
    printf("Available Linux perf events\n");
    printf(equals_separator);

    printf("Name\t\tDescription\n");
    printf(dashes_separator);

    printf("%s\tTotal cycles.\n", 
	   "PERF_COUNT_HW_CPU_CYCLES");
    printf("\n");
  }
}



/***************************************************************************
 * object
 ***************************************************************************/

#define ss_name linux_perf
#define ss_cls SS_HARDWARE

#include "ss_obj.h"

