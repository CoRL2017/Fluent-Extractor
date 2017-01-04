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
#include "pcl_ros/point_cloud.h"

using namespace std;
using namespace cv;
using namespace rapidjson;

class BufferManager {
public:
  BufferManager(int vid_idx, ros::Publisher& pub) {
    m_vid_idx = vid_idx;
    m_pub = pub;
  }

  void process(CloudPtr cloud_ptr, Mat img_bgr, int* pixel2voxel, int* voxel2pixel) {
    if (m_table_mask.size().area() == 0) {
      CommonTools::find_biggest_plane(cloud_ptr->makeShared(), voxel2pixel, m_table_mask, m_table_midpoint, m_table_normal);
      cout << "table midpoint is " << m_table_midpoint << endl;
      cout << "table normal is " << m_table_normal << endl;
    }

    Mat cloth_mask;
    if (m_table_mask.size().area() > 0) {
      CloudPtr table_cloud_ptr = CommonTools::get_pointcloud_from_mask(cloud_ptr->makeShared(), pixel2voxel, m_table_mask);
//      viewer.showCloud(table_cloud_ptr);

      // no obstacles allowed
      double max_dist_from_table = 0;
      int max_dist_vox_idx = -1;
      for (int i = 0; i < table_cloud_ptr->size(); i++) {
        PointT p = table_cloud_ptr->at(i);
        double d = -m_table_normal.x * m_table_midpoint.x -
            m_table_normal.y * m_table_midpoint.y -
            m_table_normal.z * m_table_midpoint.z;
        double dist_from_table = m_table_normal.x * p.x + m_table_normal.y * p.y + m_table_normal.z * p.z + d;
        if (dist_from_table > max_dist_from_table) {
          max_dist_from_table = dist_from_table;
          max_dist_vox_idx = i;
        }
      }
      if (max_dist_from_table > 0.1) {
        cout << "detected obstacle" << endl;
        int obstacle_row, obstacle_col;
        CommonTools::vox2pix(voxel2pixel, max_dist_vox_idx, obstacle_row, obstacle_col, cloud_ptr->width);
        Mat img_with_circle = img_bgr.clone();
        Point p;
        p.x = obstacle_row;
        p.y = obstacle_col;
        circle(img_with_circle, p, 10, Scalar(0, 0, 255), 3);
        imshow("Obstacle detected", img_with_circle);
        waitKey(100);
        return;
      }


      Mat img_masked;
      img_bgr.copyTo(img_masked, m_table_mask);
      Mat img_seg = m_seg2D.seg(img_masked, 0.5, 1000, 50);


      imshow("img_seg", img_seg);
      vector<vector<cv::Point2i> > components;
      CommonTools::get_components(img_seg, components);


      Mat max_component_img;
      int max_component_size = 0;
      Mat table_mask_eroded = m_table_mask.clone();
      CommonTools::erode(table_mask_eroded, 15);


      int table_2d_area = cv::countNonZero(m_table_mask);

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

        // component size should not be bigger than table
        if (components[i].size() > 0.9 * table_2d_area) {
          continue;
        }

        // eroding it shouldn't make it dissapear
        Mat eroded_component = component_img.clone();
        CommonTools::erode(eroded_component, 10);
        if (cv::countNonZero(eroded_component) < 10) {
          continue;
        }

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
    } else {
      cout << "no table found" << endl;
    }

    if (cloth_mask.size().area() > 0) {
      CloudPtr cloth_cloud_ptr = CommonTools::get_pointcloud_from_mask(cloud_ptr, pixel2voxel, cloth_mask);

      stringstream payload;
      payload << m_vid_idx << " "
              << m_table_normal.x << " "
              << m_table_normal.y << " "
              << m_table_normal.z;
      cloth_cloud_ptr->header.frame_id = payload.str();

      m_pub.publish(cloth_cloud_ptr);
    } else {
      cout << "no cloth found" << endl;
    }

    waitKey(60);
  }

  void kinect_callback(const CloudConstPtr& cloud_ptr) {
    Mat img;
    int pixel2voxel[cloud_ptr->width * cloud_ptr->height];
    int voxel2pixel[cloud_ptr->width * cloud_ptr->height];
    Cloud cld = *cloud_ptr->makeShared();
    CommonTools::get_cloud_info(cld, img, pixel2voxel, voxel2pixel);
    imshow("Camera Live Stream", img);
    waitKey(40);
    process(cloud_ptr->makeShared(), img, pixel2voxel, voxel2pixel);
  }
private:
  ros::Publisher m_pub;
  int m_vid_idx;
  Mat m_table_mask;
  PointT m_table_midpoint, m_table_normal;
  Seg2D m_seg2D;
  vector<double> m_cloth_feature;
};


int main(int argc, char **argv) {
  ros::init(argc, argv, "vision_buffer");
  ros::NodeHandle node_handle;

  // Read the config file
  FILE* fp = fopen(argv[1], "rb");
  char readBuffer[65536];
  FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  Document json;
  json.ParseStream(is);

  ros::Publisher pub = node_handle.advertise<Cloud>("vision_buffer_pcl", 1);
  BufferManager buffer_manager(json["vid_idx"].GetInt(), pub);

  if (json["use_kinect"].GetBool()) {
    std::string kinect_topic = node_handle.resolveName(json["kinect_topic"].GetString());
    ros::Subscriber sub = node_handle.subscribe(kinect_topic, 1, &BufferManager::kinect_callback, &buffer_manager);
    ros::spin();
  } else {
    // Define the video directory
    stringstream vids_directory;
    vids_directory << json["vids_directory"].GetString();
    vids_directory << json["vid_idx"].GetInt() << "/";
    FileFrameScanner scanner(vids_directory.str());

    Mat img_bgr, x, y, z;
    PointT left_hand, right_hand;
    int img_idx = json["start_frame_idx"].GetInt();

    while(scanner.get(img_idx++, img_bgr, x, y, z, left_hand, right_hand)) {
      int pixel2voxel[img_bgr.size().area()];
      int voxel2pixel[img_bgr.size().area()];
      CloudPtr cloud_ptr = CommonTools::make_cloud_ptr(img_bgr, x, y, z, pixel2voxel, voxel2pixel);

      buffer_manager.process(cloud_ptr, img_bgr, pixel2voxel, voxel2pixel);

    }
  }

  ros::spin();
  return 0;
}

