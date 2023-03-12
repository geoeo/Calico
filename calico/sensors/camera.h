#ifndef CALICO_SENSORS_CAMERA_H_
#define CALICO_SENSORS_CAMERA_H_

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "calico/sensors/sensor_base.h"
#include "calico/sensors/camera_models.h"
#include "calico/trajectory.h"
#include "calico/typedefs.h"
#include "ceres/problem.h"
#include "Eigen/Dense"


namespace calico::sensors {

// ObservationId type for a camera measurement. This object is hashable by
// `absl::Hash` for use as a key in `absl::flat_hash_map` or
// `absl::flat_hash_set`.
struct ObservationId {
  double stamp;
  int image_id;
  int model_id;
  int feature_id;

  template <typename H>
  friend H AbslHashValue(H h, const ObservationId& id) {
    return H::combine(std::move(h), id.stamp, id.image_id, id.model_id,
                      id.feature_id);
  }
  friend bool operator==(const ObservationId& lhs, const ObservationId& rhs) {
    return (lhs.stamp == rhs.stamp &&
            lhs.image_id == rhs.image_id &&
            lhs.model_id == rhs.model_id &&
            lhs.feature_id == rhs.feature_id);
  }
};

// Camera measurement type.
struct CameraMeasurement {
  Eigen::Vector2d pixel;
  ObservationId id;
};

// Image size
struct ImageSize {
  int width;
  int height;
};

class Camera : public Sensor {
 public:
  explicit Camera() = default;
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;
  ~Camera() = default;

  // Add this camera's parameters to the ceres problem. Returns the number of
  // parameters added to the problem, which should be intrinsics + extrinsics.
  // If the camera model hasn't been set yet, it will return an invalid
  // argument error.
  absl::StatusOr<int> AddParametersToProblem(ceres::Problem& problem) final;

  // Contribue this camera's residuals to the ceres problem.
  absl::StatusOr<int> AddResidualsToProblem(
      ceres::Problem & problem,
      Trajectory& sensorrig_trajectory,
      WorldModel& world_model) final;

  // Compute the project of a world model through a kinematic chain. This
  // method returns only valid synthetic measurements as would be observed by
  // the actual sensor, complying with physicality such as features being in
  // front of the camera and within image bounds.
  absl::StatusOr<std::vector<CameraMeasurement>> Project(
      const std::vector<double>& interp_times,
      const Trajectory& sensorrig_trajectory,
      const WorldModel& world_model) const;

  // Compute the projection of a world model through a kinematic chain. This
  // method returns only valid synthetic measurements as would be observed by
  // the actual sensor, complying with physicality such as features being in
  // front of the camera and within image bounds.
  std::vector<CameraMeasurement> Project(
      const Trajectory& sensorrig_trajectory,
      const WorldModel& world_model) const;

  // Setter/getter for name.
  void SetName(absl::string_view name) final;
  const std::string& GetName() const final;

  // Setter/getter for extrinsics parameters.
  void SetExtrinsics(const Pose3d& T_sensorrig_sensor) final;
  const Pose3d& GetExtrinsics() const final;

  // Setter/getter for intrinsics parameters.
  absl::Status SetIntrinsics(const Eigen::VectorXd& intrinsics) final;
  const Eigen::VectorXd& GetIntrinsics() const final;

  // Enable flags for intrinsics and extrinsics.
  void EnableExtrinsicsParameters(bool enable) final;
  void EnableIntrinsicsParameters(bool enable) final;

  // Setter/getter for image size. This parameter gets used in the `Project`
  // method which generates synthetic measurements within the bounds of the
  // image dimensions if the 'apply_image_bounds` flag is set to true.
  absl::Status SetImageSize(const ImageSize& image_size);
  ImageSize GetImageSize() const;

  // Setter/getter for camera model.
  absl::Status SetModel(CameraIntrinsicsModel camera_model);
  CameraIntrinsicsModel GetModel() const;

  // Add a camera measurement to the measurement list. Returns an error if the
  // measurement's id is duplicated without adding.
  absl::Status AddMeasurement(const CameraMeasurement& measurement);

  // Add multiple measurements to the measurement list. Returns an error status
  // if any measurements are duplicates. This method will add the entire vector,
  // but skips any duplicates.
  absl::Status AddMeasurements(
      const std::vector<CameraMeasurement>& measurements);

  // Remove a measurement with a specific observation id. Returns an error if
  // the id was not associated with a measurement.
  absl::Status RemoveMeasurementById(const ObservationId& id);

  // Remove multiple measurements by their observation ids. Returns an error if
  // it attempts to remove an id that was not associated with a measurement.
  // This method will remove the entire vector, but skip invalid entries.
  absl::Status RemoveMeasurementsById(const std::vector<ObservationId>& ids);

  // Clear all measurements.
  void ClearMeasurements();

  // Get current number of measurements stored.
  int NumberOfMeasurements() const;

 private:
  std::string name_;
  bool intrinsics_enabled_;
  bool extrinsics_enabled_;
  ImageSize image_size_;
  std::unique_ptr<CameraModel> camera_model_;
  Pose3d T_sensorrig_sensor_;
  Eigen::VectorXd intrinsics_;
  absl::flat_hash_map<ObservationId, CameraMeasurement> id_to_measurement_;
};

} // namespace calico::sensors

#endif // CALICO_SENSORS_CAMERA_H_
