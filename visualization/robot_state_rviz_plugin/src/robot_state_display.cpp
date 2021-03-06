/*
 * Copyright (c) 2008, Willow Garage, Inc.
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

/* Author: Ioan Sucan */

#include <moveit/robot_state_rviz_plugin/robot_state_display.h>
#include <moveit/robot_state/conversions.h>

#include <rviz/visualization_manager.h>
#include <rviz/robot/robot.h>
#include <rviz/robot/robot_link.h>

#include <rviz/properties/property.h>
#include <rviz/properties/string_property.h>
#include <rviz/properties/bool_property.h>
#include <rviz/properties/float_property.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/properties/color_property.h>
#include <rviz/display_context.h>
#include <rviz/frame_manager.h>
#include <tf/transform_listener.h>

#include <OGRE/OgreSceneManager.h>

namespace moveit_rviz_plugin
{

// ******************************************************************************************
// Base class contructor
// ******************************************************************************************
RobotStateDisplay::RobotStateDisplay() :
  Display(),
  update_state_(false)
{
  robot_description_property_ =
    new rviz::StringProperty( "Robot Description", "robot_description", "The name of the ROS parameter where the URDF for the robot is loaded",
                              this,
                              SLOT( changedRobotDescription() ), this );
  
  robot_state_topic_property_ =
    new rviz::RosTopicProperty( "Robot State Topic", "display_robot_state",
                                ros::message_traits::datatype<moveit_msgs::DisplayRobotState>(),
                                "The topic on which the moveit_msgs::RobotState messages are received",
                                this,
                                SLOT( changedRobotStateTopic() ), this );

  // Planning scene category -------------------------------------------------------------------------------------------
  root_link_name_property_ =
    new rviz::StringProperty( "Robot Root Link", "", "Shows the name of the root link for the robot model",
                              this,
                              SLOT( changedRootLinkName() ), this );
  root_link_name_property_->setReadOnly(true);
  
  robot_alpha_property_ =
    new rviz::FloatProperty( "Robot Alpha", 1.0f, "Specifies the alpha for the robot links",
                             this,
                             SLOT( changedRobotSceneAlpha() ), this );
  robot_alpha_property_->setMin( 0.0 );
  robot_alpha_property_->setMax( 1.0 );

  attached_body_color_property_ = new rviz::ColorProperty( "Attached Body Color", QColor(150, 50, 150), "The color for the attached bodies",
                                                           this,
                                                           SLOT( changedAttachedBodyColor() ), this );

  enable_link_highlight_ = new rviz::BoolProperty("Show Highlights", true, "Specifies whether link highlighting is enabled", 
                                                  this, SLOT( changedEnableLinkHighlight() ), this);
}

// ******************************************************************************************
// Deconstructor
// ******************************************************************************************
RobotStateDisplay::~RobotStateDisplay()
{ 
}

void RobotStateDisplay::onInitialize()
{  
  Display::onInitialize();
  robot_.reset(new RobotStateVisualization(scene_node_, context_, "Robot State", this));
  robot_->setVisible(true);
}

void RobotStateDisplay::reset()
{ 
  robot_->clear();
  rdf_loader_.reset();

  loadRobotModel("");
  Display::reset();
  
  robot_->setVisible(true);
}

void RobotStateDisplay::changedEnableLinkHighlight()
{
  // todo
}

void RobotStateDisplay::changedAttachedBodyColor()
{
  if (robot_)
  {
    QColor color = attached_body_color_property_->getColor();
    std_msgs::ColorRGBA color_msg;
    color_msg.r = color.redF();
    color_msg.g = color.greenF();
    color_msg.b = color.blueF();
    color_msg.a = robot_alpha_property_->getFloat();
    robot_->setDefaultAttachedObjectColor(color_msg);
    update_state_ = true;
  }
}

void RobotStateDisplay::changedRobotDescription()
{
  if (isEnabled())
    reset();
}

void RobotStateDisplay::changedRootLinkName()
{
}

void RobotStateDisplay::changedRobotSceneAlpha()
{
  if (robot_)
  {
    robot_->setAlpha(robot_alpha_property_->getFloat());  
    QColor color = attached_body_color_property_->getColor();
    std_msgs::ColorRGBA color_msg;
    color_msg.r = color.redF();
    color_msg.g = color.greenF();
    color_msg.b = color.blueF();
    color_msg.a = robot_alpha_property_->getFloat();
    robot_->setDefaultAttachedObjectColor(color_msg);
    update_state_ = true;
  }
}

void RobotStateDisplay::changedRobotStateTopic()
{
  robot_state_subscriber_.shutdown();
  robot_state_subscriber_ = root_nh_.subscribe(robot_state_topic_property_->getStdString(), 10, &RobotStateDisplay::newRobotStateCallback, this);
  robot_->clear();
  loadRobotModel("");
}

void RobotStateDisplay::newRobotStateCallback(const moveit_msgs::DisplayRobotStateConstPtr &state)
{
  if (!kmodel_)
    return;
  if (!kstate_)
    kstate_.reset(new robot_state::RobotState(kmodel_)); 
  // possibly use TF to construct a robot_state::Transforms object to pass in to the conversion functio?
  robot_state::robotStateMsgToRobotState(state->state, *kstate_);
  update_state_ = true;
}

void RobotStateDisplay::setLinkColor(const std::string& link_name, const QColor &color)
{
  setLinkColor(&robot_->getRobot(), link_name, color );
}

void RobotStateDisplay::unsetLinkColor(const std::string& link_name)
{
  unsetLinkColor(&robot_->getRobot(), link_name);
}

void RobotStateDisplay::setLinkColor(rviz::Robot* robot,  const std::string& link_name, const QColor &color )
{
  rviz::RobotLink *link = robot->getLink(link_name);
  
  // Check if link exists
  if (link)
    link->setColor( color.redF(), color.greenF(), color.blueF() );
}

void RobotStateDisplay::unsetLinkColor(rviz::Robot* robot, const std::string& link_name )
{
  rviz::RobotLink *link = robot->getLink(link_name);

  // Check if link exists
  if (link) 
    link->unsetColor();
}

// ******************************************************************************************
// Load
// ******************************************************************************************
void RobotStateDisplay::loadRobotModel(const std::string &root_link)
{
  if (!rdf_loader_)
    rdf_loader_.reset(new rdf_loader::RDFLoader(robot_description_property_->getStdString()));
  
  if (rdf_loader_->getURDF())
  {
    const boost::shared_ptr<srdf::Model> &srdf = rdf_loader_->getSRDF() ? rdf_loader_->getSRDF() : boost::shared_ptr<srdf::Model>(new srdf::Model());
    if (root_link.empty())
      kmodel_.reset(new robot_model::RobotModel(rdf_loader_->getURDF(), srdf));
    else
      kmodel_.reset(new robot_model::RobotModel(rdf_loader_->getURDF(), srdf, root_link));
    robot_->load(*kmodel_->getURDF());
    kstate_.reset(new robot_state::RobotState(kmodel_));
    kstate_->setToDefaultValues();
    bool oldState = root_link_name_property_->blockSignals(true);
    root_link_name_property_->setStdString(getRobotModel()->getRootLinkName());
    root_link_name_property_->blockSignals(oldState);
    update_state_ = true;
    setStatus( rviz::StatusProperty::Ok, "RobotState", "Planning Model Loaded Successfully" );
  }
  else
    setStatus( rviz::StatusProperty::Error, "RobotState", "No Planning Model Loaded" );
}

void RobotStateDisplay::onEnable()
{
  Display::onEnable();
  loadRobotModel("");
  if (robot_)
    robot_->setVisible(true);
  calculateOffsetPosition();
}

// ******************************************************************************************
// Disable
// ******************************************************************************************
void RobotStateDisplay::onDisable()
{
  if (robot_)
    robot_->setVisible(false);
  Display::onDisable();
}

void RobotStateDisplay::update(float wall_dt, float ros_dt)
{
  Display::update(wall_dt, ros_dt);
  if (robot_ && update_state_)
  {
    update_state_ = false;
    robot_->update(kstate_);
  }
}

// ******************************************************************************************
// Calculate Offset Position
// ******************************************************************************************
void RobotStateDisplay::calculateOffsetPosition()
{  
  if (!getRobotModel())
    return;

  ros::Time stamp;
  std::string err_string;
  if (context_->getTFClient()->getLatestCommonTime(fixed_frame_.toStdString(), getRobotModel()->getModelFrame(), stamp, &err_string) != tf::NO_ERROR)
    return;

  tf::Stamped<tf::Pose> pose(tf::Pose::getIdentity(), stamp, getRobotModel()->getModelFrame());

  if (context_->getTFClient()->canTransform(fixed_frame_.toStdString(), getRobotModel()->getModelFrame(), stamp))
  {
    try
    {
      context_->getTFClient()->transformPose(fixed_frame_.toStdString(), pose, pose);
    }
    catch (tf::TransformException& e)
    {
      ROS_ERROR( "Error transforming from frame '%s' to frame '%s'", pose.frame_id_.c_str(), fixed_frame_.toStdString().c_str() );
    }
  }

  Ogre::Vector3 position(pose.getOrigin().x(), pose.getOrigin().y(), pose.getOrigin().z());
  const tf::Quaternion &q = pose.getRotation();
  Ogre::Quaternion orientation( q.getW(), q.getX(), q.getY(), q.getZ() );
  scene_node_->setPosition(position);
  scene_node_->setOrientation(orientation);
}

void RobotStateDisplay::fixedFrameChanged()
{
  Display::fixedFrameChanged(); 
  calculateOffsetPosition();  
}


} // namespace moveit_rviz_plugin
