# Lelu Scheduler

A simple, lightweight cooperative task scheduler for STM32 microcontrollers.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Features

- **Cooperative (non-preemptive) scheduling** - Tasks run to completion
- **Configurable task periods** - Each task has its own execution interval
- **Priority by registration order** - First registered = highest priority
- **Task enable/disable** - Start and stop tasks at runtime
- **Overrun detection** - Counts when tasks take too long
- **Execution time profiling** - Track time spent in each task
- **Transport-agnostic debug output** - Works with UART, USB CDC, SWO, RTT, or any print function
- **STM32 HAL compatible** - Works with F4, G0, and other families
- **No HAL module dependencies** - Only needs `main.h` for config defines

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

### 3. Write a print function (optional)

The scheduler accepts any print function for debug output. Choose the one
that matches your hardware:

```c
/* Option A: USB CDC (Virtual COM Port) */
#include "usbd_cdc_if.h"
void my_cdc_print(const char* msg) {
    CDC_Transmit_FS((uint8_t*)msg, strlen(msg));
}

/* Option B: UART */
void my_uart_print(const char* msg) {
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
}

/* Option C: No debug output - just pass NULL to init */
```

### 4. Initialize and run

```c
#include "lelu_scheduler.h"

void my_task(void) {
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
}

// In main():
lelu_scheduler_init(my_cdc_print);  // or my_uart_print, or NULL
lelu_scheduler_add_task("blink", my_task, 500, NULL);
lelu_scheduler_set_boot_done();

while (1) {
    lelu_scheduler_run();
    while (!lelu_scheduler_tick_pending()) { __WFI(); }
    lelu_scheduler_clear_tick();
}
```

---

## Debug Output

### How It Works

The scheduler emits debug messages for task registration, boot status, and
statistics. Instead of being tied to a specific peripheral (like UART), it
calls a user-provided function pointer:

```c
typedef void (*lelu_print_func_t)(const char* msg);
```

You supply any function that takes a null-terminated string and sends it
wherever you want. The scheduler only calls this from main-loop context
(never from ISR), so it's safe to use blocking or buffered functions.

### USB CDC Example (Full)

```c
#include "main.h"
#include "usbd_cdc_if.h"
#include "../lelu_scheduler/include/lelu_scheduler.h"

extern uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len);

/** @brief Print callback for scheduler debug via USB Virtual COM Port */
void scheduler_cdc_print(const char* msg)
{
    CDC_Transmit_FS((uint8_t*)msg, strlen(msg));
}

void task_blink(void) {
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_4);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USB_DEVICE_Init();

    lelu_scheduler_init(scheduler_cdc_print);
    lelu_scheduler_add_task("blink", task_blink, 1000, NULL);
    lelu_scheduler_set_boot_done();

    while (1) {
        lelu_scheduler_run();
        while (!lelu_scheduler_tick_pending()) {}
        lelu_scheduler_clear_tick();
    }
}
```

### UART Example (Full)

```c
#include "main.h"
#include "../lelu_scheduler/include/lelu_scheduler.h"

extern UART_HandleTypeDef huart2;

/** @brief Print callback for scheduler debug via UART */
void scheduler_uart_print(const char* msg)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    lelu_scheduler_init(scheduler_uart_print);
    lelu_scheduler_add_task("blink", task_blink, 500, NULL);
    lelu_scheduler_set_boot_done();

    while (1) {
        lelu_scheduler_run();
        while (!lelu_scheduler_tick_pending()) { __WFI(); }
        lelu_scheduler_clear_tick();
    }
}
```

---

## Configuration

Configuration is done via preprocessor defines. Define these **before** including `lelu_scheduler.h`, or in your compiler/project settings. The recommended place is `main.h` (inside a `USER CODE` block so CubeMX doesn't overwrite it).

### Available Defines

| Define | Default | Description |
|--------|---------|-------------|
| `LELU_MAX_TASKS` | 8 | Maximum number of tasks that can be registered. Each task uses ~32 bytes of RAM. |
| `LELU_TASK_NAME_LEN` | 20 | Maximum characters for task names (including null terminator). Used for debug output. |
| `LELU_TICK_PERIOD_MS` | 25 | Base scheduler tick period in milliseconds. All task periods should ideally be multiples of this value. |

### Example: Custom Configuration in main.h

```c
/* In main.h, inside USER CODE Private defines */
#define LELU_MAX_TASKS      10      /* Room for expansion */
#define LELU_TICK_PERIOD_MS 20      /* 20ms = 50 Hz tick rate */
```

### Choosing LELU_TICK_PERIOD_MS

The tick period determines the scheduler's time resolution. For best results:

1. **Use the GCD** of all your task periods
   - Tasks at 100ms and 500ms -> use 100ms (or 50ms, 25ms, etc.)
   - Tasks at 30ms and 50ms -> use 10ms (GCD of 30 and 50)

2. **Trade-offs:**
   - Lower values = more responsive, but more CPU overhead
   - Higher values = less overhead, but coarser timing

3. **Typical values:**
   - 10ms - High responsiveness (100 scheduler ticks/second)
   - 25ms - Good balance (40 scheduler ticks/second)
   - 50ms - Low overhead (20 scheduler ticks/second)

---

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `lelu_scheduler_init(print_func)` | Initialize scheduler. Pass a print callback for debug, or NULL. |
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
| `lelu_scheduler_print_stats()` | Print all task statistics via the print callback. |
| `lelu_scheduler_get_stats(id, &stats)` | Get stats for a specific task. |
| `lelu_scheduler_get_total_ticks()` | Get total ms elapsed since init. |
| `lelu_scheduler_get_task_count()` | Get number of registered tasks. |
| `lelu_scheduler_get_overrun_count()` | Get number of tick overruns since boot. |

---

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `LELU_OK` | 0 | Operation successful |
| `LELU_ERROR_FULL` | 1 | Cannot add task - array is full (increase `LELU_MAX_TASKS`) |
| `LELU_ERROR_INVALID_ID` | 2 | Invalid task ID provided |
| `LELU_ERROR_NULL_HANDLER` | 3 | NULL function pointer passed to `add_task` |

---

## Overrun Detection

An overrun occurs when the scheduler tick fires before the previous tick was
processed, meaning your tasks are collectively taking longer than
`LELU_TICK_PERIOD_MS` to execute.

Overruns are **counted** (not printed from ISR) for safety. Check them via:

```c
uint32_t overruns = lelu_scheduler_get_overrun_count();
```

Or see them in the stats output:

```c
lelu_scheduler_print_stats();
// Output includes: [LELU] Task Statistics (uptime=1m23s, overruns=0)
```

**If overruns occur:**
1. Check which task is slow using `lelu_scheduler_print_stats()`
2. Optimize the slow task (remove delays, use DMA, etc.)
3. Increase `LELU_TICK_PERIOD_MS` if timing allows
4. Reduce task frequency

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

---

## Memory Usage

| Component | Size |
|-----------|------|
| Per task | ~32 bytes |
| Global state | ~20 bytes |
| Debug buffer | 128 bytes |
| **Total (8 tasks)** | **~400 bytes** |

---

## Migration from v1.x

v2.0.0 replaces the UART handle parameter with a generic print callback:

```c
/* v1.x (old) */
lelu_scheduler_init(&huart2);          // passed UART handle directly

/* v2.0.0 (new) */
void my_print(const char* msg) {
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
}
lelu_scheduler_init(my_print);         // pass any print function

/* Or with USB CDC */
void my_cdc_print(const char* msg) {
    CDC_Transmit_FS((uint8_t*)msg, strlen(msg));
}
lelu_scheduler_init(my_cdc_print);

/* Or disable debug output entirely */
lelu_scheduler_init(NULL);
```

Other changes:
- Overrun messages are no longer printed from ISR context (safer). Use
  `lelu_scheduler_get_overrun_count()` or `lelu_scheduler_print_stats()`.
- `lelu_scheduler_print_stats()` now includes overrun count in the header.
- No HAL peripheral modules required (UART HAL no longer needed).

---

## Troubleshooting

### Build error: unknown type name 'UART_HandleTypeDef'

You are using v1.x of the scheduler. Update to v2.0.0 which removed the
direct UART dependency.

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
| 2.0.0 | 2026 | **Breaking:** Replace UART handle with generic print callback. Add `lelu_scheduler_get_overrun_count()`. Remove ISR-context prints. |
| 1.0.0 | 2025 | Initial release |
