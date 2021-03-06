/*
 * Copyright (c) 2010, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ros/ros.h"
#include "pluginlib/class_list_macros.h"
#include "nodelet/nodelet.h"
#include "sensor_msgs/LaserScan.h"
#include "pcl/point_cloud.h"
#include "pcl_ros/point_cloud.h"
#include "pcl/point_types.h"
#include "pcl/ros/conversions.h"
#include "dynamic_reconfigure/server.h"
#include "pointcloud_to_laserscan/CloudScanConfig.h"
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <math.h>

namespace pointcloud_to_laserscan
{
class CloudToScan : public nodelet::Nodelet
{
public:
  //Constructor
  CloudToScan(): min_height_(0.10),
                 max_height_(0.15),
                 angle_min_(-M_PI/2),
                 angle_max_(M_PI/2),
                 angle_increment_(M_PI/180.0/2.0),
                 scan_time_(1.0/30.0),
                 range_min_(0.45),
                 range_max_(10.0),
                 output_frame_id_("/kinect_depth_frame"),
                 ref_frame_id_("/kinect_link")
  {
  };

  ~CloudToScan()
  {
    delete srv_;
  }

private:
  typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;

  boost::mutex connect_mutex_;
  // Dynamic reconfigure server
  dynamic_reconfigure::Server<pointcloud_to_laserscan::CloudScanConfig>* srv_;

  tf::TransformListener listener;
  tf::TransformBroadcaster broadcaster;

  virtual void onInit()
  {
    nh_ = getNodeHandle();
    ros::NodeHandle& private_nh = getPrivateNodeHandle();

    private_nh.getParam("min_height", min_height_);
    private_nh.getParam("max_height", max_height_);

    private_nh.getParam("angle_min", angle_min_);
    private_nh.getParam("angle_max", angle_max_);
    private_nh.getParam("angle_increment", angle_increment_);
    private_nh.getParam("scan_time", scan_time_);
    private_nh.getParam("range_min", range_min_);
    private_nh.getParam("range_max", range_max_);

    range_min_sq_ = range_min_ * range_min_;

    private_nh.getParam("output_frame_id", output_frame_id_);
    private_nh.getParam("ref_frame_id", ref_frame_id_);

    srv_ = new dynamic_reconfigure::Server<pointcloud_to_laserscan::CloudScanConfig>(private_nh);
    dynamic_reconfigure::Server<pointcloud_to_laserscan::CloudScanConfig>::CallbackType f = boost::bind(&CloudToScan::reconfigure, this, _1, _2);
    srv_->setCallback(f);

    // Lazy subscription to point cloud topic
    ros::AdvertiseOptions scan_ao = ros::AdvertiseOptions::create<sensor_msgs::LaserScan>(
      "scan", 10,
      boost::bind( &CloudToScan::connectCB, this),
      boost::bind( &CloudToScan::disconnectCB, this), ros::VoidPtr(), nh_.getCallbackQueue());

    boost::lock_guard<boost::mutex> lock(connect_mutex_);
    pub_ = nh_.advertise(scan_ao);
  };

  void connectCB() {
      boost::lock_guard<boost::mutex> lock(connect_mutex_);
      if (pub_.getNumSubscribers() > 0) {
          NODELET_DEBUG("Connecting to point cloud topic.");
          sub_ = nh_.subscribe<PointCloud>("cloud", 10, &CloudToScan::callback, this);
      }
  }

  void disconnectCB() {
      boost::lock_guard<boost::mutex> lock(connect_mutex_);
      if (pub_.getNumSubscribers() == 0) {
          NODELET_DEBUG("Unsubscribing from point cloud topic.");
          sub_.shutdown();
      }
  }

  void reconfigure(pointcloud_to_laserscan::CloudScanConfig &config, uint32_t level)
  {
    min_height_ = config.min_height;
    max_height_ = config.max_height;
    angle_min_ = config.angle_min;
    angle_max_ = config.angle_max;
    angle_increment_ = config.angle_increment;
    scan_time_ = config.scan_time;
    range_min_ = config.range_min;
    range_max_ = config.range_max;

    range_min_sq_ = range_min_ * range_min_;
  }

  void callback(const PointCloud::ConstPtr& cloud)
  {
    sensor_msgs::LaserScanPtr output(new sensor_msgs::LaserScan());
    output->header = cloud->header;
    output->header.frame_id = output_frame_id_; // Set output frame. Point clouds come from "optical" frame, scans come from corresponding mount frame
    output->angle_min = angle_min_;
    output->angle_max = angle_max_;
    output->angle_increment = angle_increment_;
    output->time_increment = 0.0;
    output->scan_time = scan_time_;
    output->range_min = range_min_;
    output->range_max = range_max_;

    uint32_t ranges_size = std::ceil((output->angle_max - output->angle_min) / output->angle_increment);
    output->ranges.assign(ranges_size, output->range_max + 1.0);

    // transform from camera into reference frame
    tf::StampedTransform cloud_to_ref;
    try{
      listener.waitForTransform(ref_frame_id_, cloud->header.frame_id, cloud->header.stamp, ros::Duration(1.0) );
      listener.lookupTransform(ref_frame_id_, cloud->header.frame_id, cloud->header.stamp, cloud_to_ref);
    }
    catch (tf::TransformException ex){
      ROS_ERROR("%s",ex.what());
    }

    // compute translation of virtual laser frame
    // x,y come from camera frame
    // z is between min/max height
    tf::Vector3 ref_origin = cloud_to_ref.getOrigin();
    ref_origin.setZ( (min_height_+max_height_)*0.5 );

    // compute orientation of virtual laser frame
    // rotation comes from the z axis of the optical camera frame
    tf::Vector3 z_axis(0, 0, 1);
    tf::Transform camera_rot(cloud_to_ref.getRotation());
    tf::Vector3 rotated_z_axis = camera_rot * z_axis;
    double alpha = atan2(rotated_z_axis.y(), rotated_z_axis.x());
    tf::Quaternion ref_ori(tf::Vector3(0,0,1), alpha);

    // transform from reference into 'virtual laser' output frame
    tf::StampedTransform ref_to_out;
    ref_to_out.frame_id_ = ref_frame_id_;
    ref_to_out.child_frame_id_ = output_frame_id_;
    ref_to_out.stamp_ = cloud->header.stamp;
    ref_to_out.setOrigin( ref_origin );
    ref_to_out.setRotation( ref_ori );
    broadcaster.sendTransform( ref_to_out );

    // transform from cloud into output frame at zero height
    ref_origin.setZ( 0.0 );
    ref_to_out.setOrigin( ref_origin );
    tf::Transform cloud_to_out;
    cloud_to_out.mult( ref_to_out.inverse(), cloud_to_ref );

    for (PointCloud::const_iterator it = cloud->begin(); it != cloud->end(); ++it)
    {
      tf::Vector3 p(it->x,it->y,it->z);
      p = cloud_to_out(p);

      const float &x = p.x();
      const float &y = p.y();
      const float &z = p.z();

      if ( std::isnan(x) || std::isnan(y) || std::isnan(z) )
      {
        NODELET_DEBUG("rejected for nan in point(%f, %f, %f)\n", x, y, z);
        continue;
      }

      if (z > max_height_ || z < min_height_)
      {
        NODELET_DEBUG("rejected for height %f not in range (%f, %f)\n", p.z(), min_height_, max_height_);
        continue;
      }

      double range_sq = y*y+x*x;
      if (range_sq < range_min_sq_) {
        NODELET_DEBUG("rejected for range %f below minimum value %f. Point: (%f, %f, %f)", range_sq, range_min_sq_, x, y, z);
        continue;
      }

      double angle = -atan2(-y, x);
      if (angle < output->angle_min || angle > output->angle_max)
      {
        NODELET_DEBUG("rejected for angle %f not in range (%f, %f)\n", angle, output->angle_min, output->angle_max);
        continue;
      }
      int index = (angle - output->angle_min) / output->angle_increment;

      if (output->ranges[index] * output->ranges[index] > range_sq)
        output->ranges[index] = sqrt(range_sq);
      }

    pub_.publish(output);
  }


  double min_height_, max_height_, angle_min_, angle_max_, angle_increment_, scan_time_, range_min_, range_max_, range_min_sq_;
  std::string output_frame_id_, ref_frame_id_;

  ros::NodeHandle nh_;
  ros::Publisher pub_;
  ros::Subscriber sub_;

};

PLUGINLIB_DECLARE_CLASS(pointcloud_to_laserscan, CloudToScan, pointcloud_to_laserscan::CloudToScan, nodelet::Nodelet);
}
