//*****************************************************************************
//
//                  my_os.h - My little embedded OS 
//                Copyright (c) 2002, John C. Overton
// 
//   Description:
//      This module is a small embedded os for small applications.
//
//*****************************************************************************
#ifndef __MY_OS_H__
#define __MY_OS_H__


#include <setjmp.h>

typedef unsigned char  U8;
typedef unsigned short U16;
typedef unsigned long  U32;
typedef void*          PTR;
#if !defined(NULL)
#define NULL           (0)
#endif

#define DEFAULT_STACK_SIZE  256;

typedef void (*fn)(U32);


//*****************************************************************************
// The task structure.
//*****************************************************************************
typedef struct _task
{
   U32            TaskNbr;             // A way to distinguish tasks.
   U16            TaskState;           // Current state of the task.
   U16            TaskWaitState;       // We may also be waiting on something else (timer).
   struct _task*  TaskChain;           // Chain of all allocated tasks.
   struct _task*  TaskWait;            // Chain of waiting tasks if waiting.
   PTR            TaskStackBase;       // Base of stack for this task.
   fn             TaskStart;           // The first function that task starts at.
   U32            TaskStartParam;      // An optional parameter passed to starting function.
   char*          TaskName;            // Name of task (null terminated).
   void*          TaskResource;        // Debug, maybe Semaphore, or Timer Event.
   jmp_buf        TaskContext;         // Task context.

} TASK;


//*****************************************************************************
// Possible values for TaskState in Task structure...
//*****************************************************************************
#define TASK_NOTACTIVE 0               // This TASK structure is not being used.
#define TASK_ZOMBIE    1               // Need to clean up this task (free resources).
#define TASK_READY     2               // Task is ready to be dispatched.
#define TASK_RUNNING   3               // Task is currently running.
#define TASK_WAITING   4               // Task is currently waiting. See TaskWaitState.


//*****************************************************************************
// Possible bit values for TaskWaitState in Task structure...
//*****************************************************************************
#define TASK_WAIT_NONE       0x00      // Not waiting for anything else.
#define TASK_WAIT_SEMAPHORE  0x01      // Waiting on semaphore post.
#define TASK_WAIT_TIMER      0x02      // Waiting for timer event to expire.
//efine TASK_WAIT_?          0x04      //


//*****************************************************************************
// The Semaphore structure defines a Semaphore.  Set Count to number of 
// resources to control (usually, just one).  You can use OS_INIT_SEMAPHORE() to
// initialize the semaphore (if you want).
//*****************************************************************************
typedef struct 
{
   int    SemCount;                    // Semaphore count if < 0, then waiting tasks.
   TASK*  SemWait;                     // Queue of waiting tasks;
   TASK*  SemWaitEnd;                  // End of queue of waiting tasks;
} SEMAPHORE;

#define OS_INIT_SEMAPHORE(semaphore, count)                 \
          {                                                 \
          memset(((void*) semaphore), 0, sizeof(SEMAPHORE));  \
          (semaphore)->SemCount = count;                         \
          }


//*****************************************************************************
// The Event structure used for timed events...
//*****************************************************************************
typedef struct _event
{
   struct _event* EventNext;           // Chain of events (free cache or timer queue).
   U32            EventSeconds;        // Point at which to timeout.
   U16            EventMillisecs;      // Ditto.
   TASK*          EventTask;           // Task that is waiting for this event to expire.
} EVENT;



//*****************************************************************************
// Return codes from OS functions...
//*****************************************************************************
typedef int RC;
enum 
{
   RC_OK      = 0,                     // Function call executed just fine.
   RC_NOMEM   = 1,                     // No more memory available.
   RC_TIMEOUT = 2,                     // Call to OSSemWaitTimeout() timed out.
   RC_LAST                             // Just a place-marker at the end of the list.
};


//*****************************************************************************
// The elapsed time structure that is used to pass or return current elapsed
// time by the elapsed time functions...
//*****************************************************************************
typedef struct 
{
   U32    Seconds;                     // The seconds portion of the elapsed time.
   U16    Millisecs;                   // The milliseconds portion of the elapsed time.
} OS_TIME;


//*****************************************************************************
// Global variables...
//*****************************************************************************
extern TASK*  ActiveTasks;             // Chain of all created tasks.
extern TASK*  RunningTask;             // TASK structure of currently running task.
extern TASK*  FreeTaskStructs;         // Cache free task structures.
extern U32    LastAllocatedTaskNbr;    // Last assigned task number.
extern EVENT* EventQueue;              // Event queue of timer events.
extern EVENT* EventFree;               // Cache free event structures.
extern U32    GlobalSeconds;           // Elapsed time seconds.
extern U16    GlobalMillisecs;         // Elapsed time milliseconds of seconds.



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
void OSNewTaskStub( void );


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
RC OSTaskCreate(char* Name, PTR Function, U32 Parameter, U32 StackSize); 


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
RC OSTaskDestroy( void );


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
RC OSTaskDispatch( void );


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
RC OSSemPost(SEMAPHORE* sem);


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
U32 OSSemPoll(SEMAPHORE* sem);


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
RC OSSemWait(SEMAPHORE* sem);


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
//         Nothing.
//         RC - the generic return code:
//              RC_OK - Task was successfully created and will get dispatched.
//              RC_NOMEM   - No memory available to allocate timer event structure.
//              RC_TIMEOUT - The timeout value has expired before the semaphore
//                           was aquired.
//
//*****************************************************************************
RC OSSemWaitTimeout(SEMAPHORE* sem,  U32 Millisecs);


//*****************************************************************************
//
// OSTickHandler() -- Tick interrupt routine
//
//    Description:
//         Called from a Tick interrupt routine at the rate of once every     
//         1/100th of a second (every .01 sec).  On entry, interrupts should be 
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
void OSTickHandler( void );


//*****************************************************************************
//
// OSDelay() -- Delay a task for a period of time.
//
//    Description:
//         Delay a task for a period of time.  Input is in millisecs, but     
//         resolution is based on timer ticks.                                
//
//    Inputs:
//         Nothing.
//
//    Returns:
//         Nothing.
//         RC - the generic return code:
//              RC_OK    - Task was successfully created and will get dispatched.
//              RC_NOMEM - No more memory available to allocate a timer EVENT
//                         structure.
//
//*****************************************************************************
RC OSDelay( U32 Millisecs);


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
static void OSChainEvent( EVENT* Event );


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
static void OSUnchainEvent( EVENT* Event );


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
RC  OSElapsedTimeGet(OS_TIME* time);


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
int OSElapsedTimeAfter(OS_TIME* time, U32 seconds, U16 millisecs);


#endif /* __MY_OS_H__ */
