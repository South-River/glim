#include <glim/frontend/odometry_estimation_ct.hpp>

#include <future>

#include <gtsam/inference/Symbol.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/slam/BetweenFactor.h>

#include <gtsam_ext/ann/kdtree.hpp>
#include <gtsam_ext/types/frame_cpu.hpp>
#include <gtsam_ext/factors/integrated_ct_gicp_factor.hpp>
#include <gtsam_ext/optimizers/levenberg_marquardt_ext.hpp>

#include <glim/util/easy_profiler.hpp>
#include <glim/util/config.hpp>
#include <glim/util/console_colors.hpp>
#include <glim/frontend/callbacks.hpp>
#include <glim/common/cloud_covariance_estimation.hpp>

namespace glim {

using gtsam::symbol_shorthand::X;
using Callbacks = OdometryEstimationCallbacks;

/**
 * @brief Target map for scan-to-model matching
 */
struct OdometryEstimationCT::TargetMap {
  gtsam_ext::Frame::Ptr frame;
  std::shared_ptr<gtsam_ext::NearestNeighborSearch> kdtree;
};

OdometryEstimationCT::OdometryEstimationCT() {
  Config config(GlobalConfig::get_config_path("config_frontend_ct"));

  num_threads = config.param<int>("odometry_estimation_ct", "num_threads", 4);
  max_correspondence_distance = config.param<double>("odometry_estimation_ct", "max_correspondence_distance", 1.0);

  max_num_keyframes = config.param<int>("odometry_estimation_ct", "max_num_keyframes", 20);
  keyframe_update_interval_rot = config.param<double>("odometry_estimation_ct", "keyframe_update_interval_rot", 3.15);
  keyframe_update_interval_trans = config.param<double>("odometry_estimation_ct", "keyframe_update_interval_trans", 1.0);

  stiffness_scale_first = config.param<double>("odometry_estimation_ct", "stiffness_scale_first", 1e6);
  stiffness_scale_second = config.param<double>("odometry_estimation_ct", "stiffness_scale_second", 1e3);
  lm_max_iterations_first = config.param<int>("odometry_estimation_ct", "lm_max_iterations_first", 5);
  lm_max_iterations_second = config.param<int>("odometry_estimation_ct", "lm_max_iterations_second", 5);

  v_last_current_history.push_back(gtsam::Vector6::Zero());

  frame_count = 0;
  covariance_estimation.reset(new CloudCovarianceEstimation);
}

OdometryEstimationCT::~OdometryEstimationCT() {}

EstimationFrame::ConstPtr OdometryEstimationCT::insert_frame(const PreprocessedFrame::Ptr& raw_frame, std::vector<EstimationFrame::ConstPtr>& marginalized_frames) {
  Callbacks::on_insert_frame(raw_frame);

  // Create a frame to hold the current estimation state
  EstimationFrame::Ptr new_frame(new EstimationFrame);
  new_frame->id = frame_count++;
  new_frame->stamp = raw_frame->stamp;
  new_frame->T_lidar_imu.setIdentity();

  new_frame->v_world_imu.setZero();
  new_frame->imu_bias.setZero();

  new_frame->raw_frame = raw_frame;

  new_frame->frame_id = FrameID::LIDAR;
  gtsam_ext::FrameCPU::Ptr frame_cpu(new gtsam_ext::FrameCPU(raw_frame->points));
  frame_cpu->add_times(raw_frame->times);
  frame_cpu->add_covs(covariance_estimation->estimate(raw_frame->points, raw_frame->neighbors));
  new_frame->frame = frame_cpu;

  // Handling the first input frame
  if (keyframes.empty()) {
    new_frame->set_T_world_sensor(FrameID::LIDAR, Eigen::Isometry3d::Identity());
    Callbacks::on_new_frame(new_frame);

    last_frame = new_frame;
    keyframes.push_back(new_frame);
    marginalized_frames.push_back(new_frame);
    Callbacks::on_marginalized_frames(marginalized_frames);

    std::vector<EstimationFrame::ConstPtr> keyframe_clones = {keyframes.front()};
    target_map = create_target_map(keyframe_clones);

    return new_frame;
  }

  // Constant linear and angular velocity assumption
  gtsam::Vector6 predicted_v_last_current = gtsam::Vector6::Zero();
  for (const auto& v : v_last_current_history) {
    predicted_v_last_current += v;
  }
  predicted_v_last_current /= v_last_current_history.size();

  // Predict the current sensor poses at the scan beginning and end
  const double dt_last_current = raw_frame->stamp - last_frame->stamp;
  const double dt_scan = raw_frame->times.back() - raw_frame->times.front();
  const gtsam::Pose3 predicted_T_world_lidar_begin = gtsam::Pose3(last_frame->T_world_lidar.matrix()) * gtsam::Pose3::Expmap(dt_last_current * predicted_v_last_current);
  const gtsam::Pose3 predicted_T_world_lidar_end = predicted_T_world_lidar_begin * gtsam::Pose3::Expmap(dt_scan * predicted_v_last_current);

  gtsam::Values values;
  values.insert(X(0), gtsam::Pose3(predicted_T_world_lidar_begin));
  values.insert(X(1), gtsam::Pose3(predicted_T_world_lidar_begin));

  // Create CT-GICP factor
  gtsam::NonlinearFactorGraph graph;
  auto factor = gtsam::make_shared<gtsam_ext::IntegratedCT_GICPFactor>(X(0), X(1), target_map->frame, new_frame->frame, target_map->kdtree);
  factor->set_num_threads(num_threads);
  factor->set_max_corresponding_distance(max_correspondence_distance);
  graph.add(factor);

  // LM configuration
  gtsam_ext::LevenbergMarquardtExtParams lm_params;
  lm_params.setlambdaInitial(1e-10);
  lm_params.setAbsoluteErrorTol(1e-2);
  // lm_params.callback = [&](const gtsam_ext::LevenbergMarquardtOptimizationStatus& status, const gtsam::Values& values_) { std::cout << status.to_string() << std::endl; };

  try {
    // First optimization step with a large stiffness
    // graph.emplace_shared<gtsam::BetweenFactor<gtsam::Pose3>>(X(0), X(1), gtsam::Pose3::identity(), gtsam::noiseModel::Isotropic::Precision(6, stiffness_scale_first));
    lm_params.setMaxIterations(lm_max_iterations_first);
    values = gtsam_ext::LevenbergMarquardtOptimizerExt(graph, values, lm_params).optimize();

    // Second optimization step with a more elastic setting
    // graph.erase(graph.end() - 1);
    // graph.emplace_shared<gtsam::BetweenFactor<gtsam::Pose3>>(X(0), X(1), gtsam::Pose3::identity(), gtsam::noiseModel::Isotropic::Precision(6, stiffness_scale_second));
    lm_params.setMaxIterations(lm_max_iterations_second);
    values = gtsam_ext::LevenbergMarquardtOptimizerExt(graph, values, lm_params).optimize();
  } catch (std::exception& e) {
    std::cerr << console::bold_red << "error: an exception was caught during odometry estimation" << console::reset << std::endl;
    std::cerr << e.what() << std::endl;
  }

  // Calculate the linear and angular velocity and record them for the estimation of successive frames
  const gtsam::Pose3 T_last_current = values.at<gtsam::Pose3>(X(0)).inverse() * values.at<gtsam::Pose3>(X(1));
  const gtsam::Vector6 v_last_current = gtsam::Pose3::Logmap(T_last_current) / dt_last_current;
  v_last_current_history.push_back(v_last_current);
  if (v_last_current_history.size() > 3) {
    v_last_current_history.pop_front();
  }

  new_frame->set_T_world_sensor(FrameID::LIDAR, Eigen::Isometry3d(values.at<gtsam::Pose3>(X(0)).matrix()));

  // Deskew the input points and covs
  auto deskewed_points = factor->deskewed_source_points(values, true);
  auto deskewed_covs = covariance_estimation->estimate(deskewed_points, raw_frame->neighbors);
  for (int i = 0; i < deskewed_points.size(); i++) {
    new_frame->frame->points[i] = deskewed_points[i];
    new_frame->frame->covs[i] = deskewed_covs[i];
  }

  Callbacks::on_new_frame(new_frame);

  // Update the keyframe list
  const Eigen::Isometry3d delta_from_last_keyframe = keyframes.back()->T_world_lidar.inverse() * new_frame->T_world_lidar;
  const double delta_trans = delta_from_last_keyframe.translation().norm();
  const double delta_angle = Eigen::AngleAxisd(delta_from_last_keyframe.linear()).angle();

  std::vector<double> dists(raw_frame->size());
  std::transform(raw_frame->points.begin(), raw_frame->points.end(), dists.begin(), [](const Eigen::Vector4d& p) { return p.head<3>().norm(); });
  std::nth_element(dists.begin(), dists.begin() + dists.size() / 2, dists.end());
  const double interval_trans_p = std::min(dists[dists.size() / 2] / 20.0, 1.0);

  if (delta_trans > interval_trans_p * keyframe_update_interval_trans || delta_angle > keyframe_update_interval_rot) {
    keyframes.push_back(new_frame);

    // Create the target map in background
    if (!async_target.valid()) {
      std::vector<EstimationFrame::ConstPtr> keyframe_clones(keyframes.begin(), keyframes.end());
      Callbacks::on_update_keyframes(keyframe_clones);
      async_target = std::async(std::launch::async, [this, keyframe_clones] { return create_target_map(keyframe_clones); });
    }
  }

  // Get the target map created asynchronously
  if (async_target.valid() && async_target.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
    target_map = async_target.get();
  }

  // Pop the oldest keyframe
  if (keyframes.size() > max_num_keyframes) {
    std::vector<EstimationFrame::ConstPtr> marginalized_keyframes = {keyframes.front()};
    Callbacks::on_marginalized_keyframes(marginalized_keyframes);

    keyframes.pop_front();
  }

  last_frame = new_frame;
  marginalized_frames.push_back(new_frame);
  Callbacks::on_marginalized_frames(marginalized_frames);

  return new_frame;
}

std::shared_ptr<OdometryEstimationCT::TargetMap> OdometryEstimationCT::create_target_map(const std::vector<EstimationFrame::ConstPtr>& keyframes) {
  int total_num_points = 0;
  for (const auto& keyframe : keyframes) {
    total_num_points += keyframe->frame->size();
  }

  gtsam_ext::FrameCPU::Ptr frame(new gtsam_ext::FrameCPU);
  frame->points_storage.resize(total_num_points);
  frame->covs_storage.resize(total_num_points);

  // Accumulate all the points in the world frame
  int begin = 0;
  for (const auto& keyframe : keyframes) {
    Eigen::Matrix4d R_world_lidar = Eigen::Matrix4d::Identity();
    R_world_lidar.block<3, 3>(0, 0) = keyframe->T_world_lidar.linear();

    for (int i = 0; i < keyframe->frame->size(); i++) {
      frame->points_storage[begin + i] = keyframe->T_world_lidar * keyframe->frame->points[i];
      frame->covs_storage[begin + i] = R_world_lidar * keyframe->frame->covs[i] * R_world_lidar.transpose();
    }
    begin += keyframe->frame->size();
  }

  frame->num_points = total_num_points;
  frame->points = frame->points_storage.data();
  frame->covs = frame->covs_storage.data();

  // Create a KDTree
  auto kdtree = std::make_shared<gtsam_ext::KdTree>(frame->points, frame->size());
  kdtree->search_eps = 1e-3;

  auto map = std::make_shared<TargetMap>();
  map->frame = frame;
  map->kdtree = kdtree;

  return map;
}

}  // namespace glim