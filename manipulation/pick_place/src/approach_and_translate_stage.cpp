/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Willow Garage, Inc.
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
*   * Neither the name of the Willow Garage nor the names of its
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
*********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/pick_place/pick_place.h>
#include <moveit/pick_place/approach_and_translate_stage.h>
#include <moveit/trajectory_processing/trajectory_tools.h>
#include <eigen_conversions/eigen_msg.h>
#include <ros/console.h>

namespace pick_place
{

ApproachAndTranslateStage::ApproachAndTranslateStage(const planning_scene::PlanningSceneConstPtr &pre_grasp_scene,
                                                     const planning_scene::PlanningSceneConstPtr &post_grasp_scene,
                                                     const collision_detection::AllowedCollisionMatrixConstPtr &collision_matrix) :
  ManipulationStage("approach & translate"),
  pre_grasp_planning_scene_(pre_grasp_scene),
  post_grasp_planning_scene_(post_grasp_scene),
  collision_matrix_(collision_matrix),
  max_goal_count_(5),
  max_fail_(3),
  max_step_(0.02),
  jump_factor_(2.0)
{
}

namespace
{

bool isStateCollisionFree(const planning_scene::PlanningScene *planning_scene, 
                          const collision_detection::AllowedCollisionMatrix *collision_matrix,
                          const sensor_msgs::JointState *grasp_posture, 
                          robot_state::JointStateGroup *joint_state_group,
                          const std::vector<double> &joint_group_variable_values)
{
  joint_state_group->setVariableValues(joint_group_variable_values);
  // apply the grasp posture for the end effector (we always apply it here since it could be the case the sampler changes this posture)
  joint_state_group->getRobotState()->setStateValues(*grasp_posture);
  return !planning_scene->isStateColliding(*joint_state_group->getRobotState(), joint_state_group->getName()) && 
    planning_scene->isStateFeasible(*joint_state_group->getRobotState());
}

bool samplePossibleGoalStates(const ManipulationPlanPtr &plan, const robot_state::RobotState &reference_state, double min_distance, unsigned int attempts) 
{
  // initialize with scene state 
  robot_state::RobotStatePtr token_state(new robot_state::RobotState(reference_state));
  robot_state::JointStateGroup *jsg = token_state->getJointStateGroup(plan->shared_data_->planning_group_);
  for (unsigned int j = 0 ; j < attempts ; ++j)
  {
    double min_d = std::numeric_limits<double>::infinity();
    if (plan->goal_sampler_->sample(jsg, *token_state, plan->shared_data_->max_goal_sampling_attempts_))
    {
      for (std::size_t i = 0 ; i < plan->possible_goal_states_.size() ; ++i)
      {
        double d = plan->possible_goal_states_[i]->getJointStateGroup(plan->shared_data_->planning_group_)->distance(jsg);
        if (d < min_d)
          min_d = d;
      }
      if (min_d >= min_distance)
      {
        plan->possible_goal_states_.push_back(token_state);
        return true;
      }
    }
  }
  return false;
}

void addGraspTrajectory(const ManipulationPlanPtr &plan, const sensor_msgs::JointState &grasp_posture, const std::string &name) 
{
  if (!grasp_posture.name.empty())
  {
    robot_state::RobotStatePtr state(new robot_state::RobotState(plan->trajectories_.back()->getLastWayPoint()));
    state->setStateValues(grasp_posture);
    robot_trajectory::RobotTrajectoryPtr traj(new robot_trajectory::RobotTrajectory(state->getRobotModel(), plan->shared_data_->end_effector_group_));
    traj->addSuffixWayPoint(state, PickPlace::DEFAULT_GRASP_POSTURE_COMPLETION_DURATION);
    plan->trajectories_.push_back(traj);
    plan->trajectory_descriptions_.push_back(name);
  }
}

}

bool ApproachAndTranslateStage::evaluate(const ManipulationPlanPtr &plan) const
{
  const robot_model::JointModelGroup *jmg = pre_grasp_planning_scene_->getRobotModel()->getJointModelGroup(plan->shared_data_->planning_group_);
  // compute what the maximum distance reported between any two states in the planning group could be, and keep 1% of that;
  // this is the minimum distance between sampled goal states 
  double min_distance = 0.01 * jmg->getMaximumExtent();
  
  // convert approach direction and retreat direction to Eigen structures
  Eigen::Vector3d approach_direction, retreat_direction;
  tf::vectorMsgToEigen(plan->approach_.direction.vector, approach_direction);
  tf::vectorMsgToEigen(plan->retreat_.direction.vector, retreat_direction);
  
  // transform the input vectors in accordance to frame specified in the header; 
  pre_grasp_planning_scene_->getTransforms()->transformVector3(plan->approach_.direction.header.frame_id, approach_direction, approach_direction);
  pre_grasp_planning_scene_->getTransforms()->transformVector3(plan->retreat_.direction.header.frame_id, retreat_direction, retreat_direction);
  
  // state validity checking during the approach must ensure that the gripper posture is that for pre-grasping
  robot_state::StateValidityCallbackFn approach_validCallback = boost::bind(&isStateCollisionFree, pre_grasp_planning_scene_.get(), 
                                                                            collision_matrix_.get(), &plan->approach_posture_, _1, _2);
  
  // state validity checking during the retreat after the grasp must ensure the gripper posture is that of the actual grasp
  robot_state::StateValidityCallbackFn retreat_validCallback = boost::bind(&isStateCollisionFree, post_grasp_planning_scene_.get(),
                                                                           collision_matrix_.get(), &plan->retreat_posture_, _1, _2);
  do 
  {
    for (std::size_t i = 0 ; i < plan->possible_goal_states_.size() && !signal_stop_ ; ++i)
    {
      // try to compute a straight line path that arrives at the goal using the specified approach direction
      robot_state::RobotStatePtr first_approach_state(new robot_state::RobotState(*plan->possible_goal_states_[i]));
      std::vector<robot_state::RobotStatePtr> approach_states;
      double d_approach = first_approach_state->getJointStateGroup(plan->shared_data_->planning_group_)->
        computeCartesianPath(approach_states, plan->shared_data_->ik_link_name_,
                             -approach_direction, false, plan->approach_.desired_distance, 
                             max_step_, jump_factor_, approach_validCallback);
      
      // if we were able to follow the approach direction for sufficient length, try to compute a retreat direction
      if (d_approach > plan->approach_.min_distance && !signal_stop_)
      {
        if (plan->retreat_.desired_distance > 0.0)
        {
          
          // try to compute a straight line path that moves from the goal in a desired direction
          robot_state::RobotStatePtr last_retreat_state(new robot_state::RobotState(*plan->possible_goal_states_[i]));  
          std::vector<robot_state::RobotStatePtr> retreat_states;
          double d_retreat = last_retreat_state->getJointStateGroup(plan->shared_data_->planning_group_)->
            computeCartesianPath(retreat_states, plan->shared_data_->ik_link_name_, 
                                 retreat_direction, true, plan->retreat_.desired_distance, 
                                 max_step_, jump_factor_, retreat_validCallback);
          
          // if sufficient progress was made in the desired direction, we have a goal state that we can consider for future stages
          if (d_retreat > plan->retreat_.min_distance && !signal_stop_)
          {
            std::reverse(approach_states.begin(), approach_states.end());
            robot_trajectory::RobotTrajectoryPtr approach_traj(new robot_trajectory::RobotTrajectory(pre_grasp_planning_scene_->getRobotModel(), plan->shared_data_->planning_group_));
            for (std::size_t k = 0 ; k < approach_states.size() ; ++k)
              approach_traj->addSuffixWayPoint(approach_states[k], 0.0);
            
            robot_trajectory::RobotTrajectoryPtr retreat_traj(new robot_trajectory::RobotTrajectory(post_grasp_planning_scene_->getRobotModel(), plan->shared_data_->planning_group_));
            for (std::size_t k = 0 ; k < retreat_states.size() ; ++k)
              retreat_traj->addSuffixWayPoint(retreat_states[k], 0.0);
            
            time_param_.computeTimeStamps(*approach_traj); 
            time_param_.computeTimeStamps(*retreat_traj);            
            
            plan->trajectories_.push_back(approach_traj);
            plan->trajectory_descriptions_.push_back("approach");
            
            addGraspTrajectory(plan, plan->retreat_posture_, "grasp");
            
            plan->trajectories_.push_back(retreat_traj);
            plan->trajectory_descriptions_.push_back("retreat");
            
            plan->approach_state_ = approach_states.front();
            return true;          
          }
        }
        else
        {          
          plan->approach_state_.swap(first_approach_state);
          std::reverse(approach_states.begin(), approach_states.end());
          robot_trajectory::RobotTrajectoryPtr approach_traj(new robot_trajectory::RobotTrajectory(pre_grasp_planning_scene_->getRobotModel(), plan->shared_data_->planning_group_));
          for (std::size_t k = 0 ; k < approach_states.size() ; ++k)
            approach_traj->addSuffixWayPoint(approach_states[k], 0.0);
          
          time_param_.computeTimeStamps(*approach_traj);
          
          plan->trajectories_.push_back(approach_traj);
          plan->trajectory_descriptions_.push_back("approach");
          
          addGraspTrajectory(plan, plan->retreat_posture_, "grasp");
          plan->approach_state_ = approach_states.front();
          
          return true;          
        }
      }
      
    }
  }
  while (plan->possible_goal_states_.size() < max_goal_count_ && !signal_stop_ && samplePossibleGoalStates(plan, pre_grasp_planning_scene_->getCurrentState(), min_distance, max_fail_));
  plan->error_code_.val = moveit_msgs::MoveItErrorCodes::PLANNING_FAILED;
  
  return false;
}

}
