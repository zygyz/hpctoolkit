#ifndef __fake_lock_h__
#define __fake_lock_h__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CILKSCREEN
  void fake_lock_acquire(void);
  void fake_lock_release(void);
#else
#define fake_lock_acquire()
#define fake_lock_release()
#endif

#ifdef __cplusplus
}
#endif

#endif
