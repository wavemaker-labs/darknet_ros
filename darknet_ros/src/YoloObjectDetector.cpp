/*
 * YoloObjectDetector.cpp
 *
 *  Created on: Dec 19, 2016
 *      Author: Marko Bjelonic
 *   Institute: ETH Zurich, Robotic Systems Lab
 * 
 *  Modified: 
 *      Bhooshan Deshpande
 *      Wavemaker Labs, Inc
 */

// yolo object detector
#include "darknet_ros/YoloObjectDetector.hpp"
#include <typeinfo>
#include <X11/Xlib.h>

#ifdef DARKNET_FILE_PATH
std::string darknetFilePath_ = DARKNET_FILE_PATH;
#else
#error Path of darknet repository is not defined in CMakeLists.txt.
#endif

namespace darknet_ros {

char* cfg;
char* weights;
char* data;
char** detectionNames;

YoloObjectDetector::YoloObjectDetector(ros::NodeHandle nh)
    : nodeHandle_(nh), 
      imageTransport_(nodeHandle_), 
      numClasses_(0), 
      classLabels_(0), 
      rosBoxes_(0), 
      rosBoxCounter_(0),
      imagergb_sub(imageTransport_,"/camera/color/image_raw",1),                      //For depth inclusion
      imagedepth_sub(imageTransport_,"/camera/aligned_depth_to_color/image_raw",1),   //For depth inclusion
      sync_1(MySyncPolicy_1(5), imagergb_sub, imagedepth_sub)                         //For depth inclusion
  {
  ROS_INFO("[YoloObjectDetector] Node started.");

  // Read parameters from config file.
  if (!readParameters()) {
    ros::requestShutdown();
  }

  init();
}

YoloObjectDetector::~YoloObjectDetector() {
  {
    boost::unique_lock<boost::shared_mutex> lockNodeStatus(mutexNodeStatus_);
    isNodeRunning_ = false;
  }
  yoloThread_.join();
}

bool YoloObjectDetector::readParameters() {
  // Load common parameters.
  nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
  nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
  nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, false);

  // Check if Xserver is running on Linux.
  if (XOpenDisplay(NULL)) {
    // Do nothing!
    ROS_INFO("[YoloObjectDetector] Xserver is running.");
  } else {
    ROS_INFO("[YoloObjectDetector] Xserver is not running.");
    viewImage_ = false;
  }

  // Set vector sizes.
  nodeHandle_.param("yolo_model/detection_classes/names", classLabels_, std::vector<std::string>(0));
  numClasses_ = classLabels_.size();
  rosBoxes_ = std::vector<std::vector<RosBox_> >(numClasses_);
  rosBoxCounter_ = std::vector<int>(numClasses_);

  return true;
}

void YoloObjectDetector::init() {
  ROS_INFO("[YoloObjectDetector] init().");

  // Initialize deep network of darknet.
  std::string weightsPath;
  std::string configPath;
  std::string dataPath;
  std::string configModel;
  std::string weightsModel;

  // Threshold of object detection.
  float thresh;
  nodeHandle_.param("yolo_model/threshold/value", thresh, (float)0.3);

  // Path to weights file.
  nodeHandle_.param("yolo_model/weight_file/name", weightsModel, std::string("yolov2-tiny.weights"));
  nodeHandle_.param("weights_path", weightsPath, std::string("/default"));
  weightsPath += "/" + weightsModel;
  weights = new char[weightsPath.length() + 1];
  strcpy(weights, weightsPath.c_str());

  // Path to config file.
  nodeHandle_.param("yolo_model/config_file/name", configModel, std::string("yolov2-tiny.cfg"));
  nodeHandle_.param("config_path", configPath, std::string("/default"));
  configPath += "/" + configModel;
  cfg = new char[configPath.length() + 1];
  strcpy(cfg, configPath.c_str());

  // Path to data folder.
  dataPath = darknetFilePath_;
  dataPath += "/data";
  data = new char[dataPath.length() + 1];
  strcpy(data, dataPath.c_str());

  // Get classes.
  detectionNames = (char**)realloc((void*)detectionNames, (numClasses_ + 1) * sizeof(char*));
  for (int i = 0; i < numClasses_; i++) {
    detectionNames[i] = new char[classLabels_[i].length() + 1];
    strcpy(detectionNames[i], classLabels_[i].c_str());
  }

  // Load network.
  setupNetwork(cfg, weights, data, thresh, detectionNames, numClasses_, 0, 0, 1, 0.5, 0, 0, 0, 0);
  yoloThread_ = std::thread(&YoloObjectDetector::yolo, this);

  // Initialize publisher and subscriber.
  //RGB Camera Topic Subscriber Params
  std::string cameraTopicName;
  int cameraQueueSize;

  // Detection Message Publisher Params
  std::string objectDetectorTopicName;
  int objectDetectorQueueSize;
  bool objectDetectorLatch;

  // Bounding Boxes Publisher Params
  std::string boundingBoxesTopicName;
  int boundingBoxesQueueSize;
  bool boundingBoxesLatch;

  // Darknet Detection Image Publisher Params
  std::string detectionImageTopicName;
  int detectionImageQueueSize;
  bool detectionImageLatch;
  std::string depthTopicName; 

  //For depth inclusion
  //DepthSceneInfo Frame Publisher Params 
  int sceneDepthQueueSize;          
  std::string sceneDepthTopicName;
  bool sceneDepthLatch;

  //Detection Depth Image Publisher Params
  int detectionDepthImageQueueSize;          
  std::string detectionDepthImageTopicName;
  bool detectionDepthImageLatch;

  // Depth Image Subscriber Params
  int cameraDepthInfoQueueSize; 
  std::string cameraDepthInfoTopicName;




  //RGB image [SUB]
  nodeHandle_.param("subscribers/camera_reading/topic", cameraTopicName, std::string("/camera/color/image_raw"));
  nodeHandle_.param("subscribers/camera_reading/queue_size", cameraQueueSize, 1);

  // depth camera info topic [SUB]
  nodeHandle_.param("subscribers/depth_cam_info/topic", cameraDepthInfoTopicName, std::string("/camera/aligned_depth_to_color/camera_info"));   //For depth inclusion
  nodeHandle_.param("subscribers/depth_cam_info/queue_size", cameraDepthInfoQueueSize, 1);                                                      //For depth inclusion

  //Object Detector [PUB]
  nodeHandle_.param("publishers/object_detector/topic", objectDetectorTopicName, std::string("found_object"));
  nodeHandle_.param("publishers/object_detector/queue_size", objectDetectorQueueSize, 1);
  nodeHandle_.param("publishers/object_detector/latch", objectDetectorLatch, false);
  
  // Bounding boxes topic [PUB]
  nodeHandle_.param("publishers/bounding_boxes/topic", boundingBoxesTopicName, std::string("bounding_boxes"));
  nodeHandle_.param("publishers/bounding_boxes/queue_size", boundingBoxesQueueSize, 1);
  nodeHandle_.param("publishers/bounding_boxes/latch", boundingBoxesLatch, false);
  
  // Detection topic [PUB]
  nodeHandle_.param("publishers/detection_image/topic", detectionImageTopicName, std::string("detection_image"));
  nodeHandle_.param("publishers/detection_image/queue_size", detectionImageQueueSize, 1);
  nodeHandle_.param("publishers/detection_image/latch", detectionImageLatch, true);

  // Depth tagged Image Detection topic [PUB]
  nodeHandle_.param("publishers/detection_depth_image/topic", detectionDepthImageTopicName, std::string("detection_depth_image"));
  nodeHandle_.param("publishers/detection_depth_image/queue_size", detectionDepthImageQueueSize, 1);
  nodeHandle_.param("publishers/detection_depth_image/latch", detectionDepthImageLatch, true);
  
  // depth topic [PUB]
  nodeHandle_.param("publishers/object_depth/topic", sceneDepthTopicName, std::string("/object_depth/scene_depth_info"));   //For depth inclusion
  nodeHandle_.param("publishers/object_depth/queue_size", sceneDepthQueueSize, 1);                                          //For depth inclusion
  nodeHandle_.param("publishers/object_depth/latch", sceneDepthLatch, true);                                                //For depth inclusion


  // Replacing image callback with a approximately synchronized callback for depth and RGB images
  cameraDepthInfoSubscriber_ = nodeHandle_.subscribe(cameraDepthInfoTopicName, 10, &YoloObjectDetector::cameraDepthInfoCallback, this);
  sync_1.registerCallback(boost::bind(&YoloObjectDetector::cameraCallback,this,_1,_2));
  
  objectPublisher_ =
      nodeHandle_.advertise<darknet_ros_msgs::ObjectCount>(objectDetectorTopicName, objectDetectorQueueSize, objectDetectorLatch);
  boundingBoxesPublisher_ =
      nodeHandle_.advertise<darknet_ros_msgs::BoundingBoxes>(boundingBoxesTopicName, boundingBoxesQueueSize, boundingBoxesLatch);
  detectionImagePublisher_ =
      nodeHandle_.advertise<sensor_msgs::Image>(detectionImageTopicName, detectionImageQueueSize, detectionImageLatch);
  depthTaggedDetectionImagePublisher_ = 
      nodeHandle_.advertise<sensor_msgs::Image>(detectionDepthImageTopicName, detectionDepthImageQueueSize, detectionDepthImageLatch);
  sceneDepthPublisher_ = 
      nodeHandle_.advertise<darknet_ros_msgs::FrameDepth>(sceneDepthTopicName, sceneDepthQueueSize, sceneDepthLatch);


  // Action servers.
  std::string checkForObjectsActionName;
  nodeHandle_.param("actions/camera_reading/topic", checkForObjectsActionName, std::string("check_for_objects"));
  checkForObjectsActionServer_.reset(new CheckForObjectsActionServer(nodeHandle_, checkForObjectsActionName, false));
  checkForObjectsActionServer_->registerGoalCallback(boost::bind(&YoloObjectDetector::checkForObjectsActionGoalCB, this));
  checkForObjectsActionServer_->registerPreemptCallback(boost::bind(&YoloObjectDetector::checkForObjectsActionPreemptCB, this));
  checkForObjectsActionServer_->start();
}

void YoloObjectDetector::cameraCallback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::ImageConstPtr& msgdepth) 
{
  // ROS_INFO("[YoloObjectDetector] DARKNET --> Camera image received.");
  cv_bridge::CvImagePtr cam_image;
  cv_bridge::CvImageConstPtr cam_depth; 

  try
  {
    cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8); 
    cam_depth = cv_bridge::toCvCopy(msgdepth, sensor_msgs::image_encodings::TYPE_16UC1);
    imageHeader_ = msg->header; 
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  if (cam_image) {
    {
      boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
      imageHeader_ = msg->header;
      camImageCopy_ = cam_image->image.clone();
    }
    {
    boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
    imageStatus_ = true;
    }
    frameWidth_ = cam_image->image.size().width;
    frameHeight_ = cam_image->image.size().height;
  }

  if (cam_depth)
  {
    depthImageCopy_ = cam_depth->image.clone();
  }

  return;
}

void YoloObjectDetector::checkForObjectsActionGoalCB() {
  ROS_DEBUG("[YoloObjectDetector] Start check for objects action.");

  boost::shared_ptr<const darknet_ros_msgs::CheckForObjectsGoal> imageActionPtr = checkForObjectsActionServer_->acceptNewGoal();
  sensor_msgs::Image imageAction = imageActionPtr->image;

  cv_bridge::CvImagePtr cam_image;

  try {
    cam_image = cv_bridge::toCvCopy(imageAction, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  if (cam_image) {
    {
      boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
      camImageCopy_ = cam_image->image.clone();
    }
    {
      boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexActionStatus_);
      actionId_ = imageActionPtr->id;
    }
    {
      boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
      imageStatus_ = true;
    }
    frameWidth_ = cam_image->image.size().width;
    frameHeight_ = cam_image->image.size().height;
  }
  return;
}

void YoloObjectDetector::checkForObjectsActionPreemptCB() {
  ROS_DEBUG("[YoloObjectDetector] Preempt check for objects action.");
  checkForObjectsActionServer_->setPreempted();
}

bool YoloObjectDetector::isCheckingForObjects() const {
  return (ros::ok() && checkForObjectsActionServer_->isActive() && !checkForObjectsActionServer_->isPreemptRequested());
}

bool YoloObjectDetector::publishDetectionImage(const cv::Mat& detectionImage) {
  if (detectionImagePublisher_.getNumSubscribers() < 1) return false;
  cv_bridge::CvImage cvImage;
  cvImage.header.stamp = ros::Time::now();
  cvImage.header.frame_id = "detection_image";
  cvImage.encoding = sensor_msgs::image_encodings::RGB8;
  cvImage.image = detectionImage;
  detectionImagePublisher_.publish(*cvImage.toImageMsg());
  ROS_DEBUG("Detection image has been published.");
  return true;
}

int YoloObjectDetector::sizeNetwork(network* net) {
  int i;
  int count = 0;
  for (i = 0; i < net->n; ++i) {
    layer l = net->layers[i];
    if (l.type == YOLO || l.type == REGION || l.type == DETECTION) {
      count += l.outputs;
    }
  }
  return count;
}

void YoloObjectDetector::rememberNetwork(network* net) {
  int i;
  int count = 0;
  for (i = 0; i < net->n; ++i) {
    layer l = net->layers[i];
    if (l.type == YOLO || l.type == REGION || l.type == DETECTION) {
      memcpy(predictions_[demoIndex_] + count, net->layers[i].output, sizeof(float) * l.outputs);
      count += l.outputs;
    }
  }
}

detection* YoloObjectDetector::avgPredictions(network* net, int* nboxes) {
  int i, j;
  int count = 0;
  fill_cpu(demoTotal_, 0, avg_, 1);
  for (j = 0; j < demoFrame_; ++j) {
    axpy_cpu(demoTotal_, 1. / demoFrame_, predictions_[j], 1, avg_, 1);
  }
  for (i = 0; i < net->n; ++i) {
    layer l = net->layers[i];
    if (l.type == YOLO || l.type == REGION || l.type == DETECTION) {
      memcpy(l.output, avg_ + count, sizeof(float) * l.outputs);
      count += l.outputs;
    }
  }
  detection* dets = get_network_boxes(net, buff_[0].w, buff_[0].h, demoThresh_, demoHier_, 0, 1, nboxes);
  return dets;
}

void* YoloObjectDetector::detectInThread() {
  running_ = 1;
  float nms = .4;

  layer l = net_->layers[net_->n - 1];
  float* X = buffLetter_[(buffIndex_ + 2) % 3].data;
  float* prediction = network_predict(net_, X);

  rememberNetwork(net_);
  detection* dets = 0;
  int nboxes = 0;
  dets = avgPredictions(net_, &nboxes);

  if (nms > 0) do_nms_obj(dets, nboxes, l.classes, nms);

  if (enableConsoleOutput_) {
    printf("\033[2J");
    printf("\033[1;1H");
    printf("\nFPS:%.1f\n", fps_);
    printf("Objects:\n\n");
  }
  image display = buff_[(buffIndex_ + 2) % 3];
  draw_detections(display, dets, nboxes, demoThresh_, demoNames_, demoAlphabet_, demoClasses_);

  // extract the bounding boxes and send them to ROS
  int i, j;
  int count = 0;
  for (i = 0; i < nboxes; ++i) {
    float xmin = dets[i].bbox.x - dets[i].bbox.w / 2.;
    float xmax = dets[i].bbox.x + dets[i].bbox.w / 2.;
    float ymin = dets[i].bbox.y - dets[i].bbox.h / 2.;
    float ymax = dets[i].bbox.y + dets[i].bbox.h / 2.;

    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax > 1) xmax = 1;
    if (ymax > 1) ymax = 1;

    // iterate through possible boxes and collect the bounding boxes
    for (j = 0; j < demoClasses_; ++j) {
      if (dets[i].prob[j]) {
        float x_center = (xmin + xmax) / 2;
        float y_center = (ymin + ymax) / 2;
        float BoundingBox_width = xmax - xmin;
        float BoundingBox_height = ymax - ymin;

        // define bounding box
        // BoundingBox must be 1% size of frame (3.2x2.4 pixels)
        if (BoundingBox_width > 0.01 && BoundingBox_height > 0.01) {
          roiBoxes_[count].x = x_center;
          roiBoxes_[count].y = y_center;
          roiBoxes_[count].w = BoundingBox_width;
          roiBoxes_[count].h = BoundingBox_height;
          roiBoxes_[count].Class = j;
          roiBoxes_[count].prob = dets[i].prob[j];
          count++;
        }
      }
    }
  }

  // create array to store found bounding boxes
  // if no object detected, make sure that ROS knows that num = 0
  if (count == 0) {
    roiBoxes_[0].num = 0;
  } else {
    roiBoxes_[0].num = count;
  }

  free_detections(dets, nboxes);
  demoIndex_ = (demoIndex_ + 1) % demoFrame_;
  running_ = 0;
  return 0;
}

void* YoloObjectDetector::fetchInThread() {
  {
    boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
    CvMatWithHeader_ imageAndHeader = getCvMatWithHeader();
    free_image(buff_[buffIndex_]);
    buff_[buffIndex_] = mat_to_image(imageAndHeader.image);
    headerBuff_[buffIndex_] = imageAndHeader.header;
    buffId_[buffIndex_] = actionId_;
  }
  rgbgr_image(buff_[buffIndex_]);
  letterbox_image_into(buff_[buffIndex_], net_->w, net_->h, buffLetter_[buffIndex_]);
  return 0;
}

void* YoloObjectDetector::displayInThread(void* ptr) {
  int c = show_image(buff_[(buffIndex_ + 1) % 3], "YOLO", 1);
  if (c != -1) c = c % 256;
  if (c == 27) {
    demoDone_ = 1;
    return 0;
  } else if (c == 82) {
    demoThresh_ += .02;
  } else if (c == 84) {
    demoThresh_ -= .02;
    if (demoThresh_ <= .02) demoThresh_ = .02;
  } else if (c == 83) {
    demoHier_ += .02;
  } else if (c == 81) {
    demoHier_ -= .02;
    if (demoHier_ <= .0) demoHier_ = .0;
  }
  return 0;
}

void* YoloObjectDetector::displayLoop(void* ptr) {
  while (1) {
    displayInThread(0);
  }
}

void* YoloObjectDetector::detectLoop(void* ptr) {
  while (1) {
    detectInThread();
  }
}

void YoloObjectDetector::setupNetwork(char* cfgfile, char* weightfile, char* datafile, float thresh, char** names, int classes, int delay,
                                      char* prefix, int avg_frames, float hier, int w, int h, int frames, int fullscreen) {
  demoPrefix_ = prefix;
  demoDelay_ = delay;
  demoFrame_ = avg_frames;
  image** alphabet = load_alphabet_with_file(datafile);
  demoNames_ = names;
  demoAlphabet_ = alphabet;
  demoClasses_ = classes;
  demoThresh_ = thresh;
  demoHier_ = hier;
  fullScreen_ = fullscreen;
  printf("YOLO\n");
  net_ = load_network(cfgfile, weightfile, 0);
  set_batch_network(net_, 1);
}

void YoloObjectDetector::yolo() {
  const auto wait_duration = std::chrono::milliseconds(2000);
  while (!getImageStatus()) {
    printf("Waiting for image.\n");
    if (!isNodeRunning()) {
      return;
    }
    std::this_thread::sleep_for(wait_duration);
  }

  std::thread detect_thread;
  std::thread fetch_thread;

  srand(2222222);

  int i;
  demoTotal_ = sizeNetwork(net_);
  predictions_ = (float**)calloc(demoFrame_, sizeof(float*));
  for (i = 0; i < demoFrame_; ++i) {
    predictions_[i] = (float*)calloc(demoTotal_, sizeof(float));
  }
  avg_ = (float*)calloc(demoTotal_, sizeof(float));

  layer l = net_->layers[net_->n - 1];
  roiBoxes_ = (darknet_ros::RosBox_*)calloc(l.w * l.h * l.n, sizeof(darknet_ros::RosBox_));

  {
    boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
    CvMatWithHeader_ imageAndHeader = getCvMatWithHeader();
    buff_[0] = mat_to_image(imageAndHeader.image);
    headerBuff_[0] = imageAndHeader.header;
  }
  buff_[1] = copy_image(buff_[0]);
  buff_[2] = copy_image(buff_[0]);
  headerBuff_[1] = headerBuff_[0];
  headerBuff_[2] = headerBuff_[0];
  buffLetter_[0] = letterbox_image(buff_[0], net_->w, net_->h);
  buffLetter_[1] = letterbox_image(buff_[0], net_->w, net_->h);
  buffLetter_[2] = letterbox_image(buff_[0], net_->w, net_->h);
  disp_ = image_to_mat(buff_[0]);

  int count = 0;
  if (!demoPrefix_ && viewImage_) {
    cv::namedWindow("YOLO", cv::WINDOW_NORMAL);
    if (fullScreen_) {
      cv::setWindowProperty("YOLO", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    } else {
      cv::moveWindow("YOLO", 0, 0);
      cv::resizeWindow("YOLO", 640, 480);
    }
  }

  demoTime_ = what_time_is_it_now();

  while (!demoDone_) {
    buffIndex_ = (buffIndex_ + 1) % 3;
    fetch_thread = std::thread(&YoloObjectDetector::fetchInThread, this);
    detect_thread = std::thread(&YoloObjectDetector::detectInThread, this);
    if (!demoPrefix_) {
      fps_ = 1. / (what_time_is_it_now() - demoTime_);
      demoTime_ = what_time_is_it_now();
      if (viewImage_) {
        displayInThread(0);
      } else {
        generate_image(buff_[(buffIndex_ + 1) % 3], disp_);
      }
      publishInThread();
    } else {
      char name[256];
      sprintf(name, "%s_%08d", demoPrefix_, count);
      save_image(buff_[(buffIndex_ + 1) % 3], name);
    }
    fetch_thread.join();
    detect_thread.join();
    ++count;
    if (!isNodeRunning()) {
      demoDone_ = true;
    }
  }
}

CvMatWithHeader_ YoloObjectDetector::getCvMatWithHeader() {
  CvMatWithHeader_ header = {.image = camImageCopy_, .header = imageHeader_};
  return header;
}

bool YoloObjectDetector::getImageStatus(void) {
  boost::shared_lock<boost::shared_mutex> lock(mutexImageStatus_);
  return imageStatus_;
}

bool YoloObjectDetector::isNodeRunning(void) {
  boost::shared_lock<boost::shared_mutex> lock(mutexNodeStatus_);
  return isNodeRunning_;
}

void* YoloObjectDetector::publishInThread() {
  // Publish image.
  cv::Mat cvImage = disp_;
  if (!publishDetectionImage(cv::Mat(cvImage))) {
    ROS_DEBUG("Detection image has not been broadcasted.");
  }

  // Publish bounding boxes and detection result.
  int num = roiBoxes_[0].num;
  if (num > 0 && num <= 100) {
    for (int i = 0; i < num; i++) {
      for (int j = 0; j < numClasses_; j++) {
        if (roiBoxes_[i].Class == j) {
          rosBoxes_[j].push_back(roiBoxes_[i]);
          rosBoxCounter_[j]++;
        }
      }
    }

    darknet_ros_msgs::ObjectCount msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = "detection";
    msg.count = num;
    objectPublisher_.publish(msg);




    for (int i = 0; i < numClasses_; i++) {
      if (rosBoxCounter_[i] > 0) {
        darknet_ros_msgs::BoundingBox boundingBox;
        darknet_ros_msgs::ObjDepth objDepthMsg;   //can use pointers here to reduce multiple objects

        for (int j = 0; j < rosBoxCounter_[i]; j++) 
        {
          int xmin = (rosBoxes_[i][j].x - rosBoxes_[i][j].w / 2) * frameWidth_;
          int ymin = (rosBoxes_[i][j].y - rosBoxes_[i][j].h / 2) * frameHeight_;
          int xmax = (rosBoxes_[i][j].x + rosBoxes_[i][j].w / 2) * frameWidth_;
          int ymax = (rosBoxes_[i][j].y + rosBoxes_[i][j].h / 2) * frameHeight_;

          boundingBox.Class = classLabels_[i];
          boundingBox.id = i;
          boundingBox.probability = rosBoxes_[i][j].prob;
          boundingBox.xmin = xmin;
          boundingBox.ymin = ymin;
          boundingBox.xmax = xmax;
          boundingBox.ymax = ymax;
          boundingBoxesResults_.bounding_boxes.push_back(boundingBox);

          //For depth inclusion
          objDepthMsg = associateDepth(boundingBox, objDepthMsg);
          DepthMsg_.objDepths.push_back(objDepthMsg);
        }
      }
    }
    boundingBoxesResults_.header.stamp = ros::Time::now();
    boundingBoxesResults_.header.frame_id = "detection";
    boundingBoxesResults_.image_header = headerBuff_[(buffIndex_ + 1) % 3];
    boundingBoxesPublisher_.publish(boundingBoxesResults_);

    //DepthFrame Message Wrapper 
    DepthMsg_.header.stamp = ros::Time::now(); 
    DepthMsg_.header.frame_id = depth_frame_;
    DepthMsg_.objCount = msg.count;
    sceneDepthPublisher_.publish(DepthMsg_);

    //publish Depth Detection Image
    if (!publishDepthTaggedDetectionImage(cv::Mat(cvImage), darknet_ros_msgs::FrameDepth(DepthMsg_))) {
      ROS_DEBUG("Depth Tagged Detection image has not been broadcasted.");
    }
  
    DepthMsg_.objDepths.clear();
  } else {
    darknet_ros_msgs::ObjectCount msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = "detection";
    msg.count = 0;
    objectPublisher_.publish(msg);
  }
  if (isCheckingForObjects()) {
    ROS_DEBUG("[YoloObjectDetector] check for objects in image.");
    darknet_ros_msgs::CheckForObjectsResult objectsActionResult;
    objectsActionResult.id = buffId_[0];
    objectsActionResult.bounding_boxes = boundingBoxesResults_;
    checkForObjectsActionServer_->setSucceeded(objectsActionResult, "Send bounding boxes.");
  }
  boundingBoxesResults_.bounding_boxes.clear();
  for (int i = 0; i < numClasses_; i++) {
    rosBoxes_[i].clear();
    rosBoxCounter_[i] = 0;
  }

  return 0;
}

darknet_ros_msgs::ObjDepth YoloObjectDetector::associateDepth(const darknet_ros_msgs::BoundingBox& bbox, darknet_ros_msgs::ObjDepth ObjDepthMsg)
{
  /*
  Depth image ROS REP : https://www.ros.org/reps/rep-0118.html

  Formula for calculating X,Y in camera frame (Z in front (Depth), X is up, y is right (toward USB-C))
  Xreal = (u - cx) * Z / fx;
  Yreal = (v - cy) * Z / fy; 
  Zreal = Z

  u, v   = Desired pixel values
  cx, cy = Intrinsic camera parameter (Principal points)
  fx, fy = Intrinsic camera parameter (Focal lengths)
  Z      = Depth of (u, v) from camera

  */

  try
  {

    int u = static_cast<int>((bbox.xmin+bbox.xmax)/2); 
    int v = static_cast<int>((bbox.ymin+bbox.ymax)/2);
    // float Z =static_cast<float>(0.001*depthImageCopy_.at<u_int16_t>(v, u));  //FOR 16UC1 (values in m)'
    float Z = 0.001*depthImageCopy_.at<u_int16_t>(v, u);

    //class name, type
    ObjDepthMsg.objID = bbox.id;
    ObjDepthMsg.className = bbox.Class;
    ObjDepthMsg.classType = "To be decided";
    ObjDepthMsg.objDepth = round(Z*1000.0)/1000.0 ; 
    ObjDepthMsg.objX = round((u - intrin_cx_) * Z * 1000.0 / intrin_fx_ ) / 1000.0; 
    ObjDepthMsg.objY = round((v - intrin_cy_) * Z * 1000.0 / intrin_fy_ ) / 1000.0;
    ObjDepthMsg.bbox_center_u = u; 
    ObjDepthMsg.bbox_center_v = v;

  }
  catch(...) 
  {
    ROS_ERROR("Some Error occurred!");
  }

  return ObjDepthMsg;

}

void YoloObjectDetector::cameraDepthInfoCallback(const sensor_msgs::CameraInfoPtr& depthInfoMsg)
{
  if (depthInfoMsg->distortion_model == "plumb_bob") //RS has a plumb_bob model 
  {
    depth_frame_ = depthInfoMsg->header.frame_id;
    intrin_fx_   = depthInfoMsg->K[0];
    intrin_fy_   = depthInfoMsg->K[4];
    intrin_cx_   = depthInfoMsg->K[2];
    intrin_cy_   = depthInfoMsg->K[5];
  }
}

bool YoloObjectDetector::publishDepthTaggedDetectionImage(const cv::Mat& incomingImage,const darknet_ros_msgs::FrameDepth& frameDepthMsg)
{
  // std::cout << "attempt to publish Depth tagged detections" << std::endl;
  // if (YoloObjectDetector::depthTaggedDetectionImagePublisher_.getNumSubscribers() < 1) return false;
  //create CV image
  cv_bridge::CvImage cvImage; 
  cvImage.header.stamp = ros::Time::now();
  cvImage.header.frame_id = "depth_tagged_detection_image";
  cvImage.encoding = sensor_msgs::image_encodings::RGB8; 
  cvImage.image = incomingImage;
  //draw here 
  if (frameDepthMsg.objCount > 0)
  {
    for (auto objDepth : frameDepthMsg.objDepths){
      std::string disp_string = "X:" + std::to_string(objDepth.objX) 
                              + "Y:" + std::to_string(objDepth.objY)
                              + "Z:" + std::to_string(objDepth.objDepth);
      cv::Point text_pos(objDepth.bbox_center_u, objDepth.bbox_center_v);
      int font_size = 1; 
      cv::Scalar font_color(0,0,0); 
      int font_weight = 2; 
      cv::putText(incomingImage, disp_string, text_pos, cv::FONT_HERSHEY_SIMPLEX, 0.5, font_color, 2);
    }
  }
  depthTaggedDetectionImagePublisher_.publish(*cvImage.toImageMsg());
  ROS_DEBUG("Depth tagged detection image has been published.");
  return true;
}

} /* namespace darknet_ros*/
