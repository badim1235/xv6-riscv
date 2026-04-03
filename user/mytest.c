#include "kernel/types.h"
#include "user/user.h"

int main() {
  int pid = getpid();
  
  printf("--- Testing getnice & setnice ---\n");
  int initial_nice = getnice(pid);
  printf("Current nice value: %d\n", initial_nice);

  setnice(pid, 10);
  printf("Updated nice value: %d\n", getnice(pid));

  printf("\n--- Testing ps ---\n");
  ps(0);
  
  printf("\n--- Testing ps with specific PID ---\n");
  ps(pid);

  int pid1, pid2;

  printf("\n--- Testing meminfo ---\n");
  uint64 mem = meminfo();
  // uint64의 큰 숫자가 깨지지 않도록 %ld를 사용하는 것이 안전합니다.
  printf("available memory : %ld bytes\n", mem); 

  printf("\n--- Testing waitpid ---\n");
  printf("wait\n");

  if((pid1 = fork()) == 0) {
    setnice(getpid(), 10); // [해결 1] 예시 출력처럼 자식1의 nice 값을 10으로 설정
    printf("start1\n");
    pause(30);             // (주의: 환경에 따라 pause로 연결하셨다면 pause(30)으로 바꾸세요)
    printf("end1\n");
    exit(0);
  }

  // [해결 2] 자식 1과 2가 동시에 printf를 호출해 글자가 겹치는 현상 방지
  pause(10); 

  // 두 번째 자식 생성
  if((pid2 = fork()) == 0) {
    setnice(getpid(), 10); // 자식2의 nice 값을 10으로 설정
    printf("start2\n");
    pause(60);             // 자식 1보다 늦게 끝나도록 대기
    printf("end2\n");
    exit(0);
  }

  // [해결 3] 부모가 getnice를 호출하기 전에 자식들이 setnice를 끝낼 수 있도록 아주 잠시 대기
  pause(5); 

  // [해결 4] waitpid가 끝나면 자식 정보가 지워지므로(freeproc), 그 전에 nice 값을 미리 저장해둠!
  int n1 = getnice(pid1);
  int n2 = getnice(pid2);

  // 특정 PID가 죽을 때까지 기다림 (성공 시 미리 저장해둔 n1 출력)
  if(waitpid(pid1) == 0) {
    printf("done1 %d %d\n", pid1, n1);
  }

  // 두 번째 자식 기다림
  if(waitpid(pid2) == 0) {
    printf("done2 %d %d\n", pid2, n2);
  }

  exit(0);
}