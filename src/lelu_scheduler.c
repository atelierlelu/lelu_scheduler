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

/* IMPORTANT: main.h must be included FIRST to pick up user's configuration
 * defines (LELU_MAX_TASKS, LELU_TICK_PERIOD_MS) before lelu_scheduler.h
 * applies its defaults via #ifndef guards. */
#include "main.h"           /* User's main.h - provides HAL types and scheduler config */
#include "../include/lelu_scheduler.h"
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
    tasks[idx].run_count = 0;
    tasks[idx].last_exec_time = 0;
    tasks[idx].max_exec_time_100 = 0;
    tasks[idx].run_count_since_max_reset = 0;
    
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
            
            /* Calculate execution time for this run */
            uint32_t exec_time = total_ticks - time_before_task;
            
            /* Update profiling statistics */
            tasks[i].total_ticks += exec_time;
            tasks[i].run_count++;
            tasks[i].last_exec_time = exec_time;
            
            /* Track max over last 100 executions */
            if (exec_time > tasks[i].max_exec_time_100)
            {
                tasks[i].max_exec_time_100 = exec_time;
            }
            tasks[i].run_count_since_max_reset++;
            if (tasks[i].run_count_since_max_reset >= 100)
            {
                /* Reset max tracking every 100 executions */
                tasks[i].run_count_since_max_reset = 0;
                tasks[i].max_exec_time_100 = exec_time;  /* Start fresh with current */
            }
            
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
 * @brief  Format time value for display
 * 
 * Formats milliseconds into human-readable string:
 * - < 10000ms: displays as "XXXXms"
 * - < 60000ms (60s): displays as "XX.Xs"
 * - >= 60000ms: displays as "Xm:XXs"
 * 
 * @param ms Time value in milliseconds
 * @param buf Output buffer (must be at least 12 chars)
 */
static void format_time(uint32_t ms, char* buf)
{
    if (ms < 10000)
    {
        /* Display as milliseconds */
        sprintf(buf, "%lums", (unsigned long)ms);
    }
    else if (ms < 60000)
    {
        /* Display as seconds with one decimal */
        uint32_t secs = ms / 1000;
        uint32_t tenths = (ms % 1000) / 100;
        sprintf(buf, "%lu.%lus", (unsigned long)secs, (unsigned long)tenths);
    }
    else
    {
        /* Display as minutes and seconds */
        uint32_t mins = ms / 60000;
        uint32_t secs = (ms % 60000) / 1000;
        sprintf(buf, "%lum%02lus", (unsigned long)mins, (unsigned long)secs);
    }
}

/**
 * @brief  Get statistics for a specific task
 */
void lelu_scheduler_get_stats(uint8_t task_id, lelu_task_stats_t* stats)
{
    if ((task_id < task_count) && (stats != NULL))
    {
        stats->total_ticks = tasks[task_id].total_ticks;
        stats->run_count = tasks[task_id].run_count;
    }
}

/**
 * @brief  Print statistics for all tasks via UART
 * 
 * Displays for each task:
 * - Total time spent in task (formatted)
 * - Average execution time per call
 * - Maximum execution time over last 100 calls
 * - Run count and status
 */
void lelu_scheduler_print_stats(void)
{
    if (debug_uart == NULL)
    {
        return;
    }
    
    char time_buf[12];  /* Buffer for formatted time strings */
    
    /* Print header with formatted total uptime */
    format_time(total_ticks, time_buf);
    memset(debug_msg, 0, sizeof(debug_msg));
    sprintf(debug_msg, "\r\n[LELU] Task Statistics (uptime=%s)\r\n", time_buf);
    lelu_debug_print(debug_msg);
    
    /* Print column headers */
    lelu_debug_print("Task          | Total    | Avg   | Max100 | Runs   | Status\r\n");
    lelu_debug_print("--------------+----------+-------+--------+--------+--------\r\n");
    
    /* Print each task */
    for (uint8_t i = 0; i < task_count; i++)
    {
        /* Calculate average execution time */
        uint32_t avg_time = 0;
        if (tasks[i].run_count > 0)
        {
            avg_time = tasks[i].total_ticks / tasks[i].run_count;
        }
        
        /* Format total time */
        char total_buf[12];
        format_time(tasks[i].total_ticks, total_buf);
        
        /* Print task line */
        memset(debug_msg, 0, sizeof(debug_msg));
        sprintf(debug_msg, "%-13s | %8s | %3lums | %4lums | %6lu | %s\r\n",
                tasks[i].name,
                total_buf,
                (unsigned long)avg_time,
                (unsigned long)tasks[i].max_exec_time_100,
                (unsigned long)tasks[i].run_count,
                tasks[i].running ? "RUN" : "STOP");
        lelu_debug_print(debug_msg);
    }
    
    /* Print separator */
    lelu_debug_print("--------------+----------+-------+--------+--------+--------\r\n");
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
