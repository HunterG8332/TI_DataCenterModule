# 04 — Control Laws

Governing requirements: FSR 3.2.1.5, 3.2.3.2.4; CONOPS §3.4 (Characterization, Reactive, Proactive), §5.2 (tuning fairness).

The control laws are the *experiment*. The goal of this design is that the two laws run on identical plumbing, differ only in how they compute fan demand, and are tuned by a method that can be written down and defended.

## 1. Layered structure

```
                 ┌──────────── control law (mode-specific) ────────────┐
 plate_c ───────>│ REACTIVE : PID(setpoint − plate_c)                   │
 i48_a  ────────>│ PROACTIVE: FF(i48_a) + PI trim(setpoint − plate_c)   │──> fan_demand_pct (0–100)
 setpoint ──────>│ CHAR     : manual FAN <pct>                          │
                 └──────────────────────────────────────────────────────┘
                                         │
                      fanctl (RPM-matching layer, this section 2)
                                         │ target_rpm = demand% × RPM_REF
                         ┌───────────────┴───────────────┐
                    fan 1 inner loop               fan 2 inner loop
                    (RPM → duty)                    (RPM → duty)
                         │                               │
                      PWM 25 kHz                      PWM 25 kHz
```

This is the same two-level shape as OpenBMC's `phosphor-pid-control` (a thermal controller whose output is a fan setpoint, feeding a fan controller that tracks it). That is what "the Open Compute Project approach" concretely means for the structure; see §4 for what it does and does not say about tuning.

## 2. RPM-matching fan layer (decision D10)

Requirement from the team: two different fans (≈10 000 RPM max and ≈7 000 RPM max) must run at the **same RPM**, where the commanded fan percentage is that RPM as a fraction of the 7 000 RPM limit.

```
RPM_REF        = 7000            (config; set from characterized achievable max of the slower fan, minus headroom)
target_rpm     = fan_demand_pct / 100 × RPM_REF
duty_i         = clamp( duty_map_i(target_rpm) + PI_i(target_rpm − rpm_i), duty_min_i, 100 )
```

- `duty_map_i` is a **per-fan characterization table** (RPM → open-loop duty), built from a 0–100 % sweep in 10 % steps (this is the same sweep as validation row 3.2.3.2.4; `tools/sweep_to_csv.py` and the earlier `fan_sweep_*.csv` are the starting point). It makes the inner loop mostly feedforward, so the PI only trims.
- The PI has anti-windup (integral clamp) and output clamp; its sample period is the tach window (see below).
- **Headroom matters.** If `RPM_REF` is exactly the slow fan's nominal maximum, that fan's duty saturates at 100 % and its integrator winds up. Set `RPM_REF` to a value the slow fan can hold with margin (e.g. 95 % of its measured maximum) and record it in the design record.
- **Floor behavior.** Below the lowest RPM a fan can sustain (`RPM_MIN_i`, from the sweep), the fan either runs at the floor or turns off. Choose floor in control modes (so the stall detector stays armed) and off in STANDBY; both are config.
- **Tach resolution.** With 2 pulses/rev, a 250 ms window gives 120 RPM per count. That is fine at 7 000 RPM (1.7 %) but noisy at low RPM. Use a 500 ms window for the inner loop's *measurement* (60 RPM resolution) while stall detection keeps its own window.
- **Failsafe bypasses all of this.** Both fans go to `FAILSAFE_FAN_DUTY_PCT` = 100 % open loop (assumption A1). The 10 k fan therefore runs faster than the 7 k fan in failsafe; that is intended, maximum cooling beats symmetry.
- **Experiment note.** Capping both fans to 7 k RPM caps maximum airflow in the Reactive and Proactive runs. Record the resulting maximum airflow-equivalent so the "integrated fan energy" comparison (CONOPS §4.2) is computed from the same RPMs for both laws. Fan energy cannot be derived from duty alone for two different fans; log RPM per fan and, once the fan datasheet curves are known, an RPM→power estimate on the host.
- Any single-fan stall is a failsafe trigger (D11).
- Interlock behavior is unchanged: `fan_enable` open forces the duty to 0 inside `bmc_fan_set_duty_pct`.

Items to measure before the inner loop is tuned:
1. Duty → RPM curve for each fan (monotonic, check for dead band at low duty).
2. Achievable maximum RPM and the 7 k fan's real ceiling.
3. Pulses per revolution for each fan (assumption A2).
4. Tach noise at each target RPM (to choose the window).

## 3. Reactive law (control group)

Structure (OpenBMC-style thermal PID, with the full PID form the FSR calls for):

```
e        = plate_c − setpoint                      (positive when too hot → more fan)
P        = Kp × e
I        = clamp( I + Ki × e × Ts, I_min, I_max )   (integral clamp + conditional integration when output saturated)
D        = Kd × d(filtered plate_c)/dt              (derivative on measurement, first-order filter; default Kd = 0)
fan_demand_pct = clamp( P + I + D + ff_offset, out_min, out_max ), then slew-limited (slew_pos/slew_neg per second)
```

Parameters (the same ones OpenBMC exposes): `Kp, Ki, Kd, integral limits, output limits, slew rates, sample period`. The feedforward offset/gain of OpenBMC is **not used** in the reactive law: the CONOPS defines reactive as "regulator current is logged but not used for control".

Defaults: `Ts = 100 ms` (one new plate sample per tick; if the TMP117 only updates at 1 Hz — see README "things to verify" — this must be fixed first, otherwise the baseline is artificially slow).

Setpoint: a single value (`SETPOINT`), locked during a run, the same for both laws.

## 4. Tuning method (decision D12, with the honest caveat)

What OpenBMC / OCP actually specifies: a controller structure and parameter set (P, I, feedforward offset/gain, integral and output limits, slew rates, stepwise lookup tables). Its documentation **does not prescribe a tuning procedure or recommended gains**; each platform tunes empirically. So "the tuning profile used by OCP" cannot be copied; what can be copied is the structure, and the team must supply and document a method. The recommended method:

**Step-response identification + SIMC (Skogestad IMC-based PI rules)**

1. **Operating point.** Fix fans at the RPM near where the controller is expected to live; start from ambient and stable.
2. **Step.** In CHARACTERIZATION, hold fan RPM constant and apply a heater step (e.g. 0 → 50 %). Log `plate_c` at 10 Hz until steady state (at least 3–5 time constants).
3. **Fit a first-order-plus-dead-time (FOPDT) model**: gain `K` (°C per % heater), time constant `τ`, dead time `θ` (sensor + thermal transport). The host analysis script does the fit and records residuals.
4. **Compute PI gains** with SIMC: `Kc = (1/K)·τ/(τ_c + θ)`, `τ_I = min(τ, 4(τ_c + θ))`, with the closed-loop time constant `τ_c` chosen from a stated rule (start with `τ_c = θ` for a balanced, documented response; widen if it overshoots). Convert to the controller's units (fan percent per °C) using the fan gain, since the *actuator* in the loop is fan demand, not heater power. That means the plant to identify for the PID is **fan demand → plate temperature** (heater held at the profile's full load), which is the loop being closed; a heater→temperature step gives the disturbance path used later for the feedforward.
5. **Repeat at 2–3 fan operating points** because the airflow-to-temperature gain is nonlinear (more airflow gives diminishing returns). Either pick the most conservative gains for all points or gain-schedule by RPM; record the choice.
6. **Verify** with a setpoint step and the CONOPS step-load profile; accept if overshoot and settling meet the documented target; otherwise adjust `τ_c`, not individual gains by hand.
7. **Freeze**: the final gains are written into `bmc_config.h` (or passed with `GAINS` before the run), and **the same method and the same `τ_c` rule are applied to the PI trim inside the proactive law**. Publish the identified model, the rule and the gains in the design record. This is the answer to the CONOPS "tuning fairness" threat.

Alternatives (rejected as default, kept as fallbacks): Ziegler–Nichols ultimate-gain (drives a hot heater to oscillation, unsafe); relay-feedback autotune (oscillates around the setpoint; acceptable only well below `OT_WARN_C`); hand tuning (hard to defend).

Safety during identification: step tests run in CHARACTERIZATION with the normal safety task active and a conservative `HEATER_MAX_PCT` until the model is known.

## 5. Proactive law (experimental group)

Fan demand = `FF(i48_a, state)` + the **same PI trim** as the reactive law acting on plate error. Per decision D13 the feedforward form is open, and the firmware provides the plug-in point rather than choosing:

```c
typedef struct {
    float (*compute)(float i48_a, float v48_v, const ff_state_t *st);   /* returns fan demand offset in % */
    void  (*reset)(ff_state_t *st);
    const char *name;
} ff_variant_t;
```

- Variants are registered in a table and selected at runtime with `FF <id>` (logged as `ff_variant` and written into `RUN_START`). Switching is allowed only between runs.
- Parameters per variant are held in a small struct and settable via a `FFPARAM <variant> <index> <value>` command (add in P9) so tuning does not require a reflash.
- Candidate variants the team has said it wants to compare (illustrative, not decided):
  1. **Linear gain**: `ff = Kff × max(0, i − i_idle)`.
  2. **Lookup table** from characterization: steady-state fan demand needed to hold the setpoint at each load level (the table built in the Characterization runs); interpolate.
  3. **Linear + lead/lag**: variant 1 followed by a first-order lead/lag filter to shape the response to a step.
  4. (Optional) **Model-based**: demand computed from the identified FOPDT model so that predicted plate temperature stays at the setpoint.
- All variants must obey the same output limits and slew limits as the reactive law (otherwise differences in results come from limit settings, not strategy).
- Input signal hygiene: `i48_a` is an average over ≥ 1 heater PWM period (ICD §5.2.2). Log both the raw and any additionally filtered value if the variant filters it, and keep the control sample time the same as the reactive law's.
- Required characterization data for variants 2–4: the same Characterization runs as the tuning (heater levels 0, 25, 50, 75, 100 % each held to steady state at several RPMs).

## 6. Characterization mode (the plant measurement)

Open-loop manual mode (CONOPS §3.4): the operator fixes heater step and fan RPM; the BMC holds both and logs. Outputs of this mode feed everything above:
- Thermal time constants and steady-state rise per load level and RPM (tuning, FOPDT).
- Airflow vs temperature behavior (gain scheduling decision).
- Fan duty→RPM maps (§2).
- Heater repeatability data for validation row 3.2.1.4 (0/25/50/75/100 % × 3 sweeps).

Host tools to write (not firmware): sweep logger/CSV → plots; FOPDT fit; SIMC calculator; run-overlay metric script (peak temp, overshoot above setpoint, time above setpoint, time to steady state, integrated fan energy from RPM) that CONOPS §4.2 requires.

## 7. Control sampling and synchronization

- One plate sample per 100 ms drives one control update. If a sample is missing (read failed), the control law holds its previous output for at most `FAIL_READS_TRIP − 1` cycles; then the safety task has already moved the system to failsafe.
- Mode change resets the law (integrators and filters cleared) so runs start from the same state.
- `run_ms` is zeroed at `PROFILE START`; all analysis aligns runs on that.
- Reactive and proactive runs must start from the same plate temperature (within a stated tolerance of ambient) and the same inlet temperature; the dashboard should refuse `PROFILE START` unless `plate_c − inlet_c` is within tolerance. This enforces "return to ambient" from CONOPS §4.2 in code, not in the operator's memory.
