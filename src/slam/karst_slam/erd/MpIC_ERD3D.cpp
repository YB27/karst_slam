#include "karst_slam/erd/MpIC_ERD3D.h"
#include "karst_slam/nrd/MpIC_NRD3D.h"
#include "karst_slam/graph/PoseGraph.h"
//#include "karst_slam/icp/PICP.h"
//#include "karst_slam/icp/IcpResults.h"
//#include "karst_slam/configuration.h"
#include "karst_slam/poses.h"
#include "karst_slam/scanMerging/dataForScanMerging.h"
#include "karst_slam/saveToFiles.h"
#include "karst_slam/Octree.h"

#include <mrpt/obs/CActionCollection.h>
#include <mrpt/obs/CSensoryFrame.h>
#include <mrpt/utils/CConfigFile.h>

using namespace std;
using namespace mrpt::utils;
using namespace mrpt::obs;
using namespace mrpt::math;
using namespace mrpt::poses;
using namespace karst_slam;
using namespace karst_slam::obs;
using namespace karst_slam::graph;
using namespace karst_slam::erd;
using namespace karst_slam::nrd;
using namespace karst_slam::sensors;
using namespace karst_slam::scanMerging;

MpIC_ERD3D::MpIC_ERD3D(const string& configFile) :
    MpIC_ERD3D(CConfigFile(configFile))
{}

MpIC_ERD3D::MpIC_ERD3D(const CConfigFile& configFile) :
    EdgeRegistrationDeciderInterface("MpIC_ERD3D", configFile),
    m_surfaceGP(configFile),
    m_lastScan(nullptr),
    m_previousScan(nullptr)
{
    m_isLearningMeanFunc = configFile.read_bool("GaussianProcess", "learnMeanFunction", false, true);
    m_sphereRadius_densityForKernelTemperature = configFile.read_bool("GaussianProcess", "sphereRadius_densityForKernelTemperature", false, true);
    m_trainingData.setIsLearningMeanFunc(m_isLearningMeanFunc);
    m_surfaceGP.setIsLearningMeanFunc(m_isLearningMeanFunc);
}

MpIC_ERD3D::~MpIC_ERD3D()
{}

bool MpIC_ERD3D::checkInit_() const
{
    return true;
}

void MpIC_ERD3D::loadParams(const CConfigFile& cfg)
{
    MRPT_START
        parent_t::loadParams(cfg);
    MRPT_END
}

void MpIC_ERD3D::printParams()const
{
    MRPT_START
        parent_t::printParams();
    MRPT_END
}

void MpIC_ERD3D::getDescriptiveReport(string* report_str) const
{
    MRPT_START;

    const string report_sep(2, '\n');
    const string header_sep(80, '#');

    // Report on graph
    stringstream class_props_ss;
    class_props_ss << "ICP Goodness-based Registration Procedure Summary: " << endl;
    class_props_ss << header_sep << endl;

    // time and output logging
    const string output_res = this->getLogAsString();

    // merge the individual reports
    report_str->clear();
    parent_t::getDescriptiveReport(report_str);

    *report_str += class_props_ss.str();
    *report_str += report_sep;

    //*report_str += time_res;
    *report_str += report_sep;

    *report_str += output_res;
    *report_str += report_sep;

    MRPT_END;
}

bool MpIC_ERD3D::updateState(const CActionCollectionPtr& action,
                             const CSensoryFramePtr& observations,
                             const CObservationPtr& observation,
                             mrpt::obs::CSensoryFramePtr generatedObservations)

{
    MRPT_START;
    MRPT_UNUSED_PARAM(action);

    // Check possible prior node registration
    checkIfNodeRegistered();

    if (observation)
        updateStateFromObservation(observation);
    else
        updateStateFromObservations(observations);

    // Observations generated by the NRD from the basic observations (typically beam --> scan)
    updateStateFromObservations(generatedObservations);

    checkRegistrationCondition();

    return true;
    MRPT_END;
}

set<TNodeID> MpIC_ERD3D::getNodesToCheckForICP_LC() const
{
    // Get set of nodes within predefined distance for ICP except the previous nodes (only loop closure)
    set<TNodeID> nodes_to_check_ICP; //= {m_prevNodeId};
    m_pose_graph->getNearbyNodesOf(&nodes_to_check_ICP,
        m_prevNodeId,
        m_currNodeId,
        m_params.pICP_max_distance);
    logFmt(LVL_DEBUG,
        "Found * %lu * nodes close to nodeID %lu",
        nodes_to_check_ICP.size(),
        m_currNodeId);

    return nodes_to_check_ICP;
}

surfaceTrainingData MpIC_ERD3D::verticalDataInGlobalFrame(const surfaceTrainingData& prevScanData,
                                                          const surfaceTrainingData& lastScanData,
                                                          const CPose3DPDFGaussian& startFrame_globalPose,
                                                          const CPose3DPDFGaussian& scans_relativePose)
{
    surfaceTrainingData res;
    res.reserve(prevScanData.size() + lastScanData.size());
    surfaceDatum d_global;
    for (const surfaceDatum& d : prevScanData.getData())
    {
        d_global = d;
        startFrame_globalPose.mean.composePoint(d.pt, d_global.pt);
        res.addData(d_global);
    }
    CPose3D secondScan_firstFrame_globalPose = startFrame_globalPose.mean + scans_relativePose.mean;
    for (const surfaceDatum& d : lastScanData.getData())
    {
        d_global = d;
        secondScan_firstFrame_globalPose.composePoint(d.pt, d_global.pt);
        res.addData(d_global);
    }

    return res;
}

void MpIC_ERD3D::processHorizontalSonarData(const CPose3DPDFGaussian& firstFrame_globalPose,
                                            const shared_ptr<fullScanProcessedData>& scan,
                                            vector<vector<pointThetaSimu>>& horizontalSonarPoints_thetaVals,
                                            vector<horizontalSonarMeasure>& horizontalSonarMeasures,
                                            int scan_idx)
{
    double verticalBeamWidth_deg = RAD2DEG(m_params.horizontalSonarParams.verticalBeamWidth);
    double anglePerThetaStep = verticalBeamWidth_deg / (double)m_params.nThetaSamples;
    for (const horizontalSonarData& hd : scan->horizontalScanData)
    {
        const CPose3DPDFGaussian& pose = firstFrame_globalPose + scan->interpolated_drPoses_h[hd.timestamp].pose;
        for (const double r : hd.ranges)
        {
            vector<pointThetaSimu> sampledPtsOnBeam = pointThetaSimu::generatePointsOnArc(sonarMeasure(r, hd.yaw),
                                                                    pose.mean,
                                                                    m_params.horizontalSonarParams.sensorPose.mean,
                                                                    m_params.nThetaSamples, 
                                                                    verticalBeamWidth_deg,
                                                                    anglePerThetaStep,
                                                                    m_params.horizontalSonarParams.sphericalLocalCovariance);

            horizontalSonarPoints_thetaVals.push_back(move(sampledPtsOnBeam));


            horizontalSonarMeasures.push_back(horizontalSonarMeasure(pose,
                                                                     hd.timestamp,
                                                                     r,
                                                                     hd.yaw,
                                                                     scan_idx)
                                             );
        }
    }
}

void MpIC_ERD3D::saveHorizontalSonarEstimatedData(const scanMergingResults& res)
{
    string fileName = "horizontalSonarEstimatedData.txt";
    ofstream file(fileName);
    if (file.is_open())
    {
        file << "# scanIdx, rho, yaw, alpha, beta, timeStamp, arcIdx" << endl;

        int nArcs = m_lastHorizontalSonarPoints_thetaVals.size();
        for (int arcIdx = 0; arcIdx < nArcs; arcIdx++)
        {
            const horizontalSonarMeasure& meas = m_lastHorizontalSonarMeasures[arcIdx];
            if (res.distribution.find(arcIdx) != res.distribution.end())
            {
                const vector<distributionTheta>& curArcDistributions = res.distribution.at(arcIdx);
                for (const distributionTheta& dist : curArcDistributions)
                {
                    file << meas.scanIdx << ","
                        << meas.rho << ","
                        << meas.yaw << ","
                        << dist.alpha << ","
                        << dist.beta << ","
                        << meas.timeStamp << ","
                        << arcIdx << endl;
                }
            }
            // If no distribution found, add a uniform distribution (ie alpha=beta=1)
            else
            {
                file << meas.scanIdx << ","
                    << meas.rho << ","
                    << meas.yaw << ","
                    << 1 /*alpha*/ << ","
                    << 1 /*beta*/ << ","
                    << meas.timeStamp << ","
                    << arcIdx << endl;
            }
        }

        file.close();
    }
}

void MpIC_ERD3D::saveBodyPoses(const vector<CPose3DPDFGaussian>& bodyPoses,
                               const vector<uint64_t>& timestamps,
                               const string& fileName) const
{
    SAVE_START(fileName)
        file << "#timestamp, pose" << endl;
    for (size_t i = 0; i < bodyPoses.size(); i++)
    {
        file << timestamps[i] << ",";
        utils::writePoseToFile(bodyPoses[i], file);
        file << endl;
    }
    SAVE_END(fileName)
}

void MpIC_ERD3D::saveRefFramesTimestamps(uint64_t firstScan_refFrame_timestamp,
                                         uint64_t secondScan_refFrame_timestamp) const 
{   
    SAVE_START("refFrames_timestamps.txt")
        file << firstScan_refFrame_timestamp << endl;
        file << secondScan_refFrame_timestamp << endl;
    SAVE_END("refFrames_timestamps.txt")
}

void MpIC_ERD3D::saveToFiles_forPythonScripts(const surfaceValidationData& horizontalData,
                                              const vector<CPose3DPDFGaussian>& bodyPoses,
                                              const vector<uint64_t>& timestamps,
                                              uint64_t firstScan_refFrame_timestamp,
                                              uint64_t secondScan_refFrame_timestamp) const
{
    m_trainingData.save("surfaceTrainingData.txt");
    m_trainingData.saveAbscissa("curvilinearAbscissa.txt");
    horizontalData.save("surfaceValidationData.txt");
    saveRefFramesTimestamps(firstScan_refFrame_timestamp, secondScan_refFrame_timestamp);
}

void MpIC_ERD3D::scanDataFromLocalToGlobalFrame(const CPose3DPDFGaussian& refFrame_globalPose,
                                                const shared_ptr<fullScanProcessedData>& scanData,
                                                vector<CPointPDFGaussian>& verticalPoints_globalFrame,
                                                vector<CPose3DPDFGaussian>& bodyPosesAtPointMeasurement_globalFrame,
                                                vector<uint64_t>& bodyPosesAtPointMeasurement_ts)
{
    CPointPDFGaussian point;
    for (const surfaceDatum& d : scanData->verticalScanData.getData())
    {
        verticalPoints_globalFrame.push_back(CPointPDFGaussian(refFrame_globalPose.mean + CPointPDFGaussian(CPoint3D(d.pt)).mean)); // ellipticCylinder do not use covariance
        bodyPosesAtPointMeasurement_globalFrame.push_back(refFrame_globalPose + scanData->interpolated_drPoses_v[d.timeStamp].pose);
        bodyPosesAtPointMeasurement_ts.push_back(d.timeStamp);
    }
    for(const horizontalSonarData& d : scanData->horizontalScanData)
    {
        bodyPosesAtPointMeasurement_globalFrame.push_back(refFrame_globalPose + scanData->interpolated_drPoses_h[d.timestamp].pose);
        bodyPosesAtPointMeasurement_ts.push_back(d.timestamp);
    }
}

void MpIC_ERD3D::pairScanDataInGlobalFrame(const CPose3DPDFGaussian& firstScanFirstFrame_globalPose,
                                           const CPose3DPDFGaussian& secondScan_firstFrame_globalPose,
                                           vector<CPointPDFGaussian>& verticalPoints_globalFrame,
                                           vector<CPose3DPDFGaussian>& bodyPosesAtPointMeasurement_globalFrame,
                                           vector<uint64_t>& bodyPosesAtPointMeasurement_ts)
{
    verticalPoints_globalFrame.reserve(m_previousScan->verticalScanData.size() +
                                       m_lastScan->verticalScanData.size());
    bodyPosesAtPointMeasurement_globalFrame.reserve(verticalPoints_globalFrame.size());
    bodyPosesAtPointMeasurement_ts.reserve(verticalPoints_globalFrame.size());

    scanDataFromLocalToGlobalFrame(firstScanFirstFrame_globalPose,
                                   m_previousScan, 
                                   verticalPoints_globalFrame,
                                   bodyPosesAtPointMeasurement_globalFrame,
                                   bodyPosesAtPointMeasurement_ts);

    saveBodyPoses(bodyPosesAtPointMeasurement_globalFrame, 
                  bodyPosesAtPointMeasurement_ts, 
                  "bodyPoses_firstScan.txt");

    vector<CPose3DPDFGaussian> lastScan_bodyPosesAtPointMeasurement;
    vector<uint64_t> lastScan_bodyPosesAtPointMeasurement_ts;
    scanDataFromLocalToGlobalFrame(secondScan_firstFrame_globalPose,
                                   m_lastScan,
                                   verticalPoints_globalFrame,
                                   lastScan_bodyPosesAtPointMeasurement,
                                   lastScan_bodyPosesAtPointMeasurement_ts);

    saveBodyPoses(lastScan_bodyPosesAtPointMeasurement,
                  lastScan_bodyPosesAtPointMeasurement_ts,
                  "bodyPoses_secondScan.txt");

    bodyPosesAtPointMeasurement_globalFrame.insert(bodyPosesAtPointMeasurement_globalFrame.end(),
                                                   make_move_iterator(lastScan_bodyPosesAtPointMeasurement.begin()),
                                                   make_move_iterator(lastScan_bodyPosesAtPointMeasurement.end()));

    bodyPosesAtPointMeasurement_ts.insert(bodyPosesAtPointMeasurement_ts.end(),
                                          make_move_iterator(lastScan_bodyPosesAtPointMeasurement_ts.begin()),
                                          make_move_iterator(lastScan_bodyPosesAtPointMeasurement_ts.end()));              
}

void MpIC_ERD3D::edgeWithPreviousScan()
{
    //----------------------------------------------------------------
    // Get the global Pose of the first and second scan starting frame
    //----------------------------------------------------------------
    const CPose3DPDFGaussian& firstScanFirstFrame_globalPose = m_previousScan->startFrame_globalPose;
    CPose3DPDFGaussian secondScan_firstFrame_globalPose = firstScanFirstFrame_globalPose + m_lastScan->startPoseRelativeToPrevScanFirstFrame;

    //----------------------------------------------------------------
    // Initialize / compute scan pair data in global frame
    //----------------------------------------------------------------
    vector<CPointPDFGaussian> lastScanPair_verticalPoints;
    lastScanPair_verticalPoints.reserve(m_previousScan->verticalScanData.size() +
        m_lastScan->verticalScanData.size());
    vector<CPose3DPDFGaussian> lastScanPair_bodyPosesAtPointMeasurement;
    vector<uint64_t> lastScanPair_bodyPosesAtPointMeasurement_ts;
    lastScanPair_bodyPosesAtPointMeasurement.reserve(lastScanPair_verticalPoints.size());
    lastScanPair_bodyPosesAtPointMeasurement_ts.reserve(lastScanPair_verticalPoints.size());

    pairScanDataInGlobalFrame(firstScanFirstFrame_globalPose,
                              secondScan_firstFrame_globalPose,
                              lastScanPair_verticalPoints,
                              lastScanPair_bodyPosesAtPointMeasurement,
                              lastScanPair_bodyPosesAtPointMeasurement_ts);

    //-------------------------------------------------------------------------------------
    // Elliptic cylinder fitting 
    //-------------------------------------------------------------------------------------
    // Need a vector<CPose3D> -> convert
    vector<CPose3D> lastScanPair_bodyPosesAtPointMeasurement_mean = getVectorOfMeans(lastScanPair_bodyPosesAtPointMeasurement);
    m_lastEllipticCylinder.fit(lastScanPair_verticalPoints,
                               m_params.verticalSonarParams.sensorPose.mean,
                               lastScanPair_bodyPosesAtPointMeasurement_mean);

    //-------------------------------------------------------------------------------------
    // Data for training the surface GP
    //-------------------------------------------------------------------------------------
    // Express data in the global frame
    double min_abscissa, max_abscissa;
    m_trainingData = verticalDataInGlobalFrame(m_previousScan->verticalScanData,
                                               m_lastScan->verticalScanData,
                                               m_previousScan->startFrame_globalPose,
                                               m_lastScan->startPoseRelativeToPrevScanFirstFrame);
    m_trainingData.setIsLearningMeanFunc(m_isLearningMeanFunc);
    m_trainingData.computeAbscissa(m_lastEllipticCylinder, min_abscissa, max_abscissa);

    //-------------------------------------------------------------------------------------
    // Data from the horizontal sonar to be estimated with the estimated surface
    //-------------------------------------------------------------------------------------
    vector<vector<pointThetaSimu>> horizontalSonarPoints_thetaVals;
    vector<horizontalSonarMeasure> horizontalSonarMeasures;
    processHorizontalSonarData(firstScanFirstFrame_globalPose,
                               m_previousScan,
                               horizontalSonarPoints_thetaVals,
                               horizontalSonarMeasures,
                               0  /*scan_idx*/);
    processHorizontalSonarData(secondScan_firstFrame_globalPose,
                               m_lastScan,
                               horizontalSonarPoints_thetaVals,
                               horizontalSonarMeasures,
                               1 /*scan_idx*/);
    m_lastHorizontalSonarPoints_thetaVals = move(horizontalSonarPoints_thetaVals);
    m_lastHorizontalSonarMeasures         = move(horizontalSonarMeasures);

    surfaceValidationData horizontalData = surfaceValidationData::generateFromHorizontalSonar_guide(m_lastHorizontalSonarPoints_thetaVals,
                                                                                                    m_lastEllipticCylinder,
                                                                                                    m_isLearningMeanFunc,
                                                                                                    min_abscissa,
                                                                                                    max_abscissa);

    // Save to file (to be used after by python scripts)
    // FIX ME : pythons scripts were used for the prototyping / testing phase
    //  ---> Implement in C++
    saveToFiles_forPythonScripts(horizontalData,
                                 lastScanPair_bodyPosesAtPointMeasurement,
                                 lastScanPair_bodyPosesAtPointMeasurement_ts,
                                 m_previousScan->refFrame_timestamp,
                                 m_lastScan->refFrame_timestamp);

    //-------------------------------------------------------------------------------------
    // Surface estimation
    //-------------------------------------------------------------------------------------
    map<uint64_t, trajectoryPose<CPose3DPDFGaussian>> poses_v; 
    for (const auto& pair : m_previousScan->interpolated_drPoses_v)
        poses_v[pair.first] = firstScanFirstFrame_globalPose + pair.second.pose;
    for (const auto& pair : m_lastScan->interpolated_drPoses_v)
        poses_v[pair.first] = secondScan_firstFrame_globalPose + pair.second.pose;

    m_lastSuccessiveScanMergingResults = m_surfaceGP.estimateDistributions(poses_v,
                                                                           m_params.verticalSonarParams.sensorPose.mean,
                                                                           m_lastEllipticCylinder);

    // Save horizontal data
    // TODO : Quite a mess ... Need to see what is needed and avoid duplicates 
    saveHorizontalSonarEstimatedData(m_lastSuccessiveScanMergingResults);

    //-------------------------------------------------------------------------------------
    // Vertical Points Octree --> Kernel temperature based on points density
    // RESEARCH TESTING ---> NO GUARANTEE (can be commented out but take care that this
    // data is not used elsewhere eg in the python scripts in particular)
    //-------------------------------------------------------------------------------------
    vector<TPoint3D> verticalPts;
    verticalPts.reserve(lastScanPair_verticalPoints.size());
    for(const CPointPDFGaussian& point_pdf : lastScanPair_verticalPoints)
        verticalPts.push_back(point_pdf.mean);
    
    Octree verticalPtsOctree(verticalPts);
    vector<int> densityScore;
    densityScore.reserve(verticalPts.size());
    for(const TPoint3D& pt : verticalPts)
    {
        vector<TPoint3D> res;
        verticalPtsOctree.getPointsWithinSphere(pt, m_sphereRadius_densityForKernelTemperature, res);
        densityScore.push_back(res.size());
    }
    SAVE_START("densityVerticalPts.txt")
    file << "#density" << endl;
    for (int d : densityScore)
        file << d << endl;
    SAVE_END("densityVerticalPts.txt")

    //-------------------------------------------------------------------------------------
    // MpIC scan Matching
    //-------------------------------------------------------------------------------------
    string command = "python " + m_params.pythonScript_mpic + " . " + "all";
    cout << "command : " << command << endl;
    system(command.c_str());
}

void MpIC_ERD3D::checkRegistrationCondition()
{
    MRPT_START;

    // Reset the loop_closure flag and run registration
    m_just_inserted_lc = false;

    // Edge registration procedure - same for both rawlog formats
    if (m_newNodeRegistered && m_previousScan != nullptr && m_lastScan != nullptr)
    {
        // ICP with previous scan
        edgeWithPreviousScan();


        // TODO : When MpIC will be working correctly on successive scans (eg. take into account censored sonar data), 
        // implement loopClosureEdgeWithScan() ! 

        // Loop closure
        //set<TNodeID> nodes_to_check_ICP_LC = getNodesToCheckForICP_LC();

        //for(TNodeID nodeId : nodes_to_check_ICP_LC)
        //    loopClosureEdgeWithScan();
    }
    MRPT_END;
}
