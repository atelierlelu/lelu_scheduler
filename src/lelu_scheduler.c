/**
 * @file    lelu_scheduler.c
 * @brief   Lelu Scheduler - Implementation
 * @version 1.0.0
 * 
 * @details Implementation of the cooperative task scheduler for STM32.
 *          Based on a simple time-triggered architecture with round-robin
 *          task execution.
 */

/* ==========================================================================
 * INCLUDES
 * ========================================================================== */

#include "../include/lelu_scheduler.h"
#include "main.h"           /* User's main.h - provides HAL types (UART_HandleTypeDef, etc.) */
#include <string.h>
#include <stdio.h>

/* ==========================================================================
 * PRIVATE VARIABLES
 * ========================================================================== */

/** @brief Array holding all registered tasks */
static lelu_task_t tasks[LELU_MAX_TASKS];

/** @brief Number of currently registered tasks */
static uint8_t task_count = 0;

/** @brief Internal tick counter (0 to LELU_TICK_PERIOD_MS-1) */
static volatile uint32_t tick_counter = 0;

/** @brief Total ticks elapsed since boot (monotonic, in milliseconds) */
static volatile uint32_t total_ticks = 0;

/** @brief Timer flag - set when a tick period has elapsed */
static volatile uint8_t timer_flag = 0;

/** @brief Boot done flag - enables overrun detection when set */
static uint8_t boot_done = 0;

/** @brief UART handle for debug output (NULL = disabled) */
static UART_HandleTypeDef* debug_uart = NULL;

/** @brief Buffer for debug messages */
static char debug_msg[128];

/** @brief Timestamp before task execution (for profiling) */
static uint32_t time_before_task = 0;

/* ==========================================================================
 * PRIVATE FUNCTIONS
 * ========================================================================== */

/**
 * @brief  Send debug message via UART
 * @param  msg  Null-terminated string to send
 */
static void lelu_debug_print(const char* msg)
{
    if (debug_uart != NULL)
    {
        HAL_UART_Transmit(debug_uart, (uint8_t*)msg, strlen(msg), 100);
    }
}

/* ==========================================================================
 * PUBLIC FUNCTIONS - INITIALIZATION
 * ========================================================================== */

/**
 * @brief  Initialize the scheduler
 */
void lelu_scheduler_init(void* uart_handle)
{
    /* Store UART handle for debug output */
    debug_uart = (UART_HandleTypeDef*)uart_handle;
    
    /* Clear task array */
    memset(tasks, 0, sizeof(tasks));
    
    /* Reset counters */
    task_count = 0;
    tick_counter = 0;
    total_ticks = 0;
    timer_flag = 0;
    boot_done = 0;
    
    /* Print init message if UART available */
    if (debug_uart != NULL)
    {
        memset(debug_msg, 0, sizeof(debug_msg));
        sprintf(debug_msg, "[LELU] Scheduler initialized (max %d tasks, %dms tick)\r\n",
                LELU_MAX_TASKS, LELU_TICK_PERIOD_MS);
        lelu_debug_print(debug_msg);
    }
}

/**
 * @brief  Mark boot sequence as complete (enables overrun detection)
 */
void lelu_scheduler_set_boot_done(void)
{
    boot_done = 1;
    
    if (debug_uart != NULL)
    {
        memset(debug_msg, 0, sizeof(debug_msg));
        sprintf(debug_msg, "[LELU] Boot done, scheduler active with %d tasks\r\n", task_count);
        lelu_debug_print(debug_msg);
    }
}

/* ==========================================================================
 * PUBLIC FUNCTIONS - TASK MANAGEMENT
 * ========================================================================== */

/**
 * @brief  Add a new task to the scheduler
 */
lelu_status_t lelu_scheduler_add_task(const char* name,
                                       lelu_task_func_t handler,
                                       uint32_t period,
                                       uint8_t* task_id)
{
    /* Validate handler */
    if (handler == NULL)
    {
        return LELU_ERROR_NULL_HANDLER;
    }
    
    /* Check if task array is full */
    if (task_count >= LELU_MAX_TASKS)
    {
        return LELU_ERROR_FULL;
    }
    
    /* Get next available slot */
    uint8_t idx = task_count;
    
    /* Initialize task */
    tasks[idx].running = 1;                          /* Start in running state */
    tasks[idx].period = period;
    tasks[idx].elapsed_time = period;                /* Run immediately on first tick */
    tasks[idx].handler = handler;
    tasks[idx].total_ticks = 0;
    
    /* Copy task name (safely truncate if too long) */
    if (name != NULL)
    {
        strncpy(tasks[idx].name, name, LELU_TASK_NAME_LEN - 1);
        tasks[idx].name[LELU_TASK_NAME_LEN - 1] = '\0';
    }
    else
    {
        sprintf(tasks[idx].name, "task%d", idx);
    }
    
    /* Return task ID if requested */
    if (task_id != NULL)
    {
        *task_id = idx;
    }
    
    /* Increment task count */
    task_count++;
    
    /* Debug output */
    if (debug_uart != NULL)
    {
        memset(debug_msg, 0, sizeof(debug_msg));
        sprintf(debug_msg, "[LELU] Added task '%s' (id=%d, period=%lums)\r\n",
                tasks[idx].name, idx, (unsigned long)period);
        lelu_debug_print(debug_msg);
    }
    
    return LELU_OK;
}

/**
 * @brief  Start (enable) a task
 */
void lelu_scheduler_start_task(uint8_t task_id)
{
    if (task_id < task_count)
    {
        tasks[task_id].running = 1;
    }
}

/**
 * @brief  Stop (disable) a task
 */
void lelu_scheduler_stop_task(uint8_t task_id)
{
    if (task_id < task_count)
    {
        tasks[task_id].running = 0;
    }
}

/* ==========================================================================
 * PUBLIC FUNCTIONS - SCHEDULER EXECUTION
 * ========================================================================== */

/**
 * @brief  SysTick handler - call every 1ms from HAL_IncTick() or timer ISR
 * 
 * This function implements the time-keeping logic for the scheduler.
 * It increments counters and sets the timer_flag when a tick period elapses.
 * 
 * Overrun detection: If timer_flag is already set when we try to set it again,
 * it means the main loop didn't process the previous tick in time.
 */
void lelu_scheduler_systick(void)
{
    /* Don't process during boot */
    if (!boot_done)
    {
        return;
    }
    
    /* Increment tick counters */
    tick_counter++;
    total_ticks++;
    
    /* Check if a tick period has elapsed */
    if (tick_counter >= LELU_TICK_PERIOD_MS)
    {
        /* Check for overrun (previous tick not yet processed) */
        if (timer_flag)
        {
            /* Overrun detected! Tasks took too long. */
            /* Note: We're in ISR context, so we just output a short message */
            if (debug_uart != NULL)
            {
                /* Direct transmit - minimal message to avoid ISR delays */
                const char* overrun_msg = "OR-\r\n";
                HAL_UART_Transmit(debug_uart, (uint8_t*)overrun_msg, 5, 10);
            }
        }
        else
        {
            /* Set flag to signal main loop */
            timer_flag = 1;
        }
        
        /* Reset tick counter */
        tick_counter = 0;
    }
}

/**
 * @brief  Run one scheduler iteration
 * 
 * This is the heart of the scheduler. It iterates through all tasks and
 * executes those whose period has elapsed. Tasks are checked in order of
 * registration (first registered = highest priority).
 */
bool lelu_scheduler_run(void)
{
    bool task_executed = false;
    
    /* Iterate through all registered tasks */
    for (uint8_t i = 0; i < task_count; i++)
    {
        /* Check if task is ready to run:
         * - elapsed_time >= period: enough time has passed
         * - running == 1: task is enabled
         */
        if ((tasks[i].elapsed_time >= tasks[i].period) && (tasks[i].running == 1))
        {
            /* Record start time for profiling */
            time_before_task = total_ticks;
            
            /* Execute the task */
            tasks[i].handler();
            
            /* Reset elapsed time */
            tasks[i].elapsed_time = 0;
            
            /* Update profiling: add execution time to total */
            tasks[i].total_ticks += (total_ticks - time_before_task);
            
            task_executed = true;
        }
        
        /* Always increment elapsed time by the tick period */
        tasks[i].elapsed_time += LELU_TICK_PERIOD_MS;
    }
    
    return task_executed;
}

/**
 * @brief  Check if a tick period has elapsed
 */
bool lelu_scheduler_tick_pending(void)
{
    return (timer_flag != 0);
}

/**
 * @brief  Clear the pending tick flag
 */
void lelu_scheduler_clear_tick(void)
{
    timer_flag = 0;
}

/* ==========================================================================
 * PUBLIC FUNCTIONS - STATISTICS AND DEBUGGING
 * ========================================================================== */

/**
 * @brief  Get statistics for a specific task
 */
void lelu_scheduler_get_stats(uint8_t task_id, lelu_task_stats_t* stats)
{
    if ((task_id < task_count) && (stats != NULL))
    {
        stats->total_ticks = tasks[task_id].total_ticks;
        stats->run_count = 0;  /* Reserved for future use */
    }
}

/**
 * @brief  Print statistics for all tasks via UART
 */
void lelu_scheduler_print_stats(void)
{
    if (debug_uart == NULL)
    {
        return;
    }
    
    /* Print header */
    memset(debug_msg, 0, sizeof(debug_msg));
    sprintf(debug_msg, "\r\n[LELU] Task Statistics (total_ticks=%lu)\r\n",
            (unsigned long)total_ticks);
    lelu_debug_print(debug_msg);
    
    /* Print separator */
    lelu_debug_print("----------------------------------------\r\n");
    
    /* Print each task */
    for (uint8_t i = 0; i < task_count; i++)
    {
        memset(debug_msg, 0, sizeof(debug_msg));
        sprintf(debug_msg, "  %s:\ttotal=%lums\tperiod=%lums\t%s\r\n",
                tasks[i].name,
                (unsigned long)tasks[i].total_ticks,
                (unsigned long)tasks[i].period,
                tasks[i].running ? "RUNNING" : "STOPPED");
        lelu_debug_print(debug_msg);
    }
    
    /* Print separator */
    lelu_debug_print("----------------------------------------\r\n");
}

/**
 * @brief  Get total elapsed ticks since scheduler started
 */
uint32_t lelu_scheduler_get_total_ticks(void)
{
    return total_ticks;
}

/**
 * @brief  Get number of registered tasks
 */
uint8_t lelu_scheduler_get_task_count(void)
{
    return task_count;
}
