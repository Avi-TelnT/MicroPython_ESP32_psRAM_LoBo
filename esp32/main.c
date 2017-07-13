/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George
 *
 * Copyright (c) 2017 Boris Lovosevic (External SPIRAM support)
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_task.h"
#include "soc/cpu.h"
#include "esp_log.h"

#include "py/stackctrl.h"
#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "lib/mp-readline/readline.h"
#include "lib/utils/pyexec.h"
#include "uart.h"
#include "modmachine.h"
#include "mpthreadport.h"
#include "mpsleep.h"
#include "machrtc.h"

#include "sdkconfig.h"

// =========================================
// MicroPython runs as a task under FreeRTOS
// =========================================

#define MP_TASK_PRIORITY        (ESP_TASK_PRIO_MIN + 1)
#if CONFIG_MEMMAP_SPIRAM_ENABLE
// External SPIRAM is available, more memory for stack & heap can be allocated
#define MP_TASK_STACK_SIZE      (32 * 1024)
#define MP_TASK_HEAP_SIZE       (4 * 1024 * 1024 - 256)
#else
// Only DRAM memory available, limited amount of memory for stack & heap can be used
#define MP_TASK_STACK_SIZE      (16 * 1024)
#define MP_TASK_HEAP_SIZE       (92 * 1024)
#endif
#define MP_TASK_STACK_LEN       (MP_TASK_STACK_SIZE / sizeof(StackType_t))


STATIC StaticTask_t mp_task_tcb;
STATIC StackType_t mp_task_stack[MP_TASK_STACK_LEN] __attribute__((aligned (8)));
STATIC uint8_t *mp_task_heap;

//===============================
void mp_task(void *pvParameter) {
    volatile uint32_t sp = (uint32_t)get_sp();
    #if MICROPY_PY_THREAD
    mp_thread_init(&mp_task_stack[0], MP_TASK_STACK_LEN);
    #endif
    uart_init();

    // Allocate heap memory
    #if !CONFIG_MEMMAP_SPIRAM_ENABLE_MALLOC
    printf("\nAllocating uPY heap (%d bytes) in SPIRAM using pvPortMallocCaps\n\n", MP_TASK_HEAP_SIZE);
    mp_task_heap = pvPortMallocCaps(MP_TASK_HEAP_SIZE, MALLOC_CAP_SPIRAM);
    #else
    #if CONFIG_MEMMAP_SPIRAM_ENABLE
    printf("\nAllocating uPY heap (%d bytes) in SPIRAM using malloc\n\n", MP_TASK_HEAP_SIZE);
    #else
    printf("\nAllocating uPY heap (%d bytes) in DRAM using malloc\n\n", MP_TASK_HEAP_SIZE);
    #endif
    mp_task_heap = malloc(MP_TASK_HEAP_SIZE);
    #endif

    if (mpsleep_get_reset_cause() != MPSLEEP_DEEPSLEEP_RESET) {
        rtc_init0();
    }

soft_reset:
    // initialise the stack pointer for the main thread
    mp_stack_set_top((void *)sp);
    mp_stack_set_limit(MP_TASK_STACK_SIZE - 1024);

    // initialize the mp heap
    gc_init(mp_task_heap, mp_task_heap + MP_TASK_HEAP_SIZE);

    mp_init();
    mp_obj_list_init(mp_sys_path, 0);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));
    mp_obj_list_init(mp_sys_argv, 0);
    readline_init0();

    // initialise peripherals
    machine_pins_init();

    mpsleep_init0();

    // run boot-up scripts
    pyexec_frozen_module("_boot.py");
    pyexec_file("boot.py");
    if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
        pyexec_file("main.py");
    }

    // Main loop
    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            if (pyexec_raw_repl() != 0) {
                break;
            }
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

    #if MICROPY_PY_THREAD
    mp_thread_deinit();
    #endif

    mp_hal_stdout_tx_str("ESP32: soft reboot\r\n");

    // deinitialise peripherals
    machine_pins_deinit();

    mp_deinit();
    fflush(stdout);
    goto soft_reset;
}

//===================
void app_main(void) {
    nvs_flash_init();

	esp_log_level_set("*", ESP_LOG_ERROR);

    xTaskCreateStaticPinnedToCore(mp_task, "mp_task", MP_TASK_STACK_LEN, NULL, MP_TASK_PRIORITY, &mp_task_stack[0], &mp_task_tcb, 0);
}

//-----------------------------
void nlr_jump_fail(void *val) {
    printf("NLR jump failed, val=%p\n", val);
    esp_restart();
}

// modussl_mbedtls uses this function but it's not enabled in ESP IDF
//-----------------------------------------------
void mbedtls_debug_set_threshold(int threshold) {
    (void)threshold;
}
