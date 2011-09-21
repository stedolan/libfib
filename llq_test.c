#include "llq.h"

int main(){
  llq_word words[2] = {4,20};
  if (llq_dcas(words, 4, 20, 100, 3294892)){
    printf("%d %d\n", (int)words[0], (int)words[1]);
  }
}
