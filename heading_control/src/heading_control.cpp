#include <ros/ros.h>
#include <ros/console.h>

#include <geometry_msgs/Point.h>
#include <geometry_msgs/Twist.h>
#include <helper_nodes/util.h>
#include <ohm_igvc_msgs/Range.h>
#include <ohm_igvc_msgs/RangeArray.h>
#include <ohm_igvc_msgs/TurnAngles.h>
#include <ohm_igvc_msgs/Waypoint.h>
#include <ohm_igvc_srvs/waypoint.h>
#include <ohm_igvc_srvs/coordinate_convert.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>

#include "pid.h"
//#include "directionalalgorithm.h"
// 	<!-- launch-prefix="xterm -e gdb --args" -->

#include <cmath>
#include <array>
#include <string>
#include <iterator>
#include <algorithm>

// TODO: change to ROS params ASAP!
#define STOP_VEL 0.0
#define DRIVE_VEL 0.0
#define HEADING_MAX 180.0
#define HEADING_MIN -180.0

struct heading_control {
	heading_control(ros::NodeHandle nh) {
		waypoint_srv = node.serviceClient<ohm_igvc_srvs::waypoint>("waypoint");
		coordinate_convert = node.serviceClient<ohm_igvc_srvs::coordinate_convert>("coordinate_convert");
		lidar_sub = node.subscribe(std::string("lidar"), 1, &heading_control::update_lidar, this);
		camera_sub = node.subscribe(std::string("camera"), 1, &heading_control::update_camera, this);
		vel_pub = node.advertise<geometry_msgs::Twist>("auto_control", 1);

		// get internal params
		node.param("waypoint_hit", waypoint_hit_thresh, 1.0);
		node.param("turn_to_heading", turn_to_heading_thresh, 90.0);
		node.param("max_linear", max_linear_speed, 0.3);
		node.param("max_angular", max_angular_speed, 0.25);
		node.param("drive_mode", drive_mode, std::string("manual"));
		node.param("enable_gps", gps_enable, true);
		node.param("enable_lidar", lidar_enable, true);
		node.param("enable_camera", camera_enable, true);
		node.param("enable_pid_debug", pid_debug, false);

		// get pid params
		node.param("kP", kP, 0.0);
		node.param("kI", kI, 0.0);
		node.param("kD", kD, 0.0);
		node.param("max_integral_error", max_i_err, 0.5);

		if(kP != 0.0) PID_type |= PID::terms_t::P;
		if(kI != 0.0) PID_type |= PID::terms_t::I;
		if(kD != 0.0) PID_type |= PID::terms_t::D;

		// tf params
		node.param("base_frame", base_frame_id, std::string("base"));
		node.param("reference_frame", ref_frame_id, std::string("world"));

		waypoint_id = 0;
	};

	void update_lidar(const ohm_igvc_msgs::RangeArray::ConstPtr &ranges) {
		lidar_ranges = *ranges;
	}

	void update_camera(const ohm_igvc_msgs::TurnAngles::ConstPtr &angles) {
		turn_angles = *angles;
	}

	bool get_pose() { // toss all the update code into one neat function
		tf::StampedTransform tform;
		if(pose_listener.waitForTransform(ref_frame_id, base_frame_id, ros::Time::now(), ros::Duration(0.15))) {
			pose_listener.lookupTransform(ref_frame_id, base_frame_id, ros::Time(0), tform);
			pose.position.x = tform.getOrigin().x();
			pose.position.y = tform.getOrigin().y();
			pose.heading = tf::getYaw(tform.getRotation()) * (180.0 / geometric::pi) + 180.0; // convert to degrees and put into [0, 360)
			return true;	
		} 
		
		return false;
	};

	bool get_next_waypoint() {
		ohm_igvc_srvs::waypoint req_wp;
		ohm_igvc_srvs::coordinate_convert req_conv;

		req_wp.request.ID = waypoint_id;
		
		if(!waypoint_srv.call(req_wp)) return false;

		req_conv.request.coordinate.latitude = req_wp.response.waypoint.latitude;
		req_conv.request.coordinate.longitude = req_wp.response.waypoint.longitude;

		if(!coordinate_convert.call(req_conv)) return false;

		goal.position.x = req_conv.response.coordinate.x;
		goal.position.y = req_conv.response.coordinate.y;
		goal.heading = req_wp.response.waypoint.heading;

		waypoint_id++;

		return true;
	};

	double condition_target(double target) {
		double diff = std::fabs(target - pose.heading);
		double supp = circular_range::supplement(target);
		
		if(diff > 180.0 && target > supp) {
			target -= 360.0;
			turn_state = 1; // turning left
		} else if(diff > 180.0 && target < supp) {
			target += 360.0;
			turn_state = 2; // turning right
		} else if(diff <= 180.0 && diff > 90.0) {
			turn_state = 3; // turning in place
		} else {
			turn_state = 0;
		}
		
		return target;
	};

	/*** VARIABLES ***/
	// ros specific
	ros::NodeHandle node;
	ros::ServiceClient waypoint_srv;
	ros::ServiceClient coordinate_convert;
	ros::Subscriber lidar_sub;
	ros::Subscriber camera_sub;
	ros::Publisher vel_pub;

	// internal variables
	double waypoint_hit_thresh;
	double turn_to_heading_thresh;
	double max_linear_speed;
	double max_angular_speed;
	double desired_heading;
	double chosen_heading;
	std::string drive_mode;
	bool gps_enable;
	bool lidar_enable;
	bool camera_enable;
	bool pid_debug;
	bool et_go_home;
	int turn_state; // 0 = NORMAL TURN, 1 = LEFT CROSSING BOUNDARY, 2 = RIGHT CROSSING BOUNDARY, 3 = IN_PLACE
	ohm_igvc_msgs::Waypoint start;
	ohm_igvc_msgs::RangeArray lidar_ranges;
	ohm_igvc_msgs::TurnAngles turn_angles;

	// waypoint variables
	int waypoint_id;
	ohm_igvc_msgs::Waypoint goal;

	// robot pose
	ohm_igvc_msgs::Waypoint pose;

//		DirectionalAlgorithm optimal_heading(node);

	// PID
	double kP;
	double kI;
	double kD;
	double max_i_err;

	int PID_type; // which terms we are using

	// tf stuff
	std::string base_frame_id;
	std::string ref_frame_id;
	tf::TransformListener pose_listener;
};
	
int main(int argc, char** argv) {
	ros::init(argc, argv, "heading_control");
	
	ros::NodeHandle nh;

	heading_control control(nh);

	geometry_msgs::Twist drive_command;
	drive_command.linear.x = STOP_VEL;

	ros::Rate r(10);

	std::array<double, 5> targets = {0.0, 100.0, 320.0, 95.0, 15.0};
	int target = 0;

	bool updatedDriveMode = false;

	if(control.get_pose()) {
		control.start = control.pose;
	}

	PID pid_controller(control.kP, control.kI, control.kD, control.max_i_err);
	
	while(ros::ok() && target < targets.size()) {
		ros::param::get("/drive_mode", control.drive_mode);
		if(control.drive_mode == "manual") {
			if(updatedDriveMode) updatedDriveMode = false;
			ROS_INFO_THROTTLE(15, "Robot is in manual mode. Not testing.");
			r.sleep();
			continue;
		} else if(control.drive_mode == "auto" && !updatedDriveMode) {
			updatedDriveMode = true;
			ROS_INFO("Switched to auto mode. Begin testing.");
			if(control.pid_debug) {
				target = 0;
				pid_controller.target(control.condition_target(targets[target]));
			}
		}		

		if(control.get_pose()) {
			/*** Heading Control code ***/			
			
			double desired_heading = control.pose.heading;
			double current_heading;
			
			if(control.pid_debug) {
				desired_heading = targets[target];
			}

			if(control.gps_enable) {
				desired_heading = geometric::angular_distance(control.pose.position, control.goal.position);

				bool hit_waypoint = rough_cmp::lt_eq(geometric::distance(control.pose.position, control.goal.position), control.waypoint_hit_thresh, 0.1);
				bool hit_heading = rough_cmp::equals(desired_heading, control.goal.heading, 3.0);

				if(hit_waypoint && hit_heading) {
					if(!control.et_go_home && !control.get_next_waypoint()) {
						control.et_go_home = true;
						control.goal = control.start;
					} else {
						drive_command.linear.x = 0.0;
						drive_command.angular.z = 0.1;

						control.vel_pub.publish(drive_command);
						break;
					}
				} else if(hit_waypoint) {
					drive_command.linear.x = 0.0;
					desired_heading = control.goal.heading;
				}
			}

			/*
			// add optimal direction code here
			chosen_heading = control.optimal_heading.optimaldirection(desired_heading, current_heading);
				
			if(rough_cmp::equals(controller.get_target(), chosen_heading, 2) {
				pid_controller.target(chosen_heading);
			}
			*/
			if(control.camera_enable) {
				if(!control.turn_angles.turn_angles.empty() && std::fabs(desired_heading - control.pose.heading) < 60.0) {
					std::vector<double> best_angles;
					best_angles.reserve(control.turn_angles.turn_angles.size());
				
					for(auto angle = control.turn_angles.turn_angles.begin(); angle != control.turn_angles.turn_angles.end(); ++angle) {
						best_angles.push_back(circular_range::direction(desired_heading, *angle) * std::fabs(desired_heading - *angle));
					}

					std::sort(best_angles.begin(), best_angles.end(), [] (double a, double b) { return (std::fabs(a) < std::fabs(b)); });
					desired_heading += best_angles.front();
				}
			}

			if(control.lidar_enable) {
				if(!control.lidar_ranges.ranges.empty() && std::fabs(desired_heading - control.pose.heading) < 135.0) {
					bool found_desired_heading = false;
					std::vector<double> best_angles;
					best_angles.reserve(control.lidar_ranges.ranges.size() * 2);
	
					for(auto range = control.lidar_ranges.ranges.begin(); range != control.lidar_ranges.ranges.end(); ++range) {
						if(circular_range::in_range(range->start, range->end, desired_heading)) {
							found_desired_heading = true; // if the desired heading is in this range, break and run with that
							ROS_INFO("\tFound the heading!");
							break;
						} else {
							best_angles.push_back(circular_range::direction(desired_heading, range->start) * std::fabs(desired_heading - range->start));
							best_angles.push_back(circular_range::direction(desired_heading, range->end) * std::fabs(desired_heading - range->end));
							ROS_INFO("\tDidn't find the heading. Finding closest angle instead");
						}
					}
					
					if(!found_desired_heading) { // ok now we look for the smallest difference
						std::sort(best_angles.begin(), best_angles.end(), [] (double a, double b) { return (std::fabs(a) < std::fabs(b)); });
						
						desired_heading += best_angles.front(); 
						ROS_INFO("\tFound next best angle");
					}
				} else if(control.lidar_ranges.ranges.empty()) {
					desired_heading = circular_range::supplement(desired_heading); // turn in place
					ROS_INFO("\tCouldn't find any heading");
				}
			}

			if(!rough_cmp::equals(pid_controller.get_target(), desired_heading, 2.0)) {
				pid_controller.target(control.condition_target(desired_heading)); // if we don't have options, turn in place
				ROS_INFO("\tSwitching heading");
			}
			
			/*** PID update code ***/

			switch(control.turn_state) { // condition inputs to get correct PID output. similar to condition_target
				case 1: // LEFT CROSS BOUNDARY
					current_heading = control.pose.heading - 360.0;
					break;
				case 2: // RIGHT CROSS BOUNDARY
					current_heading = control.pose.heading + 360.0;
					break;
				case 3: // IN_PLACE
					drive_command.linear.x = 0.0;
					break;
				default:
					current_heading = control.pose.heading;
					drive_command.linear.x = control.max_linear_speed;
			}		
			
			drive_command.angular.z = pid_controller.update(current_heading, control.PID_type); // update pid
		
			if(drive_command.angular.z > control.max_angular_speed) drive_command.angular.z = control.max_angular_speed;
			else if(drive_command.angular.z < -control.max_angular_speed) drive_command.angular.z = -control.max_angular_speed;	

			ROS_INFO("Target: %f | Actual: %f | Update: %f", pid_controller.get_target(), control.pose.heading, drive_command.angular.z);

			if(control.pid_debug) {
				if(rough_cmp::equals(control.pose.heading, targets[target], 0.5)) {
					ROS_INFO("HIT Target: %f | Actual: %f", targets[target], control.pose.heading);
					target++;
				
					pid_controller.target(control.condition_target(targets[target]));
				}
			}
		} else {
			ROS_INFO("Could not get pose!");
			drive_command.linear.x = 0.0;
			drive_command.angular.z = 0.0;
		}
		
		control.vel_pub.publish(drive_command);

		ros::spinOnce();

		r.sleep();
	}

	return 0;
}


/*for(auto heading_range = possible_headings.ranges.begin(); heading_range != possible_headings.ranges.end(); ++heading_range) {
			if(circular_range_compare(heading_range->start, heading_range->end, heading)) {
				
			}
		}*/
