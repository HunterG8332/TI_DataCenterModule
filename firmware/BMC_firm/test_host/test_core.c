#include "bmc_cmd.h"
#include "bmc_fanrpm.h"
#include "bmc_ff.h"
#include "bmc_pid.h"
#include "bmc_profile.h"
#include "bmc_sample.h"
#include "bmc_stall.h"
#include "test_util.h"

/* ---- PID ---------------------------------------------------------------- */
static void test_pid(void)
{
    bmc_pid_t p;
    bmc_pid_init(&p, 5.0f, 0.0f, 0.0f);
    p.slew_up = p.slew_down = 0.0f;
    CHECK_NEAR(bmc_pid_step(&p, 60.0f, 60.0f, 0.1f), 0.0, 1e-6);     /* on setpoint: nothing */
    CHECK_NEAR(bmc_pid_step(&p, 62.0f, 60.0f, 0.1f), 10.0, 1e-4);    /* 2 C hot -> 10 %% */
    CHECK_NEAR(bmc_pid_step(&p, 50.0f, 60.0f, 0.1f), 0.0, 1e-6);     /* cold clamps at out_min */
    CHECK_NEAR(bmc_pid_step(&p, 100.0f, 60.0f, 0.1f), 100.0, 1e-4);  /* clamps at out_max */

    /* integral action removes steady error and anti-windup stops it running away */
    bmc_pid_init(&p, 0.0f, 1.0f, 0.0f);
    p.slew_up = p.slew_down = 0.0f;
    float u = 0;
    for (int i = 0; i < 100; i++) u = bmc_pid_step(&p, 61.0f, 60.0f, 0.1f);   /* 1 C error for 10 s */
    CHECK_NEAR(u, 10.0, 0.05);
    bmc_pid_init(&p, 2.0f, 1.0f, 0.0f);                       /* with a P term the integrator stops early */
    p.slew_up = p.slew_down = 0.0f;
    for (int i = 0; i < 5000; i++) u = bmc_pid_step(&p, 70.0f, 60.0f, 0.1f);  /* saturated for 500 s */
    CHECK_NEAR(u, 100.0, 1e-4);
    CHECK(p.integ <= p.i_max + 1e-3);
    int steps = 0;                                                    /* unwinds quickly once below setpoint */
    while (u > 0.5f && steps < 200) { u = bmc_pid_step(&p, 40.0f, 60.0f, 0.1f); steps++; }
    CHECK(steps < 100);

    /* slew limiting */
    bmc_pid_init(&p, 100.0f, 0.0f, 0.0f);
    p.slew_up = 20.0f; p.slew_down = 20.0f;
    CHECK_NEAR(bmc_pid_step(&p, 70.0f, 60.0f, 0.1f), 2.0, 1e-4);     /* wants 100, may rise 20 %%/s */
    CHECK_NEAR(bmc_pid_step(&p, 70.0f, 60.0f, 0.1f), 4.0, 1e-4);

    /* feed-forward offset is added before saturation (proactive law) */
    bmc_pid_init(&p, 0.0f, 0.0f, 0.0f);
    p.slew_up = p.slew_down = 0.0f;
    p.offset = 35.0f;
    CHECK_NEAR(bmc_pid_step(&p, 60.0f, 60.0f, 0.1f), 35.0, 1e-4);

    /* derivative acts on the measurement: no kick when only the setpoint moves */
    bmc_pid_init(&p, 0.0f, 0.0f, 10.0f);
    p.slew_up = p.slew_down = 0.0f;
    bmc_pid_step(&p, 60.0f, 60.0f, 0.1f);
    CHECK_NEAR(bmc_pid_step(&p, 60.0f, 80.0f, 0.1f), 0.0, 1e-6);
}

/* ---- RPM matching --------------------------------------------------------- */
static void test_fanrpm(void)
{
    CHECK_NEAR(bmc_fanrpm_target(50.0f, 7000.0f), 3500.0, 1e-3);
    CHECK_NEAR(bmc_fanrpm_target(150.0f, 7000.0f), 7000.0, 1e-3);
    CHECK_NEAR(bmc_fanrpm_target(-5.0f, 7000.0f), 0.0, 1e-3);

    bmc_fanrpm_t a, b;                                       /* the two different fans */
    bmc_fanrpm_init(&a, 7000.0f);
    bmc_fanrpm_init(&b, 10000.0f);
    CHECK_NEAR(bmc_fanrpm_map_duty(&a, 3500.0f), 50.0, 1e-3);
    CHECK_NEAR(bmc_fanrpm_map_duty(&b, 3500.0f), 35.0, 1e-3);   /* faster fan needs less duty for the same RPM */
    CHECK_NEAR(bmc_fanrpm_step(&a, 0.0f, 0.0f, true, 0.05f), 0.0, 1e-6);

    /* closed loop: both fans, with real-ish gain mismatch, settle on the SAME rpm */
    float rpm_a = 0, rpm_b = 0;
    const float target = bmc_fanrpm_target(60.0f, BMC_RPM_REF);   /* 4200 */
    for (int i = 0; i < 400; i++) {
        float da = bmc_fanrpm_step(&a, target, rpm_a, true, 0.05f);
        float db = bmc_fanrpm_step(&b, target, rpm_b, true, 0.05f);
        float want_a = da / 100.0f * 6500.0f;                 /* plant: fan A tops out at 6500, not 7000 */
        float want_b = db / 100.0f * 10400.0f;                /* fan B at 10400 */
        rpm_a += 0.4f * (want_a - rpm_a);                     /* first-order spin-up */
        rpm_b += 0.4f * (want_b - rpm_b);
    }
    CHECK_NEAR(rpm_a, target, 25.0);
    CHECK_NEAR(rpm_b, target, 25.0);
    CHECK_NEAR(rpm_a, rpm_b, 40.0);

    /* tach window not full yet: feed-forward only, integrator frozen */
    bmc_fanrpm_reset(&a);
    float d = bmc_fanrpm_step(&a, 3500.0f, 0.0f, false, 0.05f);
    CHECK_NEAR(d, 50.0, 1e-3);

    /* a fan that cannot reach target does not wind up past its limit */
    for (int i = 0; i < 2000; i++) bmc_fanrpm_step(&a, 7000.0f, 100.0f, true, 0.05f);
    CHECK(fabsf(a.integ) <= a.i_limit + 1e-3f);
}

/* ---- stall detector ------------------------------------------------------- */
static void test_stall(void)
{
    bmc_stall_t d;
    bmc_stall_init(&d);
    uint32_t t = 0;
    for (int i = 0; i < 20; i++, t += 50) bmc_stall_update(&d, t, true, 50.0f, 3000, true);
    CHECK(!d.stalled);                                       /* healthy */

    bmc_stall_init(&d);
    t = 0;
    bmc_stall_update(&d, t, true, 50.0f, 0, true);           /* commanded, no tach, but window just started */
    CHECK(!d.stalled);
    for (; t < BMC_FAN_TACH_WINDOW_MS - 50; t += 50) bmc_stall_update(&d, t, true, 50.0f, 0, true);
    CHECK(!d.stalled);
    bmc_stall_update(&d, t + 100, true, 50.0f, 0, true);     /* one full window with no edges */
    CHECK(d.stalled);

    /* recovery needs consecutive turning cycles */
    t += 100;
    for (int i = 0; i < BMC_FAN_STALL_RECOVER_CYCLES - 1; i++) bmc_stall_update(&d, t += 50, true, 50.0f, 2000, true);
    CHECK(d.stalled);
    bmc_stall_update(&d, t += 50, true, 50.0f, 0, true);     /* interrupted: counter restarts */
    for (int i = 0; i < BMC_FAN_STALL_RECOVER_CYCLES; i++) bmc_stall_update(&d, t += 50, true, 50.0f, 2000, true);
    CHECK(!d.stalled);

    /* never armed when the fan is idle, disabled by the interlock, or the window is not full */
    bmc_stall_init(&d);
    t = 0;
    for (int i = 0; i < 20; i++, t += 50) bmc_stall_update(&d, t, true, 5.0f, 0, true);
    CHECK(!d.stalled);
    for (int i = 0; i < 20; i++, t += 50) bmc_stall_update(&d, t, false, 80.0f, 0, true);
    CHECK(!d.stalled);
    for (int i = 0; i < 20; i++, t += 50) bmc_stall_update(&d, t, true, 80.0f, 0, false);
    CHECK(!d.stalled);
}

/* ---- profile ---------------------------------------------------------------- */
static void test_profile(void)
{
    bmc_profile_t p;
    bmc_profile_clear(&p);
    CHECK(!bmc_profile_start(&p, 0));                        /* empty profile cannot run */
    CHECK(bmc_profile_preset(&p, 1));
    CHECK(p.n == 3 && bmc_profile_total_s(&p) == 960);
    CHECK(bmc_profile_start(&p, 10000));
    CHECK(!bmc_profile_start(&p, 10000));                    /* already running */
    float pct; uint32_t run;
    CHECK(bmc_profile_step(&p, 10000, &pct, &run) && pct == 0.0f && run == 0);
    CHECK(bmc_profile_step(&p, 10000 + 59999, &pct, &run) && pct == 0.0f);
    CHECK(bmc_profile_step(&p, 10000 + 60000, &pct, &run) && pct == 100.0f && run == 60000);
    CHECK(bmc_profile_step(&p, 10000 + 659999, &pct, &run) && pct == 100.0f);
    CHECK(bmc_profile_step(&p, 10000 + 660000, &pct, &run) && pct == 0.0f);
    CHECK(!bmc_profile_step(&p, 10000 + 960000, &pct, &run)); /* ended */
    CHECK(!p.running);

    bmc_profile_clear(&p);
    CHECK(bmc_profile_add(&p, 5, 50.0f));
    CHECK(!bmc_profile_add(&p, 0, 50.0f));
    CHECK(!bmc_profile_add(&p, 5, 101.0f));
    CHECK(!bmc_profile_add(&p, 5, -1.0f));
    CHECK(!bmc_profile_add(&p, 5, NAN));
    for (int i = 1; i < BMC_PROFILE_MAX_STEPS; i++) CHECK(bmc_profile_add(&p, 1, 10.0f));
    CHECK(!bmc_profile_add(&p, 1, 10.0f));                   /* full */
    CHECK(bmc_profile_start(&p, 0));
    CHECK(!bmc_profile_add(&p, 1, 10.0f));                   /* locked while running */
    CHECK(!bmc_profile_preset(&p, 1));
    bmc_profile_stop(&p);
    CHECK(!bmc_profile_preset(&p, 99));
}

/* ---- feedforward ------------------------------------------------------------ */
static void test_ff(void)
{
    float params[BMC_FF_VARIANTS][BMC_FF_PARAMS];
    bmc_ff_defaults(params);
    bmc_ff_state_t st;
    bmc_ff_reset(&st);
    CHECK(bmc_ff_count() == BMC_FF_VARIANTS);
    CHECK_STR(bmc_ff_name(1), "linear");
    CHECK_NEAR(bmc_ff_compute(0, params[0], &st, 3.0f, 0.1f), 0.0, 1e-6);
    params[1][0] = 10.0f; params[1][1] = 0.5f;
    CHECK_NEAR(bmc_ff_compute(1, params[1], &st, 0.3f, 0.1f), 0.0, 1e-6);   /* below idle: no demand */
    CHECK_NEAR(bmc_ff_compute(1, params[1], &st, 1.5f, 0.1f), 10.0, 1e-4);
    CHECK_NEAR(bmc_ff_compute(2, params[2], &st, 1.0f, 0.1f), 20.0, 1e-4);  /* table point */
    CHECK_NEAR(bmc_ff_compute(2, params[2], &st, 1.25f, 0.1f), 25.0, 1e-4); /* interpolated */
    CHECK_NEAR(bmc_ff_compute(2, params[2], &st, 99.0f, 0.1f), 80.0, 1e-4); /* clamps at the last point */
    CHECK_NEAR(bmc_ff_compute(2, params[2], &st, -1.0f, 0.1f), 0.0, 1e-4);

    /* lead-lag: a step produces an overshoot beyond the steady value, then settles */
    bmc_ff_reset(&st);
    params[3][0] = 10.0f; params[3][1] = 0.0f; params[3][2] = 2.0f; params[3][3] = 1.0f;
    bmc_ff_compute(3, params[3], &st, 0.0f, 0.1f);
    float first = bmc_ff_compute(3, params[3], &st, 2.0f, 0.1f);
    float last = first;
    for (int i = 0; i < 100; i++) last = bmc_ff_compute(3, params[3], &st, 2.0f, 0.1f);
    CHECK(first > 20.0f);                                    /* leads: above the 20 %% steady value */
    CHECK_NEAR(last, 20.0, 0.1);
    CHECK(bmc_ff_compute(3, params[3], &st, 0.0f, 0.1f) >= 0.0f);   /* output never negative */
}

/* ---- command parser ----------------------------------------------------------- */
static bmc_cmd_t parse(const char *s)
{
    bmc_cmd_t c;
    memset(&c, 0, sizeof(c));
    bool got = bmc_cmd_parse(s, strlen(s), &c);
    if (!got) c.id = (bmc_cmd_id_t)-1;
    return c;
}

static void test_cmd(void)
{
    bmc_cmd_t c;
    c = parse("HELLO\n"); CHECK(c.id == BMC_CMD_HELLO);
    c = parse("hello"); CHECK(c.id == BMC_CMD_HELLO);                   /* case-insensitive */
    c = parse("STATUS"); CHECK(c.id == BMC_CMD_STATUS);
    c = parse("#42 STATUS"); CHECK(c.id == BMC_CMD_STATUS && c.has_token && c.token == 42);
    c = parse("MODE reactive"); CHECK(c.id == BMC_CMD_MODE && c.mode == BMC_MODE_REACTIVE);
    c = parse("MODE FAILSAFE"); CHECK(c.id == BMC_CMD_BAD_ARGS);        /* cannot be commanded */
    c = parse("MODE SELFTEST"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("MODE"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("HEATER 1 40.5"); CHECK(c.id == BMC_CMD_HEATER && c.flag && fabsf(c.f[0] - 40.5f) < 1e-4);
    c = parse("HEATER 0 0.0"); CHECK(c.id == BMC_CMD_HEATER && !c.flag);
    c = parse("HEATER 2 40"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("HEATER 1"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("HEATER 1 nan"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("HEATER 1 inf"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("HEATER 1 12abc"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("FAN AUTO"); CHECK(c.id == BMC_CMD_FAN && c.flag);
    c = parse("FAN 55"); CHECK(c.id == BMC_CMD_FAN && !c.flag && c.f[0] == 55.0f);
    c = parse("SETPOINT 65.5"); CHECK(c.id == BMC_CMD_SETPOINT && fabsf(c.f[0] - 65.5f) < 1e-4);
    c = parse("GAINS 1 0.1 0"); CHECK(c.id == BMC_CMD_GAINS && fabsf(c.f[1] - 0.1f) < 1e-6);
    c = parse("GAINS 1 0.1"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("FF 2"); CHECK(c.id == BMC_CMD_FF && c.i[0] == 2);
    c = parse("FFPARAM 1 0 12.5"); CHECK(c.id == BMC_CMD_FFPARAM && c.i[0] == 1 && c.i[1] == 0 && c.f[0] == 12.5f);
    c = parse("PROFILE CLEAR"); CHECK(c.id == BMC_CMD_PROFILE_CLEAR);
    c = parse("PROFILE ADD 60 25"); CHECK(c.id == BMC_CMD_PROFILE_ADD && c.i[0] == 60 && c.f[0] == 25.0f);
    c = parse("PROFILE ADD 0 25"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("PROFILE PRESET 1"); CHECK(c.id == BMC_CMD_PROFILE_PRESET && c.i[0] == 1);
    c = parse("PROFILE START"); CHECK(c.id == BMC_CMD_PROFILE_START);
    c = parse("PROFILE STOP"); CHECK(c.id == BMC_CMD_PROFILE_STOP);
    c = parse("PROFILE"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("TESTSET OT_LIMIT 50"); CHECK(c.id == BMC_CMD_TESTSET_OT && c.f[0] == 50.0f);
    c = parse("TESTINJECT STALL"); CHECK(c.id == BMC_CMD_TESTINJECT && c.fault == BMC_FAULT_FAN_STALL);
    c = parse("TESTINJECT CLEAR"); CHECK(c.id == BMC_CMD_TESTINJECT && c.flag);
    c = parse("TESTINJECT BOGUS"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("REBOOT"); CHECK(c.id == BMC_CMD_UNKNOWN);
    c = parse(""); CHECK((int)c.id == -1);
    c = parse("   \n"); CHECK((int)c.id == -1);
    c = parse("#7"); CHECK((int)c.id == -1);
    c = parse("#x STATUS"); CHECK(c.id == BMC_CMD_BAD_ARGS);
    c = parse("A B C D E F G H"); CHECK(c.id == BMC_CMD_BAD_ARGS);       /* too many tokens */
    char big[200];
    memset(big, 'A', sizeof(big));
    bmc_cmd_parse(big, sizeof(big), &c); CHECK(c.id == BMC_CMD_BAD_ARGS);   /* over-long line */
}

/* ---- formatting ---------------------------------------------------------------- */
static int count_fields(const char *line)
{
    int n = 1;
    for (const char *p = line; *p && *p != '\n'; p++) if (*p == ',') n++;
    return n;
}

static void test_fmt(void)
{
    char hdr[400], rec[BMC_FMT_RECORD_MAX];
    CHECK(bmc_fmt_header(hdr, sizeof(hdr)) > 0);
    CHECK(count_fields(hdr) == BMC_FMT_COLUMNS);
    CHECK(strncmp(hdr, "bmc_id,seq,t_us,run_ms,mode,plate_c,", 36) == 0);

    telemetry_sample_t s;
    memset(&s, 0, sizeof(s));
    s.seq = 42; s.t_us = 1234567; s.run_ms = -1; s.mode = BMC_MODE_REACTIVE;
    s.plate_c = 41.5f; s.plate_valid = true;
    s.ina_valid = true; s.v48_v = 48.1f; s.i48_a = 2.5f; s.power_w = 120.25f;
    s.fan_rpm[0] = 4200; s.fan_rpm[1] = 4180; s.fan_enabled = true; s.flags = 0x105;
    int n = bmc_fmt_record(rec, sizeof(rec), "bmc0", &s, 7);
    CHECK(n > 0 && rec[n - 1] == '\n');
    CHECK(count_fields(rec) == BMC_FMT_COLUMNS);             /* record and header always agree */
    CHECK(strstr(rec, "bmc0,42,1234567,,REACTIVE,41.5000,,48.100,2.5000,120.250,,,,") != NULL);

    /* nothing valid, run active, worst-case numbers: still the same column count, never overflows */
    memset(&s, 0, sizeof(s));
    s.run_ms = 123456789; s.seq = 0xFFFFFFFFu; s.t_us = 0x7FFFFFFFFFFFFFFFLL;
    s.heater_cmd_pct = 100; s.heater_pct = 100; s.fan_demand_pct = 100; s.fan_target_rpm = 99999;
    s.fan_duty_pct[0] = s.fan_duty_pct[1] = 100; s.fan_rpm[0] = s.fan_rpm[1] = 99999;
    s.ctl_p = -1000; s.ctl_i = 1000; s.ctl_ff = 100; s.flags = 0xFFFFFFFFu; s.fault_mask = 0xFFFFFFFFu;
    n = bmc_fmt_record(rec, sizeof(rec), "bmc-with-a-long-id", &s, 0xFFFFFFFFu);
    CHECK(n > 0 && n < BMC_FMT_RECORD_MAX);
    CHECK(count_fields(rec) == BMC_FMT_COLUMNS);
    CHECK(bmc_fmt_record(rec, 20, "bmc0", &s, 0) == -1);     /* too small a buffer is reported, not truncated silently */

    bmc_event_t e;
    memset(&e, 0, sizeof(e));
    e.id = 9; e.type = BMC_EV_FAILSAFE_ENTER; e.t_ms = 18340; e.run_ms = -1;
    snprintf(e.code, sizeof(e.code), "OVER_TEMP");
    snprintf(e.detail, sizeof(e.detail), "plate_c=120.3 mode=REACTIVE");
    char line[300];
    n = bmc_fmt_event(line, sizeof(line), &e);
    CHECK(n > 0);
    CHECK_STR(line, "EVENT,9,18340000,,FAILSAFE_ENTER,OVER_TEMP,plate_c=120.3 mode=REACTIVE\n");
}

void run_core_tests(void)
{
    SUITE("pid");        test_pid();
    SUITE("fanrpm");     test_fanrpm();
    SUITE("stall");      test_stall();
    SUITE("profile");    test_profile();
    SUITE("feedforward"); test_ff();
    SUITE("cmd-parse");  test_cmd();
    SUITE("format");     test_fmt();
}
