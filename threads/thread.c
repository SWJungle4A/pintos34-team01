#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "vm/vm.h"
#ifdef USERPROG
#include "userprog/process.h"
// #ifdef VM
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
// 대기중인 쓰레드들이 담겨있는 큐
static struct list ready_list;

/* 자고 있는 쓰레드들이 담겨 있는 큐 */
static struct list sleep_list;

/* sleep_list의 쓰레드 중 최소 wakeup_tick을 저장 */
static uint64_t next_tick_to_awake;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
// 초기 쓰레드 생성
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
// 제거를 요청할 쓰레드의 앞, 뒤 정보를 담는 구조체
static struct list destruction_req;


/* Statistics. */
static long long idle_ticks;    /* idle thread가 수행되는데 걸리는 시간 */
static long long kernel_ticks;  /* kernel thread가 수행되는 데 걸리는 시간 */
static long long user_ticks;    /* 사용자 프로그램이 수행되는데 걸리는 시간 */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* 1. Alarm Call */
void thread_sleep(int64_t ticks);				// 실행중인 쓰레드를 슬립으로 바꿈
void thread_awake(int64_t ticks);				// sleep_list에서 깨워야할 쓰레드를 깨움
void update_next_tick_to_awake(int64_t ticks); // 최소 tick을 가진 쓰레드 저장
int64_t get_next_tick_to_awake(void);		   // thread.c의 next_tick_to_awake 반환

/* 2. Priority Scheduling */
void test_max_priority (void);
bool cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

bool check_preemption(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);
	next_tick_to_awake = INT64_MAX;

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
/* idle 쓰레드를 생성하고, Preemptive thread scheduling을 시작함 */
void thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	// idle 쓰레드를 만들고, 맨 처음 ready_list에 들어감
	// 세마포어를 1로 UP 시켜 공유자원에 접근이 가능하게 만들고 바로 block
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	// 인터럽트 활성화. 이제 쓰레드 스케줄링이 가능하다.
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
/* 새 커널 스레드를 만들고 바로 ready_list에 넣어줌 */
tid_t thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();
	
	struct thread *curr = thread_current();
	list_push_back(&curr->child_list, &t->child_elem);

	/* project 2 : system call */
	t->file_descriptor_table = palloc_get_multiple(PAL_ZERO, FDT_PAGES);
	if (t->file_descriptor_table == NULL) {
		return TID_ERROR;
	}
	t->fdidx = 2; // 0은 stdin, 1은 stdout에 이미 할당
	t->file_descriptor_table[0] = 1;	// stdin 자리
	t->file_descriptor_table[1] = 2;	// stdout 자리

	t->stdin_count = 1;
	t->stdout_count = 1;
	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread; // 다음에 자식이 cpu 잡으면 실행될 인스트럭션
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	// 새로 만든 thread의 우선 순위가 현재 실행되고 있는 thread보다 높을 경우 양보
	// struct thread *curr = thread_current();
	// if (curr->priority < t->priority) {
	if (check_preemption())
		thread_yield();

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */

// sleep_list에 있는 요소를 unblock 해주고, ready_list로 넣어주는 함수
void thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));
	// 리스트로 요소를 삽입하는 동안 인터럽트가 발생하지 않도록 인터럽트를 비활성화
	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL);
	// 인터럽트 원복
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
// 현재 실행중인 쓰레드가 문제가 없는지 확인한 다음, 그 쓰레드를 가리키는 포인터를 반환한다. 
struct thread *thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t)); // thread인지 확인. stack overflow를 체크함
	ASSERT (t->status == THREAD_RUNNING); // 쓰레드가 현재 실행중인지 체크

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
/* cpu를 양보하고 ready_list에 스레드를 삽입하는 함수 */
void thread_yield (void) {
	struct thread *curr = thread_current (); // 현재 실행중인 thread를 저장
	enum intr_level old_level;

	ASSERT (!intr_context ()); // 외부 인터럽트를 수행중이라면 종료. 외부 인터럽트는 인터럽트를 당하면 안된다

	old_level = intr_disable (); // 인터럽트 중지 및 이전 인터럽트 상태 저장
	if (curr != idle_thread) // 현재 쓰레드가 idle 쓰레드가 아니라면
		// list_push_back (&ready_list, &curr->elem); // 현재 스레드를 대기큐의 마지막으로 보냄
		list_insert_ordered(&ready_list, &curr->elem, cmp_priority, NULL);
	do_schedule (THREAD_READY); // 대기큐 첫번째에 있는 쓰레드와 컨텍스트 스위칭
	intr_set_level (old_level); // 인자로 전달된 인터럽트 상태로 인터럽트를 설정하고, 이전 인터럽트 상태를 반환
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority (int new_priority) {
	// struct list_elem *pri_thread = list_front(&ready_list);
	// struct thread *priority_of_pri_thread = list_entry(pri_thread, struct thread, elem); 
	
	// if (new_priority > priority_of_pri_thread->priority) {
	// 	thread_current ()->priority = new_priority;
	// }
	// else {
	// 	thread_current ()->priority = priority_of_pri_thread->priority;
	// }
	thread_current() ->init_priority = new_priority;

	/* 초기 우선순위가 변경되었을 때, 해당 쓰레드의 새 우선 순위와
	donations 안의 우선 순위를 비교해서 donate가 제대로 이루어질 수 있도록 한다. */
	refresh_priority();

	test_max_priority();
	
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
/* - 실행 중인 쓰레드가 없을 때 실행되는 쓰레드.
   - 맨 처음 thread_start()가 호출될 때 ready_list에 먼저 들어가 있는다. */
static void idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current (); // 현재 돌고 있는 쓰레드가 idle 밖에 없음.
	sema_up (idle_started); // 세마포어의 값을 1로 만들어서 공유 자원의 공유 (인터럽트)가 가능하게 만듬.

	for (;;) {
		/* Let someone else run. */
		intr_disable (); // 자신(idle)을 block해주기 전까지 인터럽트 당하면 안되므로 인터럽트를 비활성화 해줌
		thread_block (); // 자신을 block함

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
/*	- 만든 쓰레드를 초기화 해주는 함수.
	- 맨 처음 쓰레드의 상태는 block 상태
	- 커널 스택 포인터 rsp의 위치도 같이 정해줌. rsp의 값은 커널이 함수 혹은 변수를 쌓을수록 점점 작아짐 */
static void init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);										// 가리키는 공간이 비어있지 않고
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);	// priority의 값이 제대로 설정되어 있고 (0~63)
	ASSERT (name != NULL);									// 이름이 들어갈 공간이 있는지 (디버그할 때 사용함)

	memset (t, 0, sizeof *t);								// 해당 메모리를 모두 0으로 초기화
	t->status = THREAD_BLOCKED;								// 맨 처음 쓰레드의 상태를 BLOCKED
	strlcpy (t->name, name, sizeof t->name);				// 이름을 써줌.
	
	// 커널 스택 포인터의 위치를 지정해줌. 원래 쓰레드의 위치 t + 4KB(포인터 변수 크기)
	// 커널 스택에 변수들이 쌓이면서 이 값은 작아짐.
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority; 	// 우선순위 정해줌
	t->magic = THREAD_MAGIC;

	/* --- Project2: User programs - system call --- */
	// t->exit_status = 0;
	
	/* priority donation 관련 초기화 */
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);

	/* 자식 리스트 및 세마포어 초기화 */
	list_init(&t->child_list);
	sema_init(&t->wait_sema,0);
	sema_init(&t->fork_sema,0);
	sema_init(&t->free_sema,0);

	t->running = NULL;
	/* --- Project2: User programs - system call --- */
	
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
/* iretq를 사용하여 쓰레드 실행 */
void
do_iret (struct intr_frame *tf) {
	// args라는 값을 실행시키위해 어셈블리어 명령을 통해 레지스터에 넣음.
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

// 컨텍스트 스위칭 실시
static void schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run (); // ready_list 출구에 서 있는 쓰레드 

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
// 다음에 깨워야할 tick의 최소값을 갱신하는 함수
void update_next_tick_to_awake(int64_t ticks)
{
	next_tick_to_awake = (next_tick_to_awake > ticks) ? ticks : next_tick_to_awake;
}
// sleep_list에서 깨워야할 쓰레드를 찾아서 큐에서 제거하고 unblock해주는 함수
void thread_awake(int64_t ticks) {
	struct list_elem *curr = list_begin(&sleep_list);
	struct thread *t;

	/* sleep_list의 끝까지 순회 */
	while (curr != list_end(&sleep_list)) {
		t = list_entry(curr, struct thread, elem);
		if (t->wakeup_tick <= ticks) { // 깨울시간이 되었거나 지났다면
			curr = list_remove(&t->elem); // 해당 쓰레드를 sleep_queue에서 제거하고, curr을 삭제한 쓰레드가 가리키는 위치로 갱신
			thread_unblock(t); // 해당 쓰레드를 unblock
		} else { // 깨울시간이 되지 않았다면
			curr = list_next(curr); // 큐의 다음을 검색
			update_next_tick_to_awake(t->wakeup_tick); // tick을 갱신
		}
	}
}

// thread를 block 상태로 만들고 sleep_list에 삽입하여 대기
void thread_sleep(int64_t ticks) {
	struct thread *curr = thread_current();
	enum intr_level old_level;

	old_level = intr_disable();
	ASSERT(curr != idle_thread);

	curr->wakeup_tick = ticks;
	update_next_tick_to_awake(curr->wakeup_tick);
	list_push_back(&sleep_list, &curr->elem);

	thread_block();

	intr_set_level(old_level);
}

// 다음에 깨어나야할 쓰레드의 tick값을 리턴
int64_t get_next_tick_to_awake(void) {
	return next_tick_to_awake;
}

// 첫번째 인자의 우선순위가 높으면 1을 반환하고, 두번째 인자의 우선 순위가 높으면 0을 반환한다.
bool cmp_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *tmp_a = list_entry(a, struct thread, elem);
	struct thread *tmp_b = list_entry(b, struct thread, elem);
	
	return (tmp_a->priority > tmp_b->priority) ? 1 : 0;
}

// ready_list에서 우선 순위가 가장 높은 쓰레드와 현재 쓰레드의 우선순위를 비교
// 만약 현재 쓰레드의 우선 순위가 더 작다면 CPU를 양보한다.
void test_max_priority(void)
{	
	// ready_list가 비어있을 경우 실행하지 않고 return 
	if (list_empty(&ready_list)){
		return;
	}
	// 현재 쓰레드의 정보 가져옴
	struct thread *curr = thread_current();
	
	// ready_list에서 우선 순위가 가장 높은 녀석의 정보
	struct list_elem *pri_thread = list_front(&ready_list);
	struct thread *priority_of_pri_thread = list_entry(pri_thread, struct thread, elem); 

	// ready_list에서 뽑은 쓰레드의 우선순위가 현재 쓰레드의 우선순위보다 높다면
	// thread_yield() 호출
	if (curr->priority < priority_of_pri_thread->priority) {
		thread_yield();
	}
}

bool check_preemption(void){
    if(list_empty(&ready_list)) return false;
    return list_entry(list_front(&ready_list), struct thread, elem) -> priority > thread_current() -> priority;
}