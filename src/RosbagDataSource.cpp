/**
 * @file   RosbagDataSource.cpp
 * @brief  Parse rosbad and run spark vio 
 * @author Yun Chang
 */
#include "RosDataSource.h"

namespace VIO {

RosDataProvider::RosDataProvider(std::string left_camera_topic, 
																 std::string right_camera_topic, 
																 std::string imu_topic): 
			stereo_calib_(), 
			DataProvider(), 
			it_(nh_cam_),
			left_img_subscriber_(it_, left_camera_topic, 1), // Image subscriber (left)
			right_img_subscriber_(it_, right_camera_topic, 1), // Image subscriber (right)
			sync(sync_pol(10), left_img_subscriber_, right_img_subscriber_)
	{

	ROS_INFO(">>>>>>> Initializing Spark-VIO <<<<<<<");
	// Parse calibration info for camera and IMU 
	// Calibration info on parameter server (Parsed from yaml)
	parseCameraData(&stereo_calib_);
	parseImuData(&imuData_, &imuParams_);

	// print parameters for check 
	print();

	// Set frame count to 0 (Keeping track of number of frames processed)
	frame_count_ = 0;  

	// Initialize last timestamp to be 0 
	last_time_stamp_ = 0;

	////// Define IMU Subscriber

	imu_topic_ = imu_topic; 

	// Start IMU subscriber

	imu_subscriber_ = nh_imu_.subscribe(imu_topic, 10, &RosDataProvider::callbackIMU, this); 

	// Define Callback Queue for IMU Data
	ros::CallbackQueue imu_queue; 
	nh_imu_.setCallbackQueue(&imu_queue);
	
	// Spawn Async Spinner (Running on Custom Queue) for IMU
	// 0 to use number of processor queues 

	ros::AsyncSpinner async_spinner_imu(0, &imu_queue);
	async_spinner_imu.start();

	////// Synchronize stero image callback 

	sync.registerCallback(boost::bind(&RosDataProvider::callbackCamAndProcessStereo, this, _1, _2) );

	// Define Callback Queue for Cam Data
  ros::CallbackQueue cam_queue;
  nh_cam_.setCallbackQueue(&cam_queue);

  // Spawn Async Spinner (Running on Custom Queue) for Cam
  ros::AsyncSpinner async_spinner_cam(0, &cam_queue);
	async_spinner_cam.start();

	// start odometry publisher 
	std::string odom_topic_name; 
	nh_.getParam("odometry_topic_name", odom_topic_name);
	odom_publisher = nh_.advertise<nav_msgs::Odometry>(odom_topic_name, 10);

  ROS_INFO(">>>>>>> Started data subscribers <<<<<<<<");
}

RosDataProvider::~RosDataProvider() {}

cv::Mat RosDataProvider::readRosImage(const sensor_msgs::ImageConstPtr& img_msg) {
	// Use cv_bridge to read ros image to cv::Mat
	cv_bridge::CvImagePtr cv_ptr;
  try{
    cv_ptr = cv_bridge::toCvCopy(img_msg);
  } catch(cv_bridge::Exception& exception) {
    ROS_FATAL("cv_bridge exception: %s", exception.what());
    ros::shutdown();
  }
  return cv_ptr->image; // Return cv::Mat
}

bool RosDataProvider::parseCameraData(StereoCalibration* stereo_calib) {
	// Parse camera calibration info (from param server)

 	// Rate 
 	double rate; 
 	nh_.getParam("camera_rate_hz", rate);

  // Resoltuion 
  std::vector<int> resolution; 
  nh_.getParam("camera_resolution", resolution);

  // Get distortion/intrinsics/extrinsics for each camera 
  for (int i = 0; i < 2; i++){
  	std::string camera_name;
  	CameraParams camera_param_i; 
  	// Fill in rate and resolution 
  	camera_param_i.image_size_ = cv::Size(resolution[0], resolution[1]);
  	camera_param_i.frame_rate_ = 1.0 / rate; // Terminology wrong but following rest of the repo

  	if (i == 0) {
  		camera_name = "left_camera_";
  	} else { 
  		camera_name = "right_camera_";
  	}
  	// Parse intrinsics (camera matrix)
  	std::vector<double> intrinsics; 
  	nh_.getParam(camera_name + "intrinsics", intrinsics);
  	camera_param_i.intrinsics_ = intrinsics; 
  	// Conver intrinsics to camera matrix (OpenCV format)
    camera_param_i.camera_matrix_ = cv::Mat::eye(3, 3, CV_64F);
    camera_param_i.camera_matrix_.at<double>(0, 0) = intrinsics[0];
    camera_param_i.camera_matrix_.at<double>(1, 1) = intrinsics[1];
    camera_param_i.camera_matrix_.at<double>(0, 2) = intrinsics[2];
    camera_param_i.camera_matrix_.at<double>(1, 2) = intrinsics[3];

  	// Parse extrinsics (rotation and translation)
  	std::vector<double> extrinsics; 
  	std::vector<double> frame_change; // encode calibration frame to body frame
  	nh_.getParam(camera_name + "extrinsics", extrinsics);
  	nh_.getParam("calibration_to_body_frame", frame_change);
  	// Place into matrix 
  	// 4 4 is hardcoded here because currently only accept extrinsic input 
  	// in homoegeneous format [R T ; 0 1]
  	cv::Mat E_calib = cv::Mat::zeros(4, 4, CV_64F);
  	cv::Mat calib2body = cv::Mat::zeros(4, 4, CV_64F);
  	for (int k = 0; k < 16; k++) {
  		int row = k / 4; 
  		int col = k % 4; 
  		E_calib.at<double>(row, col) = extrinsics[k]; 
  		calib2body.at<double>(row, col) = frame_change[k];
  	}

  	cv::Mat E_body = calib2body * E_calib; // Extrinsics in body frame 
  	
  	// restore back to vector form 
  	std::vector<double> extrinsics_body; 
  	for (int k = 0; k < 16; k++) {
  		int row = k / 4; 
  		int col = k % 4; 
  		extrinsics_body.push_back(E_body.at<double>(row, col));
  	}

  	camera_param_i.body_Pose_cam_ = UtilsOpenCV::Vec2pose(extrinsics_body, 4, 4);

  	// Parse distortion 
  	std::vector<double> d_coeff; 
  	nh_.getParam(camera_name + "distortion_coefficients", d_coeff);
  	cv::Mat distortion_coeff = cv::Mat::zeros(1, 5, CV_64F);

  	switch (d_coeff.size()) { // Currently have only come across two cases 
  		case(4): // if given 4 coefficients 
  			distortion_coeff.at<double>(0,0) = d_coeff[0]; // k1
  			distortion_coeff.at<double>(0,1) = d_coeff[1]; // k2
  			distortion_coeff.at<double>(0,3) = d_coeff[2]; // p1
  			distortion_coeff.at<double>(0,4) = d_coeff[3]; // p2
  			break; 

  		case(5): // if given 5 coefficients 
  			for (int k = 0; k < 5; k++) {
    			distortion_coeff.at<double>(0, k) = d_coeff[k]; // k1, k2, k3, p1, p2
  			}
  			break; 

  		default: // otherwise 
  			ROS_FATAL("Unsupported distortion format");
  	}

  	camera_param_i.distortion_coeff_ = distortion_coeff;

  	// TODO add skew (can add switch statement when parsing intrinsics)
  	camera_param_i.calibration_ = gtsam::Cal3DS2(intrinsics[0], // fx
      																					 intrinsics[1], // fy
      																					 0.0,           // skew
      																					 intrinsics[2], // u0
      																					 intrinsics[3], // v0
      																					 distortion_coeff.at<double>(0,0),  //  k1
      																					 distortion_coeff.at<double>(0,1),  //  k2
      																					 distortion_coeff.at<double>(0,3),  //  p1
      																					 distortion_coeff.at<double>(0,4)); //  p2

  	if (i == 0){
  		stereo_calib->left_camera_info_ = camera_param_i;
  	} else { 
  		stereo_calib->right_camera_info_ = camera_param_i;
  	}

  }

  // Calculate the pose of right camera relative to the left camera
  stereo_calib->camL_Pose_camR_ = (stereo_calib->left_camera_info_.body_Pose_cam_).between(
                      						stereo_calib->right_camera_info_.body_Pose_cam_);

  ROS_INFO("Parsed stereo camera calibration");
  return true;
}

bool RosDataProvider::parseImuData(ImuData* imudata, ImuParams* imuparams) {
	// Parse IMU calibration info (from param server)
	double rate, rate_std, rate_maxMismatch, gyro_noise, gyro_walk, acc_noise, acc_walk; 

	std::vector<double> extrinsics; 

	nh_.getParam("imu_rate_hz", rate);
	nh_.getParam("gyroscope_noise_density", gyro_noise); 
	nh_.getParam("gyroscope_random_walk", gyro_walk);
	nh_.getParam("accelerometer_noise_density", acc_noise);
	nh_.getParam("accelerometer_random_walk", acc_walk);
	nh_.getParam("imu_extrinsics", extrinsics);

	imudata->nominal_imu_rate_ = 1.0 / rate;
	imudata->imu_rate_ = 1.0 / rate;
	imudata->imu_rate_std_ = 0.00500009; // set to 0 for now
	imudata->imu_rate_maxMismatch_ = 0.00500019; // set to 0 for now 
	imuparams->gyro_noise_ = gyro_noise; 
	imuparams->gyro_walk_ = gyro_walk; 
	imuparams->acc_noise_ = acc_noise;
	imuparams->acc_walk_ = acc_walk; 

	// Expects imu frame to be aligned with body frame 

  ROS_INFO("Parsed IMU calibration");
	return true; 
}

// IMU callback 
void RosDataProvider::callbackIMU(const sensor_msgs::ImuConstPtr& msgIMU){
	// Callback and store IMU data and timestamp until next StereoImuSyncPacket is made
  gtsam::Vector6 gyroAccData; 
  gtsam::Vector6 imu_accgyr;

  imu_accgyr(0) = msgIMU->linear_acceleration.x; 
  imu_accgyr(1) = msgIMU->linear_acceleration.y; 
  imu_accgyr(2) = msgIMU->linear_acceleration.z; 
  imu_accgyr(3) = msgIMU->angular_velocity.x; 
  imu_accgyr(4) = msgIMU->angular_velocity.y;
  imu_accgyr(5) = msgIMU->angular_velocity.z; 

  long double sec = (long double) msgIMU->header.stamp.sec; 
  long double nsec = (long double) msgIMU->header.stamp.nsec;
  Timestamp timestamp = (long int) (sec * 1e9 + nsec);
  // ROS_INFO("Recieved message at time %ld", timestamp);

  if (last_time_stamp_ == 0) { // initialize first time stamp
  	last_time_stamp_ = timestamp; 
  }

  // add measurement to buffer (CHECK if this is OK, need to manually delete old data?)
  imuData_.imu_buffer_.addMeasurement(timestamp, imu_accgyr);
}

// Callback for stereo images and main spin 
void RosDataProvider::callbackCamAndProcessStereo(const sensor_msgs::ImageConstPtr& msgLeft,
                                 const sensor_msgs::ImageConstPtr& msgRight){

	// store in stereo buffer 
  stereo_buffer_.add_stereo_frame(msgLeft, msgRight);

}

bool RosDataProvider::spin() {
	// Define pipeline
  // Dummy ETH data (required for now get rid later)
  ETHDatasetParser eth_dataset_parser;
	Pipeline vio_pipeline_(&eth_dataset_parser, imuParams_); // initialize vio pipeline

	// Register callback to vio_pipeline.
  registerVioCallback(
        std::bind(&Pipeline::spin, &vio_pipeline_, std::placeholders::_1)); 

	ros::Rate rate(60);
	while (ros::ok()){
		// Main spin of the data provider: Interpolates IMU data and build StereoImuSyncPacket
		// (Think of this as the spin of the other parser/data-providers)

		Timestamp timestamp = stereo_buffer_.get_earliest_timestamp(); 

		if (stereo_buffer_.get_earliest_timestamp() <= last_time_stamp_) {
			if (stereo_buffer_.size() != 0) {
				ROS_WARN("Next frame in image buffer is from the same or earlier time than the last processed frame. Skip frame.");
				// remove next frame (this would usually for the first frame)
				stereo_buffer_.remove_next();
			}
			// else just waiting for next stereo frames

		} else {
			// Test if IMU data available
			ImuMeasurements imu_meas; 

			utils::ThreadsafeImuBuffer::QueryResult imu_query = 
									imuData_.imu_buffer_.getImuDataInterpolatedUpperBorder(
																			last_time_stamp_,
																			timestamp, 
																			&imu_meas.timestamps_, 
																			&imu_meas.measurements_);

			if (imu_query == utils::ThreadsafeImuBuffer::QueryResult::kDataAvailable) {
				// data available
				sensor_msgs::ImageConstPtr left_ros_img, right_ros_img; 
				stereo_buffer_.extract_latest_images(left_ros_img, right_ros_img);

				// read to cv type 
				cv::Mat left_image = readRosImage(left_ros_img);
				cv::Mat right_image = readRosImage(right_ros_img);

				// Call VIO Pipeline.
			  // ROS_INFO("Processing frame %d with timestamp: %ld", frame_count_, timestamp);

			  // Stereo matching parameters
			  const StereoMatchingParams& stereo_matching_params = frontend_params_.getStereoMatchingParams();

			  vio_callback_(StereoImuSyncPacket(
					                StereoFrame(frame_count_, 
					               	timestamp,
					                left_image,
					                stereo_calib_.left_camera_info_,
					                right_image,
					                stereo_calib_.right_camera_info_,
					                stereo_calib_.camL_Pose_camR_,
					                stereo_matching_params),
					                imu_meas.timestamps_,
					                imu_meas.measurements_));

			  // ROS_INFO("StereoImuSyncPacket %d sent", frame_count_);

			  last_time_stamp_ = timestamp;
			  frame_count_++; 

			  // publish pipeline output 
			  gtsam::Pose3 estimated_pose = vio_pipeline_.get_estimated_pose();
			  gtsam::Vector3 estimated_velocity = vio_pipeline_.get_estimated_velocity();
			  publishOutput(estimated_pose, estimated_velocity, timestamp); 

			} else if (imu_query == utils::ThreadsafeImuBuffer::QueryResult::kTooFewMeasurementsAvailable) {
				ROS_WARN("Too few IMU measurements between next frame and last frame. Skip frame.");
				// remove next frame (this would usually for the first frame)
				stereo_buffer_.remove_next();
			}

			// else it would be the kNotYetAvailable then just wait for next loop
		}

	// spin loop
	ros::spinOnce();
	rate.sleep();

	}

	// Dataset spin has finished, shutdown VIO.
  vio_pipeline_.shutdown();
  return true; 
}

void RosDataProvider::publishOutput(gtsam::Pose3 pose, gtsam::Vector3 velocity, Timestamp ts) const {
	// publish 
	// First publish odometry estimate 
	nav_msgs::Odometry odometry_msg; 

	long int sec = ts / 1e9; 
	long int nsec = ts - sec * 1e9; 

	// create header 
	odometry_msg.header.stamp.sec = sec;
	odometry_msg.header.stamp.nsec = nsec; 
	odometry_msg.header.frame_id = "/base_link";

	// position 
	odometry_msg.pose.pose.position.x = pose.x();
	odometry_msg.pose.pose.position.y = pose.y();
	odometry_msg.pose.pose.position.z = pose.z();

	// orientation
	odometry_msg.pose.pose.orientation.w = pose.rotation().toQuaternion().w();
	odometry_msg.pose.pose.orientation.x = pose.rotation().toQuaternion().x();
	odometry_msg.pose.pose.orientation.y = pose.rotation().toQuaternion().y();
	odometry_msg.pose.pose.orientation.z = pose.rotation().toQuaternion().z();

	// linear velocity 
	odometry_msg.twist.twist.linear.x = velocity(0); 
	odometry_msg.twist.twist.linear.y = velocity(1);
	odometry_msg.twist.twist.linear.z = velocity(2);

  odom_publisher.publish(odometry_msg);
}

void RosDataProvider::print() const {
	std::cout << ">>>>>>>>> RosDataProvider::print <<<<<<<<<<<" << std::endl;
  stereo_calib_.camL_Pose_camR_.print("camL_Pose_calR \n");
  // For each of the 2 cameras.
  std::cout << ">> Left camera params <<" << std::endl; 
  stereo_calib_.left_camera_info_.print(); 
  std::cout << ">> Right camera params <<" << std::endl; 
  stereo_calib_.right_camera_info_.print(); 
  std::cout << ">> IMU info << " << std::endl; 
  imuData_.print(); 
  std::cout << std::endl; 
  std::cout << "========================================" << std::endl; 
}

} // End of VIO namespace
