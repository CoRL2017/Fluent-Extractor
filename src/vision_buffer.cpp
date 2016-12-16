#include "ros/ros.h"
#include "FileFrameScanner.h"
#include "CommonTools.h"
#include <opencv2/opencv.hpp>
#include <sstream>
#include <pcl/visualization/cloud_viewer.h>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <iostream>
#include "rapidjson/filereadstream.h"
#include "Seg2D.h"
#include <cstdio>

using namespace std;
using namespace cv;
using namespace rapidjson;

int main(int argc, char **argv) {
  ros::init(argc, argv, "vision_buffer");

  // Read the config file
  FILE* fp = fopen(argv[1], "rb");
  char readBuffer[65536];
  FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  Document json;
  json.ParseStream(is);

  // Define the video directory
  stringstream vids_directory;
  vids_directory << json["vids_directory"].GetString();
  vids_directory << json["vid_idx"].GetInt() << "/";
  FileFrameScanner scanner(vids_directory.str());

  pcl::visualization::CloudViewer viewer("Cloud Viewer");

  Mat img_bgr, x, y, z;
  PointT left_hand, right_hand;
  int img_idx = json["start_frame_idx"].GetInt();

  Mat table_mask;
  Seg2D seg2D;

  while(scanner.get(img_idx++, img_bgr, x, y, z, left_hand, right_hand)) {
    int pixel2voxel[img_bgr.size().area()];
    int voxel2pixel[img_bgr.size().area()];
    CloudPtr cloud_ptr = CommonTools::make_cloud_ptr(img_bgr, x, y, z, pixel2voxel, voxel2pixel);

    if (table_mask.size().area() == 0) {
      CommonTools::find_biggest_plane(cloud_ptr->makeShared(), voxel2pixel, table_mask);
    }

    Mat cloth_mask;
    if (table_mask.size().area() > 0) {
//      CloudPtr table_cloud_ptr = CommonTools::get_pointcloud_from_mask(cloud_ptr->makeShared(), pixel2voxel, table_mask);
//      viewer.showCloud(table_cloud_ptr);

      Mat img_masked;
      img_bgr.copyTo(img_masked, table_mask);
      Mat img_seg = seg2D.seg(img_masked, 0.5, 1000, 50);
      imshow("img", img_seg);
      vector<vector<cv::Point2i> > components;
      CommonTools::get_components(img_seg, components);

      Mat max_component_img;
      int max_component_size = 0;
      Mat table_mask_eroded = table_mask.clone();
      CommonTools::erode(table_mask_eroded, 15);
      for (int i = 0; i < components.size(); i++) {
        // don't allow small components
        if (components[i].size() < 200) continue;


        // midpoint of component should be in table_mask
        Mat component_img = Mat::zeros(img_bgr.size(), CV_8U);
        Point2i midpoint(0, 0);
        for (cv::Point2i p : components[i]) {
          component_img.at<uchar>(p.y, p.x) = 255;
          midpoint.x += p.x;
          midpoint.y += p.y;
        }
        midpoint.x /= components[i].size();
        midpoint.y /= components[i].size();

        if (table_mask_eroded.at<uchar>(midpoint.y, midpoint.x) == 0) continue;

        // component outline should not be too similar to table
        vector<Point> hull;
        convexHull(components[i], hull);
        vector<vector<Point>> hulls;
        hulls.push_back(hull);
        Mat component_img_hull = component_img.clone();
        drawContours(component_img_hull, hulls, 0, Scalar(255), -1);
        double semantic_dist_to_table = CommonTools::shape_dist(table_mask_eroded, component_img_hull);
        if (semantic_dist_to_table < 0.4) continue;

        // save the biggest component
        if (components[i].size() > max_component_size) {
          max_component_size = components[i].size();
          max_component_img = component_img.clone();
        }
      }
      if (max_component_size > 0) {
        imshow("cloth", max_component_img);
        cloth_mask = max_component_img.clone();
      }
    }

    if (cloth_mask.size().area() > 0) {
      CloudPtr cloth_cloud_ptr = CommonTools::get_pointcloud_from_mask(cloud_ptr, pixel2voxel, cloth_mask);
      viewer.showCloud(cloth_cloud_ptr);
    }

    waitKey(60);
  }

  ros::spin();
  return 0;
}

