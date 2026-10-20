#include "bmc_cmd.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 64
#define MAX_TOKENS 6

static const char *const NAMES[] = {
    [BMC_CMD_HELLO] = "HELLO", [BMC_CMD_STATUS] = "STATUS", [BMC_CMD_EVENTS] = "EVENTS",
    [BMC_CMD_SELFTEST] = "SELFTEST", [BMC_CMD_MODE] = "MODE", [BMC_CMD_ACK] = "ACK",
    [BMC_CMD_HEATER] = "HEATER", [BMC_CMD_FAN] = "FAN", [BMC_CMD_SETPOINT] = "SETPOINT",
    [BMC_CMD_GAINS] = "GAINS", [BMC_CMD_FF] = "FF", [BMC_CMD_FFPARAM] = "FFPARAM",
    [BMC_CMD_PROFILE_CLEAR] = "PROFILE", [BMC_CMD_PROFILE_ADD] = "PROFILE",
    [BMC_CMD_PROFILE_PRESET] = "PROFILE", [BMC_CMD_PROFILE_START] = "PROFILE",
    [BMC_CMD_PROFILE_STOP] = "PROFILE", [BMC_CMD_TESTSET_OT] = "TESTSET",
    [BMC_CMD_TESTINJECT] = "TESTINJECT", [BMC_CMD_UNKNOWN] = "?", [BMC_CMD_BAD_ARGS] = "?",
};

const char *bmc_cmd_name(bmc_cmd_id_t id)
{
    return (unsigned)id <= BMC_CMD_BAD_ARGS ? NAMES[id] : "?";
}

static bool to_float(const char *s, float *out)
{
    char *end;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool to_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < -1000000 || v > 1000000) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool is(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

bool bmc_cmd_parse(const char *line, size_t len, bmc_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    char buf[MAX_LINE + 1];
    if (len > MAX_LINE) {
        out->id = BMC_CMD_BAD_ARGS;
        return true;
    }
    for (size_t k = 0; k < len; k++) {
        buf[k] = (char)toupper((unsigned char)line[k]);
    }
    buf[len] = '\0';

    char *tok[MAX_TOKENS];
    int n = 0;
    for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
        if (n == MAX_TOKENS) {
            out->id = BMC_CMD_BAD_ARGS;
            return true;
        }
        tok[n++] = p;
    }
    if (n == 0) {
        return false;
    }
    int a = 0;
    if (tok[0][0] == '#') {
        int t;
        if (!to_int(tok[0] + 1, &t) || t < 0) {
            out->id = BMC_CMD_BAD_ARGS;
            return true;
        }
        out->has_token = true;
        out->token = (uint32_t)t;
        a = 1;
        if (n == 1) {
            return false;
        }
    }
    const char *cmd = tok[a];
    int argc = n - a - 1;
    char **argv = &tok[a + 1];
    out->id = BMC_CMD_BAD_ARGS;

    if (is(cmd, "HELLO")) {
        out->id = BMC_CMD_HELLO;
    } else if (is(cmd, "STATUS")) {
        out->id = BMC_CMD_STATUS;
    } else if (is(cmd, "EVENTS")) {
        out->id = BMC_CMD_EVENTS;
    } else if (is(cmd, "SELFTEST")) {
        out->id = BMC_CMD_SELFTEST;
    } else if (is(cmd, "ACK")) {
        out->id = BMC_CMD_ACK;
    } else if (is(cmd, "MODE")) {
        if (argc == 1) {
            static const struct { const char *name; bmc_mode_t m; } MODES[] = {
                { "STANDBY", BMC_MODE_STANDBY }, { "CHAR", BMC_MODE_CHAR },
                { "REACTIVE", BMC_MODE_REACTIVE }, { "PROACTIVE", BMC_MODE_PROACTIVE },
            };
            for (unsigned k = 0; k < sizeof(MODES) / sizeof(MODES[0]); k++) {
                if (is(argv[0], MODES[k].name)) {
                    out->id = BMC_CMD_MODE;
                    out->mode = MODES[k].m;
                }
            }
        }
    } else if (is(cmd, "HEATER")) {
        int on;
        if (argc == 2 && to_int(argv[0], &on) && (on == 0 || on == 1) && to_float(argv[1], &out->f[0])) {
            out->id = BMC_CMD_HEATER;
            out->flag = (on == 1);
        }
    } else if (is(cmd, "FAN")) {
        if (argc == 1 && is(argv[0], "AUTO")) {
            out->id = BMC_CMD_FAN;
            out->flag = true;
        } else if (argc == 1 && to_float(argv[0], &out->f[0])) {
            out->id = BMC_CMD_FAN;
        }
    } else if (is(cmd, "SETPOINT")) {
        if (argc == 1 && to_float(argv[0], &out->f[0])) {
            out->id = BMC_CMD_SETPOINT;
        }
    } else if (is(cmd, "GAINS")) {
        if (argc == 3 && to_float(argv[0], &out->f[0]) && to_float(argv[1], &out->f[1]) &&
            to_float(argv[2], &out->f[2])) {
            out->id = BMC_CMD_GAINS;
        }
    } else if (is(cmd, "FF")) {
        if (argc == 1 && to_int(argv[0], &out->i[0])) {
            out->id = BMC_CMD_FF;
        }
    } else if (is(cmd, "FFPARAM")) {
        if (argc == 3 && to_int(argv[0], &out->i[0]) && to_int(argv[1], &out->i[1]) &&
            to_float(argv[2], &out->f[0])) {
            out->id = BMC_CMD_FFPARAM;
        }
    } else if (is(cmd, "PROFILE")) {
        if (argc == 1 && is(argv[0], "CLEAR")) {
            out->id = BMC_CMD_PROFILE_CLEAR;
        } else if (argc == 1 && is(argv[0], "START")) {
            out->id = BMC_CMD_PROFILE_START;
        } else if (argc == 1 && is(argv[0], "STOP")) {
            out->id = BMC_CMD_PROFILE_STOP;
        } else if (argc == 2 && is(argv[0], "PRESET") && to_int(argv[1], &out->i[0])) {
            out->id = BMC_CMD_PROFILE_PRESET;
        } else if (argc == 3 && is(argv[0], "ADD") && to_int(argv[1], &out->i[0]) &&
                   to_float(argv[2], &out->f[0]) && out->i[0] > 0) {
            out->id = BMC_CMD_PROFILE_ADD;
        }
    } else if (is(cmd, "TESTSET")) {
        if (argc == 2 && (is(argv[0], "OT") || is(argv[0], "OT_LIMIT")) && to_float(argv[1], &out->f[0])) {
            out->id = BMC_CMD_TESTSET_OT;
        }
    } else if (is(cmd, "TESTINJECT")) {
        if (argc == 1) {
            static const struct { const char *name; bmc_fault_t f; } F[] = {
                { "OT", BMC_FAULT_OVER_TEMP }, { "OVER_TEMP", BMC_FAULT_OVER_TEMP },
                { "TEMP_LOSS", BMC_FAULT_TEMP_LOSS }, { "TELEM", BMC_FAULT_TELEM_LOSS },
                { "I2C", BMC_FAULT_I2C }, { "STALL", BMC_FAULT_FAN_STALL },
                { "STALE", BMC_FAULT_SENSOR_STALE },
            };
            if (is(argv[0], "CLEAR")) {
                out->id = BMC_CMD_TESTINJECT;
                out->flag = true;
            }
            for (unsigned k = 0; k < sizeof(F) / sizeof(F[0]); k++) {
                if (is(argv[0], F[k].name)) {
                    out->id = BMC_CMD_TESTINJECT;
                    out->fault = F[k].f;
                }
            }
        }
    } else {
        out->id = BMC_CMD_UNKNOWN;
    }
    return true;
}
