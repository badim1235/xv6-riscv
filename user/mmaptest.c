#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define PGSIZE 4096

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE  0x2

void test_passed(char *msg) {
  printf("[SUCCESS] %s\n", msg);
}

void test_failed(char *msg) {
  printf("[FAILED] %s\n", msg);
  exit(1);
}

int main(void) {
  uint64 addr = 0;
  char *mapped_memory;
  int fd;
  uint64 before, after_mmap, after_fault, after_munmap;

  printf("========== MMAP TEST ==========\n");
  printf("Initial free memory pages: %d\n\n", (int)freemem());

  // Test 1: Anonymous mapping WITHOUT populate (Lazy Allocation)
  printf("--- Test 1: Anonymous mapping (Without Populate) ---\n");
  before = freemem();
  addr = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  if (addr == 0) test_failed("mmap failed");
  mapped_memory = (char*)addr;

  after_mmap = freemem();
  if (after_mmap != before) test_failed("Memory allocated before page fault!");

  // Write to memory -> triggers Page Fault
  mapped_memory[0] = 'A'; 
  
  after_fault = freemem();
  if (after_fault >= before) test_failed("Physical memory not allocated after page fault");

  munmap(addr);
  after_munmap = freemem();
  if (after_munmap <= after_fault) test_failed("Memory not freed after munmap");
  test_passed("Anonymous without populate & Page Fault handled");


  // Test 2: Anonymous mapping WITH populate
  printf("\n--- Test 2: Anonymous mapping (With Populate) ---\n");
  before = freemem();
  addr = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (addr == 0) test_failed("mmap failed");
  
  after_mmap = freemem();
  if (after_mmap >= before) test_failed("Memory not populated immediately");

  munmap(addr);
  after_munmap = freemem();
  if (after_munmap <= after_mmap) test_failed("Memory not freed after munmap");
  test_passed("Anonymous with populate");


  // Test 3: File mapping WITHOUT populate
  printf("\n--- Test 3: File mapping (Without Populate) ---\n");
  fd = open("README", O_RDONLY); 
  if(fd < 0) test_failed("Failed to open README");

  before = freemem();
  addr = mmap(0, PGSIZE, PROT_READ, 0, fd, 0); 
  if (addr == 0) test_failed("mmap failed");
  mapped_memory = (char*)addr;

  after_mmap = freemem();
  if (after_mmap != before) test_failed("Memory allocated before page fault!");

  // Read from memory -> triggers Page Fault
  char first_char = mapped_memory[0];
  if (first_char != 'x' && first_char != 'r' && first_char != 'X') {
    test_failed("Wrong file content read from mapped memory");
  }

  after_fault = freemem();
  if (after_fault >= before) test_failed("Physical memory not allocated after page fault");

  munmap(addr);
  close(fd);
  
  after_munmap = freemem();
  if (after_munmap <= after_fault) test_failed("Memory not freed after munmap");
  test_passed("File mapping without populate & Page Fault handled");


  // Test 4: File mapping WITH populate
  printf("\n--- Test 4: File mapping (With Populate) ---\n");
  fd = open("README", O_RDONLY);
  before = freemem();
  addr = mmap(0, PGSIZE, PROT_READ, MAP_POPULATE, fd, 0);
  mapped_memory = (char*)addr;

  after_mmap = freemem();
  if (after_mmap >= before) test_failed("Memory not populated immediately");
  
  if (mapped_memory[0] != 'x' && mapped_memory[0] != 'r' && mapped_memory[0] != 'X') {
    test_failed("Wrong file content");
  }

  munmap(addr);
  close(fd);
  
  after_munmap = freemem();
  if (after_munmap <= after_mmap) test_failed("Memory not freed after munmap");
  test_passed("File mapping with populate");


  // Test 5: Fork test & Freemem check
  printf("\n--- Test 5: Fork comparison & Freemem ---\n");
  fd = open("README", O_RDONLY);
  addr = mmap(0, PGSIZE, PROT_READ, MAP_POPULATE, fd, 0); 
  mapped_memory = (char*)addr;
  
  char parent_content[10];
  memmove(parent_content, mapped_memory, 10);

  uint64 before_fork = freemem();

  int pid = fork();
  if (pid < 0) {
    test_failed("Fork failed");
  } 
  else if (pid == 0) {
    // Child Process
    char child_content[10];
    memmove(child_content, (char*)addr, 10);
    
    for(int i=0; i<10; i++){
      if(parent_content[i] != child_content[i]){
        test_failed("Child mapped content differs from parent");
      }
    }

    uint64 child_free = freemem();
    if (child_free >= before_fork) test_failed("Child memory didn't duplicate correctly");
    
    test_passed("Child content matches parent completely");
    exit(0); 
  } 
  else {
    // Parent Process
    wait(0); 
    munmap(addr);
    close(fd);
    test_passed("Fork test & Memory recovery handled perfectly");
  }

  printf("\n========== ALL TESTS PASSED==========\n");
  exit(0);
}