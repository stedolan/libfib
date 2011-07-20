#include <qnode.h>


struct lockless_fiber_queue{
  qnode q_head;
  qnodeptr q_tail;
  void enqueue(node* newnode){
    // retry until success
    while (1){
      qnodeptr tailptr = qnodeptr_load_acquire(&q_tail);
      qnode* tail = qnodeptr_getptr(tailptr);
      qnodeptr nextptr = qnodeptr_load_acquire(&tail->next);
      qnode* next = qnodeptr_getptr(nextptr);
      qnodeptr tailptr2 = qnodeptr_load_acquire(&q_tail);
      if (qnodeptr_eq(tailptr, tailptr2)){ // check for consistent snapshot
        if (!next){
          // try to append to list
          if (qnodeptr_cas(&tail->next, nextptr, newnode)){
            // advance tail
            qnodeptr_cas(&q_tail, tailptr, newnode);
            // may fail, in which case we were interrupted
            // and some other thread advanced the tail for us
            return;
          }
        }else{
          // tail is trailing
          qnodeptr_cas(&q_tail, tailptr, next);
        }
      }
    }
  }

  node* dequeue(){
    // retry until success
    while (1){
      qnodeptr firstptr = qnodeptr_load_acquire(&q_head->next);
      qnode* first = qnodeptr_getptr(firstptr);
      qnodeptr tailptr = qnodeptr_load_acquire(&q_tail);
      qnode* tail = qnodeptr_getptr(tailptr);
      qnodeptr next = qnodeptr_load_acquire(&first->next
      qnodeptr firstptr2 = qnodeptr_load_acquire(&q_head->next);
      if (qnodeptr_eq(firstptr, firstptr2)){ // check for consistent snapshot
        if (tail == &q_head){
          // either queue is empty or tail is trailing
          if (!first){
            // queue is empty
            return 0;
          }else{
            // tail is trailing
            qnodeptr_cas(&q_tail, tailptr, first);
          }
        }else{
          // we have something, try to grab it
          if (qnodeptr_cas(&q_head->next, 
        }
      }
    }
  }
};
