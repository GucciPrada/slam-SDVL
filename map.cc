/*
 *  Copyright (C) 1997-2017 JdeRobot Developers Team
 *
 *  This program is free software; you can redistribute it and/or modifdisty
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *   Authors : Eduardo Perdices <eperdices@gsyc.es>
 *
 */

#include "./map.h"
#include "./matcher.h"
#include "./config.h"
#include "extra/utils.h"
#include "extra/bundle.h"
#include "extra/timer.h"
#include "extra/se3.h"

using std::shared_ptr;
using std::vector;
using std::cout;
using std::endl;

namespace sdvl {

void* callback_map(void* obj) {
  static_cast<Map*>(obj)->Run();
  return (0);
}

Map::Map(int cell_size) {
  cell_size_ = cell_size;
  n_initializations_ = 0;

  candidates_updating_halt_ = false;
  new_keyframe_set_ = false;
  relocalizing_ = false;
  ba_kf_ = nullptr;
  last_kf_ = nullptr;
  last_matches_ = 0;

  pthread_mutex_init(&mutex_map_, NULL);
}

Map::~Map() {
  keyframes_.clear();
  EmptyTrash();
}

void Map::Start() {
  pthread_create(&thread_, 0, &callback_map, this);
}

void Map::Stop() {
  cout << "Stopping Map..." << endl;
  running_ = false;
  candidates_updating_halt_ = true;
}

int Map::Run() {
  running_ = true;
  while (running_) {
    UpdateMap();
    usleep(2 * 1000);
  }
  return 0;
}

void Map::UpdateMap() {
  shared_ptr<Frame> frame = nullptr;
  bool is_empty;

  if (relocalizing_)
    return;

  Lock();
  is_empty = frame_queue_.empty();
  Unlock();

  if (!is_empty) {
    Timer timer(true);

    Lock();
    if (new_keyframe_set_) {
      // Search keyframe
      do {
        assert(!frame_queue_.empty());
        frame = frame_queue_.front();
        frame_queue_.pop();
        if (!frame->IsKeyframe())
          frame_trash_.push_back(frame);
      } while (!frame->IsKeyframe());
    } else {
      // Get last frame
      frame = frame_queue_.front();
      frame_queue_.pop();
    }
    new_keyframe_set_ = false;
    candidates_updating_halt_ = false;
    Unlock();

    // Update current candidates
    UpdateCandidates(frame);
    if (frame->IsKeyframe()) {
      CheckConnections(frame);
      InitCandidates(frame);
    } else {
      // Delete frame after update
      Lock();
      frame_trash_.push_back(frame);
      Unlock();
    }

    // Check Bundle adjustment
    int size = (int)keyframes_.size();
    if (size > 2 &&  ba_kf_ != nullptr) {
      Timer timerba(true);
      BundleAdjustment();
      timerba.Stop();
      cout << "[INFO] Bundle Adjustment time is " << timerba.GetMsTime() << "ms" << endl;
    }

    timer.Stop();
    cout << "[INFO] Map time is " << timer.GetMsTime() << "ms" << endl;
  }
}

void Map::AddKeyframe(const shared_ptr<Frame> &frame, bool search) {
  if (search) {
    Lock();
    new_keyframe_set_ = true;
    candidates_updating_halt_ = true;
    frame_queue_.push(frame);
    Unlock();
  }
  keyframes_.push_back(frame);
  last_kf_ = frame;
  ba_kf_ = frame;
}

void Map::AddFrame(const shared_ptr<Frame> &frame) {
  Lock();
  frame_queue_.push(frame);
  Unlock();
}

void Map::DeletePoint(const std::shared_ptr<Point> &point) {
  Lock();
  points_trash_.push_back(point);
  Unlock();
}

bool Map::NeedKeyframe(const shared_ptr<Frame> &frame, int matches) {
  int npoints = frame->GetNumPoints();

  // Min iterations between frames
  bool enough_its = (frame->GetID() - last_kf_->GetID()) >= Config::MinKeyframeIts();

  // Many matches lost
  bool lost_many = npoints < last_matches_*Config::LostRatio();
  bool lost_some = npoints < last_matches_*0.9;

  last_matches_ = std::max(last_matches_, npoints);

  // If many matches are lost, force Keyframe
  if ((enough_its && lost_some) || lost_many) {
    last_matches_ = npoints;
    return true;
  }
  return false;
}

void Map::LimitKeyframes(const shared_ptr<Frame> &frame) {
  // Check if we have enough keyframes
  if (static_cast<int>(keyframes_.size()) < Config::MaxKeyframes()) {
    return;
  }

  // Get furthest keyframe
  shared_ptr<Frame> kf = GetFurthestKeyframe(frame);
  Lock();
  frame_trash_.push_back(kf);
  Unlock();

  cout << "[DEBUG] Furthest keyframe send to trash" << endl;
}

void Map::EmptyTrash() {
  vector<shared_ptr<Frame>> frame_trash_cp;
  vector<shared_ptr<Point>> points_trash_cp;

  // Copy frames and points that are going to be deleted
  Lock();
  for (auto it=frame_trash_.begin(); it != frame_trash_.end(); it++) {
    frame_trash_cp.push_back(*it);
    *it = nullptr;
  }
  frame_trash_.clear();
  for (auto it=points_trash_.begin(); it != points_trash_.end(); it++) {
    points_trash_cp.push_back(*it);
    *it = nullptr;
  }
  points_trash_.clear();
  Unlock();

  // Delete frames
  for (auto fit=frame_trash_cp.begin(); fit != frame_trash_cp.end(); fit++) {
    if ((*fit)->IsKeyframe()) {
      for (auto it=keyframes_.begin(); it != keyframes_.end(); it++) {
        if (*it == *fit) {
          (*it)->RemoveFeatures();
          keyframes_.erase(it);
          cout << "[DEBUG] Keyframe removed" << endl;
          break;
        }
      }
    }
    (*fit)->RemoveFeatures();
    *fit = nullptr;
  }
  frame_trash_cp.clear();

  // Delete points
  for (auto it=points_trash_cp.begin(); it != points_trash_cp.end(); it++) {
    std::list<shared_ptr<Feature>>& features = (*it)->GetFeatures();
    for (auto it=features.begin(); it != features.end(); it++)
      (*it)->SetPoint(nullptr);

    features.clear();
    (*it)->SetDelete();
    *it = nullptr;
  }
  points_trash_cp.clear();
}

struct KeyframesComparator {
  bool operator()(const std::pair<shared_ptr<Frame>, double>& left, const std::pair<shared_ptr<Frame>, double>& right) {
      return left.second < right.second;
  }
};

void Map::InitCandidates(const shared_ptr<Frame> &frame) {
  Matcher matcher(Config::PatchSize());
  shared_ptr<Feature> feature2;
  Eigen::Vector2d imgpos;
  int level, index;
  double depth, depth_mean, distance, cos_alpha;
  bool fixed = false;

  ResetSelected();
  frame->SetSelected(true);

  cout << "[DEBUG] Init new points" << endl;

  // Get connected keyframes
  vector<shared_ptr<Frame>> best_kfs;
  frame->GetBestConnections(&best_kfs, Config::MaxSearchKeyframes());
  if (best_kfs.empty())
    return;

  // Filter frame corners
  frame->FilterCorners(cell_size_);
  vector<Eigen::Vector3i> &fcorners = frame->GetFilteredCorners();
  vector<vector<uchar>>& descriptors = frame->GetFilteredDescriptors();
  vector<bool> imatches(fcorners.size(), false);

  if (Config::UseORB())
    assert(descriptors.size() == fcorners.size());

  Lock();
  candidates_updating_halt_ = true;
  Unlock();
  n_initializations_++;

  depth_mean = frame->GetSceneDepth();
  for (auto it_kf=best_kfs.begin(); it_kf != best_kfs.end(); it_kf++) {
    shared_ptr<Frame> cframe = *it_kf;
    cframe->SetSelected(true);

    // Check distance between frames. Pure rotation leads to wrong triangulation
    distance = frame->DistanceTo(*cframe);
    if (distance/depth_mean < 0.01)
      continue;

    // try to initialize a candidate for every corner
    index = 0;
    for (auto it=fcorners.begin(); it != fcorners.end(); it++, index++) {
      if (imatches[index])
        continue;

      shared_ptr<Point> candidate = std::make_shared<Point>();
      shared_ptr<Feature> feature = std::make_shared<Feature>(frame, Eigen::Vector2d((*it)(0), (*it)(1)), (*it)(2));

      if (Config::UseORB()) {
        // Get descriptor
        feature->GetDescriptor() = descriptors[index];
      }

      // Search corner in closest keyframe
      if (!matcher.SearchPoint(cframe, feature, 1.0/depth_mean, 1.0, &imgpos, &level))
        continue;

      // Get depth from triangulation
      SE3 pose = cframe->GetPose() * frame->GetPose().Inverse();
      feature2 = std::make_shared<Feature>(cframe, imgpos, level);
      if (!GetDepthFromTriangulation(pose, feature->GetVector(), feature2->GetVector(), &depth))
        continue;

      // Check if parallax is almost 0
      Eigen::Vector3d p3d = frame->GetWorldPose()*(depth*feature->GetVector());
      cos_alpha = GetParallax(frame->GetWorldPosition(), cframe->GetWorldPosition(), p3d);
      if (cos_alpha >= 0.999999)
        continue;

      // Too close
      if (depth < Config::MapScale()*Config::ScaleMinDist() || depth < depth_mean*Config::ScaleMinDist())
        continue;

      Lock();
      // Link to first frame
      candidate->InitCandidate(feature, depth);
      frame->AddFeature(feature);
      candidate->AddFeature(feature);
      feature->SetPoint(candidate);

      // Link to closest keyframe
      cframe->AddFeature(feature2);
      candidate->AddFeature(feature2);
      feature2->SetPoint(candidate);
      Unlock();

      imatches[index] = true;
      candidates_.push_back(candidate);

      if (fixed) {
        // Set fixed position
        Eigen::Vector3d pos = candidate->GetPosition();
        candidate->SetFixed();
        candidate->SetPosition(pos);
      } else {
        candidates_.push_back(candidate);
      }
    }
  }

  Lock();
  candidates_updating_halt_ = false;
  Unlock();
}

void Map::UpdateCandidates(const shared_ptr<Frame> &frame) {
  Matcher matcher(Config::PatchSize());
  Eigen::Vector3d pos;
  Eigen::Vector2d imgpos;
  int level, maxupdates = 2000;
  bool halt;
  double depth_mean, depth, distance, cos_alpha;
  double px_error_angle = frame->GetCamera()->GetPixelErrorAngle();
  int index = std::max(0, static_cast<int>(candidates_.size())-maxupdates);

  depth_mean = frame->GetSceneDepth();

  // Update candidates
  vector<shared_ptr<Point>>::iterator it = candidates_.begin()+index;
  while (it != candidates_.end()) {
    Lock();
    halt = candidates_updating_halt_;
    Unlock();

    // Updating halted
    if (halt)
      return;

    shared_ptr<Point> point = *it;
    assert(point);

    // Check if we have to delete it
    if (point->ToDelete()) {
      Lock();
      points_trash_.push_back(point);
      Unlock();
      it = candidates_.erase(it);
      continue;
    }

    // Get point position and check if is visible from current FOV
    pos = point->GetPosition();
    if (!frame->IsPointVisible(pos)) {
      it++;
      continue;
    }

    shared_ptr<Feature> feature = point->GetInitFeature();

    // Check displacement from initial frame
    distance = frame->DistanceTo(*(feature->GetFrame()));
    if (distance/depth_mean < 0.01) {
      it++;
      continue;
    }

    // Find candidate in epipolar line
    if (!matcher.SearchPoint(frame, feature, point->GetInverseDepth(), point->GetStd(), &imgpos, &level)) {
      if (point->Unpromote())
        DeletePoint(point);
      it++;
      continue;
    }

    // Get depth from triangulation
    SE3 pose = frame->GetPose() * feature->GetFrame()->GetPose().Inverse();
    Eigen::Vector3d v3d = frame->GetCamera()->Unproject(imgpos);
    if (!GetDepthFromTriangulation(pose, feature->GetVector(), v3d, &depth)) {
      it++;
      continue;
    }

    // Check if parallax is almost 0
    Eigen::Vector3d p3d = feature->GetFrame()->GetWorldPose()*(depth*feature->GetVector());
    cos_alpha = GetParallax(feature->GetFrame()->GetWorldPosition(), frame->GetWorldPosition(), p3d);
    if (cos_alpha >= 0.999999) {
      it++;
      continue;
    }

    // Too close
    if (depth < Config::MapScale()*Config::ScaleMinDist()  || depth < depth_mean*Config::ScaleMinDist()) {
      it++;
      continue;
    }

    // Update candidate
    point->Update(frame, depth, px_error_angle);

    // Change candidate type if it has converged
    if (point->HasConverged()) {
      // Remove from candidates
      it = candidates_.erase(it);
    } else {
      it++;
    }
  }
}

void Map::CheckConnections(const shared_ptr<Frame> &frame) {
  std::map<shared_ptr<Frame>, int> kfs;
  shared_ptr<Frame> best_kf;
  int min_connections, best_n;
  bool saved;

  Lock();
  // Count how many points are seen from each keyframe
  vector<shared_ptr<Feature>>& features = frame->GetFeatures();
  for (auto it=features.begin(); it != features.end(); it++) {
    assert(*it != nullptr);
    shared_ptr<Point> point = (*it)->GetPoint();
    if (!point || point->ToDelete())
      continue;

    std::list<shared_ptr<Feature>>& ffeatures = point->GetFeatures();
    for (auto fit=ffeatures.begin(); fit != ffeatures.end(); fit++) {
      assert(*fit != nullptr);
      if ((*fit)->GetFrame()->GetID() == frame->GetID())
        continue;
      kfs[(*fit)->GetFrame()]++;
    }
  }
  Unlock();

  if (kfs.empty()) {
    return;
  }

  best_n = 0;
  saved = false;
  min_connections = Config::MinMatches()/2;

  Lock();
  // Rule out connections under threshold
  for (auto it=kfs.begin(); it != kfs.end(); it++) {
    if (it->second > best_n) {
      best_n = it->second;
      best_kf = it->first;
    }

    // Connect to each other
    if (it->second >= min_connections) {
      frame->AddConnection(std::make_pair(it->first, it->second));
      it->first->AddConnection(std::make_pair(frame, it->second));
      saved = true;
    }
  }

  // At least add one
  if (!saved && best_n > 0) {
    frame->AddConnection(std::make_pair(best_kf, best_n));
    best_kf->AddConnection(std::make_pair(frame, best_n));
  }
  Unlock();
}

shared_ptr<Frame> Map::GetFurthestKeyframe(const shared_ptr<Frame> &frame) {
  Eigen::Vector3d pos = frame->GetWorldPosition();
  shared_ptr<Frame> kf = nullptr;
  double dist, maxdist = 0.0;

  for (auto it=keyframes_.begin(); it != keyframes_.end(); it++) {
    dist = ((*it)->GetWorldPosition() - pos).norm();
    if (dist > maxdist) {
      maxdist = dist;
      kf = *it;
    }
  }
  return kf;
}

bool Map::TransformInitialMap(const shared_ptr<Frame> &frame) {
  int ind1, ind2, ind3;
  cout << "[DEBUG] Transform initial map" << endl;

  vector<shared_ptr<Feature>>& features = frame->GetFeatures();
  int nfeatures = features.size();

  Eigen::Vector3d vbestmean(0, 0, 0);
  Eigen::Vector3d vbestnormal(0, 0, 0);
  double bestdist = 9999999999999999.9;
  int nit = 0, nits = 100;

  // Search best plane with RANSAC
  while (nit < nits) {
    ind1 = rand() % nfeatures;
    ind2 = ind1;
    ind3 = ind1;
    while (ind2 == ind1)
      ind2 = rand() % nfeatures;
    while (ind3 == ind1 || ind3 == ind2)
      ind3 = rand() % nfeatures;

    Eigen::Vector3d p1 = features[ind1]->GetPoint()->GetPosition();
    Eigen::Vector3d p2 = features[ind2]->GetPoint()->GetPosition();
    Eigen::Vector3d p3 = features[ind3]->GetPoint()->GetPosition();

    Eigen::Vector3d vmean = 0.33333333 * (p1 + p2 + p2);
    Eigen::Vector3d vdiff1 = p3 - p1;
    Eigen::Vector3d vdiff2 = p2 - p1;
    Eigen::Vector3d vnormal = vdiff1.cross(vdiff2);

    if (vnormal.dot(vnormal) == 0) {
       nit++;
      continue;
    }
    vnormal.normalize();

    double dSumError = 0.0;
    int valids = 0;
    for (auto it=features.begin(); it != features.end(); it++) {
      Eigen::Vector3d vdiff = (*it)->GetPoint()->GetPosition() - vmean;
      double dDistSq = vdiff.dot(vdiff);
      if (dDistSq == 0.0)
        continue;
      double dNormDist = fabs(vdiff.dot(vnormal));

      if (dNormDist > 0.05)
        dNormDist = 0.05;
      else
        valids++;
      dSumError += dNormDist;
    }
    if (dSumError < bestdist) {
      bestdist = dSumError;
      vbestmean = vmean;
      vbestnormal = vnormal;

      // Update max iterations
      double epsilon = static_cast<double>(valids) / static_cast<double>(nfeatures);
      double k = log(1.0 - 0.999)/log(1.0 - epsilon);
      double std = sqrt(1.0 - epsilon)/epsilon;
      nits = std::min(nits, static_cast<int>(k+2.0*std + 1.0));
    }
    nit++;
  }

  // Collect inlier set
  vector<Eigen::Vector3d> inliers;
  for (auto it=features.begin(); it != features.end(); it++) {
    Eigen::Vector3d vdiff = (*it)->GetPoint()->GetPosition() - vbestmean;
    double dDistSq = vdiff.dot(vdiff);
    if (dDistSq == 0.0)
      continue;
    double dNormDist = fabs(vdiff.dot(vbestnormal));

    if (dNormDist < 0.05) {
      inliers.push_back((*it)->GetPoint()->GetPosition());
    }
  }

  cout << "[INFO] Main plane has " << inliers.size() << " inliers out of " << nfeatures << endl;

  // With these inliers, calculate mean and cov
  Eigen::Vector3d vmeaninliers = Eigen::Vector3d::Zero();
  for (vector<Eigen::Vector3d>::iterator it=inliers.begin(); it != inliers.end(); it++)
    vmeaninliers += *it;
  vmeaninliers *= (1.0 / inliers.size());

  Eigen::Matrix3d mcov = Eigen::Matrix3d::Zero();
  for (vector<Eigen::Vector3d>::iterator it=inliers.begin(); it != inliers.end(); it++) {
    Eigen::Vector3d vdiff = *it - vmeaninliers;
    mcov += vdiff * vdiff.transpose();
  }

  // Find the principal component with the minimal variance: this is the plane normal
  Eigen::EigenSolver<Eigen::MatrixXd> solver(mcov);
  Eigen::VectorXcd vcomplex = solver.eigenvectors().col(2);
  Eigen::Vector3d vnormal;
  vnormal(0) = vcomplex(0).real();
  vnormal(1) = vcomplex(1).real();
  vnormal(2) = vcomplex(2).real();

  // Use the version of the normal which points towards the cam center
  if (vnormal(2) > 0)
    vnormal *= -1.0;

  Eigen::Matrix3d m3Rot = Eigen::Matrix3d::Identity();
  m3Rot.block(2, 0, 1, 3) = vnormal.transpose();
  Eigen::Vector3d fila0 = m3Rot.block(0, 0, 1, 3).transpose();
  Eigen::Vector3d aux = fila0 - vnormal * (fila0.dot(vnormal));
  aux.normalize();
  fila0 = aux;
  m3Rot.block(0, 0, 1, 3) = fila0.transpose();

  Eigen::Vector3d fila2 = m3Rot.block(2, 0, 1, 3).transpose();
  m3Rot.block(1, 0, 1, 3) = (fila2.cross(fila0)).transpose();

  SE3 se3Aligner;
  se3Aligner.SetRotation(m3Rot);
  Eigen::Vector3d v3RMean = se3Aligner * vmeaninliers;
  se3Aligner.SetTranslation(-v3RMean);

  // Transform map
  for (auto it=keyframes_.begin(); it != keyframes_.end(); it++)
    (*it)->SetPose((*it)->GetPose()*se3Aligner.Inverse());
  for (auto it=features.begin(); it != features.end(); it++) {
    if ((*it)->GetPoint()->IsFixed()) {
      Eigen::Vector3d pos = se3Aligner * (*it)->GetPoint()->GetPosition();
      (*it)->GetPoint()->SetPosition(pos);
    }
  }

  return true;
}

void Map::ResetSelected() {
  for (auto it=keyframes_.begin(); it != keyframes_.end(); it++)
    (*it)->SetSelected(false);
}

void Map::BundleAdjustment() {
  vector<shared_ptr<Frame>> fov_kfs_;
  bool local = true;

  if (local) {
    // Get keyframes sharing its field of view
    ba_kf_->GetBestConnections(&fov_kfs_, 2*Config::MaxSearchKeyframes());
    fov_kfs_.push_back(ba_kf_);
  } else {
    // Save all
    for (auto it=keyframes_.begin(); it != keyframes_.end(); it++)
      fov_kfs_.push_back(*it);
  }

  if (fov_kfs_.size() <= 1)
    return;

  cout << "[DEBUG] Perform Bundle Adjustment with " << fov_kfs_.size() << " Keyframes" << endl;

  // Run bundle adjustment
  Bundle ba(this);
  ba.Local(fov_kfs_);
  ba_kf_ = nullptr;
}

}  // namespace sdvl