#ifndef LLQ_CTYPES_H
#define LLQ_CTYPES_H

/* This file contains the basic type definitions for llq, so
   that they may be included in structure definitions exposed
   to C code (i.e. the pthreads compatibility layer). */

/* A word is an unsigned integer the same size as a pointer */
typedef __attribute__((may_alias)) unsigned long llq_word;


/* A linked list node. Generally followed by other data. */
struct llq_node{
  struct llq_node* next;
};


/* ABA-safe counted pointers */
struct __attribute__((aligned(16))) llq_ctptr{
  llq_word ptr;
  llq_word serial;
};

/* Queue head structure */
struct llq_queue{
  struct llq_node head;
  struct llq_ctptr tail;
};



#endif
