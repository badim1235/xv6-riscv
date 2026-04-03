#include "kernel/types.h"
#include "user/user.h"

// 선수들이 달릴 무한 트랙 (스냅샷이 찍힐 때 무조건 실행 중이도록 아주 길게 설정)
void cpu_bound_task() {
  volatile long long i;
  volatile long long j;
  long long sample = 0;
  for (i = 0; i < 500000; i++) {
     for (j = 0; j < 50000; j++) {
        sample += i + j;
     }
  }
  exit(0);
}

int main() {
  printf("=== EEVDF SCHEDULING TEST ===\n");
  printf("Spawning 3 Racers: Nice 0, Nice 10, Nice 20...\n");

  // 선수 1: Nice 0 (가중치 88761 - 우사인 볼트)
  int pid0 = fork();
  if (pid0 == 0) {
    setnice(getpid(), 0);  
    cpu_bound_task();
  }

  // 선수 2: Nice 10 (가중치 9548 - 일반인)
  int pid10 = fork();
  if (pid10 == 0) {
    setnice(getpid(), 10); 
    cpu_bound_task();
  }

  // 선수 3: Nice 20 (가중치 1024 - 거북이)
  int pid20 = fork();
  if (pid20 == 0) {
    setnice(getpid(), 20); 
    cpu_bound_task();
  }

  // 심판(부모)은 3명의 자식이 충분히 경쟁할 수 있도록 150틱 동안 잠을 잡니다.
  // (부모가 잠든 사이 자식들끼리 피 터지게 CPU를 나눠 씁니다)
  pause(600);

  // 150틱 후 깨어난 직후, 경쟁 한복판의 상태를 촬영(Snapshot)합니다!
  printf("\n--- Race Snapshot (150 ticks later) ---\n");
  ps(0);

  // 촬영이 끝났으므로 선수들을 강제 종료시킵니다. (무한 루프 방지)
  kill(pid0);
  kill(pid10);
  kill(pid20);

  // 좀비 프로세스 회수 (지원 님이 구현하신 waitpid 활용)
  waitpid(pid0);
  waitpid(pid10);
  waitpid(pid20);

  printf("=== TEST COMPLETE ===\n");
  exit(0);
}