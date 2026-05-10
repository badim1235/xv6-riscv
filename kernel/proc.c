#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
#include "defs.h"
#include "file.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

const int nice_weight[40] = {
/*  0*/ 88761, 71755, 56483, 46273, 36291,
/*  5*/ 29154, 23254, 18705, 14949, 11916,
/* 10*/  9548,  7620,  6100,  4904,  3906,
/* 15*/  3121,  2501,  1991,  1586,  1277,
/* 20*/  1024,   820,   655,   526,   423,
/* 25*/   335,   272,   215,   172,   137,
/* 30*/   110,    87,    70,    56,    45,
/* 35*/    36,    29,    23,    18,    15
};

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  p->nice=20;

  p->weight = nice_weight[p->nice];
  p->runtime = 0;
  p->vruntime = 0;
  p->vdeadline = p->vruntime + (5000 * 1024 / p->weight);
  p->time_slice = 5;

  for (int i = 0; i < 64; i++) { // initialize the mmap_areas array
    p->mmap_areas[i].f = 0;
    p->mmap_areas[i].addr = 0;
    p->mmap_areas[i].length = 0;
    p->mmap_areas[i].offset = 0;
    p->mmap_areas[i].prot = 0;
    p->mmap_areas[i].flags = 0;
    p->mmap_areas[i].p = 0; 
  }

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;

  if(p->pagetable) {
    for (int i = 0; i < 64; i++) {
      if (p->mmap_areas[i].length > 0) {
        uint64 start_va = MMAPBASE + p->mmap_areas[i].addr;
        uint64 length = p->mmap_areas[i].length;
        

        for (uint64 va = start_va; va < start_va + length; va += PGSIZE) {
          pte_t *pte = walk(p->pagetable, va, 0);
          if (pte != 0 && (*pte & PTE_V) != 0) {
            uint64 pa = PTE2PA(*pte);
            kfree((void*)pa);
            *pte = 0;
          }
        }
        
        // Closing open file
        if (!(p->mmap_areas[i].flags & MAP_ANONYMOUS) && p->mmap_areas[i].f) {
          fileclose(p->mmap_areas[i].f);
        }
        
        p->mmap_areas[i].length = 0;
      }
    }
  }

  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;

  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  for(i = 0; i < 64; i++){
    np->mmap_areas[i] = p->mmap_areas[i];

    if(np->mmap_areas[i].length > 0) {
      if(!(np->mmap_areas[i].flags & MAP_ANONYMOUS)){
        filedup(np->mmap_areas[i].f);
      }

      uint64 start_va = MMAPBASE + p->mmap_areas[i].addr;
      uint64 end_va = start_va + p->mmap_areas[i].length;

      for (uint64 va = start_va; va < end_va; va += PGSIZE) {
        pte_t *pte = walk(p->pagetable, va, 0);
        if (pte != 0 && (*pte & PTE_V) != 0) {
          uint64 pa = PTE2PA(*pte);
          uint flags = PTE_FLAGS(*pte);

          char *mem = kalloc();
          if (mem == 0) {
            freeproc(np);
            release(&np->lock);
            return -1;
          }
          
          memmove(mem, (char*)pa, PGSIZE);

          if (mappages(np->pagetable, va, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            freeproc(np);
            release(&np->lock);
            return -1;
          }
        }
      }
    }
  }

  safestrcpy(np->name, p->name, sizeof(p->name));

  np->nice = p->nice;
  np->weight = p->weight;
  np->runtime = 0;
  np->vruntime = p->vruntime;
  np->vdeadline = np->vruntime + (5000 * 1024 / np->weight);
  np->time_slice = 5;


  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int v0=__INT_MAX__; 
    int sum_weight = 0;
    int sum_left=0;
    struct proc *best_proc = 0;
    int min_vdeadline = __INT_MAX__;
    int found = 0;
    
    for(p = proc; p < &proc[NPROC]; p++) { // runnable process의 v0, 가중치 합 계산
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        if(p->vruntime<v0){
          v0=p->vruntime;
        }
        sum_weight += p->weight;
      }
      release(&p->lock);
    }

    if(sum_weight > 0) {
      for(p = proc; p < &proc[NPROC]; p++) { // runnable process의 left 계산
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          sum_left+=(p->vruntime-v0)*p->weight;
        }
      release(&p->lock);
      }

    for(p = proc; p < &proc[NPROC]; p++) { // runnable process 중 vdeadline이 가장 작은 process 선택
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          if((sum_left >= (p->vruntime-v0)*p->weight) && (p->vdeadline < min_vdeadline)) {
            min_vdeadline = p->vdeadline;
            best_proc = p;
          }
      } 
      release(&p->lock);
    } 
  }

    if(best_proc != 0) { //선정된 best_proc에게 cpu 할당
      acquire(&best_proc->lock);
      if(best_proc->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        best_proc->state = RUNNING;
        c->proc = best_proc;
        swtch(&c->context, &best_proc->context); //context switching

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }

      release(&best_proc->lock);
      found = 1;
    }

    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;

        p->time_slice = 5;
        p->vdeadline=p->vruntime + (5000 * 1024 / p->weight);
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}


int
getnice(int pid)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      int n = p->nice;
      release(&p->lock);
      return n;
    }
    release(&p->lock);
  }
  return -1;
}


int
setnice(int pid, int value)
{
  if(value < 0 || value > 39) return -1;
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->nice = value;
      p->weight = nice_weight[value]; 
      p->vdeadline = p->vruntime + (5 * 1024 / p->weight);
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

extern uint ticks;

void
ps(int pid)
{
  static char *states[] = {
    [UNUSED]    "unused",
    [USED]      "used",
    [SLEEPING]  "sleep ",
    [RUNNABLE]  "runble",
    [RUNNING]   "run   ",
    [ZOMBIE]    "zombie"
  };

  struct proc *p;
  int valid = 0;

  if(pid == 0) {
    valid = 1;
  } else {
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->pid == pid && p->state != UNUSED){
        valid = 1;
        release(&p->lock);
        break;
      }
      release(&p->lock);
    }
  }

  if(valid == 0) return;

  int v0 = 2147483647; 
  int sum_weight = 0;
  int sum_left = 0;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state == RUNNING){
      if(p->vruntime < v0) v0 = p->vruntime;
      sum_weight += p->weight;
    }
    release(&p->lock);
  }

  if(sum_weight > 0){
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      if(p->state == RUNNABLE || p->state == RUNNING){
        sum_left += (p->vruntime - v0) * p->weight;
      }
      release(&p->lock);
    }
  }

  printf("name\tpid\tstate\tpriority\truntime/weight\truntime\tvruntime\tvdeadline\tis_eligible\ttick %d\n",
  ticks*1000);
  
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if((pid == 0 || p->pid == pid) && (p->state != UNUSED)){
      
      int is_eligible = 0;
      if (p->state == RUNNABLE || p->state == RUNNING) {
         if (sum_weight > 0 && sum_left >= (p->vruntime - v0) * p->weight) {
             is_eligible = 1;
         }
      } else {
         is_eligible = 1;
      }


      int m_runtime = p->runtime*1000;
      int m_vruntime = p->vruntime;
      int m_vdeadline = p->vdeadline;
      int rw_ratio = (p->weight > 0) ? (m_runtime / p->weight) : 0; 

      printf("%s\t%d\t%s\t%d\t\t%d\t\t%d\t%d\t\t%d\t\t%s\t\n", 
             p->name, p->pid, states[p->state], p->nice, 
             rw_ratio, m_runtime, m_vruntime, m_vdeadline, 
             is_eligible ? "true" : "false");
    }
    release(&p->lock);
  }
}

uint64
meminfo(void)
{
  uint64 free_mem = get_free_pages_count();
  return free_mem*4096;
}

int
waitpid(int pid)
{
  struct proc *pp;
  int havekids;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p && pp->pid == pid){ 
        acquire(&pp->lock);
        havekids = 1;
        if(pp->state == ZOMBIE){
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return 0;
        }
        release(&pp->lock);
      }
    }

    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    sleep(p, &wait_lock);
  }
}

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
  struct proc *p = myproc();
  struct file *f = 0;

  // parameter error check
  if(length <= 0 || addr % PGSIZE != 0 || length % PGSIZE != 0) {
    return 0; // Failed: return 0
  }

  uint64 start_addr = addr + MMAPBASE;

  int anonymous = (flags & MAP_ANONYMOUS) ? 1 : 0;
  int populate  = (flags & MAP_POPULATE)  ? 1 : 0;
  int read      = (prot & PROT_READ)      ? 1 : 0;
  int write     = (prot & PROT_WRITE)     ? 1 : 0;

  //error check
  if (!anonymous) {
    // file mapping, but fd = -1
    if (fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0) {
      return 0; 
    }
    //The protection of the file and the prot of the parameter are different
    if (read && !f->readable) {
      return 0;
    }
    if (write && !f->writable) {
      return 0;
    }
  }
  else { //anonymous mapping, but fd != -1 or offset != 0
    if (fd != -1 || offset != 0) {
      return 0;
    }
  }

  // Find empty area
  struct mmap_area *area = 0;
  int i=0;
  for(i=0;i<64;i++){
    if(p->mmap_areas[i].length==0){
      area=&p->mmap_areas[i];
      break;
    }
  }
  if(!area){
    return 0; //no empty area
  }

  // If file mapping
  if(!anonymous){
    filedup(f);
    area->f=f;
    area->offset=offset;
  }
  else { //Anonymous mapping
    area->f=0;
    area->offset=0;
  }
  // Common
  area->addr=addr;
  area->length=length;
  area->prot=prot;
  area->flags=flags;
  area->p=p;
  

  // popluate
  if (populate) {
    // Loop with page size. length range
    for (uint64 va = start_addr; va < start_addr + length; va += PGSIZE) {
      
      // Allocate physical memory
      void *pa = kalloc();
      if (pa == 0) {
        return 0; // Fail to allocate
      }
      memset(pa, 0, PGSIZE); // Anonymous mapping should be filled with 0

      // Setting PTE flags

      int pte_flags = PTE_U;
      if (read) pte_flags |= PTE_R;
      if (write) pte_flags |= PTE_W;

      // Mapping virtual address to physical address
      if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, pte_flags) != 0) {
        kfree(pa);
        return 0;
      }

      // If file mapping, read data from file to physical memory
      if (!anonymous) {
        int page_offset = va - start_addr;
        
        // Read data from file to physical memory
        ilock(f->ip);
        readi(f->ip,0, (uint64)pa, offset+page_offset, PGSIZE);
        iunlock(f->ip);
      }
    }
  }
  // return virtual address
  return start_addr;
}

int
munmap(uint64 addr)
{
  struct proc *p = myproc();

  // Find the area to unmap
  struct mmap_area *area = 0;
  for (int i = 0; i < 64; i++) {
    if (p->mmap_areas[i].length > 0 && (MMAPBASE + p->mmap_areas[i].addr) == addr) {
      area = &p->mmap_areas[i];
      break;
    }
  }

  if(!area){
    return -1; // No such area
  }

  // Unmap that memory area
  uint64 start_va=MMAPBASE + area->addr;
  uint64 length=area->length;

  for(uint64 va=start_va; va < start_va+length; va+=PGSIZE){
    pte_t *pte = walk(p->pagetable, va, 0);
    if(pte && (*pte & PTE_V)){ // pte exists, and valid
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa); // Free physical memory
      *pte=0; // Unmap virtual address
    }
  }

  if(!(area->flags & MAP_ANONYMOUS) && area->f){ // if file mapping, decrease reference count of file
    fileclose(area->f);
  }
  //map structure initialization
  area->f=0;
  area->addr=0;
  area->length=0;
  area->offset=0;
  area->prot=0;
  area->flags=0;
  area->p=0;

  return 1; // success
}