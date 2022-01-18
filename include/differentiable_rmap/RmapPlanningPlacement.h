/* Author: Masaki Murooka */

#pragma once

#include <optmotiongen/Utils/RobotUtils.h>

#include <differentiable_rmap/RmapPlanning.h>


namespace DiffRmap
{
/** \brief Get sampling space of placement depending on sampling space of reaching
    \tparam SamplingSpaceType sampling space of reaching
*/
template <SamplingSpace SamplingSpaceType>
constexpr SamplingSpace placementSamplingSpace()
{
  return SamplingSpaceType;
}

/** \brief Class to plan manipulator placement based on differentiable reachability map.
    \tparam SamplingSpaceType sampling space of reaching
*/
template <SamplingSpace SamplingSpaceType>
class RmapPlanningPlacement: public RmapPlanning<SamplingSpaceType>
{
 public:
  /*! \brief Configuration. */
  struct Configuration: public RmapPlanning<SamplingSpaceType>::Configuration
  {
    //! Number of reaching points
    int reaching_num = 2;

    //! Regularization weight
    double reg_weight = 1e-6;

    //! QP objective weight for placement
    double placement_weight = 1e-3;

    //! QP objective weight for SVM inequality error
    double svm_ineq_weight = 1e6;

    //! Number of IK trial
    int ik_trial_num = 10;

    //! Number of IK loop
    int ik_loop_num = 50;

    //! Threshold of IK [m], [rad]
    double ik_error_thre = 1e-2;

    /*! \brief Load mc_rtc configuration. */
    inline void load(const mc_rtc::Configuration& mc_rtc_config)
    {
      RmapPlanning<SamplingSpaceType>::Configuration::load(mc_rtc_config);

      mc_rtc_config("reaching_num", reaching_num);
      mc_rtc_config("reg_weight", reg_weight);
      mc_rtc_config("placement_weight", placement_weight);
      mc_rtc_config("svm_ineq_weight", svm_ineq_weight);
      mc_rtc_config("ik_trial_num", ik_trial_num);
      mc_rtc_config("ik_loop_num", ik_loop_num);
      mc_rtc_config("ik_error_thre", ik_error_thre);
    }
  };

 public:
  /*! \brief Sampling space of placement. */
  static constexpr SamplingSpace PlacementSamplingSpaceType = placementSamplingSpace<SamplingSpaceType>();

  /*! \brief Dimension of sample of reaching. */
  static constexpr int sample_dim_ = sampleDim<SamplingSpaceType>();

  /*! \brief Dimension of SVM input of reaching. */
  static constexpr int input_dim_ = inputDim<SamplingSpaceType>();

  /*! \brief Dimension of velocity of reaching. */
  static constexpr int vel_dim_ = velDim<SamplingSpaceType>();

  /*! \brief Dimension of sample of placement. */
  static constexpr int placement_sample_dim_ = sampleDim<PlacementSamplingSpaceType>();

  /*! \brief Dimension of SVM input of placement. */
  static constexpr int placement_input_dim_ = inputDim<PlacementSamplingSpaceType>();

  /*! \brief Dimension of velocity of placement. */
  static constexpr int placement_vel_dim_ = velDim<PlacementSamplingSpaceType>();

 public:
  /*! \brief Type of sample vector of reaching. */
  using SampleType = Sample<SamplingSpaceType>;

  /*! \brief Type of input vector of reaching. */
  using InputType = Input<SamplingSpaceType>;

  /*! \brief Type of velocity vector of reaching. */
  using VelType = Vel<SamplingSpaceType>;

  /*! \brief Type of sample vector of placement. */
  using PlacementSampleType = Sample<PlacementSamplingSpaceType>;

  /*! \brief Type of input vector of placement. */
  using PlacementInputType = Input<PlacementSamplingSpaceType>;

  /*! \brief Type of velocity vector of placement. */
  using PlacementVelType = Vel<PlacementSamplingSpaceType>;

 public:
  /** \brief Constructor.
      \param svm_path path of SVM model file
      \param bag_path path of ROS bag file of grid set (empty for no grid set)
   */
  RmapPlanningPlacement(const std::string& svm_path = "/tmp/rmap_svm_model.libsvm",
                        const std::string& bag_path = "/tmp/rmap_grid_set.bag");

  /** \brief Destructor. */
  ~RmapPlanningPlacement();

  /** \brief Configure from mc_rtc configuration.
      \param mc_rtc_config mc_rtc configuration
   */
  virtual void configure(const mc_rtc::Configuration& mc_rtc_config) override;

  /** \brief Setup planning. */
  virtual void setup() override;

  /** \brief Run planning once.
      \param publish whether to publish message
   */
  virtual void runOnce(bool publish) override;

 protected:
  /** \brief Publish marker array. */
  virtual void publishMarkerArray() const override;

  /** \brief Publish current state. */
  virtual void publishCurrentState() const override;

  /** \brief Transform topic callback. */
  virtual void transCallback(const geometry_msgs::TransformStamped::ConstPtr& trans_st_msg) override;

  /** \brief Solve IK.
      \param rb robot
      \param body_name name of body to reach
      \param joint_name_list name list of joints whose position is changed
   */
  void solveIK(const std::shared_ptr<OmgCore::Robot>& rb,
               const std::string& body_name,
               const std::vector<std::string>& joint_name_list);

 protected:
  //! Sample of reaching corresponding to identity pose
  static inline const SampleType identity_sample_ = poseToSample<SamplingSpaceType>(sva::PTransformd::Identity());

  //! Sample of placement corresponding to identity pose
  static inline const PlacementSampleType identity_placement_sample_ =
      poseToSample<PlacementSamplingSpaceType>(sva::PTransformd::Identity());

 protected:
  //! Configuration
  Configuration config_;

  //! Current sample of placement
  PlacementSampleType current_placement_sample_ = identity_placement_sample_;

  //! Target sample of placement
  PlacementSampleType target_placement_sample_ = identity_placement_sample_;

  //! Current sample list of reaching
  std::vector<SampleType> current_reaching_sample_list_;

  //! Target sample list of reaching
  std::vector<SampleType> target_reaching_sample_list_;

  //! ROS related members
  ros::Publisher current_pose_arr_pub_;
  ros::Publisher rs_arr_pub_;

 protected:
  // See https://stackoverflow.com/a/6592617
  using RmapPlanning<SamplingSpaceType>::mc_rtc_config_;

  using RmapPlanning<SamplingSpaceType>::sample_min_;
  using RmapPlanning<SamplingSpaceType>::sample_max_;

  using RmapPlanning<SamplingSpaceType>::svm_mo_;

  using RmapPlanning<SamplingSpaceType>::qp_coeff_;
  using RmapPlanning<SamplingSpaceType>::qp_solver_;

  using RmapPlanning<SamplingSpaceType>::target_sample_;

  using RmapPlanning<SamplingSpaceType>::svm_coeff_vec_;
  using RmapPlanning<SamplingSpaceType>::svm_sv_mat_;

  using RmapPlanning<SamplingSpaceType>::grid_set_msg_;

  using RmapPlanning<SamplingSpaceType>::nh_;

  using RmapPlanning<SamplingSpaceType>::marker_arr_pub_;
  using RmapPlanning<SamplingSpaceType>::current_pose_pub_;
};

/** \brief Create RmapPlanningPlacement instance.
    \param sampling_space sampling space
    \param svm_path path of SVM model file
    \param bag_path path of ROS bag file of grid set (empty for no grid set)
*/
std::shared_ptr<RmapPlanningBase> createRmapPlanningPlacement(
    SamplingSpace sampling_space,
    const std::string& svm_path = "/tmp/rmap_svm_model.libsvm",
    const std::string& bag_path = "/tmp/rmap_grid_set.bag");
}
