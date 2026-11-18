#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BMC_MODE_STANDBY = 0,
    BMC_MODE_SELFTEST,
    BMC_MODE_CHAR,
    BMC_MODE_REACTIVE,
    BMC_MODE_PROACTIVE,
    BMC_MODE_FAILSAFE,
    BMC_MODE_COUNT
} bmc_mode_t;

typedef enum {
    BMC_FAULT_NONE = 0,
    BMC_FAULT_OVER_TEMP,
    BMC_FAULT_TEMP_LOSS,
    BMC_FAULT_TELEM_LOSS,
    BMC_FAULT_I2C,
    BMC_FAULT_FAN_STALL,
    BMC_FAULT_SENSOR_STALE,
    BMC_FAULT_RESET_WDT,
    BMC_FAULT_RESET_BROWNOUT,
    BMC_FAULT_RESET_PANIC,
    BMC_FAULT_COUNT
} bmc_fault_t;

const char *bmc_mode_name(bmc_mode_t m);
const char *bmc_fault_name(bmc_fault_t f);
bool bmc_mode_is_actuating(bmc_mode_t m);          /* CHAR, REACTIVE, PROACTIVE */

#define BMC_FAULT_BIT(f) ((f) == BMC_FAULT_NONE ? 0u : (1u << (unsigned)(f)))

/* telemetry flags word (bits 0-7 are unchanged from firmware v1) */
#define TELEM_FLAG_HEATER_REQUESTED (1u << 0)  /* heater demanded by profile or manual request */
#define TELEM_FLAG_HEATER_PERMIT    (1u << 1)  /* hardware permit pin asserted */
#define TELEM_FLAG_HEATER_ACTIVE    (1u << 2)  /* heater enable high, power applied */
#define TELEM_FLAG_FAN_STALL        (1u << 3)
#define TELEM_FLAG_OVER_TEMP        (1u << 4)
#define TELEM_FLAG_TEMP_INVALID     (1u << 5)
#define TELEM_FLAG_FAN_DISABLED     (1u << 6)  /* fan enable interlock open */
#define TELEM_FLAG_HEATER_TRIPPED   (1u << 7)  /* failsafe latched */
#define TELEM_FLAG_SELFTEST_OK      (1u << 8)
#define TELEM_FLAG_SAFE_TO_HANDLE   (1u << 9)
#define TELEM_FLAG_PROFILE_RUNNING  (1u << 10)
#define TELEM_FLAG_OT_WARN          (1u << 11)
#define TELEM_FLAG_TEST_HOOKS       (1u << 12) /* test-hook build: commands that force faults are enabled */
#define TELEM_FLAG_OT_LOWERED       (1u << 13) /* over-temp limit has been lowered from its default */
