#include "llq_ctypes.h"


#ifdef LLQ_PPC64
struct worker;
__thread worker* current_worker_;
worker*& get_current_worker_(){
  return current_worker_;
}
#endif
