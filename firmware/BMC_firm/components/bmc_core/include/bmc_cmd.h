#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bmc_types.h"

/* Host -> BMC text commands (docs/bmc_plan/03 section 3). One command per
 * datagram line, case-insensitive, optional leading "#<n>" token that the reply
 * echoes so the host can match replies and retry safely. */

typedef enum {
    BMC_CMD_HELLO,
    BMC_CMD_STATUS,
    BMC_CMD_EVENTS,
    BMC_CMD_SELFTEST,
    BMC_CMD_MODE,
    BMC_CMD_ACK,
    BMC_CMD_HEATER,
    BMC_CMD_FAN,
    BMC_CMD_SETPOINT,
    BMC_CMD_GAINS,
    BMC_CMD_FF,
    BMC_CMD_FFPARAM,
    BMC_CMD_PROFILE_CLEAR,
    BMC_CMD_PROFILE_ADD,
    BMC_CMD_PROFILE_PRESET,
    BMC_CMD_PROFILE_START,
    BMC_CMD_PROFILE_STOP,
    BMC_CMD_TESTSET_OT,
    BMC_CMD_TESTINJECT,
    BMC_CMD_UNKNOWN,
    BMC_CMD_BAD_ARGS,
} bmc_cmd_id_t;

typedef struct {
    bmc_cmd_id_t id;
    bool has_token;
    uint32_t token;
    bmc_mode_t mode;        /* MODE */
    bool flag;              /* HEATER on, FAN auto, TESTINJECT clear */
    float f[3];             /* numeric arguments in order */
    int i[2];               /* integer arguments (FF id, FFPARAM id/index, PROFILE PRESET n) */
    bmc_fault_t fault;      /* TESTINJECT target */
} bmc_cmd_t;

/* Returns false for an empty line. Malformed input yields id BMC_CMD_UNKNOWN or BMC_CMD_BAD_ARGS. */
bool bmc_cmd_parse(const char *line, size_t len, bmc_cmd_t *out);
const char *bmc_cmd_name(bmc_cmd_id_t id);
