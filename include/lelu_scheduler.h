/**
 * @file    lelu_scheduler.h
 * @brief   Lelu Scheduler - Simple cooperative task scheduler for STM32
 * @version 2.0.0
 * 
 * @details A lightweight, cooperative (non-preemptive) task scheduler designed
 *          for bare-metal STM32 applications. Tasks are executed based on their
 *          configured periods, with priority determined by registration order
 *          (first registered = highest priority).
 * 
 *          Debug output is transport-agnostic: pass any print function (UART,
 *          USB CDC, SWO, RTT, ...) or NULL to disable.
 * 
 * @note    Compatible with STM32 HAL (F4, G0, and other families)
 * 
 * @example See README.md for usage examples
 */

#ifndef LELU_SCHEDULER_H
#define LELU_SCHEDULER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * INCLUDES
 * ========================================================================== */

#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
 * CONFIGURATION DEFINES
 * 
 * These can be overridden by defining them before including this header,
 * or in your project's compiler settings (e.g. in main.h).
 * ========================================================================== */

/**
 * @def LELU_MAX_TASKS
 * @brief Maximum number of tasks that can be registered
 * 
 * This determines the size of the static task array. Increase if you need
 * more tasks, but be aware of RAM usage (each task uses ~32 bytes).
 */
#ifndef LELU_MAX_TASKS
#define LELU_MAX_TASKS          8
#endif

/**
 * @def LELU_TASK_NAME_LEN
 * @brief Maximum length of task name string (including null terminator)
 * 
 * Task names are used for debug output. Longer names use more RAM per task.
 */
#ifndef LELU_TASK_NAME_LEN
#define LELU_TASK_NAME_LEN      20
#endif

/**
 * @def LELU_TICK_PERIOD_MS
 * @brief Base scheduler tick period in milliseconds
 * 
 * This is the fundamental time unit of the scheduler. All task periods should
 * be multiples of this value for accurate timing. The scheduler checks for
 * ready tasks every LELU_TICK_PERIOD_MS milliseconds.
 * 
 * Recommended: Use the GCD (Greatest Common Divisor) of all your task periods.
 * Example: If tasks run at 100ms and 250ms, use 50ms (GCD of 100 and 250).
 * 
 * Lower values = more responsive but higher CPU overhead
 * Higher values = less overhead but coarser timing resolution
 */
#ifndef LELU_TICK_PERIOD_MS
#define LELU_TICK_PERIOD_MS     25
#endif

/* ==========================================================================
 * TYPE DEFINITIONS
 * ========================================================================== */

/**
 * @brief Function pointer type for task handlers
 * 
 * Task functions take no arguments and return nothing. They should complete
 * quickly (non-blocking) since this is a cooperative scheduler.
 */
typedef void (*lelu_task_func_t)(void);

/**
 * @brief Function pointer type for debug output
 * 
 * The scheduler calls this function to emit debug/diagnostic messages.
 * The user provides an implementation that sends the string to their
 * preferred output (UART, USB CDC, SWO, RTT, semihosting, etc.).
 * 
 * @param msg  Null-terminated string to output
 * 
 * @note  This is only called from main-loop context (never from ISR),
 *        so it is safe to use blocking or buffered output functions.
 * 
 * Example implementations:
 * @code
 *   // UART output
 *   void my_uart_print(const char* msg) {
 *       HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
 *   }
 *
 *   // USB CDC (Virtual COM Port) output
 *   void my_cdc_print(const char* msg) {
 *       CDC_Transmit_FS((uint8_t*)msg, strlen(msg));
 *   }
 * @endcode
 */
typedef void (*lelu_print_func_t)(const char* msg);

/**
 * @brief Task structure containing all task information
 */
typedef struct {
    uint8_t          running;                    /**< Task state: 1=running, 0=stopped */
    uint32_t         period;                     /**< Execution period in milliseconds */
    uint32_t         elapsed_time;               /**< Time elapsed since last execution */
    lelu_task_func_t handler;                    /**< Pointer to task function */
    uint32_t         total_ticks;                /**< Profiling: total ms spent in task */
    uint32_t         run_count;                  /**< Profiling: number of task executions */
    uint32_t         last_exec_time;             /**< Profiling: last execution time (ms) */
    uint32_t         max_exec_time_100;          /**< Profiling: max exec time over last 100 runs */
    uint32_t         run_count_since_max_reset;  /**< Counter for max reset (resets every 100) */
    char             name[LELU_TASK_NAME_LEN];   /**< Human-readable task name */
} lelu_task_t;

/**
 * @brief Task statistics structure for profiling
 */
typedef struct {
    uint32_t         total_ticks;    /**< Total milliseconds spent executing task */
    uint32_t         run_count;      /**< Number of times task has executed */
} lelu_task_stats_t;

/**
 * @brief Scheduler status/error codes
 */
typedef enum {
    LELU_OK = 0,                /**< Operation successful */
    LELU_ERROR_FULL,            /**< Task array is full, cannot add more tasks */
    LELU_ERROR_INVALID_ID,      /**< Invalid task ID provided */
    LELU_ERROR_NULL_HANDLER     /**< NULL function pointer provided */
} lelu_status_t;

/* ==========================================================================
 * PUBLIC API - INITIALIZATION
 * ========================================================================== */

/**
 * @brief  Initialize the scheduler
 * 
 * Must be called before any other scheduler functions. Initializes internal
 * state and optionally stores a print callback for debug output.
 * 
 * @param  print_func  Function pointer for debug message output, or NULL
 *                     to disable debug output. The function receives a
 *                     null-terminated string. See @ref lelu_print_func_t
 *                     for example implementations.
 * 
 * @note   Call this after HAL_Init() and peripheral initialization.
 * 
 * @code
 *   // With USB CDC debug output
 *   void my_cdc_print(const char* msg) {
 *       CDC_Transmit_FS((uint8_t*)msg, strlen(msg));
 *   }
 *   lelu_scheduler_init(my_cdc_print);
 *
 *   // With UART debug output
 *   void my_uart_print(const char* msg) {
 *       HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
 *   }
 *   lelu_scheduler_init(my_uart_print);
 *
 *   // No debug output
 *   lelu_scheduler_init(NULL);
 * @endcode
 */
void lelu_scheduler_init(lelu_print_func_t print_func);

/**
 * @brief  Mark boot sequence as complete
 * 
 * Call this after all initialization is done and before entering the main loop.
 * This enables overrun detection - before this is called, overrun warnings
 * are suppressed to avoid false alarms during boot.
 */
void lelu_scheduler_set_boot_done(void);

/* ==========================================================================
 * PUBLIC API - TASK MANAGEMENT
 * ========================================================================== */

/**
 * @brief  Add a new task to the scheduler
 * 
 * Registers a task function to be called periodically. Tasks are stored in
 * order of registration, which also determines priority (first = highest).
 * 
 * @param  name     Human-readable task name (for debugging). Max LELU_TASK_NAME_LEN chars.
 * @param  handler  Pointer to task function. Must not be NULL.
 * @param  period   Execution period in milliseconds. Should be >= LELU_TICK_PERIOD_MS.
 * @param  task_id  Output: Receives the assigned task ID (0 to LELU_MAX_TASKS-1).
 *                  Can be NULL if you don't need the ID.
 * 
 * @return LELU_OK on success
 * @return LELU_ERROR_FULL if task array is full
 * @return LELU_ERROR_NULL_HANDLER if handler is NULL
 * 
 * @note   Tasks are created in RUNNING state by default.
 * @note   elapsed_time is initialized to period, so task runs immediately on first tick.
 */
lelu_status_t lelu_scheduler_add_task(const char* name, 
                                       lelu_task_func_t handler,
                                       uint32_t period,
                                       uint8_t* task_id);

/**
 * @brief  Start (enable) a task
 * 
 * @param  task_id  Task ID returned by lelu_scheduler_add_task()
 * 
 * @note   Does nothing if task_id is invalid.
 */
void lelu_scheduler_start_task(uint8_t task_id);

/**
 * @brief  Stop (disable) a task
 * 
 * Stopped tasks are not executed but remain registered. Their elapsed_time
 * continues to accumulate, so they may run immediately when re-started.
 * 
 * @param  task_id  Task ID returned by lelu_scheduler_add_task()
 * 
 * @note   Does nothing if task_id is invalid.
 */
void lelu_scheduler_stop_task(uint8_t task_id);

/* ==========================================================================
 * PUBLIC API - SCHEDULER EXECUTION
 * ========================================================================== */

/**
 * @brief  SysTick handler - call from HAL_IncTick() or timer ISR
 * 
 * This function must be called every 1 millisecond (typically from SysTick).
 * It updates internal timing counters and sets the timer flag when a
 * tick period has elapsed.
 * 
 * @note   Call this from your HAL_IncTick() override:
 * @code
 *         void HAL_IncTick(void) {
 *             uwTick += uwTickFreq;
 *             lelu_scheduler_systick();
 *         }
 * @endcode
 * 
 * @warning This runs in interrupt context - keep it fast!
 */
void lelu_scheduler_systick(void);

/**
 * @brief  Run one scheduler iteration
 * 
 * Checks all tasks and executes those whose period has elapsed.
 * Call this from your main loop.
 * 
 * @return true if at least one task was executed
 * @return false if no tasks were ready
 * 
 * @note   This is non-blocking - returns immediately after checking/running tasks.
 */
bool lelu_scheduler_run(void);

/**
 * @brief  Check if a tick period has elapsed
 * 
 * Use this to implement sleep/idle between scheduler ticks.
 * 
 * @return true if LELU_TICK_PERIOD_MS has passed since last clear
 * @return false otherwise
 */
bool lelu_scheduler_tick_pending(void);

/**
 * @brief  Clear the pending tick flag
 * 
 * Call this after lelu_scheduler_run() to acknowledge the tick.
 */
void lelu_scheduler_clear_tick(void);

/* ==========================================================================
 * PUBLIC API - STATISTICS AND DEBUGGING
 * ========================================================================== */

/**
 * @brief  Get statistics for a specific task
 * 
 * @param  task_id  Task ID
 * @param  stats    Pointer to stats structure to fill
 * 
 * @note   Does nothing if task_id is invalid or stats is NULL.
 */
void lelu_scheduler_get_stats(uint8_t task_id, lelu_task_stats_t* stats);

/**
 * @brief  Print statistics for all tasks via the debug output callback
 * 
 * Outputs timing information for each registered task, including overrun
 * count. Useful for performance analysis and debugging.
 * 
 * @note   Requires a valid print callback passed to lelu_scheduler_init().
 *         Does nothing if no callback is set.
 */
void lelu_scheduler_print_stats(void);

/**
 * @brief  Get total elapsed ticks since scheduler started
 * 
 * @return Total tick count in milliseconds
 */
uint32_t lelu_scheduler_get_total_ticks(void);

/**
 * @brief  Get number of registered tasks
 * 
 * @return Number of tasks (0 to LELU_MAX_TASKS)
 */
uint8_t lelu_scheduler_get_task_count(void);

/**
 * @brief  Get number of tick overruns since boot
 * 
 * An overrun occurs when the previous tick was not processed before the
 * next tick fires, meaning tasks are collectively taking longer than
 * LELU_TICK_PERIOD_MS.
 * 
 * @return Number of overruns detected (saturates at UINT32_MAX)
 */
uint32_t lelu_scheduler_get_overrun_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LELU_SCHEDULER_H */
