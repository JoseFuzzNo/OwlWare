#include <string.h>
#include "stm32f4xx.h"
#include "ProgramManager.h"
#include "SharedMemory.h"
#include "owlcontrol.h"

#include "FreeRTOS.h"
#include "task.h"

typedef void (*ProgramFunction)(void);

ProgramManager program;

TaskHandle_t xPatchHandle = NULL;
TaskHandle_t xManagerHandle = NULL;

#define START_PROGRAM_NOTIFICATION  0x01
#define STOP_PROGRAM_NOTIFICATION   0x02
#define RESET_PROGRAM_NOTIFICATION  0x04

extern "C" {
  void runPatchTask(void* p){
    program.runPatch();
  }
  void runManagerTask(void* p){
    program.runManager();
  }
}

void ProgramManager::startManager(){
  xTaskCreate(runManagerTask, "OWL Manager", configMINIMAL_STACK_SIZE, NULL, 1, &xManagerHandle);
}

bool doStopProgram = false;

void ProgramManager::start(){
  getSharedMemory()->status = AUDIO_IDLE_STATUS;
  // doRunProgram = true;
  uint32_t ulValue = START_PROGRAM_NOTIFICATION;
  BaseType_t xHigherPriorityTaskWoken = 0; 
  if(xManagerHandle != NULL)
    xTaskNotifyFromISR(xManagerHandle, ulValue, eSetBits, &xHigherPriorityTaskWoken );
#ifdef DEFINE_OWL_SYSTICK
  // vPortYield(); // can we call this from an interrupt?
  taskYIELD();
#endif /* DEFINE_OWL_SYSTICK */
}

void ProgramManager::exit(){
  getSharedMemory()->status = AUDIO_EXIT_STATUS; // request program exit
  // doStopProgram = true;
  uint32_t ulValue = STOP_PROGRAM_NOTIFICATION;
  BaseType_t xHigherPriorityTaskWoken = 0; 
  if(xManagerHandle != NULL)
    xTaskNotifyFromISR(xManagerHandle, ulValue, eSetBits, &xHigherPriorityTaskWoken );
#ifdef DEFINE_OWL_SYSTICK
  // vPortYield(); // can we call this from an interrupt?
  taskYIELD();
#endif /* DEFINE_OWL_SYSTICK */
}

// void ProgramManager::stop(){
//   // Use the handle to delete the task.
//   if(xHandle != NULL){
// #ifdef DEFINE_OWL_SYSTICK
//     vTaskDelete(xHandle);
// #endif /* DEFINE_OWL_SYSTICK */
//     xHandle = NULL;
//     // vPortYield();
//   }
//   running = false;
// }

/* exit and restart program */
void ProgramManager::reset(){
  uint32_t ulValue = STOP_PROGRAM_NOTIFICATION|RESET_PROGRAM_NOTIFICATION;
  BaseType_t xHigherPriorityTaskWoken = 0; 
  if(xManagerHandle != NULL)
    xTaskNotifyFromISR(xManagerHandle, ulValue, eSetBits, &xHigherPriorityTaskWoken );
  // exit();
  // doRestartProgram = true;
}

void ProgramManager::load(void* address, uint32_t length){
  programAddress = (uint8_t*)address;
  programLength = length;
  /* copy patch to ram */
  memcpy((void*)PATCHRAM, (void*)(programAddress), programLength);
}

bool ProgramManager::verify(){
  if(*(uint32_t*)programAddress != 0xDADAC0DE)
    return false;
  return true;
}

void ProgramManager::runPatch(){
  /* Jump to patch */
  /* Check Vector Table: Test if user code is programmed starting from address 
     "APPLICATION_ADDRESS" */
  volatile uint32_t* bin = (volatile uint32_t*)PATCHRAM;
  uint32_t sp = *(bin+1);
  if((sp & 0x2FFE0000) == 0x20000000){
    /* store Stack Pointer before jumping */
    // msp = __get_MSP();
    uint32_t jumpAddress = *(bin+2); // (volatile uint32_t*)(PATCHRAM + 8);
    ProgramFunction jumpToApplication = (ProgramFunction)jumpAddress;
    /* Initialize user application Stack Pointer */
    // __set_MSP(sp); // volatile uint32_t*)PATCHRAM);
    running = true;
    jumpToApplication();
    // where is our stack pointer now?
    /* reset Stack Pointer to pre-program state */
    // __set_MSP(msp);
    // program has returned
  }
  getSharedMemory()->status = AUDIO_IDLE_STATUS;
  running = false;
  vTaskSuspend(NULL); // suspend ourselves
}

void ProgramManager::runManager(){
  uint32_t ulNotifiedValue = 0;
  for(;;){

 /* Block indefinitely (without a timeout, so no need to check the function's
    return value) to wait for a notification.
    Bits in this RTOS task's notification value are set by the notifying
    tasks and interrupts to indicate which events have occurred. */
    xTaskNotifyWait(0x00,      /* Don't clear any notification bits on entry. */
		    UINT32_MAX, /* Reset the notification value to 0 on exit. */
		    &ulNotifiedValue, /* Notified value pass out in ulNotifiedValue. */
		    portMAX_DELAY );  /* Block indefinitely. */

    if(ulNotifiedValue & START_PROGRAM_NOTIFICATION) // start
      doRunProgram = true;
    if(ulNotifiedValue & STOP_PROGRAM_NOTIFICATION) // stop
      doStopProgram = true;
    if(ulNotifiedValue & 0x04) // restart
      doRestartProgram = true;
    // if(ulNotifiedValue & 0x08) // copy
    //   doCopyProgram = true;

    // if(doCopyProgram){
    //   doCopyProgram = false;
    //   /* copy patch to ram */
    //   memcpy((void*)PATCHRAM, (void*)(programAddress), programLength);
    // }
    if(doRunProgram){
      doRunProgram = false;
      if(xPatchHandle == NULL){
	xTaskCreate(runPatchTask, "OWL Patch", configMINIMAL_STACK_SIZE, NULL, 1, &xPatchHandle);
      }
    }
    if(doStopProgram){
      doStopProgram = false;
      // Use the handle to delete the task.
      if(xPatchHandle != NULL){
	vTaskDelete(xPatchHandle);
	xPatchHandle = NULL;
      }
      running = false;
      if(doRestartProgram){
	doRunProgram = true;
	doRestartProgram = false;
      }
    }
    // vTaskDelay(200/portTICK_PERIOD_MS); // delay some ms
  }
}

/*
  __set_CONTROL(0x3); // Switch to use Process Stack, unprivilegedstate
  __ISB(); // Execute ISB after changing CONTROL (architecturalrecommendation)
*/
