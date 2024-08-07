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

#include "calib/calib_data_manager.h"
#include "rosbag/view.h"
#include "spdlog/spdlog.h"
#include "sensor/imu_data_loader.h"
#include "sensor/radar_data_loader.h"
#include "sensor/lidar_data_loader.h"
#include "sensor/camera_data_loader.h"
#include "opencv4/opencv2/imgcodecs.hpp"
#include "util/tqdm.h"

namespace {
bool IKALIBR_UNIQUE_NAME(_2_) = ns_ikalibr::_1_(__FILE__);
}

namespace ns_ikalibr {

// ----------------
// CalibDataManager
// ----------------

CalibDataManager::CalibDataManager() = default;

CalibDataManager::Ptr CalibDataManager::Create() { return std::make_shared<CalibDataManager>(); }

void CalibDataManager::LoadCalibData() {
    spdlog::info("loading calibration data...");

    // open the ros bag
    auto bag = std::make_unique<rosbag::Bag>();
    if (!std::filesystem::exists(Configor::DataStream::BagPath)) {
        spdlog::error("the ros bag path '{}' is invalid!", Configor::DataStream::BagPath);
    } else {
        bag->open(Configor::DataStream::BagPath, rosbag::BagMode::Read);
    }

    auto view = rosbag::View();

    // using a temp view to check the time range of the source ros bag
    auto viewTemp = rosbag::View();

    std::vector<std::string> topicsToQuery;
    // add topics to vector
    for (const auto &[topic, _] : Configor::DataStream::IMUTopics) {
        topicsToQuery.push_back(topic);
    }
    for (const auto &[topic, _] : Configor::DataStream::RadarTopics) {
        topicsToQuery.push_back(topic);
    }
    for (const auto &[topic, _] : Configor::DataStream::LiDARTopics) {
        topicsToQuery.push_back(topic);
    }
    for (const auto &[topic, _] : Configor::DataStream::CameraTopics) {
        topicsToQuery.push_back(topic);
    }

    viewTemp.addQuery(*bag, rosbag::TopicQuery(topicsToQuery));
    auto begTime = viewTemp.getBeginTime();
    auto endTime = viewTemp.getEndTime();
    spdlog::info("source data duration: from '{:.5f}' to '{:.5f}'.", begTime.toSec(),
                 endTime.toSec());

    // adjust the data time range
    if (Configor::DataStream::BeginTime > 0.0) {
        begTime += ros::Duration(Configor::DataStream::BeginTime);
        if (begTime > endTime) {
            spdlog::warn(
                "begin time '{:.5f}' is out of the bag's data range, set begin time to '{:.5f}'.",
                begTime.toSec(), viewTemp.getBeginTime().toSec());
            begTime = viewTemp.getBeginTime();
        }
    }
    if (Configor::DataStream::Duration > 0.0) {
        endTime = begTime + ros::Duration(Configor::DataStream::Duration);
        if (endTime > viewTemp.getEndTime()) {
            spdlog::warn(
                "end time '{:.5f}' is out of the bag's data range, set end time to '{:.5f}'.",
                endTime.toSec(), viewTemp.getEndTime().toSec());
            endTime = viewTemp.getEndTime();
        }
    }
    spdlog::info("expect data duration: from '{:.5f}' to '{:.5f}'.", begTime.toSec(),
                 endTime.toSec());

    view.addQuery(*bag, rosbag::TopicQuery(topicsToQuery), begTime, endTime);

    // create IMU data loader
    std::map<std::string, IMUDataLoader::Ptr> imuDataLoaders;
    std::map<std::string, RadarDataLoader::Ptr> radarDataLoaders;
    std::map<std::string, LiDARDataLoader::Ptr> lidarDataLoaders;
    std::map<std::string, CameraDataLoader::Ptr> cameraDataLoaders;

    // get type enum from the string
    for (const auto &[topic, config] : Configor::DataStream::IMUTopics) {
        imuDataLoaders.insert({topic, IMUDataLoader::GetLoader(config.Type)});
    }
    for (const auto &[topic, config] : Configor::DataStream::RadarTopics) {
        radarDataLoaders.insert({topic, RadarDataLoader::GetLoader(config.Type)});
    }
    for (const auto &[topic, config] : Configor::DataStream::LiDARTopics) {
        lidarDataLoaders.insert({topic, LiDARDataLoader::GetLoader(config.Type)});
    }
    for (const auto &[topic, config] : Configor::DataStream::CameraTopics) {
        cameraDataLoaders.insert({topic, CameraDataLoader::GetLoader(config.Type)});
    }

    // read raw data
    auto bar = std::make_shared<tqdm>();
    int idx = 0;
    for (auto iter = view.begin(); iter != view.end(); ++iter, ++idx) {
        bar->progress(idx, static_cast<int>(view.size()));
        const auto &item = *iter;
        const std::string &topic = item.getTopic();
        if (Configor::DataStream::IMUTopics.cend() != Configor::DataStream::IMUTopics.find(topic)) {
            // is an inertial frame
            auto mes = imuDataLoaders.at(topic)->UnpackFrame(item);
            if (mes != nullptr) {
                _imuMes[topic].push_back(mes);
            }
        } else if (Configor::DataStream::RadarTopics.cend() !=
                   Configor::DataStream::RadarTopics.find(topic)) {
            // is a radar frame
            auto mes = radarDataLoaders.at(topic)->UnpackScan(item);
            if (mes != nullptr) {
                _radarMes[topic].push_back(mes);
            }
        } else if (Configor::DataStream::LiDARTopics.cend() !=
                   Configor::DataStream::LiDARTopics.find(topic)) {
            // is a lidar frame
            auto mes = lidarDataLoaders.at(topic)->UnpackScan(item);
            if (mes != nullptr) {
                _lidarMes[topic].push_back(mes);
            }
        } else if (Configor::DataStream::CameraTopics.cend() !=
                   Configor::DataStream::CameraTopics.find(topic)) {
            // is a camera frame
            auto mes = cameraDataLoaders.at(topic)->UnpackFrame(item);
            if (mes != nullptr) {
                // id: uint64_t from timestamp (raw, millisecond)
                mes->SetId(static_cast<ns_veta::IndexT>(mes->GetTimestamp() * 1E3));
                _camMes[topic].push_back(mes);
            }
        }
    }
    bar->finish();
    bag->close();

    for (const auto &[topic, _] : Configor::DataStream::IMUTopics) {
        CheckTopicExists(topic, _imuMes);
    }
    for (const auto &[topic, _] : Configor::DataStream::RadarTopics) {
        CheckTopicExists(topic, _radarMes);
    }
    for (const auto &[topic, _] : Configor::DataStream::CameraTopics) {
        CheckTopicExists(topic, _camMes);
    }
    for (const auto &[topic, _] : Configor::DataStream::LiDARTopics) {
        CheckTopicExists(topic, _lidarMes);
    }

    // if the radar is AWR1843BOOST, data should be reorganized,
    // i.e., merge multiple radar target measurements to radar array measurements
    // note that although radar targets are wrapped as scans here (just for unification and
    // convenience), they are still fused separately in batch optimizations (a tightly-coupled
    // optimization framework)
    std::map<std::string, std::vector<RadarTargetArray::Ptr>> oldRadarMes = _radarMes;
    for (const auto &[topic, loader] : radarDataLoaders) {
        if (loader->GetRadarModel() == RadarModelType::AWR1843BOOST_RAW ||
            loader->GetRadarModel() == RadarModelType::AWR1843BOOST_CUSTOM) {
            const auto &mes = oldRadarMes.at(topic);
            std::vector<RadarTarget::Ptr> targets;
            std::vector<RadarTargetArray::Ptr> arrays;
            for (const auto &item : mes) {
                // merge measurements by 10 HZ (0.1 s)
                if (targets.empty() ||
                    std::abs(targets.front()->GetTimestamp() - item->GetTimestamp()) < 0.1) {
                    targets.push_back(item->GetTargets().front());
                } else {
                    // compute average time as the timestamp of radar target array
                    double t = 0.0;
                    for (const auto &target : targets) {
                        t += target->GetTimestamp() / static_cast<double>(targets.size());
                    }
                    arrays.push_back(RadarTargetArray::Create(t, targets));
                    targets.clear();
                    targets.push_back(item->GetTargets().front());
                }
            }
            _radarMes.at(topic) = arrays;
        }
    }

    OutputDataStatus();

    AdjustCalibDataSequence();
    AlignTimestamp();
}

void CalibDataManager::AdjustCalibDataSequence() {
    spdlog::info("adjust calibration data sequence...");

    // data sequence adjustment pattern (first step)
    /**
     *       |erased|                           |erased |
     * --------------------------------------------------
     * IMU1: |o o o |o o o o o o o o o o o o o o|o      |
     * IMU2: |      |o o o o o o o o o o o o o o|o o o  |
     * RAD1: |   o o|o o o o o o o o o o o o o o|       |
     * RAD2: | o o o|o o o o o o o o o o o o o o|o o o o|
     * CAM1: | o o o|o o o o o o o o o o o o o o|o      |
     * CAM2: | o o o|o o o o o o o o o o o o o o|o o    |
     * LID1: | o o o|o o o o o o o o o o o o o o|o      |
     * LID2: |   o o|o o o o o o o o o o o o o o|o      |
     * --------------------------------------------------
     *              |--> imuMinTime             |--> imuMaxTime
     */
    auto imuMinTime = std::max_element(_imuMes.begin(), _imuMes.end(),
                                       [](const auto &p1, const auto &p2) {
                                           return p1.second.front()->GetTimestamp() <
                                                  p2.second.front()->GetTimestamp();
                                       })
                          ->second.front()
                          ->GetTimestamp();
    auto imuMaxTime = std::min_element(_imuMes.begin(), _imuMes.end(),
                                       [](const auto &p1, const auto &p2) {
                                           return p1.second.back()->GetTimestamp() <
                                                  p2.second.back()->GetTimestamp();
                                       })
                          ->second.back()
                          ->GetTimestamp();

    _rawStartTimestamp = imuMinTime;
    _rawEndTimestamp = imuMaxTime;

    // data sequence adjustment pattern (second step)
    /**
     *              |--> imuMinTime             |--> imuMaxTime
     * --------------------------------------------------
     * IMU1:        |o|o o o o o o o o o o o o|o|
     * IMU2:        |o|o o o o o o o o o o o o|o|
     * RAD1:        |o|o o o o o o o o o o o o|o|
     * RAD2:        |o|o o o o o o o o o o o o|o|
     * CAM1:        | | |o o o o o o o o o o| | |
     * CAM2:        | | |o o o o o o o o o o| | |
     * LID1:        | | |o o o o o o o o o o| | |
     * LID2:        | | |o o o o o o o o o o| | |
     * --------------------------------------------------
     *                |--> calibSTime         |--> calibETime
     */

    if (Configor::IsRadarIntegrated()) {
        auto radarMinTime = std::max_element(_radarMes.begin(), _radarMes.end(),
                                             [](const auto &p1, const auto &p2) {
                                                 return p1.second.front()->GetTimestamp() <
                                                        p2.second.front()->GetTimestamp();
                                             })
                                ->second.front()
                                ->GetTimestamp();
        auto radarMaxTime = std::min_element(_radarMes.begin(), _radarMes.end(),
                                             [](const auto &p1, const auto &p2) {
                                                 return p1.second.back()->GetTimestamp() <
                                                        p2.second.back()->GetTimestamp();
                                             })
                                ->second.back()
                                ->GetTimestamp();
        _rawStartTimestamp = std::max(_rawStartTimestamp, radarMinTime);
        _rawEndTimestamp = std::min(_rawEndTimestamp, radarMaxTime);
    }

    if (Configor::IsLiDARIntegrated()) {
        auto lidarMinTime = std::max_element(_lidarMes.begin(), _lidarMes.end(),
                                             [](const auto &p1, const auto &p2) {
                                                 return p1.second.front()->GetTimestamp() <
                                                        p2.second.front()->GetTimestamp();
                                             })
                                ->second.front()
                                ->GetTimestamp();
        auto lidarMaxTime = std::min_element(_lidarMes.begin(), _lidarMes.end(),
                                             [](const auto &p1, const auto &p2) {
                                                 return p1.second.back()->GetTimestamp() <
                                                        p2.second.back()->GetTimestamp();
                                             })
                                ->second.back()
                                ->GetTimestamp();
        _rawStartTimestamp = std::max({_rawStartTimestamp, lidarMinTime});
        _rawEndTimestamp = std::min({_rawEndTimestamp, lidarMaxTime});
    }

    if (Configor::IsCameraIntegrated()) {
        auto camMinTime = std::max_element(_camMes.begin(), _camMes.end(),
                                           [](const auto &p1, const auto &p2) {
                                               return p1.second.front()->GetTimestamp() <
                                                      p2.second.front()->GetTimestamp();
                                           })
                              ->second.front()
                              ->GetTimestamp();
        auto camMaxTime = std::min_element(_camMes.begin(), _camMes.end(),
                                           [](const auto &p1, const auto &p2) {
                                               return p1.second.back()->GetTimestamp() <
                                                      p2.second.back()->GetTimestamp();
                                           })
                              ->second.back()
                              ->GetTimestamp();
        _rawStartTimestamp = std::max({_rawStartTimestamp, camMinTime});
        _rawEndTimestamp = std::min({_rawEndTimestamp, camMaxTime});
    }

    for (const auto &[topic, _] : Configor::DataStream::IMUTopics) {
        // remove imu frames that are before the start time stamp
        EraseSeqHeadData(
            _imuMes.at(topic),
            [this](const IMUFrame::Ptr &frame) {
                return frame->GetTimestamp() > _rawStartTimestamp;
            },
            "the imu data is invalid, there is no intersection.");

        // remove imu frames that are after the end time stamp
        EraseSeqTailData(
            _imuMes.at(topic),
            [this](const IMUFrame::Ptr &frame) { return frame->GetTimestamp() < _rawEndTimestamp; },
            "the imu data is invalid, there is no intersection.");
    }

    for (const auto &[topic, _] : Configor::DataStream::RadarTopics) {
        // remove radar frames that are before the start time stamp
        EraseSeqHeadData(
            _radarMes.at(topic),
            [this](const RadarTargetArray::Ptr &frame) {
                return frame->GetTimestamp() >
                       _rawStartTimestamp + 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the radar data is invalid, there is no intersection between imu data and radar data.");

        // remove radar frames that are after the end time stamp
        EraseSeqTailData(
            _radarMes.at(topic),
            [this](const RadarTargetArray::Ptr &frame) {
                return frame->GetTimestamp() <
                       _rawEndTimestamp - 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the radar data is invalid, there is no intersection between imu data and radar data.");
    }

    for (const auto &[topic, _] : Configor::DataStream::LiDARTopics) {
        // remove lidar frames that are before the start time stamp
        EraseSeqHeadData(
            _lidarMes.at(topic),
            [this](const LiDARFrame::Ptr &frame) {
                // different from other sensor, a time offset padding is used here
                // to ensure that the time of first lidar frame (map time) is in spline time range
                return frame->GetTimestamp() >
                       _rawStartTimestamp + 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the lidar data is invalid, there is no intersection between imu data and lidar data.");

        // remove lidar frames that are after the end time stamp
        EraseSeqTailData(
            _lidarMes.at(topic),
            [this](const LiDARFrame::Ptr &frame) {
                return frame->GetTimestamp() <
                       _rawEndTimestamp - 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the lidar data is invalid, there is no intersection between imu data and lidar data.");
    }

    for (const auto &[topic, _] : Configor::DataStream::CameraTopics) {
        // remove camera frames that are before the start time stamp
        EraseSeqHeadData(
            _camMes.at(topic),
            [this](const CameraFrame::Ptr &frame) {
                // different from other sensor, a time offset padding is used here
                // to ensure that the time of first lidar frame (map time) is in spline time range
                return frame->GetTimestamp() >
                       _rawStartTimestamp + 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the camera data is invalid, there is no intersection between imu data and lidar "
            "data.");

        // remove camera frames that are after the end time stamp
        EraseSeqTailData(
            _camMes.at(topic),
            [this](const CameraFrame::Ptr &frame) {
                return frame->GetTimestamp() <
                       _rawEndTimestamp - 2 * Configor::Prior::TimeOffsetPadding;
            },
            "the camera data is invalid, there is no intersection between imu data and lidar "
            "data.");
    }

    OutputDataStatus();
}

void CalibDataManager::AlignTimestamp() {
    spdlog::info("align calibration data timestamp...");

    // all time stamps minus  '_rawStartTimestamp'
    _alignedStartTimestamp = 0.0;
    _alignedEndTimestamp = _rawEndTimestamp - _rawStartTimestamp;
    for (auto &[imuTopic, mes] : _imuMes) {
        for (const auto &frame : mes) {
            frame->SetTimestamp(frame->GetTimestamp() - _rawStartTimestamp);
        }
    }
    for (const auto &[radarTopic, mes] : _radarMes) {
        for (const auto &array : mes) {
            // array
            array->SetTimestamp(array->GetTimestamp() - _rawStartTimestamp);
            // targets
            for (auto &tar : array->GetTargets()) {
                tar->SetTimestamp(tar->GetTimestamp() - _rawStartTimestamp);
            }
        }
    }
    for (const auto &[topic, data] : _lidarMes) {
        for (const auto &item : data) {
            item->SetTimestamp(item->GetTimestamp() - _rawStartTimestamp);
            for (auto &p : *item->GetScan()) {
                p.timestamp -= _rawStartTimestamp;
            }
        }
    }
    for (auto &[camTopic, mes] : _camMes) {
        for (const auto &frame : mes) {
            frame->SetTimestamp(frame->GetTimestamp() - _rawStartTimestamp);
        }
    }
    OutputDataStatus();
}

void CalibDataManager::OutputDataStatus() const {
    spdlog::info("calibration data info:");
    for (const auto &[topic, mes] : _imuMes) {
        spdlog::info(
            "IMU topic: '{}', data size: '{:06}', time span: from '{:+010.5f}' to '{:+010.5f}' (s)",
            topic, mes.size(), mes.front()->GetTimestamp(), mes.back()->GetTimestamp());
    }
    for (const auto &[topic, mes] : _radarMes) {
        spdlog::info(
            "Radar topic: '{}', data size: '{:06}', time span: from '{:+010.5f}' to '{:+010.5f}' "
            "(s)",
            topic, mes.size(), mes.front()->GetTimestamp(), mes.back()->GetTimestamp());
    }
    for (const auto &[topic, mes] : _lidarMes) {
        spdlog::info(
            "LiDAR topic: '{}', data size: '{:06}', time span: from '{:+010.5f}' to '{:+010.5f}' "
            "(s)",
            topic, mes.size(), mes.front()->GetTimestamp(), mes.back()->GetTimestamp());
    }
    for (const auto &[topic, mes] : _camMes) {
        spdlog::info(
            "Camera topic: '{}', data size: '{:06}', time span: from '{:+010.5f}' to '{:+010.5f}' "
            "(s)",
            topic, mes.size(), mes.front()->GetTimestamp(), mes.back()->GetTimestamp());
    }

    spdlog::info("raw start time: '{:+010.5f}' (s), raw end time: '{:+010.5f}' (s)",
                 GetRawStartTimestamp(), GetRawEndTimestamp());
    spdlog::info("aligned start time: '{:+010.5f}' (s), aligned end time: '{:+010.5f}' (s)",
                 GetAlignedStartTimestamp(), GetAlignedEndTimestamp());
    spdlog::info("calib start time: '{:+010.5f}' (s), calib end time: '{:+010.5f}' (s)\n",
                 GetCalibStartTimestamp(), GetCalibEndTimestamp());
}

// -----------
// time access
// -----------

double CalibDataManager::GetRawStartTimestamp() const { return _rawStartTimestamp; }

double CalibDataManager::GetRawEndTimestamp() const { return _rawEndTimestamp; }

double CalibDataManager::GetAlignedStartTimestamp() const {
    // Function 'GetAlignedStartTimestamp' always returns 0.0
    return _alignedStartTimestamp;
}

double CalibDataManager::GetAlignedEndTimestamp() const { return _alignedEndTimestamp; }

double CalibDataManager::GetAlignedTimeRange() const {
    return _alignedEndTimestamp - _alignedStartTimestamp;
}

double CalibDataManager::GetCalibStartTimestamp() const {
    return _alignedStartTimestamp + Configor::Prior::TimeOffsetPadding;
}

double CalibDataManager::GetCalibEndTimestamp() const {
    return _alignedEndTimestamp - Configor::Prior::TimeOffsetPadding;
}

double CalibDataManager::GetCalibTimeRange() const {
    return GetCalibEndTimestamp() - GetCalibStartTimestamp();
}

double CalibDataManager::GetLiDARAvgFrequency() const {
    if (_lidarMes.empty()) {
        return -1.0;
    }
    double hz = 0.0;
    for (const auto &[topic, mes] : _lidarMes) {
        hz += static_cast<double>(mes.size()) /
              (mes.back()->GetTimestamp() - mes.front()->GetTimestamp());
    }
    return hz / static_cast<double>(_lidarMes.size());
}

double CalibDataManager::GetCameraAvgFrequency() const {
    if (_camMes.empty()) {
        return -1.0;
    }
    double hz = 0.0;
    for (const auto &[topic, mes] : _camMes) {
        hz += static_cast<double>(mes.size()) /
              (mes.back()->GetTimestamp() - mes.front()->GetTimestamp());
    }
    return hz / static_cast<double>(_camMes.size());
}

// -----------
// data access
// -----------

const std::map<std::string, std::vector<IMUFrame::Ptr>> &CalibDataManager::GetIMUMeasurements()
    const {
    return _imuMes;
}

const std::vector<IMUFrame::Ptr> &CalibDataManager::GetIMUMeasurements(
    const std::string &imuTopic) const {
    return _imuMes.at(imuTopic);
}

const std::map<std::string, std::vector<RadarTargetArray::Ptr>> &
CalibDataManager::GetRadarMeasurements() const {
    return _radarMes;
}

const std::vector<RadarTargetArray::Ptr> &CalibDataManager::GetRadarMeasurements(
    const std::string &radarTopic) const {
    return _radarMes.at(radarTopic);
}

const std::map<std::string, std::vector<LiDARFrame::Ptr>> &CalibDataManager::GetLiDARMeasurements()
    const {
    return _lidarMes;
}

const std::vector<LiDARFrame::Ptr> &CalibDataManager::GetLiDARMeasurements(
    const std::string &lidarTopic) const {
    return _lidarMes.at(lidarTopic);
}

const std::map<std::string, std::vector<CameraFrame::Ptr>> &
CalibDataManager::GetCameraMeasurements() const {
    return _camMes;
}

const std::vector<CameraFrame::Ptr> &CalibDataManager::GetCameraMeasurements(
    const std::string &camTopic) const {
    return _camMes.at(camTopic);
}

const std::map<std::string, ns_veta::Veta::Ptr> &CalibDataManager::GetSfMData() const {
    return _sfmData;
}

const ns_veta::Veta::Ptr &CalibDataManager::GetSfMData(const std::string &camTopic) const {
    return _sfmData.at(camTopic);
}

void CalibDataManager::SetSfMData(const std::string &camTopic, const ns_veta::Veta::Ptr &veta) {
    _sfmData[camTopic] = veta;
}
}  // namespace ns_ikalibr