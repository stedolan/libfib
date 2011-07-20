//#include "llq.h"
#include "sync_object.h"
#include <stdio.h>

int main(){
  llq::word words[2] = {4,20};
  if (llq::dcas(words, 4, 20, 100, 3294892)){
    printf("%d %d\n", (int)words[0], (int)words[1]);
  }
  printf("%d\n", (int)llq::atomic_xchg(&words[0], 42));
  printf("%d\n", (int)words[0]);
}

blocking_queue<int> x;


void foo(){
  x.write(42);
}
