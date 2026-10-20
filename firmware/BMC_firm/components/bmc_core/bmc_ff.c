#include "bmc_ff.h"

#include <string.h>

static const char *const NAMES[BMC_FF_VARIANTS] = { "none", "linear", "table", "leadlag" };

int bmc_ff_count(void)
{
    return BMC_FF_VARIANTS;
}

const char *bmc_ff_name(int id)
{
    return (id >= 0 && id < BMC_FF_VARIANTS) ? NAMES[id] : "?";
}

void bmc_ff_defaults(float params[BMC_FF_VARIANTS][BMC_FF_PARAMS])
{
    memset(params, 0, sizeof(float) * BMC_FF_VARIANTS * BMC_FF_PARAMS);
    params[1][0] = 10.0f;                       /* %/A, PLACEHOLDER */
    params[1][1] = 0.0f;
    static const float cur[8] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f };
    static const float dem[8] = { 0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 80.0f };
    memcpy(&params[2][0], cur, sizeof(cur));
    memcpy(&params[2][8], dem, sizeof(dem));
    params[3][0] = 10.0f;
    params[3][1] = 0.0f;
    params[3][2] = 1.5f;                        /* lead [s] */
    params[3][3] = 1.0f;                        /* lag  [s] */
}

void bmc_ff_reset(bmc_ff_state_t *st)
{
    st->z = 0.0f;
    st->init = false;
}

static float linear(const float *p, float i)
{
    float x = i - p[1];
    return x > 0.0f ? p[0] * x : 0.0f;
}

static float table(const float *p, float i)
{
    if (i <= p[0]) {
        return p[8];
    }
    for (int k = 1; k < 8; k++) {
        if (i <= p[k]) {
            float span = p[k] - p[k - 1];
            if (span <= 0.0f) {
                return p[8 + k];
            }
            return p[8 + k - 1] + (i - p[k - 1]) / span * (p[8 + k] - p[8 + k - 1]);
        }
    }
    return p[15];
}

float bmc_ff_compute(int id, const float *p, bmc_ff_state_t *st, float current_a, float dt_s)
{
    float y = 0.0f;
    switch (id) {
    case 1:
        y = linear(p, current_a);
        break;
    case 2:
        y = table(p, current_a);
        break;
    case 3: {
        float u = linear(p, current_a);
        float lead = p[2], lag = p[3];
        if (!st->init) {
            st->z = u;
            st->init = true;
        }
        if (lag > 0.0f && dt_s > 0.0f) {
            st->z += dt_s / (lag + dt_s) * (u - st->z);
            float ratio = lead / lag;
            y = ratio * u + (1.0f - ratio) * st->z;
        } else {
            y = u;
        }
        break;
    }
    default:
        break;
    }
    return y > 0.0f ? y : 0.0f;
}
