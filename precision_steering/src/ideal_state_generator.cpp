#include <ros/ros.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/PoseStamped.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>
#include <precision_navigation_msgs/DesiredState.h>
#include <precision_navigation_msgs/PathSegment.h>
#include <precision_navigation_msgs/Path.h>
#include <actionlib/server/simple_action_server.h>
#include <precision_navigation_msgs/ExecutePathAction.h>
//#include <octocostmap/costmap_3d.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <segment_lib/segment_lib.h>
#include <boost/thread.hpp>

// Shorthand
using precision_navigation_msgs::PathSegment;
namespace p_nav = precision_navigation_msgs;

class IdealStateGenerator {

  public:
    IdealStateGenerator();
  private:
    bool computeState(p_nav::DesiredState& new_des_state);
    //Handle new path
    void newPathCallback();
    //Cancel current path
    void preemptPathCallback();
    p_nav::DesiredState makeHaltState(bool command_last_state);
    void computeStateLoop(const ros::TimerEvent& event);
    bool checkCollisions(bool checkEntireVolume, const p_nav::DesiredState& des_state);

    //Loop rate in Hz
    double loop_rate_;
    double dt_;

    double seg_length_done_;
    uint32_t seg_number_;
    uint32_t seg_index_;
    
    // Bad things happen if the state changes in the middle of a computation
    boost::mutex compute_state_mutex_;

    //Current path to be working on
    std::vector<PathSegment> path_;

    //The last desired state we output	
    p_nav::DesiredState desiredState_;

    //ROS communcators
    ros::NodeHandle nh_;
    ros::Publisher ideal_state_pub_;
    ros::Publisher ideal_pose_marker_pub_;
    ros::Subscriber path_sub_;
    tf::TransformListener tf_listener_;	
    geometry_msgs::PoseStamped temp_pose_in_, temp_pose_out_;
   // boost::shared_ptr<octocostmap::Costmap3D> costmap_;
    ros::Timer compute_loop_timer_;
    std::string action_name_;
    actionlib::SimpleActionServer<p_nav::ExecutePathAction> as_;
    p_nav::ExecutePathFeedback feedback_;
};

IdealStateGenerator::IdealStateGenerator(): 
  action_name_("execute_path"), 
  as_(nh_, action_name_, false),
  tf_listener_(ros::Duration(10))
{
	ros::Duration(3.0).sleep();
  //Setup the ideal state pub
  ideal_state_pub_= nh_.advertise<p_nav::DesiredState>("idealState",1);   
  ideal_pose_marker_pub_= nh_.advertise<geometry_msgs::PoseStamped>("ideal_pose",1);   
  nh_.param("loop_rate",loop_rate_,20.0); // default 20Hz
  dt_ = 1.0/loop_rate_;
  //costmap_ = boost::shared_ptr<octocostmap::Costmap3D>(new octocostmap::Costmap3D("octocostmap", tf_listener_));

  //Setup the loop timer
  compute_loop_timer_ = nh_.createTimer(ros::Duration(dt_), boost::bind(&IdealStateGenerator::computeStateLoop, this, _1));

  //Initialze private class variables
  seg_index_ = 0;
  seg_number_ = 0;
  seg_length_done_ = 0.0;
	
	// Wait to get transforms.
	/*bool found = false;
	while( !found ){
		try{
			ROS_INFO("[ideal state generator] Getting transforms");
			found = true;
			tf::StampedTransform transform;
			tf_listener_.waitForTransform("odom", "map"      , ros::Time::now(), ros::Duration(1));
			tf_listener_.lookupTransform ("odom", "map"      , ros::Time::now(), transform);
			tf_listener_.waitForTransform("odom", "base_link", ros::Time::now(), ros::Duration(1));
			tf_listener_.lookupTransform ("odom", "base_link", ros::Time::now(), transform);
		}
		catch(tf::TransformException& ex){
			ROS_WARN("[ideal state generator] Failed to get transforms %s", ex.what());
			found = false;
		}
	}
  ROS_INFO("[ideal state generator] Got transforms");*/
  desiredState_ = makeHaltState(false);

  as_.registerGoalCallback(boost::bind(&IdealStateGenerator::newPathCallback, this));
  as_.registerPreemptCallback(boost::bind(&IdealStateGenerator::preemptPathCallback, this));
  as_.start();
}

//We want to take the current location of the base and set that as the desired state with 0 velocity and rho. 
p_nav::DesiredState IdealStateGenerator::makeHaltState(bool command_last_state) {
  p_nav::DesiredState halt_state;
  //If we should command our current position, command_last_state will be false. Otherwise, command the last desired state
  if (command_last_state) {
    halt_state = desiredState_;
    halt_state.des_speed = 0.0;
  } else {
    //Convert into the odometry frame from whatever frame the path segments are in
    temp_pose_in_.header.frame_id = "base_link";
    temp_pose_in_.pose.position.x = 0.0;
    temp_pose_in_.pose.position.y = 0.0;
    temp_pose_in_.pose.orientation = tf::createQuaternionMsgFromYaw(0.0);
    ros::Time current_transform = ros::Time::now();
    tf_listener_.getLatestCommonTime(temp_pose_in_.header.frame_id, "odom", current_transform, NULL);
    temp_pose_in_.header.stamp = current_transform;
    tf_listener_.transformPose("odom", temp_pose_in_, temp_pose_out_);

    halt_state.header.frame_id = "odom";
    halt_state.seg_type = PathSegment::SPIN_IN_PLACE;
    halt_state.header.stamp = ros::Time::now();
    halt_state.des_pose = temp_pose_out_.pose;
    halt_state.des_speed = 0.0;
    halt_state.des_rho = 0.0;
  }
  return halt_state;
}

void IdealStateGenerator::computeStateLoop(const ros::TimerEvent& event) {
  ROS_DEBUG("Last callback took %f seconds", event.profile.last_duration.toSec());
  
  boost::mutex::scoped_lock l(compute_state_mutex_);

  p_nav::DesiredState new_desired_state;
  new_desired_state.header.frame_id = "odom";
  new_desired_state.header.stamp = ros::Time::now();
  // if we actually have a path to execute, try to execute it, otherwise just output our current position as the goal
  if (as_.isActive()) {
    ROS_DEBUG("We have an active goal. Compute state");
    if (computeState(new_desired_state)) {
      ROS_DEBUG("State computation failed. Command current position");
      new_desired_state = makeHaltState(false);
    }
    if(!checkCollisions(false, new_desired_state)) {
      ROS_DEBUG("No collision detected. Passing on current desired state");
      desiredState_ = new_desired_state;
    } else {
      ROS_DEBUG("Collision detected. Commanding current position");
      desiredState_ = makeHaltState(false); 
    }
  } else {
    ROS_DEBUG("No active goal, so sending last desired state");
    desiredState_ = makeHaltState(true);
  }

  //Publish desired state
  ideal_state_pub_.publish(desiredState_);
  geometry_msgs::PoseStamped des_pose;
  des_pose.header = desiredState_.header;
  des_pose.pose = desiredState_.des_pose;
  ideal_pose_marker_pub_.publish(des_pose);
}

bool IdealStateGenerator::checkCollisions(bool checkEntireVolume, const p_nav::DesiredState& des_state) {
  return false; // MEGA HAX
}

double clamp(double x, double low, double high)
{
	if( x<low  ) { return low;  }
	if( x>high ) { return high; }
	return x;
}

double clampMagnitude(double x, double mag)
{
	return clamp(x, -fabs(mag), fabs(mag));
}

double v_prev_;
int    prev_seg_type;
bool IdealStateGenerator::computeState(p_nav::DesiredState& new_des_state)
{
	//ROS_INFO_THROTTLE(2,"segnum %d (index %d)", seg_number_, seg_index_);

  double v = 0.0;
  bool end_of_path = false;
  double dL = desiredState_.des_speed * dt_;
  if(seg_index_ >= path_.size()) {
    //Out of bounds
    seg_index_ = path_.size()-1;
    end_of_path = true;
    v_prev_ = 0;
  }

	seg_number_ = path_.at(seg_index_).seg_number;

  ros::Time current_transform = ros::Time::now();
  if (path_.at(seg_index_).seg_type == PathSegment::SPIN_IN_PLACE) {
    seg_length_done_ = seg_length_done_ + dL;
  } else {
    temp_pose_in_.header.frame_id = "base_link";
    temp_pose_in_.pose.position.x = 0.0;
    temp_pose_in_.pose.position.y = 0.0;
    temp_pose_in_.pose.orientation = tf::createQuaternionMsgFromYaw(0.0);
    tf_listener_.getLatestCommonTime(temp_pose_in_.header.frame_id, "odom", current_transform, NULL);
    temp_pose_in_.header.stamp = current_transform;
    tf_listener_.transformPose("odom", temp_pose_in_, temp_pose_out_);
    double psiPSO = tf::getYaw(temp_pose_out_.pose.orientation);
    double psiDes = tf::getYaw(desiredState_.des_pose.orientation);
    //Need to only advance by the projection of what we did onto the desired heading	
    // formula is v * dt * cos(psiDes - psiPSO)
    //seg_length_done_ = seg_length_done_ + dL * cos(psiDes - psiPSO);
    seg_length_done_ = seg_length_done_ + fabs(dL * cos(psiDes - psiPSO));
  }
  double lengthSeg = path_.at(seg_index_).seg_length;
  if(seg_length_done_ > lengthSeg) {
    seg_length_done_ = 0.0;
    seg_index_++;
  }

  if(seg_index_ >= path_.size()) {
    //Out of bounds
    seg_index_ = path_.size()-1;
    end_of_path = true;
  }
	seg_number_ = path_.at(seg_index_).seg_number;

  lengthSeg = path_.at(seg_index_).seg_length;
  if(end_of_path) {
    //If we should stop because we ran out of path, we should command the state corresponding to s=1
    seg_length_done_ = lengthSeg;
  }

  PathSegment currentSeg = path_.at(seg_index_);


	// 1.0 or -1.0 depending on whether this seg is reversed
	double direction = currentSeg.max_speeds.linear.x > 0 ? 1.0 : -1.0;
	//if(direction < 0)
	//	ROS_INFO("Reverse segment detected");


	// Now to determine velocity.
	double vNext;
	v = v_prev_;
	
	// If we just jumped over to a discontinuous segment type, make sure our velocity is zero
	if( ((currentSeg.seg_type==PathSegment::SPIN_IN_PLACE)||
	     (prev_seg_type      ==PathSegment::SPIN_IN_PLACE))
	 && (currentSeg.seg_type != prev_seg_type) ){
		v = 0;
	}
	
	bool hasnextseg = seg_index_ < path_.size()-1;
	
	// Determine whether we need to stop after this segment
	if ( !hasnextseg || 
	    (currentSeg.seg_type == PathSegment::SPIN_IN_PLACE &&  path_.at(seg_index_+1).seg_type != PathSegment::SPIN_IN_PLACE) )
	{
		vNext = 0.0;
	}
	else // line or arc
	{
		if (seg_index_ < path_.size()-1
		 && path_.at(seg_index_+1).seg_type != PathSegment::SPIN_IN_PLACE) {
			vNext = path_.at(seg_index_+1).max_speeds.linear.x;
		} 
		else {
			vNext = 0.0;	
		}
	}

	// Going from line to spin - it needs to decel

  double tDecel = fabs(v - vNext)/currentSeg.decel_limit;
  double vMean = (fabs(v) + fabs(vNext))/2.0;
  double distDecel = fabs(vMean*tDecel);

  double lengthRemaining = currentSeg.seg_length - seg_length_done_;
  if(lengthRemaining < 0.0) {
    lengthRemaining = 0.0;
  }
  else if (lengthRemaining < distDecel) {
    v = direction * sqrt(2*lengthRemaining*currentSeg.decel_limit + pow(vNext, 2));
  }
  else {
    v += direction*currentSeg.accel_limit*dt_;
  }

  if (currentSeg.seg_type == PathSegment::SPIN_IN_PLACE) {
    v = clampMagnitude(v, currentSeg.max_speeds.angular.z);
  } else {
  	v = clampMagnitude(v, currentSeg.max_speeds.linear.x);
  }

	if(end_of_path){
		v = 0;
	}
	
  //done figuring out our velocity commands
	v_prev_ = v;
	prev_seg_type = currentSeg.seg_type;


  //Convert into the odometry frame from whatever frame the path segments are in
  temp_pose_in_.header.frame_id = currentSeg.header.frame_id;
  temp_pose_in_.pose.position = currentSeg.ref_point;
  temp_pose_in_.pose.orientation = currentSeg.init_tan_angle;
  
  /*
  current_transform = ros::Time::now();
  tf_listener_.getLatestCommonTime(temp_pose_in_.header.frame_id, "odom", current_transform, NULL);
  temp_pose_in_.header.stamp = current_transform;
  tf_listener_.transformPose("odom", temp_pose_in_, temp_pose_out_); */
  
  // HAX
  temp_pose_out_ = temp_pose_in_;
  //temp_pose_out_.header.stamp = ros::Time::now();

  double tanAngle = tf::getYaw(temp_pose_out_.pose.orientation);
  //std::cout << "seg_index_ " << seg_index_ << std::endl;
  //ROS_INFO("seg_length       = %.3f", path_.at(seg_index_).seg_length);
  //ROS_INFO("seg_length_done_ = %.3f", seg_length_done_);
  //ROS_INFO("tan angle        = %.2fpi", tanAngle/M_PI);
  double radius, tangentAngStart, arcAngStart, dAng, arcAng, rho;
  bool should_halt = false;
  //std::cout << seg_index_ << std::endl;
  switch(currentSeg.seg_type){
    case PathSegment::LINE:
      new_des_state.seg_type = currentSeg.seg_type;
      new_des_state.des_pose.position.x = temp_pose_out_.pose.position.x + seg_length_done_*cos(tanAngle);
      new_des_state.des_pose.position.y = temp_pose_out_.pose.position.y + seg_length_done_*sin(tanAngle);
      new_des_state.des_pose.orientation = tf::createQuaternionMsgFromYaw(tanAngle);
      new_des_state.des_rho = currentSeg.curvature;
      new_des_state.des_speed = v;
      new_des_state.des_lseg = seg_length_done_;
      
      //ROS_INFO("(x,y) = (%.3f,%.3f)   v=%.2f, vmax=%.2f", new_des_state.des_pose.position.x, new_des_state.des_pose.position.y, v, currentSeg.max_speeds.linear.x);
      break;
    case PathSegment::ARC:
      rho = currentSeg.curvature;
      //std::cout << "rho " << rho << std::endl;
      radius = 1.0/fabs(rho);
      //std::cout << "radius = " << radius << std::endl;
      tangentAngStart = tanAngle;
      arcAngStart = 0.0;
      if(rho >= 0.0) {
        arcAngStart = tangentAngStart - M_PI / 2.0;	
      } else {
        arcAngStart = tangentAngStart + M_PI / 2.0;
      }
      dAng = seg_length_done_*rho;
      //std::cout << "dAng " << dAng << std::endl;
      arcAng = arcAngStart + dAng;
      new_des_state.seg_type = currentSeg.seg_type;
      new_des_state.des_pose.position.x = temp_pose_out_.pose.position.x + radius * cos(arcAng);
      new_des_state.des_pose.position.y = temp_pose_out_.pose.position.y  + radius * sin(arcAng);
      new_des_state.des_pose.orientation = tf::createQuaternionMsgFromYaw(tanAngle + dAng);
      new_des_state.des_rho = currentSeg.curvature;
      new_des_state.des_speed = v;
      new_des_state.des_lseg = seg_length_done_;
      break;
    case PathSegment::SPIN_IN_PLACE:
      rho = currentSeg.curvature;
      tangentAngStart = tanAngle;
      arcAngStart = 0.0;
      if(rho >= 0.0) {
        arcAngStart = tangentAngStart - M_PI / 2.0;	
      } else {
        arcAngStart = tangentAngStart + M_PI / 2.0;
      }
      dAng = seg_length_done_*rho;
      arcAng = arcAngStart + dAng;
      new_des_state.seg_type = currentSeg.seg_type;
      new_des_state.des_pose.position.x = temp_pose_out_.pose.position.x;
      new_des_state.des_pose.position.y = temp_pose_out_.pose.position.y;
      new_des_state.des_pose.orientation = tf::createQuaternionMsgFromYaw(tanAngle + dAng);
      new_des_state.des_rho = currentSeg.curvature;
      new_des_state.des_speed = v;
      new_des_state.des_lseg = seg_length_done_;
      break;
    default:
      ROS_WARN("Unknown segment type. Type was %d. Halting", currentSeg.seg_type);
      new_des_state.des_speed = 0.0;
      should_halt = true;
  }
  feedback_.seg_number = seg_number_;
  feedback_.current_segment = currentSeg;
  feedback_.seg_distance_done = seg_length_done_;
  as_.publishFeedback(feedback_);
  return should_halt;
}

void IdealStateGenerator::newPathCallback() {
  boost::mutex::scoped_lock l(compute_state_mutex_);
	path_ = as_.acceptNewGoal()->segments;
	
	int old_index = seg_index_;
	int old_segnum = seg_number_;
	
	// Search for the current segment number in the new path
	p_nav::Path p;
	p.segs = path_;
	int new_index = segment_lib::segnumToIndex(p, seg_number_);
	p.segs = path_;
	
	// If it was found, set our path index to the new value.
	if( new_index != -1 ) {
		ROS_DEBUG("%s: New goal accepted, continuation of old goal.", action_name_.c_str());
	  seg_index_ = new_index;
	}
	// Otherwise, start at the beginning of the new path.
	else {
  	ROS_INFO("%s: New goal accepted, state cleared.", action_name_.c_str());
		seg_index_       = 0;
		seg_number_      = path_.at(seg_index_).seg_number;
    seg_length_done_ = 0.0;
	}
	
	ROS_DEBUG("Old index  %d, new %d/%d", old_index, seg_index_, path_.size());
	ROS_DEBUG("Old segnum %d, new %d", old_segnum, seg_number_);
	
  
}

void IdealStateGenerator::preemptPathCallback() {
  ROS_DEBUG("%s: Action server preempted.", action_name_.c_str());
  as_.setPreempted();
}

int main(int argc, char *argv[]) {
  ros::init(argc, argv, "ideal_state_generator");

  IdealStateGenerator idealState;

  ros::MultiThreadedSpinner spinner(3);
  spinner.spin();
  return 0;
}
