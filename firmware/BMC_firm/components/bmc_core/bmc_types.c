#include "bmc_types.h"

static const char *const MODE_NAMES[BMC_MODE_COUNT] = {
    "STANDBY", "SELFTEST", "CHAR", "REACTIVE", "PROACTIVE", "FAILSAFE",
};

static const char *const FAULT_NAMES[BMC_FAULT_COUNT] = {
    "NONE", "OVER_TEMP", "TEMP_LOSS", "TELEM_LOSS", "I2C_FAULT", "FAN_STALL",
    "SENSOR_STALE", "RESET_WDT", "RESET_BROWNOUT", "RESET_PANIC",
};

const char *bmc_mode_name(bmc_mode_t m)
{
    return (unsigned)m < BMC_MODE_COUNT ? MODE_NAMES[m] : "?";
}

const char *bmc_fault_name(bmc_fault_t f)
{
    return (unsigned)f < BMC_FAULT_COUNT ? FAULT_NAMES[f] : "?";
}

bool bmc_mode_is_actuating(bmc_mode_t m)
{
    return m == BMC_MODE_CHAR || m == BMC_MODE_REACTIVE || m == BMC_MODE_PROACTIVE;
}
