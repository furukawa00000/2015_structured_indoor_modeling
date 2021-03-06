#include <Eigen/Dense>
#include <fstream>
#include <numeric>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <queue>

#include "../../base/floorplan.h"
#include "../../base/indoor_polygon.h"
#include "../../base/point_cloud.h"
#include "../../base/kdtree/KDtree.h"
#include "object_segmentation.h"

using namespace Eigen;
using namespace std;

namespace structured_indoor_modeling {

namespace {
  bool IsOnFloor(const double floor_height, const double margin, const Point& point);

  bool IsOnWall(const std::vector<cv::Point2f>& contour,
                const double wall_margin,
                const Point& point);
  
  bool IsOnCeiling(const double ceiling_height, const double margin, const Point& point);
  
  double PointDistance(const Point& lhs, const Point& rhs);

  void InitializeCentroids(const std::vector<Point>& points,
                           const int num_initial_clusters,
                           std::vector<int>* segments);
  
  void ComputeDistances(const std::vector<Point>& points,
                        const std::vector<std::vector<int> >& neighbors,
                        const int index,
                        const std::vector<int>& segments,
                        std::vector<double>* distances);
  
  void AssignFromCentroids(const std::vector<Point>& points,
                           const std::vector<std::vector<int> >& neighbors,
                           std::vector<int>* segments);
  
  void ComputeCentroids(const std::vector<Point>& points, const double ratio, vector<int>* segments);
  
  void UnionFind(const std::set<std::pair<int, int> >& merged,
                 const int max_old,
                 std::map<int, int>* old_to_new);
  
  bool Merge(const std::vector<Point>& points,
             const std::vector<std::vector<int> >& neighbors,
             std::vector<int>* segments,
             std::map<int, Eigen::Vector3i>* color_table);

  Eigen::Vector3d Intersect(const Point& lhs, const Point& rhs);

  void FillOccupancy(const Eigen::Vector3d& v0,
                     const Eigen::Vector3d& v1,
                     const Eigen::Vector3d& v2,
                     const Eigen::Vector3d& min_xyz,
                     const double voxel_unit,
                     const Eigen::Vector3i& size,
                     std::vector<bool>* occupancy);

void ReportSegments(const std::vector<int>& segments);
  
}  // namespace  

void SaveData(const int id,
              const std::vector<Point>& points,
              const std::vector<int>& segments) {
  char buffer[1024];
  sprintf(buffer, "data_%d.dat", id);

  ofstream ofstr;
  ofstr.open(buffer);

  ofstr << (int)points.size() << endl;
  for (const auto& point : points) {
    ofstr << point.depth_position[1] << ' ' << point.depth_position[0] << ' '
          << point.position[0] << ' ' << point.position[1] << ' ' << point.position[2] << ' '
          << point.color[0] << ' ' << point.color[1] << ' ' << point.color[2] << ' '
          << point.normal[0] << ' ' << point.normal[1] << ' ' << point.normal[2] << ' '
          << point.intensity << endl;
  }

  ofstr << segments.size() << endl;
  for (const auto segment : segments)
    ofstr << segment << ' ';
  ofstr << endl;
  
  ofstr.close();
}

void LoadData(const int id,
              std::vector<Point>* points,
              std::vector<int>* segments) {
  points->clear();
  segments->clear();
  
  char buffer[1024];
  sprintf(buffer, "data_%d.dat", id);

  ifstream ifstr;
  ifstr.open(buffer);

  int num_points;
  ifstr >> num_points;
  points->resize(num_points);
  for (int p = 0; p < num_points; ++p) {
    ifstr >> points->at(p).depth_position[1] >> points->at(p).depth_position[0] 
          >> points->at(p).position[0] >> points->at(p).position[1] >> points->at(p).position[2] 
          >> points->at(p).color[0]    >> points->at(p).color[1]    >> points->at(p).color[2] 
          >> points->at(p).normal[0]   >> points->at(p).normal[1]   >> points->at(p).normal[2] 
          >> points->at(p).intensity;
  }

  int num_segments;
  ifstr >> num_segments;
  segments->resize(num_segments);
  for (int s = 0; s < num_segments; ++s) {
    ifstr >> segments->at(s);
  }
  
  ifstr.close();
}
  
void SetRoomOccupancy(const Floorplan& floorplan,
                      std::vector<int>* room_occupancy) {
  const Vector3i grid_size = floorplan.GetGridSize();
  room_occupancy->clear();
  const int kBackground = -1;
  room_occupancy->resize(grid_size[0] * grid_size[1], kBackground);

  // Make room contour polygons for each room.
  vector<vector<cv::Point2f> > room_contours(floorplan.GetNumRooms());
  for (int room = 0; room < floorplan.GetNumRooms(); ++room) {
    for (int vertex = 0; vertex < floorplan.GetNumRoomVertices(room); ++vertex) {
      const Vector2d local = floorplan.GetRoomVertexLocal(room, vertex);
      room_contours[room].push_back(cv::Point2f(local[0], local[1]));
    }
  }

  const double kDistanceThreshold = floorplan.GetGridUnit() * 2.0;
  int index = 0;
  for (int y = 0; y < grid_size[1]; ++y) {
    for (int x = 0; x < grid_size[0]; ++x, ++index) {
      const Vector2d local = floorplan.GridToLocal(Vector2d(x, y));
      const cv::Point2f point(local[0], local[1]);

      // Find the room with the closest distance
      const int kInvalid = -1;
      int best_room = kInvalid;
      double best_distance = 0.0;

      for (int room = 0; room < floorplan.GetNumRooms(); ++room) {
        const double distance = cv::pointPolygonTest(room_contours[room], point, true);
        if (best_room == kInvalid || distance > best_distance) {
          best_room = room;
          best_distance = distance;

          // Inside.
          if (best_distance > 0.0)
            break;
        }
      }
      
      if (best_distance > - kDistanceThreshold) {
        room_occupancy->at(index) = best_room;
      }
    }
  }

  /*
  {
    ofstream ofstr;
    ofstr.open("room.ppm");
    ofstr << "P3" << endl
          << grid_size[0] << ' ' << grid_size[1] << endl
          << 255 << endl;

    int index = 0;
    for (int y = 0; y < grid_size[1]; ++y) {
      for (int x = 0; x < grid_size[0]; ++x, ++index) {
        if (room_occupancy->at(index) < 0)
          ofstr << "255 255 255 " << endl;
        else
          ofstr << "0 0 0 " << endl;
      }
    }
    ofstr.close();
  }
  */
}

void SetDoorOccupancy(const Floorplan& floorplan,
                      std::vector<int>* room_occupancy_with_doors) {
  const Eigen::Matrix3d global_to_floorplan = floorplan.GetFloorplanToGlobal().transpose();

  const int kDoor = 1000;
  const int width  = floorplan.GetGridSize()[0];
  const int height = floorplan.GetGridSize()[1];
  
  for (int d = 0; d < floorplan.GetNumDoors(); ++d) {
    Vector3d local0 = global_to_floorplan * floorplan.GetDoorVertexGlobal(d, 0);
    const Vector2d v0 = floorplan.LocalToGrid(Vector2d(local0[0], local0[1]));

    Vector3d local1 = global_to_floorplan * floorplan.GetDoorVertexGlobal(d, 1);
    const Vector2d v1 = floorplan.LocalToGrid(Vector2d(local1[0], local1[1]));

    Vector3d local2 = global_to_floorplan * floorplan.GetDoorVertexGlobal(d, 5);
    const Vector2d v2 = floorplan.LocalToGrid(Vector2d(local2[0], local2[1]));

    const int sample1 = (v1 - v0).norm() * 2;
    const int sample2 = (v2 - v0).norm() * 2;

    for (int s1 = 0; s1 <= sample1; ++s1) {
      for (int s2 = 0; s2 <= sample2; ++s2) {
        Vector2d position = v0 + (v1 - v0) * s1 / sample1 + (v2 - v0) * s2 / sample2;
        
        
        const int x0 = max(0, min(width - 2, static_cast<int>(floor(position[0]))));
        const int y0 = max(0, min(height - 2, static_cast<int>(floor(position[1]))));
        const int x1 = x0 + 1;
        const int y1 = y0 + 1;
        room_occupancy_with_doors->at(y0 * width + x0) = kDoor;
        room_occupancy_with_doors->at(y0 * width + x1) = kDoor;
        room_occupancy_with_doors->at(y1 * width + x0) = kDoor;
        room_occupancy_with_doors->at(y1 * width + x1) = kDoor;
      }
    }
  }
  /*
  {
    ofstream ofstr;
    ofstr.open("room_door.ppm");
    ofstr << "P3" << endl
          << width << ' ' << height << endl
          << 255 << endl;

    int index = 0;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x, ++index) {
        if (room_occupancy_with_doors->at(index) < 0)
          ofstr << "255 255 255 " << endl;
        else
          ofstr << "0 0 0 " << endl;
      }
    }
    ofstr.close();
  }
  */
}
  
void CollectPointsInRoom(const std::vector<PointCloud>& point_clouds,
                         const Floorplan& floorplan,
                         const std::vector<int>& room_occupancy,
                         const int room,
                         std::vector<Point>* points) {
  for (const auto& point_cloud : point_clouds) {
    for (int p = 0; p < point_cloud.GetNumPoints(); ++p) {
      const Vector3d& local = point_cloud.GetPoint(p).position;
      const Vector2i grid_int = floorplan.LocalToGridInt(Vector2d(local[0], local[1]));
      const int index = grid_int[1] * floorplan.GetGridSize()[0] + grid_int[0];
      if (room_occupancy[index] == room)
        points->push_back(point_cloud.GetPoint(p));
    }
  }
}

void IdentifyFloorWallCeiling(const std::vector<Point>& points,
                              const Floorplan& floorplan,
                              const int room,
                              const double rescale_margin,
                              std::vector<int>* segments) {
  const double kFloorMarginRatio   = 0.1 * rescale_margin;
  const double kCeilingMarginRatio = 0.1 * rescale_margin;
  const double kWallMarginRatio    = 0.03 * rescale_margin;

  const double room_height    = floorplan.GetCeilingHeight(room) - floorplan.GetFloorHeight(room);
  const double floor_margin   = room_height * kFloorMarginRatio;
  const double wall_margin    = room_height * kWallMarginRatio;
  const double ceiling_margin = room_height * kCeilingMarginRatio;

  vector<cv::Point2f> contour;
  for (int vertex = 0; vertex < floorplan.GetNumRoomVertices(room); ++vertex) {
    const Vector2d local = floorplan.GetRoomVertexLocal(room, vertex);
    contour.push_back(cv::Point2f(local[0], local[1]));
  }
  
  segments->clear();
  segments->resize((int)points.size(), kInitial);
  // Floor check.
  for (int p = 0; p < (int)points.size(); ++p) {
    if (IsOnFloor(floorplan.GetFloorHeight(room), floor_margin, points[p]))
      segments->at(p) = kFloor;
    else if (IsOnCeiling(floorplan.GetCeilingHeight(room), ceiling_margin, points[p]))
      segments->at(p) = kCeiling;
    else if (IsOnWall(contour, wall_margin, points[p]))
      segments->at(p) = kWall;
  }
}

void IdentifyDetails(const std::vector<Point>& points,
                     const Floorplan& floorplan,
                     const IndoorPolygon& indoor_polygon,
                     const int room,
                     const double rescale_margin,
                     std::vector<int>* segments) {
  const double kDetailMarginRatio   = 0.1 * rescale_margin;
  const double room_height    = floorplan.GetCeilingHeight(room) - floorplan.GetFloorHeight(room);
  const double detail_margin = room_height * kDetailMarginRatio;

  const double voxel_unit = room_height / 128;
  Vector3d min_xyz, max_xyz;
  
  bool first = true;
  for (int s = 0; s < indoor_polygon.GetNumSegments(); ++s) {
    const Segment& segment = indoor_polygon.GetSegment(s);
    if ((segment.type == Segment::FLOOR   && segment.floor_info == room) ||
        (segment.type == Segment::CEILING && segment.ceiling_info == room) ||
        (segment.type == Segment::WALL    && segment.wall_info[0] == room)) {
      for (const auto& vertex : segment.vertices) {
        if (first) {
          for (int a = 0; a < 3; ++a) {
            min_xyz[a] = max_xyz[a] = vertex[a];
          }
          first = false;
        } else {
          for (int a = 0; a < 3; ++a) {
            min_xyz[a] = min(min_xyz[a], vertex[a]);
            max_xyz[a] = max(max_xyz[a], vertex[a]);
          }
        }
      }
    }
  }
    
  // Allocate a voxel inside a room.
  Vector3d length = max_xyz - min_xyz;
  Vector3i size;
  for (int a = 0; a < 3; ++a) {
    size[a] = max(1, static_cast<int>(round(length[a] / voxel_unit)));
  }
//  cout << size[0] << ' ' << size[1] << ' ' << size[2] << " voxels" << endl;
  
  vector<bool> occupancy(size[0] * size[1] * size[2], false);
  for (int s = 0; s < indoor_polygon.GetNumSegments(); ++s) {
    const Segment& segment = indoor_polygon.GetSegment(s);
    if ((segment.type == Segment::FLOOR   && segment.floor_info == room) ||
        (segment.type == Segment::CEILING && segment.ceiling_info == room) ||
        (segment.type == Segment::WALL    && segment.wall_info[0] == room)) {
      for (const auto& triangle : segment.triangles) {
        FillOccupancy(segment.vertices[triangle.indices[0]],
                      segment.vertices[triangle.indices[1]],
                      segment.vertices[triangle.indices[2]],
                      min_xyz,
                      voxel_unit,
                      size,
                      &occupancy);
      }
    }
  }
  
  // Expand occupancy.
  const int kExpandMargin = 1;
  {
    vector<bool> vbtmp;

    for (int t = 0; t < kExpandMargin; ++t) {
      vbtmp = occupancy;
      for (int z = 1; z < size[2] - 1; ++z) {
        for (int y = 1; y < size[1] - 1; ++y) {
          for (int x = 1; x < size[0] - 1; ++x) {
            const int index = z * (size[0] * size[1]) + y * size[0] + x;
            if (vbtmp[index] ||
                vbtmp[index - 1] ||
                vbtmp[index + 1] ||
                vbtmp[index - size[0]] ||
                vbtmp[index + size[0]] ||
                vbtmp[index - size[0] * size[1]] ||
                vbtmp[index + size[0] * size[1]]) {
              occupancy[index] = true;
            }
          }
        }
      }
    }
  }

  //----------------------------------------------------------------------
  for (int p = 0; p < (int)points.size(); ++p) {
    if (segments->at(p) != kInitial)
      continue;
    Vector3d cell_coord = (points[p].position - min_xyz) / voxel_unit;
    Vector3d cell_coord_int;
    for (int a = 0; a < 3; ++a) {
      cell_coord_int[a] = max(0, min(size[a] - 1, static_cast<int>(round(cell_coord[a]))));
    }
    const int index =
      cell_coord_int[2] * (size[0] * size[1]) + cell_coord_int[1] * size[0] + cell_coord_int[0];
    if (occupancy[index])
      segments->at(p) = kDetail;
  }
}
  
void FilterNoisyPoints(std::vector<Point>* points) {
  const int kNumNeighbors = 20;

  vector<float> point_data;
  {
    point_data.reserve(3 * points->size());
    for (int p = 0; p < points->size(); ++p) {
      for (int i = 0; i < 3; ++i)
        point_data.push_back(points->at(p).position[i]);
    }
  }
  KDtree kdtree(point_data);
  vector<const float*> knn;
  vector<float> neighbor_distances(points->size());
  for (int p = 0; p < points->size(); ++p) {
    knn.clear();
    const Vector3f ref_point(points->at(p).position[0],
                             points->at(p).position[1],
                             points->at(p).position[2]);
                      
    kdtree.find_k_closest_to_pt(knn, kNumNeighbors, &ref_point[0]);
                               
    double neighbor_distance = 0.0;
    for (int i = 0; i < knn.size(); ++i) {
      const float* fp = knn[i];
      const Vector3f point(fp[0], fp[1], fp[2]);
      neighbor_distance += (point - ref_point).norm();
    }
    neighbor_distances[p] = neighbor_distance / max(1, (int)knn.size());
  }
  //----------------------------------------------------------------------
  double average = 0.0;
  for (int p = 0; p < neighbor_distances.size(); ++p)
    average += neighbor_distances[p];
  average /= neighbor_distances.size();
  double deviation = 0.0;
  for (int p = 0; p < neighbor_distances.size(); ++p) {
    const double diff = neighbor_distances[p] - average;
    deviation += diff * diff;
  }
  deviation /= neighbor_distances.size();
  deviation = sqrt(deviation);

//  const double threshold = average + 0.5 * deviation;
  const double threshold = average + 4.0 * deviation;
  vector<Point> new_points;
  for (int p = 0; p < neighbor_distances.size(); ++p) {
    if (neighbor_distances[p] <= threshold) {
      new_points.push_back(points->at(p));
    }
  }
  points->swap(new_points);
}

void Subsample(const double ratio, std::vector<Point>* points) {
  const int new_size = static_cast<int>(round(ratio * points->size()));
  random_shuffle(points->begin(), points->end());
  points->resize(new_size);
}
  
void SegmentObjects(const std::vector<Point>& points,
                    const double centroid_subsampling_ratio,
                    const int num_initial_clusters,
                    const std::vector<std::vector<int> >& neighbors,
                    std::vector<int>* segments) {
  // WritePointsWithColor(points, *segments, "0_first.ply");
  InitializeCentroids(points, num_initial_clusters, segments);
  // WriteObjectPointsWithColor(points, *segments, "1_init.ply");

  /*
  {
    ofstream ofstr;
    ofstr.open("test.obj");
    for (int i = 0; i < segments->size(); ++i)
      if (segments->at(i) >= 0) {
        ofstr << "v "
             << points[i].position[0] << ' '
             << points[i].position[1] << ' '
             << points[i].position[2] << endl;
      }
    ofstr.close();
  }
  */
    
  AssignFromCentroids(points, neighbors, segments);
  map<int, Vector3i> color_table;
  // WriteObjectPointsWithColor(points, *segments, "2_seed.ply", &color_table);

  // Repeat K-means.
  const int kTimes = 150;
  for (int t = 0; t < kTimes; ++t) {
    // Sample representative centers in each cluster.
    ComputeCentroids(points, centroid_subsampling_ratio, segments);
    // Assign remaining samples to clusters.
    AssignFromCentroids(points, neighbors, segments);
    // Merge check.
    const bool merged = Merge(points, neighbors, segments, &color_table);
    // SaveData(3 + t, points, *segments);

    /*
    char buffer[1024];
    sprintf(buffer, "%d_ite.ply", 3 + t);
    WriteObjectPointsWithColor(points, *segments, buffer, &color_table);
    */

    if (!merged)
      break;
  }  
}

void SmoothObjects(const std::vector<std::vector<int> >& neighbors,
                   std::vector<Point>* points) {
  double unit = 0.0;
  int denom = 0;
  for (int p = 0; p < points->size(); ++p) {
    for (int i = 0; i < neighbors[p].size(); ++i) {
      unit += (points->at(p).position - points->at(neighbors[p][i]).position).norm();
      ++denom;
    }
  }
  unit /= denom;
  const double sigma = 2.0 * unit;

  // Smooth normals.
  vector<Point> new_points = *points;
  for (int p = 0; p < points->size(); ++p) {
    for (int i = 0; i < neighbors[p].size(); ++i) {
      const int q = neighbors[p][i];
      const double weight = exp(- (points->at(p).position - points->at(q).position).squaredNorm() / (2 * sigma * sigma));
      new_points[p].normal += weight * points->at(q).normal;
    }
    if (new_points[p].normal != Vector3d(0, 0, 0))
      new_points[p].normal.normalize();
  }
  *points = new_points;

  // Smooth positions.
  for (int p = 0; p < points->size(); ++p) {
    double total_weight = 1.0;
    for (int i = 0; i < neighbors[p].size(); ++i) {
      const int q = neighbors[p][i];
      // Estimate the position along the normal.
      const Vector3d intersection = Intersect(points->at(p), points->at(q));
      const double weight = exp(- (points->at(p).position - points->at(q).position).squaredNorm() / (2 * sigma * sigma));
      new_points[p].position += weight * intersection;
      total_weight += weight;
    }
    new_points[p].position /= total_weight;
  }
  *points = new_points;
}
  
// This function calls makes neighbors invalid.
void DensifyObjects(const std::vector<std::vector<int> >& neighbors,
                    std::vector<Point>* points,
                    std::vector<int>* segments) {
  double unit = 0.0;
  int denom = 0;
  for (int p = 0; p < points->size(); ++p) {
    for (int i = 0; i < neighbors[p].size(); ++i) {
      unit += (points->at(p).position - points->at(neighbors[p][i]).position).norm();
      ++denom;
    }
  }
  unit /= denom;
  const double max_radius = unit * 2;

  // Generate points in nearby space.
  const int num_points = points->size();
  for (int p = 0; p < num_points; ++p) {
    // Only densify object points.
    if (segments->at(p) < 0)
      continue;

    Vector3d x_axis;
    const Vector3d normal = points->at(p).normal;
    if (fabs(normal[0]) < fabs(normal[1]) && fabs(normal[0]) < fabs(normal[2]))
      x_axis = Vector3d(0, normal[2], -normal[1]);
    else if (fabs(normal[1]) < fabs(normal[2]) && fabs(normal[1]) < fabs(normal[0]))
      x_axis = Vector3d(-normal[2], 0, normal[0]);
    else
      x_axis = Vector3d(normal[1], -normal[0], 0);
    x_axis.normalize();
    Vector3d y_axis = x_axis.cross(normal);

    vector<double> distances;
    for (int i = 0; i < neighbors[p].size(); ++i) {
      distances.push_back((points->at(p).position - points->at(neighbors[p][i]).position).norm());
    }
    sort(distances.begin(), distances.end());
    const double unit_per_point = distances[distances.size() / 2];
    const double radius = min(unit_per_point / 2.0, max_radius);

    for (int j = -1; j <= 1; ++j) {
      for (int i = -1; i <= 1; ++i) {
        if (i == 0 && j == 0)
          continue;
        Point new_point = points->at(p);
        Vector3d move = x_axis * i + y_axis * j;
        move.normalize();
        new_point.position += move * radius;
        points->push_back(new_point);
        segments->push_back(segments->at(p));
      }
    }
  }
}  
  
void SetNeighbors(const std::vector<Point>& points,
                  const int num_neighbors,
                  std::vector<std::vector<int> >* neighbors) {
  vector<float> point_data;
  {
    point_data.reserve(3 * points.size());
    for (int p = 0; p < points.size(); ++p) {
      for (int i = 0; i < 3; ++i)
        point_data.push_back(points[p].position[i]);
    }
  }
  KDtree kdtree(point_data);
  vector<const float*> knn;
  vector<float> neighbor_distances(points.size());

  neighbors->clear();
  neighbors->resize(points.size());

  for (int p = 0; p < points.size(); ++p) {
    knn.clear();
    const Vector3f ref_point(points[p].position[0],
                             points[p].position[1],
                             points[p].position[2]);
                      
    kdtree.find_k_closest_to_pt(knn, num_neighbors, &ref_point[0]);

    for (int i = 0; i < (int)knn.size(); ++i) {
      const int index = (knn[i] - &point_data[0]) / 3;
      neighbors->at(p).push_back(index);
    }
  }
}


namespace {

bool IsOnFloor(const double floor_height, const double margin, const Point& point) {
  return (point.position[2] - floor_height) <= margin;
}
  
bool IsOnWall(const std::vector<cv::Point2f>& contour,
              const double wall_margin,
              const Point& point) {
  const cv::Point2f local(point.position[0], point.position[1]);
  return fabs(cv::pointPolygonTest(contour, local, true)) <= wall_margin;
}

bool IsOnCeiling(const double ceiling_height, const double margin, const Point& point) {
  return (ceiling_height - point.position[2]) <= margin;
}  

double PointDistance(const Point& lhs, const Point& rhs) {
  const Vector3d diff = lhs.position - rhs.position;
  const Vector3d lhs_normal_diff = lhs.normal * lhs.normal.dot(diff);
  const Vector3d rhs_normal_diff = rhs.normal * rhs.normal.dot(diff);
  const Vector3d lhs_tangent_diff = diff - lhs_normal_diff;
  const Vector3d rhs_tangent_diff = diff - rhs_normal_diff;

  // Suppress distance along tangential.
  const double kSuppressRatio = 0.25;

  return kSuppressRatio * (lhs_tangent_diff.norm() + rhs_tangent_diff.norm()) +
    (lhs_normal_diff.norm() + rhs_normal_diff.norm());
}

void InitializeCentroids(const std::vector<Point>& points,
                         const int num_initial_clusters,
                         std::vector<int>* segments) {
  // Randomly initialize seeds.
  /*
  vector<int> seeds;
  {
    for (int p = 0; p < segments->size(); ++p)
      if (segments->at(p) == kInitial)
        seeds.push_back(p);

    random_shuffle(seeds.begin(), seeds.end());
    seeds.resize(num_initial_clusters);
  }
  for (int c = 0; c < (int)seeds.size(); ++c) {
    segments->at(seeds[c]) = c;
  }
  */

  // Randomly initialize seeds.
  vector<int> seeds;
  {
    vector<int> candidates;
    for (int p = 0; p < segments->size(); ++p)
      if (segments->at(p) == kInitial)
        candidates.push_back(p);

    // Pick the first one at random.
    random_shuffle(candidates.begin(), candidates.end());
    if (candidates.empty())
      return;

    vector<bool> chosen(candidates.size(), false);

    const int kFirstIndex = 0;
    chosen[kFirstIndex] = true;
    seeds.push_back(candidates[kFirstIndex]);
    for (int s = 1; s < num_initial_clusters; ++s) {
      // Pick the farthest point.
      int best_index = -1;
      double max_distance = 0.0;
      const int kSkip = 3;
      for (int i = 0; i < (int)candidates.size(); i += kSkip) {
        if (chosen[i])
          continue;
        const int candidate = candidates[i];
        double sum_distance = numeric_limits<double>::max();
        for (const auto& seed : seeds) {
          sum_distance = min(sum_distance, (points[seed].position - points[candidate].position).norm());
        }
        if (max_distance < sum_distance) {
          max_distance = sum_distance;
          best_index = i;
        }
      }
      if (best_index != -1) {
        chosen[best_index] = true;
        seeds.push_back(candidates[best_index]);
      }
    }
  }
  
  for (int c = 0; c < (int)seeds.size(); ++c) {
    segments->at(seeds[c]) = c;
  }
}
  
const double kUnreachable = -1.0;
void ComputeDistances(const std::vector<Point>& points,
                      const std::vector<std::vector<int> >& neighbors,
                      const int index,
                      const std::vector<int>& segments,
                      std::vector<double>* distances) {
  distances->clear();
  distances->resize(points.size(), kUnreachable);
  
  priority_queue<pair<double, int> > distance_index_queue;
  distance_index_queue.push(make_pair(0.0, index));

  while (!distance_index_queue.empty()) {
    const auto distance_index = distance_index_queue.top();
    const double distance = -distance_index.first;
    const int point_index = distance_index.second;

    distance_index_queue.pop();
    // Already distance assigned.
    if (distances->at(point_index) != kUnreachable)
      continue;
    // ???? Stop at the boundary
    if (segments[point_index] == kFloor ||
        segments[point_index] == kWall ||
        segments[point_index] == kCeiling)
      continue;        

    distances->at(point_index) = distance;
    for (int i = 0; i < neighbors[point_index].size(); ++i) {
      const int neighbor = neighbors[point_index][i];
      if (distances->at(neighbor) != kUnreachable)
        continue;

      const double new_distance = distance + PointDistance(points[point_index], points[neighbor]);
      distance_index_queue.push(make_pair(- new_distance, neighbor));
    }
  }
}

void AssignFromCentroids(const std::vector<Point>& points,
                         const std::vector<std::vector<int> >& neighbors,
                         std::vector<int>* segments) {
  // cluster_pair_distances->clear();
  // For each unassigned point, compute distance from centroids. Use
  // the top 25 percents average to assign to the closest centroid.
  const double kSumRatio = 0.25;

  vector<map<int, vector<double> > > point_cluster_distances(points.size());
  vector<double> distances;
//  cerr << "Compute distances..." << flush;
  for (int p = 0; p < segments->size(); ++p) {
//    if (p % (segments->size() / 10) == 0)
//      cerr << '.' << flush;
    // Only compute distances to neighbors at the seed points.
    const int cluster = segments->at(p);
    if (cluster < 0)
      continue;

    ComputeDistances(points, neighbors, p, *segments, &distances);
    for (int p = 0; p < distances.size(); ++p) {
      if (distances[p] != kUnreachable) {
        point_cluster_distances[p][cluster].push_back(distances[p]);
      }
    }
  }
//  cerr << "done." << endl;

  // For each point, compute the per cluster distance and pick the best.
  for (int p = 0; p < segments->size(); ++p) {
    if (segments->at(p) != kInitial)
      continue;

    const map<int, vector<double> >& cluster_distances = point_cluster_distances[p];
    double best_distance = 0.0;
    int best_cluster = kInitial;
    for (const auto& item : cluster_distances) {
      const int cluster        = item.first;
      vector<double> distances = item.second;
      if (distances.empty()) {
        cerr << "Impossible." << endl;
        exit (1);
      }
      sort(distances.begin(), distances.end());
      const int length_to_sum = max(1, static_cast<int>(round(distances.size() * kSumRatio)));
      double average_distance = 0.0;
      for (int i = 0; i < length_to_sum; ++i) {
        average_distance += distances[i];
      }
      average_distance /= length_to_sum;
      if (best_cluster == kInitial || average_distance <= best_distance) {
        best_cluster = cluster;
        best_distance = average_distance;
      }
    }
    segments->at(p) = best_cluster;
  }

  
  /*
  // Initialize remaining.
  //vector<pair<double, int> > distance_segment(segments->size(), pair<double, int>(0.0, kInitial));
  
  priority_queue<pair<double, pair<int, int> > > distance_point_segment_queue;
  for (int c = 0; c < (int)seeds.size(); ++c) {
    const int point_index = seeds[c];
    // distance_segment[point_index] = pair<double, int>(0.0, c);
    const pair<int, int> point_segment(point_index, c);
    distance_point_segment_queue.push(pair<double, pair<int, int> >(0, point_segment));
  }

  while (!distance_point_segment_queue.empty()) {
    const auto distance_point_segment = distance_point_segment_queue.top();
    const double distance = - distance_point_segment.first;
    const int point_index = distance_point_segment.second.first;
    const int cluster     = distance_point_segment.second.second;
    distance_point_segment_queue.pop();

    if (segments->at(point_index) != kInitial)
      continue;

    segments->at(point_index) = cluster;
    
    for (int i = 0; i < neighbors[point_index].size(); ++i) {
      const int neighbor = neighbors[point_index][i];
      if (segments->at(neighbor) != kInitial)
        continue;

      const double new_distance = distance + PointDistance(points[point_index], points[neighbor]);
      distance_point_segment_queue.push(pair<double, pair<int, int> >(-new_distance,
                                                                      make_pair(neighbor, cluster)));
    }
  }
  */
}  

void ComputeCentroids(const std::vector<Point>& points, const double ratio, vector<int>* segments) {
  map<int, vector<int> > cluster_to_centroids;
  for (int p = 0; p < segments->size(); ++p) {
    const int cluster = segments->at(p);
    if (cluster >= 0)
      cluster_to_centroids[cluster].push_back(p);
  }

  for (int p = 0; p < segments->size(); ++p) {
    const int cluster = segments->at(p);
    if (cluster >= 0)
      segments->at(p) = kInitial;
  }

  for (const auto& item : cluster_to_centroids) {
    const int cluster = item.first;
    Vector3d center(0, 0, 0);
    for (const auto& p : item.second)
      center += points[p].position;

    center /= item.second.size();

    // Find the closest.
    double closest_distance = 0.0;
    const int kInvalid = -1;
    int closest_index = kInvalid;
    for (const auto& p : item.second) {
      const double distance = (center - points[p].position).norm();
      if (closest_index == kInvalid || distance < closest_distance) {
        closest_distance = distance;
        closest_index = p;
      }
    }
    if (closest_index == kInvalid) {
      cerr << "Impo" << endl;
      exit (1);
    }
    segments->at(closest_index) = cluster;
  }
  

  /*
  for (auto& cluster_to_centroid: cluster_to_centroids) {
    const int cluster = cluster_to_centroid.first;
    vector<int>& centroids = cluster_to_centroid.second;
    if (cluster == kInitial)
      continue;
    const int kMinimumCentroidSize = 2;
    if (centroids.size() < kMinimumCentroidSize)
      continue;
    
    random_shuffle(centroids.begin(), centroids.end());
    // We need at least 2 centroids so that intra cluster distance can be computed.
    const int new_size = max(kMinimumCentroidSize, static_cast<int>(round(centroids.size() * ratio)));
    centroids.resize(new_size);
    
    for (const auto centroid : centroids) {
      segments->at(centroid) = cluster;
    }
  }
  */
}


void UnionFind(const std::set<std::pair<int, int> >& merged,
               const int max_old,
               std::map<int, int>* old_to_new) {
  vector<int> smallests(max_old);
  for (int old = 0; old < max_old; ++old) {
    const int kInvalid = -1;
    int smallest = old;
    for (const auto& merge : merged) {
      const int lhs = merge.first;
      const int rhs = merge.second;
      if (rhs == old && smallests[lhs] < smallest) {
        smallest = smallests[lhs];
      }
    }
    smallests[old] = smallest;
  }

  set<int> unique_ids;
  for (int i = 0; i < smallests.size(); ++i)
    unique_ids.insert(smallests[i]);

  map<int, int> unique_to_new;
  int new_id = 0;
  for (const auto& id : unique_ids) {
    unique_to_new[id] = new_id;
    ++new_id;
  }

  for (int i = 0; i < max_old; ++i) {
    if (unique_to_new.find(smallests[i]) == unique_to_new.end()) {
      cerr << "bug" << endl;
      exit (1);
    }
    (*old_to_new)[i] = unique_to_new[smallests[i]];
  }
}

bool Merge(const std::vector<Point>& points,
           const std::vector<std::vector<int> >& neighbors,
           std::vector<int>* segments,
           map<int, Vector3i>* color_table) {
  map<pair<int, int>, vector<double> > cluster_pair_to_distances;
  for (int p0 = 0; p0 < (int)neighbors.size(); ++p0) {
    const int segment0 = segments->at(p0);
    if (segment0 < 0)
      continue;
    for (int i = 0; i < (int)neighbors[p0].size(); ++i) {
      const int p1 = neighbors[p0][i];
      const int segment1 = segments->at(p1);
      if (segment1 < 0)
        continue;

      cluster_pair_to_distances[make_pair(min(segment0, segment1), max(segment0, segment1))].
        push_back(PointDistance(points[p0], points[p1]));
    }
  }

  map<pair<int, int>, double> cluster_pair_to_average_distance;
  for (auto& item : cluster_pair_to_distances) {
    double average = 0.0;
    vector<double> distances = item.second;

    /*
    for (int i = 0; i < distances.size(); ++i)
      average += distances[i];
    average /= distances.size();
    */
    nth_element(distances.begin(),
                distances.begin() + distances.size() / 4,
                distances.end());
    cluster_pair_to_average_distance[item.first] = *(distances.begin() + distances.size() / 4);
  }

  double average_inter_distance = 0.0;
  {
    int denom = 0;
    for (auto& item : cluster_pair_to_distances) {
      if (item.first.first != item.first.second)
        continue;
      const vector<double>& distances = item.second;
      average_inter_distance += std::accumulate(distances.begin(), distances.end(), 0.0);
      denom += distances.size();
    }
    average_inter_distance /= max(1, denom);
  }
//  cerr << "Average inter distance: " << average_inter_distance << endl;

  // Merge test.
  //????
  const double kMergeRatio = 1.5; // 2.0;
  set<pair<int, int> > merged;

  pair<int, int> best_pair;
  double best_size = -1;
  double best_size01 = -1;
  Vector2d best_size_pair;
  Vector3d best_distance;
  set<int> merged_clusters;
  for (const auto& item : cluster_pair_to_distances) {
    const int p0 = item.first.first;
    const int p1 = item.first.second;
    if (p0 == p1)
      continue;
    const int size01 = item.second.size();
    const int size0 = cluster_pair_to_distances[make_pair(p0, p0)].size();
    const int size1 = cluster_pair_to_distances[make_pair(p1, p1)].size();

    const double distance01 = cluster_pair_to_average_distance[item.first];
    const double distance0  = cluster_pair_to_average_distance[make_pair(p0, p0)];
    const double distance1  = cluster_pair_to_average_distance[make_pair(p1, p1)];

    //???
    //if (distance01 < distance0 * kMergeRatio && distance01 < distance1 * kMergeRatio)
    if (distance01 < average_inter_distance * kMergeRatio) {
      if (merged_clusters.find(item.first.first) != merged_clusters.end() ||
          merged_clusters.find(item.first.second) != merged_clusters.end())
        continue;
      
      merged.insert(item.first);
      //??? only one each.
      //break;
      if (min(size0, size1) > best_size) {
	best_size = min(size0, size1);
	best_size_pair = Vector2d(size0, size1);
        best_size01 = size01;
	best_pair = item.first;
        best_distance = Vector3d(distance0, distance1, distance01);
      }
    }
    /*
    const double ratio = distance01 / distance0 + distance01 / distance1;
    if (ratio < best_ratio) {
      best_ratio = ratio;
      best_pair = item.first;
    }
    */
  }
  if (merged.empty())
    return false;
//  cerr << "merged: " << merged.size() << " pairs." << endl;
  
  int max_cluster_id = 0;
  {
    for (const auto& item : *segments) {
      max_cluster_id = max(max_cluster_id, item);
    }
    ++max_cluster_id;
  }
  map<int, int> old_to_new;
  UnionFind(merged, max_cluster_id, &old_to_new);

  /*
  {
    vector<Point> merged_points;
    for (int i = 0; i < points.size(); ++i) {
      if (segments->at(i) == best_pair.first) {
        Point point = points[i];
        point.color = Vector3f(0, 255, 0);
        merged_points.push_back(point);
      } else if (segments->at(i) == best_pair.second) {
        Point point = points[i];
        point.color = Vector3f(255, 0, 255);
        merged_points.push_back(point);
      }
    }
    static int count = 3;
    PointCloud pc;
    pc.SetPoints(merged_points);
    char buffer[1024];
    sprintf(buffer, "%d_merged.ply", count++);
    pc.Write(buffer);
  }
  */
  
  for (int i = 0; i < segments->size(); ++i) {
    if (segments->at(i) >= 0) {
      segments->at(i) = old_to_new[segments->at(i)];
    }
  }

  map<int, Vector3i> new_color_table;
  for (const auto& item : old_to_new) {
    new_color_table[item.second] = (*color_table)[item.first];
  }
  *color_table = new_color_table;
  
  return true;
  
  /*
  // Compute a list of possible clusters to be merged.
  map<pair<int, int>, double> averages;
  for (const auto& item : cluster_pair_distances) {
    const pair<int, int>& cluster_pair = item.first;
    double average = 0.0;
    for (const auto distance : item.second)
      average += distance;
    average /= item.second.size();
    averages[cluster_pair] = average;
  }

  set<pair<int, int> > merged;
  for (const auto& item : cluster_pair_distances) {
    const pair<int, int>& cluster_pair = item.first;
    const int lhs = cluster_pair.first;
    const int rhs = cluster_pair.second;

    if (averages.find(make_pair(lhs, lhs)) == averages.end()) {
      cerr << "Impossible." << endl;
    }
    if (averages.find(make_pair(rhs, rhs)) == averages.end()) {
      cerr << "Impossible." << endl;
    }
    
    if (averages[cluster_pair] < 2 * averages[make_pair(lhs, lhs)] &&
        averages[cluster_pair] < 2 * averages[make_pair(rhs, rhs)]) {
      merged.insert(cluster_pair);
    }
  }

  
  int max_cluster_id = 0;
  for (const auto& item : *segments) {
    max_cluster_id = max(max_cluster_id, item);
  }
  map<int, int> old_to_new;
  UnionFind(merged, max_cluster_id, &old_to_new);
  
  for (int i = 0; i < segments->size(); ++i) {
    if (segments->at(i) >= 0) {
      segments->at(i) = old_to_new[segments->at(i)];
    }
  }
  */
}

Eigen::Vector3d Intersect(const Point& lhs, const Point& rhs) {
  // Ray is point = lhs.position + lhs.normal * d.
  // rhs.normal * (point - rhs.position)

  const Vector3d diff = lhs.position - rhs.position;
  const Vector3d diff_along_rhs_normal = rhs.normal.dot(diff) * rhs.normal;
  const Vector3d final_diff = lhs.normal.dot(diff_along_rhs_normal) * lhs.normal;
  return lhs.position - final_diff;
}

void FillOccupancy(const Eigen::Vector3d& v0,
                   const Eigen::Vector3d& v1,
                   const Eigen::Vector3d& v2,
                   const Eigen::Vector3d& min_xyz,
                   const double voxel_unit,
                   const Eigen::Vector3i& size,
                   std::vector<bool>* occupancy) {
  const int kScale = 2;
  const int samples01 = max(2, kScale * static_cast<int>((v1 - v0).norm() / voxel_unit));
  const int samples02 = max(2, kScale * static_cast<int>((v2 - v0).norm() / voxel_unit));

  const int along_sample = max(samples01, samples02);

  for (int s = 0; s <= along_sample; ++s) {
    Vector3d v01 = v0 + (v1 - v0) * s / along_sample;
    Vector3d v02 = v0 + (v2 - v0) * s / along_sample;

    const int perp_sample = max(2, kScale * static_cast<int>((v02 - v01).norm() / voxel_unit));
    for (int t = 0; t <= perp_sample; ++t) {
      Vector3d v = v01 + (v02 - v01) * t / perp_sample;

      Vector3d cell_coord = (v - min_xyz) / voxel_unit;
      Vector3d cell_coord_int;
      for (int a = 0; a < 3; ++a) {
        cell_coord_int[a] = max(0, min(size[a] - 1, static_cast<int>(round(cell_coord[a]))));
      }

      const int index =
        cell_coord_int[2] * (size[0] * size[1]) + cell_coord_int[1] * size[0] + cell_coord_int[0];
      occupancy->at(index) = true;
    }
  }
}

}  // namespace

void RemoveWindowAndMirror(const Floorplan& floorplan,
                           const vector<int>& room_occupancy_with_doors,
                           const Eigen::Vector3d& center,
                           PointCloud* point_cloud) {
  const Vector2d center_grid = floorplan.LocalToGrid(Vector2d(center[0], center[1]));
  
  vector<int> remove_indexes;

  const int width  = floorplan.GetGridSize()[0];
  const int height = floorplan.GetGridSize()[1];

  for (int p = 0; p < point_cloud->GetNumPoints(); ++p) {
    const Point& point = point_cloud->GetPoint(p);
    const Vector2d grid = floorplan.LocalToGrid(Vector2d(point.position[0], point.position[1]));

    // If [ center_grid -> grid ] crosses a room boundary or not.
    const int sample = static_cast<int>(round((grid - center_grid).norm() / 2));

    // room_occupancy must be [ room ... | non-room ... | room ... ]
    int block_counts[3] = {0, 0, 0};
    int block = 0;
    for (int s = 0; s < sample; ++s) {
      const Vector2d current = (grid - center_grid) * s / sample + center_grid;
      const int x = max(0, min(width - 1, static_cast<int>(round(current[0]))));
      const int y = max(0, min(height - 1, static_cast<int>(round(current[1]))));

      const int occupancy = room_occupancy_with_doors[y * width + x];
      
      if (block == 0) {
        if (occupancy < 0) {
          block = 1;
        }
      } else if (block == 1) {
        if (occupancy >= 0) {
          block = 2;
        }
      } else if (block == 2) {
        if (occupancy < 0)
          break;
      }
      
      ++block_counts[block];
    }

    // Remove condition.
    if (block_counts[0] >= 2 && block_counts[1] >= 2) //  && block_counts[2] >= 2)
      remove_indexes.push_back(p);
  }

  /*
  {
  static int c = 0;
    ofstream ofstr;
    char buffer[1024];
    sprintf(buffer, "remove_%03d.obj", c++);
    ofstr.open(buffer);

    for (const auto& value : remove_indexes) {
      ofstr << "v "
            << point_cloud->GetPoint(value).position[0] << ' '
            << point_cloud->GetPoint(value).position[1] << ' '
            << point_cloud->GetPoint(value).position[2] << endl;
    }

    ofstr.close();
  }
  */

  point_cloud->RemovePoints(remove_indexes);
}
  
void WriteObjectPointsWithColor(const std::vector<Point>& points,
                                const std::vector<int>& segments,
                                const std::string& filename,
                                const Eigen::Matrix3d& rotation,
                                map<int, Vector3i>* color_table) {  
  if (points.size() != segments.size()) {
    cerr << "Size do not match: " << (int)points.size() << ' ' << (int)segments.size() << endl;
    exit (1);
  }

  vector<Point> object_points;
  for (int p = 0; p < points.size(); ++p) {
    Point point = points[p];
    switch (segments[p]) {
    case kInitial:
    case kFloor:
    case kWall:
    case kCeiling:
    case kDetail: {
      break;
    }
    default: {
      if ((*color_table).find(segments[p]) == (*color_table).end()) {
        (*color_table)[segments[p]][0] = rand() % 255;
        (*color_table)[segments[p]][1] = rand() % 255;
        (*color_table)[segments[p]][2] = rand() % 255;
      }
      
      point.color[0] = (*color_table)[segments[p]][0];
      point.color[1] = (*color_table)[segments[p]][1];
      point.color[2] = (*color_table)[segments[p]][2];
      point.object_id = segments[p];
      object_points.push_back(point);
      break;
    }
    }
  }

  //number of object, should be the largest element of vector segments
  int num_object = *max_element(segments.begin(), segments.end());
  PointCloud pc;
  pc.SetPoints(object_points);
  pc.Rotate(rotation);
  pc.Write(filename);
}

void WriteOtherPointsWithColor(const std::vector<Point>& points,
                               const std::vector<int>& segments,
                               const std::string& filename,
                               const Eigen::Matrix3d& rotation) {
  vector<Point> other_points;
  for (int p = 0; p < points.size(); ++p) {
    Point point = points[p];
    switch (segments[p]) {
    case kFloor: {
      point.color = Vector3f(0, 0, 255);
      point.object_id = 0;
      break;
    }
    case kWall: {
      point.color = Vector3f(0, 255, 0);
      point.object_id = 0;
      break;
    }
    case kCeiling: {
      point.color = Vector3f(255, 0, 0);
      break;
    }
    case kInitial: {
      point.color = Vector3f(255, 255, 255);
      point.object_id = 0;
      break;
    }
    case kDetail: {
      point.color = Vector3f(0, 255, 255);
      point.object_id = 0;
      break;
    }
    default: {
      continue;
    }
    }
    other_points.push_back(point);
  }

  PointCloud pc;
  pc.SetPoints(other_points);
  pc.Rotate(rotation);
  pc.Write(filename);
}
  
}  // namespace structured_indoor_modeling
