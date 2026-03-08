/**
 * @file    lelu_scheduler.c
 * @brief   Lelu Scheduler - Implementation
 * @version 2.0.0
 * 
 * @details Implementation of the cooperative task scheduler for STM32.
 *          Based on a simple time-triggered architecture with round-robin
 *          task execution.
 * 
 *          v2.0.0: Debug output is now transport-agnostic via a user-supplied
 *          print callback. No more direct UART dependency.
 */

/* ==========================================================================
 * INCLUDES
 * ========================================================================== */

/* IMPORTANT: main.h must be included FIRST to pick up user's configuration
 * defines (LELU_MAX_TASKS, LELU_TICK_PERIOD_MS) before lelu_scheduler.h
 * applies its defaults via #ifndef guards. */
#include "main.h"           /* User's main.h - provides scheduler config defines */
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

/** @brief Overrun counter - incremented from ISR when tick fires before previous was processed */
static volatile uint32_t overrun_count = 0;

/** @brief User-provided print callback for debug output (NULL = disabled) */
static lelu_print_func_t debug_print_func = NULL;

/** @brief Buffer for formatting debug messages */
static char debug_msg[128];

/** @brief Timestamp before task execution (for profiling) */
static uint32_t time_before_task = 0;

/* ==========================================================================
 * PRIVATE FUNCTIONS
 * ========================================================================== */

/**
 * @brief  Send debug message via the user's print callback
 * @param  msg  Null-terminated string to send
 */
static void lelu_debug_print(const char* msg)
{
    if (debug_print_func != NULL)
    {
        debug_print_func(msg);
    }
}

/* ==========================================================================
 * PUBLIC FUNCTIONS - INITIALIZATION
 * ========================================================================== */

/**
 * @brief  Initialize the scheduler
 */
void lelu_scheduler_init(lelu_print_func_t print_func)
{
    debug_print_func = print_func;

    memset(tasks, 0, sizeof(tasks));

    task_count = 0;
    tick_counter = 0;
    total_ticks = 0;
    timer_flag = 0;
    boot_done = 0;
    overrun_count = 0;

    if (debug_print_func != NULL)
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

    if (debug_print_func != NULL)
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
    if (handler == NULL)
    {
        return LELU_ERROR_NULL_HANDLER;
    }

    if (task_count >= LELU_MAX_TASKS)
    {
        return LELU_ERROR_FULL;
    }

    uint8_t idx = task_count;

    tasks[idx].running = 1;
    tasks[idx].period = period;
    tasks[idx].elapsed_time = period;       /* Run immediately on first tick */
    tasks[idx].handler = handler;
    tasks[idx].total_ticks = 0;
    tasks[idx].run_count = 0;
    tasks[idx].last_exec_time = 0;
    tasks[idx].max_exec_time_100 = 0;
    tasks[idx].run_count_since_max_reset = 0;

    if (name != NULL)
    {
        strncpy(tasks[idx].name, name, LELU_TASK_NAME_LEN - 1);
        tasks[idx].name[LELU_TASK_NAME_LEN - 1] = '\0';
    }
    else
    {
        sprintf(tasks[idx].name, "task%d", idx);
    }

    if (task_id != NULL)
    {
        *task_id = idx;
    }

    task_count++;

    if (debug_print_func != NULL)
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
 * Increments counters and sets the timer_flag when a tick period elapses.
 * Overruns are counted (not printed) to keep ISR fast and safe.
 */
void lelu_scheduler_systick(void)
{
    if (!boot_done)
    {
        return;
    }

    tick_counter++;
    total_ticks++;

    if (tick_counter >= LELU_TICK_PERIOD_MS)
    {
        if (timer_flag)
        {
            /* Overrun: previous tick not yet processed. Count it; the user
             * can check via lelu_scheduler_get_overrun_count() or see it
             * in lelu_scheduler_print_stats(). */
            if (overrun_count < UINT32_MAX)
            {
                overrun_count++;
            }
        }
        else
        {
            timer_flag = 1;
        }

        tick_counter = 0;
    }
}

/**
 * @brief  Run one scheduler iteration
 * 
 * Iterates through all tasks and executes those whose period has elapsed.
 * Tasks are checked in registration order (first registered = highest priority).
 */
bool lelu_scheduler_run(void)
{
    bool task_executed = false;

    for (uint8_t i = 0; i < task_count; i++)
    {
        if ((tasks[i].elapsed_time >= tasks[i].period) && (tasks[i].running == 1))
        {
            time_before_task = total_ticks;

            tasks[i].handler();

            tasks[i].elapsed_time = 0;

            uint32_t exec_time = total_ticks - time_before_task;

            tasks[i].total_ticks += exec_time;
            tasks[i].run_count++;
            tasks[i].last_exec_time = exec_time;

            if (exec_time > tasks[i].max_exec_time_100)
            {
                tasks[i].max_exec_time_100 = exec_time;
            }
            tasks[i].run_count_since_max_reset++;
            if (tasks[i].run_count_since_max_reset >= 100)
            {
                tasks[i].run_count_since_max_reset = 0;
                tasks[i].max_exec_time_100 = exec_time;
            }

            task_executed = true;
        }

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
 * @brief  Format time value for display (ms -> human-readable)
 */
static void format_time(uint32_t ms, char* buf)
{
    if (ms < 10000)
    {
        sprintf(buf, "%lums", (unsigned long)ms);
    }
    else if (ms < 60000)
    {
        uint32_t secs = ms / 1000;
        uint32_t tenths = (ms % 1000) / 100;
        sprintf(buf, "%lu.%lus", (unsigned long)secs, (unsigned long)tenths);
    }
    else
    {
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
 * @brief  Print statistics for all tasks via the debug print callback
 */
void lelu_scheduler_print_stats(void)
{
    if (debug_print_func == NULL)
    {
        return;
    }

    char time_buf[12];

    format_time(total_ticks, time_buf);
    memset(debug_msg, 0, sizeof(debug_msg));
    sprintf(debug_msg, "\r\n[LELU] Task Statistics (uptime=%s, overruns=%lu)\r\n",
            time_buf, (unsigned long)overrun_count);
    lelu_debug_print(debug_msg);

    lelu_debug_print("Task          | Total    | Avg   | Max100 | Runs   | Status\r\n");
    lelu_debug_print("--------------+----------+-------+--------+--------+--------\r\n");

    for (uint8_t i = 0; i < task_count; i++)
    {
        uint32_t avg_time = 0;
        if (tasks[i].run_count > 0)
        {
            avg_time = tasks[i].total_ticks / tasks[i].run_count;
        }

        char total_buf[12];
        format_time(tasks[i].total_ticks, total_buf);

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

/**
 * @brief  Get number of tick overruns since boot
 */
uint32_t lelu_scheduler_get_overrun_count(void)
{
    return overrun_count;
}
