// iKalibr: Unified Targetless Spatiotemporal Calibration Framework
// Copyright 2024, the School of Geodesy and Geomatics (SGG), Wuhan University, China
// https://github.com/Unsigned-Long/iKalibr.git
//
// Author: Shuolong Chen (shlchen@whu.edu.cn)
// GitHub: https://github.com/Unsigned-Long
//  ORCID: 0000-0002-5283-9057
//
// Purpose: See .h/.hpp file.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * The names of its contributors can not be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "calib/calib_solver.h"
#include "pangolin/display/display.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "core/colmap_data_io.h"
#include "opencv2/highgui.hpp"
#include "util/tqdm.h"
#include "ros/package.h"
#include "sensor/camera_data_loader.h"

namespace {
bool IKALIBR_UNIQUE_NAME(_2_) = ns_ikalibr::_1_(__FILE__);
}

namespace ns_ikalibr {

// ----------
// ImagesInfo
// ----------

std::optional<std::string> ImagesInfo::GetImagePath(ns_veta::IndexT id) const {
    auto iter = images.find(id);
    if (iter == images.cend()) {
        return {};
    } else {
        return root_path + '/' + iter->second;
    }
}

std::optional<std::string> ImagesInfo::GetImageFilename(ns_veta::IndexT id) const {
    auto iter = images.find(id);
    if (iter == images.cend()) {
        return {};
    } else {
        return iter->second;
    }
}

std::map<ns_veta::IndexT, std::string> ImagesInfo::GetImagesIdxToName() const { return images; }

std::map<std::string, ns_veta::IndexT> ImagesInfo::GetImagesNameToIdx() const {
    std::map<std::string, ns_veta::IndexT> imgsInvKV;
    for (const auto &[k, v] : images) {
        imgsInvKV.insert({v, k});
    }
    return imgsInvKV;
}

// -----------
// CalibSolver
// -----------

CalibSolver::CalibSolver(CalibDataManager::Ptr calibDataManager,
                         CalibParamManager::Ptr calibParamManager)
    : _dataMagr(std::move(calibDataManager)),
      _parMagr(std::move(calibParamManager)),
      _ceresOption(Estimator::DefaultSolverOptions(
          Configor::Preference::AvailableThreads(), true, Configor::Preference::UseCudaInSolving)),
      _viewer(nullptr),
      _solveFinished(false) {
    // create so3 and linear scale splines given start and end times, knot distances
    _splines = CreateSplineBundle(
        _dataMagr->GetCalibStartTimestamp(), _dataMagr->GetCalibEndTimestamp(),
        Configor::Prior::KnotTimeDist::SO3Spline, Configor::Prior::KnotTimeDist::ScaleSpline);

    // create viewer
    _viewer = Viewer::Create(_parMagr, _splines);
    if (!Configor::IsCameraIntegrated() && !Configor::IsLiDARIntegrated()) {
        auto modelPath = ros::package::getPath("ikalibr") + "/model/ikalibr.obj";
        if (std::filesystem::exists(modelPath)) {
            _viewer->AddObjEntity(modelPath, Viewer::VIEW_MAP);
            _viewer->AddObjEntity(modelPath, Viewer::VIEW_ASSOCIATION);
        } else {
            spdlog::warn("can not load models from '{}'!", modelPath);
        }
    }

    // pass the 'CeresViewerCallBack' to ceres option so that update the viewer after every
    // interation in ceres
    _ceresOption.callbacks.push_back(new CeresViewerCallBack(_viewer));
    _ceresOption.update_state_every_iteration = true;

    // output spatiotemporal parameters after each iteration if needed
    if (IsOptionWith(OutputOption::ParamInEachIter, Configor::Preference::Outputs)) {
        _ceresOption.callbacks.push_back(new CeresDebugCallBack(_parMagr));
    }
}

CalibSolver::Ptr CalibSolver::Create(const CalibDataManager::Ptr &calibDataManager,
                                     const CalibParamManager::Ptr &calibParamManager) {
    return std::make_shared<CalibSolver>(calibDataManager, calibParamManager);
}

CalibSolver::~CalibSolver() {
    // solving is not performed or not finished as an exception is thrown
    if (!_solveFinished) {
        pangolin::QuitAll();
    }
    // solving is finished (when use 'pangolin::QuitAll()', the window not quit immediately)
    while (_viewer->IsActive()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::optional<Sophus::SE3d> CalibSolver::CurBrToW(double timeByBr) {
    if (GetScaleType() != TimeDeriv::LIN_POS_SPLINE) {
        throw Status(Status::CRITICAL,
                     "'CurBrToW' error, scale spline is not translation spline!!!");
    }
    const auto &so3Spline = _splines->GetSo3Spline(Configor::Preference::SO3_SPLINE);
    const auto &posSpline = _splines->GetRdSpline(Configor::Preference::SCALE_SPLINE);
    if (!so3Spline.TimeStampInRange(timeByBr) || !posSpline.TimeStampInRange(timeByBr)) {
        return {};
    } else {
        return Sophus::SE3d(so3Spline.Evaluate(timeByBr), posSpline.Evaluate(timeByBr));
    }
}

std::optional<Sophus::SE3d> CalibSolver::CurLkToW(double timeByLk, const std::string &topic) {
    if (GetScaleType() != TimeDeriv::LIN_POS_SPLINE) {
        throw Status(Status::CRITICAL,
                     "'CurLkToW' error, scale spline is not translation spline!!!");
    }
    double timeByBr = timeByLk + _parMagr->TEMPORAL.TO_LkToBr.at(topic);
    const auto &so3Spline = _splines->GetSo3Spline(Configor::Preference::SO3_SPLINE);
    const auto &posSpline = _splines->GetRdSpline(Configor::Preference::SCALE_SPLINE);

    if (!so3Spline.TimeStampInRange(timeByBr) || !posSpline.TimeStampInRange(timeByBr)) {
        return {};
    } else {
        Sophus::SE3d curBrToW(so3Spline.Evaluate(timeByBr), posSpline.Evaluate(timeByBr));
        return curBrToW * _parMagr->EXTRI.SE3_LkToBr(topic);
    }
}

std::optional<Sophus::SE3d> CalibSolver::CurCmToW(double timeByCm, const std::string &topic) {
    if (GetScaleType() != TimeDeriv::LIN_POS_SPLINE) {
        throw Status(Status::CRITICAL,
                     "'CurCmToW' error, scale spline is not translation spline!!!");
    }
    double timeByBr = timeByCm + _parMagr->TEMPORAL.TO_CmToBr.at(topic);
    const auto &so3Spline = _splines->GetSo3Spline(Configor::Preference::SO3_SPLINE);
    const auto &posSpline = _splines->GetRdSpline(Configor::Preference::SCALE_SPLINE);

    if (!so3Spline.TimeStampInRange(timeByBr) || !posSpline.TimeStampInRange(timeByBr)) {
        return {};
    } else {
        Sophus::SE3d curBrToW(so3Spline.Evaluate(timeByBr), posSpline.Evaluate(timeByBr));
        return curBrToW * _parMagr->EXTRI.SE3_CmToBr(topic);
    }
}

std::optional<Sophus::SE3d> CalibSolver::CurRjToW(double timeByRj, const std::string &topic) {
    if (GetScaleType() != TimeDeriv::LIN_POS_SPLINE) {
        throw Status(Status::CRITICAL,
                     "'CurRjToW' error, scale spline is not translation spline!!!");
    }
    double timeByBr = timeByRj + _parMagr->TEMPORAL.TO_RjToBr.at(topic);
    const auto &so3Spline = _splines->GetSo3Spline(Configor::Preference::SO3_SPLINE);
    const auto &posSpline = _splines->GetRdSpline(Configor::Preference::SCALE_SPLINE);

    if (!so3Spline.TimeStampInRange(timeByBr) || !posSpline.TimeStampInRange(timeByBr)) {
        return {};
    } else {
        Sophus::SE3d curBrToW(so3Spline.Evaluate(timeByBr), posSpline.Evaluate(timeByBr));
        return curBrToW * _parMagr->EXTRI.SE3_RjToBr(topic);
    }
}

TimeDeriv::ScaleSplineType CalibSolver::GetScaleType() {
    if (Configor::IsLiDARIntegrated() || Configor::IsCameraIntegrated()) {
        return TimeDeriv::ScaleSplineType::LIN_POS_SPLINE;
    } else if (Configor::IsRadarIntegrated()) {
        return TimeDeriv::ScaleSplineType::LIN_VEL_SPLINE;
    } else {
        return TimeDeriv::ScaleSplineType::LIN_ACCE_SPLINE;
    }
}

CalibSolver::SplineBundleType::Ptr CalibSolver::CreateSplineBundle(double st,
                                                                   double et,
                                                                   double so3Dt,
                                                                   double scaleDt) {
    // create splines
    auto so3SplineInfo = ns_ctraj::SplineInfo(Configor::Preference::SO3_SPLINE,
                                              ns_ctraj::SplineType::So3Spline, st, et, so3Dt);
    auto scaleSplineInfo = ns_ctraj::SplineInfo(Configor::Preference::SCALE_SPLINE,
                                                ns_ctraj::SplineType::RdSpline, st, et, scaleDt);
    spdlog::info(
        "create spline bundle: start time: '{:.5f}', end time: '{:.5f}', so3 dt : '{:.5f}', scale "
        "dt: '{:.5f}'",
        st, et, so3Dt, scaleDt);
    return SplineBundleType::Create({so3SplineInfo, scaleSplineInfo});
}

void CalibSolver::AlignStatesToGravity() {
    auto &so3Spline = _splines->GetSo3Spline(Configor::Preference::SO3_SPLINE);
    auto &scaleSpline = _splines->GetRdSpline(Configor::Preference::SCALE_SPLINE);
    // current gravity, velocities, and rotations are expressed in the reference frame
    // align them to the world frame whose negative z axis is aligned with the gravity vector
    auto SO3_RefToW =
        ObtainAlignedWtoRef(so3Spline.Evaluate(so3Spline.MinTime()), _parMagr->GRAVITY).inverse();
    _parMagr->GRAVITY = SO3_RefToW * _parMagr->GRAVITY;
    for (int i = 0; i < static_cast<int>(so3Spline.GetKnots().size()); ++i) {
        so3Spline.GetKnot(i) = SO3_RefToW * so3Spline.GetKnot(i);
    }
    for (int i = 0; i < static_cast<int>(scaleSpline.GetKnots().size()); ++i) {
        // attention: for three kinds of scale splines, this holds
        scaleSpline.GetKnot(i) = SO3_RefToW * scaleSpline.GetKnot(i) /* + Eigen::Vector3d::Zero()*/;
    }
}

void CalibSolver::StoreImagesForSfM(const std::string &topic, const std::set<IndexPair> &matchRes) {
    // -------------
    // output images
    // -------------
    auto path = ns_ikalibr::Configor::DataStream::CreateImageStoreFolder(topic);
    if (path == std::nullopt) {
        throw ns_ikalibr::Status(Status::CRITICAL,
                                 "can not create path for image storing for topic: '{}'!!!", topic);
    }
    ImagesInfo info(topic, *path, {});
    const auto &frames = _dataMagr->GetCameraMeasurements(topic);

    int size = static_cast<int>(frames.size());
    const auto &intri = _parMagr->INTRI.Camera.at(topic);

    auto bar = std::make_shared<tqdm>();
    for (int i = 0; i != size; ++i) {
        bar->progress(i, size);
        const auto &frame = frames.at(i);
        // generate the image name
        std::string filename = std::to_string(frame->GetId()) + ".jpg";
        info.images[frame->GetId()] = filename;

        cv::Mat undistImg = CalibParamManager::ParIntri::UndistortImage(intri, frame->GetImage());

        // save image
        cv::imwrite(*path + "/" + filename, undistImg);
    }
    bar->finish();

    // -------------------
    // colmap command line
    // -------------------
    auto ws = ns_ikalibr::Configor::DataStream::CreateSfMWorkspace(topic);
    if (ws == std::nullopt) {
        throw ns_ikalibr::Status(Status::CRITICAL,
                                 "can not create workspace for SfM for topic: '{}'!!!", topic);
    }
    const std::string database_path = *ws + "/database.db";
    const std::string image_path = *path;
    const std::string match_list_path = *ws + "/matches.txt";
    const std::string output_path = *ws;

    auto logger = spdlog::basic_logger_mt("sfm_cmd", *ws + "/sfm-command-line.txt", true);
    // feature extractor
    logger->info(
        "command line for 'feature_extractor' in colmap for topic '{}':\n"
        "colmap feature_extractor "
        "--database_path {} "
        "--image_path {} "
        "--ImageReader.camera_model PINHOLE "
        "--ImageReader.single_camera 1 "
        "--ImageReader.camera_params {:.3f},{:.3f},{:.3f},{:.3f}\n",
        topic, database_path, image_path, intri->FocalX(), intri->FocalY(),
        intri->PrincipalPoint()(0), intri->PrincipalPoint()(1));

    // feature match
    std::ofstream matchPairFile(match_list_path, std::ios::out);
    for (const auto &[view1Id, view2Id] : matchRes) {
        matchPairFile << std::to_string(view1Id) + ".jpg ";
        matchPairFile << std::to_string(view2Id) + ".jpg" << std::endl;
    }
    matchPairFile.close();

    logger->info(
        "command line for 'matches_importer' in colmap for topic '{}':\n"
        "colmap matches_importer "
        "--database_path {} "
        "--match_list_path {} "
        "--match_type pairs\n",
        topic, database_path, match_list_path);

    logger->info("------------------------------------------------------------------------------");
    logger->info("-  SfM Reconstruction in COLMAP [colmap gui] (recommend) or [colmap mapper]  -");
    logger->info("------------------------------------------------------------------------------");
    logger->info(
        "performing SfM using [colmap gui] is suggested, rather than the command line, "
        "which is very strict in initialization (finding initial image pair) and would cost lots "
        "of time!!!");
    // reconstruction
    logger->info(
        "command line for 'colmap gui' for topic '{}':\n"
        "colmap gui "
        "--database_path {} "
        "--image_path {}",
        topic, database_path, image_path, output_path);
    logger->info("------------------------------------------------------------------------------");
    double init_max_error = IsRSCamera(topic) ? 2.0 : 1.0;
    // reconstruction
    logger->info(
        "command line for 'colmap mapper' for topic '{}':\n"
        "colmap mapper "
        "--database_path {} "
        "--image_path {} "
        "--output_path {} "
        "--Mapper.init_min_tri_angle 25 "
        "--Mapper.init_max_error {} "
        "--Mapper.tri_min_angle 3 "
        "--Mapper.ba_refine_focal_length 0 "
        "--Mapper.ba_refine_principal_point 0",
        topic, database_path, image_path, output_path, init_max_error);
    logger->info(
        "------------------------------------------------------------------------------\n");

    // format convert
    logger->info(
        "command line for 'model_converter' in colmap for topic '{}':\n"
        "colmap model_converter "
        "--input_path {} "
        "--output_path {} "
        "--output_type TXT\n",
        topic, output_path + "/0", output_path);
    logger->flush();
    spdlog::drop("sfm_cmd");

    std::ofstream file(ns_ikalibr::Configor::DataStream::GetImageStoreInfoFile(topic));
    auto ar = GetOutputArchiveVariant(file, Configor::Preference::OutputDataFormat);
    SerializeByOutputArchiveVariant(ar, Configor::Preference::OutputDataFormat,
                                    cereal::make_nvp("info", info));
}

ns_veta::Veta::Ptr CalibSolver::TryLoadSfMData(const std::string &topic,
                                               double errorThd,
                                               std::size_t trackLenThd) {
    // info file
    const auto infoFilename = ns_ikalibr::Configor::DataStream::GetImageStoreInfoFile(topic);
    if (!std::filesystem::exists(infoFilename)) {
        spdlog::warn("the info file, i.e., '{}', dose not exists!!!", infoFilename);
        return nullptr;
    }

    const auto &sfmWsPath = ns_ikalibr::Configor::DataStream::CreateSfMWorkspace(topic);
    if (!sfmWsPath) {
        spdlog::warn("the sfm workspace for topic '{}' dose not exists!!!", topic);
        return nullptr;
    }

    const auto camerasFilename = *sfmWsPath + "/cameras.txt";
    if (!std::filesystem::exists(camerasFilename)) {
        spdlog::warn("the cameras file, i.e., '{}', dose not exists!!!", camerasFilename);
        return nullptr;
    }

    // images
    const auto imagesFilename = *sfmWsPath + "/images.txt";
    if (!std::filesystem::exists(imagesFilename)) {
        spdlog::warn("the images file, i.e., '{}', dose not exists!!!", imagesFilename);
        return nullptr;
    }

    // points
    const auto ptsFilename = *sfmWsPath + "/points3D.txt";
    if (!std::filesystem::exists(ptsFilename)) {
        spdlog::warn("the points 3D file, i.e., '{}', dose not exists!!!", ptsFilename);
        return nullptr;
    }

    // cameras

    // load info file
    ImagesInfo info("", "", {});
    {
        std::ifstream file(infoFilename);
        auto ar = GetInputArchiveVariant(file, Configor::Preference::OutputDataFormat);
        SerializeByInputArchiveVariant(ar, Configor::Preference::OutputDataFormat,
                                       cereal::make_nvp("info", info));
    }

    // load cameras
    auto cameras = ColMapDataIO::ReadCamerasText(camerasFilename);

    // load images
    auto images = ColMapDataIO::ReadImagesText(imagesFilename);

    // load landmarks
    auto points3D = ColMapDataIO::ReadPoints3DText(ptsFilename);

    auto veta = ns_veta::Veta::Create();

    // cameras
    assert(cameras.size() == 1);
    const auto &camera = cameras.cbegin()->second;
    assert(camera.params_.size() == 4);
    const auto &intriIdx = camera.camera_id_;
    auto intri = std::make_shared<ns_veta::PinholeIntrinsic>(*_parMagr->INTRI.Camera.at(topic));
    veta->intrinsics.insert({intriIdx, intri});

    // from images to our views and poses
    std::map<ns_veta::IndexT, CameraFrame::Ptr> ourIdxToCamFrame;
    for (const auto &frame : _dataMagr->GetCameraMeasurements(topic)) {
        ourIdxToCamFrame.insert({frame->GetId(), frame});
    }

    const auto &nameToOurIdx = info.GetImagesNameToIdx();
    for (const auto &[IdFromColmap, image] : images) {
        const auto &viewId = nameToOurIdx.at(image.name_);
        const auto &poseId = viewId;

        auto frameIter = ourIdxToCamFrame.find(viewId);
        // this frame is not involved in solving
        if (frameIter == ourIdxToCamFrame.cend()) {
            continue;
        }

        // view
        auto view = ns_veta::View::Create(
            // timestamp (aligned)
            frameIter->second->GetTimestamp(),
            // index
            viewId, intriIdx, poseId,
            // width, height
            intri->imgWidth, intri->imgHeight);
        veta->views.insert({viewId, view});

        // pose
        auto T_WorldToImg = ns_veta::Posed(image.QuatWorldToImg().matrix(), image.tvec_);
        // we store pose from camera to world
        veta->poses.insert({poseId, T_WorldToImg.Inverse()});
    }

    for (const auto &frame : _dataMagr->GetCameraMeasurements(topic)) {
        if (veta->views.count(frame->GetId()) == 0) {
            spdlog::warn(
                "frame indexed as '{}' of camera '{}' is involved in solving but not reconstructed "
                "in SfM!!!",
                frame->GetId(), topic);
        }
    }

    // from point3D to our structure
    for (const auto &[pt3dId, pt3d] : points3D) {
        // filter bad landmarks
        if (pt3d.error_ > errorThd || pt3d.track_.size() < trackLenThd) {
            continue;
        }

        auto &lm = veta->structure[pt3dId];
        lm.X = pt3d.xyz_;
        lm.color = pt3d.color_;

        for (const auto &track : pt3d.track_) {
            const auto &img = images.at(track.image_id);
            auto pt2d = img.points2D_.at(track.point2D_idx);

            if (pt3dId != pt2d.point3D_id_) {
                spdlog::warn(
                    "'point3D_id_' of point3D and 'point3D_id_' of feature connected are in "
                    "conflict!!!");
                continue;
            }

            const auto viewId = nameToOurIdx.at(img.name_);
            // this frame is not involved in solving
            if (veta->views.find(viewId) == veta->views.cend()) {
                continue;
            }

            lm.obs.insert({viewId, ns_veta::Observation(pt2d.xy_, track.point2D_idx)});
        }
        if (lm.obs.size() < trackLenThd) {
            veta->structure.erase(pt3dId);
        }
    }

    return veta;
}

void CalibSolver::PerformTransformForVeta(const ns_veta::Veta::Ptr &veta,
                                          const ns_veta::Posed &curToNew,
                                          double scale) {
    // pose
    for (auto &[id, pose] : veta->poses) {
        pose.Translation() *= scale;
        pose = curToNew * pose;
    }

    // structure
    for (auto &[id, lm] : veta->structure) {
        lm.X *= scale;
        lm.X = curToNew(lm.X);
    }
}

bool CalibSolver::IsRSCamera(const std::string &camTopic) {
    auto model = EnumCast::stringToEnum<CameraModelType>(
        Configor::DataStream::CameraTopics.at(camTopic).Type);
    return IsOptionWith(CameraModelType::RS, model);
}

void CalibSolver::DownsampleVeta(const ns_veta::Veta::Ptr &veta,
                                 std::size_t lmNumThd,
                                 std::size_t obvNumThd) {
    std::default_random_engine engine(std::chrono::system_clock::now().time_since_epoch().count());
    if (veta->structure.size() > lmNumThd) {
        std::vector<ns_veta::IndexT> lmIdVec;
        lmIdVec.reserve(veta->structure.size());
        for (const auto &[lmId, lm] : veta->structure) {
            lmIdVec.push_back(lmId);
        }
        auto lmIdVecToMove =
            SamplingWoutReplace2(engine, lmIdVec, veta->structure.size() - lmNumThd);
        for (const auto &id : lmIdVecToMove) {
            veta->structure.erase(id);
        }
    }
    for (auto &[lmId, lm] : veta->structure) {
        if (lm.obs.size() < obvNumThd) {
            continue;
        }

        std::vector<ns_veta::IndexT> obvIdVec;
        obvIdVec.reserve(lm.obs.size());
        for (const auto &[viewId, obv] : lm.obs) {
            obvIdVec.push_back(viewId);
        }

        auto obvIdVecToMove = SamplingWoutReplace2(engine, obvIdVec, lm.obs.size() - obvNumThd);
        for (const auto &id : obvIdVecToMove) {
            lm.obs.erase(id);
        }
    }
}

void CalibSolver::SaveStageCalibParam(const CalibParamManager::Ptr &par, const std::string &desc) {
    const static std::string paramDir = Configor::DataStream::OutputPath + "/iteration/stage";
    if (!std::filesystem::exists(paramDir) && !std::filesystem::create_directories(paramDir)) {
        spdlog::warn("create directory failed: '{}'", paramDir);
    } else {
        const std::string paramFilename =
            paramDir + "/" + desc + ns_ikalibr::Configor::GetFormatExtension();
        par->Save(paramFilename, ns_ikalibr::Configor::Preference::OutputDataFormat);
    }
}

// ------------------
// CeresDebugCallBack
// ------------------

CeresDebugCallBack::CeresDebugCallBack(CalibParamManager::Ptr calibParamManager)
    : _parMagr(std::move(calibParamManager)),
      _outputDir(Configor::DataStream::OutputPath + "/iteration/epoch"),
      _idx(0) {
    if (std::filesystem::exists(_outputDir)) {
        std::filesystem::remove_all(_outputDir);
    }

    if (!std::filesystem::create_directories(_outputDir)) {
        spdlog::warn("create directory failed: '{}'", _outputDir);
    } else {
        _iterInfoFile = std::ofstream(_outputDir + "/epoch_info.csv", std::ios::out);
        _iterInfoFile << "cost,gradient,tr_radius(1/lambda)" << std::endl;
    }
}

ceres::CallbackReturnType CeresDebugCallBack::operator()(const ceres::IterationSummary &summary) {
    if (std::filesystem::exists(_outputDir)) {
        // save param
        const std::string paramFilename = _outputDir + "/ikalibr_param_" + std::to_string(_idx) +
                                          ns_ikalibr::Configor::GetFormatExtension();
        _parMagr->Save(paramFilename, ns_ikalibr::Configor::Preference::OutputDataFormat);

        // save iter info
        _iterInfoFile << _idx << ',' << summary.cost << ',' << summary.gradient_norm << ','
                      << summary.trust_region_radius << std::endl;

        ++_idx;
    }
    return ceres::SOLVER_CONTINUE;
}

CeresDebugCallBack::~CeresDebugCallBack() { _iterInfoFile.close(); }

// -------------------
// CeresViewerCallBack
// -------------------
CeresViewerCallBack::CeresViewerCallBack(Viewer::Ptr viewer)
    : _viewer(std::move(viewer)) {}

ceres::CallbackReturnType CeresViewerCallBack::operator()(const ceres::IterationSummary &summary) {
    _viewer->UpdateSensorViewer().UpdateSplineViewer();
    return ceres::CallbackReturnType::SOLVER_CONTINUE;
}
}  // namespace ns_ikalibr