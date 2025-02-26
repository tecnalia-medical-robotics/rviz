/*
 * Copyright (c) 2012, Willow Garage, Inc.
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

#include <boost/bind/bind.hpp>

#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreRectangle2D.h>
#include <OgreRenderSystem.h>
#include <OgreRenderWindow.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>
#include <OgreTechnique.h>
#include <OgreCamera.h>

#include <tf2_ros/message_filter.h>

#include <rviz/bit_allocator.h>
#include <rviz/frame_manager.h>
#include <rviz/ogre_helpers/axes.h>
#include <rviz/ogre_helpers/compatibility.h>
#include <rviz/properties/enum_property.h>
#include <rviz/properties/float_property.h>
#include <rviz/properties/int_property.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/panel_dock_widget.h>
#include <rviz/render_panel.h>
#include <rviz/uniform_string_stream.h>
#include <rviz/validate_floats.h>
#include <rviz/display_context.h>
#include <rviz/properties/display_group_visibility_property.h>
#include <rviz/load_resource.h>

#include <image_transport/camera_common.h>

#include "camera_display.h"

namespace rviz
{
const QString CameraDisplay::BACKGROUND("background");
const QString CameraDisplay::OVERLAY("overlay");
const QString CameraDisplay::BOTH("background and overlay");

bool validateFloats(const sensor_msgs::CameraInfo& msg)
{
  bool valid = true;
  valid = valid && validateFloats(msg.D);
  valid = valid && validateFloats(msg.K);
  valid = valid && validateFloats(msg.R);
  valid = valid && validateFloats(msg.P);
  return valid;
}

CameraDisplay::CameraDisplay()
  : ImageDisplayBase(), texture_(), render_panel_(nullptr), caminfo_ok_(false), force_render_(false)
{
  image_position_property_ =
      new EnumProperty("Image Rendering", BOTH,
                       "Render the image behind all other geometry or overlay it on top, or both.", this,
                       &CameraDisplay::forceRender);
  image_position_property_->addOption(BACKGROUND);
  image_position_property_->addOption(OVERLAY);
  image_position_property_->addOption(BOTH);

  alpha_property_ = new FloatProperty(
      "Overlay Alpha", 0.5,
      "The amount of transparency to apply to the camera image when rendered as overlay.", this,
      &CameraDisplay::updateAlpha);
  alpha_property_->setMin(0);
  alpha_property_->setMax(1);

  zoom_property_ = new FloatProperty(
      "Zoom Factor", 1.0,
      "Set a zoom factor below 1 to see a larger part of the world, above 1 to magnify the image.", this,
      &CameraDisplay::forceRender);
  zoom_property_->setMin(0.00001);
  zoom_property_->setMax(100000);
}

CameraDisplay::~CameraDisplay()
{
  if (initialized())
  {
    render_panel_->getRenderWindow()->removeListener(this);

    CameraDisplay::unsubscribe();

    delete render_panel_;
    delete bg_screen_rect_;
    delete fg_screen_rect_;

    removeAndDestroyChildNode(bg_scene_node_->getParentSceneNode(), bg_scene_node_);
    removeAndDestroyChildNode(fg_scene_node_->getParentSceneNode(), fg_scene_node_);

    context_->visibilityBits()->freeBits(vis_bit_);
  }
}

void CameraDisplay::onInitialize()
{
  ImageDisplayBase::onInitialize();

  bg_scene_node_ = scene_node_->createChildSceneNode();
  fg_scene_node_ = scene_node_->createChildSceneNode();

  {
    static int count = 0;
    UniformStringStream ss;
    ss << "CameraDisplayObject" << count++;

    // background rectangle
    bg_screen_rect_ = new Ogre::Rectangle2D(true);
    bg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    ss << "Material";
    bg_material_ = Ogre::MaterialManager::getSingleton().create(
        ss.str(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    bg_material_->setDepthWriteEnabled(false);

    bg_material_->setReceiveShadows(false);
    bg_material_->setDepthCheckEnabled(false);

    bg_material_->getTechnique(0)->setLightingEnabled(false);
    Ogre::TextureUnitState* tu = bg_material_->getTechnique(0)->getPass(0)->createTextureUnitState();
    tu->setTextureName(texture_.getTexture()->getName());
    tu->setTextureFiltering(Ogre::TFO_NONE);
    tu->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    tu->setAlphaOperation(Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 0.0);

    bg_material_->setCullingMode(Ogre::CULL_NONE);
    bg_material_->setSceneBlending(Ogre::SBT_REPLACE);

    Ogre::AxisAlignedBox aabInf;
    aabInf.setInfinite();

    bg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_BACKGROUND);
    bg_screen_rect_->setBoundingBox(aabInf);
    setMaterial(*bg_screen_rect_, bg_material_);

    bg_scene_node_->attachObject(bg_screen_rect_);
    bg_scene_node_->setVisible(false);

    // overlay rectangle
    fg_screen_rect_ = new Ogre::Rectangle2D(true);
    fg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    fg_material_ = bg_material_->clone(ss.str() + "fg");
    fg_screen_rect_->setBoundingBox(aabInf);
    setMaterial(*fg_screen_rect_, fg_material_);

    fg_material_->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    fg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);

    fg_scene_node_->attachObject(fg_screen_rect_);
    fg_scene_node_->setVisible(false);
  }

  updateAlpha();

  render_panel_ = new RenderPanel();
  render_panel_->getRenderWindow()->addListener(this);
  render_panel_->getRenderWindow()->setAutoUpdated(false);
  render_panel_->getRenderWindow()->setActive(false);
  render_panel_->resize(640, 480);
  render_panel_->initialize(context_->getSceneManager(), context_);

  setAssociatedWidget(render_panel_);
  if (auto* dock = getAssociatedWidgetPanel())
    dock->addMaximizeButton();

  render_panel_->setAutoRender(false);
  render_panel_->setOverlaysEnabled(false);
  render_panel_->getCamera()->setNearClipDistance(0.01f);

  vis_bit_ = context_->visibilityBits()->allocBit();
  render_panel_->getViewport()->setVisibilityMask(vis_bit_);

  visibility_property_ =
      new DisplayGroupVisibilityProperty(vis_bit_, context_->getRootDisplayGroup(), this, "Visibility",
                                         true,
                                         "Changes the visibility of other Displays in the camera view.");

  visibility_property_->setIcon(loadPixmap("package://rviz/icons/visibility.svg", true));

  this->addChild(visibility_property_, 0);
}

void CameraDisplay::preRenderTargetUpdate(const Ogre::RenderTargetEvent& /*evt*/)
{
  QString image_position = image_position_property_->getString();
  bg_scene_node_->setVisible(caminfo_ok_ && (image_position == BACKGROUND || image_position == BOTH));
  fg_scene_node_->setVisible(caminfo_ok_ && (image_position == OVERLAY || image_position == BOTH));

  // set view flags on all displays
  visibility_property_->update();
}

void CameraDisplay::postRenderTargetUpdate(const Ogre::RenderTargetEvent& /*evt*/)
{
  bg_scene_node_->setVisible(false);
  fg_scene_node_->setVisible(false);
}

void CameraDisplay::onEnable()
{
  subscribe();
  render_panel_->getRenderWindow()->setActive(true);
}

void CameraDisplay::onDisable()
{
  render_panel_->getRenderWindow()->setActive(false);
  unsubscribe();
  reset();
}

void CameraDisplay::subscribe()
{
  if ((!isEnabled()) || (topic_property_->getTopicStd().empty()))
  {
    return;
  }

  std::string target_frame = fixed_frame_.toStdString();
  ImageDisplayBase::enableTFFilter(target_frame);
  ImageDisplayBase::subscribe();

  try
  {
    const std::string caminfo_topic =
        image_transport::getCameraInfoTopic(topic_property_->getTopicStd());
    caminfo_sub_ = update_nh_.subscribe(caminfo_topic, 1, &CameraDisplay::processCamInfoMessage, this);
  }
  catch (ros::Exception& e)
  {
    setStatus(StatusProperty::Error, "Camera Info", QString("Error subscribing: ") + e.what());
  }
}

void CameraDisplay::unsubscribe()
{
  ImageDisplayBase::unsubscribe();
  caminfo_sub_.shutdown();

  boost::mutex::scoped_lock lock(caminfo_mutex_);
  current_caminfo_.reset();
}

void CameraDisplay::updateAlpha()
{
  float alpha = alpha_property_->getFloat();

  Ogre::Pass* pass = fg_material_->getTechnique(0)->getPass(0);
  if (pass->getNumTextureUnitStates() > 0)
  {
    Ogre::TextureUnitState* tex_unit = pass->getTextureUnitState(0);
    tex_unit->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, alpha);
  }
  else
  {
    fg_material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, alpha));
    fg_material_->setDiffuse(Ogre::ColourValue(0.0f, 1.0f, 1.0f, alpha));
  }

  force_render_ = true;
  context_->queueRender();
}

void CameraDisplay::forceRender()
{
  force_render_ = true;
  context_->queueRender();
}

void CameraDisplay::updateQueueSize()
{
  ImageDisplayBase::updateQueueSize();
}

void CameraDisplay::update(float /*wall_dt*/, float /*ros_dt*/)
{
  try
  {
    if (texture_.update() || force_render_)
    {
      caminfo_ok_ = updateCamera();
      force_render_ = false;
    }
  }
  catch (UnsupportedImageEncoding& e)
  {
    setStatus(StatusProperty::Error, "Image", e.what());
  }

  render_panel_->getRenderWindow()->update();
}

bool CameraDisplay::updateCamera()
{
  sensor_msgs::CameraInfo::ConstPtr info;
  sensor_msgs::Image::ConstPtr image;
  {
    boost::mutex::scoped_lock lock(caminfo_mutex_);

    info = current_caminfo_;
    image = texture_.getImage();
  }

  if (!info || !image)
  {
    return false;
  }

  if (image->header.frame_id != info->header.frame_id)
    setStatus(StatusProperty::Warn, "Image frame",
              QString("Image frame (%1) doesn't match camera frame (%2)")
                  .arg(QString::fromStdString(image->header.frame_id),
                       QString::fromStdString(info->header.frame_id)));
  else
    deleteStatus("Image frame");

  if (!validateFloats(*info))
  {
    setStatus(StatusProperty::Error, "Camera Info",
              "Contains invalid floating point values (nans or infs)");
    return false;
  }

  // if we're in 'exact' time mode, only show image if the time is exactly right
  ros::Time rviz_time = context_->getFrameManager()->getTime();
  if (context_->getFrameManager()->getSyncMode() == FrameManager::SyncExact &&
      rviz_time != image->header.stamp)
  {
    std::ostringstream s;
    s << "Time-syncing active and no image at timestamp " << rviz_time.toSec() << ".";
    setStatus(StatusProperty::Warn, "Time", s.str().c_str());
    return false;
  }

  Ogre::Vector3 position;
  Ogre::Quaternion orientation;
  context_->getFrameManager()->getTransform(image->header.frame_id, image->header.stamp, position,
                                            orientation);

  // printf( "CameraDisplay:updateCamera(): pos = %.2f, %.2f, %.2f.\n", position.x, position.y,
  // position.z );

  // convert vision (Z-forward) frame to ogre frame (Z-out)
  orientation = orientation * Ogre::Quaternion(Ogre::Degree(180), Ogre::Vector3::UNIT_X);

  float img_width = info->width;
  float img_height = info->height;

  // If the image width is 0 due to a malformed caminfo, try to grab the width from the image.
  if (img_width == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], width = 0", qPrintable(getName()));
    img_width = texture_.getWidth();
  }

  if (img_height == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], height = 0", qPrintable(getName()));
    img_height = texture_.getHeight();
  }

  if (img_height == 0.0 || img_width == 0.0)
  {
    setStatus(StatusProperty::Error, "Camera Info",
              "Could not determine width/height of image due to malformed CameraInfo "
              "(either width or height is 0)");
    return false;
  }

  double fx = info->P[0];
  double fy = info->P[5];

  float win_width = render_panel_->width();
  float win_height = render_panel_->height();
  float zoom_x = zoom_property_->getFloat();
  float zoom_y = zoom_x;

  // Preserve aspect ratio
  if (win_width != 0 && win_height != 0)
  {
    float img_aspect = (img_width / fx) / (img_height / fy);
    float win_aspect = win_width / win_height;

    if (img_aspect > win_aspect)
    {
      zoom_y = zoom_y / img_aspect * win_aspect;
    }
    else
    {
      zoom_x = zoom_x / win_aspect * img_aspect;
    }
  }

  // Add the camera's translation relative to the left camera (from P[3]);
  double tx = -1 * (info->P[3] / fx);
  Ogre::Vector3 right = orientation * Ogre::Vector3::UNIT_X;
  position = position + (right * tx);

  double ty = -1 * (info->P[7] / fy);
  Ogre::Vector3 down = orientation * Ogre::Vector3::UNIT_Y;
  position = position + (down * ty);

  if (!validateFloats(position))
  {
    setStatus(StatusProperty::Error, "Camera Info",
              "CameraInfo/P resulted in an invalid position calculation (nans or infs)");
    return false;
  }

  render_panel_->getCamera()->setPosition(position);
  render_panel_->getCamera()->setOrientation(orientation);

  // calculate the projection matrix
  double cx = info->P[2];
  double cy = info->P[6];

  double far_plane = 100;
  double near_plane = 0.01;

  Ogre::Matrix4 proj_matrix;
  proj_matrix = Ogre::Matrix4::ZERO;

  proj_matrix[0][0] = 2.0 * fx / img_width * zoom_x;
  proj_matrix[1][1] = 2.0 * fy / img_height * zoom_y;

  proj_matrix[0][2] = 2.0 * (0.5 - cx / img_width) * zoom_x;
  proj_matrix[1][2] = 2.0 * (cy / img_height - 0.5) * zoom_y;

  proj_matrix[2][2] = -(far_plane + near_plane) / (far_plane - near_plane);
  proj_matrix[2][3] = -2.0 * far_plane * near_plane / (far_plane - near_plane);

  proj_matrix[3][2] = -1;

  render_panel_->getCamera()->setCustomProjectionMatrix(true, proj_matrix);

#if 0
  static Axes* debug_axes = new Axes(scene_manager_, 0, 0.2, 0.01);
  debug_axes->setPosition(position);
  debug_axes->setOrientation(orientation);
#endif

  // adjust the image rectangles to fit the zoom & aspect ratio
  double x_corner_start, y_corner_start, x_corner_end, y_corner_end;

  if (info->roi.height != 0 || info->roi.width != 0)
  {
    // corners are computed according to roi
    x_corner_start = (2.0 * info->roi.x_offset / info->width - 1.0) * zoom_x;
    y_corner_start = (-2.0 * info->roi.y_offset / info->height + 1.0) * zoom_y;
    x_corner_end = x_corner_start + (2.0 * info->roi.width / info->width) * zoom_x;
    y_corner_end = y_corner_start - (2.0 * info->roi.height / info->height) * zoom_y;
  }
  else
  {
    x_corner_start = -1.0f * zoom_x;
    y_corner_start = 1.0f * zoom_y;
    x_corner_end = 1.0f * zoom_x;
    y_corner_end = -1.0f * zoom_y;
  }

  bg_screen_rect_->setCorners(x_corner_start, y_corner_start, x_corner_end, y_corner_end);
  fg_screen_rect_->setCorners(x_corner_start, y_corner_start, x_corner_end, y_corner_end);

  Ogre::AxisAlignedBox aabInf;
  aabInf.setInfinite();
  bg_screen_rect_->setBoundingBox(aabInf);
  fg_screen_rect_->setBoundingBox(aabInf);

  setStatus(StatusProperty::Ok, "Time", "OK");
  setStatus(StatusProperty::Ok, "Camera Info", "processed");

  return true;
}

void CameraDisplay::processMessage(const sensor_msgs::Image::ConstPtr& msg)
{
  texture_.addMessage(msg);
}

void CameraDisplay::processCamInfoMessage(const sensor_msgs::CameraInfo::ConstPtr& msg)
{
  boost::mutex::scoped_lock lock(caminfo_mutex_);
  current_caminfo_ = msg;
  setStatus(StatusProperty::Ok, "Camera Info", "received");
}

void CameraDisplay::fixedFrameChanged()
{
  ImageDisplayBase::fixedFrameChanged();
}

void CameraDisplay::reset()
{
  ImageDisplayBase::reset();
  // We explicitly do not reset current_caminfo_ here: If we are subscribed to a latched caminfo topic,
  // we will not receive another message after reset, i.e. the caminfo could not be recovered.
  // Thus, we reset caminfo only if unsubscribing.

  const std::string topic = topic_property_->getTopicStd();
  if (!topic.empty())
  {
    const std::string caminfo_topic = image_transport::getCameraInfoTopic(topic);
    boost::mutex::scoped_lock lock(caminfo_mutex_);
    if (!current_caminfo_)
      setStatus(StatusProperty::Warn, "Camera Info",
                "No CameraInfo received on [" + QString::fromStdString(caminfo_topic) +
                    "].\nTopic may not exist.");
  }
  texture_.clear();
  force_render_ = true;
  context_->queueRender();
  render_panel_->getCamera()->setPosition(Ogre::Vector3(999999, 999999, 999999));
}

} // namespace rviz

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(rviz::CameraDisplay, rviz::Display)
