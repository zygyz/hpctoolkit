
//******************************************************************************
// system include files 
//******************************************************************************

#include <stdio.h>
#include <stdlib.h>



//******************************************************************************
// cuda include 
//******************************************************************************

#include <cuda.h>
#include <cupti.h>



#define EMSG(...) 
#define monitor_real_abort() abort()
#define hpctoolkit_stats_increment(...)
#define get_correlation_id(ptr) (*(ptr) = 7) // test

//******************************************************************************
// macros 
//******************************************************************************

#define CUPTI_ACTIVITY_BUFFER_SIZE (64 * 1024)

#define CUPTI_ACTIVITY_BUFFER_ALIGNMENT (8)

#define FOREACH_CUPTI_STATUS(macro)             \
  macro(CUPTI_SUCCESS)				\
  macro(CUPTI_ERROR_INVALID_PARAMETER)		\
  macro(CUPTI_ERROR_INVALID_DEVICE )		\
  macro(CUPTI_ERROR_INVALID_CONTEXT)		\
  macro(CUPTI_ERROR_NOT_INITIALIZED)

#define FOREACH_STALL_REASON(macro)             \
  macro(INVALID)				\
  macro(NONE)					\
  macro(INST_FETCH)				\
  macro(EXEC_DEPENDENCY)			\
  macro(MEMORY_DEPENDENCY)			\
  macro(TEXTURE)				\
  macro(SYNC)					\
  macro(CONSTANT_MEMORY_DEPENDENCY)		\
  macro(PIPE_BUSY)				\
  macro(MEMORY_THROTTLE)			\
  macro(NOT_SELECTED)				\
  macro(OTHER)

#define COMPUTE_CAPABILITY_EXCEEDS(properties, major_val, minor_val)	\
    (properties->major >= major_val) && (properties->minor >= minor_val)

#define CUDA_CALL(fn, args)                                             \
{                                                                       \
    cudaError_t status = fn args;                                       \
    if (status != cudaSuccess) {                                        \
        cuda_error_report(status, #fn);                                 \
    }                                                                   \
}

#define CUPTI_CALL(fn, args)                                            \
{                                                                       \
    CUptiResult status = fn args;                                       \
    if (status != CUPTI_SUCCESS) {                                      \
      cupti_error_report(status, #fn);					\
    }                                                                   \
}



//******************************************************************************
// types
//******************************************************************************

typedef struct {
  uint64_t external_correlation_id;
} cupti_processing_state_t;



//******************************************************************************
// constants 
//******************************************************************************

const CUpti_ActivityKind
external_correlation_activities[] = {
  CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION, 
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
data_motion_explicit_activities[] = {
  CUPTI_ACTIVITY_KIND_MEMCPY, 
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
data_motion_implicit_activities[] = {
  CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER,
  CUPTI_ACTIVITY_KIND_MEMCPY2,
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
kernel_invocation_activities[] = {
  CUPTI_ACTIVITY_KIND_KERNEL,
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
kernel_execution_activities[] = {
  CUPTI_ACTIVITY_KIND_PC_SAMPLING,
  CUPTI_ACTIVITY_KIND_FUNCTION,
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
overhead_activities[] = {
  CUPTI_ACTIVITY_KIND_OVERHEAD,
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
driver_activities[] = {
  CUPTI_ACTIVITY_KIND_DRIVER,
  CUPTI_ACTIVITY_KIND_INVALID
};


const CUpti_ActivityKind
runtime_activities[] = {
  CUPTI_ACTIVITY_KIND_RUNTIME,
  CUPTI_ACTIVITY_KIND_INVALID
};


//******************************************************************************
// local data
//******************************************************************************

#define gpu_event_decl(stall) \
  int gpu_stall_event_ ## stall; 

FOREACH_STALL_REASON(gpu_event_decl)

int illegal_event;

CUpti_SubscriberHandle cupti_subscriber;


//******************************************************************************
// private operations
//******************************************************************************


//-----------------------------
// hpctoolkit error reporting 
//-----------------------------

static void
gpu_error_report
(
 const char *type, 
 const char *fn, 
 const char *error_string
)
{
  EMSG("%s error: function %s failed with error %s.", 
	type, fn, error_string);                        
  monitor_real_abort();
} 


//-----------------
// CUDA interface
//-----------------

static void
cuda_error_report
(
 cudaError_t error, 
 const char *fn
)
{
  const char *error_string = cudaGetErrorString(error);
  gpu_error_report("CUDA", fn, error_string);
} 


static void
cuda_device_properties
(
 cudaDeviceProp *properties,
  int device
)
{
    CUDA_CALL(cudaGetDeviceProperties, (properties, device));
}


static int
cuda_device_capability_sampling
(
 cudaDeviceProp *properties
) 
{
  return COMPUTE_CAPABILITY_EXCEEDS(properties, 5, 2);
}


static int
cuda_device_supports_sampling
(
 int device
)
{
  cudaDeviceProp properties;
  cuda_device_properties(&properties, device);
  return cuda_device_capability_sampling(&properties);
}


//-----------------
// CUPTI interface
//-----------------

static void
cupti_error_report
(
 CUptiResult error, 
 const char *fn
)
{
  const char *error_string;
  cuptiGetResultString(error, &error_string);
  gpu_error_report("CUPTI", fn, error_string);
} 


static void
cupti_enable_monitoring
(
 const  CUpti_ActivityKind activity_kinds[]
)
{
  int i = 0;
  for (;;) {
    CUpti_ActivityKind activity_kind = activity_kinds[i++];
    if (activity_kind == CUPTI_ACTIVITY_KIND_INVALID) break;
    CUPTI_CALL(cuptiActivityEnable, (activity_kind));
  }
}


static void
cupti_note_dropped
(
 CUcontext ctx, 
 uint32_t stream_id
)
{
    size_t dropped;
    CUPTI_CALL(cuptiActivityGetNumDroppedRecords, (ctx, stream_id, &dropped));
    if (dropped != 0) {
      hpctoolkit_stats_increment((unsigned int) dropped);
    }
}


static void 
cupti_buffer_alloc 
(
 uint8_t **buffer, 
 size_t *buffer_size, 
 size_t *maxNumRecords
)
{
  int retval = posix_memalign((void **) buffer, 
			      (size_t) CUPTI_ACTIVITY_BUFFER_ALIGNMENT,
			      (size_t) CUPTI_ACTIVITY_BUFFER_SIZE); 
  
  if (retval != 0) {
    gpu_error_report("CUPTI", "cupti_buffer_alloc", "out of memory");
  }
  
  *buffer_size = CUPTI_ACTIVITY_BUFFER_SIZE;

  *maxNumRecords = 0;

}



static const char *
gpu_stall_reason
(
 CUpti_ActivityPCSamplingStallReason reason
)
{
#define switchcase(stall) \
  case CUPTI_ACTIVITY_PC_SAMPLING_STALL_ ## stall: \
      return #stall; 

    switch (reason) {
    FOREACH_STALL_REASON(switchcase)
    default: 
      break;
    }
#undef switchcase
    return "Unknown stall reason";
}


static int
cupti_sample_stall_event
(
 CUpti_ActivityPCSamplingStallReason reason
)
{
#define switchcase(stall) \
  case CUPTI_ACTIVITY_PC_SAMPLING_STALL_ ## stall: \
      return gpu_stall_event_ ## stall; 

    switch (reason) {
    FOREACH_STALL_REASON(switchcase)
    default: 
      break;
    }
#undef switchcase

   return illegal_event;
}

static void
cupti_process_sample
(
 CUpti_ActivityPCSampling2 *sample,
 cupti_processing_state_t *state
)
{
  printf("source %u, functionId %u, pc 0x%x, corr %u, "
	 "samples %u, stallreason %s\n",
	 sample->sourceLocatorId,
	 sample->functionId,
	 sample->pcOffset,
	 sample->correlationId,
	 sample->samples,
	 gpu_stall_reason(sample->stallReason));
}


static void
cupti_process_source_locator
(
 CUpti_ActivitySourceLocator *asl,
 cupti_processing_state_t *state
)
{
  printf("Source Locator Id %d, File %s Line %d\n", 
	 asl->id, asl->fileName, 
	 asl->lineNumber);
}


static void
cupti_process_function
(
 CUpti_ActivityFunction *af,
 cupti_processing_state_t *state
)
{
  printf("Function Id %u, ctx %u, moduleId %u, functionIndex %u, name %s\n",
	 af->id,
	 af->contextId,
	 af->moduleId,
	 af->functionIndex,
	 af->name);
}


static void
cupti_process_sampling_record_info
(
 CUpti_ActivityPCSamplingRecordInfo *sri,
 cupti_processing_state_t *state
)
{
  printf("corr %u, totalSamples %llu, droppedSamples %llu\n",
	 sri->correlationId,
	 (unsigned long long)sri->totalSamples,
	 (unsigned long long)sri->droppedSamples);
}


static void
cupti_process_correlation
(
 CUpti_ActivityExternalCorrelation *ec,
 cupti_processing_state_t *state
)
{
  state->external_correlation_id = ec->externalId;
  printf("External CorrelationId %llu\n", ec->externalId);
}


static void
cupti_process_memcpy
(
 CUpti_ActivityMemcpy *activity,
 cupti_processing_state_t *state
)
{
}


static void
cupti_process_memcpy2
(
 CUpti_ActivityMemcpy2 *activity, 
 cupti_processing_state_t *state
)
{
}


static void
cupti_process_memctr
(
 CUpti_ActivityUnifiedMemoryCounter *activity, 
 cupti_processing_state_t *state
)
{
}


static void
cupti_process_activityAPI
(
 CUpti_ActivityAPI *activity,
 cupti_processing_state_t *state
)
{
  // case CUPTI_ACTIVITY_KIND_DRIVER:
  // case CUPTI_ACTIVITY_KIND_KERNEL:
}


static void
cupti_process_runtime
(
 CUpti_ActivityEvent *activity, 
 cupti_processing_state_t *state
)
{
}


static void
cupti_process_unknown
(
 CUpti_Activity *activity,
 cupti_processing_state_t *state
)    
{
  printf("Unknown activity kind %d\n", activity->kind);
}


static void
cupti_process_activity
(
 CUpti_Activity *activity,
 cupti_processing_state_t *state
)
{
  switch (activity->kind) {

  case CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR:
    cupti_process_source_locator((CUpti_ActivitySourceLocator *) activity, 
				 state);
    break;

  case CUPTI_ACTIVITY_KIND_PC_SAMPLING:
    cupti_process_sample((CUpti_ActivityPCSampling2 *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_PC_SAMPLING_RECORD_INFO:
    cupti_process_sampling_record_info
      ((CUpti_ActivityPCSamplingRecordInfo *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_FUNCTION:
    cupti_process_function((CUpti_ActivityFunction *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION: 
    cupti_process_correlation((CUpti_ActivityExternalCorrelation *) activity,
			      state);
    break;

  case CUPTI_ACTIVITY_KIND_MEMCPY: 
    cupti_process_memcpy((CUpti_ActivityMemcpy *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_MEMCPY2: 
    cupti_process_memcpy2((CUpti_ActivityMemcpy2 *) activity, state);

  case CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER:
    cupti_process_memctr((CUpti_ActivityUnifiedMemoryCounter *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_DRIVER:
  case CUPTI_ACTIVITY_KIND_KERNEL:
    cupti_process_activityAPI((CUpti_ActivityAPI *) activity, state);
    break;

  case CUPTI_ACTIVITY_KIND_RUNTIME:
    cupti_process_runtime((CUpti_ActivityEvent *) activity, state);
    break;

  default:
    cupti_process_unknown(activity, state);
    break;
  }
}


static void 
cupti_buffer_process_and_free
(
 CUcontext ctx, 
 uint32_t stream_id, 
 uint8_t *buffer, 
 size_t buffer_size, 
 size_t valid_size
)
{
  CUpti_Activity *activity = NULL;
  CUptiResult result;
  cupti_processing_state_t state;
  for(;;) {
    result = cuptiActivityGetNextRecord(buffer, valid_size, &activity);
    if (result == CUPTI_SUCCESS) {
      cupti_process_activity(activity, &state);
    } else {
      if (result != CUPTI_ERROR_MAX_LIMIT_REACHED) {
	cupti_error_report(result, "cuptiActivityGetNextRecord");
      }
      break;
    }
  }
  cupti_note_dropped(ctx, stream_id);
  free(buffer);
}
static void 
cupti_pop_correlation_id()
{
  uint64_t external_id;
  cuptiActivityPopExternalCorrelationId
    (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, &external_id);
}


static void 
cupti_push_correlation_id()
{
  uint64_t external_id;
  get_correlation_id(&external_id);
  int result = cuptiActivityPushExternalCorrelationId
    (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, external_id);
}


static void 
cupti_add_correlation_callback
(
 void *userdata,
 CUpti_CallbackDomain domain,
 CUpti_CallbackId cbid,
 const CUpti_CallbackData *cbd
)
{
  switch(cbid) {

  case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2:
  case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2:
  case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:

    switch(cbd->callbackSite) {

    case CUPTI_API_ENTER:
      cupti_push_correlation_id();
      break;
      
    case CUPTI_API_EXIT:
      cupti_pop_correlation_id();
      break;

    default:
      break;
    }
    break;

  case CUPTI_DRIVER_TRACE_CBID_cuModuleLoadDataEx:

#if 0
    pthread_mutex_lock(&mut);
    flagg++;
    pthread_mutex_unlock(&mut);
      
    if (cbd->callbackSite == CUPTI_API_ENTER) {
      load_module_info_rtl_ptr(flagg);
    }
#endif

  default:
    break;
  }
}


static void
cupti_correlation_enable()
{
  cuptiSubscribe(&cupti_subscriber, 
		 (CUpti_CallbackFunc) cupti_add_correlation_callback,
		 (void *) NULL);
  cuptiEnableDomain(1, cupti_subscriber, CUPTI_CB_DOMAIN_DRIVER_API);
}


static void
cupti_correlation_disable()
{
  cuptiUnsubscribe(cupti_subscriber); 
  cuptiEnableDomain(0, cupti_subscriber, CUPTI_CB_DOMAIN_DRIVER_API);
}



//******************************************************************************
// interface  operations
//******************************************************************************

void
cupti_start()
{
  cupti_correlation_enable();
  cupti_enable_monitoring(kernel_execution_activities);
  CUPTI_CALL(cuptiActivityRegisterCallbacks, 
	     (cupti_buffer_alloc, cupti_buffer_process_and_free));
}


void
cupti_stop()
{
  cupti_correlation_disable();
  CUPTI_CALL(cuptiActivityFlushAll, (0));
}
