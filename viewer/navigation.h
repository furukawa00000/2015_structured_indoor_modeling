#ifndef NAVIGATION_H__
#define NAVIGATION_H__

#include <Eigen/Dense>
#include <vector>

#include "panorama_renderer.h"

enum CameraStatus {
  // CameraPanorama handles the states.
  kPanorama,
  kPanoramaTransition,
  // CameraAir handles the state.
  kAir,
  kAirTransition,
  // CameraBetweenGroundAndAir handles the state.
  kPanoramaToAir,
  kAirToPanorama
};

struct CameraPanorama {
  int start_index;
  Eigen::Vector3d start_center;
  Eigen::Vector3d start_direction;
  
  int end_index;
  Eigen::Vector3d end_center;
  Eigen::Vector3d end_direction;
  
  double progress;
};

struct CameraAir {
  Eigen::Vector3d ground_center;
  Eigen::Vector3d start_direction;
  Eigen::Vector3d end_direction;

  double progress;

  Eigen::Vector3d GetCenter() const {
    return ground_center - start_direction;
  }
};

struct CameraBetweenPanoramaAndAir {
  // "progress" is not used in camera_panorama and camera_air.
  CameraPanorama camera_panorama;
  CameraAir camera_air;
  double progress;  
};

class Navigation {
 public:
  Navigation(const std::vector<PanoramaRenderer>& panorama_renderers);

  // Get the current camera center.
  Eigen::Vector3d GetCenter() const;
  Eigen::Vector3d GetDirection() const;
  CameraStatus GetCameraStatus() const;
  CameraPanorama GetCameraPanorama() const;
  CameraAir GetCameraAir() const;
  CameraBetweenPanoramaAndAir GetCameraBetweenPanoramaAndAir() const;
  
  void Init();

  void Tick();
  
  void RotateOnGround(const Eigen::Vector3d& axis);
  void MoveForwardOnGround();
  void MoveBackwardOnGround();
  void RotateOnGround(const double radian);

  void PanoramaToAir();
  void AirToPanorama(const int panorama_index);

  double Progress() const;
  double GetFieldOfViewInDegrees() const;
  
 private:
  void MoveToPanorama(const int target_panorama_index);
  void AllocateResources();
  void FreeResources();
  void SetMatrices();
  // int FindPanoramaFromAirClick(const Eigen::Vector2d& pixel) const;
  
  // Camera is at (center) and looks along (direction).
  CameraStatus camera_status;
  
  CameraPanorama camera_panorama;
  CameraAir camera_air;
  CameraBetweenPanoramaAndAir camera_between_panorama_and_air;

  // Z coordinate of the camera in the air.
  double air_height;
  // Angle of viewing in the air.
  double air_angle;

  const std::vector<PanoramaRenderer>& panorama_renderers;
  
  // Screen dimension.
  int current_width;
  int current_height;
};

#endif  // NAVIGATION_H__
