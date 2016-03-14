#include <string.h>
#include <stdlib.h>
#include "os.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#define F_CPU 16000000UL
#include <util/delay.h>

//Comment out the following line to remove debugging code from compiled version.
#define DEBUG

typedef void (*voidfuncptr) (void);      /* pointer to void f(void) */ 

#define WORKSPACE     256
#define MAXPROCESS   4

void debug_flash(){
  PORTB = 0x60;
  _delay_ms(500);
  PORTB = 0x00;
  _delay_ms(500);
}


/*===========
  * RTOS Internal
  *===========
  */

/**
  * This internal kernel function is the context switching mechanism.
  * It is done in a "funny" way in that it consists two halves: the top half
  * is called "Exit_Kernel()", and the bottom half is called "Enter_Kernel()".
  * When kernel calls this function, it starts the top half (i.e., exit). Right in
  * the middle, "Cp" is activated; as a result, Cp is running and the kernel is
  * suspended in the middle of this function. When Cp makes a system call,
  * it enters the kernel via the Enter_Kernel() software interrupt into
  * the middle of this function, where the kernel was suspended.
  * After executing the bottom half, the context of Cp is saved and the context
  * of the kernel is restore. Hence, when this function returns, kernel is active
  * again, but Cp is not running any more. 
  * (See file "switch.S" for details.)
  */
extern void CSwitch();
extern void Exit_Kernel();    /* this is the same as CSwitch() */

extern void a_main();

/* Prototype */
void Task_Terminate(void);
void enter_sleep_queue();

/** 
  * This external function could be implemented in two ways:
  *  1) as an external function call, which is called by Kernel API call stubs;
  *  2) as an inline macro which maps the call into a "software interrupt";
  *       as for the AVR processor, we could use the external interrupt feature,
  *       i.e., INT0 pin.
  *  Note: Interrupts are assumed to be disabled upon calling Enter_Kernel().
  *     This is the case if it is implemented by software interrupt. However,
  *     as an external function call, it must be done explicitly. When Enter_Kernel()
  *     returns, then interrupts will be re-enabled by Enter_Kernel().
  */ 
extern void Enter_Kernel();

#define Disable_Interrupt()		asm volatile ("cli"::)
#define Enable_Interrupt()		asm volatile ("sei"::)
  

/**
  *  This is the set of states that a task can be in at any given time.
  */
typedef enum process_states 
{ 
   DEAD = 0, 
   BLOCKED,
   READY, 
   RUNNING,
} PROCESS_STATES;

/**
  * This is the set of kernel requests, i.e., a request code for each system call.
  */
typedef enum kernel_request_type 
{
   NONE = 0,
   CREATE,
   NEXT,
   TERMINATE,
   SUSPEND,
   SLEEP,
   CREATE_MUTEX,
   LOCK_MUTEX,
   UNLOCK_MUTEX,
   CREATE_EVENT,
   EVENT_WAIT,
   EVENT_SIGNAL
} KERNEL_REQUEST_TYPE;

/**
  * Each task is represented by a process descriptor, which contains all
  * relevant information about this task. For convenience, we also store
  * the task's stack, i.e., its workspace, in here.
  */
typedef struct ProcessDescriptor 
{
   unsigned char *sp;   /* stack pointer into the "workSpace" */
   unsigned char workSpace[WORKSPACE]; 
   PROCESS_STATES state;
   TICK sleep_time;
  int sus;

   PID id;
   
   PRIORITY priority;
   int argument;

    MUTEX mid;
   EVENT e;

   PRIORITY create_priority;
  int create_argument;

   voidfuncptr  code;   /* function to be executed as a task */
   KERNEL_REQUEST_TYPE request;
} PD;


/**
  * This table contains ALL process descriptors. It doesn't matter what
  * state a task is in.
  */
static PD Process[MAXPROCESS];

/**
  * The process descriptor of the currently RUNNING task.
  */
volatile static PD* Cp; 

/** 
  * Since this is a "full-served" model, the kernel is executing using its own
  * stack. We can allocate a new workspace for this kernel stack, or we can
  * use the stack of the "main()" function, i.e., the initial C runtime stack.
  * (Note: This and the following stack pointers are used primarily by the
  *   context switching code, i.e., CSwitch(), which is written in assembly
  *   language.)
  */         
volatile unsigned char *KernelSp;

/**
  * This is a "shadow" copy of the stack pointer of "Cp", the currently
  * running task. During context switching, we need to save and restore
  * it into the appropriate process descriptor.
  */
volatile unsigned char *CurrentSp;

/** index to next task to run */
volatile static unsigned int NextP;  

/** index to next task id available*/
volatile static unsigned int FreeTaskId;  

/** 1 if kernel has been started; 0 otherwise. */
volatile static unsigned int KernelActive;  

/*
  * This internal kernel function is a part of the "scheduler". It chooses the 
  * next task to run, i.e., Cp.
  */
static void Dispatch(){
  PRIORITY high_priority = 10;

  int i;
  for(i = 0; i < MAXPROCESS; i++){
    if(Process[i].state == READY && Process[i].sus != 1 && Process[i].priority < high_priority){
      high_priority = Process[i].priority;
    }
  }

  while(Process[NextP].state != READY || Process[NextP].sus == 1 || Process[NextP].priority > high_priority) {
    NextP = (NextP + 1) % MAXPROCESS;
  }

  Cp = &(Process[NextP]);
  CurrentSp = Cp->sp;
  Cp->state = RUNNING;

  NextP = (NextP + 1) % MAXPROCESS;
}

/**
  *  This is the set of states that a Mutex can be in at any given time.
  */
typedef enum mutex_states { 
   LOCKED = 0, 
   UNLOCKED
} MUTEX_STATES;

/**
  * Each Mutex is represented by a mutex descriptor, which contains all
  * relevant information about this mutex.
  */
typedef struct MutexDescriptor {
    MUTEX_STATES state;   
    MUTEX id;
    PD* owner;
    PRIORITY original_priority;
    unsigned int lock_count;
    struct mutex_node *wait_queue;
} MD;

/* MUTEX CODE
 *
 *
 *
 *
 */

/* Add one for 1st indexed array  */
static MD Mutex[MAXMUTEX];

/** number of mutexes created so far */
volatile static unsigned int Mutexes;  

/**
  *  Node for blocked queue for mutex
  */
struct mutex_node{
  PD* pd;
  struct mutex_node *next;
};

/* Event structures */
typedef struct EventDescriptor {
  EVENT id;
  unsigned int signalled;
  PD *waiting_p;
} ED;

volatile static unsigned int Events;

/* add one for 1st indexed array use */
static ED Event[MAXEVENT];


void Kernel_Create_Mutex(PRIORITY priority){
  if(Mutexes >= MAXMUTEX) return; /* prevents creation of too many mutexes */

  Mutex[Mutexes].state = UNLOCKED;
  Mutex[Mutexes].id = Mutexes;
  Mutex[Mutexes].owner = NULL;
  Mutex[Mutexes].original_priority = priority;
  Mutex[Mutexes].lock_count = 0;
  Mutex[Mutexes].wait_queue = NULL;
};

void Kernel_Lock_Mutex(MUTEX mid, PD* Ct){
  if(Mutex[mid].id == MAXMUTEX || mid >= MAXMUTEX) {
    return;
  }

  if(Mutex[mid].state == UNLOCKED || Ct->id == Mutex[mid].owner->id){
    Ct->state = READY;
    Mutex[mid].owner = Ct;
    Mutex[mid].original_priority = Ct->priority;
    Mutex[mid].lock_count++;
    Mutex[mid].state = LOCKED;
  }else{
    Ct->state = BLOCKED;
    struct mutex_node *new_mutex_node = (struct mutex_node *) malloc(sizeof(struct mutex_node)); 
    new_mutex_node->pd = (PD*) Ct;
    new_mutex_node->next = Mutex[mid].wait_queue;
    Mutex[mid].wait_queue = new_mutex_node;
    
    //Priority Inheritence
    if(Ct->priority < Mutex[mid].owner->priority){
      Mutex[mid].owner->priority = Ct->priority; 
    }

    Dispatch();
  }
};

void Kernel_Unlock_Mutex(MUTEX mid, PD* Ct){
  if(Mutex[mid].id == MAXMUTEX || mid >= MAXMUTEX) {
    return;
  }

  if(Mutex[mid].state == LOCKED && Ct->id == Mutex[mid].owner->id){
    Mutex[mid].lock_count--;
    if(!Mutex[mid].lock_count){
      Mutex[mid].state = UNLOCKED;

      // Return inheirited priority
      Mutex[mid].owner->priority = Mutex[mid].original_priority;
      
      if(Mutex[mid].wait_queue != NULL){
        /* dequeue dead processes */
        while(Mutex[mid].wait_queue->pd->state == DEAD) {
          Mutex[mid].wait_queue = Mutex[mid].wait_queue->next;
        }
        PD* next_task = Mutex[mid].wait_queue->pd;
        
        Mutex[mid].wait_queue = Mutex[mid].wait_queue->next; 
        Kernel_Lock_Mutex(mid, next_task);
      }
    }
  }
};

/* Create event */
void Kernel_Create_Event() {
  if(Events >= MAXEVENT) return;

  Event[Events].id = Events;
  Event[Events].signalled = 0;
  Event[Events].waiting_p = NULL;
}

/* Leaves current task blocked on the event and calls the dispatcher if there are no other tasks waiting on it
* If the event has already been signalled or there is another task waiting on it the current task keeps running
*/
void Kernel_Event_Wait(PD* Ct) {
  unsigned int index = (unsigned int)(Ct->e);

  // If another task is waiting on the event NoOp
  if(Event[index].waiting_p != NULL) {
    Ct->state = RUNNING;

  // If waiting for event that has been signaled, reset signal and continue
  } else if(Event[index].signalled == 1) {
    Ct->state = RUNNING;
    Event[index].signalled = 0;

  // Else Block current task, associate task with event and dispatch next task
  }else{
    Cp->state = BLOCKED;    
    Event[index].waiting_p = Ct;
    Dispatch();
  }
}

/* Records that the event has been signalled
* If there is already a task waiting on that event this task is unblocked
*/
void Kernel_Event_Signal(EVENT e) {
  unsigned int index = (unsigned int)(e);

  Event[index].signalled = 1;
  if(Event[index].waiting_p != NULL) {
    //change waiting task's state to READY
    Event[index].waiting_p->state = READY;
    Event[index].waiting_p = NULL;
    Event[index].signalled = 0;
  }
}



/** number of tasks created so far */
volatile static unsigned int Tasks;  

/**
 * When creating a new task, it is important to initialize its stack just like
 * it has called "Enter_Kernel()"; so that when we switch to it later, we
 * can just restore its execution context on its stack.
 * (See file "cswitch.S" for details.)
 */
void Kernel_Create_Task_At( PD *p, void (*f)(void), PRIORITY py, int arg) 
{   
   unsigned char *sp;

#ifdef DEBUG
   int counter = 0;
#endif

   //Changed -2 to -1 to fix off by one error.
   sp = (unsigned char *) &(p->workSpace[WORKSPACE-1]);



   /*----BEGIN of NEW CODE----*/
   //Initialize the workspace (i.e., stack) and PD here!

   //Clear the contents of the workspace
   memset(&(p->workSpace),0,WORKSPACE);

   //Notice that we are placing the address (16-bit) of the functions
   //onto the stack in reverse byte order (least significant first, followed
   //by most significant).  This is because the "return" assembly instructions 
   //(rtn and rti) pop addresses off in BIG ENDIAN (most sig. first, least sig. 
   //second), even though the AT90 is LITTLE ENDIAN machine.

   //Store terminate at the bottom of stack to protect against stack underrun.
   *(unsigned char *)sp-- = ((unsigned int)Task_Terminate) & 0xff;
   *(unsigned char *)sp-- = (((unsigned int)Task_Terminate) >> 8) & 0xff;
   *(unsigned char *)sp-- = 0x00;
   
   //Place return address of function at bottom of stack
   *(unsigned char *)sp-- = ((unsigned int)f) & 0xff;
   *(unsigned char *)sp-- = (((unsigned int)f) >> 8) & 0xff;
   *(unsigned char *)sp-- = 0x00;

#ifdef DEBUG
   //Fill stack with initial values for development debugging
   //Registers 0 -> 31 and the status register
   for (counter = 0; counter < 34; counter++)
   {
      *(unsigned char *)sp-- = counter;
   }
#else
   //Place stack pointer at top of stack
   sp = sp - 34;
#endif
      
  p->sp = sp;    /* stack pointer into the "workSpace" */
  p->code = f;   /* function to be executed as a task */
  p->request = NONE;
  p->id = (PID) FreeTaskId;
  p->argument = arg;
  p->priority = py;


   p->state = READY;

}


/**
  *  Create a new task
  */
static void Kernel_Create_Task(void (*f)(void), PRIORITY py, int arg) {
   if (Tasks == MAXPROCESS) return;  /* Too many tasks! */

   /* find a DEAD PD that we can use*/
   for(FreeTaskId = 0; FreeTaskId< MAXPROCESS; FreeTaskId++) {
       if(Process[FreeTaskId].state == DEAD) break;
   }

   Kernel_Create_Task_At(&(Process[FreeTaskId]), f, py, arg);
}



/**
  * This internal kernel function is the "main" driving loop of this full-served
  * model architecture. Basically, on OS_Start(), the kernel repeatedly
  * requests the next user task's next system call and then invokes the
  * corresponding kernel function on its behalf.
  *
  * This is the main loop of our kernel, called by OS_Start().
  */
static void Next_Kernel_Request() {
  Dispatch();  /* select a new task to run */

  while(1) {

    Cp->request = NONE; /* clear its request */

    /* activate this newly selected task */
    CurrentSp = Cp->sp;
    Exit_Kernel();    /* or CSwitch() */

    /* if this task makes a system call, it will return to here! */

    /* save the Cp's stack pointer */
    Cp->sp = (unsigned char *) CurrentSp;

    switch(Cp->request){
      case CREATE:
        Kernel_Create_Task(Cp->code, Cp->create_priority, Cp->create_argument);
        break;
      case SUSPEND:
        Cp->sus = 1;
        if(Cp->state == RUNNING){
          Cp->state = READY;
        }
        Dispatch();
        break;
      case SLEEP:
        Cp->sus = 1;
        if(Cp->state == RUNNING){
          Cp->state = READY;
        }
        enter_sleep_queue();
        Dispatch();
      break;
      case NEXT:
      case NONE:
      /* NONE could be caused by a timer interrupt */
        Cp->state = READY;
        Dispatch();
        break;
      case TERMINATE:
        /* deallocate all resources used by this task */
        --Tasks;
        Cp->state = DEAD;
        Dispatch();
        break;
      case CREATE_MUTEX:
        Kernel_Create_Mutex(Cp->priority);
        break;
      case LOCK_MUTEX:
        Kernel_Lock_Mutex(Cp->mid, (PD*) Cp);
        break;
      case UNLOCK_MUTEX:
        Kernel_Unlock_Mutex(Cp->mid, (PD*) Cp);
        break;
      case CREATE_EVENT:
        Kernel_Create_Event();
        break;
      case EVENT_WAIT:
        Kernel_Event_Wait((PD*) Cp);
        break;
      case EVENT_SIGNAL:
        Kernel_Event_Signal(Cp->e);
        break;
      default:
        /* Houston! we have a problem here! */
        break;
    }
  } 
}



/*================
  * RTOS  API  and Stubs
  *================
  */

void OS_Abort(void);

/*================
  *  FUNCTIONS
  *================
  */

/**
  * Create task with given priority and argument, return the process ID
  */
PID Task_Create( void (*f)(void), PRIORITY py, int arg){

  if(KernelActive){
    Disable_Interrupt();
    Cp->request = CREATE;
    Cp->code = f;
    Cp->create_argument = arg;
    Cp->create_priority = py;
    Cp->sus = 0;
    Enter_Kernel();
  } else { 
    Kernel_Create_Task(f, py, arg);
  }

  return FreeTaskId;
}

/**
  * The calling task terminates itself.
  */
void Task_Terminate(void) {
  if (KernelActive) {
    Disable_Interrupt();
    Cp -> request = TERMINATE;
    Enter_Kernel();
    /* never returns here! */
  }
}

void Task_Yield(void){
  if (KernelActive) {
    Disable_Interrupt();
    Cp ->request = NEXT;
    Enter_Kernel();
    Enable_Interrupt();
  }
};

int Task_GetArg(void){
  return Cp->argument;
};

/**
* Suspends a task until it's handle is passed to resume
* Does nothing if the task is already suspended
*/
void Task_Suspend( PID p ){

  /* if the task to be suspended is currently running, then let the kernel get a new task running */
  if(Cp->id == p && KernelActive) {
      Disable_Interrupt();
      Cp->request = SUSPEND;
      Enter_Kernel();
      Enable_Interrupt();
  } else {
    int x;
    for(x=0; x < MAXPROCESS; x++) {
      if(Process[x].id == p) {
        Process[x].sus = 1;
        break;
      }
    }
  }
};

/**
* Resumes a suspended task
* Does nothing if the task isn't suspended
*/
void Task_Resume( PID p ){
  int x;
  for(x=0; x < MAXPROCESS; x++) {
    if(Process[x].id == p) {
      Process[x].sus = 0;
      break;
    }
  }
};

/* Sleep code
 *
 */

struct sleep_node {
  PD* pd;
  int sleep_expected_count;
  int sleep_actual_count;
  struct sleep_node *next;
};

struct sleep_node *sleep_queue_head = NULL;

void sleep_delete(PID id){

  struct sleep_node *temp, *prev;
  
  temp = sleep_queue_head;
  
  while(temp!=NULL){
    if(temp->pd->id == id){
      if(temp==sleep_queue_head){
        sleep_queue_head=temp->next;
        free(temp);
      }else{
        prev->next=temp->next;
        free(temp);
      }
    }else{
      prev = temp;
      temp = temp->next;
    }
  }
}

void Task_Sleep(TICK t){
  Disable_Interrupt();
  Cp->request = SLEEP;
  Cp->sleep_time = t;
  Enter_Kernel();
  Enable_Interrupt();
};

ISR(TIMER1_COMPA_vect){
  struct sleep_node *curr_sleep_node = sleep_queue_head;
  struct sleep_node *next_sleep_node = curr_sleep_node->next;
  while(curr_sleep_node != NULL){
    curr_sleep_node->sleep_actual_count++;
    next_sleep_node = curr_sleep_node->next;

    if(curr_sleep_node->sleep_actual_count >= curr_sleep_node->sleep_expected_count){
      Task_Resume(curr_sleep_node->pd->id);
      sleep_delete(curr_sleep_node->pd->id);
    }

    curr_sleep_node = next_sleep_node;
  }

  if(sleep_queue_head == NULL){
    TIMSK1  = 0;
  }else{
    TCNT1   = 0;
  }
}

void enter_sleep_queue(){

  struct sleep_node *new_sleep_node = (struct sleep_node *) malloc(sizeof(struct sleep_node)); 

  new_sleep_node->pd = (PD*) Cp;
  new_sleep_node->next = sleep_queue_head;
  new_sleep_node->sleep_actual_count = 0;
  new_sleep_node->sleep_expected_count = (int)(Cp->sleep_time/MSECPERTICK); 
  sleep_queue_head = new_sleep_node;

  // Control registers
  TCCR1A  = 0;

  // Configure timer 1 for CTC mode
  TCCR1B  |= (1<<WGM12); 
  
  // start timer
  TIMSK1   |= (1<<OCIE1A);

  //Output compare register, interrupt every 1 millisecond
  OCR1A   = 625;

  // Configure with 256 prescaler
  TCCR1B  |= (1<<CS12);

  //Start count to 0
  TCNT1   = 0;
}

/*================
  * MUTEX FUNCTIONS
  *================
  */



MUTEX Mutex_Init(void){
  if(KernelActive){
    Disable_Interrupt();
    Cp->request = CREATE_MUTEX;
    Enter_Kernel();
    return Mutexes++;
  }else{
    Kernel_Create_Mutex(10);
    return Mutexes++;
  }
};

void Mutex_Lock(MUTEX m){
  Disable_Interrupt();
  Cp->request = LOCK_MUTEX;
  Cp->mid = m;
  Enter_Kernel();
};

void Mutex_Unlock(MUTEX m){
  Disable_Interrupt();
  Cp->request = UNLOCK_MUTEX;
  Cp->mid = m;
  Enter_Kernel();
};



/*================
  * EVENT FUNCTIONS
  *================
  */

/* Sends a request to the kernel for the creation of an event
* Returns event id
*/
EVENT Event_Init(void){
  if(KernelActive) {
    Disable_Interrupt();
    Cp->request = CREATE_EVENT;
    Enter_Kernel();
    return (EVENT)Events++;
  } else {
    Kernel_Create_Event();
    return (EVENT)Events++;
  }
};

/* If event is waited on make EVENT_WAIT kernel request with the event id
*
*/
void Event_Wait(EVENT e){
  if(e == MAXEVENT) return;
  Disable_Interrupt();
  Cp->e = e;
  Cp->request = EVENT_WAIT;
  Enter_Kernel();
};

/* When an event is signalled, it wakes up a waiting task if there is one, otherwise it is recorded
*only one outstanding signal on an event is recorded, hence any subsequent signals on the same event will be lost
*/
void Event_Signal(EVENT e){
  if(e == MAXEVENT) return;
  Disable_Interrupt();
  Cp->e = e;
  Cp->request = EVENT_SIGNAL;
  Enter_Kernel();
};


/**
* This function initializes the RTOS and must be called before any other
* system calls.
*/
void main() {
  int x;
  Tasks = 0;
  Mutexes = 0;
  Events = 0;
  KernelActive = 0;
  NextP = 0;
  DDRB = 0xF0;

  //Reminder: Clear the memory for the task on creation.
  for (x = 0; x < MAXPROCESS; x++) {
    memset(&(Process[x]),0,sizeof(PD));
    Process[x].state = DEAD;
  }

  //Reminder: Clear the memory for the mutexes on creation.
  for (x = 0; x < MAXMUTEX; x++) {
    memset(&(Mutex[x]),0,sizeof(MD));
    Mutex[x].state = UNLOCKED;
    Mutex[x].id = MAXMUTEX;
  }

  //Reminder: Clear the memory for the events on creation.
  for (x = 0; x < MAXEVENT; x++) {
    memset(&(Event[x]),0,sizeof(ED));
    Event[x].id = MAXEVENT;
  }

  Task_Create(a_main, 0, 0);

  Disable_Interrupt();
  KernelActive = 1;
  Next_Kernel_Request();
}
