// "Copyright [year] <Copyright Owner>"

#include "pixhawk_platform.hpp"

PixhawkPlatform::PixhawkPlatform() : as2::AerialPlatform()
{
  configureSensors();

  // declare subscribers
  px4_imu_sub_ = this->create_subscription<px4_msgs::msg::SensorCombined>(
    "fmu/sensor_combined/out", rclcpp::SensorDataQoS(),
    std::bind(&PixhawkPlatform::px4imuCallback, this, std::placeholders::_1));

  px4_timesync_sub_ = this->create_subscription<px4_msgs::msg::Timesync>(
    "fmu/timesync/out", rclcpp::SensorDataQoS(),
    [this](const px4_msgs::msg::Timesync::UniquePtr msg) { timestamp_.store(msg->timestamp); });

  px4_vehicle_control_mode_sub_ = this->create_subscription<px4_msgs::msg::VehicleControlMode>(
    "fmu/vehicle_control_mode/out", rclcpp::SensorDataQoS(),
    std::bind(&PixhawkPlatform::px4VehicleControlModeCallback, this, std::placeholders::_1));

  px4_gps_sub_ = this->create_subscription<px4_msgs::msg::SensorGps>(
    "fmu/sensor_gps/out", 1,
    std::bind(&PixhawkPlatform::gpsCallback, this, std::placeholders::_1));

  if (this->getFlagSimulationMode() == true) {
    px4_odometry_sub_ = this->create_subscription<px4_msgs::msg::VehicleOdometry>(
      "fmu/vehicle_odometry/out", rclcpp::SensorDataQoS(),
      std::bind(&PixhawkPlatform::px4odometryCallback, this, std::placeholders::_1));

  } else {
    // In real flights, the odometry is published by the onboard computer.
    odometry_sub_ = this->create_subscription<nav_msgs::msg::Odometry>(
      this->generate_global_name(as2_names::topics::self_localization::odom), 
      as2_names::topics::self_localization::qos,
      [this](const nav_msgs::msg::Odometry::UniquePtr msg) { this->odometry_msg_ = *msg; });

    static auto px4_publish_vo_timer = this->create_wall_timer(
      std::chrono::milliseconds(10), [this]() { this->PX4publishVisualOdometry(); });
  }

  // declare publishers
  px4_offboard_control_mode_pub_ =
    this->create_publisher<px4_msgs::msg::OffboardControlMode>("fmu/offboard_control_mode/in", rclcpp::SensorDataQoS());
  px4_trajectory_setpoint_pub_ =
    this->create_publisher<px4_msgs::msg::TrajectorySetpoint>("fmu/trajectory_setpoint/in", rclcpp::SensorDataQoS());
  px4_vehicle_attitude_setpoint_pub_ =
    this->create_publisher<px4_msgs::msg::VehicleAttitudeSetpoint>(
      "fmu/vehicle_attitude_setpoint/in", rclcpp::SensorDataQoS());
  px4_vehicle_rates_setpoint_pub_ =
    this->create_publisher<px4_msgs::msg::VehicleRatesSetpoint>("fmu/vehicle_rates_setpoint/in", rclcpp::SensorDataQoS());
  px4_vehicle_command_pub_ =
    this->create_publisher<px4_msgs::msg::VehicleCommand>("fmu/vehicle_command/in", rclcpp::SensorDataQoS());
  px4_visual_odometry_pub_ = this->create_publisher<px4_msgs::msg::VehicleVisualOdometry>(
    "fmu/vehicle_visual_odometry/in", rclcpp::SensorDataQoS());

  // Timers
  static auto timer_commands_ =
    this->create_wall_timer(std::chrono::milliseconds(10), [this]() { this->ownSendCommand(); });
  timer_ =
    this->create_wall_timer(std::chrono::milliseconds(10), [this]() { publishSensorData(); });
}

void PixhawkPlatform::configureSensors()
{
  imu_sensor_ptr_ = std::make_unique<as2::sensors::Imu>("imu", this);

  // TODO: implement Battery Sensor for px4
  // battery_sensor_ptr_ =
  //   std::make_unique<as2::sensors::Sensor<sensor_msgs::msg::BatteryState>>("battery", this);
  gps_sensor_ptr_ =
    std::make_unique<as2::sensors::GPS>("gps", this);

  odometry_raw_estimation_ptr_ =
    std::make_unique<as2::sensors::Sensor<nav_msgs::msg::Odometry>>("odometry", this);
};

void PixhawkPlatform::publishSensorData()
{
  auto timestamp = this->get_clock()->now();
  imu_msg_.header.stamp = timestamp;
  imu_sensor_ptr_->publishData(imu_msg_);

  //battery_msg_.header.stamp = timestamp;
  //battery_sensor_ptr_->publishData(battery_msg_);

  nav_sat_fix_msg_.header.stamp = timestamp;
  gps_sensor_ptr_->publishData(nav_sat_fix_msg_);

  if (this->getFlagSimulationMode() == true) {
    px4_odometry_msg_.header.stamp = timestamp;
    odometry_raw_estimation_ptr_->publishData(px4_odometry_msg_);
  }
}

bool PixhawkPlatform::ownSetArmingState(bool state)
{
  if (state) {
    this->PX4arm();
  } else {
    this->PX4disarm();
  }
  return true;
};

bool PixhawkPlatform::ownSetOffboardControl(bool offboard)
{
  //TODO: CREATE A DEFAULT CONTROL MODE FOR BEING ABLE TO SWITCH TO OFFBOARD MODE BEFORE RUNNING THE CONTROLLER

  if (offboard == false) {
    RCLCPP_ERROR(
      this->get_logger(), "Turning into MANUAL Mode is not allowed from the onboard computer");
    return false;
  }

  // default control mode
  // platform_control_mode_.yaw_mode = as2_msgs::msg::PlatformControlMode::YAW_SPEED;
  // platform_control_mode_.control_mode = as2_msgs::msg::PlatformControlMode::ACRO_MODE;

  px4_offboard_control_mode_ = px4_msgs::msg::OffboardControlMode();
  px4_offboard_control_mode_.body_rate = true;
  resetRatesSetpoint();

  command_changes_ = false;

  RCLCPP_INFO(this->get_logger(), "Switching to OFFBOARD mode");
  rclcpp::Rate r(100);
  for (int i = 0; i < 100; i++) {
    PX4publishRatesSetpoint();
    r.sleep();
  }
  PX4publishOffboardControlMode();
  return true;
};

bool PixhawkPlatform::ownSetPlatformControlMode(const as2_msgs::msg::PlatformControlMode & msg)
{
  // return true;

  RCLCPP_INFO(this->get_logger(), "Setting platform control mode");

  px4_offboard_control_mode_ = px4_msgs::msg::OffboardControlMode();  // RESET CONTROL MODE

  /* PIXHAWK CONTROL MODES:
  px4_offboard_control_mode_.position      ->  x,y,z
  px4_offboard_control_mode_.velocity      ->  vx,vy,vz
  px4_offboard_control_mode_.acceleration  ->  ax,ay,az
  px4_offboard_control_mode_.attitude      ->  q(r,p,y) + T(tx,ty,tz) in multicopters tz = -Collective_Thrust
  px4_offboard_control_mode_.body_rate     ->  p ,q ,r  + T(tx,ty,tz) in multicopters tz = -Collective_Thrust */

  switch (msg.control_mode) {
    case as2_msgs::msg::PlatformControlMode::POSITION_MODE: {
      px4_offboard_control_mode_.position = true;
      RCLCPP_INFO(this->get_logger(), "POSITION_MODE ENABLED");
    } break;
    case as2_msgs::msg::PlatformControlMode::SPEED_MODE: {
      px4_offboard_control_mode_.velocity = true;
      RCLCPP_INFO(this->get_logger(), "SPEED_MODE ENABLED");
    } break;
    case as2_msgs::msg::PlatformControlMode::ATTITUDE_MODE: {
      px4_offboard_control_mode_.attitude = true;
      RCLCPP_INFO(this->get_logger(), "ATTITUDE_MODE ENABLED");
    } break;
    case as2_msgs::msg::PlatformControlMode::ACCEL_MODE: {
      px4_offboard_control_mode_.acceleration = true;
      RCLCPP_INFO(this->get_logger(), "ACCEL_MODE ENABLED");
    } break;
    case as2_msgs::msg::PlatformControlMode::ACRO_MODE: {
      px4_offboard_control_mode_.body_rate = true;
      RCLCPP_INFO(this->get_logger(), "ACRO_MODE ENABLED");
    } break;
    default:
      has_mode_settled_ = false;
      return false;
  }

  has_mode_settled_ = true;
  return true;
};

bool PixhawkPlatform::ownSendCommand()
{
  px4_offboard_control_mode_.body_rate = true;
  static bool first_run = true;
  if (first_run) {
    px4_rates_setpoint_.roll = 0;
    px4_rates_setpoint_.pitch = 0;
    px4_rates_setpoint_.yaw = 0;

    px4_rates_setpoint_.thrust_body[2] = -0.2f;

    PX4publishRatesSetpoint();
  }
  if (getOffboardMode() && getArmingState()) {
    first_run = false;

    px4_rates_setpoint_.roll = command_twist_msg_.twist.angular.x;
    px4_rates_setpoint_.pitch = command_twist_msg_.twist.angular.y;
    px4_rates_setpoint_.yaw = command_twist_msg_.twist.angular.z;

    // px4_rates_setpoint_.thrust_body[2] = -command_thrust_msg_.thrust_normalized;

    //TODO: CLEAN THIS UP
    if (command_thrust_msg_.thrust < 0.001) {
      px4_rates_setpoint_.thrust_body[2] = -0.15f;
    } else {
      px4_rates_setpoint_.thrust_body[2] = -command_thrust_msg_.thrust / this->getMaxThrust();
    }

    PX4publishRatesSetpoint();
  }
  return true;
}

/*
bool PixhawkPlatform::ownSendCommand()
{
  // if (command_changes_){
  if (true){
    //TODO: implement multiple PlatformControlMode

    // switch (msg.reference_frame)
    // {
    // case as2_msgs::msg::PlatformControlMode::LOCAL_ENU_FRAME:
    //   break;
    // case as2_msgs::msg::PlatformControlMode::GLOBAL_ENU_FRAME:
    //   msg.header.frame_id = "map";
    //   break;
    // case as2_msgs::msg::PlatformControlMode::BODY_FLU_FRAME:
    //   msg.header.frame_id = "base_link";
    //   break;
    // }

    // TODO: CREATE SUBSCRIBERS TO POSE, TWIST AND THRUST COMMANDS

    if (px4_offboard_control_mode_.position || px4_offboard_control_mode_.velocity || px4_offboard_control_mode_.acceleration){
      if (platform_control_mode_.yaw_mode == as2_msgs::msg::PlatformControlMode::YAW_ANGLE) {
        // RCLCPP_INFO(this->get_logger(), "Computing yaw angle");

        Eigen::Quaterniond q_enu;
        q_enu.w() = this->command_pose_msg_.pose.orientation.w;
        q_enu.x() = this->command_pose_msg_.pose.orientation.x;
        q_enu.y() = this->command_pose_msg_.pose.orientation.y;
        q_enu.z() = this->command_pose_msg_.pose.orientation.z;

        Eigen::Quaterniond q_ned = px4_ros_com::frame_transforms::transform_orientation(
          q_enu, px4_ros_com::frame_transforms::StaticTF::ENU_TO_NED);
        Eigen::Quaterniond q_aircraft = px4_ros_com::frame_transforms::transform_orientation(
          q_aircraft, px4_ros_com::frame_transforms::StaticTF::BASELINK_TO_AIRCRAFT);

        double roll, pitch, yaw;
        px4_ros_com::frame_transforms::utils::quaternion::quaternion_to_euler(
          q_aircraft, roll, pitch, yaw);

        px4_trajectory_setpoint_.yaw = yaw;

       
      } else
        // TODO: Handle yaw_speed in this modes
        px4_trajectory_setpoint_.yawspeed = command_twist_msg_.twist.angular.z;
    
    }


    switch (platform_control_mode_.control_mode) {
      case as2_msgs::msg::PlatformControlMode::POSITION_MODE: {
        Eigen::Vector3d position_enu;
        position_enu.x() = this->command_pose_msg_.pose.position.x;
        position_enu.y() = this->command_pose_msg_.pose.position.y;
        position_enu.z() = this->command_pose_msg_.pose.position.z;

        Eigen::Vector3d position_ned = px4_ros_com::frame_transforms::transform_static_frame(
          position_enu, px4_ros_com::frame_transforms::StaticTF::ENU_TO_NED);

        px4_trajectory_setpoint_.x = position_ned.x();
        px4_trajectory_setpoint_.y = position_ned.y();
        px4_trajectory_setpoint_.z = position_ned.z();
      } break;
      case as2_msgs::msg::PlatformControlMode::SPEED_MODE: {
        Eigen::Vector3d speed_enu;
        speed_enu.x() = this->command_twist_msg_.twist.linear.x;
        speed_enu.y() = this->command_twist_msg_.twist.linear.y;
        speed_enu.z() = this->command_twist_msg_.twist.linear.z;

        Eigen::Vector3d speed_ned = px4_ros_com::frame_transforms::transform_static_frame(
          speed_enu, px4_ros_com::frame_transforms::StaticTF::ENU_TO_NED);

        px4_trajectory_setpoint_.vx = speed_ned.x();
        px4_trajectory_setpoint_.vy = speed_ned.y();
        px4_trajectory_setpoint_.vz = speed_ned.z();
      } break;
      case as2_msgs::msg::PlatformControlMode::ATTITUDE_MODE :{
        
        Eigen::Quaterniond q_enu;
        q_enu.w() = this->command_pose_msg_.pose.orientation.w;
        q_enu.x() = this->command_pose_msg_.pose.orientation.x;
        q_enu.y() = this->command_pose_msg_.pose.orientation.y;
        q_enu.z() = this->command_pose_msg_.pose.orientation.z;

        Eigen::Quaterniond q_ned = px4_ros_com::frame_transforms::transform_orientation(
          q_enu, px4_ros_com::frame_transforms::StaticTF::ENU_TO_NED);
        Eigen::Quaterniond q_aircraft = px4_ros_com::frame_transforms::transform_orientation(
          q_aircraft, px4_ros_com::frame_transforms::StaticTF::BASELINK_TO_AIRCRAFT);

        px4_attitude_setpoint_.q_d[0] = q_aircraft.w();
        px4_attitude_setpoint_.q_d[1] = q_aircraft.x();
        px4_attitude_setpoint_.q_d[2] = q_aircraft.y();
        px4_attitude_setpoint_.q_d[3] = q_aircraft.z();

        px4_attitude_setpoint_.thrust_body[2] = -command_thrust_msg_.thrust_normalized;  // minus because px4 uses NED (Z is downwards)

        }
        break;
      case as2_msgs::msg::PlatformControlMode::ACRO_MODE :{

        // TODO: CHECK ORIENTATION

        // Eigen::Vector3d angular_speed_enu(command_twist_msg_.twist.angular.x,
        //                                   command_twist_msg_.twist.angular.y,
        //                                   command_twist_msg_.twist.angular.z );

        // std::cout << angular_speed_enu << std::endl;

        // Eigen::Vector3d angular_speed_ned = px4_ros_com::frame_transforms::transform_static_frame(
        //   angular_speed_enu, px4_ros_com::frame_transforms::StaticTF::ENU_TO_NED);

        // px4_rates_setpoint_.roll = angular_speed_ned.x();
        // px4_rates_setpoint_.pitch = angular_speed_ned.y();
        // px4_rates_setpoint_.yaw = angular_speed_ned.z();

        //px4_rates_setpoint_.roll = command_twist_msg_.twist.angular.x;
        //px4_rates_setpoint_.pitch = command_twist_msg_.twist.angular.y;
        // px4_rates_setpoint_.yaw = command_twist_msg_.twist.angular.z;
        
        px4_rates_setpoint_.roll = 0.0f;
        px4_rates_setpoint_.pitch = 0.0f;
        px4_rates_setpoint_.yaw = -1.0f;

        px4_rates_setpoint_.thrust_body[2] = -command_thrust_msg_.thrust_normalized;  // minus because px4 uses NED (Z is downwards)

        // RCLCPP_INFO(this->get_logger(), "ACRO_MODE");
        // RCLCPP_INFO(this->get_logger(), "roll body rates : = %f", angular_speed_ned.x());
        // RCLCPP_INFO(this->get_logger(), "pitch body rates : = %f", angular_speed_ned.y());
        // RCLCPP_INFO(this->get_logger(), "yaw body rates : = %f", angular_speed_ned.z());
        RCLCPP_INFO(this->get_logger(), "thrust : = %f", command_thrust_msg_.thrust_normalized);
        
      
        }
        
      break;

      case as2_msgs::msg::PlatformControlMode::ACCEL_MODE :{
        //TODO: implement this mode 
        
        RCLCPP_ERROR(this->get_logger(), "ACCELERATION CONTROL MODE is not supported yet ");
        return false;}
        break;

      default:
        return false;
    }
    
  }// end if(command_changes)

  // Actuator commands are published continously

  // if (platform_status_ptr_->offboard && platform_status_ptr_->armed )
  if (true)
  {
    // RCLCPP_INFO(this->get_logger(), "Publishing actuator commands");
    if (px4_offboard_control_mode_.attitude)
    {
      //RCLCPP_INFO(this->get_logger(), "Pixhawk is sending command");
      this->PX4publishOffboardControlMode();
      this->PX4publishAttitudeSetpoint();
    }
    else if (px4_offboard_control_mode_.body_rate){
      this->PX4publishOffboardControlMode();
      this->PX4publishRatesSetpoint();
    } 
    else if (px4_offboard_control_mode_.position || px4_offboard_control_mode_.velocity || px4_offboard_control_mode_.acceleration)
    {
      // RCLCPP_INFO(this->get_logger(), "Publishing TrajectorySetpoint");
      this->PX4publishOffboardControlMode();
      this->PX4publishTrajectorySetpoint();
    }
  }
  return true;
};
*/

void PixhawkPlatform::resetTrajectorySetpoint()
{
  px4_trajectory_setpoint_.x = NAN;
  px4_trajectory_setpoint_.y = NAN;
  px4_trajectory_setpoint_.z = NAN;

  px4_trajectory_setpoint_.vx = NAN;
  px4_trajectory_setpoint_.vy = NAN;
  px4_trajectory_setpoint_.vz = NAN;

  px4_trajectory_setpoint_.yaw = NAN;
  px4_trajectory_setpoint_.yawspeed = NAN;

  px4_trajectory_setpoint_.acceleration = std::array<float, 3>{NAN, NAN, NAN};
  px4_trajectory_setpoint_.jerk = std::array<float, 3>{NAN, NAN, NAN};
  px4_trajectory_setpoint_.thrust = std::array<float, 3>{NAN, NAN, NAN};
};

void PixhawkPlatform::resetAttitudeSetpoint()
{
  px4_attitude_setpoint_.pitch_body = NAN;
  px4_attitude_setpoint_.roll_body = NAN;
  px4_attitude_setpoint_.yaw_body = NAN;

  // FIXME: HARDCODED VALUES
  px4_attitude_setpoint_.q_d = std::array<float, 4>{0, 0, 0, 1};
  px4_attitude_setpoint_.thrust_body = std::array<float, 3>{0, 0, -0.2f};
};

void PixhawkPlatform::resetRatesSetpoint()
{
  RCLCPP_INFO(this->get_logger(), "Resetting rates setpoint");
  px4_rates_setpoint_.roll = 0.0f;
  px4_rates_setpoint_.pitch = 0.0f;
  px4_rates_setpoint_.yaw = 0.0f;
  px4_attitude_setpoint_.thrust_body = std::array<float, 3>{0, 0, -0.2f};
};

/** -----------------------------------------------------------------*/
/** ------------------------- PX4 FUNCTIONS -------------------------*/
/** -----------------------------------------------------------------*/

/**
 * @brief Send a command to Arm the vehicle
 */

void PixhawkPlatform::PX4arm() const
{
  PX4publishVehicleCommand(px4_msgs::msg::VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM, 1.0);
  RCLCPP_INFO(this->get_logger(), "Arm command send");
}

/**
 * @brief Send a command to Disarm the vehicle
 */
void PixhawkPlatform::PX4disarm() const
{
  PX4publishVehicleCommand(px4_msgs::msg::VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM, 0.0);
  RCLCPP_INFO(this->get_logger(), "Disarm command send");
}

/**
 * @brief Publish the offboard control mode.
 *        
 */
void PixhawkPlatform::PX4publishOffboardControlMode()
{
  PX4publishVehicleCommand(px4_msgs::msg::VehicleCommand::VEHICLE_CMD_DO_SET_MODE, 1, 6);
  RCLCPP_INFO(this->get_logger(), "OFFBOARD mode enabled");
}

/**
 * @brief Publish a trajectory setpoint
 */
void PixhawkPlatform::PX4publishTrajectorySetpoint()
{
  px4_trajectory_setpoint_.timestamp = timestamp_.load();
  px4_offboard_control_mode_.timestamp = timestamp_.load();

  px4_trajectory_setpoint_pub_->publish(px4_trajectory_setpoint_);
  px4_offboard_control_mode_pub_->publish(px4_offboard_control_mode_);
}

void PixhawkPlatform::PX4publishAttitudeSetpoint()
{
  px4_attitude_setpoint_.timestamp = timestamp_.load();
  px4_offboard_control_mode_.timestamp = timestamp_.load();

  px4_vehicle_attitude_setpoint_pub_->publish(px4_attitude_setpoint_);
  px4_offboard_control_mode_pub_->publish(px4_offboard_control_mode_);
}

void PixhawkPlatform::PX4publishRatesSetpoint()
{
  px4_rates_setpoint_.timestamp = timestamp_.load();
  px4_offboard_control_mode_.timestamp = timestamp_.load();

  px4_vehicle_rates_setpoint_pub_->publish(px4_rates_setpoint_);
  px4_offboard_control_mode_pub_->publish(px4_offboard_control_mode_);
}

/**
 * @brief Publish vehicle commands
 * @param command   Command code (matches VehicleCommand and MAVLink MAV_CMD codes)
 * @param param1    Command parameter 1
 * @param param2    Command parameter 2
 */
void PixhawkPlatform::PX4publishVehicleCommand(uint16_t command, float param1, float param2) const
{
  px4_msgs::msg::VehicleCommand msg{};
  msg.timestamp = timestamp_.load();
  msg.param1 = param1;
  msg.param2 = param2;
  msg.command = command;
  msg.target_system = 1;
  msg.target_component = 1;
  msg.source_system = 1;
  msg.source_component = 1;
  msg.from_external = true;

  px4_vehicle_command_pub_->publish(msg);
}

void PixhawkPlatform::PX4publishVisualOdometry()
{
  using namespace px4_ros_com::frame_transforms;

  Eigen::Quaterniond q_enu(
    odometry_msg_.pose.pose.orientation.w, odometry_msg_.pose.pose.orientation.x,
    odometry_msg_.pose.pose.orientation.y, odometry_msg_.pose.pose.orientation.z);

  Eigen::Vector3d pos_enu(
    odometry_msg_.pose.pose.position.x, odometry_msg_.pose.pose.position.y,
    odometry_msg_.pose.pose.position.z);

  Eigen::Vector3d vel_enu(
    odometry_msg_.twist.twist.linear.x, odometry_msg_.twist.twist.linear.y,
    odometry_msg_.twist.twist.linear.z);

  // Eigen::Vector3d ang_vel_FLU(  odometry_msg_.twist.twist.angular.x,
  //                               odometry_msg_.twist.twist.angular.y,
  //                               odometry_msg_.twist.twist.angular.z);

  px4_visual_odometry_msg_.LOCAL_FRAME_NED;

  Eigen::Quaterniond q_aircraft_enu = transform_orientation(q_enu, StaticTF::AIRCRAFT_TO_BASELINK);
  Eigen::Quaterniond q_aircraft_ned = transform_orientation(q_aircraft_enu, StaticTF::ENU_TO_NED);

  Eigen::Vector3d pos_ned = transform_static_frame(pos_enu, StaticTF::ENU_TO_NED);
  Eigen::Vector3d vel_ned = transform_static_frame(vel_enu, StaticTF::ENU_TO_NED);

  px4_visual_odometry_msg_.x = pos_ned.x();
  px4_visual_odometry_msg_.y = pos_ned.y();
  px4_visual_odometry_msg_.z = pos_ned.z();

  px4_visual_odometry_msg_.vx = vel_ned.x();
  px4_visual_odometry_msg_.vy = vel_ned.y();
  px4_visual_odometry_msg_.vz = vel_ned.z();

  px4_visual_odometry_msg_.q[0] = q_aircraft_ned.w();
  px4_visual_odometry_msg_.q[1] = q_aircraft_ned.x();
  px4_visual_odometry_msg_.q[2] = q_aircraft_ned.y();
  px4_visual_odometry_msg_.q[3] = q_aircraft_ned.z();

  // TODO CHECK THIS ORIENTATION FRAMES

  // MINUS SIGN FOR CHANGING FLU TO FRD
  px4_visual_odometry_msg_.rollspeed = odometry_msg_.twist.twist.angular.x;
  px4_visual_odometry_msg_.pitchspeed = -odometry_msg_.twist.twist.angular.y;
  px4_visual_odometry_msg_.yawspeed = -odometry_msg_.twist.twist.angular.z;

  px4_visual_odometry_msg_.timestamp = timestamp_.load();
  px4_visual_odometry_pub_->publish(px4_visual_odometry_msg_);
}

/** -----------------------------------------------------------------*/
/** ---------------------- SUBSCRIBER CALLBACKS ---------------------*/
/** -----------------------------------------------------------------*/

void PixhawkPlatform::px4imuCallback(const px4_msgs::msg::SensorCombined::SharedPtr msg)
{
  imu_msg_.header.frame_id = "imu";
  imu_msg_.linear_acceleration.x = msg->accelerometer_m_s2[0];
  imu_msg_.linear_acceleration.y = msg->accelerometer_m_s2[1];
  imu_msg_.linear_acceleration.z = msg->accelerometer_m_s2[2];
  imu_msg_.angular_velocity.x = msg->gyro_rad[0];
  imu_msg_.angular_velocity.y = msg->gyro_rad[1];
  imu_msg_.angular_velocity.z = msg->gyro_rad[2];
}

void PixhawkPlatform::px4odometryCallback(const px4_msgs::msg::VehicleOdometry::SharedPtr msg)
{
  using namespace px4_ros_com::frame_transforms;
  Eigen::Quaterniond q_aircraft(msg->q[0], msg->q[1], msg->q[2], msg->q[3]);

  Eigen::Vector3d pos_ned(msg->x, msg->y, msg->z);
  Eigen::Vector3d vel_ned(msg->vx, msg->vy, msg->vz);
  Eigen::Vector3d angular_speed_ned(msg->rollspeed, msg->pitchspeed, msg->yawspeed);

  px4_odometry_msg_.header.frame_id = "odom";

  Eigen::Quaterniond q_ned = transform_orientation(q_aircraft, StaticTF::AIRCRAFT_TO_BASELINK);
  Eigen::Quaterniond q_enu = transform_orientation(q_ned, StaticTF::NED_TO_ENU);

  Eigen::Vector3d pos_enu = transform_static_frame(pos_ned, StaticTF::NED_TO_ENU);
  Eigen::Vector3d vel_enu = transform_static_frame(vel_ned, StaticTF::NED_TO_ENU);
  Eigen::Vector3d angular_speed_enu =
    transform_static_frame(angular_speed_ned, StaticTF::NED_TO_ENU);

  // double roll, pitch, yaw;
  // utils::quaternion::quaternion_to_euler(q_enu,roll,pitch,yaw);
  // std::cout<< "roll: " << roll*(180.0f/M_PI) << "\npitch: " << pitch*(180.0f/M_PI) << "\nyaw: " << yaw*(180.0f/M_PI) << std::endl;

  px4_odometry_msg_.pose.pose.position.x = pos_enu[0];
  px4_odometry_msg_.pose.pose.position.y = pos_enu[1];
  px4_odometry_msg_.pose.pose.position.z = pos_enu[2];

  px4_odometry_msg_.pose.pose.orientation.w = q_enu.w();
  px4_odometry_msg_.pose.pose.orientation.x = q_enu.x();
  px4_odometry_msg_.pose.pose.orientation.y = q_enu.y();
  px4_odometry_msg_.pose.pose.orientation.z = q_enu.z();

  px4_odometry_msg_.twist.twist.linear.x = vel_enu[0];
  px4_odometry_msg_.twist.twist.linear.y = vel_enu[1];
  px4_odometry_msg_.twist.twist.linear.z = vel_enu[2];

  // TODO CHECK THIS ORIENTATION FRAMES
  px4_odometry_msg_.twist.twist.angular.x = angular_speed_enu[0];
  px4_odometry_msg_.twist.twist.angular.y = angular_speed_enu[1];
  px4_odometry_msg_.twist.twist.angular.z = angular_speed_enu[2];
}

void PixhawkPlatform::px4VehicleControlModeCallback(
  const px4_msgs::msg::VehicleControlMode::SharedPtr msg)
{
  static bool last_arm_state = msg->flag_armed;
  static bool last_offboard_state = msg->flag_control_offboard_enabled;

  this->platform_info_msg_.armed = msg->flag_armed;
  this->platform_info_msg_.offboard = msg->flag_control_offboard_enabled;

  if (this->platform_info_msg_.offboard != last_offboard_state) {
    if (this->platform_info_msg_.offboard)
      RCLCPP_INFO(this->get_logger(), "OFFBOARD_ENABLED");
    else
      RCLCPP_INFO(this->get_logger(), "OFFBOARD_DISABLED");
    last_offboard_state = this->platform_info_msg_.offboard;
  }

  if (this->platform_info_msg_.armed != last_arm_state) {
    if (this->platform_info_msg_.armed) {
      RCLCPP_INFO(this->get_logger(), "ARMING");
      this->handleStateMachineEvent(as2_msgs::msg::PlatformStateMachineEvent::ARM);
    } else {
      RCLCPP_INFO(this->get_logger(), "DISARMING");
      this->handleStateMachineEvent(as2_msgs::msg::PlatformStateMachineEvent::DISARM);
    }
    last_arm_state = this->platform_info_msg_.armed;
  }
}

void PixhawkPlatform::gpsCallback(const px4_msgs::msg::SensorGps::SharedPtr msg)
{
  // Reference:
  // https://github.com/PX4/PX4-Autopilot/blob/052adfbfd977abfad5b6f58d5404bba7dd209736/src/modules/mavlink/streams/GPS_RAW_INT.hpp#L58
  // https://github.com/mavlink/mavros/blob/b392c23add8781a67ac90915278fe41086fecaeb/mavros/src/plugins/global_position.cpp#L161

  nav_sat_fix_msg_.header.frame_id = "wgs84";
  if (msg->fix_type > 2) {  // At least 3D position
    nav_sat_fix_msg_.status.status = sensor_msgs::msg::NavSatStatus::STATUS_FIX;
  } else { 
    nav_sat_fix_msg_.status.status = sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX;
  }
  nav_sat_fix_msg_.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS;  // DEFAULT
  nav_sat_fix_msg_.latitude = msg->lat;
  nav_sat_fix_msg_.longitude = msg->lon;
  nav_sat_fix_msg_.altitude = msg->alt_ellipsoid;

  if (!std::isnan(msg->eph) && !std::isnan(msg->epv)) {
    // Position uncertainty --> Diagonal known
    nav_sat_fix_msg_.position_covariance.fill(0.0);
    nav_sat_fix_msg_.position_covariance[0] = std::pow(msg->eph, 2);
    nav_sat_fix_msg_.position_covariance[4] = std::pow(msg->eph, 2);
    nav_sat_fix_msg_.position_covariance[8] = std::pow(msg->epv, 2);
    nav_sat_fix_msg_.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;
  } else { 
    // UNKOWN
    nav_sat_fix_msg_.position_covariance = {-1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    nav_sat_fix_msg_.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
  }

}