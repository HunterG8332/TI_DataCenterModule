#pragma once

/* Every threshold, period and limit the BMC logic uses lives here, so tuning or
 * porting never means hunting through logic files. Values marked PLACEHOLDER are
 * starting points that need bench measurement before they are trusted. See
 * docs/bmc_plan/ for the reasoning behind each. */

/* ---- loop periods -------------------------------------------------------- */
#define BMC_SAFETY_PERIOD_MS        50     /* supervisor task: safety + fan loops + actuators */
#define BMC_SAMPLE_PERIOD_MS        100    /* sensor task and telemetry (10 Hz) */

/* ---- thermal limits (spreader plate) ------------------------------------- */
#define BMC_OT_TRIP_C               120.0f /* over-temperature cutoff; runtime override may only lower it */
#define BMC_OT_WARN_C               110.0f /* flag only */
#define BMC_SAFE_CLEAR_C            60.0f  /* safe-to-handle: plate below this ... */
#define BMC_SAFE_CLEAR_HOLD_MS      5000   /* ... for this long before ACK is accepted */
#define BMC_TEMP_VALID_MIN_C        (-20.0f)
#define BMC_TEMP_VALID_MAX_C        150.0f /* TMP117 range limit; above is a sensor fault, not a temperature */

/* ---- fault detection ----------------------------------------------------- */
#define BMC_FAIL_READS_TRIP         3      /* consecutive failed reads of a required device */
#define BMC_SENSOR_STALE_MS         500    /* no fresh sensor snapshot for this long */
#define BMC_SENSOR_REINIT_MS        1000   /* retry period for a device that is not present */

/* ---- fans ----------------------------------------------------------------- */
#define BMC_FAN_COUNT               2
#define BMC_FAN_TACH_WINDOW_MS      250    /* stall-detector window (120 RPM per tach count) */
#define BMC_FAN_STALL_MIN_DUTY_PCT  10.0f  /* below this a fan may legitimately idle */
#define BMC_FAN_STALL_RECOVER_CYCLES 5
/* Fan 1 = ARCTIC S8038-7K (~7000 RPM), fan 2 = ~10000 RPM part. Both are driven to the SAME RPM,
 * with the demand percentage meaning "percent of BMC_RPM_REF". */
#define BMC_FAN1_MAX_RPM            7000.0f
#define BMC_FAN2_MAX_RPM            10000.0f
#define BMC_RPM_REF                 7000.0f /* demand 100% = this RPM on every fan (see docs 04 section 2) */
#define BMC_FANRPM_KP               0.002f  /* PLACEHOLDER duty% per RPM of error */
#define BMC_FANRPM_KI               0.010f  /* PLACEHOLDER duty% per RPM*s */
#define BMC_FANRPM_I_LIMIT          25.0f   /* integral clamp, duty % */
#define BMC_FAILSAFE_FAN_DUTY_PCT   100.0f  /* failsafe: both fans open-loop at this duty */
/* Standby / manual-auto fallback: linear curve, 0% at LOW, 100% at HIGH */
#define BMC_FAN_CURVE_LOW_C         25.0f
#define BMC_FAN_CURVE_HIGH_C        100.0f

/* ---- heater --------------------------------------------------------------- */
#define BMC_HEATER_REQ_TIMEOUT_MS   1000   /* manual HEATER command lease */
#define BMC_HEATER_MAX_PCT_BRINGUP  100.0f /* firmware cap; lower it per bring-up stage (docs 02 section 6) */

/* ---- self-test ------------------------------------------------------------ */
#define BMC_SELFTEST_AMBIENT_MIN_C  10.0f
#define BMC_SELFTEST_AMBIENT_MAX_C  40.0f
#define BMC_SELFTEST_REQUIRE_48V    0      /* set 1 once the 48 V supply feeds the INA238 */
#define BMC_SELFTEST_V48_MIN        43.2f
#define BMC_SELFTEST_V48_MAX        52.8f
#define BMC_SELFTEST_FAN_DUTY_PCT   50.0f
#define BMC_SELFTEST_FAN_MS         2500
#define BMC_SELFTEST_FAN_MIN_FRAC   0.25f  /* driven fan must exceed this fraction of its max RPM */
#define BMC_SELFTEST_FAN_CROSS_FRAC 0.50f  /* the idle fan's RPM must stay under this fraction of the driven fan's RPM */

/* ---- control law defaults (PLACEHOLDER: identify the plant, then tune; docs 04 section 4) ----- */
#define BMC_DEFAULT_SETPOINT_C      60.0f
#define BMC_SETPOINT_MIN_C          20.0f
#define BMC_SETPOINT_MAX_C          BMC_OT_WARN_C
#define BMC_PID_KP                  5.0f   /* demand % per degC */
#define BMC_PID_KI                  0.20f  /* demand % per degC*s */
#define BMC_PID_KD                  0.0f
#define BMC_PID_D_TAU_S             1.0f
#define BMC_PID_I_MAX               100.0f
#define BMC_PID_SLEW_PCT_PER_S      50.0f
#define BMC_GAIN_MAX                1000.0f

/* ---- profile / run -------------------------------------------------------- */
#define BMC_PROFILE_MAX_STEPS       16
#define BMC_START_MAX_ABOVE_AMBIENT_C 5.0f /* PROFILE START needs plate within this of inlet */
#define BMC_START_MAX_PLATE_NO_INLET_C 40.0f /* ... or below this when no inlet sensor is fitted */

/* ---- feedforward ---------------------------------------------------------- */
#define BMC_FF_VARIANTS             4
#define BMC_FF_PARAMS               16
