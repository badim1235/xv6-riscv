#include "kernel/types.h"
#include "user/user.h"

int main() {
  printf("=== TEST START ===\n");

  int pid = fork();

  if (pid == 0) {
    setnice(getpid(), 10);
    
    volatile long long i;
    volatile long long j;
    long long sample = 0;
    for (i = 0; i < 300000; i++) {
       for (j = 0; j < 50000; j++) {
            sample += i + j;
       }
    }
    exit(0);
  }
  else {
    setnice(getpid(), 0);
    
    volatile long long i;
    volatile long long j;
    long long sample = 0;
    for (i = 0; i < 150000; i++) {
         for (j = 0; j < 50000; j++) {
             sample += i + j;
         }
    }
    
    ps(0);
    
    waitpid(pid);
  }
  
  printf("=== TEST END ===\n");

  exit(0);
}