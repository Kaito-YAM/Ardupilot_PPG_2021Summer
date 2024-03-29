// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include "Plane.h"

/*
  get a speed scaling number for control surfaces. This is applied to
  PIDs to change the scaling of the PID with speed. At high speed we
  move the surfaces less, and at low speeds we move them more.
 */
float Plane::get_speed_scaler(void)
{
    float aspeed, speed_scaler;
    if (ahrs.airspeed_estimate(&aspeed)) {
        if (aspeed > auto_state.highest_airspeed) {
            auto_state.highest_airspeed = aspeed;
        }
        if (aspeed > 0.0001f) {
            speed_scaler = g.scaling_speed / aspeed;
        } else {
            speed_scaler = 2.0;
        }
        speed_scaler = constrain_float(speed_scaler, 0.5f, 2.0f);
    } else {
        if (channel_throttle->get_servo_out() > 0) {
            speed_scaler = 0.5f + ((float)THROTTLE_CRUISE / channel_throttle->get_servo_out() / 2.0f);                 // First order taylor expansion of square root
            // Should maybe be to the 2/7 power, but we aren't going to implement that...
        }else{
            speed_scaler = 1.67f;
        }
        // This case is constrained tighter as we don't have real speed info
        speed_scaler = constrain_float(speed_scaler, 0.6f, 1.67f);
    }
    return speed_scaler;
}

/*
  return true if the current settings and mode should allow for stick mixing
 */
bool Plane::stick_mixing_enabled(void)
{
    if (auto_throttle_mode && auto_navigation_mode) {
        // we're in an auto mode. Check the stick mixing flag
        if (g.stick_mixing != STICK_MIXING_DISABLED &&
            geofence_stickmixing() &&
            failsafe.state == FAILSAFE_NONE &&
            !rc_failsafe_active()) {
            // we're in an auto mode, and haven't triggered failsafe
            return true;
        } else {
            return false;
        }
    }

    if (failsafe.ch3_failsafe && g.short_fs_action == 2) {
        // don't do stick mixing in FBWA glide mode
        return false;
    }

    // non-auto mode. Always do stick mixing
    return true;
}


/*
  this is the main roll stabilization function. It takes the
  previously set nav_roll calculates roll servo_out to try to
  stabilize the plane at the given roll
 */
void Plane::stabilize_roll(float speed_scaler)
{
    if (fly_inverted()) {
        // we want to fly upside down. We need to cope with wrap of
        // the roll_sensor interfering with wrap of nav_roll, which
        // would really confuse the PID code. The easiest way to
        // handle this is to ensure both go in the same direction from
        // zero
        nav_roll_cd += 18000;
        if (ahrs.roll_sensor < 0) nav_roll_cd -= 36000;
    }

    bool disable_integrator = false;
    if (control_mode == STABILIZE && channel_roll->get_control_in() != 0) {
        disable_integrator = true;
    }
    channel_roll->set_servo_out(rollController.get_servo_out(nav_roll_cd - ahrs.roll_sensor, 
                                                           speed_scaler, 
                                                           disable_integrator));
}

/*
  this is the main pitch stabilization function. It takes the
  previously set nav_pitch and calculates servo_out values to try to
  stabilize the plane at the given attitude.
 */
void Plane::stabilize_pitch(float speed_scaler)
{
    int8_t force_elevator = takeoff_tail_hold();
    if (force_elevator != 0) {
        // we are holding the tail down during takeoff. Just convert
        // from a percentage to a -4500..4500 centidegree angle
        channel_pitch->set_servo_out(45*force_elevator);
        return;
    }
    int32_t demanded_pitch = nav_pitch_cd + g.pitch_trim_cd + channel_throttle->get_servo_out() * g.kff_throttle_to_pitch;
    bool disable_integrator = false;
    if (control_mode == STABILIZE && channel_pitch->get_control_in() != 0) {
        disable_integrator = true;
    }
    channel_pitch->set_servo_out(pitchController.get_servo_out(demanded_pitch - ahrs.pitch_sensor, 
                                                             speed_scaler, 
                                                             disable_integrator));
}

/*
  perform stick mixing on one channel
  This type of stick mixing reduces the influence of the auto
  controller as it increases the influence of the users stick input,
  allowing the user full deflection if needed
 */
void Plane::stick_mix_channel(RC_Channel *channel, int16_t &servo_out)
{
    float ch_inf;
        
    ch_inf = (float)channel->get_radio_in() - (float)channel->get_radio_trim();
    ch_inf = fabsf(ch_inf);
    ch_inf = MIN(ch_inf, 400.0f);
    ch_inf = ((400.0f - ch_inf) / 400.0f);
    servo_out *= ch_inf;
    servo_out += channel->pwm_to_angle();
}

/*
  One argument version for when the servo out in the rc channel 
  is the target
 */
void Plane::stick_mix_channel(RC_Channel * channel)
{
   int16_t servo_out = channel->get_servo_out();
   stick_mix_channel(channel,servo_out);
   channel->set_servo_out(servo_out);
}

/*
  this gives the user control of the aircraft in stabilization modes
 */
void Plane::stabilize_stick_mixing_direct()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == QLAND ||
        control_mode == QRTL ||
        control_mode == TRAINING) {
        return;
    }
    stick_mix_channel(channel_roll);
    stick_mix_channel(channel_pitch);
}

/*
  this gives the user control of the aircraft in stabilization modes
  using FBW style controls
 */
void Plane::stabilize_stick_mixing_fbw()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == QLAND ||
        control_mode == QRTL ||
        control_mode == TRAINING ||
        (control_mode == AUTO && g.auto_fbw_steer == 42)) {
        return;
    }
    // do FBW style stick mixing. We don't treat it linearly
    // however. For inputs up to half the maximum, we use linear
    // addition to the nav_roll and nav_pitch. Above that it goes
    // non-linear and ends up as 2x the maximum, to ensure that
    // the user can direct the plane in any direction with stick
    // mixing.
    float roll_input = channel_roll->norm_input();
    if (roll_input > 0.5f) {
        roll_input = (3*roll_input - 1);
    } else if (roll_input < -0.5f) {
        roll_input = (3*roll_input + 1);
    }
    nav_roll_cd += roll_input * roll_limit_cd;
    nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit_cd, roll_limit_cd);
    
    float pitch_input = channel_pitch->norm_input();
    if (pitch_input > 0.5f) {
        pitch_input = (3*pitch_input - 1);
    } else if (pitch_input < -0.5f) {
        pitch_input = (3*pitch_input + 1);
    }
    if (fly_inverted()) {
        pitch_input = -pitch_input;
    }
    if (pitch_input > 0) {
        nav_pitch_cd += pitch_input * aparm.pitch_limit_max_cd;
    } else {
        nav_pitch_cd += -(pitch_input * pitch_limit_min_cd);
    }
    nav_pitch_cd = constrain_int32(nav_pitch_cd, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}


/*
  stabilize the yaw axis. There are 3 modes of operation:

    - hold a specific heading with ground steering
    - rate controlled with ground steering
    - yaw control for coordinated flight    
 */
void Plane::stabilize_yaw(float speed_scaler)
{
    // comment out by iwase 17/06/26
//    if (control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) {
//        // in land final setup for ground steering
//        steering_control.ground_steering = true;
//    } else {
//        // otherwise use ground steering when no input control and we
//        // are below the GROUND_STEER_ALT
//        steering_control.ground_steering = (channel_roll->get_control_in() == 0 &&
//                                            fabsf(relative_altitude()) < g.ground_steer_alt);
//        if (control_mode == AUTO &&
//                (flight_stage == AP_SpdHgtControl::FLIGHT_LAND_APPROACH ||
//                flight_stage == AP_SpdHgtControl::FLIGHT_LAND_PREFLARE)) {
//            // don't use ground steering on landing approach
//            steering_control.ground_steering = false;
//        }
//    }
//
//
//    /*
//      first calculate steering_control.steering for a nose or tail
//      wheel.
//      We use "course hold" mode for the rudder when either in the
//      final stage of landing (when the wings are help level) or when
//      in course hold in FBWA mode (when we are below GROUND_STEER_ALT)
//     */
//    if ((control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) ||
//        (steer_state.hold_course_cd != -1 && steering_control.ground_steering)) {
//        calc_nav_yaw_course();
//    } else if (steering_control.ground_steering) {
//        calc_nav_yaw_ground();
//    }
//
    /*
      now calculate steering_control.rudder for the rudder
     */
    calc_nav_yaw_coordinated(speed_scaler);
}


/*
  a special stabilization function for training mode
 */
void Plane::stabilize_training(float speed_scaler)
{
    if (training_manual_roll) {
        channel_roll->set_servo_out(channel_roll->get_control_in());
    } else {
        // calculate what is needed to hold
        stabilize_roll(speed_scaler);
        if ((nav_roll_cd > 0 && channel_roll->get_control_in() < channel_roll->get_servo_out()) ||
            (nav_roll_cd < 0 && channel_roll->get_control_in() > channel_roll->get_servo_out())) {
            // allow user to get out of the roll
            channel_roll->set_servo_out(channel_roll->get_control_in());            
        }
    }

    if (training_manual_pitch) {
        channel_pitch->set_servo_out(channel_pitch->get_control_in());
    } else {
        stabilize_pitch(speed_scaler);
        if ((nav_pitch_cd > 0 && channel_pitch->get_control_in() < channel_pitch->get_servo_out()) ||
            (nav_pitch_cd < 0 && channel_pitch->get_control_in() > channel_pitch->get_servo_out())) {
            // allow user to get back to level
            channel_pitch->set_servo_out(channel_pitch->get_control_in());            
        }
    }

    stabilize_yaw(speed_scaler);
}


/*
  this is the ACRO mode stabilization function. It does rate
  stabilization on roll and pitch axes
 */
void Plane::stabilize_acro(float speed_scaler)
{
    float roll_rate = (channel_roll->get_control_in()/4500.0f) * g.acro_roll_rate;
    float pitch_rate = (channel_pitch->get_control_in()/4500.0f) * g.acro_pitch_rate;

    /*
      check for special roll handling near the pitch poles
     */
    if (g.acro_locking && is_zero(roll_rate)) {
        /*
          we have no roll stick input, so we will enter "roll locked"
          mode, and hold the roll we had when the stick was released
         */
        if (!acro_state.locked_roll) {
            acro_state.locked_roll = true;
            acro_state.locked_roll_err = 0;
        } else {
            acro_state.locked_roll_err += ahrs.get_gyro().x * G_Dt;
        }
        int32_t roll_error_cd = -ToDeg(acro_state.locked_roll_err)*100;
        nav_roll_cd = ahrs.roll_sensor + roll_error_cd;
        // try to reduce the integrated angular error to zero. We set
        // 'stabilze' to true, which disables the roll integrator
        channel_roll->set_servo_out(rollController.get_servo_out(roll_error_cd,
                                                                speed_scaler,
                                                                true));
    } else {
        /*
          aileron stick is non-zero, use pure rate control until the
          user releases the stick
         */
        acro_state.locked_roll = false;
        channel_roll->set_servo_out(rollController.get_rate_out(roll_rate,  speed_scaler));
    }

    if (g.acro_locking && is_zero(pitch_rate)) {
        /*
          user has zero pitch stick input, so we lock pitch at the
          point they release the stick
         */
        if (!acro_state.locked_pitch) {
            acro_state.locked_pitch = true;
            acro_state.locked_pitch_cd = ahrs.pitch_sensor;
        }
        // try to hold the locked pitch. Note that we have the pitch
        // integrator enabled, which helps with inverted flight
        nav_pitch_cd = acro_state.locked_pitch_cd;
        channel_pitch->set_servo_out(pitchController.get_servo_out(nav_pitch_cd - ahrs.pitch_sensor,
                                                                  speed_scaler,
                                                                  false));
    } else {
        /*
          user has non-zero pitch input, use a pure rate controller
         */
        acro_state.locked_pitch = false;
        channel_pitch->set_servo_out( pitchController.get_rate_out(pitch_rate, speed_scaler));
    }

    /*
      manual rudder for now
     */
    steering_control.steering = steering_control.rudder = rudder_input;
}

/*
  main stabilization function for all 3 axes
 */
void Plane::stabilize()
{
    // added by iwase 17/07/30
    if(init_TLAB_Controller_flag){
        init_TLAB_Controller();
    }
    if (control_mode == MANUAL) {
        // nothing to do
        return;
    }
    float speed_scaler = get_speed_scaler();

    if (control_mode == TRAINING) {
        stabilize_training(speed_scaler);
    } else if (control_mode == ACRO) {
        stabilize_acro(speed_scaler);
    } else if (control_mode == QSTABILIZE ||
               control_mode == QHOVER ||
               control_mode == QLOITER ||
               control_mode == QLAND ||
               control_mode == QRTL) {
        quadplane.control_run();
    } else {
        if (g.stick_mixing == STICK_MIXING_FBW && control_mode != STABILIZE) {
            stabilize_stick_mixing_fbw();
        }
        stabilize_roll(speed_scaler);
        stabilize_pitch(speed_scaler);
        if (g.stick_mixing == STICK_MIXING_DIRECT || control_mode == STABILIZE) {
            stabilize_stick_mixing_direct();
        }
        stabilize_yaw(speed_scaler);
    }

    /*
      see if we should zero the attitude controller integrators. 
     */
    if (channel_throttle->get_control_in() == 0 &&
        relative_altitude_abs_cm() < 500 && 
        fabsf(barometer.get_climb_rate()) < 0.5f &&
        gps.ground_speed() < 3) {
        // we are low, with no climb rate, and zero throttle, and very
        // low ground speed. Zero the attitude controller
        // integrators. This prevents integrator buildup pre-takeoff.
        rollController.reset_I();
        pitchController.reset_I();
        yawController.reset_I();

        // if moving very slowly also zero the steering integrator
        if (gps.ground_speed() < 1) {
            steerController.reset_I();            
        }
    }
}


void Plane::calc_throttle()

{
    // comment outed by iwase 17/07/29
//    if (aparm.throttle_cruise <= 1) {
//        // user has asked for zero throttle - this may be done by a
//        // mission which wants to turn off the engine for a parachute
//        // landing
//        channel_throttle->set_servo_out(0);
//        return;
//    }
//
//    int32_t commanded_throttle = SpdHgt_Controller->get_throttle_demand();
//
//    // Received an external msg that guides throttle in the last 3 seconds?
//    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
//            plane.guided_state.last_forced_throttle_ms > 0 &&
//            millis() - plane.guided_state.last_forced_throttle_ms < 3000) {
//        commanded_throttle = plane.guided_state.forced_throttle;
//    }
    // added by iwase 17/07/29
    int32_t commanded_throttle = TLAB_Throttle_Controller();

    channel_throttle->set_servo_out(commanded_throttle);
}
/*
void max_min_z(float max_z1, float min_z1, float max_z2, float min_z2, float max_z3, float min_z3, int num){
	if (num==1){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -10 to 10(deg.)
		max_z1=-0.19956;
		min_z1=-0.80692;
		max_z2=13.9932;
		min_z2=10.2479;
		max_z3=0.31838;
		min_z3=0.073926;
	} else if (num==2){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -10 to 10(deg.)
		max_z1=-0.19956;
		min_z1=-0.80692;
		max_z2=13.9932;
		min_z2=10.2479;
		max_z3=0.31838;
		min_z3=0.073926;
	} else if(num==3){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -5 to 5(deg.)
		max_z1=-0.19956;
		min_z1=-0.80692;
		max_z2=13.2529;
		min_z2=11.1097;
		max_z3=0.25977;
		min_z3=0.13707;
	} else {
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -5 to 5(deg.)
		max_z1=-0.19956;
		min_z1=-0.80692;
		max_z2=13.2529;
		min_z2=11.1097;
		max_z3=0.25977;
		min_z3=0.13707;
	}
}
*/

void switch_controller_alt(int number_controller_alt, float f[8][4], float z[3][2]){
	//switching the feedback gain designed by LMI, added by hatae 2021/7/28
	if (number_controller_alt==1){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -10 to 10(deg.)
    	//alpha 0.01
    	//x(0)=[2;1;0;0]
    	//mu=6.5
        float F[8][4]={
        	{0.24887,3.8117,-0.014127,0.24208},
        	{0.2156,3.5863,-0.036707,0.20848},
        	{0.20803,4.1414,-0.0020214,0.21848},
        	{0.17783,3.8637,-0.026582,0.17963},
        	{0.16185,4.549,0.015949,0.164},
        	{0.15125,3.97,-0.019482,0.15579},
        	{0.10078,4.7296,0.047139,0.19323},
        	{0.1166,4.0331,-0.0075814,0.13821}
        };
        float Z[3][2]={
        	{-0.19956,-0.80692},
        	{13.9932,10.2479},
        	{0.31838,0.073926}
        };
        for (int i=0;i<8;i++){
        	for (int j=0;j<4;j++){
        		f[i][j]=F[i][j];
        	}
        }
        for (int i=0;i<3;i++){
        	for (int j=0;j<2;j++){
        		z[i][j]=Z[i][j];
        	}
        }
    } else if(number_controller_alt==2){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -10 to 10(deg.)
    	//alpha 0.1
    	//x(0)=[2;1.5;0;0]
    	//mu=12
        float F[8][4]={
        	{0.99609,4.3079,0.085038,0.29888},
        	{0.82427,4.8389,0.056465,0.26573},
        	{0.93049,5.3483,0.087198,0.29675},
        	{0.7685,5.1532,0.056999,0.25737},
        	{0.78918,6.2386,0.089284,0.26875},
        	{0.63431,5.6553,0.054239,0.22551},
        	{0.60786,6.5665,0.10033,0.24711},
        	{0.54911,5.8333,0.05316,0.21386}
        };
        float Z[3][2]={
        	{-0.19956,-0.80692},
        	{13.9932,10.2479},
        	{0.31838,0.073926}
        };
        for (int i=0;i<8;i++){
          for (int j=0;j<4;j++){
            f[i][j]=F[i][j];
          }
        }
        for (int i=0;i<3;i++){
        	for (int j=0;j<2;j++){
        		z[i][j]=Z[i][j];
        	}
        }
    } else if(number_controller_alt==3){
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -5 to 5(deg.)
    	//alpha 0.2
    	//x(0)=[3;1.5;0;0]
    	//mu=12
        float F[8][4]={
        	{1.1101,2.7506,0.062397,0.22145},
        	{1.1221,3.1775,0.057379,0.22023},
        	{1.1661,3.7206,0.065712,0.23362},
        	{1.1288,3.8152,0.058197,0.22173},
        	{0.92763,4.9578,0.057638,0.19507},
        	{0.88139,4.6416,0.049835,0.1845},
        	{0.78222,4.9678,0.056559,0.1849},
        	{0.79436,4.6206,0.047368,0.17421}
        };
        float Z[3][2]={
        	{-0.19956,-0.80692},
        	{13.2529,11.1097},
        	{0.25977,0.13707}
        };
        for (int i=0;i<8;i++){
          for (int j=0;j<4;j++){
            f[i][j]=F[i][j];
          }
        }
        for (int i=0;i<3;i++){
        	for (int j=0;j<2;j++){
        		z[i][j]=Z[i][j];
        	}
        }
    } else {
    	//dz_d -1.5 to 1.5(m/s)
    	//theta -5 to 5(deg.)
    	//alpha 0.01
    	//x(0)=[2;1;0;0]
    	//mu=6.5
        float F[8][4]={
        	{0.39106,2.2661,-0.25992,0.16117},
        	{0.35661,2.1644,-0.29158,0.15248},
        	{0.31853,2.5284,-0.1838,0.16855},
        	{0.27446,2.4481,-0.20659,0.14603},
        	{0.27142,2.8615,-0.080487,0.077737},
        	{0.26178,2.6207,-0.20091,0.10142},
        	{0.23188,2.9284,-0.065439,0.18728},
        	{0.19221,2.7164,-0.11955,0.13071}
        };
        float Z[3][2]={
        	{-0.19956,-0.80692},
        	{13.2529,11.1097},
        	{0.25977,0.13707}
        };
        for (int i=0;i<8;i++){
          for (int j=0;j<4;j++){
            f[i][j]=F[i][j];
          }
        }
        for (int i=0;i<3;i++){
        	for (int j=0;j<2;j++){
        		z[i][j]=Z[i][j];
        	}
        }
    }
}


int32_t Plane::TLAB_Throttle_Controller(void)
{
    float h_Th[2];
    int32_t z_cm = current_loc.alt;
    int32_t z_r_cm = next_WP_loc.alt;
    z = z_cm * 0.01f;  // POS高度 [m]
    z_r= z_r_cm * 0.01f;
    uint64_t current_time_Th =  AP_HAL::micros64();

    if(firsttime_Th){
        firsttime_Th = false;
        z_old = z_cm;
        pitch_old=ahrs.pitch;
        dz = 0;
        err_count = 0;
        prev_time_Th = current_time_Th;
        d1_Th = g.TPARAM_pdc_height_d1_Th;
        d2_Th = g.TPARAM_pdc_height_d2_Th;
        kp_Th[0] = g.TPARAM_height_kp0_Th;
        kp_Th[1] = g.TPARAM_height_kp1_Th;
        kd_Th[0] = g.TPARAM_height_kd0_Th;
        kd_Th[1] = g.TPARAM_height_kd1_Th;
        motor_neutral = g.TPARAM_motor_neutral_Th;
    }
    past_time_Th = current_time_Th - prev_time_Th;
    float past_time_Th_f = (float)past_time_Th;
    past_time_Th_f *= 1e-6;
//    if (z_cm == z_old || past_time_Th == 0){
//        err_count++;
//    }else{
//        int32_t dz_int = z_cm - z_old;
//        float dz_f = (float)dz_int;
//        dz = dz_f*0.01/(past_time_Th_f);
//        z_old = z_cm;
//        prev_time_Th = current_time_Th;
//    }
//    if (ahrs.pitch == pitch_old || past_time_Th == 0){
//            err_count++;
//        }else{
//            d_pitch = ahrs.pitch - pitch_old;
//            //float dz_f = (float)dz_int;
//            speed_pitch = d_pitch/(past_time_Th_f);
//            pitch_old = ahrs.pitch;
//            prev_time_Th = current_time_Th;
//        }
    //
    if (past_time_Th == 0){
            err_count++;
            z_old = z_cm;
            pitch_old = ahrs.pitch;
            prev_time_Th = current_time_Th;
        }else{
            int32_t dz_int = z_cm - z_old;
            dz_f = (float)dz_int;
            d_pitch = ahrs.pitch - pitch_old;
            dz = dz_f*0.01/(past_time_Th_f);  // 高度の時間微分 [m/s]
            speed_pitch = d_pitch/(past_time_Th_f);
            z_old = z_cm;
            pitch_old = ahrs.pitch;
            prev_time_Th = current_time_Th;
        }
    //
    e_m = z - z_r;  // 高度の制御偏差 [m]
    de_m = dz;  // 高度の時間微分 [m/s]
    h_Th[0] = constrain_float((de_m - d2_Th)/(d1_Th - d2_Th),0.0f,1.0f);
    h_Th[1] = constrain_float((d1_Th - de_m)/(d1_Th - d2_Th),0.0f,1.0f);
    gps_dh = -gps.velocity().z;//add by aoki
    gps_dpitch= wrap_PI(ahrs.get_gyro().y);//added by hatae

    //2.49(N)平衡点（ミッションプランナーより入力）
    //↓風洞実験の式を使用して平衡点(N)の計算
    if(g.TPARAM_cha_pow == 1){
    	//PD? Controller(Iwase or Nishizuka)
        motor_Th_N = (1/cosf(g.TPARAM_theta_a*M_PI/180))*(0.1059*g.TPARAM_V_a*g.TPARAM_V_a-0.3342*g.TPARAM_V_a +1.6227);
    for(int i = 0; i <= 1; i++){
        motor_Th_N += -h_Th[i]*(kp_Th[i]*e_m  + kd_Th[i]*de_m);
    }
    }else if(g.TPARAM_cha_pow == 2){
    	//SOS Controller(Nishizuka)
        motor_Th_N = motor_neutral;
        F1=0.0080059*e_m-0.0057775*de_m+0.17926*(ahrs.pitch-g.TPARAM_theta_a*M_PI/180)+0.049755*speed_pitch+0.3259;
        F2=-0.0057775*e_m-0.0011929*de_m+0.063286*(ahrs.pitch-g.TPARAM_theta_a*M_PI/180)-0.020788*speed_pitch+0.09056;
        F3=0.17926*e_m+0.063286*de_m+5.1494*(ahrs.pitch-g.TPARAM_theta_a*M_PI/180)+0.52875*speed_pitch+16.1688;
        F4=0.049755*e_m-0.020788*de_m+0.52875*(ahrs.pitch-g.TPARAM_theta_a*M_PI/180)+0.19909*speed_pitch+2.9633;
      //  motor_Th_N +=-(F1*e_m+F2*de_m+F3*(ahrs.pitch-g.TPARAM_theta_a*M_PI/180)+F4*speed_pitch);
        motor_Th_N +=-(F1*e_m+F2*de_m);
    }else if(g.TPARAM_cha_pow == 3){
    	//PD Controller
        motor_Th_N = motor_neutral;
        motor_Th_N +=-(kp_Th[0]*e_m + kd_Th[0]*de_m);
    }else if(g.TPARAM_cha_pow == 4){
    	//LQR Controller, but not done yet, added by hatae
    	motor_Th_N = 0;
    }else if(g.TPARAM_cha_pow == 5){
    	//KI Controller, but not done yet, added by hatae
    	motor_Th_N = 0;
    }else if(g.TPARAM_cha_pow == 6){
    	//LMI controller, added by hatae 2021/7/28
    	//定数
    	float c_m_1=1.700f*pow(10.0f,-1.0f); //small m
    	float c_m_2=1.197f;
    	float c_C_L=4.660f*pow(10.0f,-1.0f);
    	float c_C_D=1.640f*pow(10.0f,-1.0f);
    	float c_rho=1.293f;
    	float c_l=9.59f*pow(10.0f,-1.0f);
    	float c_l_g=1.36f*pow(10.0f,-1.0f);
    	float c_S=9.424f*pow(10.0f,-1.0f);
    	float c_I_y=1.37f*pow(10.0f,-1.0f);
    	float c_alpha=4.36f/180.0f*M_PI;
    	float c_L=c_rho*c_C_L*c_S;
    	float c_D=c_rho*c_C_D*c_S;

    	//コントローラ選択(フィードバックゲイン)
    	int num=g.TPARAM_c_alt;
    	//最大値最小値設定
        float maxmin_z[3][2];
        //フィードバックゲイン指定
        float f[8][4];
        switch_controller_alt(num,f,maxmin_z);
        //平衡点周りの変数
        float theta_n=15.8f/180.0f*M_PI;
        float T_neutral=4.46f;
        float theta_r=ahrs.pitch-theta_n;
        float v_xn=6.55;
        //z1からz3の設定
        float z1=(1.0f/2.0f*c_L*gps_dh*cos(atan(gps_dh/v_xn))-1.0f/2.0f*c_D*gps_dh*sin(atan(gps_dh/v_xn))-1.0f/2.0f*c_D*v_xn)/(c_m_1+c_m_2);
        float z2=(-1.0f/2.0f*(c_l-c_l_g)*c_L*gps_dpitch*sin(theta_r+theta_n+alpha-atan(gps_dh/v_xn))+1.0f/2.0f*(c_l-c_l_g)*c_l*v_xn*cos(theta_r)*cos(theta_n+alpha)+1.0f/2.0f*(c_l-c_l_g)*c_D*gps_dh*cos(theta_r+theta_n+alpha-atan(gps_dh/v_xn))+1.0f/2.0f*(c_l-c_l_g)*c_D*v_xn*cos(theta_r)*sin(theta_n+alpha)-1.0f/2.0f*(c_l-c_l_g)*c_L*v_xn*sin(theta_r)*sin(theta_n+alpha)-1.0f/2.0f*(c_l-c_l_g)*c_D*v_xn*sin(theta_r)*cos(theta_n+alpha))/c_I_y;
        float z3=sin(theta_r+theta_n)/(c_m_1+c_m_2);
        //メンバシップ関数指定
        float mem_M[2];
        float mem_N[2];
        float mem_L[2];
        float h[8];
        //メンバシップ関数
        mem_M[0]=constrain_float((maxmin_z[0][0]-z1)/(maxmin_z[0][0]-maxmin_z[0][1]),0.0f,1.0f);
        mem_M[1]=constrain_float((z1-maxmin_z[0][1])/(maxmin_z[0][0]-maxmin_z[0][1]),0.0f,1.0f);
        mem_N[0]=constrain_float((maxmin_z[1][0]-z2)/(maxmin_z[1][0]-maxmin_z[1][1]),0.0f,1.0f);
        mem_N[1]=constrain_float((z2-maxmin_z[1][1])/(maxmin_z[1][0]-maxmin_z[1][1]),0.0f,1.0f);
        mem_L[0]=constrain_float((maxmin_z[2][0]-z3)/(maxmin_z[2][0]-maxmin_z[2][1]),0.0f,1.0f);
        mem_L[1]=constrain_float((z3-maxmin_z[2][1])/(maxmin_z[2][0]-maxmin_z[2][1]),0.0f,1.0f);
        //メンバシップ関数計算
        int num_loop=0;
        for (int i=0;i<2;i++){
          for (int j=0;j<2;j++){
            for (int k=0;k<2;k++){
              h[num_loop]=mem_M[i]*mem_N[j]*mem_L[k];
              num_loop+=1;
            }
          }
        }
        //ログ残し用変数
        h_0=h[0];
        h_1=h[1];
        h_2=h[2];
        h_3=h[3];
        h_4=h[4];
        h_5=h[5];
        h_6=h[6];
        h_7=h[7];
        float x_r[4]={e_m,gps_dh,theta_r,gps_dpitch};
        //推力計算
        motor_Th_N=T_neutral;
        float motor_Th_N_i;
        for (int i=0;i<8;i++){
        	motor_Th_N_i=0;
        	for (int j=0;j<4;j++){
        		motor_Th_N_i+=f[i][j]*x_r[j];
        	}
        	motor_Th_N+=-h[i]*motor_Th_N_i;
        }
    }
    //↓motor_Th_Nがスピコン変換用のプログラムを通して%出力になる
    motor_per = thrust_to_percent(motor_Th_N);
    return static_cast<int32_t>(motor_per);
}

//
float Plane::thrust_to_percent(float thrust) {

//  float a = 0.000910328864658;
//  float c = -0.207848958892118;
//  float offset = -0.456728006689083;
    //2015/10/03
//  float a = 0.000689550869658;
//  float c = -0.157440275952635;
//  float offset = -0.274096113464822;
//    if(g.TPARAM_cha_pow == 1){
//            float a = 0.000624704535504;
//            float c = -0.142634612315694;
//            float offset = -0.117384315454501;
//        int32_t value = (int32_t)(sqrtf((thrust - c) / a) - offset);
//        //
//        if (thrust < 0.1)
//            return 0;
//        else {
//            return constrain_int32(value, 0, 100);
//        }
//    }else{
            float a = 0.002287471638222;
            float c = 0.069756864241495;
            //float offset = -0.117384315454501;
            airpower = (1/cosf(g.TPARAM_theta_a*M_PI/180))*(0.1059*g.TPARAM_V_a*g.TPARAM_V_a-0.3342*g.TPARAM_V_a +1.6227);
            //float motor_neutral_2 = sqrtf((motor_neutral - c) / a);
            kk = (airpower)/(a*g.TPARAM_neutral_t*g.TPARAM_neutral_t + c);
            int32_t value = (int32_t) ((sqrtf((thrust - c*kk) / (a*kk))));
                 value1 = value;
                     if (value1<0){
                         value2=0;
                     }else if(value1>80){
                         value2=80;
                     }else{
                         value2=value1;
                     }
            //power15 = kk*(a*pow(15,2)+c);
            if (thrust < 0.3256)
                return 0;
           else {
                return constrain_int32(value, 0, g.TPARAM_MAX_slo);
//            }
    }
}

/*****************************************
* Calculate desired roll/pitch/yaw angles (in medium freq loop)
*****************************************/

/*
  calculate yaw control for coordinated flight
 */
void Plane::calc_nav_yaw_coordinated(float speed_scaler)
{
//    added by iwase 17/06/26
//    bool disable_integrator = false;
//    if (control_mode == STABILIZE && rudder_input != 0) {
//        disable_integrator = true;
//    }
//
//    int16_t commanded_rudder;
//
//    // Received an external msg that guides yaw in the last 3 seconds?
//    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
//            plane.guided_state.last_forced_rpy_ms.z > 0 &&
//            millis() - plane.guided_state.last_forced_rpy_ms.z < 3000) {
//        commanded_rudder = plane.guided_state.forced_rpy_cd.z;
//    } else {
//        commanded_rudder = yawController.get_servo_out(speed_scaler, disable_integrator);
//
//        // add in rudder mixing from roll
//        commanded_rudder += channel_roll->get_servo_out() * g.kff_rudder_mix;
//        commanded_rudder += rudder_input;
//    }
//    steering_control.rudder = constrain_int16(commanded_rudder, -4500, 4500);
//added by iwase 17/06/26
    TLAB_CMD_index = mission.get_current_nav_index();
//    TLAB_CMD_total = g.command_total;

    // Commented out by Kaito Yamamoto 2021.07.12.
    /*
    if(!Combine_Mode_flag){ //円と直線の組み合わせ飛行ではない
        switch(TLAB_Control_flag){
        case 1: // 円追従左旋回
        case 2: //　円追従右旋回
            steering_control.rudder = constrain_int16(TLAB_Circle_Trace_Controller(), -4500, 4500);
            break;
        default : //0 -> 直線追従
            steering_control.rudder = constrain_int16(TLAB_Line_Trace_Controller(), -4500, 4500);
            break;
        }
    }else{
        steering_control.rudder = constrain_int16(TLAB_Combine_Controller(), -4500, 4500);
    }
    */

    // Added by Kaito Yamamoto 2021.08.11.
    if (g.TPARAM_Bar_Control_Mode == 1) {
    	// 過去の直線追従コントローラ  "g.TPARAM_switch_mo" でコントローラ切り替え
    	steering_control.rudder = constrain_int16(TLAB_Line_Trace_Controller(), -4500, 4500);
    }
    else if (g.TPARAM_Bar_Control_Mode == 2) {
    	// MPで指定する "g.TPARAM_s_neutral" [deg]の一定値をサーボモーター角度として出力する
    	steering_control.rudder = constrain_int16(TLAB_Constant_Output(), -4500, 4500);
    }
    else {
        // Added by Kaito Yamamoto 2021.07.12.
        //多様な2次元経路追従コントローラ
       	steering_control.rudder = constrain_int16(TLAB_2D_Trace_Controller(), -4500, 4500);
    }
}

void Plane::init_TLAB_Controller(void)
{
    init_TLAB_Controller_flag = false;
    Target_Circle_Center.lat = g.TPARAM_center_lat;
    Target_Circle_Center.lng = g.TPARAM_center_lng;
    TLAB_Control_flag = g.TPARAM_control_mode;
    calc_GCRS_flag = g.TPARAM_calc_GCRS;
    const_k = g.TPARAM_k;
    v_a = g.TPARAM_Va;
    Vg_min = g.TPARAM_Vg_min;
    Vg_max = g.TPARAM_Vg_max;
    alpha_min = g.TPARAM_alpha_min*M_PI/180.0f;
    alpha_max = g.TPARAM_alpha_max*M_PI/180.0f;
    r_min = g.TPARAM_r_min;
    target_R = g.TPARAM_R;
    U_min = g.TPARAM_U_min;
    U_max = g.TPARAM_U_max;
    // 比較ファジィ制御用
    if (g.TPARAM_rule_num != 0) {
        use_fuzzy_controller = true;
        (g.TPARAM_rule_num == 2)?(rule_num = 2):(rule_num = 4);
        phi_max = g.TPARAM_phi_max*M_PI/180.0f;
        F_fuzzy[0][0] = g.TPARAM_F11;
        F_fuzzy[0][1] = g.TPARAM_F12;
        F_fuzzy[1][0] = g.TPARAM_F21;
        F_fuzzy[1][1] = g.TPARAM_F22;
        F_fuzzy[2][0] = g.TPARAM_F31;
        F_fuzzy[2][1] = g.TPARAM_F32;
        F_fuzzy[3][0] = g.TPARAM_F41;
        F_fuzzy[3][1] = g.TPARAM_F42;
    }else{
        use_fuzzy_controller = false;
    }
    // 円と直線の組み合わせ飛行用
    Combine_Mode_flag = (g.TPARAM_Combine==1);//g.TPARAM=1のとき真(比較演算子)
    TLAB_WP_nav_flag = false;
    alternate_orbit_flag = (g.TPARAM_alternate==1);
    change_to_circle_flag = false;
    if (Combine_Mode_flag){
        TLAB_Control_flag = 0; //組み合わせ飛行の時は最初は必ず直線追従モード
    }
    change_y = g.TPARAM_change_y;
    orbit_num = g.TPARAM_orbit_num;
}

void Plane::init_TLAB_Controller_AUTO(void)
{
    init_TLAB_Controller_AUTO_flag = false;
    prev_POS = current_loc;
    mid_POS = current_loc;
    theta = wrap_PI(static_cast<float>(get_bearing_cd(Target_Circle_Center,mid_POS))*0.01f*M_PI/180.0f);
    prev_theta = theta;
    Int_theta = 0;
    chi = wrap_PI(static_cast<float>(gps.ground_course_cd())*0.01f*M_PI/180.0f);
}

//added by iwase 17/06/26
float Plane::calc_controller(float x1, float x2)
{
    if(prev_POS.lat != current_loc.lat || prev_POS.lng != current_loc.lng){
        prev_POS = current_loc;
    }

    if (!use_fuzzy_controller){ // 有理多項式コントローラ
        // 2017-08-03no1 decay rate
        //    float C = -0.00064483*x1*x1*x1 - 0.0076749*x1*x1*x2 + 0.0052302*x1*x2*x2 - 0.00095738*x2*x2*x2
        //            + 1.6742e-10*x1*x1 + 1.9249e-09*x1*x2 - 3.6543e-10*x2*x2 - 0.0017774*x1 - 0.029072*x2;
        //    float p = 0.54266*x1*x1 + 0.0077622*x1*x2 + 0.22527*x2*x2 - 1.4174e-07*x1 - 4.924e-09*x2 + 1.3274;
        //    return C/p;

        // 2017-07-27no2 decay rate
//        float C = -0.0001009*x1*x1*x1 - 0.00058231*x1*x1*x2 + 0.0011526*x1*x2*x2 - 0.00049326*x2*x2*x2
//                + 4.252e-11*x1*x1 + 2.0754e-10*x1*x2 - 6.5417e-10*x2*x2 - 0.0027384*x1 - 0.046293*x2;
//        float p = 0.066351*x1*x1 + 0.018827*x1*x2 + 0.83599*x2*x2 - 2.7996e-08*x1 + 4.2446e-08*x2 + 1.399;
//        return C/p;

        // 2017-08-13-no7 decay rate
//        float C = -0.0002153*x1*x1*x1 - 0.0054878*x1*x1*x2 + 0.011556*x1*x2*x2 - 0.048155*x2*x2*x2
//          + 1.0064e-10*x1*x1 + 2.4047e-09*x1*x2 - 8.6895e-09*x2*x2 - 0.025378*x1 - 0.84804*x2;
//        float p = 0.42164*x1*x1 - 0.64182*x1*x2 + 14.5681*x2*x2 - 1.9678e-07*x1 + 6.1901e-07*x2 + 19.314;
//

        // 2017-08-22 controller2
//        SOL.alpha{3} = -0.0336
//        float C =  -0.0016834*x1*x1*x1 - 0.031967*x1*x1*x2 + 0.031117*x1*x2*x2 - 0.1068*x2*x2*x2
//          + 6.5845e-09*x1*x1 + 1.3074e-07*x1*x2 - 1.0582e-07*x2*x2 - 0.02352*x1 - 0.96118*x2;
//        float p = 2.939*x1*x1 - 1.3108*x1*x2 + 23.4547*x2*x2 - 1.1239e-05*x1 + 4.7209e-06*x2 + 18.2868;
        // 2017-08-22 controller3
//        SOL.alpha{5} = -0.0400
//        float C = -0.00061307*x1*x1*x1 - 0.0088527*x1*x1*x2 + 0.022075*x1*x2*x2 - 0.15904*x2*x2*x2
//          - 3.7948e-11*x1*x1 - 3.7631e-10*x1*x2 + 1.6026e-09*x2*x2 - 0.032922*x1 - 1.1063*x2;
//        float p = 0.91724*x1*x1 - 1.5649*x1*x2 + 26.6455*x2*x2 + 8.0325e-08*x1 - 1.9019e-07*x2 + 21.5489;

//        2017-08-22 controller6
//        SOL.alpha{15} = -0.0427
//        float C = -0.00042755*x1*x1*x1 - 0.006522*x1*x1*x2 + 0.021443*x1*x2*x2 - 0.10879*x2*x2*x2
//          - 3.8546e-13*x1*x1 + 2.1641e-11*x1*x2 - 1.7535e-10*x2*x2 - 0.028681*x1 - 0.9248*x2;
//        float p = 0.63375*x1*x1 - 1.5095*x1*x2 + 19.8394*x2*x2 - 1.2029e-09*x1 + 5.3263e-09*x2 + 19.6162;
//
//        2017-08-23no3
//        SOL.alpha{24} = -0.0432
//        SOL.V{24} = 0.00048428*x1^2 + 0.037259*x1*x2 + 0.85429*x2^2
        float C = -0.0013832*pow(x1,3) - 0.020961*pow(x1,2)*x2 + 0.068193*x1*pow(x2,2) - 0.34265*pow(x2,3)
                - 1.3518e-10*pow(x1,2) - 1.9737e-09*x1*x2 + 7.2815e-09*pow(x2,2) - 0.096888*x1 - 3.1216*x2;
        float p = 0.78953*pow(x1,2) - 1.8155*x1*x2 + 24.7114*pow(x2,2) + 8.1027e-08*x1 - 2.4228e-07*x2 + 25.6252;

        return C/p;

    }else{ // 比較用Fuzzyコントローラ
        float m_mem[2];
        float n_mem[2];
        if(v_g > Vg_max){
            m_mem[0] = 1.0f;
            m_mem[1] = 0.0f;
        }else if(v_g < Vg_min){
            m_mem[0] = 0.0f;
            m_mem[1] = 1.0f;
        }else{
            m_mem[0] = (v_g - Vg_min)/(Vg_max - Vg_min);
            m_mem[1] = 1.0f - m_mem[0];
        }

        if (rule_num == 2){
            h_mem[0] = m_mem[0];
            h_mem[1] = m_mem[1];
            h_mem[2] = 0.0f;
            h_mem[3] = 0.0f;
        }else{
            if(fabsf(x2) < 2*180/M_PI){
                n_mem[0] = 1.0f;
                n_mem[1] = 0.0f;
            }else if(fabsf(x2) > phi_max){
                n_mem[0] = 0.0f;
                n_mem[1] = 1.0f;
            }else{
                float b1 = 1.0f;
                float b2 = sinf(phi_max)/phi_max;
                n_mem[0] = (sinf(x2) - b2*x2)/((b1 - b2)*x2);
                n_mem[1] = 1.0f - n_mem[0];
            }
            h_mem[0] = m_mem[0]*n_mem[0];
            h_mem[1] = m_mem[0]*n_mem[1];
            h_mem[2] = m_mem[1]*n_mem[0];
            h_mem[3] = m_mem[1]*n_mem[1];
        }
        float u_control = 0;
        for(int i = 0;i < rule_num; i++){
            u_control -= h_mem[i]*(F_fuzzy[i][0]*x1 + F_fuzzy[i][1]*x2);
        }
        return u_control;
    }
    return 0;
}
// added by Nishiduka 2018/4/19
float sinc(const float x){

    if ( -pow(10,-2) <= x && x <= pow(10,-2)){
        return 1;
    }else{
        return sinf(x)/x;}

}
//added by Nishiduka 2018/4/19
float sqn(const float x){

    if(x < 0){

        return -1;

    }else if(x > 0){

        return 1;
    }

    return 0;

}

int32_t Plane::TLAB_Line_Trace_Controller()
{
    Dist_currWP2UAV = get_distance(current_loc, prev_WP_loc);
    rad_prevWP2UAV = (float)(get_bearing_cd(prev_WP_loc, current_loc)*0.01*M_PI/180.0);
    rad_prevWP2currWP = (float)(get_bearing_cd(prev_WP_loc, next_WP_loc)*0.01*M_PI/180.0);
//    int16_t cd_prevWP2UAV = get_bearing_cd(prev_WP_loc, current_loc);
//    int16_t cd_prevWP2currWP = get_bearing_cd(prev_WP_loc, next_WP_loc);
    float rad_WPline2UAV = rad_prevWP2UAV - rad_prevWP2currWP;
//    int16_t cd_WPline2UAV = cd_prevWP2UAV - cd_prevWP2currWP;
//    float rad_WPline2UAV = (float)cd_WPline2UAV*0.01*M_PI/180.0;

    state_UAV_x = Dist_currWP2UAV*cosf(rad_WPline2UAV);
    state_UAV_y = Dist_currWP2UAV*sinf(rad_WPline2UAV);
    state_UAV_phi = wrap_PI(ahrs.yaw_sensor*0.01*M_PI/180.0 - rad_prevWP2currWP);
    state_UAV_GCRS = wrap_PI(gps.ground_course_cd()*0.01*M_PI/180.0 - rad_prevWP2currWP);
    if(TLAB_Control_flag != 0)return 0;
//    float Va = 6.4;
    v_g = gps.ground_speed();
    if(v_g > Vg_max){
        Vg_limited = Vg_max;
    }else if(v_g < Vg_min){
        Vg_limited = Vg_min;
    }else{
        Vg_limited = v_g;
    }
    //added by Nishiduka 2018/4/19
    if(g.TPARAM_switch_mo == 1){
    u_star = Vg_limited*calc_controller(state_UAV_y,state_UAV_GCRS);
    alpha=wrap_PI(state_UAV_phi-state_UAV_GCRS);
    if(alpha > alpha_max){
            alpha = alpha_max;
        }else if(alpha < alpha_min){
            alpha = alpha_min;
        }
    L_conv = Vg_limited/(v_a*cosf(alpha));
    }else if(g.TPARAM_switch_mo == 2){
    //added by Nishiduka 2018/4/19
    //y_set = g.TPARAM_y_set;
    //a_bottom = atanf(y_set);
    //a_above = g.TPARAM_control_miu * M_PI /2;
    //ログ表示
    //det_a = pow(a_above,2)/pow(a_bottom,2);
    sinc_kai = sinc(state_UAV_GCRS);
    //ここまで
    det_a=g.TPARAM_control_a;
    det_b=g.TPARAM_control_b;
    det_p=g.TPARAM_control_p;
    //u_star = -(((pow(det_a,2)*det_b*v_g)/(1+pow(det_b,2)*pow(state_UAV_y,2)))*atanf(det_b*state_UAV_y)*sinc(state_UAV_GCRS)+det_p*state_UAV_GCRS);
    u_star = -det_b*(state_UAV_GCRS+atanf(det_a*state_UAV_y)) - (det_a*v_g*sinf(state_UAV_GCRS))/(1+pow(det_a,2)*pow(state_UAV_y,2));
    alpha=wrap_PI(state_UAV_phi-state_UAV_GCRS);
    if(alpha > alpha_max){
            alpha = alpha_max;
        }else if(alpha < alpha_min){
            alpha = alpha_min;
        }
    L_conv = Vg_limited/(v_a*cosf(alpha));
    }else if(g.TPARAM_switch_mo == 3){
        //u_star = Vg_limited*calc_controller(state_UAV_y,state_UAV_phi);
        det_a=g.TPARAM_control_a;
        det_b=g.TPARAM_control_b;
        det_p=g.TPARAM_control_p;
        u_star = -(1/det_b)*(v_g*state_UAV_y+det_a*state_UAV_GCRS);
        alpha=wrap_PI(state_UAV_phi-state_UAV_GCRS);
        if(alpha > alpha_max){
                    alpha = alpha_max;
                }else if(alpha < alpha_min){
                    alpha = alpha_min;
                }
        L_conv = Vg_limited/(v_a*cosf(alpha));
    }else if(g.TPARAM_switch_mo == 4){
        //sinc_kai = sinc(state_UAV_GCRS);
            L_1=g.TPARAM_L_1;
            if (state_UAV_y<L_1){
                eta=constrain_float(-state_UAV_GCRS-atanf(state_UAV_y/sqrtf(pow(L_1,2)-pow(state_UAV_y,2))),-M_PI/2,M_PI/2);
            }else if (state_UAV_y>=L_1){
                eta=constrain_float(-state_UAV_GCRS-M_PI/2*sqn(state_UAV_y),-M_PI/2,M_PI/2);
            }
            //u_star = -(((pow(det_a,2)*det_b*v_g)/(1+pow(det_b,2)*pow(state_UAV_y,2)))*atanf(det_b*state_UAV_y)*sinc(state_UAV_phi)+det_p*state_UAV_phi);
            u_star=2*v_a*sinf(eta)/L_1;
            alpha=wrap_PI(state_UAV_phi-state_UAV_GCRS);
            if(alpha > alpha_max){
                        alpha = alpha_max;
                    }else if(alpha < alpha_min){
                        alpha = alpha_min;
                    }
         L_conv = Vg_limited/(v_a*cosf(alpha));
    }
    //alpha = wrap_PI(state_UAV_phi-state_UAV_GCRS);
    //if(alpha > alpha_max){
    //    alpha = alpha_max;
    //}else if(alpha < alpha_min){
    //    alpha = alpha_min;
    //}
    u = constrain_float(L_conv/const_k*u_star,U_min*M_PI/180.0,U_max*M_PI/180.0) + g.TPARAM_servo_neutral*M_PI/180.0;
    servo = static_cast<int32_t>(asinf(constrain_float(58.0f / 29.0f * sinf(u), -1.0f, 1.0f))*100.0f*180.0f/M_PI);
    return servo;
}
int32_t Plane::TLAB_Circle_Trace_Controller(void)
{
    arg_r = get_distance(Target_Circle_Center,current_loc);
    switch(calc_GCRS_flag){
    case 0:
        theta = wrap_PI(static_cast<float>(get_bearing_cd(Target_Circle_Center,current_loc))*0.01f*M_PI/180.0f);
        chi = wrap_PI(static_cast<float>(gps.ground_course_cd())*0.01*M_PI/180.0);
        break;
    default:
        if (init_TLAB_Controller_AUTO_flag){
            init_TLAB_Controller_AUTO();
            diff_theta = 0;
            Int_theta = 0;
        }else{
            chi = wrap_PI(static_cast<float>(get_bearing_cd(prev_POS,current_loc))*0.01f*M_PI/180.0f);
            mid_POS.lat = static_cast<int32_t>(prev_POS.lat + (current_loc.lat - prev_POS.lat)/2);
            mid_POS.lng = static_cast<int32_t>(prev_POS.lng + (current_loc.lng - prev_POS.lng)/2);
            theta = wrap_PI(static_cast<float>(get_bearing_cd(Target_Circle_Center,mid_POS))*0.01f*M_PI/180.0f);
            diff_theta = wrap_PI(theta - prev_theta);
            Int_theta += diff_theta;
        }
        prev_theta = theta;

//        mid_POS.lat = static_cast<int32_t>(prev_POS.lat + (current_loc.lat - prev_POS.lat)/2);
//        mid_POS.lng = static_cast<int32_t>(prev_POS.lng + (current_loc.lng - prev_POS.lng)/2);
//        theta = wrap_PI(static_cast<float>(get_bearing_cd(Target_Circle_Center,mid_POS))*0.01f*M_PI/180.0f);
        break;
    }
    switch(TLAB_Control_flag){ //
    case 1: //
        chi_r = wrap_PI(theta - M_PI*0.5);
        e_r = arg_r - target_R;
        break;
    default: //
        chi_r = wrap_PI(theta + M_PI*0.5);
        e_r = target_R - arg_r;
        break;
    }
    e_chi = wrap_PI(chi - chi_r);
    v_g = gps.ground_speed();

    if(v_g > Vg_max){
        Vg_limited = Vg_max;
    }else if(v_g < Vg_min){
        Vg_limited = Vg_min;
    }else{
        Vg_limited = v_g;
    }
    //以下3行はprev_locationの更新である。これがないとおかしなことになる。
    if(prev_POS.lat != current_loc.lat || prev_POS.lng != current_loc.lng){
            prev_POS = current_loc;
        }
    /*
    if(g.TPARAM_switch_mo == 1){
    u_star = Vg_limited*calc_controller(e_r,e_chi);
    }else{
    //sinc_kai = sinc(e_chi);
    det_a=g.TPARAM_control_a;
    det_b=g.TPARAM_control_b;
    det_p=g.TPARAM_control_p;
    u_star = -(((pow(det_a,2)*det_b*v_g)/(1+pow(det_b,2)*pow(e_r,2)))*atanf(det_b*e_r)*sinc(e_chi)+det_p*e_chi);
    }
*/
    phi = wrap_PI(static_cast<float>(ahrs.yaw_sensor)*0.01f*M_PI/180.0f);
    if(g.TPARAM_switch_mo == 1){
        u_star = Vg_limited*calc_controller(e_r,chi_r);
        alpha = wrap_PI(phi-chi);
            if(alpha > alpha_max){
                alpha = alpha_max;
            }else if(alpha < alpha_min){
                alpha = alpha_min;
            }
            L_conv = Vg_limited/(v_a*cosf(alpha));
    }else if(g.TPARAM_switch_mo == 2){
        //added by Nishiduka 2018/4/19
        //y_set = g.TPARAM_y_set;
        //a_bottom = atanf(y_set);
        //a_above = g.TPARAM_control_miu * M_PI /2;
        //ログ表示
        //det_a = pow(a_above,2)/pow(a_bottom,2);
        //sinc_kai = sinc(state_UAV_GCRS);
        //ここまで
        det_a=g.TPARAM_control_a;
        det_b=g.TPARAM_control_b;
        det_p=g.TPARAM_control_p;
        //u_star = -(((pow(det_a,2)*det_b*v_g)/(1+pow(det_b,2)*pow(state_UAV_y,2)))*atanf(det_b*state_UAV_y)*sinc(state_UAV_GCRS)+det_p*state_UAV_GCRS);
        u_star = -det_b*(e_chi + atanf(det_a*e_r)) - (det_a*v_g*sinf(e_chi))/(1+pow(det_a,2)*pow(e_r,2));
        alpha = wrap_PI(phi-chi);
            if(alpha > alpha_max){
                alpha = alpha_max;
            }else if(alpha < alpha_min){
                alpha = alpha_min;
            }
            L_conv = Vg_limited/(v_a*cosf(alpha));
        }else if(g.TPARAM_switch_mo == 3){
            //u_star = Vg_limited*calc_controller(state_UAV_y,state_UAV_phi);
            det_a=g.TPARAM_control_a;
            det_b=g.TPARAM_control_b;
            det_p=g.TPARAM_control_p;
            u_star = -(1/det_b)*(v_g*e_r+det_a*e_chi);
            //alpha=wrap_PI(state_UAV_phi-state_UAV_GCRS);
            alpha = wrap_PI(phi-chi);
                if(alpha > alpha_max){
                    alpha = alpha_max;
                }else if(alpha < alpha_min){
                    alpha = alpha_min;
                }
                L_conv = Vg_limited/(v_a*cosf(alpha));
        }else if(g.TPARAM_switch_mo == 4){
            L_1=g.TPARAM_L_1;
             if (e_r<L_1){
                eta=constrain_float(-e_chi-atanf(e_r/sqrtf(pow(L_1,2)-pow(e_r,2))),-M_PI/2,M_PI/2);
             }else if (e_r>=L_1){
                eta=constrain_float(-e_chi-M_PI/2*sqn(e_r),-M_PI/2,M_PI/2);
             }
            //u_star = -(((pow(det_a,2)*det_b*v_g)/(1+pow(det_b,2)*pow(state_UAV_y,2)))*atanf(det_b*state_UAV_y)*sinc(state_UAV_phi)+det_p*state_UAV_phi);
             u_star=2*v_a*sinf(eta)/L_1;
             alpha = wrap_PI(phi-chi);
             if(alpha > alpha_max){
                      alpha = alpha_max;
             }else if(alpha < alpha_min){
                      alpha = alpha_min;
             }
               L_conv = Vg_limited/(v_a*cosf(alpha));
    }
    /*
    phi = wrap_PI(static_cast<float>(ahrs.yaw_sensor)*0.01f*M_PI/180.0f);
    alpha = wrap_PI(phi-chi);
    if(alpha > alpha_max){
        alpha = alpha_max;
    }else if(alpha < alpha_min){
        alpha = alpha_min;
    }
    L_conv = Vg_limited/(v_a*cosf(alpha));
    */
    (arg_r < r_min)?(r_limited = r_min):(r_limited = arg_r);

    switch(TLAB_Control_flag){ //
    case 1: //
        u = L_conv/const_k*u_star - Vg_limited*L_conv/(r_limited*const_k)*cosf(e_chi);
        break;
    default: //
        u = L_conv/const_k*u_star + Vg_limited*L_conv/(r_limited*const_k)*cosf(e_chi);
        break;
    }
    u = constrain_float(u,U_min*M_PI/180.0,U_max*M_PI/180.0) + g.TPARAM_servo_neutral*M_PI/180.0;
    servo = static_cast<int32_t>(asinf(constrain_float(58.0f / 29.0f * sinf(u), -1, 1))*100.0f*180.0f/M_PI);
    return servo;
}

int32_t Plane::TLAB_Combine_Controller(void)
{
    if (!TLAB_WP_nav_flag){
        TLAB_Control_flag = 0;
        return TLAB_Line_Trace_Controller();
    }//TLAB_WP_nav_flagが真でないとき上のif文実行(L638より組み合わせのときはfalseになる)

    if(TLAB_Control_flag == 0){ //直線追従モードの時
        if(change_to_circle_flag){
            change_to_circle_flag = false;
            Target_Circle_Center = prev_WP_loc;
            init_TLAB_Controller_AUTO_flag = true; //次円追従モードになった時にthetaとか初期化するため
            if (alternate_orbit_flag){
                TLAB_Control_flag = static_cast<int8_t>((TLAB_CMD_index % 2) + 1);//余りを計算して左右1,2を切り替える
            }else{
                (g.TPARAM_control_mode == 1)?(TLAB_Control_flag = 1):(TLAB_Control_flag = 2); //確実に円追従モードに切り替える,alternate=0で確実に左右どちらか一方
            }
            gcs_send_text_fmt(MAV_SEVERITY_INFO, "change to circle trace mode");
            return TLAB_Circle_Trace_Controller();
        }else{
            return TLAB_Line_Trace_Controller();
        }
    }else{ //円追従モードの時
        Target_Circle_Center = prev_WP_loc;
        int32_t u_kari = TLAB_Line_Trace_Controller(); //WP座標系でのxとyを計算するため
        if((fabsf(Int_theta) > 2*M_PI*orbit_num) && (state_UAV_x > 0.0f) && (fabsf(state_UAV_y) < change_y)){
            //thetaの積分が(2パイ)x(何週(orbit))を超えたら直線に切り替え超えていなければそのまま円追従
            TLAB_Control_flag = 0;
            Int_theta = 0;
            gcs_send_text_fmt(MAV_SEVERITY_INFO, "change to line trace mode");
            return TLAB_Line_Trace_Controller();
        }else{
            return TLAB_Circle_Trace_Controller();
        }
    }
}


/* ##### Added by Kaito Yamamoto 2021.08.11. #####
 * 概要: 一定のサーボモーター角度を出力する
*/
int32_t Plane::TLAB_Constant_Output(void){
	// 変数の初期設定
	if (!yet_init) {
		servo = 0;
		yet_init = true;
	}
	servo = static_cast<int32_t>(g.TPARAM_servo_neutral*100);  // [cdeg]
	return servo;
}


/* ##### Added by Kaito Yamamoto 2021.08.04. #####
 * 概要: Plane::TLAB_2D_Trace_Controller(void)内の変数の初期化
*/
void Plane::init_TLAB_2D_Trace_Controller(void){
	// UAVに固有な定数
	v_a = g.TPARAM_v_a;  // 対気速度の大きさ [m/s]: const
	k_prop_const = g.TPARAM_k_prop_const;  // コントロールバー角度 [rad]と旋回速度 [rad/s]の比例定数(proportional constant) [1/s]
	// xy座標の原点位置をHP(オートモード開始地点)に設定(int32_t): 緯度,経度 [1e-7*deg]
    Path_Origin.lat = prev_WP_loc.lat;
    Path_Origin.lng = prev_WP_loc.lng;
    // フライトプランの設定
    Flight_Plan = g.TPARAM_Flight_Plan;
	// フィードバックゲイン  Fx[3], Fchi[4][3]
	Fx[0] = g.TPARAM_Fx1;
	Fx[1] = g.TPARAM_Fx2;
	Fx[2] = g.TPARAM_Fx3;
	Fchi[0][0] = g.TPARAM_Fchi1_1;
	Fchi[0][1] = g.TPARAM_Fchi1_2;
	Fchi[0][2] = g.TPARAM_Fchi1_3;
	Fchi[1][0] = g.TPARAM_Fchi2_1;
	Fchi[1][1] = g.TPARAM_Fchi2_2;
	Fchi[1][2] = g.TPARAM_Fchi2_3;
	Fchi[2][0] = g.TPARAM_Fchi3_1;
	Fchi[2][1] = g.TPARAM_Fchi3_2;
	Fchi[2][2] = g.TPARAM_Fchi3_3;
	Fchi[3][0] = g.TPARAM_Fchi4_1;
	Fchi[3][1] = g.TPARAM_Fchi4_2;
	Fchi[3][2] = g.TPARAM_Fchi4_3;
	// ファジィ化範囲(const)
	v_g_min = g.TPARAM_v_g_min;  // default: 3 [m/s]
	v_g_max = g.TPARAM_v_g_max;  // default: 10 [m/s]
	kappa_max = g.TPARAM_kappa_max;
	kappa_min = g.TPARAM_kappa_min;
	u_x_max = g.TPARAM_u_x_max;
	chiF_max = g.TPARAM_chiF_max_deg*M_PI/180.f;  // default: 178/180*PI [rad]
	// 非線形項の最大・最小値
	z1_max = (v_g_max + u_x_max)*kappa_max;
	z1_min = (v_g_max + u_x_max)*kappa_min;
	z2_max = v_g_max;
	z2_min = v_g_max*sinf(chiF_max)/chiF_max;
	// 初期値
	Path_Mode = 0;  // 目標経路の設定
	s = 0;  // 経路長 [m] >= 0
	zeta = 0;  // 媒介変数 >= 0
	P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> x,y) [m]
	P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> x,y) [m]
	dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新
	i_now_CMD = 0;
	u_x = 0;
	t_now = AP_HAL::micros64();  // 現在の時刻 [us]
}


/* ##### Added by Kaito Yamamoto 2021.07.11. #####
 * "目標経路の生成"
 * 概要: 目標点 P の慣性座標位置や目標航路角を現在の経路長 s から生成する
*/
void Plane::TLAB_generate_2D_Path(void){
	uint16_t i_prev_CMD = i_now_CMD;
	i_now_CMD = TLAB_CMD_index;  // CMDインデックスの更新
	float zeta_prev = zeta;

	// Commented out 2021.08.26.
	/*
	if (s < 0) {
		s = 0;
	}
	*/

	float Px, Py;  // 構造体 Vector2f のx, yを置換するために使用する
	Location WP0;  // 目標経路の初期位置を設定するためのローカル変数
	// フライトプランを指定する
	switch (Flight_Plan) {
	// Mode 0: HP -(直線)-> WP1, WP0 -(直線)-> WP2 -...
	// 指定WP -- N個 (何個でも指定可能.ただし,HPはAUTOモードに切り替えた地点が割り当てられる.)
	case 0:
		Path_Mode = 0;
		// AUTOモードに切り替わり,WPがセットされたとき
		if (zeta < 0.1 && i_now_CMD != i_prev_CMD) {
			// 直線経路を更新(初回なので定義)する
			dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新
			P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
			Px = P0.y;
			Py = P0.x;
			P0.x = Px;
			P0.y = Py;
			P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
			Px = P1.y;
			Py = P1.x;
			P1.x = Px;
			P1.y = Py;
		}
		// WP半径内に到達し,WPが新たにセットされたとき
		else if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
			// 次に媒介変数zetaが1に達したときに,P0
			change_path_flag = true;
		}
		// WP半径内に到達したあとで,zetaが1に到達したとき
		if (change_path_flag == true && zeta >= 1) {
			// 直線経路の切り替え: s, zeta の初期化
			s = 0;
			zeta = 0;
			change_path_flag = false;
			// 直線経路の更新
			if (i_now_CMD == 2) {
				// 検証用 目標経路の初期位置 緯度・経度の設定
				WP0.lat = g.TPARAM_Path_Origin_lat;  // 目標経路の初期位置(WP0) 緯度 [1e-7*deg]
				WP0.lng = g.TPARAM_Path_Origin_lng;  // 目標経路の初期位置(WP0) 経度 [1e-7*deg]
				dist_WPs = get_distance(WP0, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, WP0);  // P0をMPで設定した位置にセット: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
			}
			else {
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
			}
			P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
			Px = P1.y;
			Py = P1.x;
			P1.x = Px;
			P1.y = Py;
		}
		break;
	// Mode 1: WP0 -(直線)-> WP1 -(円経路・右旋回)-> WP2
	// 指定WP -- N個 (何個でも指定可能.ただし,HPはAUTOモードに切り替えた地点が割り当てられる.)
	case 1:
		if (Path_Mode == 0) {
			// AUTOモードに切り替わり,WPがセットされたとき
			if (zeta < 0.1 && i_now_CMD != i_prev_CMD) {
				Path_Mode = 0;  // 直線経路モードにセット
				// 直線経路を更新(初回なので定義)する
				// 目標経路の初期位置 緯度・経度の設定
				WP0.lat = g.TPARAM_Path_Origin_lat;  // 目標経路の初期位置(WP0) 緯度 [1e-7*deg]
				WP0.lng = g.TPARAM_Path_Origin_lng;  // 目標経路の初期位置(WP0) 経度 [1e-7*deg]
				dist_WPs = get_distance(WP0, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, WP0);  // P0をMPで設定した位置にセット: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				//P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				//Px = P0.y;
				//Py = P0.x;
				//P0.x = Px;
				//P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
			}
			// WP半径内に到達し,WPが新たにセットされたとき
			else if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaが1に到達したとき
			if (change_path_flag == true && zeta >= 1) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				Path_Mode = 5;  // 円経路 右旋回モードにセット
			}
		}

		if (Path_Mode == 5) {
			// do nothing
			// 経路の切り替えはしない  -> 円経路を旋回し続ける
		}
		break;
	// Mode 2: HP -(直線)-> WP1 -(直線)-> WP2 -> -(リサージュ曲線 8の字 2周) -> WP3 -(直線)-> WP4 ...
	// 指定WP -- N個 (何個でも指定可能.ただし,HPはAUTOモードに切り替えた地点が割り当てられる.)
	case 2:
		if (Path_Mode == 0) {
			// AUTOモードに切り替わり,WPがセットされたとき
			if (zeta < 0.1 && i_now_CMD != i_prev_CMD) {
				// 直線経路を更新(初回なので定義)する
				// 目標経路の初期位置 緯度・経度の設定
				//WP0.lat = g.TPARAM_Path_Origin_lat;  // 目標経路の初期位置(WP0) 緯度 [1e-7*deg]
				//WP0.lng = g.TPARAM_Path_Origin_lng;  // 目標経路の初期位置(WP0) 経度 [1e-7*deg]
				//dist_WPs = get_distance(WP0, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				//P0 = location_diff(Path_Origin, WP0);  // P0をMPで設定した位置にセット: 目標経路中心からの変位(N.E <-> y,x) [m]
				//Px = P0.y;
				//Py = P0.x;
				//P0.x = Px;
				//P0.y = Py;
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
			}
			// WP半径内に到達し,WPが新たにセットされたとき
			else if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaが1に到達したとき
			if (change_path_flag == true && zeta >= 1) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				if (i_now_CMD == 3) {
					Path_Mode = 6;  // リサージュ曲線（8の字）経路モードにセット
				}
			}
		}
		if (Path_Mode == 6) {
			// WP半径内に到達し,WPが新たにセットされたとき
			if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaが4PIに到達したとき
			if (change_path_flag == true && zeta >= 4*M_PI) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				Path_Mode = 0;  // 直線経路モードにセット
			}
		}
		break;
	// Mode 3: HP -(直線)-> WP1 -(直線)-> WP2 -> -(リサージュ曲線 電通大マーク) -> WP3 -(直線)-> WP4 ...
	// 指定WP -- N個 (何個でも指定可能.ただし,HPはAUTOモードに切り替えた地点が割り当てられる.)
	case 3:
		if (Path_Mode == 0) {
			// AUTOモードに切り替わり,WPがセットされたとき
			if (zeta < 0.1 && i_now_CMD != i_prev_CMD) {
				// 直線経路を更新(初回なので定義)する
				// 目標経路の初期位置 緯度・経度の設定
				//WP0.lat = g.TPARAM_Path_Origin_lat;  // 目標経路の初期位置(WP0) 緯度 [1e-7*deg]
				//WP0.lng = g.TPARAM_Path_Origin_lng;  // 目標経路の初期位置(WP0) 経度 [1e-7*deg]
				//dist_WPs = get_distance(WP0, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				//P0 = location_diff(Path_Origin, WP0);  // P0をMPで設定した位置にセット: 目標経路中心からの変位(N.E <-> y,x) [m]
				//Px = P0.y;
				//Py = P0.x;
				//P0.x = Px;
				//P0.y = Py;
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
			}
			// WP半径内に到達し,WPが新たにセットされたとき
			else if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaが1に到達したとき
			if (change_path_flag == true && zeta >= 1) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				if (i_now_CMD == 3) {
					P1.x -= g.TPARAM_r;
					P1.y -= g.TPARAM_r;
					Path_Mode = 3;  // リサージュ曲線（電通大マーク）経路モードにセット
				}
			}
		}
		if (Path_Mode == 3) {
			// WP半径内に到達し,WPが新たにセットされたとき
			if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaがPIに到達したとき
			if (change_path_flag == true && zeta >= M_PI) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				Path_Mode = 0;  // 直線経路モードにセット
			}
		}
		break;
	// Mode 4: WP0 -(直線)-> WP1 -(円経路・左旋回)-> WP2
	// 指定WP -- N個 (何個でも指定可能.ただし,HPはAUTOモードに切り替えた地点が割り当てられる.)
	case 4:
		if (Path_Mode == 0) {
			// AUTOモードに切り替わり,WPがセットされたとき
			if (zeta < 0.1 && i_now_CMD != i_prev_CMD) {
				Path_Mode = 0;  // 直線経路モードにセット
				// 直線経路を更新(初回なので定義)する
				// 目標経路の初期位置 緯度・経度の設定
				WP0.lat = g.TPARAM_Path_Origin_lat;  // 目標経路の初期位置(WP0) 緯度 [1e-7*deg]
				WP0.lng = g.TPARAM_Path_Origin_lng;  // 目標経路の初期位置(WP0) 経度 [1e-7*deg]
				dist_WPs = get_distance(WP0, next_WP_loc);  // 2つのWP間の距離 [m]を更新
				P0 = location_diff(Path_Origin, WP0);  // P0をMPで設定した位置にセット: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				//P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				//Px = P0.y;
				//Py = P0.x;
				//P0.x = Px;
				//P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
			}
			// WP半径内に到達し,WPが新たにセットされたとき
			else if (zeta >= 0.1 && i_now_CMD != i_prev_CMD) {
				// 次に媒介変数zetaが1に達したときに,P0
				change_path_flag = true;
			}
			// WP半径内に到達したあとで,zetaが1に到達したとき
			if (change_path_flag == true && zeta >= 1) {
				// 目標経路の切り替え: s, zeta の初期化
				s = 0;
				zeta_prev = 0;  // zeta 初期化の瞬間に dot_zetaが大きくなるのを防ぐため
				change_path_flag = false;
				// P0, P1の更新
				dist_WPs = get_distance(prev_WP_loc, next_WP_loc);  // 2つのWP間の距離 [m]を更新 ---使わない
				P0 = location_diff(Path_Origin, prev_WP_loc);  // P0を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P0.y;
				Py = P0.x;
				P0.x = Px;
				P0.y = Py;
				P1 = location_diff(Path_Origin, next_WP_loc);  // P1を更新: 目標経路中心からの変位(N.E <-> y,x) [m]
				Px = P1.y;
				Py = P1.x;
				P1.x = Px;
				P1.y = Py;
				Path_Mode = 4;  // 円経路 左旋回モードにセット
			}
		}
		if (Path_Mode == 4) {
			// do nothing
			// 経路の切り替えはしない  -> 円経路を旋回し続ける
		}
		break;
	}  // switch(Flight_Plan)文の終わり

	float dPdzeta = 0;
	// 経路の種類を指定する
	switch (Path_Mode) {
	// Mode 0: 2点のWP(P0, P1)で定義される直線経路
	case 0:
		zeta = s/dist_WPs;  // 媒介変数の更新 (s の関数)
		dot_zeta = (zeta - zeta_prev)/dt;  // 使わない
		x_d = (1 - zeta)*P0.x + zeta*P1.x;  // [m] x,y が実際とは逆を意味していることに注意
		y_d = (1 - zeta)*P0.y + zeta*P1.y;  // [m]
		chi_d = - atan2f((P1.y - P0.y), (P1.x - P0.x));  // [rad]: (-PI ~ PI)
		dot_chi_d = 0;
		kappa = 0;
		break;
	// Mode 1: 2点のWP(P0, P1)で定義される円経路(左旋回)
	case 1:
		zeta = s/dist_WPs*2;
		dot_zeta = (zeta - zeta_prev)/dt;  // 使わない
		x_d = dist_WPs/2*cosf(zeta) + (P0.x + P1.x)/2;
		y_d = dist_WPs/2*sinf(zeta) + (P0.y + P1.y)/2;
		chi_d = atan2f(cosf(zeta), sinf(zeta));
		dot_chi_d = - dot_zeta;
		kappa = 2/dist_WPs;
		break;
	// Mode 2: 2点のWP(P0, P1)で定義される円経路(右旋回)
	case 2:
		zeta = s/dist_WPs*2;
		dot_zeta = (zeta - zeta_prev)/dt;  // 使わない
		x_d = dist_WPs/2*cosf(-zeta) + (P0.x + P1.x)/2;
		y_d = dist_WPs/2*sinf(-zeta) + (P0.y + P1.y)/2;
		chi_d = atan2f(cosf(zeta), -sinf(zeta));
		dot_chi_d = dot_zeta;
		kappa = 2/dist_WPs;
		break;
	// Mode 3: 1点のWP(P1)と半径rで定義されるリサージュ曲線経路(電通大マークver.) : 左下が始点
	case 3:
		// s と dPdzeta から数値計算でzetaを求める
		while (1) {
			dPdzeta = g.TPARAM_r*sqrt((25*powf(sinf(5*g.TPARAM_dzeta*i_zeta), 2) + 36*pow(sinf(6*g.TPARAM_dzeta*i_zeta), 2)));
			s_calc += dPdzeta*g.TPARAM_dzeta;
			if (s_calc >= s) {
				zeta = i_zeta*g.TPARAM_dzeta;
				break;
			}
			i_zeta ++;
		}
		dot_zeta = (zeta - zeta_prev)/dt;
		x_d = - g.TPARAM_r*cosf(5.f*zeta) + P1.x;
		y_d = g.TPARAM_r*cosf(6.f*zeta) + P1.y;
		chi_d = atan2f(6.f/5.f*sinf(6.f*zeta), sinf(5.f*zeta));
		dot_chi_d = - 30*dot_zeta*(sinf(11*zeta) - 11*sinf(zeta))/(25*cosf(10*zeta) + 36*cosf(12*zeta) - 61.f);
		kappa = (15*fabsf(11.f*sinf(zeta) - sinf(11*zeta))) / (g.TPARAM_r*powf((25*powf(sinf(5*zeta) , 2) + 36*powf(sinf(6*zeta) , 2)) , 1.5f));
		break;
	// Mode 4: 1点のWP(P1)と半径rで定義される円経路(左旋回): 初期位相 +PI
	case 4:
		zeta = s/g.TPARAM_r;
		dot_zeta = (zeta - zeta_prev)/dt;
		x_d = - g.TPARAM_r*cosf(zeta) + P1.x;
		y_d = - g.TPARAM_r*sinf(zeta) + P1.y;
		chi_d = atan2f(cosf(zeta), sinf(zeta));
		dot_chi_d = - dot_zeta;
		kappa = 1.f/g.TPARAM_r;
		break;
	// Mode 5: 1点のWP(P1)と半径rで定義される円経路(右旋回): 初期位相 +PI
	case 5:
		zeta = s/g.TPARAM_r;
		dot_zeta = (zeta - zeta_prev)/dt;
		x_d = - g.TPARAM_r*cosf(zeta) + P1.x;
		y_d = g.TPARAM_r*sinf(zeta) + P1.y;
		chi_d = atan2f(-cosf(zeta), sinf(zeta));
		dot_chi_d = dot_zeta;
		kappa = 1.f/g.TPARAM_r;
		break;
	// Mode 6: 1点のWP(P1)と半径rで定義されるリサージュ曲線経路(8の字ver.) : 初期ベクトルは右上に向かって
	case 6:
		// s と dPdzeta から数値計算でzetaを求める
		while (1) {
			dPdzeta = g.TPARAM_r*sqrt((25*powf(sinf(5*g.TPARAM_dzeta*i_zeta), 2) + 36*pow(sinf(6*g.TPARAM_dzeta*i_zeta), 2)));
			s_calc += dPdzeta*g.TPARAM_dzeta;
			if (s_calc >= s) {
				zeta = i_zeta*g.TPARAM_dzeta;
				break;
			}
			i_zeta ++;
		}
		dot_zeta = (zeta - zeta_prev)/dt;
		x_d = 2*g.TPARAM_r*sinf(zeta) + P1.x;
		y_d = g.TPARAM_r*sinf(2*zeta) + P1.y;
		chi_d = atan2f(- cosf(2*zeta), cosf(zeta));
		dot_chi_d = - (dot_zeta*sinf(zeta)*(2*powf(sinf(zeta), 2) - 3))/(4*powf(sinf(zeta), 4) - 5*powf(sinf(zeta), 2) + 2);
		kappa = - (fabsf(sinf(zeta))*(2*powf(sinf(zeta) , 2) - 3))/(2*g.TPARAM_r*powf((4*powf(cosf(zeta) , 4) - 3*powf(cosf(zeta) , 2) + 1) , 1.5f));
		break;
	default:
		zeta = 0;
		break;
	}  // switch(Path_Mode)文の終わり
}


/* ##### Added by Kaito Yamamoto 2021.08.04. #####
 * "PPG　2次元経路追従コントローラ"
 * 参考文献: 高橋裕徳さんの修論(2016), ミーティング資料2021/05/16 etc.
 * 概要: コントロールバーのサーボモータ角度 [cdeg.]を計算して返す
 * 制御目的: 設定した2次元経路への追従（セレ・フレネ座標系におけるレギュレータとして設計）
*/
int32_t Plane::TLAB_2D_Trace_Controller(void){
	// 変数の初期設定
	if (!yet_init) {
		init_TLAB_2D_Trace_Controller();
		yet_init = true;
		return 0;  // dt = 0 による0割りを防ぐため
	}

	// 慣性座標系における現在の状態の更新(IMU + GPS + Barometer)
	uint64_t t_prev = t_now;  // 直前の時刻 [us]
	t_now = AP_HAL::micros64();  // 現在の時刻 [us]
	dt = (t_now - t_prev)*1.e-6f;  // サンプリング時間間隔 [s]
	Vector2f xyI = location_diff(Path_Origin, current_loc);  // 現在地点: 目標経路中心からの変位(N.E <-> y,x) [m]
	xI = xyI.y;  // 慣性x座標(緯度方向) [m]
	yI = xyI.x;  // 慣性y座標(経度方向) [m]
	psi = wrap_2PI(ahrs.yaw - M_PI/2);  // ヨー角(機首方位角) [rad] (0 ~ 2PI)
	chi = wrap_2PI(gps.ground_course()*M_PI/180.0f - M_PI/2);  // 航路角 [rad]: (0 ~ 2PI)
	v_g = gps.ground_speed();  // 対地速度の大きさ [m/s]

	// 経路生成: 経路情報(目標点 P の慣性座標 etc.)を現在の経路長 s から計算する
	TLAB_generate_2D_Path();

	// 慣性座標系{I}からセレ・フレネ座標系{F}への変換
	float e_xI = x_d - xI;  // 慣性座標 x の制御偏差 [m]
	float e_yI = y_d - yI;  // 慣性座標 y の制御偏差 [m]
	xF = - cosf(chi_d)*e_xI + sinf(chi_d)*e_yI;  // [m]
	yF = - sinf(chi_d)*e_xI - cosf(chi_d)*e_yI;  // [m]
	chiF = wrap_PI(chi_d - chi);  // [rad]: (-PI ~ PI)
	float X[3] = {xF, yF, chiF};  // 状態変数ベクトル

	// 制御入力 u_x の計算
	u_x_calc = 0;  // 計算用 u_x の初期化
	for (int i = 0; i < 3; i++) {
		u_x_calc -= Fx[i]*X[i];
	}
	/*
	// u_x のファジィ化範囲上限 u_x_max で入力値をカット
	if (u_x_calc > u_x_max) {
		u_x = u_x_max;
	}
	else if (u_x_calc < - u_x_max) {
		u_x = - u_x_max;
	}
	else {
		u_x = u_x_calc;
	}
	*/
	u_x = u_x_calc;

	// メンバーシップ関数の計算
	// 非線形項
	float z1 = (v_g*cosf(chiF) + u_x)*kappa;  // 係数行列の非線形項 1
	float z2 = v_g*sinf(chiF)/chiF;  // 係数行列の非線形項2
	// K1, K2, M1, M2 の計算
	if (chiF == 0) {
		// 0割り対策
		K1 = 1;
		K2 = 0;
	}
	else {
		K1 = (z2 - z2_min)/(z2_max - z2_min);
		K2 = (z2_max - z2)/(z2_max - z2_min);
	}
	M1 = (z1 - z1_min)/(z1_max - z1_min);  // 分母は定数なので0割りは起こり得ない
	M2 = (z1_max - z1)/(z1_max - z1_min);
	// K1, K2, M1, M2 の上限・下限カット
	if (K1 > 1) {
		K1 = 1;
		K2 = 0;
	}
	else if (K1 < 0) {
		K1 = 0;
		K2 = 1;
	}
	if (M1 > 1) {
		M1 = 1;
		M2 = 0;
	}
	else if (M1 < 0) {
		M1 = 0;
		M2 = 1;
	}
	// メンバーシップ関数  h_chi[1] ~ h_chi[4] の計算
	h_chi[0] = K1*M1;
	h_chi[1] = K2*M1;
	h_chi[2] = K1*M2;
	h_chi[3] = K2*M2;

	// 制御入力 u_chi の計算
	float u_chi_calc = 0;  // 計算用
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 3; j++) {
			u_chi_calc -= h_chi[i]*Fchi[i][j]*X[j];
		}
	}
	u_chi = u_chi_calc;

	// 経路長  s の更新(数値積分)
	ds = u_x + v_g*cosf(chiF);  // 目標点 P の移動速度 [m/s]
	s += ds*dt;  // 経路長の更新式 [m]

	// u_chi [rad/s] をコントロールバー角度 bar_angle, サーボモーター角度 servo [cdeg] へ変換
    d_angle = static_cast<int32_t>(1/k_prop_const*v_g/v_a/cosf(chi - psi)*(-u_chi + dot_chi_d)*100.0f*180.0f/M_PI);  // [cdeg]
    bar_angle = d_angle + g.TPARAM_servo_neutral*100;  // コントロールバー角度 [cdeg]
    // Changed by Kaito Yamamoto 2021.08.11.
    u = bar_angle/100.f*M_PI/180.f;  // [cdeg] --> [rad]
    servo = static_cast<int32_t>(asinf(constrain_float(58.0f / 29.0f * sinf(u), -1, 1))*100.0f*180.0f/M_PI);  // サーボモーター角度 [cdeg]
    return servo;
}


/*
  calculate yaw control for ground steering with specific course
 */
void Plane::calc_nav_yaw_course(void)
{
    // holding a specific navigation course on the ground. Used in
    // auto-takeoff and landing
    int32_t bearing_error_cd = nav_controller->bearing_error_cd();
    steering_control.steering = steerController.get_steering_out_angle_error(bearing_error_cd);
    if (stick_mixing_enabled()) {
        stick_mix_channel(channel_rudder, steering_control.steering);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}

/*
  calculate yaw control for ground steering
 */
void Plane::calc_nav_yaw_ground(void)
{
    if (gps.ground_speed() < 1 && 
        channel_throttle->get_control_in() == 0 &&
        flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
        flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        // manual rudder control while still
        steer_state.locked_course = false;
        steer_state.locked_course_err = 0;
        steering_control.steering = rudder_input;
        return;
    }

    float steer_rate = (rudder_input/4500.0f) * g.ground_steer_dps;
    if (flight_stage == AP_SpdHgtControl::FLIGHT_TAKEOFF ||
        flight_stage == AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        steer_rate = 0;
    }
    if (!is_zero(steer_rate)) {
        // pilot is giving rudder input
        steer_state.locked_course = false;        
    } else if (!steer_state.locked_course) {
        // pilot has released the rudder stick or we are still - lock the course
        steer_state.locked_course = true;
        if (flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
            flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
            steer_state.locked_course_err = 0;
        }
    }
    if (!steer_state.locked_course) {
        // use a rate controller at the pilot specified rate
        steering_control.steering = steerController.get_steering_out_rate(steer_rate);
    } else {
        // use a error controller on the summed error
        int32_t yaw_error_cd = -ToDeg(steer_state.locked_course_err)*100;
        steering_control.steering = steerController.get_steering_out_angle_error(yaw_error_cd);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}


/*
  calculate a new nav_pitch_cd from the speed height controller
 */
void Plane::calc_nav_pitch()
{
    // Calculate the Pitch of the plane
    // --------------------------------
    int32_t commanded_pitch = SpdHgt_Controller->get_pitch_demand();

    // Received an external msg that guides roll in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_rpy_ms.y > 0 &&
            millis() - plane.guided_state.last_forced_rpy_ms.y < 3000) {
        commanded_pitch = plane.guided_state.forced_rpy_cd.y;
    }

    nav_pitch_cd = constrain_int32(commanded_pitch, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}


/*
  calculate a new nav_roll_cd from the navigation controller
 */
void Plane::calc_nav_roll()
{
    int32_t commanded_roll = nav_controller->nav_roll_cd();

    // Received an external msg that guides roll in the last 3 seconds?
    if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
            plane.guided_state.last_forced_rpy_ms.x > 0 &&
            millis() - plane.guided_state.last_forced_rpy_ms.x < 3000) {
        commanded_roll = plane.guided_state.forced_rpy_cd.x;
    }

    nav_roll_cd = constrain_int32(commanded_roll, -roll_limit_cd, roll_limit_cd);
    update_load_factor();
}


/*****************************************
* Throttle slew limit
*****************************************/
void Plane::throttle_slew_limit(int16_t last_throttle)
{
    uint8_t slewrate = aparm.throttle_slewrate;
    if (control_mode==AUTO) {
        if (auto_state.takeoff_complete == false && g.takeoff_throttle_slewrate != 0) {
            slewrate = g.takeoff_throttle_slewrate;
        } else if (g.land_throttle_slewrate != 0 &&
                (flight_stage == AP_SpdHgtControl::FLIGHT_LAND_APPROACH || flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL || flight_stage == AP_SpdHgtControl::FLIGHT_LAND_PREFLARE)) {
            slewrate = g.land_throttle_slewrate;
        }
    }
    // if slew limit rate is set to zero then do not slew limit
    if (slewrate) {                   
        // limit throttle change by the given percentage per second
        float temp = slewrate * G_Dt * 0.01f * fabsf(channel_throttle->get_radio_max() - channel_throttle->get_radio_min());
        // allow a minimum change of 1 PWM per cycle
        if (temp < 1) {
            temp = 1;
        }
        channel_throttle->set_radio_out(constrain_int16(channel_throttle->get_radio_out(), last_throttle - temp, last_throttle + temp));
    }
}

/*****************************************
Flap slew limit
*****************************************/
void Plane::flap_slew_limit(int8_t &last_value, int8_t &new_value)
{
    uint8_t slewrate = g.flap_slewrate;
    // if slew limit rate is set to zero then do not slew limit
    if (slewrate) {                   
        // limit flap change by the given percentage per second
        float temp = slewrate * G_Dt;
        // allow a minimum change of 1% per cycle. This means the
        // slowest flaps we can do is full change over 2 seconds
        if (temp < 1) {
            temp = 1;
        }
        new_value = constrain_int16(new_value, last_value - temp, last_value + temp);
    }
    last_value = new_value;
}

/* We want to suppress the throttle if we think we are on the ground and in an autopilot controlled throttle mode.

   Disable throttle if following conditions are met:
   *       1 - We are in Circle mode (which we use for short term failsafe), or in FBW-B or higher
   *       AND
   *       2 - Our reported altitude is within 10 meters of the home altitude.
   *       3 - Our reported speed is under 5 meters per second.
   *       4 - We are not performing a takeoff in Auto mode or takeoff speed/accel not yet reached
   *       OR
   *       5 - Home location is not set
*/
bool Plane::suppress_throttle(void)
{
#if PARACHUTE == ENABLED
    if (auto_throttle_mode && parachute.release_initiated()) {
        // throttle always suppressed in auto-throttle modes after parachute release initiated
        throttle_suppressed = true;
        return true;
    }
#endif

    if (!throttle_suppressed) {
        // we've previously met a condition for unsupressing the throttle
        return false;
    }
    if (!auto_throttle_mode) {
        // the user controls the throttle
        throttle_suppressed = false;
        return false;
    }

    if (control_mode==AUTO && g.auto_fbw_steer == 42) {
        // user has throttle control
        return false;
    }

    bool gps_movement = (gps.status() >= AP_GPS::GPS_OK_FIX_2D && gps.ground_speed() >= 5);
    
    if (control_mode==AUTO && 
        auto_state.takeoff_complete == false) {

        uint32_t launch_duration_ms = ((int32_t)g.takeoff_throttle_delay)*100 + 2000;
        if (is_flying() &&
            millis() - started_flying_ms > MAX(launch_duration_ms, 5000U) && // been flying >5s in any mode
            adjusted_relative_altitude_cm() > 500 && // are >5m above AGL/home
            labs(ahrs.pitch_sensor) < 3000 && // not high pitch, which happens when held before launch
            gps_movement) { // definite gps movement
            // we're already flying, do not suppress the throttle. We can get
            // stuck in this condition if we reset a mission and cmd 1 is takeoff
            // but we're currently flying around below the takeoff altitude
            throttle_suppressed = false;
            return false;
        }
        if (auto_takeoff_check()) {
            // we're in auto takeoff 
            throttle_suppressed = false;
            auto_state.baro_takeoff_alt = barometer.get_altitude();
            return false;
        }
        // keep throttle suppressed
        return true;
    }
    
    if (relative_altitude_abs_cm() >= 1000) {
        // we're more than 10m from the home altitude
        throttle_suppressed = false;
        return false;
    }

    if (gps_movement) {
        // if we have an airspeed sensor, then check it too, and
        // require 5m/s. This prevents throttle up due to spiky GPS
        // groundspeed with bad GPS reception
        if ((!ahrs.airspeed_sensor_enabled()) || airspeed.get_airspeed() >= 5) {
            // we're moving at more than 5 m/s
            throttle_suppressed = false;
            return false;        
        }
    }

    if (quadplane.is_flying()) {
        throttle_suppressed = false;
    }

    // throttle remains suppressed
    return true;
}

/*
  implement a software VTail or elevon mixer. There are 4 different mixing modes
 */
void Plane::channel_output_mixer(uint8_t mixing_type, int16_t & chan1_out, int16_t & chan2_out)const
{
    int16_t c1, c2;
    int16_t v1, v2;

    // first get desired elevator and rudder as -500..500 values
    c1 = chan1_out - 1500;
    c2 = chan2_out - 1500;

    // apply MIXING_OFFSET to input channels using long-integer version
    //  of formula:  x = x * (g.mixing_offset/100.0 + 1.0)
    //  -100 => 2x on 'c1', 100 => 2x on 'c2'
    if (g.mixing_offset < 0) {
        c1 = (int16_t)(((int32_t)c1) * (-g.mixing_offset+100) / 100);
    } else if (g.mixing_offset > 0) {
        c2 = (int16_t)(((int32_t)c2) * (g.mixing_offset+100) / 100);
    }

    v1 = (c1 - c2) * g.mixing_gain;
    v2 = (c1 + c2) * g.mixing_gain;

    // now map to mixed output
    switch (mixing_type) {
    case MIXING_DISABLED:
        return;

    case MIXING_UPUP:
        break;

    case MIXING_UPDN:
        v2 = -v2;
        break;

    case MIXING_DNUP:
        v1 = -v1;
        break;

    case MIXING_DNDN:
        v1 = -v1;
        v2 = -v2;
        break;
    }

    // scale for a 1500 center and 900..2100 range, symmetric
    v1 = constrain_int16(v1, -600, 600);
    v2 = constrain_int16(v2, -600, 600);

    chan1_out = 1500 + v1;
    chan2_out = 1500 + v2;
}

void Plane::channel_output_mixer(uint8_t mixing_type, RC_Channel* chan1, RC_Channel* chan2)const
{
   int16_t ch1 = chan1->get_radio_out();
   int16_t ch2 = chan2->get_radio_out();

   channel_output_mixer(mixing_type,ch1,ch2);

   chan1->set_radio_out(ch1);
   chan2->set_radio_out(ch2);
}

/*
  setup flaperon output channels
 */
void Plane::flaperon_update(int8_t flap_percent)
{
    if (!RC_Channel_aux::function_assigned(RC_Channel_aux::k_flaperon1) ||
        !RC_Channel_aux::function_assigned(RC_Channel_aux::k_flaperon2)) {
        return;
    }
    int16_t ch1, ch2;
    /*
      flaperons are implemented as a mixer between aileron and a
      percentage of flaps. Flap input can come from a manual channel
      or from auto flaps.

      Use k_flaperon1 and k_flaperon2 channel trims to center servos.
      Then adjust aileron trim for level flight (note that aileron trim is affected
      by mixing gain). flapin_channel's trim is not used.
     */
     
    ch1 = channel_roll->get_radio_out();
    // The *5 is to take a percentage to a value from -500 to 500 for the mixer
    ch2 = 1500 - flap_percent * 5;
    channel_output_mixer(g.flaperon_output, ch1, ch2);
    RC_Channel_aux::set_radio_trimmed(RC_Channel_aux::k_flaperon1, ch1);
    RC_Channel_aux::set_radio_trimmed(RC_Channel_aux::k_flaperon2, ch2);
}

/*
  setup servos for idle mode
  Idle mode is used during balloon launch to keep servos still, apart
  from occasional wiggle to prevent freezing up
 */
void Plane::set_servos_idle(void)
{
    RC_Channel_aux::output_ch_all();
    if (auto_state.idle_wiggle_stage == 0) {
        RC_Channel::output_trim_all();
        return;
    }
    int16_t servo_value = 0;
    // move over full range for 2 seconds
    auto_state.idle_wiggle_stage += 2;
    if (auto_state.idle_wiggle_stage < 50) {
        servo_value = auto_state.idle_wiggle_stage * (4500 / 50);
    } else if (auto_state.idle_wiggle_stage < 100) {
        servo_value = (100 - auto_state.idle_wiggle_stage) * (4500 / 50);        
    } else if (auto_state.idle_wiggle_stage < 150) {
        servo_value = (100 - auto_state.idle_wiggle_stage) * (4500 / 50);        
    } else if (auto_state.idle_wiggle_stage < 200) {
        servo_value = (auto_state.idle_wiggle_stage-200) * (4500 / 50);        
    } else {
        auto_state.idle_wiggle_stage = 0;
    }
    channel_roll->set_servo_out(servo_value);
    channel_pitch->set_servo_out(servo_value);
    channel_rudder->set_servo_out(servo_value);
    channel_roll->calc_pwm();
    channel_pitch->calc_pwm();
    channel_rudder->calc_pwm();
    channel_roll->output();
    channel_pitch->output();
    channel_throttle->output();
    channel_rudder->output();
    channel_throttle->output_trim();
}

/*
  return minimum throttle PWM value, taking account of throttle reversal. For reverse thrust you get the throttle off position
 */
uint16_t Plane::throttle_min(void) const
{
    if (aparm.throttle_min < 0) {
        return channel_throttle->get_radio_trim();
    }
    return channel_throttle->get_reverse() ? channel_throttle->get_radio_max() : channel_throttle->get_radio_min();
};


/*****************************************
* Set the flight control servos based on the current calculated values
*****************************************/
void Plane::set_servos(void)
{
    // this is to allow the failsafe module to deliberately crash 
    // the plane. Only used in extreme circumstances to meet the
    // OBC rules
    if (afs.should_crash_vehicle()) {
        afs.terminate_vehicle();
        return;
    }

    int16_t last_throttle = channel_throttle->get_radio_out();

    // do any transition updates for quadplane
    quadplane.update();    

    // comment outed by iwase 2017/08/05
//    if (control_mode == AUTO && auto_state.idle_mode) {
//        // special handling for balloon launch
//        set_servos_idle();
//        return;
//    }

    /*
      see if we are doing ground steering.
     */
    if (!steering_control.ground_steering) {
        // we are not at an altitude for ground steering. Set the nose
        // wheel to the rudder just in case the barometer has drifted
        // a lot
        steering_control.steering = steering_control.rudder;
    } else if (!RC_Channel_aux::function_assigned(RC_Channel_aux::k_steering)) {
        // we are within the ground steering altitude but don't have a
        // dedicated steering channel. Set the rudder to the ground
        // steering output
        steering_control.rudder = steering_control.steering;
    }
    channel_rudder->set_servo_out(steering_control.rudder);

    // clear ground_steering to ensure manual control if the yaw stabilizer doesn't run
    steering_control.ground_steering = false;

    RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_rudder, steering_control.rudder);
    RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_steering, steering_control.steering);

    if (control_mode == MANUAL) {
        // do a direct pass through of radio values
        if (g.mix_mode == 0 || g.elevon_output != MIXING_DISABLED) {
            channel_roll->set_radio_out(channel_roll->get_radio_in());
            channel_pitch->set_radio_out(channel_pitch->get_radio_in());
        } else {
            channel_roll->set_radio_out(channel_roll->read());
            channel_pitch->set_radio_out(channel_pitch->read());
        }
        channel_throttle->set_radio_out(channel_throttle->get_radio_in());
        channel_rudder->set_radio_out(channel_rudder->get_radio_in());

        // setup extra channels. We want this to come from the
        // main input channel, but using the 2nd channels dead
        // zone, reverse and min/max settings. We need to use
        // pwm_to_angle_dz() to ensure we don't trim the value for the
        // deadzone of the main aileron channel, otherwise the 2nd
        // aileron won't quite follow the first one
        RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_aileron, channel_roll->pwm_to_angle_dz(0));
        RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_elevator, channel_pitch->pwm_to_angle_dz(0));

        // this variant assumes you have the corresponding
        // input channel setup in your transmitter for manual control
        // of the 2nd aileron
        RC_Channel_aux::copy_radio_in_out(RC_Channel_aux::k_aileron_with_input);
        RC_Channel_aux::copy_radio_in_out(RC_Channel_aux::k_elevator_with_input);

    } else {
        if (g.mix_mode == 0) {
            // both types of secondary aileron are slaved to the roll servo out
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_aileron, channel_roll->get_servo_out());
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_aileron_with_input, channel_roll->get_servo_out());

            // both types of secondary elevator are slaved to the pitch servo out
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_elevator, channel_pitch->get_servo_out());
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_elevator_with_input, channel_pitch->get_servo_out());
        }else{
            /*Elevon mode*/
            float ch1;
            float ch2;
            ch1 = channel_pitch->get_servo_out() - (BOOL_TO_SIGN(g.reverse_elevons) * channel_roll->get_servo_out());
            ch2 = channel_pitch->get_servo_out() + (BOOL_TO_SIGN(g.reverse_elevons) * channel_roll->get_servo_out());

			/* Differential Spoilers
               If differential spoilers are setup, then we translate
               rudder control into splitting of the two ailerons on
               the side of the aircraft where we want to induce
               additional drag.
             */
			if (RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler1) && RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler2)) {
				float ch3 = ch1;
				float ch4 = ch2;
				if ( BOOL_TO_SIGN(g.reverse_elevons) * channel_rudder->get_servo_out() < 0) {
				    ch1 += abs(channel_rudder->get_servo_out());
				    ch3 -= abs(channel_rudder->get_servo_out());
				} else {
					ch2 += abs(channel_rudder->get_servo_out());
				    ch4 -= abs(channel_rudder->get_servo_out());
				}
				RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_dspoiler1, ch3);
				RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_dspoiler2, ch4);
			}

            // directly set the radio_out values for elevon mode
            channel_roll->set_radio_out(elevon.trim1 + (BOOL_TO_SIGN(g.reverse_ch1_elevon) * (ch1 * 500.0f/ SERVO_MAX)));
            channel_pitch->set_radio_out(elevon.trim2 + (BOOL_TO_SIGN(g.reverse_ch2_elevon) * (ch2 * 500.0f/ SERVO_MAX)));
        }

        // push out the PWM values
        if (g.mix_mode == 0) {
            channel_roll->calc_pwm();
            channel_pitch->calc_pwm();
        }
        channel_rudder->calc_pwm();

#if THROTTLE_OUT == 0
        channel_throttle->set_servo_out(0);
#else
        // convert 0 to 100% (or -100 to +100) into PWM
        int8_t min_throttle = aparm.throttle_min.get();
        int8_t max_throttle = aparm.throttle_max.get();

        if (min_throttle < 0 && !allow_reverse_thrust()) {
           // reverse thrust is available but inhibited.
           min_throttle = 0;
        }

        if (control_mode == AUTO) {
            if (flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) {
                min_throttle = 0;
            }

            if (flight_stage == AP_SpdHgtControl::FLIGHT_TAKEOFF || flight_stage == AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
                if(aparm.takeoff_throttle_max != 0) {
                    max_throttle = aparm.takeoff_throttle_max;
                } else {
                    max_throttle = aparm.throttle_max;
                }
            }
        }

        uint32_t now = millis();
        if (battery.overpower_detected()) {
            // overpower detected, cut back on the throttle if we're maxing it out by calculating a limiter value
            // throttle limit will attack by 10% per second

            if (channel_throttle->get_servo_out() > 0 && // demanding too much positive thrust
                throttle_watt_limit_max < max_throttle - 25 &&
                now - throttle_watt_limit_timer_ms >= 1) {
                // always allow for 25% throttle available regardless of battery status
                throttle_watt_limit_timer_ms = now;
                throttle_watt_limit_max++;

            } else if (channel_throttle->get_servo_out() < 0 &&
                min_throttle < 0 && // reverse thrust is available
                throttle_watt_limit_min < -(min_throttle) - 25 &&
                now - throttle_watt_limit_timer_ms >= 1) {
                // always allow for 25% throttle available regardless of battery status
                throttle_watt_limit_timer_ms = now;
                throttle_watt_limit_min++;
            }

        } else if (now - throttle_watt_limit_timer_ms >= 1000) {
            // it has been 1 second since last over-current, check if we can resume higher throttle.
            // this throttle release is needed to allow raising the max_throttle as the battery voltage drains down
            // throttle limit will release by 1% per second
            if (channel_throttle->get_servo_out() > throttle_watt_limit_max && // demanding max forward thrust
                throttle_watt_limit_max > 0) { // and we're currently limiting it
                throttle_watt_limit_timer_ms = now;
                throttle_watt_limit_max--;

            } else if (channel_throttle->get_servo_out() < throttle_watt_limit_min && // demanding max negative thrust
                throttle_watt_limit_min > 0) { // and we're limiting it
                throttle_watt_limit_timer_ms = now;
                throttle_watt_limit_min--;
            }
        }

        max_throttle = constrain_int16(max_throttle, 0, max_throttle - throttle_watt_limit_max);
        if (min_throttle < 0) {
            min_throttle = constrain_int16(min_throttle, min_throttle + throttle_watt_limit_min, 0);
        }

        channel_throttle->set_servo_out(constrain_int16(channel_throttle->get_servo_out(), 
                                                      min_throttle,
                                                      max_throttle));

        //comment outed by iwase 17/08/05
//        if (!hal.util->get_soft_armed()) {
//            channel_throttle->set_servo_out(0);
//            channel_throttle->calc_pwm();
//        } else if (suppress_throttle()) {
//            // throttle is suppressed in auto mode
//            channel_throttle->set_servo_out(0);
//            if (g.throttle_suppress_manual) {
//                // manual pass through of throttle while throttle is suppressed
//                channel_throttle->set_radio_out(channel_throttle->get_radio_in());
//            } else {
//                channel_throttle->calc_pwm();
//            }
//        } else if (g.throttle_passthru_stabilize &&
//                   (control_mode == STABILIZE ||
//                    control_mode == TRAINING ||
//                    control_mode == ACRO ||
//                    control_mode == FLY_BY_WIRE_A ||
//                    control_mode == AUTOTUNE) &&
//                   !failsafe.ch3_counter) {
//            // manual pass through of throttle while in FBWA or
//            // STABILIZE mode with THR_PASS_STAB set
//            channel_throttle->set_radio_out(channel_throttle->get_radio_in());
//        } else if ((control_mode == GUIDED || control_mode == AVOID_ADSB) &&
//                   guided_throttle_passthru) {
//            // manual pass through of throttle while in GUIDED
//            channel_throttle->set_radio_out(channel_throttle->get_radio_in());
//        } else if (quadplane.in_vtol_mode()) {
//            // ask quadplane code for forward throttle
//            channel_throttle->set_servo_out(quadplane.forward_throttle_pct());
//            channel_throttle->calc_pwm();
//        } else {
//            // normal throttle calculation based on servo_out
//            channel_throttle->calc_pwm();
//        }
        channel_throttle->calc_pwm();
#endif
    }

    // Auto flap deployment
    int8_t auto_flap_percent = 0;
    int8_t manual_flap_percent = 0;
    static int8_t last_auto_flap;
    static int8_t last_manual_flap;

    // work out any manual flap input
    RC_Channel *flapin = RC_Channel::rc_channel(g.flapin_channel-1);
    if (flapin != NULL && !failsafe.ch3_failsafe && failsafe.ch3_counter == 0) {
        flapin->input();
        manual_flap_percent = flapin->percent_input();
    }

    if (auto_throttle_mode) {
        int16_t flapSpeedSource = 0;
        if (ahrs.airspeed_sensor_enabled()) {
            flapSpeedSource = target_airspeed_cm * 0.01f;
        } else {
            flapSpeedSource = aparm.throttle_cruise;
        }
        if (g.flap_2_speed != 0 && flapSpeedSource <= g.flap_2_speed) {
            auto_flap_percent = g.flap_2_percent;
        } else if ( g.flap_1_speed != 0 && flapSpeedSource <= g.flap_1_speed) {
            auto_flap_percent = g.flap_1_percent;
        } //else flaps stay at default zero deflection

        /*
          special flap levels for takeoff and landing. This works
          better than speed based flaps as it leads to less
          possibility of oscillation
         */
        if (control_mode == AUTO) {
            switch (flight_stage) {
            case AP_SpdHgtControl::FLIGHT_TAKEOFF:
            case AP_SpdHgtControl::FLIGHT_LAND_ABORT:
                if (g.takeoff_flap_percent != 0) {
                    auto_flap_percent = g.takeoff_flap_percent;
                }
                break;
            case AP_SpdHgtControl::FLIGHT_NORMAL:
                if (auto_flap_percent != 0 && in_preLaunch_flight_stage()) {
                    // TODO: move this to a new FLIGHT_PRE_TAKEOFF stage
                    auto_flap_percent = g.takeoff_flap_percent;
                }
                break;
            case AP_SpdHgtControl::FLIGHT_LAND_APPROACH:
            case AP_SpdHgtControl::FLIGHT_LAND_PREFLARE:
            case AP_SpdHgtControl::FLIGHT_LAND_FINAL:
                if (g.land_flap_percent != 0) {
                    auto_flap_percent = g.land_flap_percent;
                }
                break;
            default:
                break;
            }
        }
    }

    // manual flap input overrides auto flap input
    if (abs(manual_flap_percent) > auto_flap_percent) {
        auto_flap_percent = manual_flap_percent;
    }

    flap_slew_limit(last_auto_flap, auto_flap_percent);
    flap_slew_limit(last_manual_flap, manual_flap_percent);

    RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_flap_auto, auto_flap_percent);
    RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_flap, manual_flap_percent);

    if (control_mode >= FLY_BY_WIRE_B ||
        quadplane.in_assisted_flight() ||
        quadplane.in_vtol_mode()) {
        /* only do throttle slew limiting in modes where throttle
         *  control is automatic */
        throttle_slew_limit(last_throttle);
    }

    if (control_mode == TRAINING) {
        // copy rudder in training mode
        channel_rudder->set_radio_out(channel_rudder->get_radio_in());
    }

    if (g.flaperon_output != MIXING_DISABLED && g.elevon_output == MIXING_DISABLED && g.mix_mode == 0) {
        flaperon_update(auto_flap_percent);
    }
    if (g.vtail_output != MIXING_DISABLED) {
        channel_output_mixer(g.vtail_output, channel_pitch, channel_rudder);
    } else if (g.elevon_output != MIXING_DISABLED) {
        channel_output_mixer(g.elevon_output, channel_pitch, channel_roll);
        // if (both) differential spoilers setup then apply rudder
        //  control into splitting the two elevons on the side of
        //  the aircraft where we want to induce additional drag:
        if (RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler1) &&
            RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler2)) {
            int16_t ch3 = channel_roll->get_radio_out();    //diff spoiler 1
            int16_t ch4 = channel_pitch->get_radio_out();   //diff spoiler 2
            // convert rudder-servo output (-4500 to 4500) to PWM offset
            //  value (-500 to 500) and multiply by DSPOILR_RUD_RATE/100
            //  (rudder->servo_out * 500 / SERVO_MAX * dspoiler_rud_rate/100):
            int16_t ruddVal = (int16_t)((int32_t)(channel_rudder->get_servo_out()) *
                                        g.dspoiler_rud_rate / (SERVO_MAX/5));
            if (ruddVal != 0) {   //if nonzero rudder then apply to spoilers
                int16_t ch1 = ch3;          //elevon 1
                int16_t ch2 = ch4;          //elevon 2
                if (ruddVal > 0) {     //apply rudder to right or left side
                    ch1 += ruddVal;
                    ch3 -= ruddVal;
                } else {
                    ch2 += ruddVal;
                    ch4 -= ruddVal;
                }
                // change elevon 1 & 2 positions; constrain min/max:
                channel_roll->set_radio_out(constrain_int16(ch1, 900, 2100));
                channel_pitch->set_radio_out(constrain_int16(ch2, 900, 2100));
                // constrain min/max for intermediate dspoiler positions:
                ch3 = constrain_int16(ch3, 900, 2100);
                ch4 = constrain_int16(ch4, 900, 2100);
            }
            // set positions of differential spoilers (convert PWM
            //  900-2100 range to servo output (-4500 to 4500)
            //  and use function that supports rev/min/max/trim):
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_dspoiler1,
                                              (ch3-(int16_t)1500) * (int16_t)(SERVO_MAX/300) / (int16_t)2);
            RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_dspoiler2,
                                              (ch4-(int16_t)1500) * (int16_t)(SERVO_MAX/300) / (int16_t)2);
        }
    }

    if (!arming.is_armed()) {
        //Some ESCs get noisy (beep error msgs) if PWM == 0.
        //This little segment aims to avoid this.
        switch (arming.arming_required()) { 
        case AP_Arming::NO:
            //keep existing behavior: do nothing to radio_out
            //(don't disarm throttle channel even if AP_Arming class is)
            break;

        case AP_Arming::YES_ZERO_PWM:
            channel_throttle->set_servo_out(0);
            channel_throttle->set_radio_out(0);
            break;

        case AP_Arming::YES_MIN_PWM:
        default:
            channel_throttle->set_servo_out(0);
            channel_throttle->set_radio_out(throttle_min());
            break;
        }
    }

#if HIL_SUPPORT
    if (g.hil_mode == 1) {
        // get the servos to the GCS immediately for HIL
        if (HAVE_PAYLOAD_SPACE(MAVLINK_COMM_0, RC_CHANNELS_SCALED)) {
            send_servo_out(MAVLINK_COMM_0);
        }
        if (!g.hil_servos) {
            return;
        }
    }
#endif

    if (g.land_then_servos_neutral > 0 &&
            control_mode == AUTO &&
            g.land_disarm_delay > 0 &&
            auto_state.land_complete &&
            !arming.is_armed()) {
        // after an auto land and auto disarm, set the servos to be neutral just
        // in case we're upside down or some crazy angle and straining the servos.
        if (g.land_then_servos_neutral == 1) {
            channel_roll->set_radio_out(channel_roll->get_radio_trim());
            channel_pitch->set_radio_out(channel_pitch->get_radio_trim());
            channel_rudder->set_radio_out(channel_rudder->get_radio_trim());
        } else if (g.land_then_servos_neutral == 2) {
            channel_roll->disable_out();
            channel_pitch->disable_out();
            channel_rudder->disable_out();
        }
    }

    uint8_t override_pct;
    if (g2.ice_control.throttle_override(override_pct)) {
        // the ICE controller wants to override the throttle for starting
        channel_throttle->set_servo_out(override_pct);
        channel_throttle->calc_pwm();
    }

    // allow for secondary throttle
    RC_Channel_aux::set_servo_out_for(RC_Channel_aux::k_throttle, channel_throttle->get_servo_out());
    
    // send values to the PWM timers for output
    // ----------------------------------------
    if (g.rudder_only == 0) {
        // when we RUDDER_ONLY mode we don't send the channel_roll
        // output and instead rely on KFF_RDDRMIX. That allows the yaw
        // damper to operate.
        channel_roll->output();
    }
    channel_pitch->output();
    channel_throttle->output();
    channel_rudder->output();
    RC_Channel_aux::output_ch_all();
}

bool Plane::allow_reverse_thrust(void)
{
    // check if we should allow reverse thrust
    bool allow = false;

    if (g.use_reverse_thrust == USE_REVERSE_THRUST_NEVER) {
        return false;
    }

    switch (control_mode) {
    case AUTO:
        {
        uint16_t nav_cmd = mission.get_current_nav_cmd().id;

        // never allow reverse thrust during takeoff
        if (nav_cmd == MAV_CMD_NAV_TAKEOFF) {
            return false;
        }

        // always allow regardless of mission item
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_ALWAYS);

        // landing
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LAND_APPROACH) &&
                (nav_cmd == MAV_CMD_NAV_LAND);

        // LOITER_TO_ALT
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_TO_ALT) &&
                (nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT);

        // any Loiter (including LOITER_TO_ALT)
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_ALL) &&
                    (nav_cmd == MAV_CMD_NAV_LOITER_TIME ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TURNS ||
                     nav_cmd == MAV_CMD_NAV_LOITER_UNLIM);

        // waypoints
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_WAYPOINT) &&
                    (nav_cmd == MAV_CMD_NAV_WAYPOINT ||
                     nav_cmd == MAV_CMD_NAV_SPLINE_WAYPOINT);
        }
        break;

    case LOITER:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_LOITER);
        break;
    case RTL:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_RTL);
        break;
    case CIRCLE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CIRCLE);
        break;
    case CRUISE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CRUISE);
        break;
    case FLY_BY_WIRE_B:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_FBWB);
        break;
    case AVOID_ADSB:
    case GUIDED:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_GUIDED);
        break;
    default:
        // all other control_modes are auto_throttle_mode=false.
        // If we are not controlling throttle, don't limit it.
        allow = true;
        break;
    }

    return allow;
}

/*
  adjust nav_pitch_cd for STAB_PITCH_DOWN_CD. This is used to make
  keeping up good airspeed in FBWA mode easier, as the plane will
  automatically pitch down a little when at low throttle. It makes
  FBWA landings without stalling much easier.
 */
void Plane::adjust_nav_pitch_throttle(void)
{
    int8_t throttle = throttle_percentage();
    if (throttle >= 0 && throttle < aparm.throttle_cruise && flight_stage != AP_SpdHgtControl::FLIGHT_VTOL) {
        float p = (aparm.throttle_cruise - throttle) / (float)aparm.throttle_cruise;
        nav_pitch_cd -= g.stab_pitch_down * 100.0f * p;
    }
}


/*
  calculate a new aerodynamic_load_factor and limit nav_roll_cd to
  ensure that the load factor does not take us below the sustainable
  airspeed
 */
void Plane::update_load_factor(void)
{
    float demanded_roll = fabsf(nav_roll_cd*0.01f);
    if (demanded_roll > 85) {
        // limit to 85 degrees to prevent numerical errors
        demanded_roll = 85;
    }
    aerodynamic_load_factor = 1.0f / safe_sqrt(cosf(radians(demanded_roll)));

    if (!aparm.stall_prevention) {
        // stall prevention is disabled
        return;
    }
    if (fly_inverted()) {
        // no roll limits when inverted
        return;
    }

    float max_load_factor = smoothed_airspeed / aparm.airspeed_min;
    if (max_load_factor <= 1) {
        // our airspeed is below the minimum airspeed. Limit roll to
        // 25 degrees
        nav_roll_cd = constrain_int32(nav_roll_cd, -2500, 2500);
        roll_limit_cd = constrain_int32(roll_limit_cd, -2500, 2500);
    } else if (max_load_factor < aerodynamic_load_factor) {
        // the demanded nav_roll would take us past the aerodymamic
        // load limit. Limit our roll to a bank angle that will keep
        // the load within what the airframe can handle. We always
        // allow at least 25 degrees of roll however, to ensure the
        // aircraft can be maneuvered with a bad airspeed estimate. At
        // 25 degrees the load factor is 1.1 (10%)
        int32_t roll_limit = degrees(acosf(sq(1.0f / max_load_factor)))*100;
        if (roll_limit < 2500) {
            roll_limit = 2500;
        }
        nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit, roll_limit);
        roll_limit_cd = constrain_int32(roll_limit_cd, -roll_limit, roll_limit);
    }    
}
