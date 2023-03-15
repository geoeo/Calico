#ifndef CALICO_SENSORS_GYROSCOPE_COST_FUNCTOR_H_
#define CALICO_SENSORS_GYROSCOPE_COST_FUNCTOR_H_

#include "calico/sensors/gyroscope_models.h"
#include "calico/trajectory.h"
#include "ceres/cost_function.h"


namespace calico::sensors {

// Enum listing the positions of parameters for a gyroscope cost function.
enum class GyroscopeParameterIndices : int {
  // Gyroscope intrinsics.
  kIntrinsicsIndex = 0,
  // Extrinsics parameters of the gyroscope relative to its sensor rig.
  kExtrinsicsRotationIndex = 1,
  kExtrinsicsTranslationIndex = 2,
  // Sensor latency.
  kLatencyIndex = 3,
  // Rotation and position control points of the entire trajectory spline as an
  // Nx6 matrix.
  kSensorRigPoseSplineControlPointsIndex = 7,
};

// Generic auto-differentiation gyroscope cost functor. Residuals will be based on
// how the gyroscope model is initialized.
class GyroscopeCostFunctor {
 public:
  static constexpr int kGyroscopeResidualSize = 3;
  explicit GyroscopeCostFunctor(
      GyroscopeIntrinsicsModel gyroscope_model,
      const Eigen::Vector3d& measurement, double stamp,
      const Trajectory& sp_T_world_sensorrig);

  // Convenience function for creating a gyroscope cost function.
  static ceres::CostFunction* CreateCostFunction(
      const Eigen::Vector3d& measurement,
      GyroscopeIntrinsicsModel gyroscope_model, Eigen::VectorXd& intrinsics,
      Pose3d& extrinsics, double& latency,
      Trajectory& trajectory_world_sensorrig, double stamp,
      std::vector<double*>& parameters);

  // Parameters to the cost function:
  //   intrinsics:
  //     All parameters in the intrinsics model as an Eigen column vector.
  //     Order of the parameters will need to be in agreement with the model
  //     being used.
  //   q_sensorrig_gyroscope:
  //     Rotation from sensorrig frame to gyroscope frame as a quaternion.
  //   t_sensorrig_gyroscope:
  //     Position of gyroscope relative to sensorrig origin resolved in the
  //     sensorrig frame.
  //   latency:
  //     Sensor latency in seconds.
  //   control_points:
  //     Control points for the entire pose trajectory.
  template <typename T>
    bool operator()(T const* const* parameters, T* residual) {
    // Parse intrinsics.
    const T* intrinsics_ptr =
      static_cast<const T*>(&(parameters[static_cast<int>(
          GyroscopeParameterIndices::kIntrinsicsIndex)][0]));
    const int parameter_size = gyroscope_model_->NumberOfParameters();
    const Eigen::VectorX<T> intrinsics = Eigen::Map<const Eigen::VectorX<T>>(
        intrinsics_ptr, parameter_size);
    // Parse extrinsics.
    const Eigen::Map<const Eigen::Quaternion<T>> q_sensorrig_gyroscope(
        &(parameters[static_cast<int>(
            GyroscopeParameterIndices::kExtrinsicsRotationIndex)][0]));
    const Eigen::Map<const Eigen::Vector3<T>> t_sensorrig_gyroscope(
        &(parameters[static_cast<int>(
            GyroscopeParameterIndices::kExtrinsicsTranslationIndex)][0]));
    // Parse latency.
    const T latency =
        parameters[static_cast<int>(
            GyroscopeParameterIndices::kLatencyIndex)][0];
    // Parse sensor rig spline resolved in the world frame.
    const int num_control_points =
        trajectory_evaluation_params_.num_control_points;
    const Eigen::Map<const Eigen::MatrixX<T>> all_control_points(
        &(parameters[static_cast<int>(
            GyroscopeParameterIndices::kSensorRigPoseSplineControlPointsIndex)]
          [0]), num_control_points, 6);
    const Eigen::Ref<const Eigen::MatrixX<T>> control_points =
        all_control_points.block(trajectory_evaluation_params_.spline_index, 0,
                                 Trajectory::kSplineOrder, 6);
    const Eigen::MatrixX<T> basis_matrix =
        trajectory_evaluation_params_.basis_matrix.template cast<T>();
    const T knot0 = static_cast<T>(trajectory_evaluation_params_.knot0);
    const T knot1 = static_cast<T>(trajectory_evaluation_params_.knot1);
    const T stamp =
        static_cast<T>(trajectory_evaluation_params_.stamp) - latency;
    // Evaluate the pose and pose rate.
    const Eigen::Vector<T, 6> pose_vector =
        BSpline<Trajectory::kSplineOrder, T>::Evaluate(
            control_points, knot0, knot1, basis_matrix, stamp,
            /*derivative=*/0);
    const Eigen::Vector<T, 6> pose_dot_vector =
        BSpline<Trajectory::kSplineOrder, T>::Evaluate(
            control_points, knot0, knot1, basis_matrix, stamp,
            /*derivative=*/1);
    // Compute the axis-angle manifold Jacobian (resolved in the sensor frame).
    const Eigen::Vector3<T> phi = -pose_vector.head(3);
    const T theta_sq = phi.squaredNorm();
    Eigen::Matrix3<T> J;
    J.setIdentity();
    if (theta_sq != T(0.0)) {
      const T theta = sqrt(theta_sq);
      const T theta_fo = theta_sq * theta_sq;
      T c1, c2;
      // If small angle, compute the first 3 terms of the Taylor expansion.
      if (abs(theta) < static_cast<T>(1e-7)) {
        c1 = static_cast<T>(0.5) - theta_sq * static_cast<T>(1.0 / 24.0) +
          theta_fo * static_cast<T>(1.0 / 720.0);
        c2 = static_cast<T>(1.0 / 6.0) - theta_sq * static_cast<T>(1.0 / 120.0) +
          theta_fo * static_cast<T>(1.0 / 5040.0);
      } else {
        const T inv_theta_sq = static_cast<T>(1.0) / theta_sq;
        c1 = (static_cast<T>(1.0) - cos(theta)) * inv_theta_sq;
        c2 = (static_cast<T>(1.0) - sin(theta) / theta) * inv_theta_sq;
      }
      Eigen::Matrix3<T> phi_x = skew(phi);
      J += c1 * phi_x + c2 * phi_x * phi_x;
    }
    // Compute the angular velocity of the sensor resolved in the sensor frame.
    // TODO(yangjames): Also evaluate acceleration for g-sensitivity
    //                  calculations.
    const Eigen::Vector3<T> phi_dot = -pose_dot_vector.head(3);
    const Eigen::Vector3<T> omega_sensor_world =
        q_sensorrig_gyroscope.inverse() * J * phi_dot;
    // Project the sensor angular velocity through the gyroscope model.
    const absl::StatusOr<Eigen::Vector3<T>> projection =
      gyroscope_model_->Project(intrinsics, omega_sensor_world);
    if (projection.ok()) {
      Eigen::Map<Eigen::Vector3<T>> error(residual);
      const Eigen::Vector3<T> measurement = measurement_.template cast<T>();
      error = measurement - *projection;
      return true;
    }
    return false;
  }

  template <typename T>
  Eigen::Matrix3<T> skew(const Eigen::Vector3<T>& v) {
    Eigen::Matrix3<T> V;
    V.setZero();
    V(0, 1) = -v.z();
    V(1, 0) = v.z();
    V(0, 2) = v.y();
    V(2, 0) = -v.y();
    V(1, 2) = -v.x();
    V(2, 1) = v.x();
    return V;
  }

 private:
  Eigen::Vector3d measurement_;
  std::unique_ptr<GyroscopeModel> gyroscope_model_;
  TrajectoryEvaluationParams trajectory_evaluation_params_;
};
} // namespace calico::sensors
#endif // CALICO_SENSORS_GYROSCOPE_COST_FUNCTOR_H_
