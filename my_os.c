//*****************************************************************************
//
//                  my_os.c - My little embedded OS 
//                Copyright (c) 2002, John C. Overton
// 
//   Description:
//      This module is a small embedded os for small applications.
//      This version was designed for an ARM M3 or R3 processor using 
//      gcc and a small library containing at least some memory management.
//
//*****************************************************************************

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "my_os.h"



#define _OS_INT_DISABLE()  (IntMasterDisable())
#define _OS_INT_ENABLE()   (IntMasterEnable())



//*****************************************************************************
// Global variables...
//*****************************************************************************
TASK*  ActiveTasks          = NULL;    // Chain of all created tasks.
TASK*  RunningTask          = NULL;    // TASK structure of currently running task.
TASK*  FreeTaskStructs      = NULL;    // Cache free task structures.
U32    LastAllocatedTaskNbr = 0;       // Last assigned task number.
EVENT* EventQueue           = NULL;    // Event queue of timer events.          
EVENT* EventFree            = NULL;    // Cache free event structures.          
U32    GlobalSeconds        = 0;       // Elapsed time seconds.                 
U16    GlobalMillisecs      = 0;       // Elapsed time milliseconds of seconds. 



//*****************************************************************************
// Definitions of internal functions...
//*****************************************************************************
static void OSChainEvent(   EVENT* Event );
static void OSUnchainEvent( EVENT* Event );



//*****************************************************************************
//
// OSNewTaskStub() -- Internal routine that is the starting place for a new task.
//
//    Description:
//         This is where a new task starts out the first time it is dispatched.
//         If the new task ever returns, then call OSDestroy task to destroy it.
//         This function will not return to it's caller.
//
//    Inputs:
//         Nothing.
//
//    Returns:
//         Nothing.
//
//*****************************************************************************
void OSNewTaskStub( void )
{
   TASK* task = RunningTask;
   // Now go to start of new task.  If we come back, then destroy us.
   task->TaskStart(task->TaskStartParam);

   // Destroy us, never to return.
   OSTaskDestroy();
}



//*****************************************************************************
//
// OSTaskCreate() -- Called to create a task. 
//
//    Description:
//         Create a new task.  We create a task structure, which is the repres-
//         tation of the new task and contains context state so that the task
//         can run independantly of other tasks in the system.  The first time
//         we are called, we see that there have never been any tasks created
//         and therefor the caller is not a task - so we turn him into one.
//         Once the task structure is set up, we put task into the TASK_READY 
//         state so that OSTaskDispatch() will dispatch him when his time comes.
//
//    Inputs:
//         Function - A pointer to the funtion to start to execute when dispatched.
//         Parameter - A U32 (unsigned long) parameter that is pasted to Function.
//         StackSize - The size of stack space that this task needs.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Task was successfully created and will get dispatched.
//              RC_NOMEM - Out of memory. Bummer!
//       
//*****************************************************************************
RC OSTaskCreate(char* Name, PTR Function, U32 Parameter, U32 StackSize) 
{
   TASK*   task;
   U32* reg_save;

   _OS_INT_DISABLE();

   // If this is first time, then turn us into the idle task (task #1)...
   if( ActiveTasks == NULL)
   {
      task = calloc(1, sizeof(TASK));
      if(task == NULL) 
      {
         return RC_NOMEM;
      }
      task->TaskNbr   = LastAllocatedTaskNbr = 1;
      task->TaskName  = "IdleTask";
      // We are the currently running task.
      task->TaskState = TASK_RUNNING;
      ActiveTasks = task;                    // We are the first on the chain of active guys.
      RunningTask = task;                    // We are the currently running task.
   }

   // Now, start to allocate new task. See if we have a free task structure, 
   // otherwise allocate one...
   if((task = FreeTaskStructs) != NULL) 
   {
      FreeTaskStructs = task->TaskChain; // Finish unchain from free cache.
      memset((void*)task, 0, sizeof(TASK));     // Clean out structure.
   }
   else
   {
      task = calloc(1, sizeof(TASK));
      if(task == NULL) 
      {
         _OS_INT_ENABLE();
         return RC_NOMEM;
      }
   }

   // Save function ptr and function parameter...
   task->TaskStart      = Function;
   task->TaskStartParam = Parameter;

   // Save pointer to name of task (helpful for debugging and stuff!)...
   task->TaskName       = Name;

   // Allocate a stack for this task...
   if((task->TaskStackBase = calloc(1, StackSize)) == NULL) 
   {
      _OS_INT_ENABLE();
      return RC_NOMEM;
   }

   // Give this guy a number...
   task->TaskNbr = ++LastAllocatedTaskNbr;

   // Put our new task on the Active Task chain and say he's ready to roll...
   task->TaskChain = ActiveTasks;
   ActiveTasks     = task;
   task->TaskState = TASK_READY;

   // Save tasks new context. 
   setjmp(task->TaskContext);   // Saves r4,r5,r6,r7,r8,r9,sl,fp,sp,lr regs.

   //**************************************************************************
   // NOTE: ARCHITECTURE SPECIFIC CODE... (Should be moved into sep file)
   // Set up jump buffer to go to OSNewTaskStub() first time...
   //To make life easier, we use this pointer to array of regs... 
   reg_save = (U32*)(&(task->TaskContext));
   reg_save[8] = (U32) task->TaskStackBase + StackSize; 
   reg_save[9] = (U32) OSNewTaskStub;
   // END OF ARCHITECTURE SPECIFIC CODE.
   //**************************************************************************

   _OS_INT_ENABLE();

   return RC_OK;
}



//*****************************************************************************
//
// OSTaskDestroy() -- Destroy currently running task.
//
//    Description:
//         Called to destroy currently running task.  So, all we do here is to  
//         just mark task as a zombie task.  Then, when the task dispatcher     
//         dispatches the idle task (task #1) it will clean up the task by      
//         freeing it's stack memory and popping the task struct onto the free  
//         chain.  This routine does not return.  It goes off to                
//         OSDispatchTask().                                                    
//
//    Inputs:
//         Nothing.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Task was successfully created and will get dispatched.
//
//*****************************************************************************
RC OSTaskDestroy( void )
{
   _OS_INT_DISABLE();

   RunningTask->TaskState = TASK_ZOMBIE;

   //_OS_INT_ENABLE();

   OSTaskDispatch();

   return RC_OK;
}



//*****************************************************************************
//
// OSTaskDispatch() -- Run next TASK_READY task or return if none ready. 
//
//    Description:
//         Run next TASK_READY task or return if none ready.  This routine is  
//         ment to be called from the idle task (Task #1).  If none are ready  
//         to be dispatched and before we return to the idle task, we check to 
//         see if there are any zombie tasks that need to be cleaned up.  If   
//         so, we free their stack and cache their task structure.  This       
//         routine is also called when a task ends.  In this case, what        
//         probably happens (should happen) is that some other task will get   
//         dispatched (at least the idle task will get dispatched because he   
//         never should block).  Of course, as mentioned before, if the idle   
//         task does get dispatched, it will clean up all zombie tasks in-     
//         cluding the one that just ended.                                    
//
//    Inputs:
//         Nothing.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Operation was normal. We may have run one or more
//                      tasks before we returned to the caller.
//
//*****************************************************************************
RC OSTaskDispatch( void )
{
   TASK*   task;
   TASK*   prev_task;
   TASK*   next_task = NULL;

   _OS_INT_DISABLE();

   // First, change us from Running to Ready. Some tasks enter from services
   // so their state may have already changed. So test to see if we are Running
   // before changing to Ready...
   if( RunningTask->TaskState == TASK_RUNNING)
      RunningTask->TaskState = TASK_READY;

   // Save context of calling task.  A just dispatched task will return from the
   // setjmp() call.
   if (setjmp(RunningTask->TaskContext) == 0)
   {
       // We're here because we just saved context, not because task just got
       // dispatched. So, see if there is a new task that we can dispatch...

       // Do something LIKE round-robin scheduling.  We will start looking for READY
       // tasks starting from the one that just ran.  If we get to the bottom of the 
       // active task list, start back at the top.  Of course, there are problems with 
       // this algorithm; for example, new tasks get run before tasks that have been 
       // waiting and are back at the top of the chain. Review this code later...
       task = RunningTask;
       do {
          if(task == NULL)                        // If we've come to the end of the task chain...
          {
             task = ActiveTasks;                  // Then, start back up at the top of the chain.
          }
          else
          {
             task = task->TaskChain;              // Otherwise, go to next task on the chain.
          }

          // If we've got back to the same task that we started at and it's not runnable...
          if(task == RunningTask && task->TaskState != TASK_READY)
          {                                       // Then open an interrupt window
             _OS_INT_ENABLE();                    // to see if any interrupts pending.
             _OS_INT_DISABLE();                   // (Architecturally, this will let task #1 block.)
          }
       } while(task->TaskState != TASK_READY);

       // If this is not the currently running task, then switch to his context...
       // Otherwise, no need to switch contexts...
       if(task != RunningTask) 
       {
          RunningTask = task;
          RunningTask->TaskState = TASK_RUNNING;
          longjmp(task->TaskContext, 1);   // Does not return.
       }
       // Otherwise, dispatching same task. Change back to running...
       RunningTask->TaskState = TASK_RUNNING;
   }

   // Check to see if we are idle task and if so, check for any zombies that
   // need to be cleaned up...
   if(RunningTask->TaskNbr == 1) {

      // Find first/next zombie task...
      task = ActiveTasks;
      prev_task = NULL;
      while(task)
      {
         if(task->TaskState == TASK_ZOMBIE) 
         {
            // This one is a zombie. Unchain him...
            if(prev_task) 
            {
               prev_task->TaskChain = task->TaskChain;
            }
            else
            {
               ActiveTasks = task->TaskChain;
            }

            // Save next task to check for zombie (if there is a next one).
            next_task            = task->TaskChain;

            // Free his stack...
            free(task->TaskStackBase);

            // Now cache this free task structure on the free chain.
            task->TaskState = TASK_NOTACTIVE;
            task->TaskChain = FreeTaskStructs;
            FreeTaskStructs = task;
            task = next_task;
         }
         else
         {
            // Current one was not zombie, so go to next one to check it...
            prev_task = task;
            task = task->TaskChain;   // Next one on active chain.
         }
      }
   }

   _OS_INT_ENABLE();

   return RC_OK;
}



//*****************************************************************************
//
// OSSemPost() -- Post a semaphore. 
//
//    Description:
//         Post a semaphore by incrementing semaphore count and making 
//         first task on waiting chain (if any) runnable.  This function can 
//         be called from an interrupt routine. Semaphores need to be
//         initialized before being used.  They can be initialized manually, 
//         by clearing and setting the count to the number of resources to 
//         protect (usually just one), or you can use the OS_INIT_SEMAPHORE()
//         macro to initialize a semaphore.
//
//    Inputs:
//         sem - Pointer to semaphore to post.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - The operation was successful.
//
//*****************************************************************************
RC OSSemPost(SEMAPHORE* sem)
{
   TASK* task;

   _OS_INT_DISABLE();

   // If Count is < 0 and there is a task waiting, then make that task ready...
   if( sem->SemCount++ < 0 && sem->SemWait )
   {
      task            = sem->SemWait;           // Point to waiting task;
      sem->SemWait    = task->TaskWait;         // Dequeue waiting task.

      if(sem->SemWait == NULL)                  // Was he the only one in queue?
      {
         sem->SemWaitEnd = NULL;                // Then clear tail end pointer.
      }

      task->TaskWaitState &= ~TASK_WAIT_SEMAPHORE; // Clear his "waiting on semaphore" flag.
      // Since we could be entered while task is running by way of interrupt
      // routine, check before changing state to ready...
      if( task->TaskState != TASK_RUNNING )        // If not already running...
      {
         task->TaskState = TASK_READY;             // Make him ready to run.
      }
   }
   
   _OS_INT_ENABLE();

   return RC_OK;
}



//*****************************************************************************
//
// OSSemPoll() -- Return the number of available resources (semaphore count).
//
//    Description:
//         Return the number of available resources (semaphore count). If the 
//         semaphore can be acquired (count 1 or greater), then it will be.                                                                          
//         Otherwise this routine will not wait.                                                                          
//                                                                                   
//
//    Inputs:
//         sem - Pointer to semaphore to try to acquire.
//
//    Returns:
//         The current count from the semaphore. If the count zero or greater,
//         then the semaphore was successfully acquired.  If count is < 0, then
//         operation did not acquire the semaphore.
//
//*****************************************************************************
U32 OSSemPoll(SEMAPHORE* sem)
{
   U32 count;

   _OS_INT_DISABLE();

   // If Semaphore count is > 0, acquire the semphore...
   if( (count = sem->SemCount) > 0)
   {
      count = --sem->SemCount;  
   }
   
   _OS_INT_ENABLE();

   return count;
}



//*****************************************************************************
//
// OSSemWait() --  Acquire or wait on a semaphore.
//
//    Description:
//         Acquire or wait on a semaphore.  This routine can not be called from
//         an interrupt routine because it may block the currently running task
//         (interrupt routines run ontop of the currently running task - they  
//         are not a task themselves.) Semaphores need to be initialized before
//         being used.  They can be initialized manually, by clearing and      
//         setting the count to the number of resources to protect (usually    
//         just one), or you can use the OS_INIT_SEMAPHORE() macro to          
//         initialize a semaphore.                                             
//
//    Inputs:
//         sem - Pointer to semaphore to try to acquire.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Operation was successful.  The Semaphore has been acquired.
//
//*****************************************************************************
RC OSSemWait(SEMAPHORE* sem)
{
   TASK* task;

   _OS_INT_DISABLE();
   
   // If count goes negative, then make current process wait...
   if( --sem->SemCount < 0) 
   {
      task                = RunningTask;      // Get currently running task (us).
      task->TaskState     = TASK_WAITING;     // Say that we are waiting.
      task->TaskWaitState = TASK_WAIT_SEMAPHORE;    // Say that we are waiting on semaphore.
      task->TaskWait      = sem->SemWaitEnd;  // Queue him off of the semaphore.
      sem->SemWaitEnd     = task;             // At the end of the queue.
      if(sem->SemWait)                        // Is he the first in queue?
      {
         sem->SemWait     = task;             // Yes.
      }
      task->TaskResource  = (void*) sem;      // Helpful for debugging.
   }

   _OS_INT_ENABLE();

   // Go try to dispatch some other task...
   OSTaskDispatch();                        

   return RC_OK;
}



//*****************************************************************************
//
// OSSemWaitTimeout() --  Acquire or wait on a semaphore with a timeout value.     
//
//    Description:
//         Acquire or wait on a semaphore with a timeout value.  This routine 
//         will acquire or wait on a semaphore.  If it waits on a semaphore,  
//         it will only wait as long as n milliseconds, and if that time      
//         expires, it will not acquire the semaphore, but will return a code 
//         indicating it timed out.  Semaphores need to be initialized before 
//         being used.  They can be initialized manually, by clearing and     
//         setting the count to the number of resources to protect (usually   
//         just one), or you can use the OS_INIT_SEMAPHORE(sem, count) macro  
//         to initialize a semaphore.
//
//    Inputs:
//         sem       - Pointer to semaphore to try to acquire.
//         Millisecs - Amount of time (in milliseconds) to wait when trying
//                     to acquire the semaphore before timing out and returning.
//                     Input is in milliseconds, but resolution is based on 
//                     timer tick resolution.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Task was successfully created and will get dispatched.
//              RC_NOMEM   - No memory available to allocate timer event structure.
//              RC_TIMEOUT - The timeout value has expired before the semaphore
//                           was aquired.
//
//*****************************************************************************
RC OSSemWaitTimeout(SEMAPHORE* sem,  U32 Millisecs)
{
   TASK*       task;
   EVENT*      event;
   TASK*       next_task;
   TASK*       last_task;
   RC          rc = RC_OK;

   _OS_INT_DISABLE();

   // Fast path: First see if we can immediately acquire semaphore...
   if(--sem->SemCount >= 0) 
   {
      _OS_INT_ENABLE();
      return RC_OK;
   }
 
   // Queue up task on semaphore...
   task                = RunningTask;         // Get currently running task (us).
   task->TaskState     = TASK_WAITING;        // Say that we are waiting.
   task->TaskWaitState = TASK_WAIT_SEMAPHORE; // Say that we are waiting on semaphore.
   task->TaskWait      = sem->SemWaitEnd;     // Queue him off of the semaphore.
   sem->SemWaitEnd     = task;                // At the end of the queue.
   if(sem->SemWait == NULL)                   // Is he the first in queue?
   {
      sem->SemWait     = task;                // Yes.
   }
   task->TaskResource  = (void*) sem;         // Helpful for debugging.

   // See if there's a free Event structure, or allocate new one...
   if ((event = EventFree) != NULL)
   {
      EventFree = event->EventNext;
      memset((void*)event, 0, sizeof(EVENT));
   }
   else
   {
      if((event = (EVENT*)calloc(1, sizeof(EVENT))) == NULL) 
      {
         _OS_INT_ENABLE();
         return RC_NOMEM;
      }
   }

   // Calculate when event will expire...
   event->EventSeconds   = GlobalSeconds   + (Millisecs / 1000);
   event->EventMillisecs = GlobalMillisecs + (Millisecs % 1000);
   if(event->EventMillisecs > 1000) 
   {
      event->EventMillisecs -= 1000;
      event->EventSeconds++;
   }

   // Say we will also be waiting for timer to expire...
   RunningTask->TaskWaitState |= TASK_WAIT_TIMER;
   event->EventTask            = RunningTask;

   // Now, queue up timer event in ascending order onto chain...
   OSChainEvent(event);
   
   // Go try to dispatch some other task...
   //_OS_INT_ENABLE();
   OSTaskDispatch();                        
   _OS_INT_DISABLE();

   // If timer did not yet expire, we need to unchain event...
   if(RunningTask->TaskWaitState & TASK_WAIT_TIMER) 
   {
      OSUnchainEvent( event );
      // Chain event structure on free chain...
      event->EventNext = EventFree;
      EventFree        = event;
      RunningTask->TaskWaitState &= ~TASK_WAIT_TIMER;
   }

   // If semaphore did not post, then we need to "un-wait" the semaphore
   // and return with rc that indicates timer expired...
   if((RunningTask->TaskWaitState & TASK_WAIT_SEMAPHORE) != 0 ) 
   {
      ++sem->SemCount;            // We are not waiting on the semaphore anymore.
      // Now, find where we are on the semaphore's wait list...
      last_task = NULL;
      next_task = sem->SemWait;                   // Is he the first in queue?
    
      do {

         if(next_task == RunningTask) 
         {
            // Was there a task waiting before us?
            if(last_task) 
            {
               last_task->TaskWait = next_task->TaskWait;
            }
            else
            {
               sem->SemWait = next_task->TaskWait;
            }

            // If we were at the end of the list, then update tail pointer...
            if(next_task == sem->SemWaitEnd) 
            {
               sem->SemWaitEnd = last_task;   // Either prev task or NULL (i.e. no tasks).
            }
            break;
         }
         last_task = next_task;
         next_task = next_task->TaskWait;

      } while( next_task != NULL );

      rc = RC_TIMEOUT;
   } 

   // Clear waiting flags...
   RunningTask->TaskWaitState = 0;

   _OS_INT_ENABLE();

   return rc;
}



//*****************************************************************************
//
// OSTickHandler() -- Tick interrupt routine
//
//    Description:
//         Called from a Tick interrupt routine at the rate of once every     
//         1/100th of a second (every .01 sec).  On entry interrupts should be 
//         disabled.  This routing updates the global time and dispatches any                                                        
//         expired timer events on the timer event queue.
//
//    Inputs:
//         Nothing.
//
//    Returns:
//         Nothing.
//
//*****************************************************************************
void OSTickHandler( void )
{
   EVENT*     p;
   TASK*      task;

   // First, update our elapsed time...
   GlobalMillisecs += 10;
   if(GlobalMillisecs > 1000) 
   {
      GlobalMillisecs -= 1000;
      GlobalSeconds++;
   }

   // Now, check for any expirees. Since the chain is in ascending order,
   // we just need to check the top of the chain for an expiree...
   p = EventQueue;
   while(p)
   {
      if(  p->EventSeconds < GlobalSeconds ||
          (p->EventSeconds == GlobalSeconds && p->EventMillisecs <= GlobalMillisecs))
      {
         task                 = p->EventTask;      // Save pointer to waiting task.
         EventQueue           = p->EventNext;      // Unchain this guy; was top of chain.
         p->EventNext         = EventFree;         // And push him onto free chain.
         EventFree            = p;                 // Ditto.
         p                    = EventQueue;        // New p is the new top of chain.

         task->TaskState      = TASK_READY;        // Make this task ready to run.
         task->TaskWaitState &= ~TASK_WAIT_TIMER;  // Clear his "waiting on timer" flag.
      }
      else
      {
         break;   // Don't need to go any further.
      }
   }

   return;
}



//*****************************************************************************
//
// OSDelay() -- Delay a task for a period of time.
//
//    Description:
//         Delay a task for a period of time.  Input is in millisecs, but     
//         resolution is based on timer ticks.                                
//
//    Inputs:
//         Millisecs - Amount of time to delay by in milliseconds.  The actual
//                     resolution may not be in milliseconds, but in what ever
//                     resolution of timer ticks are.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK    - Task was successfully created and will get dispatched.
//              RC_NOMEM - No more memory available to allocate a timer EVENT
//                         structure.
//
//*****************************************************************************
RC OSDelay( U32 Millisecs)
{
   EVENT* event;

   _OS_INT_DISABLE();

   // See if free Event structure, or allocate new one...
   if ((event = EventFree) != NULL)
   {
      EventFree = event->EventNext;
      memset((void*)event, 0, sizeof(EVENT));
   }
   else
   {
      if((event = (EVENT*)calloc(1, sizeof(EVENT))) == NULL) 
      {
         return RC_NOMEM;
      }
   }

   // Calculate when event will expire...
   event->EventSeconds   = GlobalSeconds   + (Millisecs / 1000);
   event->EventMillisecs = GlobalMillisecs + (Millisecs % 1000);
   if(event->EventMillisecs > 1000) 
   {
      event->EventMillisecs -= 1000;
      event->EventSeconds++;
   }

   // Now, queue up timer event in ascending order onto chain...
   OSChainEvent(event);

   event->EventTask           = RunningTask;
   RunningTask->TaskState     = TASK_WAITING;      // Say task is waiting.
   RunningTask->TaskWaitState = TASK_WAIT_TIMER;   // Say waiting on timer event.
   RunningTask->TaskResource  = (void*) event;     // Helpful for debugging.      

   // OK, now find someone else to run...
   // _OS_INT_ENABLE();
   OSTaskDispatch();

   return;
}



//*****************************************************************************
//
// OSChainEvent() -- Chain timer event onto timer event queue.
//
//    Description:
//         Internal routine to queue up an event structure onto Event queue.  
//         Called with interrupts disabled.                                   
//
//    Inputs:
//         Event - Pointer to timer event structure.
//
//    Returns:
//         Nothing.
//
//*****************************************************************************
static void OSChainEvent( EVENT* Event )
{
   EVENT* p;
   EVENT* q;

   p = EventQueue;
   q = NULL;
   while(p) 
   {
      if((Event->EventSeconds <  p->EventSeconds) ||
         (Event->EventSeconds == p->EventSeconds  &&  
                              Event->EventMillisecs <= p->EventMillisecs))
      {
         break;
      }
      q = p;
      p = p->EventNext;
   }

   Event->EventNext = p;         // Chain next one after us (if there is one).
   if(q)                         // Is there one to chain us after?
   {
      q->EventNext = Event;      // Yes, so chain us after him.
   }
   else
   {
      EventQueue = Event;        // No, we are the first one in chain now.
   }

   return;
}



//*****************************************************************************
//
// OSUnchainEvent() -- Unchain timer event off of timer event queue.
//
//    Description:
//         Internal routine to remove an event structure off of the Event     
//         queue.  Called with interrupts disabled.                           
//
//    Inputs:
//         Event - Pointer to timer event structure.
//
//    Returns:
//         Nothing.
//
//*****************************************************************************
static void OSUnchainEvent( EVENT* Event )
{
   EVENT* p;
   EVENT* q;

   p = EventQueue;
   q = NULL;

   // Go thru chain looking for our guy...
   while(p != Event) 
   {
      q = p;
      p = p->EventNext;
   }

   // Now p is our guy (same as Event), and q is any element before our guy...
   if(p == Event) 
   {
      if(q)                                  // Is there a guy before us?
      {
         q->EventNext = p->EventNext;        // Yes, remove us after q.
      }
      else
      {
         EventQueue = p->EventNext;          // No, remove us at head of list.
      }
   }

   return;
}



//*****************************************************************************
//
// OSElapsedTimeGet() -- Return the current elapsed time.
//
//    Description:
//         This routine simply returns the current elapsed time from the
//         global seconds and global milliseconds variables.      
//
//    Inputs:
//         time - Pointer to an OS_TIME structure to hold seconds and millisecs.
//
//    Returns:
//         RC - the generic return code:
//              RC_OK - Time was successfully saved. Really, the only option.
//
//*****************************************************************************
RC  OSElapsedTimeGet(OS_TIME* time)
{
   _OS_INT_DISABLE();

   time->Seconds   = GlobalSeconds;
   time->Millisecs = GlobalMillisecs;

   _OS_INT_ENABLE();
}



//*****************************************************************************
//
// OSElapsedTimeAfter() -- Compare saved elapsed time against seconds/millisecs.
//
//    Description:
//         This routine can be used to test to see if we are at some time in the
//         future.  That is, given "time", which is a pointer to some time in 
//         the past (was set by calling OSElapsedTimeGet()), and given so many
//         seconds/milliseconds from that time, this function will return true
//
//    Inputs:
//         time      - Ptr to an OS_TIME structure to hold seconds and millisecs.
//         seconds/millisecs -  Added to "time" and then compared against the cur-
//                     ent global elapsed time to see if we are currently after
//                     that time.
//
//    Returns:
//         boolean:
//            true   - We are now after seconds/millisecs into the future from time.
//            false  - We have not yet gotten to seconds/millisecs into the future
//                     from time.
//
//*****************************************************************************
int OSElapsedTimeAfter(OS_TIME* time, U32 seconds, U16 millisecs)
{
   int   after = 0;

   _OS_INT_DISABLE();

   if ( (time->Seconds + seconds) < GlobalSeconds )
   {
      after = 1;
   }
   else
   {
      if ( (time->Seconds + seconds) == GlobalSeconds &&
           (time->Millisecs + millisecs) < GlobalMillisecs)
      {
         after = 1;
      }
   }

   _OS_INT_ENABLE();

   return after;
}
