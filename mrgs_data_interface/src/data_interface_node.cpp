/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, ISR University of Coimbra.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the ISR University of Coimbra nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Gonçalo S. Martins, 2014
*********************************************************************/

/** 
 * data_interface_node:
 * 
 * Summary:
 * Essentially, this node is charged with all the communication between robots, and with converting between datatypes 
 * used for external and internal communication.
 * This node is responsible for: keeping track of all the topics we must subscribe to in order to communicate with other
 * robots; keeping local copies of foreign maps transforms and poses; propagating those in a standardized way across
 * our local system; transmitting the latest local map, relevant transform and pose across the network.
 * 
 * Methodology:
 * This node maintains a multitude of global variables that, together, represent the current state of the operation, as
 * seen by the local robot. These variables are only written to on very very well determined moments, to prevent race 
 * conditions and other data-related issues.
 * I'm sure that with sufficient time, a few of these could be converted into local variables of some sort. However, in
 * order to complete this project in time, I have opted to combine this approach with programming discipline to ensure
 * there are no issues with using these variables. To any possible future maintaineres, austerity is advised in modify-
 * ing the way these variables interact with the remaining program.
 * 
 * Data Structures:
 * -> ForeignMap: A map received from another robot.
 * -> ForeignMapVector: A vector with a foreign map for each robot we know.
 * -> NetworkMap: The datatype that flows across the network. Contains a compressed map that is decompressed into a 
 * foreign map, as well as a map to base_link transform.
 * -> LatestRobotPose: A Pose including the robot's ID, carried by a NetworkMap from a foreign robot, for transmission 
 * into the internal network.
 */

/// ROS includes
#include "ros/ros.h"
#include "nav_msgs/OccupancyGrid.h"
#include "nav_msgs/Odometry.h"
#include "geometry_msgs/Pose.h"
#include "tf/transform_listener.h"

/// Include our messages
#include "mrgs_data_interface/ForeignMap.h"
#include "mrgs_data_interface/ForeignMapVector.h"
#include "mrgs_data_interface/NetworkMap.h"
#include "mrgs_data_interface/LatestRobotPose.h"

/// LZ4 include:
#include "lz4/lz4.h"

/// Wifi_comm includes
#include "wifi_comm/wifi_comm_lib.h"

/// Other includes
#include <string>
#include <fstream>

/// Global variables
// Node handle. Must be global to be accessible by callbacks.
ros::NodeHandle *g_n;

// Program state:
// To be written only by the processForeignMap callback (and once in main() for initialization)
// (at(0) is always our local mac)
std::vector<std::string> g_peer_macs;
// To be written by the processForeignMap callback
std::vector<mrgs_data_interface::ForeignMap> g_foreign_map_vector;
// wifi_comm object
wifi_comm::WiFiComm* g_my_comm;
// Global list of wifi_comm subscribers
std::vector<ros::Subscriber> g_subs;
// To indicate whether or not we have a map from the local robot
bool g_local_map_exists = false;
// To indicate whether or not we are operating in centralized mode, and if we're a central or a transmitter node.
// The values of these are determined by getting parameters from ROS.
bool g_centralized_mode = false;
bool g_transmitter_mode = false;

// TF listener
tf::TransformListener *g_listener;

// Publishers:
// To enable publishing from callback, to be edited once in main()
ros::Publisher g_foreign_map_vector_publisher;
// Publisher for external map (map that goes into the external network)
ros::Publisher *g_external_map;
// Publisher for poses from other robots
ros::Publisher g_latest_pose;
// Publisher for poses to other robots
ros::Publisher g_external_pose;
// Time at which the last pose transmission occurred
ros::Time g_since_last_pose;

inline int getRobotID(std:: string mac){
  // Find the desired MAC's index
  // If we're on centralized mode, this may show up empty once
  if(g_peer_macs.size() == 0)
    return -1;
  // If not, we search
  int index = std::distance(g_peer_macs.begin(), std::find(g_peer_macs.begin(), g_peer_macs.end(), mac));
  // If the index isn't smaller than the size, the the desired MAC doesn't exist, 
  // and we return -1 to indicate just that.
  if(index < g_peer_macs.size())
    return index;
  else
    return -1;
}

void processForeignMap(std::string ip, const mrgs_data_interface::NetworkMap::ConstPtr& msg)
{
  // Start counting time
  ROS_INFO("Processing new foreign map.");
  ros::Time init = ros::Time::now();
  
  /// Determine which robot sent the map (i.e. determine its ID) and act on that knowledge.
  int id = getRobotID(msg->mac);
  // Inform the outside world of our reception.
  ROS_INFO("Received from %s, with mac %s, id %d.", ip.c_str(), msg->mac.c_str(), id);
  bool is_repeated = false;
  if(id == -1)
  {
    // We've never found this robot before!
    // Add new robot to our list of peer macs and allocate space for its map.
    id = g_peer_macs.size();                      // The new MAC will be added at the end of the vector
    g_peer_macs.push_back(msg->mac);              // Add new MAC
    mrgs_data_interface::ForeignMap newMap;
    newMap.robot_id = id;                         // Attribute the right id
    g_foreign_map_vector.push_back(newMap);       // Add a new, uninitialized map.
    ROS_DEBUG("We've never met this guy before. His id is now %d. Vector sizes are %d and %d.", id, g_peer_macs.size(), g_foreign_map_vector.size());
  }
  else
  {
    // This is a robot we've met before. Let's see is we already have this map. We're not interested in re-decompressing
    // the same map.
    if(g_foreign_map_vector.at(id).map.header.stamp == msg->grid_stamp)
    {
      ROS_DEBUG("Repeated map, no need to decompress. Processing took %fs.", (ros::Time::now() - init).toSec());
      is_repeated = true;
    }
  }
  
  /// Decompress data
  if(msg->decompressed_length > 0 && is_repeated == false)  // Messages with decompressed_length == 0 are test messages.
  {
    ROS_DEBUG("Received map consists of %d compressed bytes. Decompressing.", msg->compressed_data.size());
    // Allocate and populate compressed buffer
    char* compressed = new char[msg->compressed_data.size()];
    for(int i = 0; i < msg->compressed_data.size(); i++)
      compressed[i] = msg->compressed_data.at(i);
    // Allocate decompression buffer
    char* decompressed = new char [msg->decompressed_length];
    // Decompress
    int decompressed_bytes = LZ4_decompress_safe(compressed, decompressed, msg->compressed_data.size(), msg->decompressed_length);
    
    // Copy data to foreign map vector
    // Copy metadata
    g_foreign_map_vector.at(id).map.header.stamp = msg->grid_stamp;
    g_foreign_map_vector.at(id).map.info = msg->info;
    g_foreign_map_vector.at(id).map.data.clear();
    // Pre-allocate and copy map
    g_foreign_map_vector.at(id).map.data.reserve(decompressed_bytes);
    for(int i = 0; i < decompressed_bytes; i++)
      g_foreign_map_vector.at(id).map.data.push_back(decompressed[i]);
  }
  else
    ROS_DEBUG("This is a debug or repeated map. No decompression took place.");
  
  /// Publish foreign maps and transform
  // We only publish if the local map exists, so we don't send an empty map to the complete map node.
  // In cetralized mode, we only publish if we have at least two maps, of course.
  if(g_local_map_exists == true || (g_centralized_mode == true && g_foreign_map_vector.size() > 1))
  {
    ROS_DEBUG("Publishing foreign_map_vector...");
    mrgs_data_interface::ForeignMapVector map_vector;
    map_vector.map_vector = g_foreign_map_vector; // This is a potential time sink, depending on how the copy is handled.
    g_foreign_map_vector_publisher.publish(map_vector);
  }
  mrgs_data_interface::LatestRobotPose latest_pose;
  latest_pose.transform = msg->map_to_base_link;
  latest_pose.id = id;
  g_latest_pose.publish(latest_pose);
  
  /// Inform
  ROS_INFO("Processing foreign map took %fs.", (ros::Time::now() - init).toSec());
}

void newRobotInNetwork(char * ip)
{
  // Inform
  ROS_INFO("Connecting to new peer at %s.", ip);

  // Send
  // We only need to send maps if we're transmitters or if we're on distributed mode
  if(g_transmitter_mode || g_centralized_mode)
    g_my_comm->openForeignRelay(ip, "/mrgs/external_map", true);

  // Receive
  // We only need to receive maps if we are not simple transmitters.
  if(!g_transmitter_mode)
  {
    char topic[128];
    ROS_INFO("Subscribing to remote topic.");
    ros::Subscriber sub = g_n->subscribe<mrgs_data_interface::NetworkMap>(wifi_comm::WiFiComm::concatTopicAndIp(topic, "/mrgs/external_map", ip),
                                                                          1,  // Number of messages to keep on the input queue 
                                                                          boost::bind(processForeignMap, 
                                                                          std::string(ip), _1));
    g_subs.push_back(sub);
    ROS_INFO("Subscribed");
  }
}

void processMap(const nav_msgs::OccupancyGrid::ConstPtr& map)
{
  // This function processes a new local map. It updates the latest local map pointer and creates a new publish-able
  // NetworkMap.
  ROS_INFO("Processing local map.");
  // Start counting time
  ros::Time init = ros::Time::now();
  
  /// Update the local map
  g_foreign_map_vector.at(0).map = *map;
  g_local_map_exists = true;
  
  /// Create the new NetworkMap
  mrgs_data_interface::NetworkMap::Ptr publish_map(new mrgs_data_interface::NetworkMap);
  // Fill in local mac
  publish_map->mac = g_peer_macs.at(0);
  // Fill in time stamp, metadata and decompressed length
  publish_map->grid_stamp = map->header.stamp;
  publish_map->info = map->info;
  unsigned int map_length = publish_map-> info.height * publish_map-> info.width;
  publish_map->decompressed_length = map_length;
  // Compress the new map
  char* compressed = new char [LZ4_compressBound(map_length)];              // We have to allocate this buffer with 
  char* decompressed = new char [map_length];                               // extra space, lest the data be 
  for(int i = 0; i < map_length; i++)                                        // incompressible.
    decompressed[i] = map->data.at(i);                                        // Copy data to compress.
  int compressed_bytes = LZ4_compress(decompressed, compressed, map_length);  // Compress
  // Store the new map
  publish_map->compressed_data.clear();
  publish_map->compressed_data.reserve(compressed_bytes);
  for(int i = 0; i < compressed_bytes; i++)
    publish_map->compressed_data.push_back(compressed[i]);
  // Add transform to NetworkMap
  bool will_publish = true;
  if(g_listener->canTransform ("/base_link", "/map", ros::Time(0)))
  {
    tf::StampedTransform map_to_base_link;
    g_listener->lookupTransform(std::string("/map"), std::string("/base_link"), ros::Time(0), map_to_base_link);
    tf::transformStampedTFToMsg(map_to_base_link, publish_map->map_to_base_link);
  }
  else
  {
    ROS_WARN("Can't find map->base_link TF, won't publish network map.");
    will_publish = false;
  }
  // Publish
  if(will_publish)
    g_external_map->publish(*publish_map);
  
  /// Inform
  ROS_INFO("Processed a new local map. Size: %d bytes. Compressed size: %d bytes. Ratio: %f", 
           map_length, compressed_bytes, (float)map_length/(float)compressed_bytes);
  ROS_INFO("Processing local map took %fs.", (ros::Time::now() - init).toSec());
}

int main(int argc, char **argv)
{
  // ROS init
  ros::init(argc, argv, "data_interface_node");
  g_n = new ros::NodeHandle;
  
  // Determine if we're on centralized mode, and if we're a transmitter
  if(!g_n->getParam("is_centralized", g_centralized_mode))
  {
    ROS_FATAL("Could not get a parameter indicating whether or not we're on centralized mode!");
    return -1;
  }
  else
  {
    if(!g_n->getParam("is_transmitter", g_transmitter_mode))
    {
      ROS_FATAL("Could not get a parameter indicating whether or not we're a transmitter!");
      return -1;
    }
  }
  
  // Report mode of operation:
  if(!g_centralized_mode)
      ROS_INFO("Parameters received. Entering distributed mode.");
  else
    if(g_transmitter_mode)
      ROS_INFO("Parameters received. Entering centralized mode. This is a transmitter node.");
    else
      ROS_INFO("Parameters received. Entering centralized mode. This is a center node.");
  
  // Determine the interface we'll be using and its MAC
  // We only need an interface in distributed or transmitter modes
  if(!g_centralized_mode || g_transmitter_mode)
  {
    // Get interface parameter
    std::string interface;
    if(!g_n->getParam("interface", interface))
    {
      ROS_FATAL("Could not get a parameter indicating the interface we'll be using!");
      return -1;
    }
    else
    {
      ROS_INFO("Parameter received. Using interface %s.", interface.c_str());
    }
    
    // Retrieve local MAC address
    std::string* mac_file_path = new std::string(std::string("/sys/class/net/") + 
                                                 interface + 
                                                 std::string("/address"));
    ROS_DEBUG("Retrieving MAC from %s.", mac_file_path->c_str());                                                
    std::string* local_mac = new std::string;
    std::ifstream mac_file;
    mac_file.open((*mac_file_path).c_str(), std::ios::in);
    if(mac_file.is_open())
    {
      mac_file >> *local_mac;
      mac_file.close();
      g_peer_macs.push_back(*local_mac);
      delete local_mac;                   // No need to keep this extra stuff in memory
      delete mac_file_path;               // Idem
    }
    else
    {
      ROS_FATAL("Can't open mac address file, did you input the right interface?");
      return -1;
    }
    
    // Push an empty map into the foreign map vector, to keep it aligned with IDs.
    mrgs_data_interface::ForeignMap emptyMap;
    emptyMap.robot_id = 0;
    g_foreign_map_vector.push_back(emptyMap);
  }
  
  // wifi_comm init
  boost::function<void (char *)> new_robot_callback;
  new_robot_callback = newRobotInNetwork;
  g_my_comm = new wifi_comm::WiFiComm(new_robot_callback);
  g_external_map = new ros::Publisher;
  *g_external_map = g_n->advertise<mrgs_data_interface::NetworkMap>("/mrgs/external_map", 10);
  g_foreign_map_vector_publisher = g_n->advertise<mrgs_data_interface::ForeignMapVector>("mrgs/foreign_maps", 10);
  g_latest_pose = g_n->advertise<mrgs_data_interface::LatestRobotPose>("mrgs/remote_poses", 10);
  g_since_last_pose = ros::Time::now();
  
  // tf init
  g_listener = new tf::TransformListener;
  
  // Declare callbacks
  ros::Subscriber map = g_n->subscribe<nav_msgs::OccupancyGrid>("mrgs/local_map", 1, processMap);
  
  // Regular execution:
  ros::spin();
  

  return 0;
}
