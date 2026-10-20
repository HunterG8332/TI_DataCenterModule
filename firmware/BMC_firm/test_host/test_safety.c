#include <stdlib.h>
#include "bmc_safety.h"
#include "test_util.h"

static bmc_events_t ev;
static safety_t s;
static uint32_t now;

static safety_in_t healthy(float plate_c)
{
    safety_in_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.plate_valid = true;
    in.plate_c = plate_c;
    in.ina_valid = true;
    in.fan_interlock_ok = true;
    in.heater_permit = true;
    return in;
}

static safety_out_t step(safety_in_t in)
{
    safety_out_t out;
    in.now_ms = now;
    safety_step(&s, &in, &out);
    return out;
}

static void fresh(void)
{
    bmc_ev_init(&ev);
    safety_init(&s, &ev, 0);
    now = 1000;
}

/* STANDBY -> SELFTEST -> pass -> CHAR */
static void get_to_char(void)
{
    CHECK(safety_begin_selftest(&s) == NULL);
    safety_selftest_done(&s, true);
    CHECK(safety_request_mode(&s, BMC_MODE_CHAR, false, true) == NULL);
}

static bmc_event_t last_event(void)
{
    bmc_event_t all[BMC_EV_RING];
    int n = bmc_ev_recent(&ev, all, BMC_EV_RING);
    return all[n - 1];
}

static void test_boot_and_standby(void)
{
    fresh();
    safety_out_t o = step(healthy(25));
    CHECK(o.mode == BMC_MODE_STANDBY);
    CHECK(!o.force_safe && !o.heater_allowed && !o.selftest_ok);
    CHECK(o.fault_code == BMC_FAULT_NONE);
    CHECK_NEAR(o.ot_limit_c, 120.0, 0.001);

    /* Non-thermal faults are observable but do not latch in STANDBY: there is no heat to remove,
     * and the self-test must be able to name them. */
    safety_in_t in = healthy(25);
    in.plate_fails = 3;
    in.plate_valid = false;
    o = step(in);
    CHECK(o.mode == BMC_MODE_STANDBY);
    CHECK(o.fault_mask & BMC_FAULT_BIT(BMC_FAULT_TEMP_LOSS));
}

static void test_over_temp_any_mode(void)
{
    fresh();
    safety_out_t o = step(healthy(119.9f));
    CHECK(o.mode == BMC_MODE_STANDBY);
    o = step(healthy(120.0f));
    CHECK(o.mode == BMC_MODE_FAILSAFE);
    CHECK(o.force_safe && !o.heater_allowed);
    CHECK(o.fault_code == BMC_FAULT_OVER_TEMP);
    bmc_event_t e = last_event();
    CHECK(e.type == BMC_EV_MODE_CHANGE);
    bmc_event_t all[BMC_EV_RING];
    int n = bmc_ev_recent(&ev, all, BMC_EV_RING);
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (all[i].type == BMC_EV_FAILSAFE_ENTER) found = i;
    }
    CHECK(found >= 0);
    if (found >= 0) {
        CHECK_STR(all[found].code, "OVER_TEMP");
        CHECK(strstr(all[found].detail, "plate_c=120.0") != NULL);
        CHECK(all[found].t_ms == now);                          /* timestamped at the moment of detection */
    }
}

static void test_each_fault_trips_in_actuating_mode(void)
{
    struct { const char *name; bmc_fault_t expect; } cases[] = {
        { "temp_loss", BMC_FAULT_TEMP_LOSS }, { "telem_loss", BMC_FAULT_TELEM_LOSS },
        { "i2c_stuck", BMC_FAULT_I2C }, { "both_sensors", BMC_FAULT_I2C },
        { "stall1", BMC_FAULT_FAN_STALL }, { "stall2", BMC_FAULT_FAN_STALL },
        { "stale", BMC_FAULT_SENSOR_STALE }, { "garbage_low", BMC_FAULT_TEMP_LOSS },
        { "over_temp", BMC_FAULT_OVER_TEMP },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh();
        get_to_char();
        CHECK(step(healthy(30)).mode == BMC_MODE_CHAR);
        safety_in_t in = healthy(30);
        const char *n = cases[i].name;
        if (!strcmp(n, "temp_loss")) { in.plate_valid = false; in.plate_fails = 3; }
        if (!strcmp(n, "telem_loss")) { in.ina_valid = false; in.ina_fails = 3; }
        if (!strcmp(n, "i2c_stuck")) { in.i2c_stuck = true; }
        if (!strcmp(n, "both_sensors")) { in.plate_valid = false; in.plate_fails = 3; in.ina_valid = false; in.ina_fails = 3; }
        if (!strcmp(n, "stall1")) { in.fan_stalled[0] = true; }
        if (!strcmp(n, "stall2")) { in.fan_stalled[1] = true; }
        if (!strcmp(n, "stale")) { in.sensor_age_ms = BMC_SENSOR_STALE_MS + 1; }
        if (!strcmp(n, "garbage_low")) { in.plate_c = -40.0f; }
        if (!strcmp(n, "over_temp")) { in.plate_c = 121.0f; }
        safety_out_t o = step(in);
        if (o.mode != BMC_MODE_FAILSAFE || o.fault_code != cases[i].expect) {
            printf("  case %s: mode=%s code=%s\n", n, bmc_mode_name(o.mode), bmc_fault_name(o.fault_code));
        }
        CHECK(o.mode == BMC_MODE_FAILSAFE);
        CHECK(o.fault_code == cases[i].expect);
        CHECK(o.force_safe);
    }
}

static void test_debounce(void)
{
    fresh();
    get_to_char();
    safety_in_t in = healthy(30);
    in.plate_valid = false;
    in.plate_fails = BMC_FAIL_READS_TRIP - 1;
    CHECK(step(in).mode == BMC_MODE_CHAR);                 /* two bad reads: not yet */
    in.plate_fails = BMC_FAIL_READS_TRIP;
    CHECK(step(in).mode == BMC_MODE_FAILSAFE);             /* the third trips it */
}

static void test_first_fault_wins_and_priority(void)
{
    fresh();
    get_to_char();
    safety_in_t in = healthy(30);
    in.fan_stalled[0] = true;
    CHECK(step(in).fault_code == BMC_FAULT_FAN_STALL);
    in.plate_fails = 5;                                    /* a later fault must not overwrite the cause */
    in.plate_valid = false;
    safety_out_t o = step(in);
    CHECK(o.fault_code == BMC_FAULT_FAN_STALL);
    CHECK(o.fault_mask & BMC_FAULT_BIT(BMC_FAULT_TEMP_LOSS));
}

static void test_selftest_and_mode_rules(void)
{
    fresh();
    CHECK_STR(safety_request_mode(&s, BMC_MODE_CHAR, false, true), "no_selftest");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_REACTIVE, false, true), "no_selftest");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_FAILSAFE, false, true), "bad_mode");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_SELFTEST, false, true), "bad_mode");
    CHECK(safety_begin_selftest(&s) == NULL);
    CHECK(s.mode == BMC_MODE_SELFTEST);
    CHECK_STR(safety_request_mode(&s, BMC_MODE_STANDBY, false, true), "busy");
    CHECK_STR(safety_begin_selftest(&s), "bad_state");
    safety_selftest_done(&s, false);
    CHECK(s.mode == BMC_MODE_STANDBY && !s.selftest_ok);
    CHECK_STR(safety_request_mode(&s, BMC_MODE_CHAR, false, true), "no_selftest");
    CHECK(safety_begin_selftest(&s) == NULL);
    safety_selftest_done(&s, true);
    CHECK(s.selftest_ok);
    CHECK(safety_request_mode(&s, BMC_MODE_REACTIVE, false, true) == NULL);
    CHECK_STR(safety_request_mode(&s, BMC_MODE_PROACTIVE, true, true), "profile_running");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_PROACTIVE, false, false), "heater_on");
    CHECK(safety_request_mode(&s, BMC_MODE_PROACTIVE, false, true) == NULL);
    CHECK(safety_request_mode(&s, BMC_MODE_STANDBY, true, false) == NULL);   /* standby is always reachable */
    CHECK(s.mode == BMC_MODE_STANDBY);
}

static void test_selftest_blocked_in_failsafe(void)
{
    fresh();
    step(healthy(125));
    CHECK(s.mode == BMC_MODE_FAILSAFE);
    CHECK_STR(safety_begin_selftest(&s), "latched");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_STANDBY, false, true), "latched");
    CHECK_STR(safety_request_mode(&s, BMC_MODE_CHAR, false, true), "latched");
}

static void test_latch_cooldown_and_ack(void)
{
    fresh();
    get_to_char();
    step(healthy(30));
    safety_in_t hot = healthy(121);
    CHECK(step(hot).mode == BMC_MODE_FAILSAFE);
    CHECK_STR(safety_ack(&s), "not_safe");                   /* still hot */

    /* cools to 59 C: must hold for BMC_SAFE_CLEAR_HOLD_MS before it counts */
    now += 100;
    safety_out_t o = step(healthy(59.0f));
    CHECK(!o.safe_to_handle);
    CHECK_STR(safety_ack(&s), "not_safe");
    now += BMC_SAFE_CLEAR_HOLD_MS - 100;
    o = step(healthy(59.0f));
    CHECK(!o.safe_to_handle);                                 /* 4.9 s */
    now += 100;
    o = step(healthy(59.0f));
    CHECK(o.safe_to_handle);
    CHECK(last_event().type == BMC_EV_SAFE_TO_HANDLE);

    /* a still-active fault blocks the ACK even when cool */
    safety_in_t fan = healthy(59.0f);
    fan.fan_stalled[1] = true;
    step(fan);
    CHECK_STR(safety_ack(&s), "fault_active");

    o = step(healthy(59.0f));
    CHECK(o.safe_to_handle);
    CHECK(safety_ack(&s) == NULL);
    CHECK(s.mode == BMC_MODE_STANDBY);
    CHECK(!s.selftest_ok);                                    /* full self-test needed again */
    CHECK(s.fault_code == BMC_FAULT_NONE);
    CHECK_STR(safety_request_mode(&s, BMC_MODE_CHAR, false, true), "no_selftest");
    CHECK_STR(safety_ack(&s), "bad_state");
}

static void test_safe_resets_if_it_warms_again(void)
{
    fresh();
    step(healthy(125));
    now += 6000;
    CHECK(step(healthy(50)).safe_to_handle == false);        /* timer starts here */
    now += 6000;
    CHECK(step(healthy(50)).safe_to_handle == true);
    now += 100;
    CHECK(step(healthy(65)).safe_to_handle == false);        /* warmed past 60: not safe any more */
    CHECK_STR(safety_ack(&s), "not_safe");
}

static void test_ack_needs_readable_plate(void)
{
    fresh();
    step(healthy(125));
    now += 6000; step(healthy(50));
    now += 6000; step(healthy(50));
    safety_in_t blind = healthy(50);
    blind.plate_valid = false;                               /* plate sensor currently unreadable */
    step(blind);
    CHECK_STR(safety_ack(&s), "not_safe");
}

static void test_ot_limit_override(void)
{
    fresh();
    safety_set_ot_limit(&s, 500.0f);
    CHECK_NEAR(s.ot_limit_c, 120.0, 0.001);                  /* can never be raised above the hard limit */
    safety_set_ot_limit(&s, 40.0f);
    CHECK(step(healthy(39.9f)).mode == BMC_MODE_STANDBY);
    safety_out_t o = step(healthy(40.0f));
    CHECK(o.mode == BMC_MODE_FAILSAFE && o.fault_code == BMC_FAULT_OVER_TEMP);
    now += 6000; step(healthy(30));
    now += 6000; step(healthy(30));
    CHECK(safety_ack(&s) == NULL);
    CHECK_NEAR(s.ot_limit_c, 120.0, 0.001);                  /* a lowered test limit does not survive recovery */
}

static void test_injection(void)
{
    fresh();
    get_to_char();
    safety_inject(&s, BMC_FAULT_FAN_STALL, true);
    safety_out_t o = step(healthy(30));
    CHECK(o.mode == BMC_MODE_FAILSAFE && o.fault_code == BMC_FAULT_FAN_STALL);
    safety_clear_injections(&s);
    CHECK(step(healthy(30)).fault_mask == 0);
}

static void test_warn_event_once(void)
{
    fresh();
    step(healthy(111));
    step(healthy(112));
    bmc_event_t all[BMC_EV_RING];
    int n = bmc_ev_recent(&ev, all, BMC_EV_RING);
    int warns = 0;
    for (int i = 0; i < n; i++) {
        warns += all[i].type == BMC_EV_WARN;
    }
    CHECK(warns == 1);
}

static void test_events_ring(void)
{
    bmc_events_t e;
    bmc_ev_init(&e);
    for (int i = 0; i < 40; i++) {
        bmc_ev_emit(&e, BMC_EV_WARN, (uint32_t)i, -1, "X", "n=%d,with,commas", i);
    }
    bmc_event_t o;
    int popped = 0;
    uint32_t first = 0;
    while (bmc_ev_pop(&e, &o)) {
        if (!popped) first = o.id;
        popped++;
        CHECK(strchr(o.detail, ',') == NULL);
    }
    CHECK(popped == BMC_EV_RING);                            /* a slow consumer skips overwritten events */
    CHECK(first == 40 - BMC_EV_RING + 1);
    bmc_event_t rec[BMC_EV_RING];
    CHECK(bmc_ev_recent(&e, rec, BMC_EV_RING) == BMC_EV_RING);
    CHECK(rec[BMC_EV_RING - 1].id == 40 && rec[0].id == 25);
}

void run_safety_tests(void)
{
    SUITE("safety");
    test_boot_and_standby();
    test_over_temp_any_mode();
    test_each_fault_trips_in_actuating_mode();
    test_debounce();
    test_first_fault_wins_and_priority();
    test_selftest_and_mode_rules();
    test_selftest_blocked_in_failsafe();
    test_latch_cooldown_and_ack();
    test_safe_resets_if_it_warms_again();
    test_ack_needs_readable_plate();
    test_ot_limit_override();
    test_injection();
    test_warn_event_once();
    test_events_ring();
}
