#include <fstream>
#include <list>
#include <opencv2/highgui/highgui.hpp>
#include "ceres/ceres.h"
#include "stitch_panorama.h"

using cv::imread;
using cv::imshow;
using cv::Mat;
using cv::Vec3b;
using cv::Vec4f;
using cv::waitKey;

using Eigen::Vector2d;
using Eigen::Vector2i;
using Eigen::Matrix3d;
using Eigen::Vector3d;
using Eigen::Vector3f;

using std::cerr;
using std::endl;
using std::flush;
using std::ifstream;
using std::list;
using std::make_pair;
using std::max;
using std::min;
using std::pair;
using std::string;
using std::vector;

namespace {

Vector3d Rodrigues(const Eigen::Matrix3d& matrix) {
  cv::Mat mat;
  mat.create(3, 3, CV_32F);

  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) {
      mat.at<float>(y, x) = matrix(y, x);
    }
  }

  cv::Vec3f vec;
  cv::Rodrigues(mat, vec);
  return Vector3d(vec(0), vec(1), vec(2));
}

Matrix3d Rodrigues(const Eigen::Vector3d& vect) {
  cv::Vec3f vec;
  for (int i = 0; i < 3; ++i)
    vec[i] = vect[i];

  cv::Mat mat;
  mat.create(3, 3, CV_32F);
  cv::Rodrigues(vec, mat);

  Matrix3d matrix;
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) {
      matrix(y, x) = mat.at<float>(y, x);
    }
  }

  return matrix;
}

cv::Vec3b Interpolate(const cv::Mat& image, const Eigen::Vector2d& pixel) {
  int x = (int)floor(pixel[0]);
  int y = (int)floor(pixel[1]);

  int x0 = cv::borderInterpolate(x,   image.cols, cv::BORDER_REFLECT_101);
  int x1 = cv::borderInterpolate(x+1, image.cols, cv::BORDER_REFLECT_101);
  int y0 = cv::borderInterpolate(y,   image.rows, cv::BORDER_REFLECT_101);
  int y1 = cv::borderInterpolate(y+1, image.rows, cv::BORDER_REFLECT_101);

  float a = pixel[0] - (float)x;
  float c = pixel[1] - (float)y;

  uchar b = (uchar)cvRound((image.at<Vec3b>(y0, x0)[0] * (1.f - a) +
                            image.at<Vec3b>(y0, x1)[0] * a) * (1.f - c) +
                           (image.at<Vec3b>(y1, x0)[0] * (1.f - a) +
                            image.at<Vec3b>(y1, x1)[0] * a) * c);
  uchar g = (uchar)cvRound((image.at<Vec3b>(y0, x0)[1] * (1.f - a) +
                            image.at<Vec3b>(y0, x1)[1] * a) * (1.f - c) +
                           (image.at<Vec3b>(y1, x0)[1] * (1.f - a) +
                            image.at<Vec3b>(y1, x1)[1] * a) * c);
  uchar r = (uchar)cvRound((image.at<Vec3b>(y0, x0)[2] * (1.f - a) +
                            image.at<Vec3b>(y0, x1)[2] * a) * (1.f - c) +
                           (image.at<Vec3b>(y1, x0)[2] * (1.f - a) +
                            image.at<Vec3b>(y1, x1)[2] * a) * c);

  return Vec3b(b, g, r);
}

Eigen::Vector3f InterpolateF(const cv::Mat& image, const Eigen::Vector2d& pixel) {
  int x = (int)floor(pixel[0]);
  int y = (int)floor(pixel[1]);

  int x0 = cv::borderInterpolate(x,   image.cols, cv::BORDER_REFLECT_101);
  int x1 = cv::borderInterpolate(x+1, image.cols, cv::BORDER_REFLECT_101);
  int y0 = cv::borderInterpolate(y,   image.rows, cv::BORDER_REFLECT_101);
  int y1 = cv::borderInterpolate(y+1, image.rows, cv::BORDER_REFLECT_101);

  float a = pixel[0] - (float)x;
  float c = pixel[1] - (float)y;

  Vector3f bgr;
  bgr[0] =
    (image.at<Vec3b>(y0, x0)[0] * (1.f - a) +
     image.at<Vec3b>(y0, x1)[0] * a) * (1.f - c) +
    (image.at<Vec3b>(y1, x0)[0] * (1.f - a) +
     image.at<Vec3b>(y1, x1)[0] * a) * c;
  bgr[1] =
    (image.at<Vec3b>(y0, x0)[1] * (1.f - a) +
     image.at<Vec3b>(y0, x1)[1] * a) * (1.f - c) +
    (image.at<Vec3b>(y1, x0)[1] * (1.f - a) +
     image.at<Vec3b>(y1, x1)[1] * a) * c;
  bgr[2] =
    (image.at<Vec3b>(y0, x0)[2] * (1.f - a) +
     image.at<Vec3b>(y0, x1)[2] * a) * (1.f - c) +
    (image.at<Vec3b>(y1, x0)[2] * (1.f - a) +
     image.at<Vec3b>(y1, x1)[2] * a) * c;

  return bgr;
}

Eigen::Matrix3d RotationX(const double radian) {
  Matrix3d rotation;
  rotation <<
    1, 0, 0,
    0, cos(radian), -sin(radian),
    0, sin(radian), cos(radian);

  return rotation;
}

Eigen::Matrix3d RotationY(const double radian) {
  Matrix3d rotation;
  rotation <<
    cos(radian), 0, sin(radian),
    0, 1, 0,
    -sin(radian), 0, cos(radian);

  return rotation;
}

Eigen::Matrix3d RotationZ(const double radian) {
  Matrix3d rotation;
  rotation <<
    cos(radian), -sin(radian), 0,
    sin(radian), cos(radian), 0,
    0, 0, 1;

  return rotation;
}

double InverseNcc(std::vector<Vector3f>& patch0, std::vector<Vector3f>& patch1) {
  // Per channel intensity average.
  Vector3f ave0(0, 0, 0);
  for (const auto& p : patch0)
    ave0 += p;
  Vector3f ave1(0, 0, 0);
  for (const auto& p : patch1)
    ave1 += p;

  ave0 /= patch0.size();
  ave1 /= patch1.size();

  double ncc = 0.0;

  double var0 = 0.0;
  double var1 = 0.0;
  for (int i = 0; i < patch0.size(); ++i) {
    const auto& diff0 = patch0[i] - ave0;
    const auto& diff1 = patch1[i] - ave1;
    var0 += diff0.squaredNorm();
    var1 += diff1.squaredNorm();
    ncc += diff0.dot(diff1);
  }

  const int kNumChannels = 3;
  var0 = sqrt(var0);
  var1 = sqrt(var1);

  ncc /= max(0.01, var0 * var1);
  return min(1.0 - ncc, 0.7);
}
  
}  // namespace

namespace pre_process {

class RegularizationResidual {
 public:
  RegularizationResidual(const pre_process::StitchPanorama& stitch_panorama, const int camera_index) {
    rotation_org_ = stitch_panorama.rotations[camera_index];
  }

  template <typename T> bool operator()(const T* params, T* residual) const {
    const double kScale = 1000.0;
    const auto& mat = Rodrigues(Vector3d(params[0], params[1], params[2]));

    // residual[0] = fabs(mat.row(0).dot(rotation_org_.row(1)));
    
    const double kOffset = 0.0;
    residual[0] =
      kScale * max(0.0, (1.0 - (mat.row(1)).dot(rotation_org_.row(1)) - kOffset));

    return true;
  }

 private:
    Eigen::Matrix3d rotation_org_;
};

class RegularizationResidual2 {
 public:
  RegularizationResidual2(const pre_process::StitchPanorama& stitch_panorama, const int camera_index) {
    rotation_org_ = stitch_panorama.rotations[camera_index];
  }

  template <typename T> bool operator()(const T* params, T* residual) const {
    const double kScale = 0.01;
    const Matrix3d mat = Rodrigues(Vector3d(params[0], params[1], params[2]));
    const Matrix3d product = mat * rotation_org_.transpose();
    const Vector3d vec = Rodrigues(product);
    const double norm = vec.norm();

    const double kOffset = 0.02;
    residual[0] = kScale * max(0.0, norm - kOffset);
    return true;
  }

 private:
    Eigen::Matrix3d rotation_org_;
};
  
class ManualSpecification {
 public:
  ManualSpecification(const pre_process::StitchPanorama& stitch_panorama,
                      const Eigen::Vector2d& lhs,
                      const Eigen::Vector2d& rhs) : stitch_panorama_(stitch_panorama), lhs_(lhs), rhs_(rhs) {
  }

  template<typename T>
  bool operator()(const T* const param0,
                  const T* const param1,
                  T* residual) const {
    const auto rot0 = Rodrigues(Vector3d(param0[0], param0[1], param0[2]));
    const auto rot1 = Rodrigues(Vector3d(param1[0], param1[1], param1[2]));
    const auto proj0 = stitch_panorama_.intrinsics * rot0;
    const auto proj1 = stitch_panorama_.intrinsics * rot1;

    const auto& coord = proj0.inverse() * Vector3d(lhs_[0], lhs_[1], 1);
    Vector3d pixel = proj1 * coord;
    if (pixel[2] != 0.0) {
      pixel[0] /= pixel[2];
      pixel[1] /= pixel[2];
    }

    const double kScale = 10;
    const int kOffset = 3;
    residual[0] = max(0.0, (Vector2d(pixel[0], pixel[1]) - rhs_).norm() - kOffset) * kScale;
    return true;      
  }

 private:
  const pre_process::StitchPanorama& stitch_panorama_;
  const Eigen::Vector2d lhs_;
  const Eigen::Vector2d rhs_;
};
  
PatchCorrelationResidual::PatchCorrelationResidual(const pre_process::StitchPanorama& stitch_panorama,
                                                   const int x,
                                                   const int y,
                                                   const int size,
                                                   const int index0,
                                                   const int index1) :
  stitch_panorama_(stitch_panorama),
  x_(x),
  y_(y),
  size_(size),
  index0_(index0),
  index1_(index1) {
}

template<typename T>
  bool PatchCorrelationResidual::operator ()(const T* const param0,
                                             const T* const param1,
                                             T* residual) const {
  const auto rot0 = Rodrigues(Vector3d(param0[0], param0[1], param0[2]));
  const auto rot1 = Rodrigues(Vector3d(param1[0], param1[1], param1[2]));
  const auto proj0 = stitch_panorama_.intrinsics * rot0;
  const auto proj1 = stitch_panorama_.intrinsics * rot1;

  vector<Vector3f> patch0, patch1;
  if (!GrabPatch(index0_, proj0, patch0))
    return false;
  if (!GrabPatch(index1_, proj1, patch1))
    return false;

  residual[0] = InverseNcc(patch0, patch1);

  /*
  cerr << residual[0] << "  --  "
       << param0[0] << ' ' << param0[1] << ' ' << param0[2] << ' '
       << param1[0] << ' ' << param1[1] << ' ' << param1[2] << endl;
  */
  return true;
}

 bool PatchCorrelationResidual::GrabPatch(const int index,
                                          const Eigen::Matrix3d& projection,
                                          std::vector<Vector3f>& patch) const {
   patch.clear();
   patch.reserve(size_ * size_);
   for (int y = y_; y < y_ + size_; ++y) {
     for (int x = x_; x < x_ + size_; ++x) {
       const auto& ray = stitch_panorama_.ScreenToRay(Vector2d(x, y));
       Vector3d pixel = projection * ray;
       if (pixel[2] != 0.0) {
         pixel[0] /= pixel[2];
         pixel[1] /= pixel[2];
       }
       patch.push_back(InterpolateF(stitch_panorama_.images[index], Vector2d(pixel[0], pixel[1])));
     }
   }

   return true;
 }

bool StitchPanorama::Init(const std::vector<Eigen::Matrix3d>& initial_rotations) {
  {
    ifstream ifstr;
    ifstr.open((directory + "/K.txt").c_str());
    if (!ifstr.is_open()) {
      cerr << "No K file." << endl;
      return false;
    }
    for (int y = 0; y < 3; ++y) {
      for (int x = 0; x < 3; ++x) {
        ifstr >> intrinsics(y, x);
      }
    }
    const double scale = 1.0 / (1 << level);
    for (int x = 0; x < 3; ++x) {
      intrinsics(0, x) *= scale;
      intrinsics(1, x) *= scale;
    }

    ifstr.close();
  }

  if (initial_rotations.empty()) {
    ifstream ifstr;
    ifstr.open((directory + "/IMU_rotation.txt").c_str());
    if (!ifstr.is_open()) {
      cerr << "No IMU_rotation file." << endl;
      return false;
    }
    ifstr >> num_cameras;
    rotations.resize(num_cameras);
    for (int c = 0; c < num_cameras; ++c) {
      for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
          ifstr >> rotations[c](x, y);
        }
      }
    }
    ifstr.close();
  } else {
    rotations = initial_rotations;
  }

  projections.resize(num_cameras);
  for (int c = 0; c < num_cameras; ++c) {
    projections[c] = intrinsics * rotations[c];
  }

  images.resize(num_cameras);
  // for (int c = 0; c < num_cameras; ++c) {
  for (int c = 0; c < num_cameras; c += subsample) {
    char buffer[1024];
    sprintf(buffer, "%s/images/%04d.jpg", directory.c_str(), c);
    images[c] = imread(buffer, CV_LOAD_IMAGE_COLOR);

    for (int l = 0; l < level; ++l) {
      cv::Mat mtmp;
      cv::pyrDown(images[c], mtmp);
      images[c] = mtmp;
    }

    // imshow("Input Image", images[c]);
    // waitKey(0);
  }

  return true;
}

bool StitchPanorama::SetMasks() {
  masks.clear();
  masks.resize(num_cameras);
  // for (int c = 0; c < num_cameras; ++c) {
  for (int c = 0; c < num_cameras; c += subsample) {
    masks[c].create(out_height, out_width, CV_8UC(1));
    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        masks[c].at<unsigned char>(y, x) = 0;
      }
    }

    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        const Vector3d ray = ScreenToRay(Vector2d(x, y));
        const Vector3d pixel = Project(c, ray);

        if (pixel[2] <= 0.0)
          continue;

        if (0 <= pixel[0] && pixel[0] <= images[c].cols - 1 &&
            0 <= pixel[1] && pixel[1] <= images[c].rows - 1)
          masks[c].at<unsigned char>(y, x) = 255;
      }
    }

    // Set alpha.
    cv::Mat distance;
    distance.create(out_height, out_width, CV_32S);
    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        if (masks[c].at<unsigned char>(y, x) == 255)
          distance.at<short>(y, x) = -1;
        else
          distance.at<short>(y, x) = -2;
      }
    }

    list<Vector2i> front;
    for (int y = 1; y < out_height - 1; ++y) {
      for (int x = 0; x < out_width; ++x) {
        const int px = (x - 1 + out_width) % out_width;
        const int nx = (x + 1) % out_width;
        if (masks[c].at<unsigned char>(y, x) == 255 &&
            (masks[c].at<unsigned char>(y, px) == 0 ||
             masks[c].at<unsigned char>(y, nx) == 0 ||
             masks[c].at<unsigned char>(y - 1, x) == 0 ||
             masks[c].at<unsigned char>(y + 1, x) == 0)) {
          distance.at<short>(y, x) = 0;
          front.push_back(Vector2i(x, y));
        }
      }
    }
    while (!front.empty()) {
      const Vector2i pixel = front.front();
      front.pop_front();
      const int x = pixel[0];
      const int y = pixel[1];
      const int current_distance = distance.at<short>(y, x);
      if (current_distance >= margin)
        continue;

      for (int j = -1; j <= 1; ++j) {
        const int new_y = y + j;
        if (new_y < 0 || out_height <= new_y)
          continue;
        for (int i = -1; i <= 1; ++i) {
          const int new_x = (x + i + out_width) % out_width;
          if (distance.at<short>(new_y, new_x) == -1) {
            distance.at<short>(new_y, new_x) = current_distance + 1;
            front.push_back(Vector2i(new_x, new_y));
          }
        }
      }
      /*
      const int px = (x - 1 + out_width) % out_width;
      const int nx = (x + 1) % out_width;
      const int py = max(0, y - 1);
      const int ny = min(out_height - 1, y + 1);
      front.pop_front();

      const int current_distance = distance.at<short>(y, x);
      if (current_distance >= margin)
        continue;

      {
        if (distance.at<short>(y, px) == -1) {
          distance.at<short>(y, px) = current_distance + 1;
          front.push_back(Vector2i(px, y));
        }
      }
      {
        if (distance.at<short>(y, nx) == -1) {
          distance.at<short>(y, nx) = current_distance + 1;
          front.push_back(Vector2i(nx, y));
        }
      }
      {
        if (distance.at<short>(py, x) == -1) {
          distance.at<short>(py, x) = current_distance + 1;
          front.push_back(Vector2i(x, py));
        }
      }
      {
        if (distance.at<short>(ny, x) == -1) {
          distance.at<short>(ny, x) = current_distance + 1;
          front.push_back(Vector2i(x, ny));
        }
      }
      */
    }
    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        if (distance.at<short>(y, x) == -2)
          masks[c].at<unsigned char>(y, x) = 0;
        else if (distance.at<short>(y, x) == -1)
          masks[c].at<unsigned char>(y, x) = 255;
        else {
          masks[c].at<unsigned char>(y, x) = distance.at<short>(y, x) * (int)255 / margin;
        }
      }
    }

    // imshow("Mask", masks[c]);
    // waitKey(0);
  }

  return true;
}

bool StitchPanorama::RefineCameras() {
  // Sample patches in the screen and a set of related image indexes.
  SamplePatches();

  vector<double> params;
  for (int c = 0; c < num_cameras; c += subsample) {
    const auto& param = Rodrigues(rotations[c]);
    for (int i = 0; i < 3; ++i)
      params.push_back(param[i]);
  }
  vector<int> to_sampled_index;
  {
    int index = 0;
    for (int c = 0; c < num_cameras; ++c) {
      if (c % subsample == 0)
        to_sampled_index.push_back(index++);
      else
        to_sampled_index.push_back(-1);
    }
  }
  cerr << "Param size: " << params.size() << endl;

  ceres::Problem problem;
  const double kHuberParameter = 0.3;
  const int kNumOfParamsPerIndex = 3;
  for (const auto& patch : patches) {
    for (int i = 0; i < patch.indexes.size(); ++i) {
      for (int j = i+1; j < patch.indexes.size(); ++j) {
        ceres::CostFunction* cost_function =
          new ceres::NumericDiffCostFunction
          <PatchCorrelationResidual, ceres::CENTRAL, 1, 3, 3>
          (new PatchCorrelationResidual(*this, patch.x, patch.y, patch.size, patch.indexes[i], patch.indexes[j]));
        const int sampled_index0 = to_sampled_index[patch.indexes[i]];
        const int sampled_index1 = to_sampled_index[patch.indexes[j]];
        problem.AddResidualBlock(cost_function, new ceres::HuberLoss(kHuberParameter),
                                 &params[kNumOfParamsPerIndex * sampled_index0],
                                 &params[kNumOfParamsPerIndex * sampled_index1]);
      }
    }
  }

  for (int c = 0; c < num_cameras; c+= subsample) {
    const int sampled_index = to_sampled_index[c];
    ceres::CostFunction* cost_function =
      new ceres::NumericDiffCostFunction
      <RegularizationResidual, ceres::CENTRAL, 1, 3>(new RegularizationResidual(*this, c));
    problem.AddResidualBlock(cost_function, new ceres::TrivialLoss(),
                             &params[kNumOfParamsPerIndex * sampled_index]);
  }

  for (int c = 0; c < num_cameras; c+= subsample) {
    const int sampled_index = to_sampled_index[c];
    ceres::CostFunction* cost_function =
      new ceres::NumericDiffCostFunction
      <RegularizationResidual2, ceres::CENTRAL, 1, 3>(new RegularizationResidual2(*this, c));
    problem.AddResidualBlock(cost_function, new ceres::TrivialLoss(),
                             &params[kNumOfParamsPerIndex * sampled_index]);
  }
  
  //----------------------------------------------------------------------
  // Manual clicks.
  //----------------------------------------------------------------------
  // Lopata.
  /*
  {
    vector<pair<int, int> > image_pairs;
    vector<pair<Vector2d, Vector2d> > pixel_pairs;
    {
      const int kNumData = 7;
      const int image_list[kNumData] = { 30, 31, 32, 33, 34, 35, 36 };
      const Vector2d pixel_list[kNumData] = { Vector2d(626, 319),
                                              Vector2d(512, 310),
                                              Vector2d(413, 300),
                                              Vector2d(341, 309),
                                              Vector2d(244, 307),
                                              Vector2d(139, 311),
                                              Vector2d(47, 304) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    {
      const int kNumData = 7;
      const int image_list[kNumData] = { 34, 35, 36, 37, 38, 39, 40 };
      const Vector2d pixel_list[kNumData] = { Vector2d(663, 312),
                                              Vector2d(539, 322),
                                              Vector2d(436, 324),
                                              Vector2d(334, 322),
                                              Vector2d(232, 343),
                                              Vector2d(133, 343),
                                              Vector2d(36, 329) };
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    {
      const int kNumData = 9;
      const int image_list[kNumData] = { 20, 21, 22, 23, 24, 25, 26, 27, 28 };
      const Vector2d pixel_list[kNumData] = { Vector2d(663, 709),
                                              Vector2d(619, 707),
                                              Vector2d(567, 697),
                                              Vector2d(510, 710),
                                              Vector2d(429, 716),
                                              Vector2d(323, 713),
                                              Vector2d(236, 711),
                                              Vector2d(122, 698),
                                              Vector2d(40, 699) };
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    {
      const int kNumData = 7;
      const int image_list[kNumData] = { 25, 26, 27, 28, 29, 30, 31 };
      const Vector2d pixel_list[kNumData] = { Vector2d(649, 715),
                                              Vector2d(556, 714),
                                              Vector2d(445, 696),
                                              Vector2d(371, 690),
                                              Vector2d(287, 690),
                                              Vector2d(195, 686),
                                              Vector2d(87, 681) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    {
      const int kNumData = 6;
      const int image_list[kNumData] = { 25, 26, 27, 28, 29, 30 };
      const Vector2d pixel_list[kNumData] = { Vector2d(532, 511),
                                              Vector2d(440, 510),
                                              Vector2d(329, 494),
                                              Vector2d(252, 491),
                                              Vector2d(164, 485),
                                              Vector2d(68, 479) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    
    
    {
      const int kNumData = 8;
      const int image_list[kNumData] = { 28, 29, 30, 31, 32, 33, 34, 35 };
      const Vector2d pixel_list[kNumData] = { Vector2d(667, 577),
                                              Vector2d(576, 570),
                                              Vector2d(482, 567),
                                              Vector2d(377, 556),
                                              Vector2d(286, 546),
                                              Vector2d(221, 554),
                                              Vector2d(130, 556),
                                              Vector2d(37, 563) };
                                              
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    
    
    {
      const int kNumData = 10;
      const int image_list[kNumData] = { 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };
      const Vector2d pixel_list[kNumData] = { Vector2d(657, 512),
                                              Vector2d(586, 510),
                                              Vector2d(517, 518),
                                              Vector2d(435, 511),
                                              Vector2d(357, 511),
                                              Vector2d(279, 509),
                                              Vector2d(207, 503),
                                              Vector2d(162, 500),
                                              Vector2d(109, 492),
                                              Vector2d(55, 501) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    {
      const int kNumData = 7;
      const int image_list[kNumData] = { 36, 37, 38, 39, 40, 41, 42 };
      const Vector2d pixel_list[kNumData] = { Vector2d(665, 486),
                                              Vector2d(568, 486),
                                              Vector2d(475, 502),
                                              Vector2d(380, 507),
                                              Vector2d(296, 500),
                                              Vector2d(200, 492),
                                              Vector2d(100, 484) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    

    
    {
      const int kNumData = 9;
      const int image_list[kNumData] = { 9, 10, 11, 12, 13, 14, 15, 16, 17};
      const Vector2d pixel_list[kNumData] = { Vector2d(675, 335),
                                              Vector2d(596, 339),
                                              Vector2d(521, 343),
                                              Vector2d(455, 349),
                                              Vector2d(379, 344),
                                              Vector2d(310, 352),
                                              Vector2d(240, 354),
                                              Vector2d(167, 359),
                                              Vector2d(77, 346) };

      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }    
  
    const double scale = 1.0 / (1 << level);
    for (auto& pixel_pair : pixel_pairs) {
      pixel_pair.first  *= scale;
      pixel_pair.second *= scale;
    }

    for (int i = 0; i < image_pairs.size(); ++i) {
      ceres::CostFunction* cost_function =
        new ceres::NumericDiffCostFunction<ManualSpecification, ceres::CENTRAL, 1, 3, 3>(new ManualSpecification(*this, pixel_pairs[i].first, pixel_pairs[i].second));
      const int sampled_index0 = to_sampled_index[image_pairs[i].first];
      const int sampled_index1 = to_sampled_index[image_pairs[i].second];

      if (sampled_index0 == -1 || sampled_index1 == -1 || sampled_index0 == sampled_index1) {
        cerr << " NONO " << image_pairs[i].first << ' ' << image_pairs[i].second << endl
             << sampled_index0 << ' ' << sampled_index1 << endl;
        exit (1);
      }
      
      problem.AddResidualBlock(cost_function, new ceres::TrivialLoss(),
                               &params[kNumOfParamsPerIndex * sampled_index0],
                               &params[kNumOfParamsPerIndex * sampled_index1]);
    }
  }
  */

  // Atrium.
  {
    vector<pair<int, int> > image_pairs;
    vector<pair<Vector2d, Vector2d> > pixel_pairs;
    {
      const int kNumData = 11;
      const int image_list[kNumData] = { 0, 6, 12, 18, 24, 30, 36, 336, 342, 348, 354};
      const Vector2d pixel_list[kNumData] = { Vector2d(495, 438),
                                              Vector2d(487, 442),
                                              Vector2d(452, 442),
                                              Vector2d(395, 438),
                                              Vector2d(318, 424),
                                              Vector2d(206, 421),
                                              Vector2d(98, 408),
                                              Vector2d(546, 346),
                                              Vector2d(399, 344),
                                              Vector2d(256, 350),
                                              Vector2d(55, 335) };
      
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }


    {
      const int kNumData = 6;
      const int image_list[kNumData] = { 0, 6, 318, 324, 330, 336};
      const Vector2d pixel_list[kNumData] = { Vector2d(37, 333),
                                              Vector2d(30, 328),
                                              Vector2d(612, 252),
                                              Vector2d(432, 272),
                                              Vector2d(270, 265),
                                              Vector2d(70, 256) };
      
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }

    {
      const int kNumData = 5;
      const int image_list[kNumData] = { 306, 312, 318, 324, 330 };
      const Vector2d pixel_list[kNumData] = { Vector2d(679, 766),
                                              Vector2d(550, 770),
                                              Vector2d(412, 760),
                                              Vector2d(261, 775),
                                              Vector2d(110, 774) };
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }
    
    {
      const int kNumData = 4;
      const int image_list[kNumData] = { 204, 210, 216, 222 };
      const Vector2d pixel_list[kNumData] = { Vector2d(551, 380),
                                              Vector2d(390, 375),
                                              Vector2d(224, 359),
                                              Vector2d(100, 327) };
      
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }

    {
      const int kNumData = 4;
      const int image_list[kNumData] = { 246, 252, 258, 264 };
      const Vector2d pixel_list[kNumData] = { Vector2d(516, 999),
                                              Vector2d(358, 974),
                                              Vector2d(209, 973),
                                              Vector2d(45, 1012) };
      for (int i = 0; i < kNumData; ++i) {
        if (image_list[i] % subsample != 0)
          continue;
        for (int j = i+1; j < kNumData; ++j) {
          if (image_list[j] % subsample != 0)
            continue;
          image_pairs.push_back(make_pair<int, int>(image_list[i], image_list[j]));
          pixel_pairs.push_back(make_pair(pixel_list[i], pixel_list[j]));
        }
      }
    }
    
    
    
    const double scale = 1.0 / (1 << level);
    for (auto& pixel_pair : pixel_pairs) {
      pixel_pair.first  *= scale;
      pixel_pair.second *= scale;
    }

    for (int i = 0; i < image_pairs.size(); ++i) {
      ceres::CostFunction* cost_function =
        new ceres::NumericDiffCostFunction<ManualSpecification, ceres::CENTRAL, 1, 3, 3>(new ManualSpecification(*this, pixel_pairs[i].first, pixel_pairs[i].second));
      const int sampled_index0 = to_sampled_index[image_pairs[i].first];
      const int sampled_index1 = to_sampled_index[image_pairs[i].second];

      if (sampled_index0 == -1 || sampled_index1 == -1 || sampled_index0 == sampled_index1) {
        cerr << " NONO " << image_pairs[i].first << ' ' << image_pairs[i].second << endl
             << sampled_index0 << ' ' << sampled_index1 << endl;
        exit (1);
      }
      
      problem.AddResidualBlock(cost_function, new ceres::TrivialLoss(),
                               &params[kNumOfParamsPerIndex * sampled_index0],
                               &params[kNumOfParamsPerIndex * sampled_index1]);
    }
    
  }
  
  ceres::Solver::Options options;
  options.max_num_iterations = 25; // 50;
  options.num_threads = 4;
  options.minimizer_progress_to_stdout = true;
  ceres::Solver::Summary summary;
  cerr << "Starts solving" << endl;
  ceres::Solve(options, &problem, &summary);
  std::cerr << summary.FullReport() << endl;

  // Update rotations and projections.
  {
    int index = 0;
    for (int c = 0; c < num_cameras; c += subsample) {
      const Vector3d vtmp(params[index], params[index + 1], params[index + 2]);
      index += 3;
      rotations[c] = Rodrigues(vtmp);
      projections[c] = intrinsics * rotations[c];
    }
  }

  refined_rotations = rotations;

  return true;
}

void StitchPanorama::SamplePatches() {
  patches.clear();
  const int step = out_width / 200;
  const int kSize = 7;
  for (int y = step + kSize; y < out_height - step - kSize; y += step) {
     // for (int y = out_height / 2; y < out_height - step - kSize; y += step) {
    for (int x = 0; x < out_width; x += step) {
      Patch patch;
      patch.x = x;
      patch.y = y;
      patch.size = kSize;
      vector<int> valid_count_camera(num_cameras, 0);
      for (int j = 0; j < kSize; ++j) {
        const int ytmp = y + j;
        for (int i = 0; i < kSize; ++i) {
          const int xtmp = (x + i) % out_width;
          for (int c = 0; c < num_cameras; c += subsample) {
            if (masks[c].at<unsigned char>(ytmp, xtmp) == 255)
              ++valid_count_camera[c];
          }
        }
      }
      for (int c = 0; c < num_cameras; c += subsample) {
        if (valid_count_camera[c] == kSize * kSize)
          patch.indexes.push_back(c);
      }

      if (patch.indexes.size() >= 2) {
        patches.push_back(patch);
      }
    }
  }

  cerr << patches.size() << " patches sampled." << endl;
}

bool StitchPanorama::Blend(const std::string& filename) {
  cv::Mat output;
  output.create(out_height, out_width, CV_32FC4);

  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      output.at<cv::Vec4f>(y, x) = cv::Vec4f(0, 0, 0, 0);
    }
  }

  // Accumulate.
  for (int c = 0; c < num_cameras; c += subsample) {
    for (int y = 0; y < out_height; ++y) {
      for (int x = 0; x < out_width; ++x) {
        const float alpha = masks[c].at<unsigned char>(y, x) / 255.0;
        if (alpha == 0.0)
          continue;

        const Vector3d pixel = Project(c, ScreenToRay(Vector2d(x, y)));
        const auto& rgb = InterpolateF(images[c], Vector2d(pixel[0], pixel[1]));
        for (int i = 0; i < 3; ++i)
          output.at<Vec4f>(y, x)[i] += rgb[i] * alpha;
        output.at<Vec4f>(y, x)[3] += alpha;
      }
    }
  }

  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      if (output.at<Vec4f>(y, x)[3] == 0.0)
        continue;
      output.at<Vec4f>(y, x) /= output.at<Vec4f>(y, x)[3];
    }
  }

  stitched_image.create(out_height, out_width, CV_8UC3);
  for (int y = 0; y < out_height; ++y) {
    const int out_y = out_height - 1 - y;
    for (int x = 0; x < out_width; ++x) {
      const int out_x = x; // (x + out_width / 2) % out_width;
      for (int c = 0; c < 3; ++c) {
        stitched_image.at<Vec3b>(out_y, out_x) =
          Vec3b(min(255, (int)round(output.at<Vec4f>(y, x)[0])),
                min(255, (int)round(output.at<Vec4f>(y, x)[1])),
                min(255, (int)round(output.at<Vec4f>(y, x)[2])));
      }
    }
  }

  imwrite(filename.c_str(), stitched_image);
  imshow(filename.c_str(), stitched_image);
  // waitKey(0);

  return true;
}

Eigen::Vector3d StitchPanorama::ScreenToRay(const Eigen::Vector2d& screen) const {
  const double longitude = screen[0] * 2.0 * M_PI / out_width;
  const double latitude  = screen[1] * M_PI / out_height - (M_PI / 2.0);

  const Matrix3d rotation = RotationZ(longitude) * RotationX(latitude);
  return rotation * Vector3d(0, 1, 0);
}

Eigen::Vector2d StitchPanorama::RayToScreen(const Eigen::Vector3d& ray) const {
  // cos -sin 0     1 0 0             0
  // sin cos  0     0 cosa -sina      1
  // 0   0    1     0 sina cosa       0
  //
  // y (-sin cosa, cos cosa, sina)
  const double sina = ray[2];
  const double cosa = sqrt(ray[0] * ray[0] + ray[1] * ray[1]);

  const double latitude = atan2(sina, cosa);
  double longitude;
  if (cosa == 0.0)
    longitude = 0.0;
  longitude = atan2(-ray[0] / cosa, ray[1] / cosa);
  if (longitude < 0.0)
    longitude += 2 * M_PI;

  const double x = min((double)out_width, longitude / (2 * M_PI) * out_width);
  const double y = max(0.0, min((double)out_height, (latitude + M_PI / 2.0) / M_PI * out_height));

  return Vector2d(x, y);

  /*
  // cos 0 sin      1 0 0             0
  // 0 1 0        0 cosa -sina        0
  // -sin 0 cos    0 sina cosa        1
  //
  // z (sin cosa, -sina, cos cosa)
  const double sina = -ray[1];
  const double cosa = sqrt(ray[0] * ray[0] + ray[2] * ray[2]);
  const double latitude = atan2(sina, cosa);
  double longitude;
  if (cosa == 0.0)
    longitude = 0.0;
  longitude = atan2(ray[0] / cosa, ray[2] / cosa);
  if (longitude < 0.0)
    longitude += 2 * M_PI;

  const double x = min((double)out_width, longitude / (2 * M_PI) * out_width);
  const double y = max(0.0, min((double)out_height, (latitude + M_PI / 2.0) / M_PI * out_height));

  return Vector2d(x, y);
  */
}

Eigen::Vector3d StitchPanorama::Project(const int camera, const Eigen::Vector3d& ray) const {
  Vector3d pixel = projections[camera] * ray;
  if (pixel[2] != 0.0) {
    pixel[0] /= pixel[2];
    pixel[1] /= pixel[2];
  }
  return pixel;
}

bool StitchPanorama::Stitch(const Input& input) {
  directory  = input.directory;
  out_width  = input.out_width;
  out_height = input.out_height;
  level      = input.level;
  margin     = input.margin;
  subsample  = input.subsample;
  if (!Init(input.initial_rotations))
    return false;

  /*
  Vector3d axis0(0.93162, -0.35736, -0.06617);
  Vector3d axis1(0.9315, -0.35675  , -0.071);
  Vector3d axis2(0.92628, -0.36947, -0.07413);
  Vector3d axis3(0.91541, -0.39554, -0.07462);
  Vector3d axis4(0.89898, -0.43169, -0.07407);
  Vector3d axis5(0.8745, -0.47818, -0.08122);
  Vector3d axis6(0.84627 , -0.5267, -0.08004);
  Vector3d axis7(0.81547, -0.57273, -0.08358);
  Vector3d axis8(0.7911, -0.60633, -0.08077);
  Vector3d axis9(0.7545, -0.65019, -0.08941);
  Vector3d axis10(0.71475 , -0.6939, -0.08738);
  Vector3d axis11(0.67475, -0.73297, -0.08641);
  Vector3d axis12(0.63439, -0.76817, -0.08639);

  cerr << RayToScreen(axis0) << endl;
  cerr << RayToScreen(axis1) << endl;
  cerr << RayToScreen(axis2) << endl;
  cerr << RayToScreen(axis3) << endl;
  cerr << RayToScreen(axis4) << endl;
  cerr << RayToScreen(axis5) << endl;
  cerr << RayToScreen(axis6) << endl;
  cerr << RayToScreen(axis7) << endl;
  cerr << RayToScreen(axis8) << endl;
  cerr << RayToScreen(axis9) << endl;
  cerr << RayToScreen(axis10) << endl;
  cerr << RayToScreen(axis11) << endl;
  cerr << RayToScreen(axis12) << endl;
  //  return true;
  */
  /*
  for (int y = 10; y < out_height - 10; ++y) {
    for (int x = 0; x < out_width; ++x) {
      const auto ray = ScreenToRay(Vector2d(x, y));
      const auto pixel = RayToScreen(ray);
      cerr << (pixel - Vector2d(x, y)).norm() << ' ' << flush;
    }
    }
  */

  if (!SetMasks())
    return false;

  char buffer[1024];
  sprintf(buffer, "%s/%d-before.png", directory.c_str(), level);
  if (!Blend(buffer))
    return false;

  if (!RefineCameras())
    return false;

  sprintf(buffer, "%s/%d-after.png", directory.c_str(), level);
  if (!Blend(buffer))
    return false;

  return true;
}

}  // namespace pre_process
