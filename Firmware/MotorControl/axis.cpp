
#include <stdlib.h>
#include <functional>
#include "gpio.h"

#include "utils.h"
#include "odrive_main.h"

Axis::Axis(const AxisHardwareConfig_t& hw_config,
           Config_t& config,
           Encoder& encoder,
           SensorlessEstimator& sensorless_estimator,
           Controller& controller,
           Motor& motor,
           TrapezoidalTrajectory& trap)
    : hw_config_(hw_config),
      config_(config),
      encoder_(encoder),
      sensorless_estimator_(sensorless_estimator),
      controller_(controller),
      motor_(motor),
      trap_(trap)
{
    encoder_.axis_ = this;
    sensorless_estimator_.axis_ = this;
    controller_.axis_ = this;
    motor_.axis_ = this;
    trap_.axis_ = this;
}

static void step_cb_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->step_cb();
}

// @brief Sets up all components of the axis,
// such as gate driver and encoder hardware.
void Axis::setup() {
    encoder_.setup();
    motor_.setup();
}

static void run_state_machine_loop_wrapper(void* ctx) {
    reinterpret_cast<Axis*>(ctx)->run_state_machine_loop();
    reinterpret_cast<Axis*>(ctx)->thread_id_valid_ = false;
}

// @brief Starts run_state_machine_loop in a new thread
void Axis::start_thread() {
    osThreadDef(thread_def, run_state_machine_loop_wrapper, hw_config_.thread_priority, 0, 4*512);
    thread_id_ = osThreadCreate(osThread(thread_def), this);
    thread_id_valid_ = true;
}

// @brief Unblocks the control loop thread.
// This is called from the current sense interrupt handler.
void Axis::signal_current_meas() {
    if (thread_id_valid_)
        osSignalSet(thread_id_, M_SIGNAL_PH_CURRENT_MEAS);
}

// @brief Blocks until a current measurement is completed
// @returns True on success, false otherwise
bool Axis::wait_for_current_meas() {
    return osSignalWait(M_SIGNAL_PH_CURRENT_MEAS, PH_CURRENT_MEAS_TIMEOUT).status == osEventSignal;
}

// step/direction interface
void Axis::step_cb() {
    if (enable_step_dir_) {
        GPIO_PinState dir_pin = HAL_GPIO_ReadPin(hw_config_.dir_port, hw_config_.dir_pin);
        float dir = (dir_pin == GPIO_PIN_SET) ? 1.0f : -1.0f;
        controller_.pos_setpoint_ += dir * config_.counts_per_step;
    }
};

// @brief Enables or disables step/dir input
void Axis::set_step_dir_enabled(bool enable) {
    if (enable) {
        // Set up the direction GPIO as input
        GPIO_InitTypeDef GPIO_InitStruct;
        GPIO_InitStruct.Pin = hw_config_.dir_pin;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(hw_config_.dir_port, &GPIO_InitStruct);

        // Subscribe to rising edges of the step GPIO
        GPIO_subscribe(hw_config_.step_port, hw_config_.step_pin, GPIO_PULLDOWN,
                step_cb_wrapper, this);

        enable_step_dir_ = true;
    } else {
        enable_step_dir_ = false;

        // Unsubscribe from step GPIO
        GPIO_unsubscribe(hw_config_.step_port, hw_config_.step_pin);
    }
}

// @brief Do axis level checks and call subcomponent do_checks
// Returns true if everything is ok.
bool Axis::do_checks() {
    if (!brake_resistor_armed)
        error_ |= ERROR_BRAKE_RESISTOR_DISARMED;
    if ((current_state_ != AXIS_STATE_IDLE) && (motor_.armed_state_ == Motor::ARMED_STATE_DISARMED))
        // motor got disarmed in something other than the idle loop
        error_ |= ERROR_MOTOR_DISARMED;
    if (!(vbus_voltage >= board_config.dc_bus_undervoltage_trip_level))
        error_ |= ERROR_DC_BUS_UNDER_VOLTAGE;
    if (!(vbus_voltage <= board_config.dc_bus_overvoltage_trip_level))
        error_ |= ERROR_DC_BUS_OVER_VOLTAGE;

    // Sub-components should use set_error which will propegate to this error_
    motor_.do_checks();
    encoder_.do_checks();
    // sensorless_estimator_.do_checks();
    // controller_.do_checks();

    return check_for_errors();
}

// @brief Update all esitmators
bool Axis::do_updates() {
    // Sub-components should use set_error which will propegate to this error_
    encoder_.update();
    sensorless_estimator_.update();
    return check_for_errors();
}

float Axis::get_temp() {
    float adc = adc_measurements_[hw_config_.thermistor_adc_ch];
    float normalized_voltage = adc / adc_full_scale;
    return horner_fma(normalized_voltage, thermistor_poly_coeffs, thermistor_num_coeffs);
}

bool Axis::run_lockin_spin() {
    // Spiral up current for softer rotor lock-in
    lockin_state_ = LOCKIN_STATE_RAMP;
    float x = 0.0f;
    run_control_loop([&]() {
        float phase = wrap_pm_pi(config_.lockin_ramp_distance * x);
        float I_mag = config_.lockin_current * x;
        x += current_meas_period / config_.lockin_ramp_time;
        if (!motor_.update(I_mag, phase))
            return false;
        return x < 1.0f;
    });
    
    // Spin states
    float distance = config_.lockin_ramp_distance;
    float phase = wrap_pm_pi(distance);
    float vel = distance / config_.lockin_ramp_time;

    // Function of states to check if we are done
    auto spin_done = [&](bool vel_override = false) -> bool {
        bool done = false;
        if (config_.lockin_finish_on_vel || vel_override)
            done = done || fabsf(vel) >= fabsf(config_.lockin_vel);
        if (config_.lockin_finish_on_distance)
            done = done || fabsf(distance) >= fabsf(config_.lockin_finish_distance);
        if (config_.lockin_finish_on_enc_idx)
            done = done || encoder_.index_found_;
        return done;
    };

    // Accelerate
    lockin_state_ = LOCKIN_STATE_ACCELERATE;
    run_control_loop([&]() {
        vel += config_.lockin_accel * current_meas_period;
        distance += vel * current_meas_period;
        phase = wrap_pm_pi(phase + vel * current_meas_period);

        if (!motor_.update(config_.lockin_current, phase))
            return false;
        return !spin_done(true); //vel_override to go to next phase
    });

    // Constant speed
    if (!spin_done()) {
        lockin_state_ = LOCKIN_STATE_CONST_VEL;
        vel = config_.lockin_vel; // reset to actual specified vel to avoid small integration error
        run_control_loop([&]() {
            distance += vel * current_meas_period;
            phase = wrap_pm_pi(phase + vel * current_meas_period);

            if (!motor_.update(config_.lockin_current, phase))
                return false;
            return !spin_done();
        });
    }

    lockin_state_ = LOCKIN_STATE_INACTIVE;
    return check_for_errors();
}

// Note run_sensorless_control_loop and run_closed_loop_control_loop are very similar and differ only in where we get the estimate from.
bool Axis::run_sensorless_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    run_control_loop([this](){
        if (controller_.config_.control_mode >= Controller::CTRL_MODE_POSITION_CONTROL)
            return error_ |= ERROR_POS_CTRL_DURING_SENSORLESS, false;

        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(sensorless_estimator_.pll_pos_, sensorless_estimator_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false;
        if (!motor_.update(current_setpoint, sensorless_estimator_.phase_))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

bool Axis::run_closed_loop_control_loop() {
    set_step_dir_enabled(config_.enable_step_dir);
    run_control_loop([this](){
        // Note that all estimators are updated in the loop prefix in run_control_loop
        float current_setpoint;
        if (!controller_.update(encoder_.pos_estimate_, encoder_.vel_estimate_, &current_setpoint))
            return error_ |= ERROR_CONTROLLER_FAILED, false; //TODO: Make controller.set_error
        if (!motor_.update(current_setpoint, encoder_.phase_))
            return false; // set_error should update axis.error_
        return true;
    });
    set_step_dir_enabled(false);
    return check_for_errors();
}

bool Axis::run_idle_loop() {
    // run_control_loop ignores missed modulation timing updates
    // if and only if we're in AXIS_STATE_IDLE
    safety_critical_disarm_motor_pwm(motor_);
    run_control_loop([this](){
        return true;
    });
    return check_for_errors();
}

// Infinite loop that does calibration and enters main control loop as appropriate
void Axis::run_state_machine_loop() {

    // Allocate the map for anti-cogging algorithm and initialize all values to 0.0f
    // TODO: Move this somewhere else
    // TODO: respect changes of CPR
    int encoder_cpr = encoder_.config_.cpr;
    controller_.anticogging_.cogging_map = (float*)malloc(encoder_cpr * sizeof(float));
    if (controller_.anticogging_.cogging_map != NULL) {
        for (int i = 0; i < encoder_cpr; i++) {
            controller_.anticogging_.cogging_map[i] = 0.0f;
        }
    }

    // arm!
    motor_.arm();
    
    for (;;) {
        // Load the task chain if a specific request is pending
        if (requested_state_ != AXIS_STATE_UNDEFINED) {
            size_t pos = 0;
            if (requested_state_ == AXIS_STATE_STARTUP_SEQUENCE) {
                if (config_.startup_motor_calibration)
                    task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (config_.startup_encoder_index_search && encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                if (config_.startup_encoder_offset_calibration)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                if (config_.startup_closed_loop_control)
                    task_chain_[pos++] = AXIS_STATE_CLOSED_LOOP_CONTROL;
                else if (config_.startup_sensorless_control)
                    task_chain_[pos++] = AXIS_STATE_SENSORLESS_CONTROL;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ == AXIS_STATE_FULL_CALIBRATION_SEQUENCE) {
                task_chain_[pos++] = AXIS_STATE_MOTOR_CALIBRATION;
                if (encoder_.config_.use_index)
                    task_chain_[pos++] = AXIS_STATE_ENCODER_INDEX_SEARCH;
                task_chain_[pos++] = AXIS_STATE_ENCODER_OFFSET_CALIBRATION;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            } else if (requested_state_ != AXIS_STATE_UNDEFINED) {
                task_chain_[pos++] = requested_state_;
                task_chain_[pos++] = AXIS_STATE_IDLE;
            }
            task_chain_[pos++] = AXIS_STATE_UNDEFINED; // TODO: bounds checking
            requested_state_ = AXIS_STATE_UNDEFINED;
            // Auto-clear any invalid state error
            error_ &= ~ERROR_INVALID_STATE;
        }

        // Note that current_state is a reference to task_chain_[0]

        // Run the specified state
        // Handlers should exit if requested_state != AXIS_STATE_UNDEFINED
        bool status;
        switch (current_state_) {
            case AXIS_STATE_MOTOR_CALIBRATION: {
                status = motor_.run_calibration();
            } break;

            case AXIS_STATE_ENCODER_INDEX_SEARCH: {
                if (!motor_.is_calibrated_)
                    goto invalid_state_label;
                if (encoder_.config_.idx_search_unidirectional && motor_.config_.direction==0)
                    goto invalid_state_label;

                // TODO: move code body to function in Encoder
                encoder_.config_.use_index = true;
                encoder_.index_found_ = false;

                bool orig_setting = config_.lockin_finish_on_enc_idx;
                config_.lockin_finish_on_enc_idx = true;
                status = run_lockin_spin();
                config_.lockin_finish_on_enc_idx = orig_setting;
            } break;

            case AXIS_STATE_ENCODER_DIR_FIND: {
                if (!motor_.is_calibrated_)
                    goto invalid_state_label;

                // TODO: move code body to function in Encoder
                int32_t init_enc_val = encoder_.shadow_count_;
                bool orig_setting = config_.lockin_finish_on_distance;
                config_.lockin_finish_on_distance = true;
                motor_.config_.direction = 1; // Must test spin forwards for direction detect logic
                status = run_lockin_spin();
                config_.lockin_finish_on_distance = orig_setting;

                if (status) {
                    // Check response and direction
                    if (encoder_.shadow_count_ > init_enc_val + 8) {
                        // motor same dir as encoder
                        motor_.config_.direction = 1;
                    } else if (encoder_.shadow_count_ < init_enc_val - 8) {
                        // motor opposite dir as encoder
                        motor_.config_.direction = -1;
                    } else {
                        motor_.config_.direction = 0;
                    }
                }
            } break;

            case AXIS_STATE_ENCODER_OFFSET_CALIBRATION: {
                if (!motor_.is_calibrated_)
                    goto invalid_state_label;
                status = encoder_.run_offset_calibration();
            } break;

            case AXIS_STATE_LOCKIN_SPIN: {
                if (!motor_.is_calibrated_ || motor_.config_.direction==0)
                    goto invalid_state_label;
                status = run_lockin_spin();
            } break;

            case AXIS_STATE_SENSORLESS_CONTROL: {
                if (!motor_.is_calibrated_ || motor_.config_.direction==0)
                        goto invalid_state_label;
                status = run_lockin_spin(); // TODO: restart if desired
                if (status) {
                    // call to controller.reset() that happend when arming means that vel_setpoint
                    // is zeroed. So we make the setpoint the spinup target for smooth transition.
                    controller_.vel_setpoint_ = config_.lockin_vel;
                    status = run_sensorless_control_loop();
                }
            } break;

            case AXIS_STATE_CLOSED_LOOP_CONTROL: {
                if (!motor_.is_calibrated_ || motor_.config_.direction==0)
                    goto invalid_state_label;
                if (!encoder_.is_ready_)
                    goto invalid_state_label;
                status = run_closed_loop_control_loop();
            } break;

            case AXIS_STATE_IDLE: {
                run_idle_loop();
                status = motor_.arm(); // done with idling - try to arm the motor
            } break;

            default:
            invalid_state_label:
                error_ |= ERROR_INVALID_STATE;
                status = false; // this will set the state to idle
                break;
        }

        // If the state failed, go to idle, else advance task chain
        if (!status)
            current_state_ = AXIS_STATE_IDLE;
        else
            memcpy(task_chain_, task_chain_ + 1, sizeof(task_chain_) - sizeof(task_chain_[0]));
    }
}
