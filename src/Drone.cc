#include "Drone.h"
#include "DroneSensors.h"
#include "Logging/ConsoleLogger.h"
#include "Helpers/rotationMatrix.h"

// #define HIL_ACTUATOR_CONTROLS_VERBOSE

DroneConfig config_from_file_path(char* path) {
    DroneConfig conf;
    std::ifstream fin(path);
    fin >> conf;
    return conf;
}

Drone::Drone(char* config_file, MAVLinkMessageRelay& connection, Clock& clock) : 
    MAVLinkSystem::MAVLinkSystem(1, 1),
    DynamicObject::DynamicObject(config_from_file_path(config_file), clock),
    config(config_from_file_path(config_file)),
    connection(connection)
    {
        this->connection.add_message_handler(this);
        this->_setup_drone();
    }

void Drone::_setup_drone() {
    // Inject controllers into dynamics model
    this->setControllerThrust([this] (double dt) -> Eigen::VectorXd
        { return this->thrust_propellers.control(dt); });
    this->setControllerAero([this] (double dt) -> Eigen::VectorXd
        { return this->ailerons.control(dt); });
    this->setControllerVTOL([this] (double dt) -> Eigen::VectorXd
        { return this->vtol_propellers.control(dt); });
}

void Drone::fake_ground_transform(boost::chrono::microseconds us) {
    double dt = (us.count() / 1000.0) / 1000.0;
    Eigen::Vector3d position = this->get_sensors().get_earth_frame_position(); // NED
    Eigen::Vector3d velocity = this->get_sensors().get_earth_frame_velocity(); // NED
    Eigen::Vector3d acceleration = caelus_fdm::body2earth(this->state) * this->get_sensors().get_body_frame_acceleration();

    if (position[2] >= (this->ground_height - 0.001) && velocity[2] + acceleration[2] * dt >= 0.0) {
        this->state[2] = 0;
        // Body frame velocity
        this->state.segment(3, 3) = Eigen::VectorXd::Zero(3);
        // Body frame acc
        this->dx_state.segment(3, 3) = Eigen::VectorXd::Zero(3);
        this->dx_state[5] = G_FORCE;
        // Rotation rate
        this->state.segment(9, 3) = Eigen::VectorXd::Zero(3);
        // Orientation
        this->state.segment(6, 3) = Eigen::VectorXd::Zero(3);
    }
}
void Drone::update(boost::chrono::microseconds us) {
    MAVLinkSystem::update(us);
    DynamicObject::update(us);
    this->fake_ground_transform(us);
    this->_process_mavlink_messages();
    this->_publish_state(us);
}

MAVLinkMessageRelay& Drone::get_mavlink_message_relay() {
    return this->connection;
}

void Drone::_publish_hil_gps() {
    this->connection.enqueue_message(
        this->hil_gps_msg(this->system_id, this->component_id)
    );
}

void Drone::_publish_system_time() {
    this->connection.enqueue_message(
        this->system_time_msg(this->system_id, this->component_id)
    );
}

void Drone::_publish_hil_sensor() {
    this->connection.enqueue_message(this->hil_sensor_msg(this->system_id, this->component_id));
}

void Drone::_publish_hil_state_quaternion() {
    mavlink_message_t msg = this->hil_state_quaternion_msg(this->system_id, this->component_id);
    this->connection.enqueue_message(msg);
}

void Drone::_publish_state(boost::chrono::microseconds us)
 {
    if (!this->connection.connection_open()) return;
    if (!(this->should_reply_lockstep || this->hil_actuator_controls_msg_n < 300)) return;

    this->clock.unlock_time();

    if (this->sys_time_throttle_counter++ % 1000) {
        this->_publish_system_time();
    }

    this->_publish_hil_gps();
    this->_publish_hil_sensor();
    this->should_reply_lockstep = false;

    bool should_send_autopilot_telemetry = 
        (this->clock.get_current_time_us() - this->last_autopilot_telemetry).count()
        > this->hil_state_quaternion_message_frequency;
    
    if (!should_send_autopilot_telemetry) return;
    
    this->last_autopilot_telemetry = this->clock.get_current_time_us();
    
    this->_publish_hil_state_quaternion();

}

/**
 * Process a MAVLink command.
 * Correct command receival must be ACK'ed.
 * 
 */
void Drone::_process_command_long_message(mavlink_message_t m) {
    mavlink_command_long_t command;
    mavlink_message_t command_ack_msg;
    mavlink_msg_command_long_decode(&m, &command);
    uint16_t command_id = command.command;
    
    switch(command_id) {
        case MAV_CMD_SET_MESSAGE_INTERVAL:
            printf("Simulator -> PX4 message interval now set to %f (us)\n", command.param2);
            this->hil_state_quaternion_message_frequency = command.param2;
            break;
        default:
            fprintf(stdout, "Unknown command id from command long (%d)", command_id);
    }
    
    mavlink_msg_command_ack_pack(this->system_id, this->component_id, &command_ack_msg, command_id, 0, 0, 0, command.target_system, command.target_component);
    this->connection.enqueue_message(command_ack_msg);
}

void Drone::_process_hil_actuator_controls(mavlink_message_t m) {
    
    this->should_reply_lockstep = true;
    this->hil_actuator_controls_msg_n++;

    mavlink_hil_actuator_controls_t controls;
    mavlink_msg_hil_actuator_controls_decode(&m, &controls);
    this->armed = (controls.mode & MAV_MODE_FLAG_SAFETY_ARMED) > 0;
    //this->mav_mode = controls.mode;
    
    Eigen::VectorXd vtol_prop_controls{4};
    for (int i = 0; i < 4; i++) vtol_prop_controls[i] = controls.controls[i];
    Eigen::VectorXd ailerons_controls{2};
    for (int i = 0; i < 2; i++) ailerons_controls[i] = controls.controls[4+i];
    Eigen::VectorXd thrust_propeller_controls{1};
    for (int i = 0; i < 1; i++) thrust_propeller_controls[i] = controls.controls[8+i];

#ifdef HIL_ACTUATOR_CONTROLS_VERBOSE
    printf("HIL_ACTUATOR_CONTROLS:\n");
    for (int i = 0; i < 16; i++) {
        printf("\tControl #%d: %f\n", i, controls.controls[i]);
    } printf("\n");
#endif

    this->thrust_propellers.set_control(thrust_propeller_controls);
    this->ailerons.set_control(ailerons_controls);
    this->vtol_propellers.set_control(vtol_prop_controls);
    
}

void Drone::_process_mavlink_message(mavlink_message_t m) {
    ConsoleLogger* logger = ConsoleLogger::shared_instance();
    switch(m.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT:
            logger->debug_log("MSG: HEARTBEAT");
            break;
        case MAVLINK_MSG_ID_HIL_ACTUATOR_CONTROLS:
            logger->debug_log("MSG: HIL_ACTUATOR_CONTROLS");
            this->_process_hil_actuator_controls(m);
            break;
        case MAVLINK_MSG_ID_COMMAND_LONG:
            logger->debug_log("MSG: COMMAND_LONG");
            this->_process_command_long_message(m);
            break;
        default:
            logger->debug_log("Unknown message!");
    }
}

void Drone::_process_mavlink_messages() {
    this->message_queue.consume_all([this](mavlink_message_t m){
        this->_process_mavlink_message(m);
    });
}

void Drone::handle_mavlink_message(mavlink_message_t m) {
    this->message_queue.push(m);
}

Sensors& Drone::get_sensors() {
    return this->sensors;
}

#pragma mark DroneStateEncoder

uint64_t Drone::get_sim_time() {
    return this->clock.get_current_time_us().count();
}

uint8_t Drone::get_mav_mode() {
    return this->mav_mode;
}