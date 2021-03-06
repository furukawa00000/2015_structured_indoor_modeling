/*
  A point cloud class with read and write capabilities. Each point
  can have many information beyond its 3D position. struct Point has
  the list.

  There are several utility functions also implemented. For example,
  one can transform the point information to the global coordinate
  system by ToGlobal(). This is the typical suggested usage. Read data
  from FileIO::GetLocalPly(const int panorama), and convert them to
  the global coordinate frame.

  Rotate() and Transform() can apply arbitrary
  transformations. SetPoints and AddPoints can change the stored point
  cloud data. This class has been mostly used for read-only access,
  and the implemented features are fairly limited at the moment.

  < Example >

  FileIO file_io("../some_data_directory");
  PointCloud point_cloud;
  const int kPanoramaID = 3;
  point_cloud.Init(file_io, kPanoramaID);

  PointCloud point_cloud2;
  point_cloud2.Init(file_io.GetObjectCloudsWithColor());

  PointCloud point_cloud3;
  point_cloud3.AddPoints(point_cloud);
  point_cloud3.AddPoints(point_cloud2);
  point_cloud3.Write("new_file.ply");
 */

#ifndef BASE_POINT_CLOUD_H_
#define BASE_POINT_CLOUD_H_

#include <Eigen/Dense>
#include <vector>

namespace structured_indoor_modeling {

class FileIO;

struct Point {
  Eigen::Vector2i depth_position;
  Eigen::Vector3d position;
  Eigen::Vector3f color;

  Eigen::Vector3d normal;
  int intensity;
  int object_id;
  Point(){object_id = 0;}
};

class PointCloud {
 public:
  PointCloud();
  
  // Read the corresponding point cloud in the local coordinate frame.
  bool Init(const FileIO& file_io, const int panorama);
  // Read the point cloud with the given filename.
  bool Init(const std::string& filename);
  // Writer.
  void Write(const std::string& filename);
  void WriteObject(const std::string& filename, const int objectid);

  // Transformations.
  void ToGlobal(const FileIO& file_io, const int panorama);
  void Rotate(const Eigen::Matrix3d& rotation);
  void Translate(const Eigen::Vector3d& translation);
  void Transform(const Eigen::Matrix4d& transformation);

  void RandomSampleScale(const double scale);
  void RandomSampleCount(const int max_count);
  
  // Accessors.
  inline int GetNumPoints() const { return points.size(); }
  inline int GetDepthWidth() const { return depth_width; }
  inline int GetDepthHeight() const { return depth_height; }
  inline const std::vector<Point>& GetPointData() const {return points;}
  inline std::vector<Point> &GetPointData() {return points;}
  // yasu This should return const reference to speed-up.
  inline const std::vector<double>& GetBoundingbox() const { return bounding_box; }
  inline int GetNumObjects() const { return num_objects; }
  // yasu This should return const reference to speed-up.
  inline const Eigen::Vector3d& GetCenter() const { return center; }
  inline const Point& GetPoint(const int p) const { return points[p]; }
  inline Point& GetPoint(const int p) { return points[p]; }
  inline bool isempty() const {return (int)points.size() == 0;}
  void GetObjectIndice(int objectid, std::vector<int>&indices) const;
  void GetObjectPoints(int objectid, std::vector<structured_indoor_modeling::Point>& object_points) const;
  void GetObjectBoundingbox(int objectid, std::vector<double>&bbox)const;
  double GetBoundingboxVolume();
  double GetObjectBoundingboxVolume(const int objectid);
  // Setters.
  void SetPoints(const std::vector<Point>& new_points) {
    points = new_points;
    Update();
  }
  
  void SetAllColor(float r,float g,float b);
  void SetColor(int ind, float r, float g,float b);
  
  inline void SetColor(int ind, Eigen::Vector3f new_color){
      SetColor(ind, new_color[0], new_color[1], new_color[2]);
  }

  void AddPoints(const PointCloud& point_cloud, bool mergeid = false);
  void AddPoints(const std::vector<Point>& new_points, bool mergeid = false);

  void RemovePoints(const std::vector<int>& indexes);
  void Update();  
 private:
  void InitializeMembers();
  std::vector<Point> points;

  Eigen::Vector3d center;
  int depth_width;
  int depth_height;
  int num_objects;
  std::vector <double> bounding_box; //xmin,xmax,ymin,ymax,zmin,zmax

  static const int kDepthPositionOffset;
};

typedef std::vector<PointCloud> PointClouds;

//----------------------------------------------------------------------
void ReadPointClouds(const FileIO& file_io, std::vector<PointCloud>* point_clouds);

void ReadObjectPointClouds(const FileIO& file_io,
                           const int num_rooms,
                           std::vector<PointCloud>* object_point_clouds);

void ReadRefinedObjectPointClouds(const FileIO& file_io,
				  const int num_rooms,
				  std::vector<PointCloud>* object_point_clouds);
}  // namespace structured_indoor_modeling

#endif  // BASE_POINT_CLOUD_H_
