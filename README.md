# Lelu Scheduler

A simple, lightweight cooperative task scheduler for STM32 microcontrollers.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Features

- ✅ **Cooperative (non-preemptive) scheduling** - Tasks run to completion
- ✅ **Configurable task periods** - Each task has its own execution interval
- ✅ **Priority by registration order** - First registered = highest priority
- ✅ **Task enable/disable** - Start and stop tasks at runtime
- ✅ **Overrun detection** - Warns when tasks take too long
- ✅ **Execution time profiling** - Track time spent in each task
- ✅ **Debug output via UART** - Optional diagnostic messages
- ✅ **STM32 HAL compatible** - Works with F4, G0, and other families

## Repository Structure

```
lelu_scheduler/
├── include/
│   └── lelu_scheduler.h    # Public API header
├── src/
│   └── lelu_scheduler.c    # Implementation
├── examples/
│   └── blinky_two_leds.c   # Complete working example
├── LICENSE                 # MIT License
└── README.md               # This file
```

## Installation

### Option 1: Git Submodule (Recommended)

Add as a submodule to your STM32 project:

```bash
cd your_project/Core
git submodule add https://github.com/atelierlelu/lelu_scheduler.git
```

Then add to your include paths:
- `lelu_scheduler/include`

And add to your source files:
- `lelu_scheduler/src/lelu_scheduler.c`

### Option 2: Manual Copy

Copy the files to your project:

```bash
# Copy header to your Inc folder
cp lelu_scheduler/include/lelu_scheduler.h your_project/Core/Inc/

# Copy source to your Src folder  
cp lelu_scheduler/src/lelu_scheduler.c your_project/Core/Src/
```

## Quick Start

### 1. Include the header

```c
#include "lelu_scheduler.h"
```

### 2. Modify HAL_IncTick()

Add the scheduler systick call to your `HAL_IncTick()` function. You can place this
in `main.c` or `stm32f4xx_it.c`:

```c
void HAL_IncTick(void)
{
    uwTick += (uint32_t)uwTickFreq;
    lelu_scheduler_systick();  // <-- Add this line
}
```

### 3. Initialize and run

```c
#include "lelu_scheduler.h"

// In main():
lelu_scheduler_init(&huart2);  // Pass UART for debug, or NULL
lelu_scheduler_add_task("myTask", my_handler, 100, NULL);
lelu_scheduler_set_boot_done();

while (1) {
    lelu_scheduler_run();
    while (!lelu_scheduler_tick_pending()) { __WFI(); }
    lelu_scheduler_clear_tick();
}
```

---

## Configuration

Configuration is done via preprocessor defines. Define these **before** including `lelu_scheduler.h`, or in your compiler/project settings.

### Available Defines

| Define | Default | Description |
|--------|---------|-------------|
| `LELU_MAX_TASKS` | 8 | Maximum number of tasks that can be registered. Each task uses ~32 bytes of RAM. |
| `LELU_TASK_NAME_LEN` | 20 | Maximum characters for task names (including null terminator). Used for debug output. |
| `LELU_TICK_PERIOD_MS` | 25 | Base scheduler tick period in milliseconds. All task periods should ideally be multiples of this value. |

### Example: Custom Configuration

```c
/* In main.c, BEFORE including lelu_scheduler.h */
#define LELU_MAX_TASKS      4       /* Only need 4 tasks */
#define LELU_TICK_PERIOD_MS 10      /* 10ms tick for finer resolution */
#include "lelu_scheduler.h"
```

### Choosing LELU_TICK_PERIOD_MS

The tick period determines the scheduler's time resolution. For best results:

1. **Use the GCD** of all your task periods
   - Tasks at 100ms and 500ms → use 100ms (or 50ms, 25ms, etc.)
   - Tasks at 30ms and 50ms → use 10ms (GCD of 30 and 50)

2. **Trade-offs:**
   - Lower values = more responsive, but more CPU overhead
   - Higher values = less overhead, but coarser timing

3. **Typical values:**
   - 10ms - High responsiveness (100 scheduler ticks/second)
   - 25ms - Good balance (40 scheduler ticks/second)
   - 50ms - Low overhead (20 scheduler ticks/second)

---

## Complete Blinky Example

This example blinks two LEDs at different rates using the Lelu Scheduler.

See [`examples/blinky_two_leds.c`](examples/blinky_two_leds.c) for the full source.

### Hardware Setup

- LED1 on PA5 (many Nucleo boards have this)
- LED2 on PA6 (or any other available GPIO)

### Code

```c
/* main.c - Two LED Blinky Example with Lelu Scheduler */

#include "main.h"
#include "lelu_scheduler.h"

UART_HandleTypeDef huart2;

/* Task Functions */
void task_blink_led1(void) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5); }
void task_blink_led2(void) { HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_6); }

/* HAL Tick Override */
void HAL_IncTick(void)
{
    uwTick += (uint32_t)uwTickFreq;
    lelu_scheduler_systick();
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    
    /* Initialize Scheduler */
    lelu_scheduler_init(&huart2);
    
    /* Register Tasks */
    lelu_scheduler_add_task("LED1_fast", task_blink_led1, 250, NULL);
    lelu_scheduler_add_task("LED2_slow", task_blink_led2, 1000, NULL);
    
    lelu_scheduler_set_boot_done();
    
    /* Main Loop */
    while (1)
    {
        lelu_scheduler_run();
        while (!lelu_scheduler_tick_pending()) { __WFI(); }
        lelu_scheduler_clear_tick();
    }
}
```

### Expected Output (UART @ 115200 baud)

```
[LELU] Scheduler initialized (max 8 tasks, 25ms tick)
[LELU] Added task 'LED1_fast' (id=0, period=250ms)
[LELU] Added task 'LED2_slow' (id=1, period=1000ms)
[LELU] Boot done, scheduler active with 2 tasks
```

### Expected Behavior

- LED1 toggles every 250ms (blinks at 2 Hz)
- LED2 toggles every 1000ms (blinks at 0.5 Hz)
- Both LEDs blink independently and continuously

---

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `lelu_scheduler_init(uart_handle)` | Initialize scheduler. Pass UART handle for debug, or NULL. |
| `lelu_scheduler_set_boot_done()` | Enable overrun detection. Call after all setup is complete. |

### Task Management

| Function | Description |
|----------|-------------|
| `lelu_scheduler_add_task(name, handler, period, &id)` | Register a new task. Returns `LELU_OK` on success. |
| `lelu_scheduler_start_task(id)` | Enable a task (tasks start enabled by default). |
| `lelu_scheduler_stop_task(id)` | Disable a task (it won't execute until re-enabled). |

### Execution

| Function | Description |
|----------|-------------|
| `lelu_scheduler_systick()` | Call from `HAL_IncTick()` every 1ms. |
| `lelu_scheduler_run()` | Execute ready tasks. Call from main loop. Returns `true` if any task ran. |
| `lelu_scheduler_tick_pending()` | Check if a tick period has elapsed. |
| `lelu_scheduler_clear_tick()` | Clear the tick flag after processing. |

### Statistics

| Function | Description |
|----------|-------------|
| `lelu_scheduler_print_stats()` | Print all task statistics via UART. |
| `lelu_scheduler_get_stats(id, &stats)` | Get stats for a specific task. |
| `lelu_scheduler_get_total_ticks()` | Get total ms elapsed since init. |
| `lelu_scheduler_get_task_count()` | Get number of registered tasks. |

---

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `LELU_OK` | 0 | Operation successful |
| `LELU_ERROR_FULL` | 1 | Cannot add task - array is full (increase `LELU_MAX_TASKS`) |
| `LELU_ERROR_INVALID_ID` | 2 | Invalid task ID provided |
| `LELU_ERROR_NULL_HANDLER` | 3 | NULL function pointer passed to `add_task` |

---

## Tips and Best Practices

### 1. Keep Tasks Short

This is a **cooperative** scheduler - tasks are not preempted. Long-running tasks
will block other tasks and may cause overruns.

```c
/* BAD - blocks for 1 second! */
void bad_task(void) {
    HAL_Delay(1000);
    do_something();
}

/* GOOD - non-blocking, uses state machine */
void good_task(void) {
    static uint8_t state = 0;
    switch (state) {
        case 0: start_operation(); state = 1; break;
        case 1: if (operation_done()) state = 2; break;
        case 2: finish_operation(); state = 0; break;
    }
}
```

### 2. Use Appropriate Periods

Don't schedule tasks more often than needed:

```c
lelu_scheduler_add_task("button", check_button, 50, NULL);  /* Good */
lelu_scheduler_add_task("button", check_button, 1, NULL);   /* Wasteful */
```

### 3. Monitor for Overruns

If you see `OR-` messages on UART, your tasks are taking too long. Either:
- Optimize the slow task
- Increase `LELU_TICK_PERIOD_MS`
- Reduce task frequency

---

## Memory Usage

| Component | Size |
|-----------|------|
| Per task | ~32 bytes |
| Global state | ~16 bytes |
| Debug buffer | 128 bytes |
| **Total (8 tasks)** | **~400 bytes** |

---

## Troubleshooting

### "OR-" messages appearing

**Cause:** Tasks are taking longer than `LELU_TICK_PERIOD_MS` to complete.

**Solutions:**
1. Check which task is slow using `lelu_scheduler_print_stats()`
2. Optimize the slow task (remove delays, use DMA, etc.)
3. Increase `LELU_TICK_PERIOD_MS` if timing allows

### Tasks not running

**Checklist:**
1. Did you call `lelu_scheduler_init()`?
2. Did you call `lelu_scheduler_set_boot_done()`?
3. Did you add `lelu_scheduler_systick()` to `HAL_IncTick()`?
4. Is `lelu_scheduler_run()` being called in your main loop?

---

## License

MIT License - see [LICENSE](LICENSE) file.

Copyright (c) 2025 Atelier Lelu

---

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | 2025 | Initial release |
