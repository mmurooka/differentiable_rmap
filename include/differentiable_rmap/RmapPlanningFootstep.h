/* Author: Masaki Murooka */

#pragma once

#include <differentiable_rmap/RmapPlanning.h>


namespace DiffRmap
{
/** \brief Class to plan footstep sequence based on differentiable reachability map.
    \tparam SamplingSpaceType sampling space
*/
template <SamplingSpace SamplingSpaceType>
class RmapPlanningFootstep: public RmapPlanning<SamplingSpaceType>
{
 public:
  /*! \brief Configuration. */
  struct Configuration: public RmapPlanning<SamplingSpaceType>::Configuration
  {
    //! Number of footsteps
    int footstep_num = 3;

    /*! \brief Load mc_rtc configuration. */
    inline void load(const mc_rtc::Configuration& mc_rtc_config)
    {
      RmapPlanning<SamplingSpaceType>::Configuration::load(mc_rtc_config);

      mc_rtc_config("footstep_num", footstep_num);
    }
  };

 public:
  /*! \brief Dimension of sample. */
  static constexpr int sample_dim_ = sampleDim<SamplingSpaceType>();

  /*! \brief Dimension of SVM input. */
  static constexpr int input_dim_ = inputDim<SamplingSpaceType>();

  /*! \brief Dimension of velocity. */
  static constexpr int vel_dim_ = velDim<SamplingSpaceType>();

 public:
  /*! \brief Type of sample vector. */
  using SampleType = Sample<SamplingSpaceType>;

  /*! \brief Type of input vector. */
  using InputType = Input<SamplingSpaceType>;

  /*! \brief Type of velocity vector. */
  using VelType = Vel<SamplingSpaceType>;

 public:
  /** \brief Constructor.
      \param svm_path path of SVM model file
      \param bag_path path of ROS bag file of grid set (empty for no grid set)
   */
  RmapPlanningFootstep(const std::string& svm_path = "/tmp/rmap_svm_model.libsvm",
                       const std::string& bag_path = "/tmp/rmap_grid_set.bag");

  /** \brief Destructor. */
  ~RmapPlanningFootstep();

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

 protected:
  //! Configuration
  Configuration config_;

  //! Current sample sequence
  std::vector<SampleType> current_sample_seq_;

  //! ROS related members
  ros::Publisher current_pose_arr_pub_;

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
};

/** \brief Create RmapPlanningFootstep instance.
    \param sampling_space sampling space
    \param svm_path path of SVM model file
    \param bag_path path of ROS bag file of grid set (empty for no grid set)
*/
std::shared_ptr<RmapPlanningBase> createRmapPlanningFootstep(
    SamplingSpace sampling_space,
    const std::string& svm_path = "/tmp/rmap_svm_model.libsvm",
    const std::string& bag_path = "/tmp/rmap_grid_set.bag");
}