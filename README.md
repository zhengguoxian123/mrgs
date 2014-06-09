mrgs
===
A general communication framework for Multi-Robot SLAM based on Occupancy Grid merging.  
This work was developed by Gonçalo Martins in the context of his MSc work.

## Introduction
This software solution aims to transparently enable any Single-Robot SLAM system to perform Multi-Robot SLAM. It is implemented using the [ROS](http://ros.org) cummunication framework. It is divided into five different packages.

<!-- Imagem! -->

To achieve our goal, this system runs on top of any existing SLAM system that conforms to ROS's usual standards for SLAM (e.g. the usage of the OccupancyGrid message to transmit occupancy grids), and enables it to communicate with nearby SLAMming robots in order to build a joint representation of the environment.

## mrgs: A Practical Guide

### Requirements
1. This system runs on top of ROS Fuerte, which requires that the machine you use is running Linux, preferrably Ubuntu 12.04. Other flavours of Ubuntu may or may not work with ROS.
2. You must already have a working SLAM system, since this system does not include a SLAM solution. A commonly used SLAM package is [GMapping](http://wiki.ros.org/gmapping).
3. The system itself depends on a few other things, which are detailed on the setup script (mrgs_scripts/scripts/setup.sh), which brings us to...

### Setup
Setting up the system is as simple as running the setup.sh bash script that exists in the scripts folder of the mrgs_scripts package. Please do not do this blindly! The script has instructions on the top, read them carefully. This script downloads and installs everything you should need in order to run this on top of your pre-existing SLAM solution. Running the script is usually accomplished by:

    $roscd mrgs_scripts/scripts
    $./setup.sh
    
or, alternatively, if you want to see the script (which you should):

    $roscd mrgs_scripts/scripts
    $nano setup.sh
    
Again, do no do this blindly, take a look at the script before executing it. In this folder you will find other useful goodies, like the ip_setup python script, and a script that takes your system from freshly installed to having a working ROS Fuerte system.

### Running the system
The system can run in one of two different modes: centralized and distributed. The distributed mode runs the full mrgs system on every robot, and the centralized mode relies on a non-mapping computer to receive the maps and do all the heavy processing.

The launching of the system is taken care of by using the provided launch files.

#### Distributed (normal) mode:

    $roslaunch mrgs_scripts distributed_node.launch interface:=wlan0
    
Interface is usually wlan0, eth0 or something similar, substitute wlan0 with whatever interface OLSRd is using.

#### Centralized mode:
On the central machine:

    $roslaunch mrgs_scripts central_node.launch
    
On the remaining machines:

    $roslaunch mrgs_scripts transmitter_node.launch interface:=wlan0
    
Same rules as before for the interface argument.
