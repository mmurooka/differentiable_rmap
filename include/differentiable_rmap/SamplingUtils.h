/* Author: Masaki Murooka */

/** \file SamplingUtils.h
    Utilities for sampling.
 */

#pragma once

#include <stdexcept>
#include <string>

#include <mc_rtc/logging.h>
#include <SpaceVecAlg/SpaceVecAlg>

namespace DiffRmap
{
/** \brief Sampling space. */
enum class SamplingSpace
{
  R2 = 21,
  SO2 = 22,
  SE2 = 23,
  R3 = 31,
  SO3 = 32,
  SE3 = 33
};

/** \brief Get dimension of sample.
    \tparam SamplingSpaceType sampling space
*/
template<SamplingSpace SamplingSpaceType>
constexpr int sampleDim();

/*! \brief Type of sample vector. */
template<SamplingSpace SamplingSpaceType>
using Sample = Eigen::Matrix<double, sampleDim<SamplingSpaceType>(), 1>;

/** \brief Get dimension of SVM input.
    \tparam SamplingSpaceType sampling space
*/
template<SamplingSpace SamplingSpaceType>
constexpr int inputDim();

/*! \brief Type of SVM input vector. */
template<SamplingSpace SamplingSpaceType>
using Input = Eigen::Matrix<double, inputDim<SamplingSpaceType>(), 1>;

/** \brief Get dimension of sample velocity.
    \tparam SamplingSpaceType sampling space
*/
template<SamplingSpace SamplingSpaceType>
constexpr int velDim();

/*! \brief Type of sample vector. */
template<SamplingSpace SamplingSpaceType>
using Vel = Eigen::Matrix<double, velDim<SamplingSpaceType>(), 1>;

/** \brief Convert pose to sample.
    \tparam SamplingSpaceType sampling space
    \param[in] pose pose
    \return sample (fixed size Eigen::Vector)
 */
template<SamplingSpace SamplingSpaceType>
Sample<SamplingSpaceType> poseToSample(const sva::PTransformd & pose);

/** \brief Convert sample to pose.
    \tparam SamplingSpaceType sampling space
    \param[in] sample sample
    \return pose
 */
template<SamplingSpace SamplingSpaceType>
sva::PTransformd sampleToPose(const Sample<SamplingSpaceType> & sample);

/** \brief Convert sample to pointcloud position.
    \tparam SamplingSpaceType sampling space
    \param[in] sample sample
    \return pointcloud position (Eigen::Vector3d)
 */
template<SamplingSpace SamplingSpaceType>
Eigen::Vector3d sampleToCloudPos(const Sample<SamplingSpaceType> & sample);

/** \brief Convert sample to SVM input.
    \tparam SamplingSpaceType sampling space
    \param[in] sample sample
    \return SVM input (fixed size Eigen::Vector)
*/
template<SamplingSpace SamplingSpaceType>
Input<SamplingSpaceType> sampleToInput(const Sample<SamplingSpaceType> & sample);

/** \brief Convert SVM input to sample.
    \tparam SamplingSpaceType sampling space
    \param[in] input SVM input
    \return sample (fixed size Eigen::Vector)
*/
template<SamplingSpace SamplingSpaceType>
Sample<SamplingSpaceType> inputToSample(const Input<SamplingSpaceType> & input);

/** \brief Integrate velocity to sample. (duration is assumed to be one)
    \tparam SamplingSpaceType sampling space
    \param[in,out] sample sample
    \param[in] vel velocity
*/
template<SamplingSpace SamplingSpaceType>
void integrateVelToSample(Eigen::Ref<Sample<SamplingSpaceType>> sample, const Vel<SamplingSpaceType> & vel);

/** \brief Calculate error between two samples.
    \tparam SamplingSpaceType sampling space
    \param pre_sample predecessor sample
    \param suc_sample successor sample
    \return velocity corresponding to the error from pre_sample to suc_sample
*/
template<SamplingSpace SamplingSpaceType>
Vel<SamplingSpaceType> sampleError(const Sample<SamplingSpaceType> & pre_sample,
                                   const Sample<SamplingSpaceType> & suc_sample);

/** \brief Get random pose in sampling space.
    \tparam SamplingSpaceType sampling space
*/
template<SamplingSpace SamplingSpaceType>
sva::PTransformd getRandomPose();

/** \brief Convert string to sampling space. */
SamplingSpace strToSamplingSpace(const std::string & sampling_space_str);
} // namespace DiffRmap

namespace std
{
using DiffRmap::SamplingSpace;

inline string to_string(SamplingSpace sampling_space)
{
  if(sampling_space == SamplingSpace::R2)
  {
    return std::string("R2");
  }
  else if(sampling_space == SamplingSpace::SO2)
  {
    return std::string("SO2");
  }
  else if(sampling_space == SamplingSpace::SE2)
  {
    return std::string("SE2");
  }
  else if(sampling_space == SamplingSpace::R3)
  {
    return std::string("R3");
  }
  else if(sampling_space == SamplingSpace::SO3)
  {
    return std::string("SO3");
  }
  else if(sampling_space == SamplingSpace::SE3)
  {
    return std::string("SE3");
  }
  else
  {
    mc_rtc::log::error_and_throw<std::runtime_error>("[to_string] Unsupported SamplingSpace: {}",
                                                     static_cast<int>(sampling_space));
  }
}
} // namespace std

// See method 3 in https://www.codeproject.com/Articles/48575/How-to-Define-a-Template-Class-in-a-h-File-and-Imp
#include <differentiable_rmap/SamplingUtils.hpp>
