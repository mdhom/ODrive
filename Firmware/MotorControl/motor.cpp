
#include "motor.hpp"
#include "axis.hpp"
#include "low_level.h"
#include "odrive_main.h"

#include <algorithm>

Motor::Motor(TIM_HandleTypeDef* timer,
             uint16_t control_deadline,
             float shunt_conductance,
             TGateDriver& gate_driver,
             TOpAmp& opamp,
             OnboardThermistorCurrentLimiter& fet_thermistor,
             OffboardThermistorCurrentLimiter& motor_thermistor) :
        timer_(timer),
        control_deadline_(control_deadline),
        shunt_conductance_(shunt_conductance),
        gate_driver_(gate_driver),
        opamp_(opamp),
        fet_thermistor_(fet_thermistor),
        motor_thermistor_(motor_thermistor) {
    apply_config();
    fet_thermistor_.motor_ = this;
    motor_thermistor_.motor_ = this;
}

// @brief Arms the PWM outputs that belong to this motor.
//
// Note that this does not yet activate the PWM outputs, it just unlocks them.
//
// While the motor is armed, the control loop must set new modulation timings
// between any two interrupts (that is, enqueue_modulation_timings must be executed).
// If the control loop fails to do so, the next interrupt handler floats the
// phases. Once this happens, missed_control_deadline is set to true and
// the motor can be considered disarmed.
//
// @returns: True on success, false otherwise
bool Motor::arm() {

    // Reset controller states, integrators, setpoints, etc.
    axis_->controller_.reset();
    reset_current_control();

    // Wait until the interrupt handler triggers twice. This gives
    // the control loop the correct time quota to set up modulation timings.
    if (!axis_->wait_for_current_meas())
        return axis_->error_ |= Axis::ERROR_CURRENT_MEASUREMENT_TIMEOUT, false;
    next_timings_valid_ = false;
    safety_critical_arm_motor_pwm(*this);
    return true;
}

void Motor::reset_current_control() {
    current_control_.v_current_control_integral_d = 0.0f;
    current_control_.v_current_control_integral_q = 0.0f;
    current_control_.acim_rotor_flux = 0.0f;
    current_control_.Ibus = 0.0f;
}

// @brief Tune the current controller based on phase resistance and inductance
// This should be invoked whenever one of these values changes.
// TODO: allow update on user-request or update automatically via hooks
void Motor::update_current_controller_gains() {
    // Calculate current control gains
    current_control_.p_gain = config_.current_control_bandwidth * config_.phase_inductance;
    float plant_pole = config_.phase_resistance / config_.phase_inductance;
    current_control_.i_gain = plant_pole * current_control_.p_gain;
}

bool Motor::apply_config() {
    config_.parent = this;
    is_calibrated_ = config_.pre_calibrated;
    update_current_controller_gains();
    return true;
}

// @brief Set up the gate drivers
bool Motor::setup() {
    if (!gate_driver_.init()) {
        set_error(ERROR_DRV_FAULT);
        return false;
    }
    
    // Solve for exact gain, then snap down to have equal or larger range as requested
    // or largest possible range otherwise
    constexpr float kMargin = 0.90f;
    constexpr float kTripMargin = 1.0f; // Trip level is at edge of linear range of amplifer
    constexpr float max_output_swing = 1.35f; // [V] out of amplifier
    float max_unity_gain_current = kMargin * max_output_swing * shunt_conductance_; // [A]
    float requested_gain = max_unity_gain_current / config_.requested_current_range; // [V/V]
    
    float actual_gain = NAN;
    bool success = opamp_.set_gain(requested_gain, &actual_gain);
    if (!success)
        return false;

    // Values for current controller
    phase_current_rev_gain_ = 1.0f / actual_gain;
    // Clip all current control to actual usable range
    current_control_.max_allowed_current = max_unity_gain_current * phase_current_rev_gain_;
    // Set trip level
    current_control_.overcurrent_trip_level = (kTripMargin / kMargin) * current_control_.max_allowed_current;

    return true;
}

void Motor::set_error(Motor::Error error){
    error_ |= error;
    axis_->error_ |= Axis::ERROR_MOTOR_FAILED;
    safety_critical_disarm_motor_pwm(*this);
    update_brake_current();
}

bool Motor::do_checks() {
    if (!gate_driver_.check_fault()) {
        set_error(ERROR_DRV_FAULT);
        return false;
    }
    if (!motor_thermistor_.do_checks()) {
        axis_->error_ |= Axis::ERROR_MOTOR_FAILED;
        set_error(ERROR_MOTOR_THERMISTOR_OVER_TEMP);
        return false;
    }
    if (!fet_thermistor_.do_checks()) {
        axis_->error_ |= Axis::ERROR_MOTOR_FAILED;
        set_error(ERROR_FET_THERMISTOR_OVER_TEMP);
        return false;
    }
    return true;
}

float Motor::effective_current_lim() {
    // Configured limit
    float current_lim = config_.current_lim;
    // Hardware limit
    if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_GIMBAL) {
        current_lim = std::min(current_lim, 0.98f*one_by_sqrt3*vbus_voltage); //gimbal motor is voltage control
    } else {
        current_lim = std::min(current_lim, axis_->motor_.current_control_.max_allowed_current);
    }

    // Apply thermistor current limiters
    current_lim = std::min(current_lim, motor_thermistor_.get_current_limit(config_.current_lim));
    current_lim = std::min(current_lim, fet_thermistor_.get_current_limit(config_.current_lim));
    effective_current_lim_ = current_lim;

    return effective_current_lim_;
}

//return the maximum available torque for the motor.
//Note - for ACIM motors, available torque is allowed to be 0.
float Motor::max_available_torque() {
    if (config_.motor_type == Motor::MOTOR_TYPE_ACIM) {
        float max_torque = effective_current_lim_ * config_.torque_constant * current_control_.acim_rotor_flux;
        max_torque = std::clamp(max_torque, 0.0f, config_.torque_lim);
        return max_torque;
    }
    else {
        float max_torque = effective_current_lim_ * config_.torque_constant;
        max_torque = std::clamp(max_torque, 0.0f, config_.torque_lim);
        return max_torque;
    }
}

void Motor::log_timing(TimingLog_t log_idx) {
    static const uint16_t clocks_per_cnt = (uint16_t)((float)TIM_1_8_CLOCK_HZ / (float)TIM_APB1_CLOCK_HZ);
    uint16_t timing = clocks_per_cnt * htim13.Instance->CNT; // TODO: Use a hw_config

    if (log_idx < TIMING_LOG_NUM_SLOTS) {
        timing_log_[log_idx] = timing;
    }
}

float Motor::phase_current_from_adcval(uint32_t ADCValue) {
    int adcval_bal = (int)ADCValue - (1 << 11);
    float amp_out_volt = (3.3f / (float)(1 << 12)) * (float)adcval_bal;
    float shunt_volt = amp_out_volt * phase_current_rev_gain_;
    float current = shunt_volt * shunt_conductance_;
    return current;
}

//--------------------------------
// Measurement and calibration
//--------------------------------

// TODO check Ibeta balance to verify good motor connection
bool Motor::measure_phase_resistance(float test_current, float max_voltage) {
    static const float kI = 10.0f;                                 // [(V/s)/A]
    static const int num_test_cycles = (int)(3.0f / CURRENT_MEAS_PERIOD); // Test runs for 3s
    float test_voltage = 0.0f;
    
    size_t i = 0;
    axis_->run_control_loop([&](){
        float Ialpha = -(current_meas_.phB + current_meas_.phC);
        test_voltage += (kI * current_meas_period) * (test_current - Ialpha);
        if (test_voltage > max_voltage || test_voltage < -max_voltage)
            return set_error(ERROR_PHASE_RESISTANCE_OUT_OF_RANGE), false;

        // Test voltage along phase A
        if (!enqueue_voltage_timings(test_voltage, 0.0f))
            return false; // error set inside enqueue_voltage_timings
        log_timing(TIMING_LOG_MEAS_R);

        return ++i < num_test_cycles;
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    //// De-energize motor
    //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
    //    return false; // error set inside enqueue_voltage_timings

    float R = test_voltage / test_current;
    config_.phase_resistance = R;
    return true; // if we ran to completion that means success
}

bool Motor::measure_phase_inductance(float voltage_low, float voltage_high) {
    float test_voltages[2] = {voltage_low, voltage_high};
    float Ialphas[2] = {0.0f};
    static const int num_cycles = 5000;

    size_t t = 0;
    axis_->run_control_loop([&](){
        int i = t & 1;
        Ialphas[i] += -current_meas_.phB - current_meas_.phC;

        // Test voltage along phase A
        if (!enqueue_voltage_timings(test_voltages[i], 0.0f))
            return false; // error set inside enqueue_voltage_timings
        log_timing(TIMING_LOG_MEAS_L);

        return ++t < (num_cycles << 1);
    });
    if (axis_->error_ != Axis::ERROR_NONE)
        return false;

    //// De-energize motor
    //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
    //    return false; // error set inside enqueue_voltage_timings

    float v_L = 0.5f * (voltage_high - voltage_low);
    // Note: A more correct formula would also take into account that there is a finite timestep.
    // However, the discretisation in the current control loop inverts the same discrepancy
    float dI_by_dt = (Ialphas[1] - Ialphas[0]) / (current_meas_period * (float)num_cycles);
    float L = v_L / dI_by_dt;

    config_.phase_inductance = L;
    // TODO arbitrary values set for now
    if (L < 2e-6f || L > 4000e-6f)
        return set_error(ERROR_PHASE_INDUCTANCE_OUT_OF_RANGE), false;
    return true;
}


bool Motor::run_calibration() {
    float R_calib_max_voltage = config_.resistance_calib_max_voltage;
    if (config_.motor_type == MOTOR_TYPE_HIGH_CURRENT
        || config_.motor_type == MOTOR_TYPE_ACIM) {
        if (!measure_phase_resistance(config_.calibration_current, R_calib_max_voltage))
            return false;
        if (!measure_phase_inductance(-R_calib_max_voltage, R_calib_max_voltage))
            return false;
    } else if (config_.motor_type == MOTOR_TYPE_GIMBAL) {
        // no calibration needed
    } else {
        return false;
    }

    update_current_controller_gains();
    
    is_calibrated_ = true;
    return true;
}

bool Motor::enqueue_modulation_timings(float mod_alpha, float mod_beta) {
    if (is_nan(mod_alpha) || is_nan(mod_beta))
        return set_error(ERROR_MODULATION_IS_NAN), false;
    auto [tA, tB, tC, success] = SVM(mod_alpha, mod_beta);
    if(!success)
        return set_error(ERROR_MODULATION_MAGNITUDE), false;
    next_timings_[0] = (uint16_t)(tA * (float)TIM_1_8_PERIOD_CLOCKS);
    next_timings_[1] = (uint16_t)(tB * (float)TIM_1_8_PERIOD_CLOCKS);
    next_timings_[2] = (uint16_t)(tC * (float)TIM_1_8_PERIOD_CLOCKS);
    next_timings_valid_ = true;
    return true;
}

bool Motor::enqueue_voltage_timings(float v_alpha, float v_beta) {
    float vfactor = 1.0f / ((2.0f / 3.0f) * vbus_voltage);
    float mod_alpha = vfactor * v_alpha;
    float mod_beta = vfactor * v_beta;
    if (!enqueue_modulation_timings(mod_alpha, mod_beta))
        return false;
    log_timing(TIMING_LOG_FOC_VOLTAGE);
    return true;
}

// We should probably make FOC Current call FOC Voltage to avoid duplication.
bool Motor::FOC_voltage(float v_d, float v_q, float pwm_phase) {
    float c = our_arm_cos_f32(pwm_phase);
    float s = our_arm_sin_f32(pwm_phase);
    float v_alpha = c*v_d - s*v_q;
    float v_beta = c*v_q + s*v_d;
    return enqueue_voltage_timings(v_alpha, v_beta);
}

bool Motor::FOC_current(float Id_des, float Iq_des, float I_phase, float pwm_phase, float phase_vel) {
    axis_->task_times_.FOC_Current.beginTimer();
    // Syntactic sugar
    CurrentControl_t& ictrl = current_control_;

    // For Reporting
    ictrl.Iq_setpoint = Iq_des;

    // Check for current sense saturation
    if (std::abs(current_meas_.phB) > ictrl.overcurrent_trip_level || std::abs(current_meas_.phC) > ictrl.overcurrent_trip_level) {
        set_error(ERROR_CURRENT_SENSE_SATURATION);
        return false;
    }

    // Clarke transform
    float Ialpha = -current_meas_.phB - current_meas_.phC;
    float Ibeta = one_by_sqrt3 * (current_meas_.phB - current_meas_.phC);

    // Park transform
    float c_I = our_arm_cos_f32(I_phase);
    float s_I = our_arm_sin_f32(I_phase);
    float Id = c_I * Ialpha + s_I * Ibeta;
    float Iq = c_I * Ibeta - s_I * Ialpha;
    ictrl.Iq_measured += ictrl.I_measured_report_filter_k * (Iq - ictrl.Iq_measured);
    ictrl.Id_measured += ictrl.I_measured_report_filter_k * (Id - ictrl.Id_measured);

    // Check for violation of current limit
    float I_trip = effective_current_lim_ + config_.current_lim_margin;
    if (SQ(Id) + SQ(Iq) > SQ(I_trip)) {
        set_error(ERROR_CURRENT_LIMIT_VIOLATION);
        return false;
    }

    // Current error
    float Ierr_d = Id_des - Id;
    float Ierr_q = Iq_des - Iq;

    // Apply PI control
    float Vd = ictrl.v_current_control_integral_d + Ierr_d * ictrl.p_gain;
    float Vq = ictrl.v_current_control_integral_q + Ierr_q * ictrl.p_gain;

    if (config_.R_wL_FF_enable) {
        Vd -= phase_vel * config_.phase_inductance * Iq_des;
        Vq += phase_vel * config_.phase_inductance * Id_des;
        Vd += config_.phase_resistance * Id_des;
        Vq += config_.phase_resistance * Iq_des;
    }

    if (config_.bEMF_FF_enable) {
        Vq += phase_vel * (2.0f/3.0f) * (config_.torque_constant / config_.pole_pairs);
    }

    float mod_to_V = (2.0f / 3.0f) * vbus_voltage;
    float V_to_mod = 1.0f / mod_to_V;
    float mod_d = V_to_mod * Vd;
    float mod_q = V_to_mod * Vq;

    // Vector modulation saturation, lock integrator if saturated
    // TODO make maximum modulation configurable
    float mod_scalefactor = 0.80f * sqrt3_by_2 * 1.0f / std::sqrt(mod_d * mod_d + mod_q * mod_q);
    if (mod_scalefactor < 1.0f) {
        mod_d *= mod_scalefactor;
        mod_q *= mod_scalefactor;
        // TODO make decayfactor configurable
        ictrl.v_current_control_integral_d *= 0.99f;
        ictrl.v_current_control_integral_q *= 0.99f;
    } else {
        ictrl.v_current_control_integral_d += Ierr_d * (ictrl.i_gain * current_meas_period);
        ictrl.v_current_control_integral_q += Ierr_q * (ictrl.i_gain * current_meas_period);
    }

    // Compute estimated bus current
    ictrl.Ibus = mod_d * Id + mod_q * Iq;

    // Inverse park transform
    float c_p = our_arm_cos_f32(pwm_phase);
    float s_p = our_arm_sin_f32(pwm_phase);
    float mod_alpha = c_p * mod_d - s_p * mod_q;
    float mod_beta = c_p * mod_q + s_p * mod_d;

    // Report final applied voltage in stationary frame (for sensorles estimator)
    ictrl.final_v_alpha = mod_to_V * mod_alpha;
    ictrl.final_v_beta = mod_to_V * mod_beta;

    // Apply SVM
    if (!enqueue_modulation_timings(mod_alpha, mod_beta))
        return false; // error set inside enqueue_modulation_timings
    log_timing(TIMING_LOG_FOC_CURRENT);

    if (axis_->axis_num_ == 0) {

        // Edit these to suit your capture needs
        float trigger_data = ictrl.v_current_control_integral_d;
        float trigger_threshold = 0.5f;
        float sample_data = Ialpha;

        static bool ready = false;
        static bool capturing = false;
        if (trigger_data < trigger_threshold) {
            ready = true;
        }
        if (ready && trigger_data >= trigger_threshold) {
            capturing = true;
            ready = false;
        }
        if (capturing) {
            oscilloscope[oscilloscope_pos] = sample_data;
            if (++oscilloscope_pos >= OSCILLOSCOPE_SIZE) {
                oscilloscope_pos = 0;
                capturing = false;
            }
        }
    }

    axis_->task_times_.FOC_Current.stopTimer();
    return true;
}

// torque_setpoint [Nm]
// phase [rad electrical]
// phase_vel [rad/s electrical]
bool Motor::update(float torque_setpoint, float phase, float phase_vel) {
    float current_setpoint = 0.0f;
    phase *= config_.direction;
    phase_vel *= config_.direction;

    if (config_.motor_type == MOTOR_TYPE_ACIM) {
        current_setpoint = torque_setpoint / (config_.torque_constant * std::max(current_control_.acim_rotor_flux, config_.acim_gain_min_flux));
    }
    else {
        current_setpoint = torque_setpoint / config_.torque_constant;
    }
    current_setpoint *= config_.direction;

    // TODO: 2-norm vs independent clamping (current could be sqrt(2) bigger)
    float ilim = effective_current_lim_;
    float id = std::clamp(current_control_.Id_setpoint, -ilim, ilim);
    float iq = std::clamp(current_setpoint, -ilim, ilim);

    if (config_.motor_type == MOTOR_TYPE_ACIM) {
        // Note that the effect of the current commands on the real currents is actually 1.5 PWM cycles later
        // However the rotor time constant is (usually) so slow that it doesn't matter
        // So we elect to write it as if the effect is immediate, to have cleaner code

        if (config_.acim_autoflux_enable) {
            float abs_iq = std::abs(iq);
            float gain = abs_iq > id ? config_.acim_autoflux_attack_gain : config_.acim_autoflux_decay_gain;
            id += gain * (abs_iq - id) * current_meas_period;
            id = std::clamp(id, config_.acim_autoflux_min_Id, ilim);
            current_control_.Id_setpoint = id;
        }

        // acim_rotor_flux is normalized to units of [A] tracking Id; rotor inductance is unspecified
        float dflux_by_dt = config_.acim_slip_velocity * (id - current_control_.acim_rotor_flux);
        current_control_.acim_rotor_flux += dflux_by_dt * current_meas_period;
        float slip_velocity = config_.acim_slip_velocity * (iq / current_control_.acim_rotor_flux);
        // Check for issues with small denominator.
        if (is_nan(slip_velocity) || std::abs(slip_velocity) > 0.1f * (float)current_meas_hz)
            slip_velocity = 0.0f;
        phase_vel += slip_velocity;
        // reporting only:
        current_control_.async_phase_vel = slip_velocity;

        current_control_.async_phase_offset += slip_velocity * current_meas_period;
        current_control_.async_phase_offset = wrap_pm_pi(current_control_.async_phase_offset);
        phase += current_control_.async_phase_offset;
        phase = wrap_pm_pi(phase);
    }

    float pwm_phase = phase + 1.5f * current_meas_period * phase_vel;

    // Execute current command
    switch(config_.motor_type){
        case MOTOR_TYPE_HIGH_CURRENT: return FOC_current(id, iq, phase, pwm_phase, phase_vel); break;
        case MOTOR_TYPE_ACIM: return FOC_current(id, iq, phase, pwm_phase, phase_vel); break;
        case MOTOR_TYPE_GIMBAL: return FOC_voltage(id, iq, pwm_phase); break;
        default: set_error(ERROR_NOT_IMPLEMENTED_MOTOR_TYPE); return false; break;
    }
    return true;
}

void Motor::tim_update_cb() {
    // If the corresponding timer is counting up, we just sampled in SVM vector 0, i.e. real current
    // If we are counting down, we just sampled in SVM vector 7, with zero current
    bool counting_down = timer_->Instance->CR1 & TIM_CR1_DIR;
    if (counting_down)
        return;

    axis_->encoder_.sample_now();
}
