/*
 * Copyright (C) 2018-2019  DENSO WAVE INCORPORATED
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "denso_cobotta_gripper/denso_cobotta_gripper.h"
#include "denso_cobotta_driver/cobotta_common.h"

int main(int argc, char** argv)
{
  ros::init(argc, argv, "denso_cobotta_gripper");
  ros::NodeHandle nh;

  denso_cobotta_gripper::DensoCobottaGripper gripper;
  bool success = gripper.initialize(nh);

  if (!success)
  {
    ROS_ERROR_STREAM("Failed to initialize denso_cobotta_gripper.");
    return 1;
  }
  ROS_INFO_STREAM("Success to initialize denso_cobotta_gripper.");

  ros::Rate rate(1.0 / cobotta_common::getPeriod().toSec());
  ros::AsyncSpinner spinner(1);
  spinner.start();

  while (ros::ok())
  {
    success = gripper.Read();
    if (!success)
    {
      return 1;
    }

    if (!gripper.isMotorOn())
    {
      rate.sleep();
      continue;
    }

    success = gripper.Write();
    if (!success)
    {
      return 1;
    }
    ros::Duration(0.001).sleep();
  }
  spinner.stop();

  return 0;
}

namespace denso_cobotta_gripper
{
using namespace cobotta_common;

DensoCobottaGripper::DensoCobottaGripper() : current_position_(0.0), current_effort_(20.0)
{
}

DensoCobottaGripper::~DensoCobottaGripper()
{
}

bool DensoCobottaGripper::initialize(ros::NodeHandle& nh)
{
  // subscriber (Start until motor_on)
  motor_on_ = false;
  std::lock_guard<std::mutex> gripper_lock(gripper_mtx_);
  sub_robot_state_ = nh.subscribe("robot_state", 64, &DensoCobottaGripper::subRobotStateCB, this);

  // Open device file
  errno = 0;
  fd_ = open(PATH_DEVFILE.c_str(), O_RDWR);
  if (fd_ < 0)
  {
    ROS_ERROR("open(%s): %s", PATH_DEVFILE.c_str(), std::strerror(errno));
    return false;
  }

  // Publisher
  pub_joint_state_ = nh.advertise<sensor_msgs::JointState>("joint_states", 1);
  // Action server
  as_gripper_move_ = std::make_shared<actionlib::SimpleActionServer<GripperMoveAction> >(
      nh, "gripper_move", std::bind(&DensoCobottaGripper::gripperMoveActionCB, this, std::placeholders::_1), false);
  as_gripper_move_->registerPreemptCallback(std::bind(&DensoCobottaGripper::cancelCB, this));
  as_gripper_move_->start();

  as_gripper_cmd_ = std::make_shared<actionlib::SimpleActionServer<control_msgs::GripperCommandAction> >(
      nh, "gripper_action", std::bind(&DensoCobottaGripper::gripperCommandActionGoalCB, this, std::placeholders::_1),
      false);
  as_gripper_cmd_->registerPreemptCallback(std::bind(&DensoCobottaGripper::cancelCB, this));
  as_gripper_cmd_->start();

  bool success = getEncoderData();
  if (!success)
  {
    return false;
  }

  start_position_ = current_position_;
  current_target_position_ = start_position_;
  current_cmd_position_ = start_position_;
  current_cmd_velocity_ = 0.0;
  current_speed_ = 0.0;
  move_complete_ = true;

  return true;
}

bool DensoCobottaGripper::Read()
{
  return getEncoderData();
}

bool DensoCobottaGripper::Write()
{
  return setServoUpdateData();
}

bool DensoCobottaGripper::gripperMoveActionCB(const GripperMoveGoalConstPtr& goal)
{
  GripperMoveResult result;
  double target_position = goal->target_position, speed_ = goal->speed, effort_ = goal->effort;
  bool success = gripperMove(target_position, speed_, effort_);

  while (!move_complete_)
  {
    // Wait until move is complete.
  }
  // set the action state to succeeded
  if (as_gripper_move_->isActive())
  {
    result.success = success;
    as_gripper_move_->setSucceeded(result);
  }
  else
  {
    return false;
  }

  return success;
}

void DensoCobottaGripper::cancelCB()
{
  // set the action state to succeeded
  if (as_gripper_move_->isActive())
  {
    as_gripper_move_->setPreempted();
  }
  else if (as_gripper_cmd_->isActive())
  {
    as_gripper_cmd_->setPreempted();
  }

  gripperStop();
  return;
}

void DensoCobottaGripper::actionFeedback()
{
  GripperMoveFeedback feedback;
  feedback.current_position = current_position_;
  as_gripper_move_->publishFeedback(feedback);
  return;
}

bool DensoCobottaGripper::gripperMove(const double& target_position, const double& speed, const double& effort)
{
  if ((target_position < MIN_POSITION) || (target_position > MAX_POSITION))
  {
    ROS_ERROR("DensoCobottaGripper: target_position is out of range(%lf). Min is %lf. Max is %lf.", target_position,
              MIN_POSITION, MAX_POSITION);
    return false;
  }

  if ((speed < MIN_SPEED) || (speed > MAX_SPEED))
  {
    ROS_ERROR("DensoCobottaGripper: speed is out of range(%lf). Min is %lf. Max is %lf.", speed, MIN_SPEED, MAX_SPEED);
    return false;
  }

  if ((effort < MIN_EFFORT) || (effort > MAX_EFFORT))
  {
    ROS_ERROR("DensoCobottaGripper: effort is out of range(%lf). Min is %lf. Max is %lf.", effort, MIN_EFFORT,
              MAX_EFFORT);
    return false;
  }

  // Set command parameters.
  {
    std::lock_guard<std::mutex> gripper_lock(gripper_mtx_);
    start_position_ = current_position_;
    current_speed_ = speed;
    current_effort_ = effort;
    current_cmd_position_ = start_position_;
    current_target_position_ = target_position;
    move_complete_ = false;
  }

  return true;
}

bool DensoCobottaGripper::gripperStop()
{
  current_speed_ = 0.0;
  move_complete_ = true;
  return true;
}

bool DensoCobottaGripper::gripperCurPos(double& current_position)
{
  current_position = current_position_;
  return true;
}

bool DensoCobottaGripper::gripperBusyState(bool& state)
{
  state = !move_complete_;
  return true;
}

bool DensoCobottaGripper::setGripperCommand()
{
  if (gripper_mtx_.try_lock())
  {
    const double vel_max = MAX_VELOCITY * current_speed_ * 0.01;
    const double direction = ((current_target_position_ - start_position_) >= 0.0) ? 1.0 : -1.0;
    const double cmd_vel_increment = MAX_ACCELERATION * getPeriod().toSec();
    const double slowdown_length = std::pow(current_cmd_velocity_, 2) * 0.5 / MAX_ACCELERATION;
    const double position_error = std::abs(current_target_position_ - current_cmd_position_);
    double target_velocity;
    if (position_error >= slowdown_length)
    {
      target_velocity = direction * vel_max;
    }
    else
    {
      target_velocity = direction * cmd_vel_increment;
    }

    if (current_cmd_velocity_ < target_velocity)
    {
      current_cmd_velocity_ += cmd_vel_increment;
      if (current_cmd_velocity_ > target_velocity)
      {
        current_cmd_velocity_ = target_velocity;
      }
    }
    else if (current_cmd_velocity_ > target_velocity)
    {
      current_cmd_velocity_ -= cmd_vel_increment;
      if (current_cmd_velocity_ < target_velocity)
      {
        current_cmd_velocity_ = target_velocity;
      }
    }

    current_cmd_position_ += (current_cmd_velocity_ * getPeriod().toSec());
    if (current_cmd_velocity_ != target_velocity)
    {
      if (current_cmd_velocity_ <= target_velocity)
      {
        current_cmd_position_ -= cmd_vel_increment * getPeriod().toSec() * 0.5;
      }
      else
      {
        current_cmd_position_ += cmd_vel_increment * getPeriod().toSec() * 0.5;
      }
    }

    if (((direction >= 0.0) && (current_cmd_position_ >= current_target_position_)) ||
        ((direction < 0.0) && (current_cmd_position_ <= current_target_position_)))
    {
      // Reached the target position.
      // Stop moving.
      current_cmd_velocity_ = 0.0;
      current_cmd_position_ = current_target_position_;
      move_complete_ = true;
    }
    else
    {
      // Keep moving.
      move_complete_ = false;
    }
    gripper_mtx_.unlock();
  }

  return true;
}

bool DensoCobottaGripper::setServoUpdateData()
{
  setGripperCommand();

  // ArmNo: 0 => J1 to J6.
  // ArmNo: 1 => Gripper
  static IOCTL_DATA_UPDATE servo_upd_data_;
  servo_upd_data_.Send.ArmNo = 1;
  servo_upd_data_.Send.Discontinuous = 0;
  servo_upd_data_.Send.DisableCurLim = 0;
  servo_upd_data_.Send.StayHere = 0;
  servo_upd_data_.Send.Position[0] = COEFF_OUTPOS_TO_PULSE * current_cmd_position_;
  servo_upd_data_.Send.CurrentLimit[0] = COEFF_EFFORT_TO_TORQUE * current_effort_ * 1000;
  servo_upd_data_.Send.CurrentOffset[0] = 0;

  errno = 0;
  int ret = ioctl(fd_, COBOTTA_IOCTL_SRV_UPDATE, &servo_upd_data_);
  if (ret != 0)
  {
    ROS_ERROR("ioctl(SRV_UPDATE): %s (ret=%d errno=%d"
              " ArmNo=1 Discontinuous=0 DisableCurLim=0 )",
              std::strerror(errno), ret, errno);
    return false;
  }

  if (servo_upd_data_.Recv.Result == 0)
  {
    // Servo updating with no delay.
  }
  else if (servo_upd_data_.Recv.Result == 0x0F408101)
  {
    // The current number of commands in buffer is 11.
    // To avoid vuffer overflow, sleep 8msec.
    ros::Duration(0.008).sleep();
  }
  else
  {
    ROS_ERROR_THROTTLE(1,
                       "DensoCobottaGripper: Command buffer overflow or other errors."
                       " (res=0x%08x state=0x%04x num=%d ArmNo=1)",
                       servo_upd_data_.Recv.Result, (servo_upd_data_.Recv.BuffState & 0xffff0000) >> 16,
                       (servo_upd_data_.Recv.BuffState & 0x0ffff));
    return false;
  }

  return true;
}

bool DensoCobottaGripper::getEncoderData()
{
  // Arm:0 => J1 to J6.
  // Arm:1 => Gripper.
  static IOCTL_DATA_GETENC encoder_data_;
  encoder_data_.Arm = 1;
  errno = 0;
  int ret = ioctl(fd_, COBOTTA_IOCTL_SRV_GETENC, &encoder_data_);
  if (ret != 0)
  {
    ROS_ERROR("ioctl(SRV_GETENC): %s (ret=%d errno=%d)", std::strerror(errno), ret, errno);
    return false;
  }

  current_position_ = encoder_data_.Recv.Encoder[0] / COEFF_OUTPOS_TO_PULSE;

  // Publish joint data to joint state publisher.
  sensor_msgs::JointState gripper;
  gripper.header.stamp = ros::Time::now();
  gripper.name.resize(1);
  gripper.name[0] = "joint_gripper";
  gripper.position.resize(1);
  // As we use mimic joint, mutiply by 0.5 to get the intended width.
  gripper.position[0] = current_position_ * 0.5;
  pub_joint_state_.publish(gripper);

  return true;
}

bool DensoCobottaGripper::gripperCommandActionGoalCB(const control_msgs::GripperCommandGoalConstPtr& goal)
{
  double target_position = goal->command.position;
  double max_effort = goal->command.max_effort;
  double speed_rate = 20.0;

  bool success = gripperMove(target_position, speed_rate, max_effort);
  // set the action state to succeeded
  if (!success)
  {
    return false;
  }

  while (!move_complete_)
  {
    // Wait until move is complete.
  }
  control_msgs::GripperCommandResult result;
  // set the action state to succeeded
  if (as_gripper_cmd_->isActive())
  {
    result.reached_goal = true;
    as_gripper_cmd_->setSucceeded(result);
  }
  else
  {
    return false;
  }
  return true;
}

void DensoCobottaGripper::subRobotStateCB(const denso_cobotta_driver::RobotState& msg)
{
  switch (msg.state_code)
  {
    case 0x0f200201:  // motor on
      motor_on_ = true;
      ROS_DEBUG("DensoCobottaGripper: motor on");
      break;
    case 0x0f200202:  // motor off
      motor_on_ = false;
      ROS_DEBUG("DensoCobottaGripper: motor off");
      break;
  }
  ROS_DEBUG("DensoCobottaGripper: msg received. ArmNo=%d code=0x%08X sub=0x%08X", msg.arm_no, msg.state_code,
            msg.state_subcode);
}

// @return ture:on false:off
bool DensoCobottaGripper::isMotorOn(void)
{
  return motor_on_;
}

}  // namespace denso_cobotta_gripper
