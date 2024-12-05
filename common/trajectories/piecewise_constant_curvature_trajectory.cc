#include "drake/common/trajectories/piecewise_constant_curvature_trajectory.h"

#include <functional>
#include <limits>

namespace drake {
namespace trajectories {

template <typename T>
PiecewiseConstantCurvatureTrajectory<T>::PiecewiseConstantCurvatureTrajectory(
    const std::vector<T>& breaks, const std::vector<T>& turning_rates,
    const Vector3<T>& initial_curve_tangent, const Vector3<T>& plane_normal,
    const Vector3<T>& initial_position)
    : PiecewiseTrajectory<T>(breaks),
      segment_turning_rates_(turning_rates),
      segment_start_poses_(MakeSegmentStartPoses(
          MakeBasePose(initial_curve_tangent, plane_normal, initial_position),
          breaks, turning_rates)) {
  if (turning_rates.size() != breaks.size() - 1) {
    throw std::invalid_argument(
        "The number of turning rates must be equal to the number of segments.");
  }
  if (breaks[0] != 0) {
    throw std::invalid_argument(
        "The first break must be 0, as the breaks are in arclength units.");
  }
}

template <typename T>
std::unique_ptr<Trajectory<T>> PiecewiseConstantCurvatureTrajectory<T>::Clone()
    const {
  auto base_pose = get_base_pose();
  auto base_frame = base_pose.rotation();
  return std::make_unique<PiecewiseConstantCurvatureTrajectory<T>>(
      this->breaks(), segment_turning_rates_,
      base_frame.col(kCurveTangentIndex), base_frame.col(kPlaneNormalIndex),
      base_pose.translation());
}

template <typename T>
math::RigidTransform<T> PiecewiseConstantCurvatureTrajectory<T>::GetPose(
    const T& s) const {
  int segment_index = this->get_segment_index(s);
  const math::RigidTransform<T> X_FiF =
      CalcRelativePoseInSegment(segment_turning_rates_[segment_index],
                                s - this->start_time(segment_index));
  return segment_start_poses_[segment_index] * X_FiF;
}

template <typename T>
multibody::SpatialVelocity<T>
PiecewiseConstantCurvatureTrajectory<T>::CalcSpatialVelocity(
    const T& s, const T& s_dot) const {
  const math::RotationMatrix<T> R_AF = GetPose(s).rotation();
  const T& k_i = segment_turning_rates_[this->get_segment_index(s)];

  multibody::SpatialVelocity<T> spatial_velocity;
  // From Frenet–Serret analysis and the class doc for
  // PiecewiseConstantCurvatureTrajectory, the rotational velocity is equal to
  // the Darboux vector times the tangential velocity: kᵢ * Fz * ds/dt.
  spatial_velocity.rotational() = k_i * R_AF.col(kPlaneNormalIndex) * s_dot;

  // The translational velocity is equal to the tangent direction times the
  // tangential velocity: t̂(s) * ds/dt = Fx(s) * ds/dt.
  spatial_velocity.translational() = R_AF.col(kCurveTangentIndex) * s_dot;

  return spatial_velocity;
}

template <typename T>
multibody::SpatialAcceleration<T>
PiecewiseConstantCurvatureTrajectory<T>::CalcSpatialAcceleration(
    const T& s, const T& s_dot, const T& s_ddot) const {
  const math::RotationMatrix<T> R_AF = GetPose(s).rotation();
  const T& k_i = segment_turning_rates_[this->get_segment_index(s)];

  multibody::SpatialAcceleration<T> spatial_acceleration;
  // The spatial acceleration is the time derivative of the spatial velocity.
  // We compute the acceleration by applying the chain rule to the formulas
  // in CalcSpatialVelocity.

  // The angular velocity is kᵢ * Fz * ds/dt. As Fz and kᵢ are constant
  // everywhere except the breaks, the angular acceleration is then equal to kᵢ
  // * Fz * d²s/dt².
  spatial_acceleration.rotational() =
      k_i * R_AF.col(kPlaneNormalIndex) * s_ddot;

  // The translational velocity is Fx(s) * ds/dt. Thus by product and chain
  // rule, the translational acceleration is:
  //    dFx(s)/ds * (ds/dt)² + F(x) * d²s/dt².
  // From the class doc, we know dFx/dt = kᵢ * Fy.
  // We also know that the translational velocity is Fx and thus the
  // translational acceleration is kᵢ * Fy * (ds/dt)² + Fx * d²s/dt².
  spatial_acceleration.translational() =
      k_i * R_AF.col(kCurveNormalIndex) * s_dot * s_dot +
      R_AF.col(kCurveTangentIndex) * s_ddot;
  return spatial_acceleration;
}

template <typename T>
boolean<T> PiecewiseConstantCurvatureTrajectory<T>::IsNearlyPeriodic(
    double tolerance) const {
  return GetPose(0.).IsNearlyEqualTo(GetPose(length()), tolerance);
}

template <typename T>
math::RigidTransform<T>
PiecewiseConstantCurvatureTrajectory<T>::CalcRelativePoseInSegment(
    const T& k_i, const T& ds) {
  Vector3<T> p_FioFo_Fi = Vector3<T>::Zero();
  // Calculate rotation angle
  const T theta = ds * k_i;
  math::RotationMatrix<T> R_FiF = math::RotationMatrix<T>::MakeZRotation(theta);
  if (k_i == T(0)) {
    // Case 1: zero curvature (straight line)
    //
    // The tangent axis is constant, thus the displacement p_FioFo_Fi is just
    // t̂_Fi * Δs.
    p_FioFo_Fi(kCurveTangentIndex) = ds;
  } else {
    // Case 2: non-zero curvature (circular arc)
    //
    // The entire trajectory lies in a plane with normal Fiz, and thus
    // the arc is embedded in the Fix-Fiy plane.
    //
    //     Fiy
    //      ↑
    //    C o             x
    //      │\            x
    //      │ \           x
    //      │  \         x
    //      │   \       x
    //      │    \     x
    //      │     \  xx
    //      │ p_Fo ox
    //      │   xxx
    //      └xxx───────────────→ Fix
    //     p_Fio
    //
    // The circular arc's centerpoint C is located at p_FioC = (1/kᵢ) * Fiy,
    // with initial direction Fix and radius abs(1/kᵢ). Thus the angle traveled
    // along the arc is θ = kᵢ * Δs, with the sign of kᵢ handling the
    // clockwise/counterclockwise direction. The kᵢ > 0 case is shown above.
    p_FioFo_Fi(kCurveNormalIndex) = (T(1) - cos(theta)) / k_i;
    p_FioFo_Fi(kCurveTangentIndex) = sin(theta) / k_i;
  }
  return math::RigidTransform<T>(R_FiF, p_FioFo_Fi);
}

template <typename T>
math::RigidTransform<T> PiecewiseConstantCurvatureTrajectory<T>::MakeBasePose(
    const Vector3<T>& initial_curve_tangent, const Vector3<T>& plane_normal,
    const Vector3<T>& initial_position) {
  const Vector3<T> initial_Fy_A = plane_normal.cross(initial_curve_tangent);

  auto base_frame = math::RotationMatrix<T>::MakeFromOrthonormalColumns(
      initial_curve_tangent.normalized(), initial_Fy_A.normalized(),
      plane_normal.normalized());
  return math::RigidTransform<T>(base_frame, initial_position);
}

template <typename T>
std::vector<math::RigidTransform<T>>
PiecewiseConstantCurvatureTrajectory<T>::MakeSegmentStartPoses(
    const math::RigidTransform<T>& base_pose, const std::vector<T>& breaks,
    const std::vector<T>& turning_rates) {
  const size_t num_breaks = breaks.size();
  const size_t num_segments = num_breaks - 1;

  // calculate angular change and length of each segment.
  const VectorX<T> breaks_eigen =
      Eigen::Map<const VectorX<T>>(breaks.data(), num_breaks);
  const VectorX<T> segment_durations =
      breaks_eigen.tail(num_segments) - breaks_eigen.head(num_segments);

  // build frames for the start of each segment.
  std::vector<math::RigidTransform<T>> segment_start_poses;
  segment_start_poses.reserve(num_segments);
  segment_start_poses.push_back(base_pose);

  for (size_t i = 0; i < (num_segments - 1); i++) {
    math::RigidTransform<T> X_FiFip1 =
        CalcRelativePoseInSegment(turning_rates[i], segment_durations[i]);
    segment_start_poses.push_back(segment_start_poses.back() * X_FiFip1);
  }

  return segment_start_poses;
}

// Explicit instantiation for the types specified in the header
DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class drake::trajectories::PiecewiseConstantCurvatureTrajectory);

}  // namespace trajectories
}  // namespace drake
