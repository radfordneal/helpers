/* HELPERS - A LIBRARY SUPPORTING COMPUTATIONS USING HELPER THREADS
             C Procedures Implementing the Facility

   Copyright (c) 2013, 2014, 2015, 2016, 2018 Radford M. Neal.

   The helpers library is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "helpers-app.h"

#ifndef HELPERS_DISABLED

#ifndef HELPERS_NO_MULTITHREADING
#include <omp.h>
#endif


/* -----------------------------  OPTIONS  ---------------------------------- */

/* MAXIMUM NUMBER OF TASKS THAT CAN BE OUTSTANDING.  Must be a power of two
   minus one, and no more than 255 (to fit in an unsigned char).  (Setting 
   MAX_TASKS to exactly 255 may produce faster code, since masking will then
   be unnecessary if an unsigned char is 8 bits in size.) */

#ifdef HELPERS_MAX_TASKS /* Allow helpers-app.h or compile option to override */
#define MAX_TASKS HELPERS_MAX_TASKS
#else
#define MAX_TASKS 255
#endif

#if MAX_TASKS < 1 || MAX_TASKS > 255 || (MAX_TASKS & (MAX_TASKS+1)) != 0
#error Invalid value for MAX_TASKS
#endif


/* OPTION FOR AMOUNT OF TRACE OUTPUT.  Setting this option to 0 will disable
   all trace output, even when it is enabled by helpers_trace.  Normal trace
   output is obtained when the option is set to 1, with only a small overhead
   when trace output has not been enabled by helpers_trace.  Setting this 
   option to 2 or 3 produces more informative trace output, but at the cost of
   increasing overhead (more for 3 than 2) even when tracing is not enabled. 
   Setting this option to 4 produces lots of additional information on the 
   task list. */

#ifndef ENABLE_TRACE   /* Allow value from compile option to override below's */
#define ENABLE_TRACE 1 /* 0, 1, 2, 3, 4 for no, normal, extra... trace output */
#endif


/* OPTION FOR DISABLING STATISTICS.  If ENABLE_STATS is defined as 0, the
   helpers_stats procedure does nothing, which gives a small savings in time
   for some common operations. */

#ifndef ENABLE_STATS   /* Allow value from compile option to override below's */
#define ENABLE_STATS 1
#endif


/* COUNT OF TIMES TO CHECK FOR NON-EMPTY UNTAKEN QUEUE BEFORE SUSPENDING. */

#ifndef SPIN_EMPTY    /* Allow value from compile option to override below's */
#define SPIN_EMPTY 1000
#endif


/* ----------------------------  OMP SETUP ---------------------------------- */

/* MODIFY LOCK STRUCTURE.  Here, my_lock_t is defined as a union of
   the omp lock structure and eight doubles (64 bytes) for two
   reasons.  One is to try to avoid false cache invalidation when two
   locks are near each other.  The other is to guard against the
   possibility that the omp_lock_t type in omp.h is not the correct
   size, which is known to sometimes happen due to confusion between
   versions of omp.h meant for 32-bit vs. 64-bit platforms. */

#ifndef HELPERS_NO_MULTITHREADING

typedef union { omp_lock_t lock; double pad[8]; } my_lock_t;

#endif


/* ---------------------------  DECLARATIONS -------------------------------- */

/* DEFAULT DEFINITIONS FOR MACROS.  Defined here if not in helpers-app.h. */

#ifndef helpers_var_name
#define helpers_var_name(v) "?"
#endif

#ifndef helpers_task_name
#define helpers_task_name(t) "?"
#endif

#ifndef helpers_printf
#include <stdio.h>
#define helpers_printf printf
#endif

#ifndef helpers_is_being_computed
#define helpers_is_being_computed(v) 1
#endif

#ifndef helpers_is_in_use
#define helpers_is_in_use(v) 1
#endif

#ifndef HELPERS_TASK_DATA_AMT
#define HELPERS_TASK_DATA_AMT 1
#endif


/* NULL VARIABLE POINTER, AND VARIABLE NAME MACRO HANDLING NULL. */

#define null ((helpers_var_ptr) 0)

#define var_name(v) ((v)==null ? "" : helpers_var_name(v))


/* MACROS FOR OMP ATOMIC DIRECTIVES.  Atomic read and write directives exist
   only in OpenMP 3.1, so with OpenMP 3.0 access to the data types used must be 
   atomic due to the architecture.  See "imp-doc" for discussion.  Use of the
   OpenMP 3.1 atomic read and write directives can be disabled by defining
   ASSUME_ATOMIC_READ_CHAR, ASSUME_ATOMIC_WRITE_CHAR, ASSUME_ATOMIC_READ_SIZE, 
   or ASSUME_ATOMIC_WRITE_SIZE, which may be advisable for performance reasons,
   if the architecture guarantees atomicity in any case. */

#define ATOMIC_READ_CHAR(_stmt_) _stmt_   /* First define macros for use with */
#define ATOMIC_WRITE_CHAR(_stmt_) _stmt_  /*   OpenMP 3.0, or when use of the */
#define ATOMIC_READ_SIZE(_stmt_) _stmt_   /*   atomic directives is disabled, */
#define ATOMIC_WRITE_SIZE(_stmt_) _stmt_  /*   or when no multithreading.     */

#ifndef HELPERS_NO_MULTITHREADING

#if _OPENMP>=201107  /* Redefine macros for OpenMP version 3.1 or later, if use
                        of an atomic directive isn't disabled in the context. */

#ifndef ASSUME_ATOMIC_READ_CHAR
#undef ATOMIC_READ_CHAR
#define ATOMIC_READ_CHAR(_stmt_) do { \
  _Pragma("omp atomic read") \
  _stmt_; \
} while (0)
#endif

#ifndef ASSUME_ATOMIC_WRITE_CHAR
#undef ATOMIC_WRITE_CHAR
#define ATOMIC_WRITE_CHAR(_stmt_) do { \
  _Pragma("omp atomic write") \
  _stmt_; \
} while (0)
#endif

#ifndef ASSUME_ATOMIC_READ_SIZE
#undef ATOMIC_READ_SIZE
#define ATOMIC_READ_SIZE(_stmt_) do { \
  _Pragma("omp atomic read") \
  _stmt_; \
} while (0)
#endif

#ifndef ASSUME_ATOMIC_WRITE_SIZE
#undef ATOMIC_WRITE_SIZE
#define ATOMIC_WRITE_SIZE(_stmt_) do { \
  _Pragma("omp atomic write") \
  _stmt_; \
} while (0)
#endif

#endif
#endif


/* MACRO FOR OMP FLUSH DIRECTIVE.  Can be used followed by a semicolon anywhere
   a statement is syntactically allowed. */

#ifndef HELPERS_NO_MULTITHREADING

#define FLUSH do { \
  _Pragma("omp flush"); \
} while (0)

#else

#define FLUSH do { } while (0)

#endif


/* MACRO FOR OMP WTIME FUNCTION.  Just returns zero if multithreading is
   disabled, so we don't depend on OpenMP being implemented. */

#ifndef HELPERS_NO_MULTITHREADING
#define WTIME() omp_get_wtime()
#else
#define WTIME() 0
#endif


/* NUMBER OF HELPERS.  Declared as extern in helpers.h, defined here.  Helpers 
   are identified by integers from 1 to helpers_num, with 0 sometimes used to 
   identify the master.  The value of helpers_num will not exceed HELPERS_MAX, 
   which will be no more than 127, to allow a helper index to be stored in an 
   signed char.  A helper index of -1 is sometimes used to indicate no helper 
   (nor the master) is assigned. 

   Defined as the constant 0 if HELPERS_NO_MULTITHREADING defined */

#ifndef HELPERS_NO_MULTITHREADING

int helpers_num;          /* Number of helpers */

#endif

typedef signed char hix;  /* Type of a helper index (0 = master, -1 = none) */


/* DECLARATION OF TASK INDEXES.  Task indexes range from 1 to MAX_TASKS.
   An index of 0 either indicates no task, or sometimes a master-now task
   that hasn't really been scheduled.  Task indexes are stored in an unsigned
   char, so MAX_TASKS is limited to be at most 255. 

   The mtix type is for use in data structure accessed only by a single 
   thread (so atomicity of access is not an issue).  It may either be the 
   same as tix, or be some larger integer type, if that is more efficient 
   (and can be tuned for a particular implementation). */

typedef unsigned char tix;  /* Type of a task index, or of a count of tasks */

typedef int mtix;           /* Task index for use in one thread only */


/* TABLE OF TASKS.  The "info" fields in an entry in the "task" array are 
   initialized by the master when a task is scheduled.  The "done", "amt_out",
   and "helper" fields may be updated by both the master and the helpers, with 
   careful flushing but without a lock being used, since accesses to them
   are assumed to be atomic.

   Entry zero in the table is used for tasks run in the master that haven't 
   really been scheduled.  The "pipe" field in this entry should be kept zero 
   except when such a task is being done, so that pipelined task procedures 
   called directly by the master thread will work properly.  The "amt_out" 
   field for entry zero may be set, but will never be accessed (except for 
   trace output, so it is set to 0 before a task procedure is started).

   Entries are forced to be 256 bytes in size by the presence of the "space" 
   field (assuming that the "info" field is no more than 256 bytes in size). 
   This makes index arithmetic faster, and may reduce possible performance 
   degradation from cache invalidation when one entry is updated and a 
   different entry is then accessed.  Fields updated by helpers are all put 
   first to increase the chance that they are not split between cache lines
   (since the start of the structure is likely aligned to some extent).
*/

static union task_entry 
{ 
  struct task_info               /* Information on a task */
  { 
    /* Fields initialized by the master, updated by helpers and the master. */

    helpers_size_t amt_out;        /* Number of parts produced for output */
    hix helper;                    /* Helper that took this task, or 0, or -1 */
    char done;                     /* Has this task finished? */

    /* Fields below are written only by the master, but read by helpers too. 
       The "needed" field may be written by the master and read by helpers
       without synchronization. */

    signed char needed;            /* Needed by master? (+1 finish, -1 start) */
    tix pipe[3];                   /* Tasks producing inputs, 0 when done */
    short flags;                   /* Flags task was scheduled with */
    helpers_task_proc *task_to_do; /* Task procedure to execute */
    helpers_op_t op;               /* The unsigned integer operand */
    helpers_var_ptr var[3];        /* The output variable, [0], and the input 
                                      variables, [1] and [2]; any may be null */
    double task_data [HELPERS_TASK_DATA_AMT]; /* Data the task procedure may
                                      look at - currently only usable for tasks
                                      that result from merging other tasks */

    /* These fields are used only by the master. */

#   ifndef HELPERS_NO_HOLDING
    char is_on_hold;               /* Set to 1 if task is currently on hold */
#   endif
    char out_used;                 /* Set to 1 if the output has been used as
                                      the input of a task scheduled later */
    char not_in_use_before[3];     /* When 1, indicates that in1/in2 was not
                                      in use before this task was scheduled;
                                      not_in_use_before[0] isn't actually used;
                                      others used only if var is in use here */

    /* The fields below are used only when ENABLE_TRACE is 2 or more. */

    tix pipe_at_start[3];          /* Value of "pipe" when task is started */
    helpers_size_t first_amt[3];   /* First non-zero value from helpers_availN*/
    helpers_size_t last_amt[3];    /* Last value from helpers_availN */

    /* The fields below are used only when ENABLE_TRACE is 3 or more. */

    double start_wtime;            /* Wall clock time when started */
    double done_wtime;             /* Wall clock time when done */

  } info;

  char space[256];               /* Sets the size of each entry */

} task[MAX_TASKS+1];


/* ARRAY OF TASK ENTRIES CURRENTLY USED AND UNUSED.  Read and written
   only by the master thread.  The "used" array always contains all
   the task indexes from 1 to MAX_TASKS, in some order.  The first
   "helpers_tasks" of these are currently being used, and are in the
   order they were scheduled.  The remaining entries are unused, and
   in arbitrary order. */

static mtix used[MAX_TASKS]; /* All task indexes; first helpers_tasks in use */
int helpers_tasks = 0;       /* Number of tasks outstanding = indexes in use */


/* QUEUES OF TASK INDEXES.  These are circular queues, with in/out pointers
   incremented modulo MAX_TASKS+1, which must be a power of two.  They can 
   never overflow because the number of tasks scheduled but not yet gone is 
   limited to MAX_TASKS.  The index 0, used for tasks done directly in the 
   master, never appears in these queues. */

#define QSize (MAX_TASKS+1)  /* Number of entries in a wrap-around queue  */
#define QMask MAX_TASKS      /* Mask for implementing modulo QSize arithmetic */

/* Queue of master-only tasks.  This queue is accessed only by the master. 
   Tasks in this queue are in the order they were scheduled, which is the
   order in which they must be done. */

static mtix master_only[QSize], master_only_in, master_only_out;

/* Queue of tasks on hold.  Accessed only by the master. */

#ifndef HELPERS_NO_HOLDING

static mtix on_hold[QSize], on_hold_in, on_hold_out;

#endif

/* Queue of tasks not yet taken by a helper (or master), in arbitrary order.  
   The untaken_in pointer is modified only by the master; the untaken_out 
   pointer is modified by the master or a helper.  Accesses by helpers
   and the master that check for the queue being empty, or make it non-empty,
   are done with "untaken_lock" set.  */

static tix untaken[QSize], untaken_in, untaken_out;

#ifndef HELPERS_NO_MULTITHREADING

static my_lock_t untaken_lock;  /* Lock to set for accessing untaken queue */

#endif


/* LOCK SET FOR STARTING A TASK.  Set by a helper or the master when looking
   for a task to start.  When there's nothing to do, other helpers will block
   trying to set this lock. */

#ifndef HELPERS_NO_MULTITHREADING

static my_lock_t start_lock;   /* Lock set by thread looking for task to start*/

#endif


/* PAIR OF LOCKS USED FOR SUSPENDING A HELPER. */

#ifndef HELPERS_NO_MULTITHREADING

static hix suspended;      /* Helper that has suspended, or 0 if none */

static my_lock_t suspend_lock[2]; /* Locks used alternately to suspend helper */

static int which_suspends; /* Which lock a helper sets to suspend itself */
static int which_wakes;    /* Which lock the master unsets to wake helper */

static char suspend_initialized; /* Set to 1 when master has done initial set */

#endif


/* VARIABLE LIST.  Storage for an array of variables used by current tasks. 
   Each task uses at most three variables (out, in1, and in2).  The array
   is null-terminated. */

static helpers_var_ptr var_list[3*MAX_TASKS+1] = { (helpers_var_ptr) 0 };


/* VARIABLES PRIVATE TO EACH HELPER AND MASTER. */

static int this_thread;   /* What thread we are: 0 = master, other = helper # */

static mtix this_task;    /* The task this thread is doing, undefined if none,
                             except 0 in the master when it's not doing a task
                             so directly-called pipelined task procedures work*/

static struct task_info *this_task_info     /* Pointer to info for this_task, */
                          = &task[0].info;  /*    initialized for master here */

#ifndef HELPERS_NO_MULTITHREADING

#pragma omp threadprivate (this_thread, this_task, this_task_info)

#endif


/* VARIABLES HOLDING DISABLING OPTIONS. */

int helpers_are_disabled = 0;    /* 1 if helpers currently disabled */

#ifdef helpers_can_merge
int helpers_not_merging = 0;     /* 1 if task merging is not enabled */
int helpers_not_merging_now = 0; /* 1 if task merging not done at the moment */
#endif

#ifndef HELPERS_NO_MULTITHREADING
int helpers_not_pipelining;      /* 1 if pipelining currently disabled */
int helpers_not_pipelining_now;  /* 1 if not pipelining right now */
int helpers_not_multithreading;  /* 1 if multithreading currently disabled */
int helpers_not_multithreading_now; /* 1 if not multithreading right now */
#endif

#ifndef HELPERS_NO_HOLDING
int helpers_not_holding;         /* 1 if holding is not enabled */
int helpers_not_holding_now;     /* 1 if no holding at the moment */
#endif

static int flag_mask = ~0;       /* Mask used to clear task flags according
                                    to the settings of the above options */

/* TRACE VARIABLES. */

#if ENABLE_TRACE>0
static char trace = 0;    /* Are we printing trace information now? */
#else
#define trace 0           /* Tracing never enabled */
#endif

static double init_wtime; /* Wall clock time when helpers_startup called, set
                             and used only if ENABLE_TRACE is 3 */


/* DEBUG OUTPUT FOR TASKS.  Saved on a per-task basis in the array
   below, for printing when the task finishes.  The array is global,
   since it needs to be accessible from application procedures calling
   helpers_debug.  The helpers_task_number_internal procedure is
   similarly needed to provide global access to this_task from an
   application. */

#if ENABLE_DEBUG

char helpers_debug_output [256] [1024];
int helpers_task_number_internal (void) { return this_task; }

#endif


/* STATISTICS ON HELPERS AND MASTER.  The tasks_done field in the zeroth entry 
   pertains to the master.  These statistics are updated and read only in the 
   master thread.  Declared but not used if ENABLE_STATS is zero. */

static struct stats
{ int tasks_done;         /* Number of tasks done by this helper/master */
  int times_woken;        /* Number of times this helper was woken */
} stats[HELPERS_MAX+1];


/* FORWARD DECLARATIONS OF STATIC PROCEDURES. */

static void do_task_in_master (int);
static int runnable (mtix);


/* ------------------------  CONSISTENCY CHECK ------------------------------ */

/* CHECK CONSISTENCY OF DATA STRUCTURES.  Prints a message and calls abort if 
   an error is found.  Disabled unless HELPERS_DEBUG is defined.  Meant only
   for debugging - has seriously bad performance impact. */

static int done_setup = 0;   /* Set to 1 after data structures initialized */

#ifndef HELPERS_DEBUG

#define check_consistency() ((void) 0)

#else

static void check_consistency (void)
{
  int listed[MAX_TASKS+1];
  int in_use[MAX_TASKS+1];
  int in_queue[MAX_TASKS+1];
  mtix t;
  int j;

  if (!done_setup) 
  { return;
  }

  for (t = 0; t<=MAX_TASKS; t++)
  { listed[t] = in_use[t] = in_queue[t] = 0;
  }

  /* Check that all task ids occur once in 'used'. */

  for (j = 0; j<MAX_TASKS; j++)
  { t = used[j];
    listed[t] += 1;
  }

  for (t = 1; t<=MAX_TASKS; t++)
  { if (listed[t]!=1)
    { helpers_printf("INVALID!  id %d occurs %d times in used\n", t, listed[t]);
      abort();
    }
  }

  /* Check list of current tasks in 'used'. */

  for (j = 0; j<helpers_tasks; j++)
  { t = used[j];
    if (t<=0 || t>MAX_TASKS)
    { helpers_printf("INVALID!  used[%d] = %d out of range\n",j,t);
      abort();
    }
    if (in_use[t])
    { helpers_printf("INVALID!  used[%d] = %d already in use\n",j,t);
      abort();
    }
    in_use[t] = 1;
  }

  /* Check contents of 'untaken' queue.  Note that tasks may be being
     taken out concurrently by a helper.  Because of this, we can't
     check for a task being queued twice here because that can happen
     momentarily as a helper manipulates the queue.  Also, other checks 
     depend on reads from 'untaken' being atomic even though this
     isn't technically ensured by the code here and elsewhere (but not
     likely to be a problem in practice). */

  tix u_in, u_out;
  u_in = untaken_in;
  ATOMIC_READ_CHAR (u_out = untaken_out);

  if (u_in>MAX_TASKS || u_out>MAX_TASKS) 
  { helpers_printf("INVALID!  Bad untaken_in/out: %d/%d\n", u_in, u_out);
    abort();
  }

  for (j = u_out; j!=u_in; j = (j + 1) & QMask)
  { t = untaken[j];
    if (t<=0 || t>MAX_TASKS)
    { helpers_printf("INVALID!  untaken[%d] = %d out of range (%d %d)\n",
                      j, t, u_out, u_in);
      abort();
    }
    if (!in_use[t])
    { helpers_printf("INVALID!  untaken[%d] = %d not in use (%d %d)\n",
                      j, t, u_out, u_in);
      abort();
    }
    in_queue[t] = 1;
  }

  /* Check contents of 'master_only' queue. */

  if (master_only_in>MAX_TASKS || master_only_out>MAX_TASKS) 
  { helpers_printf("INVALID!  Bad master_only_in/out: %d/%d\n",
                   master_only_in, master_only_out);
    abort();
  }

  for (j = master_only_out; j!=master_only_in; j = (j + 1) & QMask)
  { t = master_only[j];
    if (t<=0 || t>MAX_TASKS)
    { helpers_printf("INVALID!  master_only[%d] = %d out of range\n",j,t);
      abort();
    }
    if (!in_use[t])
    { helpers_printf("INVALID!  master_only[%d] = %d not in use\n",j,t);
      abort();
    }
    if (in_queue[t])
    { helpers_printf("INVALID!  master_only[%d] = %d queued two places\n",j,t);
      abort();
    }
    in_queue[t] = 1;
  }

  /* Check contents of 'on_hold' queue. */

# ifndef HELPERS_NO_HOLDING

  if (on_hold_in>MAX_TASKS || on_hold_out>MAX_TASKS) 
  { helpers_printf("INVALID!  Bad on_hold_in/out: %d/%d\n",
                   on_hold_in, on_hold_out);
    abort();
  }

  int n_on_hold = 0;

  for (j = on_hold_out; j!=on_hold_in; j = (j + 1) & QMask)
  { t = on_hold[j];
    if (t<=0 || t>MAX_TASKS)
    { helpers_printf("INVALID!  on_hold[%d] = %d out of range\n",j,t);
      abort();
    }
    if (!in_use[t])
    { helpers_printf("INVALID!  on_hold[%d] = %d not in use\n",j,t);
      abort();
    }
    if (!task[t].info.is_on_hold)
    { helpers_printf("INVALID!  on_hold[%d] = %d has false is_on_hold\n",j,t);
      abort();
    }
    if (in_queue[t])
    { helpers_printf("INVALID!  on_hold[%d] = %d queued two places\n",j,t);
      abort();
    }
    in_queue[t] = 1;
    n_on_hold += 1;
  }

  /* Further check of consistency of is_on_hold flags. */

  int n_on_hold2 = 0;

  for (j = 0; j<helpers_tasks; j++)
  { t = used[j];
    if (task[t].info.is_on_hold)
    { n_on_hold2 += 1;
    }
  }

  if (n_on_hold!=n_on_hold2)
  { helpers_printf("INVALID!  inconsistent on_hold counts (%d %d)\n",
                   n_on_hold, n_on_hold2);
  }

# endif
}

#endif 


/* -------------------------  TRACE PROCEDURES  ----------------------------- */

/* PRINT LIST OF CURRENT TASKS.  Prints the task indexes.  Each index
   is followed by "*" if the task is flagged as needing to complete,
   "+" if the task is flagged as needing to start, and "-" if the task
   is not needed and not on hold, and "." if the task is on hold.
   This is followed by "F" if the task has finished, "X" if the task
   is executing, "R" if the task is not executing but is runnable and not
   on hold, and nothing otherwise.

   The 'used' array is also checked to make sure that it contains a
   permutation of the integers from 1 to MAX_TASKS.

   If ENABLE_TRACE is greater than 3, extensive additional information
   on each task is printed. */

static void trace_task_list (void)
{ 
  char b[MAX_TASKS+1];
  int i;

  /* Check that 'used' has the right stuff in it. */

  for (i = 1; i<=MAX_TASKS; i++) 
  { b[i] = 0;
  }
  for (i = 0; i<MAX_TASKS; i++)
  { if (used[i]<1 || used[i]>MAX_TASKS)
    { helpers_printf("The 'used' array contains a bad entry (%d)!\n",used[i]);
      break;
    }
    if (b[used[i]]!=0)
    { helpers_printf("The 'used' array has id %d more than once!\n",used[i]);
      break;
    }
    b[used[i]] = 1;
  }

  /* Print short task list. */
  
  for (i = 0; i<helpers_tasks; i++) 
  { 
    mtix t = used[i];
    struct task_info *info = &task[t].info;
    int hold = 0;

#   ifndef HELPERS_NO_HOLDING
    { int j;
      for (j = on_hold_out; j!=on_hold_in; j = (j + 1) & QMask)
      { if (on_hold[j]==t)
        { hold = 1;
          break;
        }
      }
      if (hold != info->is_on_hold) 
      { helpers_printf (
         "\nINCONSISTENCY! is_on_hold (%d) not consistent with on_hold queue\n",
         info->is_on_hold);
      }
    }
#   endif

    helpers_printf(" %d%s%s", (int) t, 
      info->needed>0 ? "*" : info->needed<0 ? "+" : !hold ? "-" : ".",
      info->done ? "F" : info->helper>=0 ? "X" : !hold&&runnable(t) ? "R" : "");
  }

  /* Print lots of stuff about tasks if enabled. */

  if (ENABLE_TRACE>3)
  { for (i = 0; i<helpers_tasks; i++) 
    { struct task_info *info = &task[used[i]].info;
      helpers_printf("\nused[%d], task[%d]:\n", i, (int) used[i]);
      helpers_printf("  amt_out: %"PRIuMAX"\n", (uintmax_t) info->amt_out);
      helpers_printf("  helper: %d\n",        (int) info->helper);
      helpers_printf("  done: %d\n",          (int) info->done);
      helpers_printf("  needed: %d\n",        (int) info->needed);
      helpers_printf("  pipe[]: %d %d %d\n",  (int) info->pipe[0],
                                              (int) info->pipe[1],
                                              (int) info->pipe[2]);
      helpers_printf("  flags: %x\n",         (int) info->flags);
      helpers_printf("  task_to_do: %s\n", helpers_task_name(info->task_to_do));
      helpers_printf("  op: %"PRIuMAX"\n",    (uintmax_t) info->op);
#     define var_marks(v) \
        (v==null || helpers_is_in_use(v) && helpers_is_being_computed(v) ? "" \
          : helpers_is_in_use(v) ? " c" : helpers_is_being_computed(v) ? " u" \
          : " uc")
      helpers_printf("  var[]: %s%s / %s%s / %s%s\n",   
                          var_name(info->var[0]), var_marks(info->var[0]),
                          var_name(info->var[1]), var_marks(info->var[1]),
                          var_name(info->var[2]), var_marks(info->var[2]));

#     ifndef HELPERS_NO_HOLDING
        helpers_printf("  is_on_hold: %d\n",  (int) info->is_on_hold);
#     endif

      helpers_printf("  out_used: %d\n",      (int) info->out_used);
      helpers_printf("  not_in_use_before[]: %d %d %d\n", 
                                              (int) info->not_in_use_before[0],
                                              (int) info->not_in_use_before[1],
                                              (int) info->not_in_use_before[2]);
      helpers_printf("  pipe_at_start[]: %d %d %d\n",
                                              (int) info->pipe_at_start[0],
                                              (int) info->pipe_at_start[1],
                                              (int) info->pipe_at_start[2]);
      helpers_printf("  first_amt[]: %d %d %d\n",
                                              (int) info->first_amt[0],
                                              (int) info->first_amt[1],
                                              (int) info->first_amt[2]);
      helpers_printf("  last_amt[]: %d %d %d\n",
                                              (int) info->last_amt[0],
                                              (int) info->last_amt[1],
                                              (int) info->last_amt[2]);
      helpers_printf("  start_wtime: %f\n",   (double) info->start_wtime);
      helpers_printf("  done_wtime: %f\n",    (double) info->done_wtime);
    }
  }
}


/* PRINT FLAGS FOR TASK. */

static void trace_flags (int flags)
{
  if (flags & HELPERS_MASTER_ONLY) helpers_printf(" MASTER_ONLY");
  if (flags & HELPERS_MASTER_NOW)  helpers_printf(" MASTER_NOW");

  if ((flags & HELPERS_MERGE_IN_OUT) == HELPERS_MERGE_IN_OUT) 
  { helpers_printf(" MERGE_IN_OUT");
  }
  else if ((flags & HELPERS_MERGE_IN))  helpers_printf(" MERGE_IN");
  else if ((flags & HELPERS_MERGE_OUT)) helpers_printf(" MERGE_OUT");

  if (flags & HELPERS_HOLD) helpers_printf(" HOLD");

  if ((flags & HELPERS_PIPE_IN012_OUT) != 0)
  { helpers_printf ((flags & HELPERS_PIPE_IN012) != 0 ? " PIPE_IN" : " PIPE");
    if (flags & HELPERS_PIPE_IN0) helpers_printf("0");
    if (flags & HELPERS_PIPE_IN1) helpers_printf("1");
    if (flags & HELPERS_PIPE_IN2) helpers_printf("2");
    if (flags & HELPERS_PIPE_OUT) helpers_printf("_OUT");
  }
}


/* TRACE OUTPUT FOR STARTING A TASK.  The task index is the first argument,
   with 0 indicating that the task will be done in the master without giving
   it a task index.  The remaining arguments are from helpers_do_task. */

static void trace_started 
  (tix t, int flags, helpers_task_proc *task_to_do, helpers_op_t op, 
   helpers_var_ptr out, helpers_var_ptr in1, helpers_var_ptr in2)
{ 
  if (t==0)
  { helpers_printf (
      "HELPERS: Task done directly in master: %s(%"PRIuMAX",%s,%s,%s)", 
      helpers_task_name(task_to_do), (uintmax_t) op, 
      var_name(out), var_name(in1), var_name(in2));
  }
  else
  { helpers_printf (
      "HELPERS: Task %d scheduled: %s(%"PRIuMAX",%s,%s,%s)", t,
      helpers_task_name(task_to_do), (uintmax_t) op, 
      var_name(out), var_name(in1), var_name(in2));
  }

  trace_flags(flags);

  if (ENABLE_TRACE>1)
  { struct task_info *info = &task[t].info;
    helpers_printf (" : %d %d %d", info->pipe[0], info->pipe[1], info->pipe[2]);
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");

  if (ENABLE_TRACE>1)
  { helpers_printf("HELPERS:   current tasks:");
    trace_task_list();
    helpers_printf("\n");
  }
}


/* TRACE OUTPUT FOR MERGING WITH A TASK.  The index of the task merged with
   is the first argument, followed by the arguments of helpers_do_task. */

#ifdef helpers_can_merge

static void trace_merged
  (tix t, int flags, helpers_task_proc *task_to_do, helpers_op_t op, 
   helpers_var_ptr out, helpers_var_ptr in1, helpers_var_ptr in2)
{ 
  struct task_info *info = &task[t].info;

  helpers_printf (
    "HELPERS: Task %d merged with new task %s(%"PRIuMAX",%s,%s,%s)", t,
    helpers_task_name(task_to_do), (uintmax_t) op, 
    var_name(out), var_name(in1), var_name(in2));

  trace_flags(flags);

  helpers_printf("\n");

  helpers_printf (
    "HELPERS:   merged task is %s(%"PRIuMAX",%s,%s,%s)",
    helpers_task_name(info->task_to_do), (uintmax_t) info->op, 
    var_name(info->var[0]), var_name(info->var[1]), var_name(info->var[2]));

  trace_flags(info->flags);

  if (ENABLE_TRACE>1)
  { helpers_printf (" : %d %d %d", info->pipe[0], info->pipe[1], info->pipe[2]);
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");

  if (ENABLE_TRACE>1)
  { helpers_printf("HELPERS:   current tasks:");
    trace_task_list();
    helpers_printf("\n");
  }
}

#endif


/* TRACE OUTPUT FOR BEGINNING EXECUTION OF A TASK IN THE MASTER. */

static void trace_begun_in_master (tix t)
{
  helpers_printf ("HELPERS: Task %d begun in master\n", t);
}


/* TRACE OUTPUT FOR COMPLETION OF A TASK. */

static void trace_completed (tix t)
{
  struct task_info *info = &task[t].info;

  if (t==0)
  { helpers_printf ("HELPERS: Task done directly completed");
  }
  else if (info->helper==0)
  { helpers_printf ("HELPERS: Task %d completed in master  ", t);
  }
  else
  { helpers_printf ("HELPERS: Task %d completed in helper %d", t, info->helper);
  }

  if (ENABLE_TRACE>1)
  { helpers_printf (" : %d %d %d : %"PRIuMAX" %"PRIuMAX" %"PRIuMAX
                    " : %"PRIuMAX" %"PRIuMAX" %"PRIuMAX" : %"PRIuMAX, 
      info->pipe_at_start[0], info->pipe_at_start[1], info->pipe_at_start[2],
      (uintmax_t) info->first_amt[0], 
      (uintmax_t) info->first_amt[1], 
      (uintmax_t) info->first_amt[2],
      (uintmax_t) info->last_amt[0], 
      (uintmax_t) info->last_amt[1], 
      (uintmax_t) info->last_amt[2],
      (uintmax_t) info->amt_out);
    if (ENABLE_TRACE>2)
    { if (t==0)
      { helpers_printf (" @ %.3f>%.3f", info->start_wtime - init_wtime,
                                        info->done_wtime - init_wtime);
      }
      else
      { helpers_printf (" @ %.3f>%.3f>%.3f", info->start_wtime - init_wtime,
                                             info->done_wtime - init_wtime,
                                             WTIME() - init_wtime);
      }
    }
  }

  helpers_printf("\n");
}


/* TRACE OUTPUT FOR STARTING COMPUTATION OF A VARIABLE. */

static void trace_start_computing_var (int f, helpers_var_ptr v)
{
  switch (f)
  { case 0: 
      helpers_printf ("HELPERS: Computation of %s has already finished\n",
                       var_name(v));
      break;
    case 1: 
      helpers_printf ("HELPERS: Computation of %s has already started\n",
                       var_name(v));
      break;
    case 2: 
      helpers_printf ("HELPERS: Computation of %s is starting in a helper\n",
                       var_name(v));
      break;
    case 3: 
      helpers_printf ("HELPERS: Computation of %s is being started\n",
                       var_name(v));
      break;
  }
}


/* TRACE OUTPUT FOR WAITING FOR COMPUTATIONS TO FINISH. */

static void trace_wait_until_not_being_computed
  (int any, helpers_var_ptr v1, helpers_var_ptr v2)
{ 
  helpers_printf (any ? "HELPERS: Wait until " : "HELPERS: No wait until ");

  if (v1==null && v2==null)
  { helpers_printf ("no vars not being computed");
  }
  else if (v1!=null && v2!=null)
  { helpers_printf ("%s and %s not being computed", var_name(v1), var_name(v2));
  }
  else
  { helpers_printf ("%s not being computed", var_name (v1==null ? v2 : v1));
  }

  if (ENABLE_TRACE>1)
  { helpers_printf(" :");
    trace_task_list();
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");
}


/* TRACE OUTPUT FOR WAITING FOR VARIABLE TO NOT BE IN USE. */

static void trace_wait_until_not_in_use (int any, helpers_var_ptr v)
{ 
  if (!any)
  { helpers_printf ("HELPERS: No wait until %s not in use\n", var_name(v));
    return;
  }

  helpers_printf ("HELPERS: Waiting until %s not in use", var_name(v));

  if (ENABLE_TRACE>1)
  { helpers_printf(" :");
    trace_task_list();
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");
}


/* TRACE OUTPUT FOR RELEASING ALL TASKS THAT ARE ON HOLD. */

#ifndef HELPERS_NO_HOLDING

static void trace_release_holds (void)
{ 
  if (on_hold_in==on_hold_out)
  { helpers_printf ("HELPERS: No tasks on hold to release\n");
    return;
  }

  helpers_printf ("HELPERS: Releasing tasks on hold\n");
}

#endif


/* TRACE OUTPUT FOR WAITING FOR ALL MASTER-ONLY TASKS TO COMPLETE. */

static void trace_wait_for_all_master_only (void)
{ 
  if (master_only_in==master_only_out)
  { helpers_printf ("HELPERS: No wait for all master-only tasks to complete\n");
    return;
  }

  helpers_printf ("HELPERS: Waiting for all master-only tasks to complete");

  if (ENABLE_TRACE>1)
  { helpers_printf(" :");
    trace_task_list();
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");
}


/* TRACE OUTPUT FOR WAITING FOR ALL TASKS TO COMPLETE. */

static void trace_wait_for_all (void)
{ 
  if (helpers_tasks==0)
  { helpers_printf ("HELPERS: No wait for all tasks to complete\n");
    return;
  }

  helpers_printf ("HELPERS: Waiting for all tasks to complete");

  if (ENABLE_TRACE>1)
  { helpers_printf(" :");
    trace_task_list();
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }

  helpers_printf("\n");
}


/* TRACE OUTPUT FOR NO LONGER WAITING. */

static void trace_done_waiting (void)
{
  helpers_printf ("HELPERS: Done waiting");
  if (ENABLE_TRACE>1)
  { helpers_printf(" :");
    trace_task_list();
    if (ENABLE_TRACE>2)
    { helpers_printf (" @ %.3f", WTIME()-init_wtime);
    }
  }
  helpers_printf("\n");
}


/* --------------------------  UTILITY PROCEDURES  -------------------------- */

/* RUN THE TASK JUST TAKEN.  When called, the new task should be in this_task,
   with its info in this_task_info.  A flush operation (explicit or implicit)
   should be done before calling this procedure. */

static void run_this_task (void)
{
  if (ENABLE_TRACE>1) 
  { this_task_info->pipe_at_start[0] = this_task_info->pipe[0];
    this_task_info->pipe_at_start[1] = this_task_info->pipe[1];
    this_task_info->pipe_at_start[2] = this_task_info->pipe[2];
    this_task_info->first_amt[0] = 0;
    this_task_info->first_amt[1] = 0;
    this_task_info->first_amt[2] = 0;
    this_task_info->last_amt[0] = 0;
    this_task_info->last_amt[1] = 0;
    this_task_info->last_amt[2] = 0;
    if (ENABLE_TRACE>2)
    { this_task_info->start_wtime = WTIME();
    }
  }

  this_task_info->task_to_do (this_task_info->op, this_task_info->var[0], 
                              this_task_info->var[1], this_task_info->var[2]);

  if (ENABLE_TRACE>2) 
  { this_task_info->done_wtime = WTIME();
  }

  FLUSH;  /* ensure data and info is up-to-date when 'done' flag is set */

  ATOMIC_WRITE_CHAR (this_task_info->done = 1);
  FLUSH;
}


/* CHECK WHETHER A TASK IS RUNNABLE.  A task cannot be run if any of its
   input/output variables are being computed by another task that hasn't
   finished, and the other task has either not started, or it doesn't 
   produce pipelined output, or this task doesn't handle pipelined input. 
   This function returns 0 if the task is not runnable, -1 if it is runnable
   with pipelining, and +1 if it is runnable without pipelining. 

   Careful flushing is needed here to handle the possibility that a task
   producing one of the inputs finishes around this time and a new task 
   with the same task id quickly starts up. */

static int runnable (mtix t)
{
  struct task_info *info = &task[t].info;
  int f, r;
  char d;
  tix p;
  hix h;

  r = +1;

  FLUSH;  /* make sure we see recent values of 'pipe', 'done', and 'helper' */

  /* Look at availability of input from the 'out' varable. */

  ATOMIC_READ_CHAR (p = info->pipe[0]);
  if (p!=0)
  { ATOMIC_READ_CHAR (d = task[p].info.done);
    if (!d)
    { ATOMIC_READ_CHAR (h = task[p].info.helper);
      f = task[p].info.flags;
      FLUSH;
      ATOMIC_READ_CHAR (p = info->pipe[0]);
      if (p!=0) /* could be 0 if task finished since check above */
      { if (h<0 || !(f & HELPERS_PIPE_OUT) || !(info->flags & HELPERS_PIPE_IN0))
        { return 0;
        }
        r = -1;
      }
    }
  }

  /* Look at availability of input from the 'in1' varable. */

  ATOMIC_READ_CHAR (p = info->pipe[1]);
  if (p!=0)
  { ATOMIC_READ_CHAR (d = task[p].info.done);
    if (!d)
    { ATOMIC_READ_CHAR (h = task[p].info.helper);
      f = task[p].info.flags;
      FLUSH;
      ATOMIC_READ_CHAR (p = info->pipe[1]);
      if (p!=0) /* could be 0 if task finished since check above */
      { if (h<0 || !(f & HELPERS_PIPE_OUT) || !(info->flags & HELPERS_PIPE_IN1))
        { return 0;
        }
        r = -1;
      }
    }
  }

  /* Look at availability of input from the 'in2' varable. */

  ATOMIC_READ_CHAR (p = info->pipe[2]);
  if (p!=0)
  { ATOMIC_READ_CHAR (d = task[p].info.done);
    if (!d)
    { ATOMIC_READ_CHAR (h = task[p].info.helper);
      f = task[p].info.flags;
      FLUSH;
      ATOMIC_READ_CHAR (p = info->pipe[2]);
      if (p!=0) /* could be 0 if task finished since check above */
      { if (h<0 || !(f & HELPERS_PIPE_OUT) || !(info->flags & HELPERS_PIPE_IN2))
        { return 0;
        }
        r = -1;
      }
    }
  }

  return r;
}


/* FIND AN UNTAKEN TASK THAT IS RUNNABLE, AND REMOVE IT FROM QUEUE.  If its
   argument is zero, it returns 0 if no untaken task is runnable (master-only 
   tasks not considered), and otherwise the index of a runnable task from the 
   untaken queue, which it removes from this queue (moving untaken_out).  If 
   more than one untaken task is runnable, a task whose start or completion is 
   needed now by the master is preferred, and secondarily, a task whose inputs 
   are fully computed is preferred to one that would use one or more pipelined 
   inputs that are not fully computed, with any other tie broken arbitrarily.

   If the argument is 1, only a "needed" task will be found, with 0 being
   returned if no needed task is runnable.

   This procedure should be called only when start_lock has been set, so that
   another thread will not be manipulating the queue simultaneously.  When
   this procedure is called from a helper, however, it is possible that the
   master will simultaneously be adding an entry at the untaken_in end. 
   The value of untaken_in is therefore fetched once, followed by a flush
   to ensure that data accessed in the queue before that is up-to-date. */

static mtix find_untaken_runnable (int only_needed)
{
  int i, f, p, r, n;
  tix u_in, new_u_out;
  mtix t;

  /* We assume a flush has been done recently (explicitly, or implicity from
     setting start_lock), so the value seen for untaken_in is recent.  We flush
     after to ensure that the data in the queue up to u_in is up-to-date. */

  ATOMIC_READ_CHAR (u_in = untaken_in);
  FLUSH;

  /* Look for a runnable task in the untaken queue, setting f to its index
     in this queue. */

  f = -1;  /* nothing runnable found yet */

  p = only_needed ? 1 : -1;  /* if p is 1, ignore non-needed tasks
                                if p is 0, ignore non-needed pipelined tasks
                                if p is -1, any runnable task might be used */

  for (i = untaken_out; i!=u_in; i = (i + 1) & QMask) 
  { t = untaken[i];
#   ifdef HELPERS_DEBUG
      if (t<1 || t>MAX_TASKS) abort();
#   endif
    ATOMIC_READ_CHAR (n = task[t].info.needed);
    if (n)
    { r = runnable(t);
      if (r!=0)
      { f = i;
        if (r>0)    /* needed now and runnable without pipelining, so we can */
        { break;    /*   stop the search now, since this is highest priority */
	}
        else        /* needed now and runnable with pipelining */
        { p = 1;
        }
      }
    }
    else if (p<=0)
    { r = runnable(t);
      if (r>0)
      { f = i;      /* not needed now and runnable without pipelining */
        p = 0;
      }
      else if (r<0 && p<0)
      { f = i;      /* not needed now and runnable with pipelining */
      }
    }
  }

  /* Return 0 if no runnable task found, or none needed, if only_needed is 1. */

  if (f < 0) return 0;

  /* Otherwise, take the highest priority runnable task. */

  t = untaken[f];

  /* Remove the task taken from the untaken queue.  The index at untaken_out
     is copied to where the task being removed is located, at which point
     untaken_out can be moved forward. */

  untaken[f] = untaken[untaken_out];
  new_u_out = (untaken_out + 1) & QMask;
  FLUSH;  /* so new value of untaken[f] is seen; maybe unneeded given lock */
  ATOMIC_WRITE_CHAR (untaken_out = new_u_out);

  return t;
}


/* PUT A TASK IN UNTAKEN QUEUE (AND OUT OF ON_HOLD), MAYBE WAKING HELPER.
   Must be called only from the master.  Stores the task id in the
   untaken queue at untaken_in, and advances untaken_in, setting the
   untaken lock when incrementing it. Then unsuspends a helper if it
   has suspended while the lock is set.  (But don't unsuspend a helper
   if multithreading is currently disabled.) Also removes the task
   from the on_hold queue if it is on hold. */

static void put_in_untaken (mtix t)
{
  check_consistency();  /* only enabled for debugging */

# ifdef HELPERS_DEBUG
    if (t<1 || t>MAX_TASKS) abort();
# endif

  untaken[untaken_in] = t;

# ifdef HELPERS_NO_MULTITHREADING

  untaken_in = (untaken_in + 1) & QMask;

# else

  if (helpers_not_multithreading_now)
  { 
    tix new_u_in;
    FLUSH;
    new_u_in = (untaken_in + 1) & QMask;
    ATOMIC_WRITE_CHAR (untaken_in = new_u_in);
  }
  else
  { 
    tix new_u_in;
    hix h;

    omp_set_lock (&untaken_lock.lock);    /* does implicit FLUSH */
    h = suspended;

    new_u_in = (untaken_in + 1) & QMask;
    ATOMIC_WRITE_CHAR (untaken_in = new_u_in);

    omp_unset_lock (&untaken_lock.lock);  /* does implicit FLUSH */

    /* Wake the suspended helper, if there is one. */

    if (h!=0)
    { omp_set_lock (&suspend_lock[1-which_wakes].lock);
      suspended = 0;
      omp_unset_lock (&suspend_lock[which_wakes].lock);
      which_wakes = 1-which_wakes;
      if (ENABLE_STATS) stats[h].times_woken += 1;
    }
  }

# endif

  /* Remove the task from the on_hold queue if it's there. */

# ifndef HELPERS_NO_HOLDING
  { struct task_info *info = &task[t].info;
    if (info->is_on_hold)
    { int j;
      for (j = on_hold_out; on_hold[j]!=t; j = (j + 1) & QMask) ;
      on_hold[j] = on_hold[on_hold_out];
      on_hold_out = (on_hold_out + 1) & QMask;
      info->is_on_hold = 0;
    }
  }
# endif

  check_consistency();  /* only enabled for debugging */
}


/* MARK INPUTS AS NOT IN USE, IF NOT USED BY AN UNCOMPLETED TASK.  Looks at 
   whether the input variables of a task are used by any task that does not
   have 'done' set (excluding the task given as the argument).  Note that this
   may result in a variable being marked as not in use when a task that used
   it has finished but not yet been noticed to have finished by the master 
   thread, but this should be OK.

   The 'i' argument is the index in 'used' of the task whose input variables
   are to (maybe) be unmarked. */

#ifdef helpers_mark_not_in_use

static inline void maybe_mark_not_in_use (int i)
{ 
  struct task_info *info = &task[used[i]].info;
  int w, j;

  for (w = 1; w<=2; w++)
  { helpers_var_ptr v = info->var[w];
    if (v!=null && v!=info->var[0])
    { for (j = (info->not_in_use_before[w] ? i+1 : 0); j<helpers_tasks; j++)
      { if (j==i) continue;
        struct task_info *einfo = &task[used[j]].info;
        if (einfo->var[0]!=v && (einfo->var[1]==v || einfo->var[2]==v))
        { char d;
          ATOMIC_READ_CHAR (d = einfo->done);
          if (!d) goto next;
        }
      }
      helpers_mark_not_in_use(v);
    }
  next: ;
  }
}

#endif


/* NOTICE COMPLETED TASKS.  Called only from the master thread.  Note that 
   it starts and ends with FLUSH operations.  The notice_completed_proc
   procedure is called only via the notice_completed macro. */

#define notice_completed() \
  do { if (helpers_tasks>0) notice_completed_proc(); } while (0)

static void notice_completed_proc (void)
{
  int i, j, k;
  char d;

  /* Flush so that 'done' flags will be visible. */

  FLUSH;

  /* Find first task that has completed, if there is any. */

  i = 0;
  for (;;)
  { ATOMIC_READ_CHAR (d = task[used[i]].info.done);
    if (d) 
    { break;
    }
    i += 1;
    if (i==helpers_tasks)
    { return;
    }
  }

  /* Loop through tasks in 'used', processing completion of any now done. 
     Note that if more than one task has finished, they are processed
     in the order in which they were scheduled. */

  k = i;
  for ( ; i<helpers_tasks; i++)
  { 
    mtix t = used[i];
    struct task_info *info = &task[t].info;

    ATOMIC_READ_CHAR (d = info->done);

    if (!d) /* Swap so tasks not finished end up all at the bottom */
    { 
      used[i] = used[k];
      used[k] = t;
      k += 1;
    }
    else /* Process completion of a task that has now finished. */
    {
      /* Update 'pipe' fields for tasks that were taking input from this one,
         if there may be any.  We can stop after seeing a task scheduled
         after this one that has the same output variable, since tasks after
         that taking the variable as an input will reference the later task. 

         Also, we find out here whether the output variable is still being 
         computed.  We rely on a task that takes piped input for its 'out' 
         variable not finishing before the earlier task has fully computed 
         this variable, so only later tasks might still be computing it. 
         Note, however, that we can't rely on the 'done' flags being set in
         the sequence that the tasks completed their computations. */

      helpers_var_ptr out = info->var[0];
      int still_being_computed = 0;

      if (info->out_used)
      { for (j = i+1; j<helpers_tasks; j++)
        { struct task_info *ninfo = &task[used[j]].info;
          if (ninfo->pipe[2]==t) 
          { ATOMIC_WRITE_CHAR (ninfo->pipe[2] = 0);
          }
          if (ninfo->pipe[1]==t) 
          { ATOMIC_WRITE_CHAR (ninfo->pipe[1] = 0);
          }
          if (ninfo->pipe[0]==t) 
          { ATOMIC_WRITE_CHAR (ninfo->pipe[0] = 0);
            still_being_computed = 1;
            break;
          }
          if (ninfo->var[0]==out) /* possible even if ninfo->pipe[0]!=t */
          { still_being_computed = 1;
          }
        }
      }
  
      /* Increment count of tasks done by helper/master that did this task. */
  
      if (ENABLE_STATS) stats[info->helper].tasks_done += 1;
  
      /* Write trace output showing task completion, if trace enabled. 
         Also write debug output, if any was produced. */
  
      if (trace) trace_completed(t);

#     if ENABLE_DEBUG
      { if (helpers_debug_output[t][0] != 0)
        { helpers_printf ("HELPERS: Task %d debug output: %s\n", t,
                           helpers_debug_output[t]);
          helpers_debug_output[t][0] = 0;  /* just in case - set when started */
        }
      }
#     endif
  
      /* Unset the being-computed flag as appropriate, if the application 
         defined the required macro. */
  
#     ifdef helpers_mark_not_being_computed
      { if (!still_being_computed && out!=null)
        { helpers_mark_not_being_computed(out);
        }
      }
#     endif
  
      /* Unset the in-use flags as appropriate, if the application defined the 
         required macro. */

#     ifdef helpers_mark_not_in_use
        maybe_mark_not_in_use (i);
#     endif
    }
  }

  /* Update number of tasks. */

  helpers_tasks = k;

  /* Flush so updated 'pipe' fields will be visible to helpers, and so that
     data written by tasks that have completed will be visible to master. */

  FLUSH;

  /* Output new task list, if that's enabled. */

  if (ENABLE_TRACE>1 && trace)
  { helpers_printf("HELPERS:   current tasks:");
    trace_task_list();
    helpers_printf("\n");
  }
}


/* MARK AS NEEDED A TASK AND THE TASKS IT TAKES INPUT FROM.  The 'needed' 
   argument should be -1 (needs to start) or +1 (needs to finish).  Tasks 
   should be marked from most recent to oldest, so that a task marked with 
   -1 because it provides input will not have previously been marked with +1
   because it's output is directly needed.

   It's enough to mark a task that provides input as needing to start, since 
   if pipelining can't be done, it will anyway have to finish before the task 
   taking that input can start. 

   If a task marked as providing input is on hold, it is moved to the
   untaken queue.  But if the task being marked as needed is on hold,
   the caller must handled moving it to untaken. */

static void mark_as_needed (struct task_info *info, int needed)
{
  struct task_info *uinfo;
  int p;

  if (info->needed <= 0 && info->needed != needed) 
  { ATOMIC_WRITE_CHAR (info->needed = needed);
  }

  p = info->pipe[0];
  if (p != 0) 
  { uinfo = &task[p].info;
    if (uinfo->needed == 0) ATOMIC_WRITE_CHAR (uinfo->needed = -1);
#   ifndef HELPERS_NO_HOLDING
      if (uinfo->is_on_hold) put_in_untaken(p);
#   endif
  }

  p = info->pipe[1];
  if (p != 0) 
  { uinfo = &task[p].info;
    if (uinfo->needed == 0) ATOMIC_WRITE_CHAR (uinfo->needed = -1);
#   ifndef HELPERS_NO_HOLDING
      if (uinfo->is_on_hold) put_in_untaken(p);
#   endif
  }

  p = info->pipe[2];
  if (p != 0) 
  { uinfo = &task[p].info;
    if (uinfo->needed == 0) ATOMIC_WRITE_CHAR (uinfo->needed = -1);
#   ifndef HELPERS_NO_HOLDING
      if (uinfo->is_on_hold) put_in_untaken(p);
#   endif
  }
}


/* WAIT WHILE ANY TASKS NEED TO START OR FINISH.  Looks at the "needed" flags
   for tasks, where -1 means needs to start, +1 means needs to finish, and 0
   means not needed.  While it waits, it tries to do needed tasks in the master,
   and notices completed tasks.  

   Note that if a task needs to finish, a FLUSH will have been done (in 
   notice_completed) after that task finishes, but if a task needs only to
   start no FLUSH may have been done after it starts when wait_while_any_needed
   returns.  However, the lack of a FLUSH in this circumstance should be 
   innocuous, since data will be accessed only after amt_out is looked at 
   after a later FLUSH. */

static void wait_while_any_needed (void)
{
  loop:
  { int i; hix h;

    do_task_in_master(1);
    notice_completed();  /* does a FLUSH */

    for (i = 0; i<helpers_tasks; i++)
    { struct task_info *info = &task[used[i]].info;
      if (info->needed != 0)
      { if (info->needed > 0) goto loop;  /* needs to finish, but hasn't */
        ATOMIC_READ_CHAR (h = info->helper);
        if (h < 0) goto loop;  /* needs to start, but hasn't */
      }
    }
  }
}


/* -----------------  TASK EXECUTION IN THE MASTER OR A HELPER -------------- */

/* MAYBE DO A NEEDED TASK IN THE MASTER.  Does the next master-only task if it
   is runnable (and is needed, if only_needed is 1), and otherwise tries to do 
   a task from the untaken queue (provided it is needed, if only_needed is 1).
   Returns without waiting if no task is suitable, or if it can't set 
   start_lock (so a helper must currently be looking for a task to run). */

static void do_task_in_master (int only_needed)
{
  tix u_out;

  this_task = 0;

  /* Do the next master-only task if there is one (and perhaps needed), and it
     is runnable.  If so, remove the task to be done from the master_only queue.
     Note that this doesn't require any queue locking, since the master_only
     queue is accessed only in the master thread. */

  if (master_only_in!=master_only_out)
  { 
    mtix m = master_only[master_only_out];

    if ((!only_needed || task[m].info.needed) && runnable(m))
    { master_only_out = (master_only_out + 1) & QMask;
      this_task = m;
    }
  }

  /* Otherwise, look for a (perhaps needed) task from the untaken queue, but 
     only if no helper is trying to (and hence no other thread has set 
     start_lock).  Return if no needed and runnable task can be found this 
     way either. */

  if (this_task==0)
  { 
    hix h;
 
#   ifndef HELPERS_NO_MULTITHREADING

    if (!helpers_not_multithreading_now)
    {
      ATOMIC_READ_CHAR (h = suspended);

      FLUSH;  /* ensures that h>0 below after seeing untaken_in!=untaken_out
                 does really mean the queue is non-empty (but shouldn't be) */
    }

#   endif

    ATOMIC_READ_CHAR (u_out = untaken_out);
    if (untaken_in==u_out)
    { return; 
    }

#   ifndef HELPERS_NO_MULTITHREADING

    if (!helpers_not_multithreading_now)
    { 
      if (!omp_test_lock (&start_lock.lock))
      { 
        /* See if a helper is supended - it shouldn't be! - and wake it up
           if it is. */

        if (h>0)
        { omp_set_lock (&suspend_lock[1-which_wakes].lock);
          suspended = 0;
          omp_unset_lock (&suspend_lock[which_wakes].lock);
          which_wakes = 1-which_wakes;
          if (ENABLE_STATS) stats[h].times_woken += 1;
          helpers_printf("HELPER WAS SUSPENDED WHEN IT SHOULDN'T HAVE BEEN!\n");
        }

        return;
      }
    }

#   endif

    this_task = find_untaken_runnable(only_needed);

#   ifndef HELPERS_NO_MULTITHREADING

    if (!helpers_not_multithreading_now)
    { omp_unset_lock (&start_lock.lock);
    }

#   endif

    if (this_task==0)
    { return;
    }
  }

  /* Do the task in the master. */

  if (trace) trace_begun_in_master (this_task);

  this_task_info = &task[this_task].info;
  this_task_info->helper = 0;
  FLUSH;

  run_this_task();

  /* Set this_task to indicate that nothing is being done in the master,
     so that directly-called task procedures will work correctly. */

  this_task = 0;
  this_task_info = &task[this_task].info;
}


/* PROCEDURE EXECUTED IN HELPER THREADS.  Loops looking for something for 
   this helper to do. */

#ifndef HELPERS_NO_MULTITHREADING

static void helper_proc (void)
{
  /* Set lock for becoming the helper that looks for a task to start. */

  omp_set_lock (&start_lock.lock);

  /* Loop, each time waiting for a non-empty untaken queue, looking for a 
     runnable task to start, doing it, and flagging its completion. */

  for (;;)
  {
    int count;
    tix u_in;

    /* Loop up to SPIN_EMPTY times, or until there is an untaken task
       (and multithreading hasn't been disabled). */

    for (count = 0; count<SPIN_EMPTY; count++)
    { ATOMIC_READ_CHAR (u_in = untaken_in);
      if (u_in!=untaken_out && !helpers_not_multithreading_now)
      { break;
      }
      FLUSH;
    }

    if (count==SPIN_EMPTY)
    {
      int will_suspend = 0;

      /* Decide whether to suspend by looking whether the queue is empty
         with the lock set, so the master won't be putting a task in at 
         the same time. */

      omp_set_lock (&untaken_lock.lock);

      ATOMIC_READ_CHAR (u_in = untaken_in);
      if (u_in==untaken_out || helpers_not_multithreading_now)
      { ATOMIC_WRITE_CHAR (suspended = this_thread);
        will_suspend = 1;
      }

      omp_unset_lock (&untaken_lock.lock);

      /* If we decided to suspend, do that now. */

      if (will_suspend) /* look at will_suspend, not suspended (could change) */
      {
        /* Suspend by setting the lock that will have been set by the master 
           (though if it has already noticed the suspended flag, and decided 
           to unsuspend, it may have already unset it). 

           The master will unsuspend this suspended helper when it next puts
           a task in the untaken queue (when multithreading hasn't been 
           disabled). */

        omp_set_lock (&suspend_lock[which_suspends].lock);
        omp_unset_lock (&suspend_lock[which_suspends].lock);
        which_suspends = 1-which_suspends;

        /* Go back to the start of the main loop, looking again at whether
           the queue is empty. */

        continue;
      }
    }

    /* Wait until there is a task to do.  Note that start_lock will have 
       been set, and the untaken queue must contain at least one task, which
       must eventually become runnable (though perhaps only after the master
       thread does some required master-only tasks). */

    do { this_task = find_untaken_runnable(0); } while (this_task==0);

    /* Do the task in this helper, unsetting start_lock first, then setting
       it again after. */

    this_task_info = &task[this_task].info;
    ATOMIC_WRITE_CHAR (this_task_info->helper = this_thread);

    omp_unset_lock (&start_lock.lock);  /* implies a flush */

    run_this_task();

    omp_set_lock (&start_lock.lock);  /* implies a flush */
  }
}

#endif


/* -------------------------  TASK SCHEDULING  ------------------------------ */

/* DO A TASK OR SCHEDULE IT TO BE DONE. */

void helpers_do_task 
  (int flags0, helpers_task_proc *task_to_do, helpers_op_t op, 
   helpers_var_ptr out, helpers_var_ptr in1, helpers_var_ptr in2)
{
  int pipe0; /* must be signed, since -1 is used to indicate not found yet */
  struct task_info *info;
  int flags;
  mtix *uh;
  mtix t;
  int i;
  hix h;

  check_consistency();  /* only enabled for debugging */

  /* If helpers are disabled, do the task directly.  There's no possible need 
     to wait.  Note that task[0].info will be set to all zeros (either from 
     initialization or clearing when helpers were disabled). */

  if (helpers_are_disabled)
  { info = &task[0].info;
    goto direct;
  }

  /* Set flags to flags0 with disabled features removed.  Also get rid
     of HELPERS_HOLD for master-only and master-now tasks. */

  flags = flags0 & flag_mask;

# ifndef HELPERS_NO_HOLDING
    if (flags & HELPERS_HOLD)
    { if (flags & (HELPERS_MASTER_ONLY | HELPERS_MASTER_NOW))
      { flags &= ~ HELPERS_HOLD;
      }
    }
# endif

  /* Notice tasks that have now completed.  Note that this does a flush
     (unless there are no tasks). */

  notice_completed();

  /* If merge possible, find the most-recently-scheduled task (if any) that 
     outputs the output variable of this new task, setting pipe0 to the task 
     index, or zero.  If one is found, "uh" is left pointing to its position 
     in "used" (needed below when merging produces a master-now task).  Set
     pipe0 to -1 if we can see no merge is possible; then we look later. */

  pipe0 = -1;

# ifdef helpers_can_merge

    if ((flags & HELPERS_MERGE_IN) && helpers_tasks>0 
          && out!=null && helpers_is_being_computed(out))
    { uh = &used[helpers_tasks];
      pipe0 = 0;
      do
      { if (task[*--uh].info.var[0]==out)
        { pipe0 = *uh;
          break;
        }
      } while (uh>used);
    }

    if (pipe0<=0)
    { goto no_merge;
    }

    /* Perhaps try to merge the new task with the task, indexed by pipe0, 
       that pipes into its output variable.  If a merge can be done, we may
       need to move the new task to the master-only queue if the new task is 
       master-only, or do it now, if the new task is master-now, or we may 
       just return, if the merged task can stay in the same queue as the 
       old one. */

    struct task_info *m = &task[pipe0].info;

    /* Can't merge unless flags for both tasks allow merge.  Note that we have
       (flags & HELPERS_MERGE_IN), since otherwise pipe0 would be -1. */

    if (! (m->flags & HELPERS_MERGE_OUT))
    { goto no_merge;
    }

    /* Can't merge if both tasks must be done in master, and either some
       other master-only task(s) should be done in between, or the new 
       task is master-now and the old task is not the only master-only task. */

    if ((m->flags & HELPERS_MASTER_ONLY) 
          && (flags & (HELPERS_MASTER_ONLY | HELPERS_MASTER_NOW)))
    { if (master_only [(master_only_in + (QSize-1)) & QMask] != pipe0
       || (flags & HELPERS_MASTER_NOW) && master_only[master_only_out] != pipe0)
      { goto no_merge;
      }
    }

    /* Can't merge if application-provided function says we can't. */

    if (! helpers_can_merge (out, task_to_do, op, in1, in2, 
                             m->task_to_do, m->op, m->in1, m->in2))
    { goto no_merge;
    }

    /* Start trying to merge (may still fail later). */

    helpers_var_ptr old_var[3];

    /* Set "merge" to 1 if we can merge with "m" (it hasn't started running).
       Set "locked" to 1 if start_lock needs to be unset later. */

    int merge = 1, locked = 0;

#   ifndef HELPERS_NO_MULTITHREADING
    { if (!helpers_not_multithreading_now)
      { 
        /* If task we're trying to merge with is not master-only or on hold,
           set the start lock, in order to check for sure whether the merged
           task has already been started in a helper.  This also allows the
           untaken queue may be manipulated later, until the lock is unset. */

        if (! (m->flags & HELPERS_MASTER_ONLY)
#          ifndef HELPERS_NO_HOLDING
             && !m->is_on_hold
#          endif
           )
        { 
#         ifndef HELPERS_NO_HOLDING
          if (!m->is_on_hold)
#         endif
          { FLUSH;
            ATOMIC_READ_CHAR (h = m->helper);

            /* Don't merge if the task to merge with has started to run. */

            if (h!=-1)
            { goto no_merge;
            }
          }

          /* This lock will usually be unset, but it may be set for a
             prolonged time if here are untaken tasks and they can't
             be run until some master-only tasks have run. */

          while (!omp_test_lock (&start_lock.lock))
          { ATOMIC_READ_CHAR (h = m->helper);
            if (h!=-1)
            { goto no_merge;
            }
            do_task_in_master(0);
          }
          locked = 1;

#         ifndef HELPERS_NO_HOLDING
          if (!m->is_on_hold)
#         endif
          { ATOMIC_READ_CHAR (h = m->helper);
            if (h!=-1) 
            { merge = 0;
            }
          }
        }
      }
    }
#   endif

    if (merge)
    { 
      double *task_data_loc;

      /* If the merged task will be master-now, unmark variables.  Also
         copy the task_data into pseudo-task entry 0, since that's where
         the task running as master-now will see it.

         For non-master-now tasks, remember what the input variables were 
         before the merge, to help in possible later unmarking.  Also,
         sets the pointer to task_data. */

      if (flags & HELPERS_MASTER_NOW)
      { 
        int i;

        /* Copy task_data to where it will be seen by master-now task. */

        task_data_loc = task[0].info.task_data;
        for (i = 0; i<HELPERS_TASK_DATA_AMT; i++)
        { task_data_loc[i] = m->task_data[i];
        }

        /* Mark the output as not being computed, since it won't be after
           this task is done (immediately) in the master thread. */

#       ifdef helpers_mark_not_being_computed
          helpers_mark_not_being_computed (out);
#       endif

        /* Mark inputs of task being merged into as not being used, if
           not in use by another task. */

#       ifdef helpers_mark_not_in_use
          maybe_mark_not_in_use (uh-used);
#       endif
      }

      else /* not master-now */
      {
        task_data_loc = m->task_data;

        old_var[1] = m->var[1];
        old_var[2] = m->var[2];
      }

      /* Merge the new task with the existing task (which is indexed
         by 'pipe0' and has info at 'm').  Sets up the flags for the
         merged task.  Removes the old task from the on_hold queue
         if it was there but the merged task will not be. */

      int flags_to_clear = helpers_merge (out, task_to_do, op, in1, in2, 
                             &m->task_to_do, &m->op, &m->var[1], &m->var[2],
                             task_data_loc);

      m->flags &= ~ (HELPERS_MERGE_IN_OUT | HELPERS_PIPE_OUT);
      m->flags |= flags & (HELPERS_MERGE_OUT | HELPERS_PIPE_OUT);
      m->flags &= ~ (flags_to_clear & (HELPERS_MERGE_OUT | HELPERS_HOLD 
                                        | HELPERS_PIPE_IN012_OUT));

      /* Remove and/or add merged task from/to queues.  Nothing needs to
         be done if the task merged into is master-only and the new task
         isn't master-now, or if the task merged into is not master-only
         and the new task is neither master-only nor master-now.  When
         the new task is master-now, the task merged into is removed from
         its queue, but not added to a queue, since it will be done 
         immediately. */

      if (m->flags & HELPERS_MASTER_ONLY)
      { if (flags & HELPERS_MASTER_NOW)
        { 
          /* Remove the task to merge into from the master-only queue.
             It must be the next in the queue (checked above). */

          master_only_out = (master_only_out + 1) & QMask;
          m->flags &= ~ HELPERS_MASTER_ONLY;
          m->flags |= HELPERS_MASTER_NOW;
        }
      }
      else /* task merged into is not master-only */
      { 
        if (flags & (HELPERS_MASTER_ONLY | HELPERS_MASTER_NOW))
        { 
          /* Remove the task to merge into from the untaken or on_hold queue. */

          int j;

#         ifndef HELPERS_NO_HOLDING
          if (m->is_on_hold)
          { for (j = on_hold_out; on_hold[j]!=pipe0; j = (j + 1) & QMask)
            { if (j==on_hold_in)
              { helpers_printf("TASK TO MERGE INTO IS NOT IN ON_HOLD QUEUE!\n");
                exit(1);
              }
            }
            on_hold[j] = on_hold[on_hold_out];
            on_hold_out = (on_hold_out + 1) & QMask;
            m->is_on_hold = 0;
          }
          else
#         endif
          { 
            /* Task being merged into must be in the 'untaken' queue.  Note that
               start_lock should already be set (unless not multithreading). */

#           ifdef HELPERS_DEBUG
              if (!locked && !helpers_no_multithreading) abort();
#           endif

            for (j = untaken_out; ; j = (j + 1) & QMask)
            { if (j==untaken_in)
              { helpers_printf("TASK TO MERGE INTO IS NOT IN UNTAKEN QUEUE!\n");
                exit(1);
              }
#             ifdef HELPERS_DEBUG
                if (untaken[j]<1 || untaken[j]>MAX_TASKS) abort();
#             endif
              if (untaken[j]==pipe0)
              { break;
              }
            }

            untaken[j] = untaken[untaken_out];
            untaken_out = (untaken_out + 1) & QMask;
          }

          if ( ! (flags & HELPERS_MASTER_NOW))
          { 
            /* Add the merged task to the master-only queue. */

            master_only[master_only_in] = pipe0;
            master_only_in = (master_only_in + 1) & QMask;
          }
        }
        else /* neither task is master-only or master-now */
        {
          if ((m->flags & ~flags) & HELPERS_HOLD) /* old had HOLD, new doesn't*/
          {
            /* Move the old task from the on_hold queue, if it's there, 
               to the untaken queue.  Doesn't require start_lock to be
               set (but note the task may start running immediately). */

            if (m->is_on_hold)
            { put_in_untaken (pipe0);
            }

            m->flags &= ~ HELPERS_HOLD;
          }
        }
      }
    }

    /* Queue manipulations are all done, so release start_lock if it was set. */

#   ifndef HELPERS_NO_MULTITHREADING
    if (locked)
    { omp_unset_lock (&start_lock.lock);
    }
#   endif

    /* If we didn't merge, go on to process the new task as usual. */

    if (!merge) 
    { goto no_merge;
    }

    /* If we merged to a master-now task, set up to do it immediately. 
       Otherwise, fiddle markers (and old markers) and return from this
       function with the task left in its queue. */

    if (flags & HELPERS_MASTER_NOW)
    { 
      if (trace) 
      { trace_merged (pipe0, flags0, task_to_do, op, out, in1, in2);
      }

      /* Replace arguments of this function by merged task's values. */

      flags = flags0 = m->flags | HELPERS_MASTER_NOW;
      task_to_do = m->task_to_do;
      in1 = m->var[1];
      in2 = m->var[2];
      op = m->op;

      /* Remove the merged task from "used".  The position of the
         merged task in "used" was left in "uh" by code above. */

      helpers_tasks -= 1;
      mtix *ue = &used[helpers_tasks];
      while (uh!=ue) 
      { *uh = *(uh+1);
        uh += 1;
      }
      *uh = pipe0;

      /* Update pipe0 to be the task producing output for the merged 
         task (or zero). */

      pipe0 = m->pipe[0];
    }
    else /* not master-now */
    {
#     ifdef helpers_mark_not_in_use
      {
        char old_not_in_use_before_1 = m->not_in_use_before[1];
        helpers_var_ptr m_in1 = m->var[1];
        helpers_var_ptr m_in2 = m->var[2];
        int w;

        /* Unmark old in-use inputs if they're not also among the inputs used
           in the new merged task, and are also not in use by other tasks 
           (mimicking code in maybe_mark_not_in_use). */

        for (w = 1; w<=2; w++)
        { helpers_var_ptr v = old_var[w];
          int j;
          if (v!=null && v!=out && v!=m_in1 && v!=m_in2)
          { for (j = (m->not_in_use_before[w] ? pipe0+1 : 0); 
                 j<helpers_tasks; j++)
            { struct task_info *einfo = &task[used[j]].info;
              if (einfo->var[0]!=v && (einfo->var[1]==v || einfo->var[2]==v))
              { char d;
                ATOMIC_READ_CHAR (d = einfo->done);
                if (!d) goto next;
              }
            }
            helpers_mark_not_in_use(v);
          }
        next: ;
        }

        /* Update not_in_use_before to reflect the new inputs, and mark
           them as now in use. */

        if (m_in1!=null && m_in1!=out)
        { if (m_in1!=old_var[1])
          { m->not_in_use_before[1] = 
                m_in1==old_var[2] ? m->not_in_use_before[2]
                                  : !helpers_is_in_use(m_in1);
          }
          helpers_mark_in_use(m_in1);
        }

        if (m_in2!=null && m_in2!=out)
        { if (m_in2!=old_var[2])
          { m->not_in_use_before[2] = 
                m_in1==old_var[1] ? old_not_in_use_before_1
                                  : !helpers_is_in_use(m_in2);
          }
          helpers_mark_in_use(m_in2);
        }
      }
#     endif

      if (trace) 
      { trace_merged (pipe0, flags0, task_to_do, op, out, in1, in2);
      }

      check_consistency();  /* only enabled for debugging */

      return;
    }

  no_merge:

# endif

  check_consistency();  /* only enabled for debugging */

  /* Release any tasks on hold that compute inputs of the new task. */

# ifndef HELPERS_NO_HOLDING
  { int j;
    for (j = on_hold_out; j!=on_hold_in; j = (j + 1) & QMask)
    { mtix h = on_hold[j];
      struct task_info *info = &task[h].info;
      helpers_var_ptr hout = info->var[0];
      if (hout!=null && (hout==in1 || hout==in2))
      { put_in_untaken (h);
      }
    }
  }
# endif

  check_consistency();  /* only enabled for debugging */

  /* Set up for task - either master-now or another kind. */

  if (flags & HELPERS_MASTER_NOW)
  { 
    /* Use the pseudo-entry 0 for this master-now task, which will be done
       directly in the master.  Don't bother setting task_to_do, op, out, 
       in1, and in2 in info, since they are unused.  Note that pipe[0], 
       pipe[1], pipe[2], last_amt[0], last_amt[1], last_amt[2], and helper 
       should already be zero. */

    t = 0;
    info = &task[0].info;

    /* Wait for other tasks as required, if there are any other tasks. */

    if (helpers_tasks > 0) 
    {
      /* Mark tasks that are master-only or that compute variables needed by 
         the master-now task as needing to be started or completed.  Also mark 
         as needed those tasks needed to do those tasks, etc.*/

      int any_needed = 0;
  
      for (i = helpers_tasks-1; i>=0; i--)
      {
        struct task_info *uinfo = &task[used[i]].info;
        int needed = uinfo->needed;

        if (uinfo->flags & HELPERS_MASTER_ONLY)
        { needed = +1;
        }
        else if (needed <= 0)
        { helpers_var_ptr v0 = uinfo->var[0];
          if (v0 != null)
          { if (uinfo->flags & HELPERS_PIPE_OUT)
            { if (v0==out) 
              { needed = flags & HELPERS_PIPE_IN0 ? -1 : +1;
              }
              if (v0==in1 && needed<=0)
              { needed = flags & HELPERS_PIPE_IN1 ? -1 : +1;
              }
              if (v0==in2 && needed<=0)
              { needed = flags & HELPERS_PIPE_IN2 ? -1 : +1;
              }
            }
            else
            { if (v0==out || v0==in1 || v0==in2) 
              { needed = +1;
              }
            }
          }
        }

        if (needed != 0)
        { mark_as_needed (uinfo, needed);
#         ifndef HELPERS_NO_HOLDING
            if (uinfo->is_on_hold)
            { put_in_untaken(used[i]);
            }
#         endif
          any_needed = 1;
        }
      }

      /* Wait for the tasks marked as needed above to start or complete. */
  
      if (any_needed)
      { wait_while_any_needed();
        if (pipe0>0) pipe0 = -1; /* task pipe0 might've finished, look again */
      }
    }
  }

  else /* not master-now */
  { 
    /* Wait for a free task entry.  If there are no free entries, loop until 
       an entry is free, while doing master-only tasks that are runnable now,
       or any other tasks in the master if no runnable master-only tasks. 
       But first check whether all tasks are on hold, and if so release one. */

    if (helpers_tasks==MAX_TASKS)
    { 
#     ifndef HELPERS_NO_HOLDING
        if (((on_hold_in + 1) & QMask) == on_hold_out)
        { put_in_untaken (on_hold[on_hold_out]);
        }
#     endif

      do 
      { do_task_in_master(0);
        notice_completed();
      } while (helpers_tasks==MAX_TASKS);

      if (pipe0>0) pipe0 = -1; /* task pipe0 might've finished, look again */
    }

    /* Store info about the new task in a new task entry (t and info).  But 
       don't move helpers_tasks yet, since we don't want to see the new task. */

    t = used[helpers_tasks];
    info = &task[t].info;

    info->flags = flags;
    info->task_to_do = task_to_do;
    info->op = op;
    info->var[0] = out;
    info->var[1] = in1;
    info->var[2] = in2;

    /* Initialize to indicate not on hold, but nobody is doing this task, 
       or needs its output. */

#   ifndef HELPERS_NO_HOLDING
      info->is_on_hold = 0;
#   endif

    info->helper = -1;
    info->needed = 0;

    /* Clear 'done' and 'amt_out' in the task info for the new task.  Not
       necessary in a task done directly in the master (since never seen). */

    info->done = 0;
    info->amt_out = 0;

    /* Initialize variables used by the master in managing the list of 
       tasks in 'used'. */

    info->out_used = 0;

#   ifdef helpers_mark_not_in_use
      if (in1!=null && in1!=out)
      { info->not_in_use_before[1] = !helpers_is_in_use(in1);
      }
      if (in2!=null && in2!=out)
      { info->not_in_use_before[2] = !helpers_is_in_use(in2);
      }
#   endif

    /* Initialize extra trace info for this task. */

    if (ENABLE_TRACE>1)
    { info->last_amt[0] = info->last_amt[1] = info->last_amt[2] = 0;
    }
    if (ENABLE_TRACE>2)
    { info->start_wtime = info->done_wtime = 0.0;
    }
  }

  /* Look for the previous task (if any) outputting the output variable of the
     new task.  This might have been found earlier, and stored in pipe0, but 
     if tasks had to be done in the master, so it might have changed, it will
     have been reset to -1 so that we look again. */

  if (pipe0==-1) 
  { pipe0 = 0;
    if (out!=null && helpers_tasks>0)
    { uh = &used[helpers_tasks];
      do
      { if (task[*--uh].info.var[0]==out)
        { pipe0 = *uh;
          break;
        }
      } while (uh>used);
    }
  }

  info->pipe[0] = pipe0;
  if (pipe0>0)
  { task[pipe0].info.out_used = 1;
  }

  /* For each input variable in the new task, find the task (if any) that is
     outputting that variable.  When more than one task has the same output
     variable, the one scheduled most recently takes precedence.  If an
     input variable is the same as the output variable, we can use pipe0. 
     If helpers_is_being_computed is false, we don't need to search. */

  info->pipe[1] = info->pipe[2] = 0;

  if (helpers_tasks>0)
  { 
    uh = &used[helpers_tasks-1];

    if (in1==out) info->pipe[1] = pipe0;
    if (in2==out) info->pipe[2] = pipe0;

    if (in1==null || in1==out || !helpers_is_being_computed(in1))
    { if (in2==null || in2==out || !helpers_is_being_computed(in2))
      { goto search_done;
      }
      else
      { goto search_in2;
      }
    }

    if (in2==null || in2==out || !helpers_is_being_computed(in2))
    { goto search_in1;
    }
	
    for (;;)
    { mtix uhi = *uh;
      if (task[uhi].info.var[0]==in1)
      { info->pipe[1] = uhi;
        task[uhi].info.out_used = 1;
        goto search_in2;
      }
      if (task[uhi].info.var[0]==in2)
      { info->pipe[2] = uhi;
        task[uhi].info.out_used = 1;
        goto search_in1;
      }
      if (uh==used) 
      { break;
      }
      uh -= 1;
    }

    goto search_done;

  search_in1:
    for (;;)
    { mtix uhi = *uh;
      if (task[uhi].info.var[0]==in1)
      { info->pipe[1] = uhi;
        task[uhi].info.out_used = 1;
        goto search_done;
      }
      if (uh==used) 
      { break;
      }
      uh -= 1;
    }

    goto search_done;

  search_in2:
    for (;;)
    { mtix uhi = *uh;
      if (task[uhi].info.var[0]==in2)
      { info->pipe[2] = uhi;
        task[uhi].info.out_used = 1;
        goto search_done;
      }
      if (uh==used) 
      { break;
      }
      uh -= 1;
    }

  search_done: ;
  }

  /* Do a master-now task directly. */

  if (t==0) 
  {
    goto direct;
  }

  /* Clear debug output. */

# if ENABLE_DEBUG
  { helpers_debug_output[t][0] = 0;
  }
# endif

  /* Set the in-use and being-computed flags as appropriate, if the 
     application defined the required macros.  (Note that we don't do
     this if the task is done directly in the master, since the flags
     could never be consulted until unset anyway.) */

# ifdef helpers_mark_in_use
    if (in1!=null && in1!=out) helpers_mark_in_use(in1);
    if (in2!=null && in2!=out) helpers_mark_in_use(in2);
# endif

# ifdef helpers_mark_being_computed
    if (out!=null) helpers_mark_being_computed(out);
# endif

  /* Mark the new task entry as being in use. */

  helpers_tasks += 1;

  /* If this is a master-only task, just put it in the master_only queue. */

  if (flags & HELPERS_MASTER_ONLY)
  { master_only[master_only_in] = t;
    master_only_in = (master_only_in + 1) & QMask;
    goto scheduling_done;
  }

  /* For a task that will be on hold, just put it in the on_hold queue. */

# ifndef HELPERS_NO_HOLDING
    if (flags & HELPERS_HOLD)
    { on_hold[on_hold_in] = t;
      on_hold_in = (on_hold_in + 1) & QMask;
      task[t].info.is_on_hold = 1;
      goto scheduling_done;
    }
# endif

  /* For a non-master-only task not on hold, put it in the untaken
     queue, where it may then be noticed by a helper looking for a
     task to start (or eventually be done by the master thread). */

  put_in_untaken (t);

scheduling_done:

  /* Write trace output showing task scheduled, if trace enabled. */

  if (trace) trace_started (t, flags0, task_to_do, op, out, in1, in2);

  check_consistency();  /* only enabled for debugging */

  return;

direct:

  /* Do this task in the master without scheduling it. */

  /* Clear debug output. */

# if ENABLE_DEBUG
  { helpers_debug_output[0][0] = 0; 
  }
# endif

  if (trace) trace_started (0, flags0, task_to_do, op, out, in1, in2);

  check_consistency();  /* only enabled for debugging */

  /* Code below is like in run_this_task, except this procedure's arguments
     are used without their being stored in the task info structure, and
     there's no need to set the 'done' flag. */

  if (ENABLE_TRACE>1) 
  { this_task_info->pipe_at_start[0] = this_task_info->pipe[0];
    this_task_info->pipe_at_start[1] = this_task_info->pipe[1];
    this_task_info->pipe_at_start[2] = this_task_info->pipe[2];
    this_task_info->first_amt[0] = 0;
    this_task_info->first_amt[1] = 0;
    this_task_info->first_amt[2] = 0;
    this_task_info->last_amt[0] = 0;
    this_task_info->last_amt[1] = 0;
    this_task_info->last_amt[2] = 0;
    if (ENABLE_TRACE>2)
    { this_task_info->start_wtime = WTIME();
    }
  }

  task_to_do (op, out, in1, in2);

  if (ENABLE_TRACE>2)
  { this_task_info->done_wtime = WTIME();
  }

  if (trace) trace_completed (0);

# if ENABLE_DEBUG
  { if (helpers_debug_output[0][0] != 0)
    { helpers_printf ("HELPERS: Task %d debug output: %s\n", 0,
                       helpers_debug_output[0]);
      helpers_debug_output[0][0] = 0;  /* just in case, also set when started */
    }
  }
# endif

  /* Update stats on tasks done in master. */

  if (ENABLE_STATS) stats[0].tasks_done += 1;

  /* Set "pipe" and "last_amt" fields to zero, so that later direct
     calls of task procedures and master-now tasks will work properly. */

  info->amt_out = 0;
  info->pipe[0] = info->pipe[1] = info->pipe[2] = 0;
  if (ENABLE_TRACE>1)
  { info->last_amt[0] = info->last_amt[1] = info->last_amt[2] = 0;
  }

  check_consistency();  /* only enabled for debugging */
}


/* ----------  MAJOR PROCEDURES CALLED FROM APPLICATION PROGRAMS ------------ */

/* START COMPUTATION OF A VARIABLE.  Checks whether the task computing the
   variable passed has started in a helper.  If so, it returns immediately.
   Otherwise, it sees which tasks are needed for computation to start.  If
   a master-only task isn't needed, it waits as long as there seems to be an 
   idle helper (so helpers can do the work).  It does the task itself in the 
   master thread if there seem to be no idle helpers, or a master-only task
   needs to be done. */

void helpers_start_computing_var (helpers_var_ptr v)
{
  struct task_info *vinfo;
  int master_only_needed;
  int vindex;
  char d;
  hix h;
  int i;

  check_consistency();  /* only enabled for debugging */

  /* Quick exit if no processes scheduled (includes when no helper threads). */

  if (helpers_tasks == 0)
  { if (trace) trace_start_computing_var(0,v);
    return;
  }

  /* Flush so that 'done' and 'helper' fields will be up-to-date. */

  FLUSH;

  /* Search for the task outputting this variable.  Return if none, or task
     doing computation has finished. */

  vindex = helpers_tasks;
  for (;;)
  { vindex -= 1;
    vinfo = &task[used[vindex]].info;
    if (vinfo->var[0] == v) 
    { ATOMIC_READ_CHAR (d = vinfo->done);
      if (d)
      { if (trace) trace_start_computing_var(0,v);
        return;
      }
      else
      { break;
      }
    }
    if (vindex == 0) 
    { if (trace) trace_start_computing_var(0,v);
      return;
    }
  }

  /* See if we need to wait at all. */

  ATOMIC_READ_CHAR (h = vinfo->helper);
  if (h > 0) 
  { if (trace) trace_start_computing_var(1,v);
    return;
  }

  /* Mark the task that computes v as needing to start, along with any task 
     that computes a variable used by the task that computes v, or that is 
     needed to finish a needed task scheduled later, or that is a master-only 
     task that needs to run before a later master-only task that is needed. */

  master_only_needed = vinfo->flags & HELPERS_MASTER_ONLY;

# ifndef HELPERS_NO_HOLDING
    if (vinfo->is_on_hold)
    { put_in_untaken (used[vindex]);
    }
# endif

  if (vindex==0) /* handle this case quickly */
  { vinfo->needed = -1;
  }
  else
  {
    mark_as_needed (vinfo, -1);

    for (i = vindex-1; i>=0; i--)
    { 
      struct task_info *uinfo = &task[used[i]].info;
      int needed = uinfo->needed;

      if (master_only_needed && (uinfo->flags & HELPERS_MASTER_ONLY))
      { needed = -1;
      }

      if (needed != 0) 
      { mark_as_needed (uinfo, needed);
        if (uinfo->flags & HELPERS_MASTER_ONLY)
        { master_only_needed = 1;
        }
#       ifndef HELPERS_NO_HOLDING
          if (uinfo->is_on_hold)
          { put_in_untaken (used[i]);
          }
#       endif
      }
    }
  }

  /* If computing v does not require running a master-only task, loop until
     the computation of v has started (in which case return), or it looks 
     like no helpers are idle.  It is possible that it will look like a
     helper is idle when actually none are (since a helper might quickly
     finish a task and start on a new one), but it should not be possible for 
     the loop to continue for very long when there are no idle helpers. */

  if (!master_only_needed)
  {
    while (helpers_idle() > 0)  /* Note: helpers_idle calls notice_completed */
    {
      ATOMIC_READ_CHAR (h = vinfo->helper);
      if (h > 0) 
      { if (trace) trace_start_computing_var(2,v);
        return;
      }
    }
  }

  /* Wait until all the needed tasks have at least started (but needn't have
     finished), perhaps doing some of them in the master thread. */

  if (trace) trace_start_computing_var(3,v);

  wait_while_any_needed();
}


/* WAIT FOR TASKS USING A VARIABLE.  Waits until all tasks have completed that
   have v as an input (not including as output too).  These tasks, and the
   tasks that have to start before they can complete, are marked as needed
   so that they will preferentially be done by helpers or the master. */

void helpers_wait_until_not_in_use (helpers_var_ptr v)
{
  int any_needed, master_only_needed, i;

  check_consistency();  /* only enabled for debugging */

  /* Quick check for variable being one we needn't wait for. */

  if (v==null || !helpers_is_in_use(v))
  { if (trace) trace_wait_until_not_in_use(0,v);
    return;
  }

  /* Quick check for there being no uncompleted tasks, so no need to wait.
     Plus repeat the last test above, in case it has changed. */

  notice_completed();

  if (helpers_tasks==0 || !helpers_is_in_use(v))
  { if (trace) trace_wait_until_not_in_use(0,v);
    return;
  }

  /* Mark a task as needing to complete if it uses the variable passed as
     an input (but not as its output), or is a master-only task that needs 
     to run before some later master-only task that is needed.  Mark a task 
     as needing to start if it is needed by some needed task scheduled later. */

  any_needed = 0;
  master_only_needed = 0;

  for (i = helpers_tasks-1; i>=0; i--)
  { 
    struct task_info *uinfo = &task[used[i]].info;
    int needed = uinfo->needed;

    if (master_only_needed && (uinfo->flags & HELPERS_MASTER_ONLY)
         || (uinfo->var[1]==v || uinfo->var[2]==v) && uinfo->var[0]!=v)
    { needed = +1;
    }

    if (needed != 0) 
    { mark_as_needed (uinfo, needed);
      if (uinfo->flags & HELPERS_MASTER_ONLY)
      { master_only_needed = 1;
      }
#     ifndef HELPERS_NO_HOLDING
        if (uinfo->is_on_hold)
        { put_in_untaken (used[i]);
        }
#     endif
      any_needed = 1;
    }
  }

  /* Quick check for no "needed" tasks. */

  if (!any_needed)
  { if (trace) trace_wait_until_not_in_use(0,v);
    return;
  }

  /* Wait while doing tasks in the master and noticing completed tasks. */

  if (trace) trace_wait_until_not_in_use(1,v);

  wait_while_any_needed();

  if (trace) trace_done_waiting();
}


/* WAIT FOR TASKS THAT ARE COMPUTING VARIABLES.  Waits until all tasks have
   completed that have v1 or v2 as an output.  These tasks, and the tasks that 
   have to start before they can complete, are marked as needed so that they 
   will preferentially be done (by helpers or the master). */

void helpers_wait_until_not_being_computed2
  (helpers_var_ptr v1, helpers_var_ptr v2)
{
  int any_needed, master_only_needed, i;

  check_consistency();  /* only enabled for debugging */

  /* Quick check for there being no uncompleted tasks. */

  notice_completed();

  if (helpers_tasks==0 || (v1==null || !helpers_is_being_computed(v1)) 
                            && (v2==null || !helpers_is_being_computed(v2)))
  { if (trace) trace_wait_until_not_being_computed (0,v1,v2);
    return;
  }

  /* Mark a task as needing to complete if it computes one of the variables 
     passed as arguments, or is a master-only task that needs to run before 
     some later master-only task that is needed.  Mark a task as needing to 
     start if it is needed by some needed task scheduled later. */

  any_needed = 0;
  master_only_needed = 0;

  for (i = helpers_tasks-1; i>=0; i--)
  { 
    struct task_info *uinfo = &task[used[i]].info;
    int needed = uinfo->needed;

    if (master_only_needed && (uinfo->flags & HELPERS_MASTER_ONLY)
         || uinfo->var[0]!=null && (v1==uinfo->var[0] || v2==uinfo->var[0]))
    { needed = +1;
    }

    if (needed != 0) 
    { mark_as_needed (uinfo, needed);
      if (uinfo->flags & HELPERS_MASTER_ONLY)
      { master_only_needed = 1;
      }
#     ifndef HELPERS_NO_HOLDING
        if (uinfo->is_on_hold)
        { put_in_untaken (used[i]);
        }
#     endif
      any_needed = 1;
    }
  }

  /* Quick check for no "needed" tasks. */

  if (!any_needed)
  { if (trace) trace_wait_until_not_being_computed (0,v1,v2);
    return;
  }

  /* Wait while doing tasks in the master and noticing completed tasks. */

  if (trace) trace_wait_until_not_being_computed (1,v1,v2);

  wait_while_any_needed();

  if (trace) trace_done_waiting();
}


/* RELEASE ALL TASKS THAT ARE ON HOLD. */

#ifndef HELPERS_NO_HOLDING

static void release_all (void)
{
  while (on_hold_out!=on_hold_in)
  { put_in_untaken (on_hold[on_hold_out]);
  }
}

void helpers_release_holds (void)
{
  if (trace) trace_release_holds();

  check_consistency();  /* only enabled for debugging */

  release_all();
}

#endif


/* WAIT FOR ALL SCHEDULED MASTER-ONLY TASKS TO COMPLETE.  Also has to wait
   for tasks that need to start before some master-only task can be done. */

void helpers_wait_for_all_master_only (void)
{
  int i;

  if (trace) trace_wait_for_all_master_only();

  check_consistency();  /* only enabled for debugging */

  /* Return if there are no uncompleted master-only tasks. */

  if (master_only_in==master_only_out)
  { return;
  }

  /* Mark a task as needing to start if it is master-only, or is needed to 
     do a needed task scheduled later. */

  for (i = helpers_tasks-1; i>=0; i--)
  { 
    struct task_info *uinfo = &task[used[i]].info;
    int needed = uinfo->needed;

    if (uinfo->flags & HELPERS_MASTER_ONLY)
    { needed = +1;
    }

    if (needed != 0) 
    { mark_as_needed (uinfo, needed);
    }
  }

  /* Wait while doing tasks in the master and noticing completed tasks. */

  wait_while_any_needed();

  if (trace) trace_done_waiting();
}


/* WAIT FOR ALL SCHEDULED TASKS TO COMPLETE. */

void helpers_wait_for_all (void)
{
  int i;

  if (trace) trace_wait_for_all();

  check_consistency();  /* only enabled for debugging */

  /* Quick check for no uncompleted tasks. */

  if (helpers_tasks==0) 
  { return;
  }

  /* Release all tasks that are on hold. */

# ifndef HELPERS_NO_HOLDING
    release_all();
# endif

  /* Mark all tasks as needed. */

  for (i = 0; i<helpers_tasks; i++)
  { ATOMIC_WRITE_CHAR (task[used[i]].info.needed = +1);
  }

  /* Wait while doing tasks in the master and noticing completed tasks. */

  wait_while_any_needed();

  if (trace) trace_done_waiting();
}


/* -----------------------  PIPELINING PROCEDURES  -------------------------- */

/* CHECK WHETHER THERE'S ANY NEED FOR PIPELINING OUTPUT. */

#ifndef HELPERS_NO_MULTITHREADING

int helpers_output_perhaps_pipelined (void)
{ 
  return (this_task_info->flags & HELPERS_PIPE_OUT) != 0;
}

#endif

/* SAY HOW MUCH OF THE OUTPUT HAS BEEN PRODUCED SO FAR.  Changes the 
   amt_out field for this task - without synchronization, on the assumption 
   that reading and writing are atomic operations.  A flush is done before
   to ensure that the new data is there before the updated value for amt_out
   indicates that it is there.  No flush is done after the update - at worst,
   the new value for amt_out will be flushed on the next call, or when the
   task finishes. */

#ifndef HELPERS_NO_MULTITHREADING

void helpers_amount_out (helpers_size_t amt)
{ 
  struct task_info *info = this_task_info;

  if (info->flags & HELPERS_PIPE_OUT)
  { FLUSH;
    ATOMIC_WRITE_SIZE (info->amt_out = amt);
  }
}

#endif


/* GET THE AMOUNT OF AN INPUT THAT HAS BEEN PRODUCED SO FAR.  Care is needed to
   handle the possibility that the task producing the input will terminate
   around this time, and another task with the same task index will immediately
   start.  To handle this, the amt_out field for the task producing the
   input is read before a flush, and termination of the input task is checked
   after the flush.  If that check says the input task hadn't terminated at
   the later time, the earlier read before the flush must have obtained amt_out
   for that input task, not some later task.  If the input task has terminated,
   the maximum passed is returned.  No flush is done at the beginning - at 
   worst, fresh values will be obtained on the next call. */

#ifndef HELPERS_NO_MULTITHREADING

helpers_size_t helpers_avail0 (helpers_size_t mx)
{
  struct task_info *info = this_task_info;

  helpers_size_t n;
  char d;
  tix p;

  ATOMIC_READ_CHAR (p = info->pipe[0]);
  if (p == 0) return mx;

  ATOMIC_READ_CHAR (d = task[p].info.done);
  if (d) return mx;

  ATOMIC_READ_SIZE (n = task[p].info.amt_out);

  FLUSH;

  ATOMIC_READ_CHAR (p = info->pipe[0]);
  if (p==0) return mx;

  if (ENABLE_TRACE>1)
  { if (info->first_amt[0]==0) info->first_amt[0] = n;
    info->last_amt[0] = n;
  }

  return n;
}

helpers_size_t helpers_avail1 (helpers_size_t mx)
{
  struct task_info *info = this_task_info;

  helpers_size_t n;
  char d;
  tix p;

  ATOMIC_READ_CHAR (p = info->pipe[1]);
  if (p == 0) return mx;

  ATOMIC_READ_CHAR (d = task[p].info.done);
  if (d) return mx;

  ATOMIC_READ_SIZE (n = task[p].info.amt_out);

  FLUSH;

  ATOMIC_READ_CHAR (p = info->pipe[1]);
  if (p==0) return mx;

  if (ENABLE_TRACE>1)
  { if (info->first_amt[1]==0) info->first_amt[1] = n;
    info->last_amt[1] = n;
  }

  return n;
}

helpers_size_t helpers_avail2 (helpers_size_t mx)
{
  struct task_info *info = this_task_info;

  helpers_size_t n;
  char d;
  tix p;

  ATOMIC_READ_CHAR (p = info->pipe[2]);
  if (p == 0) return mx;

  ATOMIC_READ_CHAR (d = task[p].info.done);
  if (d) return mx;

  ATOMIC_READ_SIZE (n = task[p].info.amt_out);

  FLUSH;

  ATOMIC_READ_CHAR (p = info->pipe[2]);
  if (p==0) return mx;

  if (ENABLE_TRACE>1)
  { if (info->first_amt[2]==0) info->first_amt[2] = n;
    info->last_amt[2] = n;
  }

  return n;
}

#endif


/* GET THE AMOUNT OF A VARIABLE THAT HAS BEEN PRODUCED SO FAR.  Searches for the
   most recently scheduled task that has that variable as its output, and 
   returns its amt_out value.  Returns the maximum passed if no task is 
   outputting the variable.  Note that there is no need to worry about
   the task with some id changing, since this is called only by the master. */

#ifndef HELPERS_NO_MULTITHREADING

helpers_size_t helpers_avail_var (helpers_var_ptr v, helpers_size_t mx)
{
  helpers_size_t n;
  char d;
  int i;

  for (i = helpers_tasks-1; i>=0; i--)
  { struct task_info *info = &task[used[i]].info;
    if (info->var[0] == v)
    { FLUSH;
      ATOMIC_READ_CHAR (d = info->done);
      if (d) return mx;
      ATOMIC_READ_SIZE (n = info->amt_out);
      return n;
    }
  }

  return mx;
}

#endif


/* -------------  MISCELLANEOUS PROCEDURES USED BY APPLICATIONS  ------------ */


/* RETURN AN ESTIMATE OF THE NUMBER OF IDLE HELPERS.  Note that it starts by 
   calling notice_completed (unless no multithreading is going on now, or
   no tasks are outstanding). */

#ifndef HELPERS_NO_MULTITHREADING

int helpers_idle (void)
{
  int i, c;
  hix h;

  if (helpers_not_multithreading_now) 
  { return 0;
  }

  if (helpers_tasks==0)
  { return helpers_num;
  }

  notice_completed();

  c = helpers_num;

  for (i = 0; i<helpers_tasks; i++)
  { ATOMIC_READ_CHAR (h = task[used[i]].info.helper);
    if (h>0) c -= 1;
    if (c<=0) return 0;
  }

  return c;
}

#endif


/* RETURN A POINTER TO DATA FOR A MERGED TASK. */

double *helpers_task_data (void)
{ 
  return this_task_info->task_data;
}


/* RETURN A LIST OF ALL VARIABLES USED BY UNCOMPLETED TASKS. */

helpers_var_ptr *helpers_var_list (int out_only)
{
  int i, j;

  check_consistency();  /* only enabled for debugging */

  notice_completed();

  for (i = 0, j = 0; i<helpers_tasks; i++)
  { struct task_info *info = &task[used[i]].info;
    if (info->var[0]!=null) var_list[j++] = info->var[0];
    if (!out_only)
    { if (info->var[1]!=null) var_list[j++] = info->var[1];
      if (info->var[2]!=null) var_list[j++] = info->var[2];
    }
  }

  var_list[j] = (helpers_var_ptr) 0;

  return var_list;
}


/* SET FLAG MASK AND "NOW" VARIABLES ACCORDING TO CURRENT OPTIONS. */

static void set_flag_mask_now (void)
{
# ifndef HELPERS_NO_MULTITHREADING
    helpers_not_multithreading_now
      = helpers_not_multithreading || helpers_are_disabled || helpers_num==0;
    helpers_not_pipelining_now
      = helpers_not_pipelining || helpers_not_multithreading_now;
# endif

# ifdef helpers_can_merge
    helpers_not_merging_now
      = helpers_not_merging || helpers_are_disabled;
# endif

# ifndef HELPERS_NO_HOLDING
    helpers_not_holding_now
      = helpers_not_holding || helpers_are_disabled;
# endif

  flag_mask = ~0;

  if (helpers_not_pipelining_now)
  { flag_mask &= ~HELPERS_PIPE_IN012_OUT;
  }

  if (helpers_not_merging_now)
  { flag_mask &= ~HELPERS_MERGE_IN_OUT;
  }

  if (helpers_not_holding_now)
  { flag_mask &= ~HELPERS_HOLD;
  }
}


/* DISABLE / RE-ENABLE PIPELINING. */

#ifndef HELPERS_NO_MULTITHREADING

void helpers_no_pipelining (int a)
{
  helpers_not_pipelining = a!=0;

  set_flag_mask_now();

  if (trace) 
  { helpers_printf ("HELPERS: Pipelining %s\n",
                    helpers_not_pipelining ? "disabled" : "enabled");
  }
}

#endif


/* DISABLE / RE-ENABLE TASK MERGING. */

#ifdef helpers_can_merge

void helpers_no_merging (int a)
{
  helpers_not_merging = a!=0;

  set_flag_mask_now();

  if (trace) 
  { helpers_printf ("HELPERS: Task merging %s\n",
                    helpers_not_merging ? "disabled" : "enabled");
  }
}

#endif


/* DISABLE / RE-ENABLE HELPERS.  Before disabling, wait for all tasks to 
   complete, and clear task[0].info. */

void helpers_disable (int a)
{
  if (a && !helpers_are_disabled)
  { helpers_wait_for_all();
    memset (&task[0].info, 0, sizeof task[0].info);
  }
  helpers_are_disabled = a!=0;

  set_flag_mask_now();
  FLUSH;

  if (trace) 
  { helpers_printf ("HELPERS: Task deferral %s\n",
                    helpers_are_disabled ? "disabled" : "enabled");
  }
}


/* DISABLE / RE-ENABLE USE OF HELPER THREADS.  Before setting, wait for
   all tasks to complete. */

#ifndef HELPERS_NO_MULTITHREADING

void helpers_no_multithreading (int a)
{
  helpers_wait_for_all();
  helpers_not_multithreading = a!=0;

  set_flag_mask_now();
  FLUSH;

  if (trace) 
  { helpers_printf ("HELPERS: Multithreading %s\n",
                    helpers_not_multithreading ? "disabled" : "enabled");
  }
}

#endif


/* DISABLE / RE-ENABLE HOLDING OF TASKS. */

#ifndef HELPERS_NO_HOLDING

void helpers_no_holding (int a)
{
  helpers_not_holding = a!=0;

  set_flag_mask_now();
  FLUSH;

  if (trace) 
  { helpers_printf ("HELPERS: Holding %s\n",
                    helpers_not_holding ? "disabled" : "enabled");
  }
}

#endif


/* CHANGE WHETHER OR NOT TRACE OUTPUT IS PRODUCED. */

void helpers_trace (int f)
{
# if ENABLE_TRACE!=0
    trace = f;
# endif
}


/* PRINT STATISTICS. */

void helpers_stats (void)
{
  int tot_done, tot_woken;
  struct stats *h;
  int j;

  if (!ENABLE_STATS) return;

  if (this_thread!=0) return;

  helpers_printf("\nHELPERS STATISTICS\n\n");

  if (helpers_num==0)
  { helpers_printf ("          Tasks done\n\n");
  }
  else
  { helpers_printf ("          Tasks done  Times woken\n\n");
  }

  tot_done = 0;
  tot_woken = 0;

  for (j = 1; j<=helpers_num; j++)
  { h = &stats[j];
    helpers_printf ("helper %-2d %8d    %9d\n",
                    j, h->tasks_done, h->times_woken);
    tot_done += h->tasks_done;
    tot_woken += h->times_woken;
  }

  if (helpers_num>0)
  { helpers_printf ("master    %8d\n\n", stats[0].tasks_done);
  }
  tot_done += stats[0].tasks_done;

  helpers_printf ("totals    %8d    %9d\n", tot_done, tot_woken);
}


/* ------------------------  STARTUP PROCEDURE  ----------------------------- */

/* START HELPER THREADS AND RUN MASTER PROCEDURE. */

void helpers_startup (int n)
{
  mtix i;

  /* Check that the size of a task entry isn't bigger than intended. */

  if (sizeof task[0] != sizeof task[0].space)
  { helpers_printf(
     "WARNING: SIZE OF TASK ENTRY BIGGER THAN INTENDED MAXIMUM!\n");
  }

  /* Record initial wall clock time, if ENABLE_TRACE is 3. */

  if (ENABLE_TRACE>2)
  { init_wtime = WTIME();
  }

  /* Initialize for there being no tasks yet. */

  for (i = 0; i<MAX_TASKS; i++) used[i] = i+1;
  helpers_tasks = 0;

  master_only_in = master_only_out = 0;
  untaken_in = untaken_out = 0;

  this_task = 0;
  this_task_info = &task[this_task].info;

  /* Set the number of helpers desired, always zero if no multithreading. */

# ifndef HELPERS_NO_MULTITHREADING
    helpers_num = n<0 ? 0 : n>HELPERS_MAX ? HELPERS_MAX : n;
# endif

  /* If no helpers are wanted, just call the master procedure, then exit. */

  if (helpers_num==0) 
  { 
#   ifndef HELPERS_NO_MULTITHREADING
      helpers_not_multithreading = 1;
      helpers_not_pipelining = 1;
#   endif

    set_flag_mask_now();

    helpers_master();
    exit(0);
  }

  /* Otherwise, set up for using helper threads.  If HELPERS_NO_MULTITHREADING
     is defined, helpers_num will be zero, so this will code not be reached,
     and is disabled so that OpenMP support will not be needed. */

# ifndef HELPERS_NO_MULTITHREADING

  helpers_not_multithreading = 0;
  helpers_not_pipelining = 0;

  /* Initialize all locks. */

  omp_init_lock (&untaken_lock.lock);
  omp_init_lock (&start_lock.lock);
  omp_init_lock (&suspend_lock[0].lock);
  omp_init_lock (&suspend_lock[1].lock);
  
  which_suspends = which_wakes = 0;
  suspend_initialized = 0;

  done_setup = 1;  /* check_consistency can now look at things */

  #pragma omp parallel num_threads(helpers_num+1)
  {
    /* Get thread number, with master being zero. */

    this_thread = omp_get_thread_num();

    if (this_thread==0)
    {
      /* CODE EXECUTED BY THE MASTER THREAD. */

      /* Find out how many helpers we really have. */

      helpers_num = omp_get_num_threads() - 1;

      if (helpers_num==0) 
      { helpers_not_multithreading = 1;
        helpers_not_pipelining = 1;
      }

      set_flag_mask_now();

      /* Set suspend_lock[0] so helpers will be able to suspend themselves. */

      omp_set_lock (&suspend_lock[0].lock);

      ATOMIC_WRITE_CHAR (suspend_initialized = 1);
      FLUSH;

      /* Run the user-supplied master procedure. */

      helpers_master();

      exit(0);
    }
    else
    {
      /* CODE EXECUTED BY THE HELPER THREADS. */

#     ifdef helpers_helper_init
        helpers_helper_init();
#     endif

      char si;
  
      /* Wait for master to set suspend lock. */
  
      do {
        FLUSH;
        ATOMIC_READ_CHAR (si = suspend_initialized);
      } while (!si);
  
      /* Run the procedure done in each helper. */
  
      helper_proc();
    }
  }

# endif

}

#endif
