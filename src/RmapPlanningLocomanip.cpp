/* Author: Masaki Murooka */

#include <chrono>

#include <mc_rtc/constants.h>

#include <geometry_msgs/PoseArray.h>
#include <sensor_msgs/PointCloud.h>
#include <visualization_msgs/MarkerArray.h>
#include <jsk_recognition_msgs/PolygonArray.h>

#include <optmotiongen/Utils/RosUtils.h>

#include <differentiable_rmap/RmapPlanningLocomanip.h>
#include <differentiable_rmap/SVMUtils.h>
#include <differentiable_rmap/GridUtils.h>
#include <differentiable_rmap/libsvm_hotfix.h>

using namespace DiffRmap;


RmapPlanningLocomanip::RmapPlanningLocomanip(
    const std::unordered_map<Limb, std::string>& svm_path_list,
    const std::unordered_map<Limb, std::string>& bag_path_list)
{
  // Setup ROS
  trans_sub_ = nh_.subscribe(
      "interactive_marker_transform",
      100,
      &RmapPlanningLocomanip::transCallback,
      this);
  marker_arr_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("marker_arr", 1, true);
  current_pose_arr_pub_ = nh_.template advertise<geometry_msgs::PoseArray>(
      "current_pose_arr", 1, true);
  current_poly_arr_pub_ = nh_.template advertise<jsk_recognition_msgs::PolygonArray>(
      "current_poly_arr", 1, true);
  current_left_poly_arr_pub_ = nh_.template advertise<jsk_recognition_msgs::PolygonArray>(
      "current_left_poly_arr", 1, true);
  current_right_poly_arr_pub_ = nh_.template advertise<jsk_recognition_msgs::PolygonArray>(
      "current_right_poly_arr", 1, true);
  current_cloud_pub_ = nh_.template advertise<sensor_msgs::PointCloud>(
      "current_cloud", 1, true);

  rmap_planning_list_[Limb::LeftFoot] =
      std::make_shared<RmapPlanning<SamplingSpaceType>>(
          svm_path_list.at(Limb::LeftFoot),
          bag_path_list.at(Limb::LeftFoot),
          false);
  rmap_planning_list_[Limb::RightFoot] =
      std::make_shared<RmapPlanning<SamplingSpaceType>>(
          svm_path_list.at(Limb::RightFoot),
          bag_path_list.at(Limb::RightFoot),
          false);
  rmap_planning_list_[Limb::LeftHand] =
      std::make_shared<RmapPlanning<SamplingSpaceType>>(
          svm_path_list.at(Limb::LeftHand),
          bag_path_list.at(Limb::LeftHand),
          false);
}

RmapPlanningLocomanip::~RmapPlanningLocomanip()
{
}

void RmapPlanningLocomanip::configure(const mc_rtc::Configuration& mc_rtc_config)
{
  mc_rtc_config_ = mc_rtc_config;
  config_.load(mc_rtc_config);
}

void RmapPlanningLocomanip::setup()
{
  // Setup dimensions
  config_dim_ = 2 * config_.motion_len * vel_dim_;
  svm_ineq_dim_ = 3 * config_.motion_len - 1;
  collision_ineq_dim_ = 0;
  hand_start_config_idx_ = config_.motion_len * vel_dim_;

  // Setup QP coefficients and solver
  // Introduce variables for inequality constraint errors
  qp_coeff_.setup(
      config_dim_ + svm_ineq_dim_ + collision_ineq_dim_,
      0,
      svm_ineq_dim_ + collision_ineq_dim_);
  qp_coeff_.x_min_.head(config_dim_).setConstant(-config_.delta_config_limit);
  qp_coeff_.x_max_.head(config_dim_).setConstant(config_.delta_config_limit);
  qp_coeff_.x_min_.tail(svm_ineq_dim_ + collision_ineq_dim_).setConstant(-1e10);
  qp_coeff_.x_max_.tail(svm_ineq_dim_ + collision_ineq_dim_).setConstant(1e10);

  qp_solver_ = OmgCore::allocateQpSolver(OmgCore::QpSolverType::JRLQP);

  // Setup current sample sequence
  for (const Limb& limb : Limbs::all) {
    start_sample_list_.emplace(
        limb,
        poseToSample<SamplingSpaceType>(
            config_.initial_sample_pose_list.count(limb) > 0 ?
            config_.initial_sample_pose_list.at(limb) :
            sva::PTransformd::Identity()));
  }
  current_foot_sample_seq_.resize(config_.motion_len);
  current_hand_sample_seq_.resize(config_.motion_len);
  for (int i = 0; i < config_.motion_len; i++) {
    current_foot_sample_seq_[i] = start_sample_list_.at(i % 2 == 0 ? Limb::LeftFoot : Limb::RightFoot);
    current_hand_sample_seq_[i] = start_sample_list_.at(Limb::LeftHand);
  }

  // Setup adjacent regularization
  adjacent_reg_mat_.setZero(config_dim_, config_dim_);
  //// Set for adjacent foot
  for (int i = 0; i < config_.motion_len; i++) {
    adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
        i * vel_dim_, i * vel_dim_).diagonal().setConstant(
            (i == config_.motion_len - 1 ? 1 : 2) * config_.adjacent_reg_weight);
    if (i != config_.motion_len - 1) {
      adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
          (i + 1) * vel_dim_, i * vel_dim_).diagonal().setConstant(
              -config_.adjacent_reg_weight);
      adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
          i * vel_dim_, (i + 1) * vel_dim_).diagonal().setConstant(
              -config_.adjacent_reg_weight);
    }
  }
  //// Set for adjacent hand
  for (int i = 0; i < config_.motion_len; i++) {
    adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
        hand_start_config_idx_ + i * vel_dim_, hand_start_config_idx_ + i * vel_dim_).diagonal().setConstant(
            (i == config_.motion_len - 1 ? 1 : 2) * config_.adjacent_reg_weight);
    if (i != config_.motion_len - 1) {
      adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
          hand_start_config_idx_ + (i + 1) * vel_dim_, hand_start_config_idx_ + i * vel_dim_).diagonal().setConstant(
              -config_.adjacent_reg_weight);
      adjacent_reg_mat_.block<vel_dim_, vel_dim_>(
          hand_start_config_idx_ + i * vel_dim_, hand_start_config_idx_ + (i + 1) * vel_dim_).diagonal().setConstant(
              -config_.adjacent_reg_weight);
    }
  }
  // ROS_INFO_STREAM("adjacent_reg_mat_:\n" << adjacent_reg_mat_);
}

void RmapPlanningLocomanip::runOnce(bool publish)
{
  // Set QP objective matrices
  qp_coeff_.obj_mat_.setZero();
  qp_coeff_.obj_vec_.setZero();
  const VelType& target_sample_error =
      sampleError<SamplingSpaceType>(target_hand_sample_, current_hand_sample_seq_.back());
  qp_coeff_.obj_mat_.diagonal().template segment<vel_dim_>(config_dim_ - vel_dim_).setConstant(1.0);
  qp_coeff_.obj_mat_.diagonal().head(config_dim_).array() +=
      target_sample_error.squaredNorm() + config_.reg_weight;
  qp_coeff_.obj_mat_.diagonal().tail(svm_ineq_dim_ + collision_ineq_dim_).head(
      svm_ineq_dim_).setConstant(config_.svm_ineq_weight);
  // qp_coeff_.obj_mat_.diagonal().tail(svm_ineq_dim_ + collision_ineq_dim_).tail(
  //     collision_ineq_dim_).setConstant(config_.collision_ineq_weight);
  qp_coeff_.obj_vec_.template segment<vel_dim_>(config_dim_ - vel_dim_) = target_sample_error;
  Eigen::VectorXd current_config(config_dim_);
  // This implementation of adjacent regularization is not exact because the error between samples is not a simple subtraction
  for (int i = 0; i < config_.motion_len; i++) {
    current_config.template segment<vel_dim_>(i * vel_dim_) =
        sampleError<SamplingSpaceType>(identity_sample_, current_foot_sample_seq_[i]);
    current_config.template segment<vel_dim_>(hand_start_config_idx_ + i * vel_dim_) =
        sampleError<SamplingSpaceType>(identity_sample_, current_hand_sample_seq_[i]);
  }
  // ROS_INFO_STREAM("current_config:\n" << current_config.transpose());
  qp_coeff_.obj_vec_.head(config_dim_) += adjacent_reg_mat_ * current_config;
  qp_coeff_.obj_vec_.head(vel_dim_) -=
      config_.adjacent_reg_weight * sampleError<SamplingSpaceType>(
          identity_sample_, start_sample_list_.at(Limb::LeftFoot));
  qp_coeff_.obj_vec_.segment(hand_start_config_idx_, vel_dim_) -=
      config_.adjacent_reg_weight * sampleError<SamplingSpaceType>(
          identity_sample_, start_sample_list_.at(Limb::LeftHand));
  qp_coeff_.obj_mat_.topLeftCorner(config_dim_, config_dim_) += adjacent_reg_mat_;

  // Set QP inequality matrices of reachability
  qp_coeff_.ineq_mat_.setZero();
  qp_coeff_.ineq_vec_.setZero();
  //// Set for reachability between foot
  for (int i = 0; i < config_.motion_len; i++) {
    const SampleType& pre_foot_sample =
        i == 0 ? start_sample_list_.at(Limb::RightFoot) : current_foot_sample_seq_[i - 1];
    const SampleType& suc_foot_sample = current_foot_sample_seq_[i];
    std::shared_ptr<RmapPlanning<SamplingSpaceType>> rmap_planning =
        rmapPlanning(i % 2 == 0 ? Limb::LeftFoot : Limb::RightFoot);

    const SampleType& rel_sample =
        relSample<SamplingSpaceType>(pre_foot_sample, suc_foot_sample);
    const VelType& rel_svm_grad = rmap_planning->calcSVMGrad(rel_sample);
    if (i > 0) {
      qp_coeff_.ineq_mat_.template block<1, vel_dim_>(i, (i - 1) * vel_dim_) =
          -1 * rel_svm_grad.transpose() *
          relVelToVelMat<SamplingSpaceType>(pre_foot_sample, suc_foot_sample, false);
    }
    qp_coeff_.ineq_mat_.template block<1, vel_dim_>(i, i * vel_dim_) =
        -1 * rel_svm_grad.transpose() *
        relVelToVelMat<SamplingSpaceType>(pre_foot_sample, suc_foot_sample, true);
    qp_coeff_.ineq_vec_.template segment<1>(i) <<
        rmap_planning->calcSVMValue(rel_sample) - config_.svm_thre;
  }
  // //// Set for reachability from foot to hand
  // for (int i = 0; i < config_.motion_len; i++) {
  //   int start_ineq_idx = config_.motion_len - 1 + 4 * i - 1;
  //   const SampleType& pre1_foot_sample = current_foot_sample_seq_[2 * i];
  //   const SampleType& suc1_foot_sample = current_foot_sample_seq_[2 * i + 1];
  //   const SampleType& suc2_foot_sample = current_foot_sample_seq_[2 * i + 2];
  //   const SampleType& hand_sample = current_hand_sample_seq_[i];
  //   std::shared_ptr<RmapPlanning<SamplingSpaceType>> rmap_planning = rmapPlanning(Limb::LeftHand);

  //   if (i != 0) {
  //     const SampleType& pre2_foot_sample = current_foot_sample_seq_[2 * i - 1];
  //     const SampleType& pre12_foot_sample =
  //         midSample<SamplingSpaceType>(pre1_foot_sample, pre2_foot_sample);
  //     const SampleType& pre12_rel_sample =
  //         relSampleHandFromFoot(pre12_foot_sample, hand_sample, config_.waist_height);
  //     const VelType& pre12_rel_svm_grad = rmap_planning->calcSVMGrad(pre12_rel_sample);
  //     // The implementation of gradient of mean sample is not exact because the mean of two samples is not a simple arithmetic mean
  //     Eigen::MatrixXd pre12_foot_ineq_mat =
  //         -1 * pre12_rel_svm_grad.transpose() * relSampleGradHandFromFoot(pre12_foot_sample, hand_sample, false) / 2;
  //     qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 0, (2 * i - 1) * vel_dim_) =
  //         pre12_foot_ineq_mat;
  //     qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 0, (2 * i) * vel_dim_) =
  //         pre12_foot_ineq_mat;
  //     qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 0, hand_start_config_idx_ + i * vel_dim_) =
  //         -1 * pre12_rel_svm_grad.transpose() *
  //         relSampleGradHandFromFoot(pre12_foot_sample, hand_sample, true);
  //     qp_coeff_.ineq_vec_.template segment<1>(start_ineq_idx + 0) <<
  //         rmap_planning->calcSVMValue(pre12_rel_sample) - config_.svm_thre;
  //   }

  //   const SampleType& pre1_rel_sample =
  //       relSampleHandFromFoot(pre1_foot_sample, hand_sample, config_.waist_height);
  //   const VelType& pre1_rel_svm_grad = rmap_planning->calcSVMGrad(pre1_rel_sample);
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 1, (2 * i) * vel_dim_) =
  //       -1 * pre1_rel_svm_grad.transpose() *
  //       relSampleGradHandFromFoot(pre1_foot_sample, hand_sample, false);
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 1, hand_start_config_idx_ + i * vel_dim_) =
  //       -1 * pre1_rel_svm_grad.transpose() *
  //       relSampleGradHandFromFoot(pre1_foot_sample, hand_sample, true);
  //   qp_coeff_.ineq_vec_.template segment<1>(start_ineq_idx + 1) <<
  //       rmap_planning->calcSVMValue(pre1_rel_sample) - config_.svm_thre;

  //   const SampleType& suc1_rel_sample =
  //       relSampleHandFromFoot(suc1_foot_sample, hand_sample, config_.waist_height);
  //   const VelType& suc1_rel_svm_grad = rmap_planning->calcSVMGrad(suc1_rel_sample);
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 2, (2 * i + 1) * vel_dim_) =
  //       -1 * suc1_rel_svm_grad.transpose() *
  //       relSampleGradHandFromFoot(suc1_foot_sample, hand_sample, false);
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 2, hand_start_config_idx_ + i * vel_dim_) =
  //       -1 * suc1_rel_svm_grad.transpose() *
  //       relSampleGradHandFromFoot(suc1_foot_sample, hand_sample, true);
  //   qp_coeff_.ineq_vec_.template segment<1>(start_ineq_idx + 2) <<
  //       rmap_planning->calcSVMValue(suc1_rel_sample) - config_.svm_thre;

  //   const SampleType& suc12_foot_sample =
  //       midSample<SamplingSpaceType>(suc1_foot_sample, suc2_foot_sample);
  //   const SampleType& suc12_rel_sample =
  //       relSampleHandFromFoot(suc12_foot_sample, hand_sample, config_.waist_height);
  //   const VelType& suc12_rel_svm_grad = rmap_planning->calcSVMGrad(suc12_rel_sample);
  //   Eigen::MatrixXd suc12_foot_ineq_mat =
  //       -1 * suc12_rel_svm_grad.transpose() * relSampleGradHandFromFoot(suc12_foot_sample, hand_sample, false) / 2;
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 3, (2 * i + 1) * vel_dim_) =
  //       suc12_foot_ineq_mat;
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 3, (2 * i + 2) * vel_dim_) =
  //       suc12_foot_ineq_mat;
  //   qp_coeff_.ineq_mat_.template block<1, vel_dim_>(start_ineq_idx + 3, hand_start_config_idx_ + i * vel_dim_) =
  //       -1 * suc12_rel_svm_grad.transpose() *
  //       relSampleGradHandFromFoot(suc12_foot_sample, hand_sample, true);
  //   qp_coeff_.ineq_vec_.template segment<1>(start_ineq_idx + 3) <<
  //       rmap_planning->calcSVMValue(suc12_rel_sample) - config_.svm_thre;
  // }
  qp_coeff_.ineq_mat_.rightCols(
      svm_ineq_dim_ + collision_ineq_dim_).diagonal().head(svm_ineq_dim_).setConstant(-1);

  // ROS_INFO_STREAM("qp_coeff_.obj_mat_:\n" << qp_coeff_.obj_mat_);
  // ROS_INFO_STREAM("qp_coeff_.obj_vec_:\n" << qp_coeff_.obj_vec_.transpose());
  // ROS_INFO_STREAM("qp_coeff_.ineq_mat_:\n" << qp_coeff_.ineq_mat_);
  // ROS_INFO_STREAM("qp_coeff_.ineq_vec_:\n" << qp_coeff_.ineq_vec_.transpose());

  // Solve QP
  Eigen::VectorXd vel_all = qp_solver_->solve(qp_coeff_);
  if (qp_solver_->solve_failed_) {
    vel_all.setZero();
  }

  // Integrate
  for (int i = 0; i < config_.motion_len; i++) {
    integrateVelToSample<SamplingSpaceType>(
        current_foot_sample_seq_[i], vel_all.template segment<vel_dim_>(i * vel_dim_));
    integrateVelToSample<SamplingSpaceType>(
        current_hand_sample_seq_[i], vel_all.template segment<vel_dim_>(hand_start_config_idx_ + i * vel_dim_));
  }

  if (publish) {
    // Publish
    publishMarkerArray();
    publishCurrentState();
  }
}

void RmapPlanningLocomanip::runLoop()
{
  setup();

  ros::Rate rate(config_.loop_rate);
  int loop_idx = 0;
  while (ros::ok()) {
    runOnce(loop_idx % config_.publish_interval == 0);

    rate.sleep();
    ros::spinOnce();
    loop_idx++;
  }
}

void RmapPlanningLocomanip::publishMarkerArray() const
{
  std_msgs::Header header_msg;
  header_msg.frame_id = "world";
  header_msg.stamp = ros::Time::now();

  // Instantiate marker array
  visualization_msgs::MarkerArray marker_arr_msg;

  // Delete marker
  visualization_msgs::Marker del_marker;
  del_marker.action = visualization_msgs::Marker::DELETEALL;
  del_marker.header = header_msg;
  del_marker.id = marker_arr_msg.markers.size();
  marker_arr_msg.markers.push_back(del_marker);

  // Foot reachable grids marker
  {
    visualization_msgs::Marker grids_marker;
    grids_marker.header = header_msg;
    grids_marker.type = visualization_msgs::Marker::CUBE_LIST;

    for (int i = 0; i < config_.motion_len; i++) {
      std::shared_ptr<RmapPlanning<SamplingSpaceType>> rmap_planning =
          i % 2 == 0 ? rmapPlanning(Limb::LeftFoot) : rmapPlanning(Limb::RightFoot);
      const SampleType& sample_min = rmap_planning->sample_min_;
      const SampleType& sample_max = rmap_planning->sample_max_;
      const SampleType& sample_range = sample_max - sample_min;
      const auto& grid_set_msg = rmap_planning->grid_set_msg_;

      grids_marker.ns = "foot_reachable_grids_" + std::to_string(i);
      grids_marker.id = marker_arr_msg.markers.size();
      grids_marker.scale = OmgCore::toVector3Msg(
          calcGridCubeScale<SamplingSpaceType>(grid_set_msg->divide_nums, sample_range));
      grids_marker.scale.z = 0.01;
      const SampleType& pre_sample =
          i == 0 ? start_sample_list_.at(Limb::RightFoot) : current_foot_sample_seq_[i - 1];
      grids_marker.pose = OmgCore::toPoseMsg(sampleToPose<SamplingSpaceType>(pre_sample));
      grids_marker.color = OmgCore::toColorRGBAMsg(
          i % 2 == 0 ? std::array<double, 4>{0.8, 0.0, 0.0, 0.3} : std::array<double, 4>{0.0, 0.8, 0.0, 0.3});
      const SampleType& slice_sample =
          relSample<SamplingSpaceType>(pre_sample, current_foot_sample_seq_[i]);
      GridIdxsType<SamplingSpaceType> slice_divide_idxs;
      gridDivideRatiosToIdxs(
          slice_divide_idxs,
          (slice_sample - sample_min).array() / sample_range.array(),
          grid_set_msg->divide_nums);
      grids_marker.points.clear();
      loopGrid<SamplingSpaceType>(
          grid_set_msg->divide_nums,
          sample_min,
          sample_range,
          [&](int grid_idx, const SampleType& sample) {
            if (grid_set_msg->values[grid_idx] > config_.svm_thre) {
              Eigen::Vector3d pos = sampleToCloudPos<SamplingSpaceType>(sample);
              pos.z() = 0;
              grids_marker.points.push_back(OmgCore::toPointMsg(pos));
            }
          },
          std::vector<int>{0, 1},
          slice_divide_idxs);
      marker_arr_msg.markers.push_back(grids_marker);
    }
  }

  // Hand reachable grids marker
  // {
  //   std::shared_ptr<RmapPlanning<SamplingSpaceType>> rmap_planning = rmapPlanning(Limb::LeftHand);
  //   const SampleType& sample_min = rmap_planning->sample_min_;
  //   const SampleType& sample_max = rmap_planning->sample_max_;
  //   const SampleType& sample_range = sample_max - sample_min;
  //   const auto& grid_set_msg = rmap_planning->grid_set_msg_;

  //   visualization_msgs::Marker grids_marker;
  //   grids_marker.header = header_msg;
  //   grids_marker.type = visualization_msgs::Marker::CUBE_LIST;
  //   grids_marker.color = OmgCore::toColorRGBAMsg({0.0, 0.0, 0.8, 0.1});
  //   grids_marker.scale = OmgCore::toVector3Msg(
  //       calcGridCubeScale<SamplingSpaceType>(grid_set_msg->divide_nums, sample_range));
  //   loopGrid<SamplingSpaceType>(
  //       grid_set_msg->divide_nums,
  //       sample_min,
  //       sample_range,
  //       [&](int grid_idx, const SampleType& sample) {
  //         if (grid_set_msg->values[grid_idx] > config_.svm_thre) {
  //           grids_marker.points.push_back(
  //               OmgCore::toPointMsg(sampleToCloudPos<SamplingSpaceType>(sample)));
  //         }
  //       });
  //   for (int i = 0; i < config_.motion_len - 1; i++) {
  //     // Publish only the grid set at the timing of hand transition
  //     if (i % 2 == 0) {
  //       continue;
  //     }
  //     grids_marker.ns = "hand_reachable_grids_" + std::to_string(i);
  //     sva::PTransformd pose = sampleToPose<SamplingSpaceType>(
  //         midSample<SamplingSpaceType>(current_foot_sample_seq_[i], current_foot_sample_seq_[i + 1]));
  //     pose.translation().z() = config_.waist_height;
  //     grids_marker.pose = OmgCore::toPoseMsg(pose);
  //     grids_marker.id = marker_arr_msg.markers.size();
  //     marker_arr_msg.markers.push_back(grids_marker);
  //   }
  // }

  marker_arr_pub_.publish(marker_arr_msg);
}

void RmapPlanningLocomanip::publishCurrentState() const
{
  std_msgs::Header header_msg;
  header_msg.frame_id = "world";
  header_msg.stamp = ros::Time::now();

  // Publish pose array for foot and hand
  geometry_msgs::PoseArray pose_arr_msg;
  pose_arr_msg.header = header_msg;
  pose_arr_msg.poses.resize(2 * config_.motion_len);
  for (int i = 0; i < config_.motion_len; i++) {
    pose_arr_msg.poses[i] =
        OmgCore::toPoseMsg(sampleToPose<SamplingSpaceType>(current_foot_sample_seq_[i]));
    pose_arr_msg.poses[config_.motion_len + i] =
        OmgCore::toPoseMsg(sampleToPose<SamplingSpaceType>(current_hand_sample_seq_[i]));
  }
  current_pose_arr_pub_.publish(pose_arr_msg);

  // Publish polygon array for foot
  jsk_recognition_msgs::PolygonArray poly_arr_msg;
  jsk_recognition_msgs::PolygonArray left_poly_arr_msg;
  jsk_recognition_msgs::PolygonArray right_poly_arr_msg;
  poly_arr_msg.header = header_msg;
  left_poly_arr_msg.header = header_msg;
  right_poly_arr_msg.header = header_msg;
  poly_arr_msg.polygons.resize(config_.motion_len + 2);
  for (int i = 0; i < config_.motion_len + 2; i++) {
    poly_arr_msg.polygons[i].header = header_msg;
    sva::PTransformd foot_pose;
    if (i < config_.motion_len) {
      foot_pose = sampleToPose<SamplingSpaceType>(current_foot_sample_seq_[i]);
    } else {
      foot_pose = config_.initial_sample_pose_list.at(i % 2 == 0 ? Limb::LeftFoot : Limb::RightFoot);
    }
    poly_arr_msg.polygons[i].polygon.points.resize(config_.foot_vertices.size());
    for (size_t j = 0; j < config_.foot_vertices.size(); j++) {
      poly_arr_msg.polygons[i].polygon.points[j] =
          OmgCore::toPoint32Msg(foot_pose.rotation().transpose() * config_.foot_vertices[j] + foot_pose.translation());
    }
    if (i % 2 == 0) {
      left_poly_arr_msg.polygons.push_back(poly_arr_msg.polygons[i]);
    } else {
      right_poly_arr_msg.polygons.push_back(poly_arr_msg.polygons[i]);
    }
  }
  current_poly_arr_pub_.publish(poly_arr_msg);
  current_left_poly_arr_pub_.publish(left_poly_arr_msg);
  current_right_poly_arr_pub_.publish(right_poly_arr_msg);

  // // Publish cloud for hand
  // sensor_msgs::PointCloud cloud_msg;
  // cloud_msg.header = header_msg;
  // for (int i = 0; i < config_.motion_len; i++) {
  //   cloud_msg.points.push_back(OmgCore::toPoint32Msg(
  //       sampleToCloudPos<SamplingSpaceType>(current_hand_sample_seq_[i])));
  // }
  // current_cloud_pub_.publish(cloud_msg);
}

void RmapPlanningLocomanip::transCallback(
    const geometry_msgs::TransformStamped::ConstPtr& trans_st_msg)
{
  if (trans_st_msg->child_frame_id == "target") {
    target_hand_sample_ = poseToSample<SamplingSpaceType>(OmgCore::toSvaPTransform(trans_st_msg->transform));
  }
}