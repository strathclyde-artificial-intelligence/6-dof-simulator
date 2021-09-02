#ifndef __DRONESTATE_H__
#define __DRONESTATE_H__

#include "../Helpers/magnetic_field_lookup.h"
#include "../Logging/ConsoleLogger.h"
#include <cmath>
#include <boost/chrono.hpp>
#include <Eigen/Eigen>
#include <mavlink.h>
#include <EquationsOfMotion/rotationMatrix.h>

// #define HIL_STATE_QUATERNION_VERBOSE
// #define HIL_SENSOR_VERBOSE
// #define HIL_GPS_VERBOSE

class DroneStateEncoder {
protected:

#define K_Pb 101325.0  // static pressure at sea level [Pa]
#define K_Tb 288.15    // standard temperature at sea level [K]
#define K_Lb -0.0065   // standard temperature lapse rate [K/m]
#define K_M 0.0289644  // molar mass of Earth's air [kg/mol]
#define K_G 9.80665    // gravity
#define K_R 8.31432    // universal gas constant

    // Ripped from JMavSim
    /**
     * Convert altitude to barometric pressure
     * @param alt        Altitude in meters
     * @return Barometric pressure in Pa
     */
    static double alt_to_baro(double alt) {
        if (alt <= 11000.0) {
            return K_Pb * std::pow(K_Tb / (K_Tb + (K_Lb * alt)), (K_G * K_M) / (K_R * K_Lb));
        } else if (alt <= 20000.0) {
            double f = 11000.0;
            double a = alt_to_baro(f);
            double c = K_Tb + (f * K_Lb);
            return a * std::pow(M_E, ((-K_G) * K_M * (alt - f)) / (K_R * c));
        }
        return 0.0;
    }

    static void euler_to_quaterions(const float* euler_rpy, float* quaternion) {
        float roll = euler_rpy[0];
        float pitch = euler_rpy[1];
        float yaw = euler_rpy[2];
        quaternion[0] = sin(roll/2) * cos(pitch/2) * cos(yaw/2) - cos(roll/2) * sin(pitch/2) * sin(yaw/2);
        quaternion[1] = cos(roll/2) * sin(pitch/2) * cos(yaw/2) + sin(roll/2) * cos(pitch/2) * sin(yaw/2);
        quaternion[2] = cos(roll/2) * cos(pitch/2) * sin(yaw/2) - sin(roll/2) * sin(pitch/2) * cos(yaw/2);
        quaternion[3] = cos(roll/2) * cos(pitch/2) * cos(yaw/2) + sin(roll/2) * sin(pitch/2) * sin(yaw/2);
    }
    
    void get_attitude(float* attitude) { // <float(4)>
        Eigen::VectorXd& state = this->get_vector_state();
        float euler[3] = {0};
        for (uint i = 0; i < 3; i++) euler[i] = state[6+i];
        DroneStateEncoder::euler_to_quaterions((const float*)euler, attitude);
    } 

    void get_rpy_speed(float* rpy) { // <float(3)>
        Eigen::VectorXd state = this->get_vector_state();
        for (uint i = 0; i < 3; i++) *(rpy+i) = state[i + 9];
    } 
    
    /**
     * Ground speed (lat. , lon. , alt.) in cm/s
     */
    void get_ground_speed(int16_t* x_y_z) { // <int16_t(3)>
        Eigen::VectorXd state_derivative = this->get_vector_dx_state();
        for (uint i = 0; i < 3; i++) x_y_z[i] = state_derivative[i];
        x_y_z[0] *= 100;
        x_y_z[1] *= 100;
        x_y_z[2] *= 100;
    } 
    
    /**
     * Body frame (NED) acceleration (ẍ , ÿ , z̈) in mG
     */
    void get_body_frame_acceleration(float* x_y_z) { // <float(3)>
#define G_FORCE 9.81f
        Eigen::VectorXd state_derivative = this->get_vector_dx_state();
        for (uint i = 0; i < 3; i++) *(x_y_z+i) = state_derivative[3 + i];
        if (x_y_z[2] < .0001f && x_y_z[2] > -.0001f) { // FIX FOR FAKE GROUND IN 6DOF
            x_y_z[2] = (float)-G_FORCE;
        }
    } 

    /**
     * Body frame origin (x,y,z) in NED with respect to earth frame
     */
    void get_body_frame_origin(float* x_y_z) { // <float(3)>
        Eigen::VectorXd state = this->get_vector_state();
        for (uint i = 0; i < 3; i++) *(x_y_z+i) = state[i];
    }
    
    void get_earth_fixed_velocity(int16_t* xyz) { //<int16_t(3)>
        
        Eigen::VectorXd state = this->get_vector_state();
        Eigen::VectorXd xyz_dot_body_frame = state.segment(3,3);

        Eigen::MatrixXd body_to_earth_rot = caelus_fdm::body2earth(state);
        Eigen::VectorXd earth_frame_velocity = body_to_earth_rot * xyz_dot_body_frame;

        for (uint i = 0; i < 3; i++) *(xyz+i) = earth_frame_velocity[i];
    }   

    /**
     * lat: [degE7]
     * lon: [degE7]
     * alt: [mm]
     */
    void get_lat_lon_alt(int32_t* lat_lon_alt) {  // <int32_t(3)>
// UK Grid origin
#define INITIAL_LAT 49.766809
#define INITIAL_LON -7.5571598
        Eigen::VectorXd state = this->get_vector_state();
        double d_lat_lon_alt[3] = {0};
        caelus_fdm::convertState2LlA(INITIAL_LAT, INITIAL_LON, state, d_lat_lon_alt[0], d_lat_lon_alt[1], d_lat_lon_alt[2]);
        lat_lon_alt[0] = (int32_t)(d_lat_lon_alt[0] * 1.e7);
        lat_lon_alt[1] = (int32_t)(d_lat_lon_alt[1] * 1.e7);
        lat_lon_alt[2] = (int32_t)((d_lat_lon_alt[2] * 1000)); // m to mm
    }
    
    /**
     *  Simulation airspeed + opposite of velocity vector.
     *  Windspeed should be acquired in [cm/s]
     */
    void get_true_wind_speed(uint16_t* wind_speed) { // <uint16_t(1)>
        
        int16_t ground_speed[3] = {0};
        this->get_ground_speed((int16_t*)ground_speed);   

        Eigen::Vector3d ground_speed_vec{3};
        for (uint i = 0; i < sizeof(ground_speed) / sizeof(int16_t); i++)
            ground_speed_vec[i] = ground_speed[i];

        // Environment wind is assumed to be in m/s -- cm/s is required.
        Eigen::Vector3d environment_wind = this->get_environment_wind() * 100; 
        Eigen::Vector3d cumulative_wind = (ground_speed_vec + environment_wind) * -1;
        double wind_magnitude = cumulative_wind.norm();
        *wind_speed = (uint16_t)wind_magnitude;
    }

    /**
     * Vehicle course-over-ground in [cDeg]
     */
    void get_course_over_ground(uint16_t* cog) { // <uint16_t(1)>
        Eigen::VectorXd state = this->get_vector_state();
        float xyz_dot[3] = {0};
        for (uint i = 0; i < 3; i++) *(xyz_dot+i) = state[i + 3];
        // Maybe convert xyz to earth frame?
        *cog = ((atan2(xyz_dot[0], xyz_dot[1]) * 180) / M_PI) * 100; // Deg => cDeg
    }

    /**
     * Yaw of vehicle relative to Earth's North, zero means not available, use 36000 for north
     * TODO: Make sure that the 6DOF does not spit out 0 for NORTH oriented vehicle
     * (0 in PX4 represents no-yaw info)
     * return yaw in [cDeg]
     */
    void get_vehicle_yaw_wrt_earth_north(uint16_t* yaw) { // <uint16_t(1)>
        Eigen::VectorXd state = this->get_vector_state();
        ConsoleLogger* logger = ConsoleLogger::shared_instance();
        *yaw = (uint16_t)std::round((((state[8]) * 180) / M_PI) * 100);

        if (*yaw == 0) {
            // logger->debug_log("[Warning] YAW is ZERO -- PX4 will interpret this as no-yaw!");
            // logger->debug_log("[Warning] Setting yaw to 1°...");
            *yaw += 1;
        }
    }

public:
    virtual uint64_t get_sim_time() = 0;
    // Environment wind in m/s
    virtual Eigen::Vector3d get_environment_wind() = 0;
    // Temperature in [degC]
    virtual float get_temperature_reading() = 0;

    /**
     * Drone state as populated by the CAELUS_FDM package.
     * <
     *  x , y , z    [0:3]  body-frame origin with respect to earth-frame (NED m)
     *  ẋ , ẏ , ż    [3:6]  body-frame velocity (m/s)
     *  ɸ , θ , ѱ    [6:9]  (roll, pitch, yaw) body-frame orientation with respect to earth-frame (rad)
     *  ɸ. , θ. , ѱ. [9:12] (roll., pitch., yaw.) body-frame orientation velocity (rad/s)
     * >
     */
    virtual Eigen::VectorXd& get_vector_state() = 0;
    /**
     *  (FixedWingEOM.h:evaluate)
     *  Drone state derivative as populated by the CAELUS_FDM package.
     *  ẋ , ẏ , ż       [0:3]  earth-frame velocity (What unit?)
     *  ẍ , ÿ , z̈       [3:6]  body-frame acceleration (m/s**2)
     *  ? , ? , ?       [6:9]  earth-frame angle rates (Euler rates?)
     *  ɸ.. , θ.. , ѱ.. [9:12] body-frame angular acceleration (What unit?)
     */
    virtual Eigen::VectorXd& get_vector_dx_state() = 0;
    
    mavlink_message_t hil_state_quaternion_msg(uint8_t system_id, uint8_t component_id) {
        mavlink_message_t msg;
        float attitude[4] = {0};
        float rpy_speed[3] = {0};
        int32_t lat_lon_alt[3] = {0};
        int16_t ground_speed[3] = {0};
        float f_acceleration[3] = {0};
        int16_t acceleration[3] = {0};
        uint16_t true_wind_speed = 0;

        this->get_attitude((float*)attitude);
        this->get_rpy_speed((float*)rpy_speed);
        this->get_lat_lon_alt((int32_t*)lat_lon_alt);
        this->get_ground_speed((int16_t*)ground_speed);
        this->get_body_frame_acceleration((float*)f_acceleration);
        this->get_true_wind_speed(&true_wind_speed);

        // (acc / G * 1000) => m/s**2 to mG (milli Gs)
        acceleration[0] = (int16_t)std::round((f_acceleration[0] / G_FORCE) * 1000);
        acceleration[1] = (int16_t)std::round((f_acceleration[1] / G_FORCE) * 1000);
        acceleration[2] = (int16_t)std::round((f_acceleration[2] / G_FORCE) * 1000);

#ifdef HIL_STATE_QUATERNION_VERBOSE
        Eigen::VectorXd& state = this->get_vector_state();
        float attitude_euler[3] = {0};
        for (uint i = 0; i < 3; i++) attitude_euler[i] = state[6+i];

        printf("[HIL STATE QUATERNION]\n");
        printf("Attitude quaternion: %f %f %f %f \n", attitude[0], attitude[1], attitude[2], attitude[3]);
        printf("Attitude euler: roll: %f pitch: %f yaw: %f \n", attitude_euler[0], attitude_euler[1], attitude_euler[2]);
        printf("RPY Speed: %f %f %f \n", rpy_speed[0], rpy_speed[1], rpy_speed[2]);
        printf("Lat Lon Alt: %d %d %d \n", lat_lon_alt[0], lat_lon_alt[1], lat_lon_alt[2]);
        printf("Ground speed: %d %d %d \n", ground_speed[0], ground_speed[1], ground_speed[2]);
        printf("Acceleration: %d %d %d \n", acceleration[0], acceleration[1], acceleration[2]);
        printf("True wind speed: %d \n", true_wind_speed);
        printf("Sim time %llu\n", this->get_sim_time());
#endif


        mavlink_msg_hil_state_quaternion_pack(
            system_id,
            component_id,
            &msg,
            this->get_sim_time(),
            (float*)attitude,
            rpy_speed[0], 
            rpy_speed[1],
            rpy_speed[2],
            lat_lon_alt[0],
            lat_lon_alt[1],
            lat_lon_alt[2],
            ground_speed[0],
            ground_speed[1],
            ground_speed[2],
            true_wind_speed,
            true_wind_speed,
            acceleration[0],
            acceleration[1],
            acceleration[2]
        );

        return msg;
    }

    mavlink_message_t hil_sensor_msg(uint8_t system_id, uint8_t component_id) {
        mavlink_message_t msg;
        
        int32_t lat_lon_alt[3] = {0};
        float body_frame_acc[3] = {0}; // m/s**2
        float gyro_xyz[3] = {0}; // rad/s
        float magfield[3] = {0}; // gauss
        float abs_pressure = 0; // hPa
        float diff_pressure = 0;

        this->get_body_frame_acceleration((float*)body_frame_acc);
        this->get_rpy_speed((float*) gyro_xyz);
        this->get_lat_lon_alt((int32_t*)lat_lon_alt);
        abs_pressure = DroneStateEncoder::alt_to_baro((double)lat_lon_alt[2] / 1000) / 100;

        Eigen::VectorXd mag_field_vec = magnetic_field_for_latlonalt((const int32_t*)lat_lon_alt);
        for (int i = 0; i < mag_field_vec.size(); i++) magfield[i] = mag_field_vec[i];

#ifdef HIL_SENSOR_VERBOSE
        printf("[HIL_SENSOR]\n");
        printf("Body frame Acceleration: %f %f %f \n", body_frame_acc[0], body_frame_acc[1], body_frame_acc[2]);
        printf("GYRO xyz: %f %f %f \n", gyro_xyz[0], gyro_xyz[1], gyro_xyz[2]);
        printf("Magfield: %f %f %f \n", magfield[0], magfield[1], magfield[2]);
        printf("Absolute pressure: %f\n", abs_pressure);
        printf("Differential pressure: %f\n", diff_pressure);
        printf("Alt: %d \n", lat_lon_alt[2]);
        printf("Temperature %f\n", this->get_temperature_reading());
        printf("Sim time %llu\n", this->get_sim_time());
#endif

        mavlink_msg_hil_sensor_pack(
            system_id,
            component_id,
            &msg,
            this->get_sim_time(),
            body_frame_acc[0],
            body_frame_acc[1],
            body_frame_acc[2],
            gyro_xyz[0],
            gyro_xyz[1],
            gyro_xyz[2],
            magfield[0],
            magfield[1],
            magfield[2],
            abs_pressure,
            diff_pressure,
            lat_lon_alt[2], // Altitude (Should be noisy -- exact for now)
            this->get_temperature_reading(),
            0b111 | 0b111000 | 0b111000000 | 0b1111000000000,//4294967295,// 7167, // Fields updated (all)
            0 // ID
        );

        return msg;
    }

    mavlink_message_t hil_gps_msg(uint8_t system_id, uint8_t component_id) {
        mavlink_message_t msg;
        
        // DegE7, DegE7, mm
        int32_t lon_lat_alt[3] = {0};
        // Earth-fixed NED frame (cm/s)
        uint16_t ground_speed[3] = {0};
        // Scalar horizontal speed (sqrt(vel.x**2, vel.y**2)) (see below)
        uint16_t gps_ground_speed = 0;
        // cm/s in NED earth fixed frame
        int16_t gps_velocity_ned[3] = {0};
        // Course Over Ground is the actual direction of progress of a vessel, 
        // between two points, with respect to the surface of the earth.
        uint16_t course_over_ground = 0; // 0.0..359.99 degrees // cdeg
        // number of visible satellites
        uint8_t sat_visible = UINT8_MAX;
        // Vehicle yaw relative to earth's north
        // Yaw of vehicle relative to Earth's North, zero means not available, use 36000 for north
        uint16_t vehicle_yaw = 0; // 0 means not available

        // Diluition of position measurements
        // Should smooth overtime from high value to low value
        // to simulate improved measurement accuracy over time.
        // TODO: Implement smoothing (Kalman filter?)
        uint16_t eph = 0.3f * 100; // minimum HDOP 
        uint16_t epv = 0.4f * 100; // minimum HDOP 

        this->get_lat_lon_alt((int32_t*)&lon_lat_alt);
        this->get_earth_fixed_velocity((int16_t*)gps_velocity_ned);
        this->get_vehicle_yaw_wrt_earth_north(&vehicle_yaw);
        this->get_ground_speed((int16_t*)ground_speed);
        gps_ground_speed = sqrt(pow(ground_speed[0], 2) + pow(ground_speed[1], 2));

#ifdef HIL_GPS_VERBOSE
        printf("[GPS SENSOR]\n");
        printf("Lon Lat Alt: %d %d %d \n", lon_lat_alt[0], lon_lat_alt[1], lon_lat_alt[2]);
        printf("EPH EPV: %d %d \n", eph, epv);
        printf("Ground speed: %d %d %d \n", ground_speed[0], ground_speed[1], ground_speed[2]);
        printf("GPS ground speed: %d\n", gps_ground_speed);
        printf("GPS velocity NED: %d %d %d \n", gps_velocity_ned[0], gps_velocity_ned[1], gps_velocity_ned[2]);
        printf("Course over ground: %d \n",  course_over_ground);
        printf("Sats visible: %d \n",  sat_visible);
        printf("Vehicle yaw: %d \n",  vehicle_yaw);
        printf("Sim time %llu\n", this->get_sim_time());
#endif


        mavlink_msg_hil_gps_pack(
            system_id,
            component_id,
            &msg,
            this->get_sim_time(),
            3, // 3d fix
            lon_lat_alt[1],
            lon_lat_alt[0],
            lon_lat_alt[2],
            eph,
            epv,
            gps_ground_speed,
            gps_velocity_ned[0],
            gps_velocity_ned[1],
            gps_velocity_ned[2],
            course_over_ground,
            sat_visible,
            0, // ID
            vehicle_yaw
        );

        return msg;
    }

    mavlink_message_t system_time_msg(uint8_t system_id, uint8_t component_id) {
        mavlink_message_t msg;

        uint64_t time_unix_usec = boost::chrono::duration_cast<boost::chrono::microseconds>(boost::chrono::system_clock::now().time_since_epoch()).count();
        uint32_t time_boot_ms = this->get_sim_time() / 1000; // us to ms

        mavlink_msg_system_time_pack(
            system_id,
            component_id,
            &msg,
            time_unix_usec,
            time_boot_ms
        );

        return msg;
    }
};

#endif // __DRONESTATE_H__