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
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Average number of threads ready to run per minute */
fixed_point load_avg;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static int thread_get_max_priority (void);
static void thread_reinsert_ready_list (struct thread *);
static int thread_get_donated_priority (struct thread *);
static bool thread_compare_donation (const struct list_elem *a,
                                     const struct list_elem *b,
                                     void *aux UNUSED);
static void thread_calculate_bsd_priority (struct thread *t, void *aux UNUSED);
static void thread_recalculate_bsd_variables (void);
static void thread_calculate_recent_cpu (struct thread *t, void *aux UNUSED);
static void thread_calculate_load_avg (void);
static int get_ready_threads_size (void);


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
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  load_avg = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (thread_mlfqs)
  {
    thread_recalculate_bsd_variables ();
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
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
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  thread_max_yield ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
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
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list,
                      &t->elem,
                      &thread_compare_priority,
                      NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Returns the highest priority of all ready threads */
static int
thread_get_max_priority (void)
{
    int return_val = PRI_MIN - 1;

    enum intr_level old_level = intr_disable ();

    if (!list_empty (&ready_list))
      {
        struct thread *t = list_entry (list_begin (&ready_list),
                                       struct thread, elem);
        return_val = t->priority;
      }

    intr_set_level (old_level);
    return return_val;
}

/* Yields the CPU to the thread with highest priority */
void
thread_max_yield (void)
{
  if (thread_get_max_priority () > thread_get_priority ())
  {
    if (intr_context ())
    {
      intr_yield_on_return ();
    }
    else
    {
      thread_yield ();
    }
  }
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered(&ready_list,
                        &cur->elem,
                        &thread_compare_priority,
                        NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Returns the maximum priority of all donations */
static int
thread_get_donated_priority (struct thread *t)
{
  ASSERT (is_thread (t));
  enum intr_level old_level = intr_disable ();

  int donated_priority = PRI_MIN - 1;
  if (!list_empty (&t->donations))
  {
    struct thread *doner = list_entry (list_front (&t->donations),
                                       struct thread, dona_elem);
    donated_priority = doner->priority;
  }

  intr_set_level (old_level);
  return donated_priority;
}

/* Reinserts thread into correct position in sorted list */
static void
thread_reinsert_ready_list (struct thread *t)
{
  if (t->status == THREAD_READY)
    {
      /* Interrupts should already be off for
         non-running threads */
      ASSERT (intr_get_level () == INTR_OFF);

      list_remove (&t->elem);
      list_insert_ordered (&ready_list,
                          &t->elem,
                          thread_compare_priority,
                          NULL);
    }
}

/* Sets the thread's priority to the highest of it's base and all
  donations */
void
thread_reset_priority (struct thread *t)
{
  ASSERT (is_thread (t));

  enum intr_level old_level = intr_disable ();

  int donated_priority = thread_get_donated_priority (t);
  if (donated_priority > t->base_priority)
    t->priority = donated_priority;
  else
    t->priority = t->base_priority;

  thread_reinsert_ready_list(t);
  intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY and yields if
   new priority is not highest priority */
void
thread_set_priority (int new_priority) 
{
  ASSERT (PRI_MIN <= new_priority && new_priority <= PRI_MAX);
  if (!thread_mlfqs)
  {
    thread_current ()->base_priority = new_priority;
    thread_reset_priority (thread_current ());
    thread_max_yield ();
  }
}

/* Priority comparison for threads in the priority_donation list. */
static bool
thread_compare_donation (const struct list_elem *a,
                         const struct list_elem *b,
                         void *aux UNUSED)
{
  struct thread *a_thread = list_entry (a, struct thread, dona_elem);
  struct thread *b_thread = list_entry (b, struct thread, dona_elem);
  return a_thread->priority > b_thread->priority;
}

/* Calculate an up-to-date priority for the given thread and then
   recursively donate this to the thread which holds its' required lock */
void
thread_donate_priority (struct thread *t)
{
    ASSERT (intr_get_level () == INTR_OFF);
    ASSERT (is_thread (t));

    while (t->required_lock != NULL)
    {
      thread_reset_priority (t);

      struct thread *holder = t->required_lock->holder;
      ASSERT (holder != t);

      /* If the thread is not current and has a required lock, it may
         have already donated so remove old donation to make way for
         updated donation */
      if (thread_current () != t)
      {
        thread_remove_donation (t);
      }

      if (holder != NULL)
      {
        ASSERT (is_thread (holder));

        list_insert_ordered (&holder->donations,
                             &t->dona_elem,
                             thread_compare_donation,
                             NULL);

        /* Iterate through chain of holders */
        t = holder;
      }
  }
    thread_reset_priority (t);
}

/* Remove the priority donated to a thread holding a lock this thread
   previously required */
void
thread_remove_donation (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));

  if (t->dona_elem.next != NULL)
    {
      list_remove (&t->dona_elem);
      t->dona_elem.next = NULL;
    }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

bool
thread_compare_priority (const struct list_elem *a,
                         const struct list_elem *b,
                         void *aux UNUSED)
{
  struct thread *a_thread;
  struct thread *b_thread;
  a_thread = list_entry (a, struct thread, elem);
  b_thread = list_entry (b, struct thread, elem);

  return a_thread->priority > b_thread->priority;
}

/* Returns number of ready threads plus the current one if not idle */
static int
get_ready_threads_size (void)
{
  ASSERT (thread_mlfqs);

  int size = list_size (&ready_list);
  if (thread_current () != idle_thread)
  {
    size++;
  }
  return size;
}

/* Calculates bsd_scheduler priority using the formula
   priority = PRI_MAX - recent_cpu / 4 - 2 * nice
   and ensures new_priority is valid */
static void
thread_calculate_bsd_priority (struct thread *t, void *aux UNUSED)
{
  ASSERT (thread_mlfqs);
  ASSERT (is_thread (t));

  fixed_point fp_priority_max = convert_to_fixed_point (PRI_MAX);
  fixed_point temp_recent_cpu = div_fixed_by_int (t->recent_cpu, 4);
  fixed_point temp_nice = convert_to_fixed_point (t->nice * 2);
  fixed_point result = fixed_subtract_fixed (fp_priority_max, temp_recent_cpu);
  result = fixed_subtract_fixed (result, temp_nice);

  int new_priority = convert_to_int_round_down (result);
  if (new_priority < PRI_MIN)
  {
    new_priority = PRI_MIN;
  }
  else if (new_priority > PRI_MAX)
  {
    new_priority = PRI_MAX;
  }
  t->priority = new_priority;
}

/* Recalculates variables for bsd:
   Thread priority is recalculated every 4 ticks.
   Recent_cpu for running thread is incremented every tick
   and recalculated for every thread once per second.  */
static void
thread_recalculate_bsd_variables (void)
{
  ASSERT (thread_mlfqs);
  struct thread *t = thread_current ();

  if (t != idle_thread)
  {
    ASSERT (t->status == THREAD_RUNNING);
    t->recent_cpu = add_fixed_to_int (t->recent_cpu, 1);
  }

  if (timer_ticks () % TIMER_FREQ == 0)
  {
    thread_calculate_load_avg ();
    thread_foreach (thread_calculate_recent_cpu, NULL);
  }
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  ASSERT (thread_mlfqs);
  ASSERT (nice >= NICE_MIN && nice <= NICE_MAX);

  struct thread *t = thread_current ();
  t->nice = nice;
  thread_calculate_bsd_priority (t, NULL);
  thread_reinsert_ready_list (t);
  thread_max_yield ();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  ASSERT (thread_mlfqs);

  return thread_current ()->nice;
}

/* Performs
  load_avg = (59 / 60) * load_avg + (1 / 60) * ready_lists
*/
static void
thread_calculate_load_avg (void)
{
  ASSERT (thread_mlfqs);

  fixed_point fp_59 = convert_to_fixed_point (59);
  fixed_point fp_1 = convert_to_fixed_point (1);
  fixed_point temp_load_avg = div_fixed_by_int (fp_59, 60);
  fixed_point temp_ready_coeff = div_fixed_by_int (fp_1, 60);
  temp_load_avg = multiply_fixed_by_fixed (temp_load_avg, load_avg);
  temp_ready_coeff =  multiply_fixed_by_int (temp_ready_coeff,
                                             get_ready_threads_size ());
  load_avg = add_fixed_to_fixed (temp_load_avg, temp_ready_coeff);
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  ASSERT (thread_mlfqs);

  fixed_point temp_load_avg = multiply_fixed_by_int (load_avg, 100);
  return convert_to_int_round_nearest (temp_load_avg);
}

/*Calculates bsd_scheduler priority using the formula
  recent_cpu = (2 * load_avg) / (2 * load_avg + 1) * recent_cpu + nice
*/
static void
thread_calculate_recent_cpu (struct thread *t, void *aux UNUSED)
{
  ASSERT (thread_mlfqs);

  fixed_point recent_cpu = t->recent_cpu;
  fixed_point temp_load_avg = multiply_fixed_by_int (load_avg, 2);
  fixed_point temp_sum = add_fixed_to_int (temp_load_avg, 1);
  fixed_point coefficient = div_fixed_by_fixed (temp_load_avg, temp_sum);
  recent_cpu = multiply_fixed_by_fixed (coefficient, recent_cpu);
  recent_cpu = add_fixed_to_int (recent_cpu, t->nice);
  t->recent_cpu = recent_cpu;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  ASSERT (thread_mlfqs);

  fixed_point temp_recent_cpu = thread_current ()->recent_cpu;
  temp_recent_cpu = multiply_fixed_by_int (temp_recent_cpu, 100);
  return convert_to_int_round_nearest (temp_recent_cpu);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

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
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->base_priority = priority;
  t->required_lock = NULL;
  t->magic = THREAD_MAGIC;

  list_init (&t->donations);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (thread_mlfqs)
  {
    thread_foreach (thread_calculate_bsd_priority, NULL);
    list_sort (&ready_list, thread_compare_priority, NULL);
  }

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
