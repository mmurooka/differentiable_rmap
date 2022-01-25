/* Author: Masaki Murooka */

#include <rosbag/bag.h>
#include <optmotiongen_msgs/RobotStateArray.h>
#include <differentiable_rmap/RmapSampleSet.h>

#include <optmotiongen/Utils/RosUtils.h>

#include <differentiable_rmap/RmapSampling.h>

using namespace DiffRmap;


template <SamplingSpace SamplingSpaceType>
RmapSampling<SamplingSpaceType>::RmapSampling(
    const std::shared_ptr<OmgCore::Robot>& rb)
{
  // Setup robot
  rb_arr_.push_back(rb);
  rb_arr_.setup();
  rbc_arr_ = OmgCore::RobotConfigArray(rb_arr_);

  // Setup ROS
  rs_arr_pub_ = nh_.advertise<optmotiongen_msgs::RobotStateArray>("robot_state_arr", 1, true);
  reachable_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud>("reachable_cloud", 1, true);
  unreachable_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud>("unreachable_cloud", 1, true);
}

template <SamplingSpace SamplingSpaceType>
RmapSampling<SamplingSpaceType>::RmapSampling(
    const std::shared_ptr<OmgCore::Robot>& rb,
    const std::string& body_name,
    const std::vector<std::string>& joint_name_list):
    RmapSampling(rb)
{
  // Setup body and joint
  body_name_ = body_name;
  body_idx_ = rb->bodyIndexByName(body_name_);
  joint_name_list_ = joint_name_list;
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::configure(const mc_rtc::Configuration& mc_rtc_config)
{
  mc_rtc_config_ = mc_rtc_config;
  config_.load(mc_rtc_config);
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::run(
    const std::string& bag_path,
    int sample_num,
    double sleep_rate)
{
  setupSampling();

  sample_list_.resize(sample_num);
  reachability_list_.resize(sample_num);
  reachable_cloud_msg_.points.clear();
  unreachable_cloud_msg_.points.clear();

  ros::Rate rate(sleep_rate > 0 ? sleep_rate : 1000);
  int loop_idx = 0;
  while (ros::ok()) {
    if (loop_idx == sample_num) {
      break;
    }

    // Sample once
    sampleOnce(loop_idx);

    if(loop_idx % config_.publish_loop_interval == 0) {
      publish();
    }

    if (sleep_rate > 0) {
      rate.sleep();
    }
    ros::spinOnce();
    loop_idx++;
  }

  // Dump sample set
  dumpSampleSet(bag_path);
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::setupSampling()
{
  // Set robot root pose
  rb_arr_[0]->rootPose(config_.root_pose);

  // Calculate coefficient and offset to make random position
  joint_idx_list_.resize(joint_name_list_.size());
  joint_pos_coeff_.resize(joint_name_list_.size());
  joint_pos_offset_.resize(joint_name_list_.size());
  {
    for (size_t i = 0; i < joint_name_list_.size(); i++) {
      const auto& joint_name = joint_name_list_[i];
      joint_idx_list_[i] = rb_arr_[0]->jointIndexByName(joint_name);
      double lower_joint_pos = rb_arr_[0]->limits_.lower.at(joint_name)[0];
      double upper_joint_pos = rb_arr_[0]->limits_.upper.at(joint_name)[0];
      joint_pos_coeff_[i] = (upper_joint_pos - lower_joint_pos) / 2;
      joint_pos_offset_[i] = (upper_joint_pos + lower_joint_pos) / 2;
    }
  }
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::sampleOnce(int sample_idx)
{
  const auto& rb = rb_arr_[0];
  const auto& rbc = rbc_arr_[0];

  // Set random configuration
  Eigen::VectorXd joint_pos =
      joint_pos_coeff_.cwiseProduct(Eigen::VectorXd::Random(joint_name_list_.size())) + joint_pos_offset_;
  for (size_t i = 0; i < joint_name_list_.size(); i++) {
    rbc->q[joint_idx_list_[i]][0] = joint_pos[i];
  }
  rbd::forwardKinematics(*rb, *rbc);

  // Append new sample to sample list
  const auto& body_pose = config_.body_pose_offset * rbc->bodyPosW[body_idx_];
  const SampleType& sample = poseToSample<SamplingSpaceType>(body_pose);
  sample_list_[sample_idx] = sample;
  reachability_list_[sample_idx] = true;
  reachable_cloud_msg_.points.push_back(OmgCore::toPoint32Msg(sampleToCloudPos<SamplingSpaceType>(sample)));
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::publish()
{
  // Publish robot
  rs_arr_pub_.publish(rb_arr_.makeRobotStateArrayMsg(rbc_arr_));

  // Publish cloud
  const auto& time_now = ros::Time::now();
  reachable_cloud_msg_.header.frame_id = "world";
  reachable_cloud_msg_.header.stamp = time_now;
  reachable_cloud_pub_.publish(reachable_cloud_msg_);
  unreachable_cloud_msg_.header.frame_id = "world";
  unreachable_cloud_msg_.header.stamp = time_now;
  unreachable_cloud_pub_.publish(unreachable_cloud_msg_);
}

template <SamplingSpace SamplingSpaceType>
void RmapSampling<SamplingSpaceType>::dumpSampleSet(const std::string& bag_path) const
{
  differentiable_rmap::RmapSampleSet sample_set_msg;
  sample_set_msg.type = static_cast<size_t>(SamplingSpaceType);
  sample_set_msg.samples.resize(sample_list_.size());

  SampleType sample_min = SampleType::Constant(1e10);
  SampleType sample_max = SampleType::Constant(-1e10);

  // Since libsvm considers the first class to be positive,
  // add the reachable sample from the beginning and the unreachable sample from the end.
  size_t reachable_idx = 0;
  size_t unreachable_idx = 0;
  for (size_t i = 0; i < sample_list_.size(); i++) {
    const SampleType& sample = sample_list_[i];

    // Get msg_idx according to sample reachability
    size_t msg_idx;
    if (reachability_list_[i]) {
      msg_idx = reachable_idx;
      reachable_idx++;
    } else {
      msg_idx = sample_list_.size() - 1 - unreachable_idx;
      unreachable_idx++;
    }

    // Set sample to message
    sample_set_msg.samples[msg_idx].position.resize(sample_dim_);
    for (int j = 0; j < sample_dim_; j++) {
      sample_set_msg.samples[msg_idx].position[j] = sample[j];
    }
    sample_set_msg.samples[msg_idx].is_reachable = reachability_list_[i];

    // Update min/max samples
    sample_min = sample_min.cwiseMin(sample);
    sample_max = sample_max.cwiseMax(sample);
  }

  // Set min/max samples to message
  sample_set_msg.min.resize(sample_dim_);
  sample_set_msg.max.resize(sample_dim_);
  for (int i = 0; i < sample_dim_; i++) {
    sample_set_msg.min[i] = sample_min[i];
    sample_set_msg.max[i] = sample_max[i];
  }

  // Dump to ROS bag
  rosbag::Bag bag(bag_path, rosbag::bagmode::Write);
  bag.write("/rmap_sample_set", ros::Time::now(), sample_set_msg);
  ROS_INFO_STREAM("Dump sample set to " << bag_path);
}

std::shared_ptr<RmapSamplingBase> DiffRmap::createRmapSampling(
    SamplingSpace sampling_space,
    const std::shared_ptr<OmgCore::Robot>& rb,
    const std::string& body_name,
    const std::vector<std::string>& joint_name_list)
{
  if (sampling_space == SamplingSpace::R2) {
    return std::make_shared<RmapSampling<SamplingSpace::R2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SO2) {
    return std::make_shared<RmapSampling<SamplingSpace::SO2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SE2) {
    return std::make_shared<RmapSampling<SamplingSpace::SE2>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::R3) {
    return std::make_shared<RmapSampling<SamplingSpace::R3>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SO3) {
    return std::make_shared<RmapSampling<SamplingSpace::SO3>>(rb, body_name, joint_name_list);
  } else if (sampling_space == SamplingSpace::SE3) {
    return std::make_shared<RmapSampling<SamplingSpace::SE3>>(rb, body_name, joint_name_list);
  } else {
    mc_rtc::log::error_and_throw<std::runtime_error>(
        "[createRmapSampling] Unsupported SamplingSpace: {}", std::to_string(sampling_space));
  }
}

// Declare template specialized class
// See https://stackoverflow.com/a/8752879
template class RmapSampling<SamplingSpace::R2>;
template class RmapSampling<SamplingSpace::SO2>;
template class RmapSampling<SamplingSpace::SE2>;
template class RmapSampling<SamplingSpace::R3>;
template class RmapSampling<SamplingSpace::SO3>;
template class RmapSampling<SamplingSpace::SE3>;
