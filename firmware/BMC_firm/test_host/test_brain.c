#include "bmc_brain.h"
#include "bmc_fanrpm.h"
#include "bmc_stall.h"
#include "test_util.h"

/* A small simulated world around the brain: two fans (ideal, with fault switches),
 * the sensors' consecutive-failure counters, and a first-order thermal plant. It
 * mirrors what the supervisor task does on hardware. */

typedef struct {
    brain_t b;
    uint32_t now;
    uint32_t seq;
    int tick;
    /* switches */
    bool plate_ok, ina_ok, permit, interlock;
    bool fan_dead[2];
    bool tach_swapped;
    float i48, v48;
    /* state */
    float plate_c;
    float duty[2];
    uint8_t plate_fails, ina_fails;
    bmc_stall_t stall[2];
    brain_out_t out;
    /* thermal plant */
    bool plant;
    float ambient_c, heater_w_max, cap_j_per_k;
    float peak_c;
} world_t;

static const float FAN_MAX[2] = { BMC_FAN1_MAX_RPM, BMC_FAN2_MAX_RPM };

static void world_init(world_t *w, bool test_hooks)
{
    memset(w, 0, sizeof(*w));
    w->now = 5000;
    brain_init(&w->b, w->now, test_hooks);
    w->plate_ok = w->ina_ok = w->permit = w->interlock = true;
    w->plate_c = 25.0f;
    w->v48 = 48.0f;
    w->ambient_c = 25.0f;
    w->heater_w_max = 100.0f;
    w->cap_j_per_k = 200.0f;
    for (int i = 0; i < 2; i++) bmc_stall_init(&w->stall[i]);
    w->peak_c = w->plate_c;
}

static void tick(world_t *w)
{
    w->now += BMC_SAFETY_PERIOD_MS;
    w->tick++;
    bool new_sample = (w->tick % 2) == 0;

    float rpm[2];
    for (int i = 0; i < 2; i++) {
        rpm[i] = (w->fan_dead[i] || !w->interlock) ? 0.0f : w->duty[i] / 100.0f * FAN_MAX[i];
    }
    float rep[2] = { rpm[0], rpm[1] };
    if (w->tach_swapped) { rep[0] = rpm[1]; rep[1] = rpm[0]; }

    if (new_sample) {
        w->seq++;
        w->plate_fails = w->plate_ok ? 0 : (uint8_t)(w->plate_fails < 250 ? w->plate_fails + 1 : 250);
        w->ina_fails = w->ina_ok ? 0 : (uint8_t)(w->ina_fails < 250 ? w->ina_fails + 1 : 250);
    }
    for (int i = 0; i < 2; i++) {
        bmc_stall_update(&w->stall[i], w->now, w->interlock, w->duty[i], (uint32_t)rep[i], true);
    }

    brain_in_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = w->now;
    in.sample_seq = w->seq;
    in.safety.plate_valid = w->plate_ok;
    in.safety.plate_c = w->plate_c;
    in.safety.plate_fails = w->plate_fails;
    in.safety.ina_valid = w->ina_ok;
    in.safety.ina_fails = w->ina_fails;
    in.safety.fan_interlock_ok = w->interlock;
    in.safety.heater_permit = w->permit;
    in.safety.fan_stalled[0] = w->stall[0].stalled;
    in.safety.fan_stalled[1] = w->stall[1].stalled;
    in.v48_v = w->v48;
    in.i48_a = w->i48;
    in.power_w = w->v48 * w->i48;
    for (int i = 0; i < 2; i++) {
        in.fan[i].window_full = true;
        in.fan[i].rpm = (uint32_t)rep[i];
        in.fan_duty_applied[i] = w->duty[i];
    }
    brain_step(&w->b, &in, &w->out);

    /* apply the outputs the way the supervisor does */
    for (int i = 0; i < 2; i++) {
        float d;
        if (w->out.fan_full) {
            d = BMC_FAILSAFE_FAN_DUTY_PCT;
        } else if (w->out.fan_selftest) {
            d = w->out.fan_test_duty[i];
        } else {
            d = bmc_fanrpm_target(w->out.fan_demand_pct, BMC_RPM_REF) / FAN_MAX[i] * 100.0f;
        }
        w->duty[i] = w->interlock ? d : 0.0f;
    }

    if (w->plant) {                                          /* first-order plate model */
        float frac = (rpm[0] / BMC_RPM_REF + rpm[1] / BMC_RPM_REF) / 2.0f;
        if (frac > 1.0f) frac = 1.0f;
        float g = 0.3f + 1.5f * frac;                        /* W/K, rises with airflow */
        float p = w->heater_w_max * w->out.heater_pct / 100.0f;
        w->plate_c += (p - g * (w->plate_c - w->ambient_c)) / w->cap_j_per_k * (BMC_SAFETY_PERIOD_MS / 1000.0f);
        w->i48 = p / w->v48 + 0.2f;
        if (w->plate_c > w->peak_c) w->peak_c = w->plate_c;
    }
}

static void run(world_t *w, double seconds)
{
    int n = (int)(seconds * 1000.0 / BMC_SAFETY_PERIOD_MS);
    for (int i = 0; i < n; i++) tick(w);
}

static char g_reply[600];
static const char *cmd(world_t *w, const char *line)
{
    bmc_cmd_t c;
    g_reply[0] = '\0';
    if (!bmc_cmd_parse(line, strlen(line), &c)) return g_reply;
    brain_command(&w->b, &c, w->now, g_reply, sizeof(g_reply));
    return g_reply;
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool has_event(const world_t *w, bmc_ev_type_t type, const char *code)
{
    bmc_event_t all[BMC_EV_RING];
    int n = bmc_ev_recent(&w->b.ev, all, BMC_EV_RING);
    for (int i = 0; i < n; i++) {
        if (all[i].type == type && (!code || !strcmp(all[i].code, code))) return true;
    }
    return false;
}

static void pass_selftest(world_t *w)
{
    CHECK(starts(cmd(w, "SELFTEST"), "OK SELFTEST"));
    run(w, 12.0);
}

static void to_char_mode(world_t *w)
{
    pass_selftest(w);
    CHECK(starts(cmd(w, "MODE CHAR"), "OK MODE"));
}

/* ------------------------------------------------------------------------------ */

static void test_selftest_pass(void)
{
    world_t w; world_init(&w, true);
    CHECK(w.out.st.mode == BMC_MODE_STANDBY);
    CHECK_STR(cmd(&w, "MODE CHAR"), "ERR MODE no_selftest");
    CHECK(starts(cmd(&w, "SELFTEST"), "OK SELFTEST"));
    tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_SELFTEST);
    CHECK(w.out.heater_pct == 0.0f && !w.out.heater_enable);
    CHECK(w.out.fan_selftest);
    CHECK_STR(cmd(&w, "SELFTEST"), "ERR SELFTEST bad_state");
    CHECK_STR(cmd(&w, "MODE STANDBY"), "ERR MODE busy");
    run(&w, 12.0);
    CHECK(w.out.st.mode == BMC_MODE_STANDBY);
    CHECK(w.out.st.selftest_ok);
    CHECK(has_event(&w, BMC_EV_SELFTEST, "PASS"));
    CHECK(starts(cmd(&w, "MODE CHAR"), "OK MODE CHAR"));
}

static void selftest_fails(const char *label, void (*setup)(world_t *), const char *expect)
{
    world_t w; world_init(&w, true);
    setup(&w);
    cmd(&w, "SELFTEST");
    run(&w, 12.0);
    bmc_event_t all[BMC_EV_RING];
    int n = bmc_ev_recent(&w.b.ev, all, BMC_EV_RING);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (all[i].type == BMC_EV_SELFTEST && !strcmp(all[i].code, "FAIL") && strstr(all[i].detail, expect)) found = true;
    }
    if (!found) printf("  selftest case '%s' did not report '%s'\n", label, expect);
    CHECK(found);
    CHECK(w.out.st.mode == BMC_MODE_STANDBY);
    CHECK(!w.out.st.selftest_ok);
    CHECK_STR(cmd(&w, "MODE CHAR"), "ERR MODE no_selftest");           /* failure blocks actuating modes */
    CHECK_STR(cmd(&w, "MODE REACTIVE"), "ERR MODE no_selftest");
}

static void s_plate_missing(world_t *w) { w->plate_ok = false; w->plate_fails = 255; }
static void s_ina_missing(world_t *w) { w->ina_ok = false; w->ina_fails = 255; }
static void s_plate_hot(world_t *w) { w->plate_c = 80.0f; }
static void s_interlock_open(world_t *w) { w->interlock = false; }
static void s_fan1_dead(world_t *w) { w->fan_dead[0] = true; }
static void s_fan2_dead(world_t *w) { w->fan_dead[1] = true; }
static void s_swapped(world_t *w) { w->tach_swapped = true; }

static void test_selftest_failures(void)
{
    selftest_fails("plate sensor missing", s_plate_missing, "tmp117_plate:no_response");
    selftest_fails("ina238 missing", s_ina_missing, "ina238:no_response");
    selftest_fails("plate implausible", s_plate_hot, "tmp117_plate:implausible");
    selftest_fails("fan interlock open", s_interlock_open, "fan_interlock:open");
    selftest_fails("fan 1 disconnected", s_fan1_dead, "fan1:no_tach");
    selftest_fails("fan 2 disconnected", s_fan2_dead, "fan2:no_tach");
    selftest_fails("tach wires swapped", s_swapped, "tach_swapped_with_fan2");
}

static void test_arbitration(void)
{
    world_t w; world_init(&w, true);
    CHECK(starts(cmd(&w, "HEATER 1 40"), "ERR HEATER mode"));        /* STANDBY: no heat */
    CHECK_STR(cmd(&w, "HEATER 0 0"), "");                            /* "off" is silent in any mode */
    to_char_mode(&w);
    CHECK_STR(cmd(&w, "HEATER 1 40"), "");                           /* keep-alive: no reply traffic */
    tick(&w);
    CHECK_NEAR(w.out.heater_pct, 40.0, 1e-4);
    CHECK(w.out.heater_enable);
    CHECK(w.out.st.flags & TELEM_FLAG_HEATER_ACTIVE);

    w.permit = false; tick(&w);                                      /* hardware permit open */
    CHECK(w.out.heater_pct == 0.0f && !w.out.heater_enable);
    CHECK(!(w.out.st.flags & TELEM_FLAG_HEATER_PERMIT));
    w.permit = true; cmd(&w, "HEATER 1 40"); tick(&w);
    CHECK_NEAR(w.out.heater_pct, 40.0, 1e-4);

    w.interlock = false; tick(&w);                                   /* fan interlock open */
    CHECK(w.out.heater_pct == 0.0f);
    CHECK(w.out.st.flags & TELEM_FLAG_FAN_DISABLED);
    w.interlock = true; cmd(&w, "HEATER 1 40"); tick(&w);
    CHECK_NEAR(w.out.heater_pct, 40.0, 1e-4);

    w.plate_ok = false; w.plate_fails = 1; tick(&w);                 /* temperature unreadable: no blind heating */
    CHECK(w.out.heater_pct == 0.0f);
    CHECK(w.out.st.mode == BMC_MODE_CHAR);                           /* but not yet a trip (debounce) */
    w.plate_ok = true; w.plate_fails = 0; cmd(&w, "HEATER 1 40"); tick(&w);
    CHECK_NEAR(w.out.heater_pct, 40.0, 1e-4);

    run(&w, 0.5);                                                    /* lease ran out (no keep-alive) */
    CHECK(w.out.heater_pct > 0.0f);
    run(&w, 0.7);
    CHECK(w.out.heater_pct == 0.0f);
    cmd(&w, "HEATER 1 150"); tick(&w);
    CHECK_NEAR(w.out.heater_pct, 100.0, 1e-4);                       /* clamped */
    cmd(&w, "HEATER 0 0"); tick(&w);
    CHECK(w.out.heater_pct == 0.0f);
}

static void test_failsafe_overrides_everything(void)
{
    world_t w; world_init(&w, true);
    to_char_mode(&w);
    cmd(&w, "FAN 20");
    cmd(&w, "HEATER 1 80"); tick(&w);
    CHECK_NEAR(w.out.heater_pct, 80.0, 1e-4);

    w.plate_c = 121.0f;
    cmd(&w, "HEATER 1 80");                                          /* even with a fresh request ... */
    tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_FAILSAFE);
    CHECK(w.out.heater_pct == 0.0f && !w.out.heater_enable);
    CHECK(w.out.fan_full);
    CHECK_NEAR(w.out.fan_demand_pct, 100.0, 1e-4);
    CHECK(w.out.st.fault_code == BMC_FAULT_OVER_TEMP);
    CHECK(has_event(&w, BMC_EV_FAILSAFE_ENTER, "OVER_TEMP"));
    for (int i = 0; i < 2; i++) CHECK_NEAR(w.duty[i], 100.0, 1e-4); /* both fans open-loop at full duty */

    CHECK(starts(cmd(&w, "HEATER 1 80"), "ERR HEATER mode"));
    CHECK_STR(cmd(&w, "MODE CHAR"), "ERR MODE latched");
    CHECK_STR(cmd(&w, "SELFTEST"), "ERR SELFTEST latched");
    CHECK_STR(cmd(&w, "ACK"), "ERR ACK not_safe");
    w.plate_c = 120.0f; run(&w, 1.0);
    CHECK(w.out.heater_pct == 0.0f && w.out.fan_full);               /* still latched while hot */

    w.plate_c = 40.0f;                                               /* cooled: safe after the hold time */
    run(&w, 4.0);
    CHECK_STR(cmd(&w, "ACK"), "ERR ACK not_safe");
    run(&w, 2.0);
    CHECK(w.out.st.safe_to_handle);
    CHECK(w.out.fan_full);                                           /* airflow held until acknowledged */
    CHECK(starts(cmd(&w, "ACK"), "OK ACK"));
    tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_STANDBY && !w.out.st.selftest_ok);
    CHECK(!w.out.fan_full);
    CHECK(has_event(&w, BMC_EV_ACK_OK, NULL));
    CHECK_STR(cmd(&w, "MODE CHAR"), "ERR MODE no_selftest");          /* self-test required before heat again */
    pass_selftest(&w);
    CHECK(starts(cmd(&w, "MODE CHAR"), "OK MODE CHAR"));
}

static int ticks_to_failsafe(world_t *w, int max_ticks)
{
    for (int i = 1; i <= max_ticks; i++) {
        tick(w);
        if (w->out.st.mode == BMC_MODE_FAILSAFE) return i;
    }
    return -1;
}

static void test_fault_latencies(void)
{
    /* plate sensor unplugged: ~3 sample periods (300 ms) + one safety period */
    world_t w; world_init(&w, true);
    to_char_mode(&w);
    w.plate_ok = false;
    int t = ticks_to_failsafe(&w, 40);
    CHECK(t > 0 && t * BMC_SAFETY_PERIOD_MS <= 400);
    CHECK(w.out.st.fault_code == BMC_FAULT_I2C || w.out.st.fault_code == BMC_FAULT_TEMP_LOSS);
    CHECK(w.out.st.fault_code == BMC_FAULT_TEMP_LOSS);               /* only the plate failed: not a bus fault */

    world_init(&w, true); to_char_mode(&w);                          /* INA238 unplugged */
    w.ina_ok = false;
    t = ticks_to_failsafe(&w, 40);
    CHECK(t > 0 && t * BMC_SAFETY_PERIOD_MS <= 400);
    CHECK(w.out.st.fault_code == BMC_FAULT_TELEM_LOSS);

    world_init(&w, true); to_char_mode(&w);                          /* whole bus dead */
    w.plate_ok = false; w.ina_ok = false;
    t = ticks_to_failsafe(&w, 40);
    CHECK(t > 0 && w.out.st.fault_code == BMC_FAULT_I2C);

    world_init(&w, true); to_char_mode(&w);                          /* fan disconnected under load */
    cmd(&w, "FAN 50");
    run(&w, 1.0);
    CHECK(w.out.st.mode == BMC_MODE_CHAR);
    w.fan_dead[1] = true;
    t = ticks_to_failsafe(&w, 40);
    CHECK(t > 0 && t * BMC_SAFETY_PERIOD_MS <= 700);
    CHECK(w.out.st.fault_code == BMC_FAULT_FAN_STALL);
    CHECK(w.out.fan_full && w.out.heater_pct == 0.0f);

    world_init(&w, true); to_char_mode(&w);                          /* sensor task stops delivering */
    cmd(&w, "HEATER 1 30");
    w.seq = w.seq;                                                    /* (age is supplied by the caller) */
    brain_in_t in = w.b.last_in;
    in.now_ms = w.now += 50;
    in.safety.sensor_age_ms = BMC_SENSOR_STALE_MS + 10;
    brain_step(&w.b, &in, &w.out);
    CHECK(w.out.st.mode == BMC_MODE_FAILSAFE && w.out.st.fault_code == BMC_FAULT_SENSOR_STALE);
}

static void test_profile_run(void)
{
    world_t w; world_init(&w, true);
    CHECK(starts(cmd(&w, "PROFILE PRESET 2"), "OK PROFILE"));
    CHECK_STR(cmd(&w, "PROFILE START"), "ERR PROFILE mode");         /* not in an actuating mode */
    to_char_mode(&w);
    cmd(&w, "PROFILE CLEAR");
    CHECK_STR(cmd(&w, "PROFILE START"), "ERR PROFILE no_profile");
    cmd(&w, "PROFILE PRESET 2");
    w.plate_c = 55.0f; tick(&w);
    CHECK_STR(cmd(&w, "PROFILE START"), "ERR PROFILE not_at_ambient");
    w.plate_c = 26.0f; tick(&w);
    CHECK(starts(cmd(&w, "#5 PROFILE START"), "#5 OK PROFILE"));       /* token echoed */
    CHECK_STR(cmd(&w, "PROFILE START"), "ERR PROFILE profile_running");
    CHECK(has_event(&w, BMC_EV_RUN_START, "CHAR"));

    CHECK_STR(cmd(&w, "SETPOINT 55"), "ERR SETPOINT profile_running");  /* parameters frozen during a run */
    CHECK_STR(cmd(&w, "GAINS 1 1 0"), "ERR GAINS profile_running");
    CHECK_STR(cmd(&w, "FF 1"), "ERR FF profile_running");
    CHECK_STR(cmd(&w, "MODE REACTIVE"), "ERR MODE profile_running");

    run(&w, 5.0);
    CHECK(w.out.heater_pct == 0.0f && w.out.st.profile_running);
    CHECK(w.out.st.run_ms >= 4900 && w.out.st.run_ms <= 5100);
    run(&w, 10.0);                                                   /* t = 15 s: in the 50 % step */
    CHECK_NEAR(w.out.heater_pct, 50.0, 1e-4);
    run(&w, 30.0);                                                   /* t = 45 s: back to idle */
    CHECK(w.out.heater_pct == 0.0f);
    run(&w, 20.0);                                                   /* t = 65 s: finished */
    CHECK(!w.out.st.profile_running && w.out.st.run_ms < 0);
    CHECK(has_event(&w, BMC_EV_RUN_END, "complete"));

    /* aborts: STOP, and failsafe */
    w.plate_c = 26.0f;
    cmd(&w, "PROFILE PRESET 1"); cmd(&w, "PROFILE START");
    run(&w, 65.0);
    CHECK_NEAR(w.out.heater_pct, 100.0, 1e-4);
    CHECK(starts(cmd(&w, "PROFILE STOP"), "OK PROFILE"));
    tick(&w);
    CHECK(w.out.heater_pct == 0.0f && !w.out.st.profile_running);
    CHECK(has_event(&w, BMC_EV_RUN_END, "stopped"));

    w.plate_c = 26.0f;
    cmd(&w, "PROFILE PRESET 1"); cmd(&w, "PROFILE START");
    run(&w, 65.0);
    CHECK_NEAR(w.out.heater_pct, 100.0, 1e-4);
    w.plate_c = 125.0f; tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_FAILSAFE && w.out.heater_pct == 0.0f && !w.out.st.profile_running);
    CHECK(has_event(&w, BMC_EV_RUN_END, "failsafe"));
}

static void test_standby_fan_curve(void)
{
    world_t w; world_init(&w, true);
    w.plate_c = 62.5f;
    run(&w, 0.5);
    CHECK_NEAR(w.out.fan_demand_pct, 50.0, 0.5);                     /* legacy linear curve */
    CHECK(w.out.heater_pct == 0.0f && !w.out.heater_enable);
    w.plate_ok = false; w.plate_fails = 1;
    run(&w, 0.5);
    CHECK_NEAR(w.out.fan_demand_pct, 0.0, 1e-4);
}

static void test_test_hooks(void)
{
    world_t w; world_init(&w, false);                                 /* production build */
    CHECK_STR(cmd(&w, "TESTSET OT 50"), "ERR TESTSET disabled");
    CHECK_STR(cmd(&w, "TESTINJECT STALL"), "ERR TESTINJECT disabled");
    tick(&w);
    CHECK(!(w.out.st.flags & TELEM_FLAG_TEST_HOOKS));

    world_init(&w, true);
    tick(&w);
    CHECK(w.out.st.flags & TELEM_FLAG_TEST_HOOKS);                    /* never hidden from the log */
    to_char_mode(&w);
    CHECK(starts(cmd(&w, "TESTSET OT 45"), "OK TESTSET"));
    tick(&w);
    CHECK(w.out.st.flags & TELEM_FLAG_OT_LOWERED);
    CHECK_NEAR(w.out.st.ot_limit_c, 45.0, 1e-4);
    w.plate_c = 46.0f; tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_FAILSAFE && w.out.st.fault_code == BMC_FAULT_OVER_TEMP);

    world_init(&w, true); to_char_mode(&w);
    CHECK(starts(cmd(&w, "TESTINJECT STALL"), "OK TESTINJECT"));
    tick(&w);
    CHECK(w.out.st.mode == BMC_MODE_FAILSAFE && w.out.st.fault_code == BMC_FAULT_FAN_STALL);
}

static void test_replies(void)
{
    world_t w; world_init(&w, true);
    CHECK(starts(cmd(&w, "STATUS"), "OK STATUS mode=STANDBY selftest=0 fault=NONE"));
    CHECK(starts(cmd(&w, "#12 STATUS"), "#12 OK STATUS"));
    CHECK_STR(cmd(&w, "#3 REBOOT"), "#3 ERR ? unknown");
    CHECK_STR(cmd(&w, "MODE BANANA"), "ERR ? args");
    CHECK_STR(cmd(&w, "SETPOINT 5"), "ERR SETPOINT range");
    CHECK_STR(cmd(&w, "SETPOINT 200"), "ERR SETPOINT range");
    CHECK(starts(cmd(&w, "SETPOINT 62"), "OK SETPOINT"));
    CHECK_STR(cmd(&w, "GAINS -1 0 0"), "ERR GAINS range");
    CHECK(starts(cmd(&w, "GAINS 4 0.1 0"), "OK GAINS"));
    CHECK_STR(cmd(&w, "FF 9"), "ERR FF range");
    CHECK(starts(cmd(&w, "FF 2"), "OK FF 2 table"));
    CHECK_STR(cmd(&w, "FFPARAM 1 99 1"), "ERR FFPARAM range");
    CHECK(starts(cmd(&w, "FFPARAM 1 0 12"), "OK FFPARAM"));
    CHECK_STR(cmd(&w, "FAN 50"), "ERR FAN mode");
    CHECK_STR(cmd(&w, "HELLO"), "");
    cmd(&w, "SELFTEST"); run(&w, 12.0);
    const char *r = cmd(&w, "EVENTS");
    CHECK(starts(r, "OK EVENTS count="));
    CHECK(strstr(r, "\nEVENT,") != NULL);
    CHECK(strstr(r, "SELFTEST,PASS") != NULL);
}

/* ---- closed loop against the thermal plant ------------------------------------------ */

static void run_loop(world_t *w, const char *mode, double seconds)
{
    w->plant = true;
    to_char_mode(w);
    cmd(w, "MODE STANDBY");
    CHECK(starts(cmd(w, mode), "OK MODE"));
    cmd(w, "PROFILE CLEAR");
    cmd(w, "PROFILE ADD 3000 50");                                   /* constant 50 W load */
    CHECK(starts(cmd(w, "PROFILE START"), "OK PROFILE START"));
    run(w, seconds);
}

static void test_reactive_loop(void)
{
    world_t w; world_init(&w, true);
    cmd(&w, "SETPOINT 60");
    cmd(&w, "GAINS 5 0.2 0");
    w.plant = true;
    pass_selftest(&w);
    w.plant = false;
    run_loop(&w, "MODE REACTIVE", 900.0);
    CHECK(w.out.st.mode == BMC_MODE_REACTIVE);                        /* never tripped */
    CHECK(fabsf(w.plate_c - 60.0f) < 3.0f);                           /* regulates to the setpoint */
    CHECK(w.peak_c < 70.0f);
    CHECK(w.out.st.fan_demand_pct > 20.0f && w.out.st.fan_demand_pct < 95.0f);
    CHECK_NEAR(w.out.fan_demand_pct, w.out.st.fan_demand_pct, 1e-4);
    CHECK(fabsf(w.out.st.ctl_ff) < 1e-6);                             /* reactive uses no feedforward */
    /* the two different fans run at the same RPM */
    CHECK_NEAR(w.duty[0] / 100.0f * BMC_FAN1_MAX_RPM, w.duty[1] / 100.0f * BMC_FAN2_MAX_RPM, 1.0);
}

static void test_proactive_loop(void)
{
    world_t w; world_init(&w, true);
    cmd(&w, "SETPOINT 60");
    cmd(&w, "GAINS 5 0.2 0");
    cmd(&w, "FF 1");
    cmd(&w, "FFPARAM 1 0 20");                                        /* 20 %/A on a ~1.2 A step */
    w.plant = true;
    pass_selftest(&w);
    w.plant = false;
    run_loop(&w, "MODE PROACTIVE", 900.0);
    CHECK(w.out.st.mode == BMC_MODE_PROACTIVE);
    CHECK(fabsf(w.plate_c - 60.0f) < 3.0f);
    CHECK(w.peak_c < 70.0f);
    CHECK(w.out.st.ff_variant == 1);
    CHECK(w.out.st.ctl_ff > 5.0f);                                    /* feedforward is active and logged */
}

static void test_runtime_mode_switch_without_reset(void)
{
    /* FSR 3.2.1.5: modes selectable at runtime; no controller reset between selections */
    world_t w; world_init(&w, true);
    pass_selftest(&w);
    uint32_t boots_before = w.b.ev.next_id;
    const char *modes[] = { "MODE REACTIVE", "MODE PROACTIVE", "MODE CHAR", "MODE REACTIVE" };
    for (unsigned i = 0; i < 4; i++) {
        CHECK(starts(cmd(&w, modes[i]), "OK MODE"));
        run(&w, 1.0);
        CHECK(w.out.st.selftest_ok);
    }
    CHECK(w.out.st.mode == BMC_MODE_REACTIVE);
    CHECK(w.b.ev.next_id > boots_before);                              /* mode changes are logged */
    CHECK(w.out.st.fault_code == BMC_FAULT_NONE);
}

void run_brain_tests(void)
{
    SUITE("brain: self-test");        test_selftest_pass(); test_selftest_failures();
    SUITE("brain: arbitration");      test_arbitration();
    SUITE("brain: failsafe");         test_failsafe_overrides_everything(); test_fault_latencies();
    SUITE("brain: profile");          test_profile_run();
    SUITE("brain: standby fans");     test_standby_fan_curve();
    SUITE("brain: test hooks");       test_test_hooks();
    SUITE("brain: command replies");  test_replies();
    SUITE("brain: closed loop");      test_reactive_loop(); test_proactive_loop();
    SUITE("brain: runtime modes");    test_runtime_mode_switch_without_reset();
}
