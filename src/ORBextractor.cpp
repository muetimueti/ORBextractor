#include <string>
#include <iostream>
#include <iterator>
#include <opencv2/imgproc/imgproc.hpp>
#include "include/ORBextractor.h"
#include "include/ORBconstants.h"
#include <unistd.h>
#include <chrono>


#ifndef NDEBUG
#   define D(x) x


#else
#   define D(x)
#endif

#define MYFAST 1
#define TESTFAST 0

#define THREADEDPATCHES 0

#if TESTFAST
#include "include/avx.h"
#endif

#if !MYFAST and !TESTFAST
#include <opencv2/features2d/features2d.hpp>
#endif

namespace ORB_SLAM2
{

float ORBextractor::IntensityCentroidAngle(const uchar* pointer, int step)
{
    //m10 ~ x^1y^0, m01 ~ x^0y^1
    int x, y, m01 = 0, m10 = 0;

    int half_patch = PATCH_SIZE / 2;

    for (x = -half_patch; x <= half_patch; ++x)
    {
        m10 += x * pointer[x];
    }

    for (y = 1; y <= half_patch; ++y)
    {
        int cols = CIRCULAR_ROWS[y];
        int sumY = 0;
        for (x = -cols; x <= cols; ++x)
        {
            int uptown = pointer[x + y*step];
            int downtown = pointer[x - y*step];
            sumY += uptown - downtown;
            m10 += x * (uptown + downtown);
        }
        m01 += y * sumY;
    }

    return cv::fastAtan2((float)m01, (float)m10);
}


ORBextractor::ORBextractor(int _nfeatures, float _scaleFactor, int _nlevels, int _iniThFAST, int _minThFAST):
        nfeatures(_nfeatures), scaleFactor(_scaleFactor), nlevels(_nlevels), iniThFAST(_iniThFAST),
        minThFAST(_minThFAST), stepsChanged(true), levelToDisplay(-1), softSSCThreshold(10), prevDims(-1, -1),
        kptDistribution(Distribution::DistributionMethod::SSC), pixelOffset{}, fast(_iniThFAST, _minThFAST, _nlevels),
        fileInterface(), saveFeatures(false), usePrecomputedFeatures(false), timeVector{}
{
    SetnLevels(_nlevels);

    SetFASTThresholds(_iniThFAST, _minThFAST);

    SetnFeatures(nfeatures);

    const int nPoints = 512;
    const auto tempPattern = (const cv::Point*) bit_pattern_31_;
    std::copy(tempPattern, tempPattern+nPoints, std::back_inserter(pattern));
}

void ORBextractor::SetnFeatures(int n)
{
    if (n < 1 || n > 10000)
        return;

    nfeatures = n;

    float fac = 1.f / scaleFactor;
    float nDesiredFeaturesPerScale = nfeatures * (1.f - fac) / (1.f - (float) pow((double) fac, (double) nlevels));

    int sumFeatures = 0;
    for (int i = 0; i < nlevels - 1; ++i)
    {
        nfeaturesPerLevelVec[i] = myRound(nDesiredFeaturesPerScale);
        sumFeatures += nfeaturesPerLevelVec[i];
        nDesiredFeaturesPerScale *= fac;
    }
    nfeaturesPerLevelVec[nlevels-1] = std::max(nfeatures - sumFeatures, 0);
}

void ORBextractor::SetFASTThresholds(int ini, int min)
{
    if ((ini == iniThFAST && min == minThFAST))
        return;

    iniThFAST = std::min(255, std::max(1, ini));
    minThFAST = std::min(iniThFAST, std::max(1, min));

    fast.SetFASTThresholds(ini, min);
}


void ORBextractor::operator()(cv::InputArray inputImage, cv::InputArray mask,
                              std::vector<knuff::KeyPoint> &resultKeypoints, cv::OutputArray outputDescriptors)
{
    this->operator()(inputImage, mask, resultKeypoints, outputDescriptors, true);
}

/** @overload
 * @param inputImage single channel img-matrix
 * @param mask ignored
 * @param resultKeypoints keypoint vector in which results will be stored
 * @param outputDescriptors matrix in which descriptors will be stored
 * @param distributePerLevel true->distribute kpts per octave, false->distribute kpts per image
 */

void ORBextractor::operator()(cv::InputArray inputImage, cv::InputArray mask,
        std::vector<knuff::KeyPoint> &resultKeypoints, cv::OutputArray outputDescriptors, bool distributePerLevel)
{
    if (usePrecomputedFeatures)
    {
        resultKeypoints = fileInterface.LoadFeatures(loadPath);

        cv::Mat BRIEFdescriptors;
        int nkpts = resultKeypoints.size();
        if (nkpts <= 0)
        {
            outputDescriptors.release();
        }
        else
        {
            outputDescriptors.create(nkpts, 32, CV_8U);
            BRIEFdescriptors = outputDescriptors.getMat();
        }

        fileInterface.LoadDescriptors(loadPath, BRIEFdescriptors, nkpts);
        return;
    }

    std::chrono::high_resolution_clock::time_point funcEntry = std::chrono::high_resolution_clock::now();

    if (inputImage.empty())
        return;

    cv::Mat image = inputImage.getMat();
    message_assert("Image must be single-channel!",image.type() == CV_8UC1);

    if (prevDims.x != image.cols || prevDims.y != image.rows)
        stepsChanged = true;

    ComputeScalePyramid(image);

    SetSteps();

    std::vector<std::vector<knuff::KeyPoint>> allkpts;

    //using namespace std::chrono;
    //high_resolution_clock::time_point t1 = high_resolution_clock::now();

    DivideAndFAST(allkpts, kptDistribution, true, 30, distributePerLevel);

    if (!distributePerLevel)
    {
        ComputeAngles(allkpts);
        int lvl;
        int nkpts = 0;
        for (lvl = 0; lvl < nlevels; ++lvl)
        {
            nkpts += allkpts[lvl].size();

            float size = PATCH_SIZE * scaleFactorVec[lvl];
            float scale = scaleFactorVec[lvl];
            for (auto &kpt : allkpts[lvl])
            {
                kpt.size = size;
                if (lvl)
                    kpt.pt *= scale;
            }
        }

        auto temp = allkpts[0];
        for (lvl = 1; lvl < nlevels; ++lvl)
        {
            temp.insert(temp.end(), allkpts[lvl].begin(), allkpts[lvl].end());
        }
        Distribution::DistributeKeypoints(temp, 0, imagePyramid[0].cols, 0, imagePyramid[0].rows,
                                          nfeatures, kptDistribution, softSSCThreshold);

        for (lvl = 0; lvl < nlevels; ++lvl)
            allkpts[lvl].clear();

        for (auto &kpt : temp)
        {
            allkpts[kpt.octave].emplace_back(kpt);
        }
    }

    if (distributePerLevel)
        ComputeAngles(allkpts);


    cv::Mat BRIEFdescriptors;
    int nkpts = 0;
    for (int lvl = 0; lvl < nlevels; ++lvl)
    {
        nkpts += (int)allkpts[lvl].size();
    }
    if (nkpts <= 0)
    {
        outputDescriptors.release();
    }
    else
    {
        outputDescriptors.create(nkpts, 32, CV_8U);
        BRIEFdescriptors = outputDescriptors.getMat();
    }

    resultKeypoints.clear();
    resultKeypoints.reserve(nkpts);

    ComputeDescriptors(allkpts, BRIEFdescriptors);

    if (distributePerLevel)
    {
        for (int lvl = 0; lvl < nlevels; ++lvl)
        {
            float size = PATCH_SIZE * scaleFactorVec[lvl];
            float scale = scaleFactorVec[lvl];
            for (auto &kpt : allkpts[lvl])
            {
                kpt.size = size;
                if (lvl)
                    kpt.pt *= scale;
            }
        }
    }

    for (int lvl = 0; lvl < nlevels; ++lvl)
    {
        resultKeypoints.insert(resultKeypoints.end(), allkpts[lvl].begin(), allkpts[lvl].end());
    }

    if (saveFeatures)
    {
        fileInterface.SaveFeatures(resultKeypoints);
        fileInterface.SaveDescriptors(BRIEFdescriptors);
    }

    //ensure feature detection always takes 50ms
    unsigned long maxDuration = 50000;
    std::chrono::high_resolution_clock::time_point funcExit = std::chrono::high_resolution_clock::now();
    auto funcDuration = std::chrono::duration_cast<std::chrono::microseconds>(funcExit-funcEntry).count();
    //assert(funcDuration <= maxDuration);
    //if (funcDuration < maxDuration)
    //{
    //    auto sleeptime = maxDuration - funcDuration;
    //    usleep(sleeptime);
    //}
}


void ORBextractor::ComputeAngles(std::vector<std::vector<knuff::KeyPoint>> &allkpts)
{
#pragma omp parallel for
    for (int lvl = 0; lvl < nlevels; ++lvl)
    {
        for (auto &kpt : allkpts[lvl])
        {
            kpt.angle = IntensityCentroidAngle(&imagePyramid[lvl].at<uchar>(myRound(kpt.pt.y), myRound(kpt.pt.x)),
                                               imagePyramid[lvl].step1());
        }
    }
}


void ORBextractor::ComputeDescriptors(std::vector<std::vector<knuff::KeyPoint>> &allkpts, cv::Mat &descriptors)
{
    const auto degToRadFactor = (float)(CV_PI/180.f);
    const cv::Point* p = &pattern[0];

    int current = 0;

    for (int lvl = 0; lvl < nlevels; ++lvl)
    {
        cv::Mat lvlClone = imagePyramid[lvl].clone();
        cv::GaussianBlur(lvlClone, lvlClone, cv::Size(7, 7), 2, 2, cv::BORDER_REFLECT_101);

        const int step = (int)lvlClone.step;


        int i = 0, nkpts = allkpts[lvl].size();
        for (int k = 0; k < nkpts; ++k, ++current)
        {
            const knuff::KeyPoint &kpt = allkpts[lvl][k];
            auto descPointer = descriptors.ptr<uchar>(current);        //ptr to beginning of current descriptor
            const uchar* pixelPointer = &lvlClone.at<uchar>(myRound(kpt.pt.y), myRound(kpt.pt.x));  //ptr to kpt in img

            float angleRad = kpt.angle * degToRadFactor;
            auto a = (float)cos(angleRad), b = (float)sin(angleRad);

            int byte = 0, v0, v1, idx0, idx1;
            for (i = 0; i <= 512; i+=2)
            {
                if (i > 0 && i%16 == 0) //working byte full
                {
                    descPointer[i/16 - 1] = (uchar)byte;  //write current byte to descriptor-mat
                    byte = 0;      //reset working byte
                    if (i == 512)  //break out after writing very last byte, so oob indices aren't accessed
                        break;
                }

                idx0 = myRound(p[i].x*a - p[i].y*b) + myRound(p[i].x*b + p[i].y*a)*step;
                idx1 = myRound(p[i+1].x*a - p[i+1].y*b) + myRound(p[i+1].x*b + p[i+1].y*a)*step;

                v0 = pixelPointer[idx0];
                v1 = pixelPointer[idx1];

                byte |= (v0 < v1) << ((i%16)/2); //write comparison bit to current byte
            }
        }
    }
}


/**
 * @param allkpts KeyPoint vector in which the result will be stored
 * @param mode decides which method to call for keypoint distribution over image, see Distribution.h
 * @param divideImage  true-->divide image into cellSize x cellSize cells, run FAST per cell
 * @param cellSize must be greater than 16 and lesser than min(rows, cols) of smallest image in pyramid
 */
void ORBextractor::DivideAndFAST(std::vector<std::vector<knuff::KeyPoint>> &allkpts,
                                 Distribution::DistributionMethod mode, bool divideImage, int cellSize, bool distributePerLevel)
{
    allkpts.resize(nlevels);

    const int minimumX = EDGE_THRESHOLD - 3, minimumY = minimumX;

    if (!divideImage)
    {
        for (int lvl = 0; lvl < nlevels; ++lvl)
        {
            std::vector<knuff::KeyPoint> levelKpts;
            levelKpts.clear();
            levelKpts.reserve(nfeatures * 10);

            const int maximumX = imagePyramid[lvl].cols - EDGE_THRESHOLD + 3;
            const int maximumY = imagePyramid[lvl].rows - EDGE_THRESHOLD + 3;
#if MYFAST
            fast.FAST(imagePyramid[lvl].rowRange(minimumY, maximumY).colRange(minimumX, maximumX),
                      levelKpts, iniThFAST, lvl);

            if (levelKpts.empty())
            {
                fast.FAST(imagePyramid[lvl].rowRange(minimumY, maximumY).colRange(minimumX, maximumX),
                          levelKpts, minThFAST, lvl);
            }
#else
            cv::FAST(imagePyramid[lvl].rowRange(minimumY, maximumY).colRange(minimumX, maximumX),
                            levelKpts, iniThFAST, true);
            if (levelKpts.empty())
            {
                cv::FAST(imagePyramid[lvl].rowRange(minimumY, maximumY).colRange(minimumX, maximumX),
                         levelKpts, minThFAST, true);
            }
#endif
            if(levelKpts.empty())
                continue;

            allkpts[lvl].reserve(nfeaturesPerLevelVec[lvl]);

            if (distributePerLevel)
                Distribution::DistributeKeypoints(levelKpts, minimumX, maximumX, minimumY, maximumY,
                                                  nfeaturesPerLevelVec[lvl], mode, softSSCThreshold);


            allkpts[lvl] = levelKpts;

            for (auto &kpt : allkpts[lvl])
            {
                kpt.pt.y += minimumY;
                kpt.pt.x += minimumX;
                kpt.octave = lvl;
                //kpt.angle = IntensityCentroidAngle(&imagePyramid[lvl].at<uchar>(
                //        myRound(kpt.pt.x), myRound(kpt.pt.y)), imagePyramid[lvl].step1());
            }
        }
    }
    else
    {
        int c = std::min(imagePyramid[nlevels-1].rows, imagePyramid[nlevels-1].cols);
        assert(cellSize < c && cellSize > 16);

        int minLvl = 0, maxLvl = nlevels;
        if (levelToDisplay != -1)
        {
            minLvl = levelToDisplay;
            maxLvl = minLvl + 1;
        }
        long distributionDuration = 0;

#pragma omp parallel for
        for (int lvl = minLvl; lvl < maxLvl; ++lvl)
        {
            std::vector<knuff::KeyPoint> levelKpts;
            levelKpts.clear();
            levelKpts.reserve(nfeatures*10);

            const int maximumX = imagePyramid[lvl].cols - EDGE_THRESHOLD + 3;
            const int maximumY = imagePyramid[lvl].rows - EDGE_THRESHOLD + 3;
            const float width = maximumX - minimumX;
            const float height = maximumY - minimumY;

            const int npatchesInX = width / cellSize;
            const int npatchesInY = height / cellSize;
            const int patchWidth = ceil(width / npatchesInX);
            const int patchHeight = ceil(height / npatchesInY);

#if THREADEDPATCHES
            int nCells = npatchesInX * npatchesInY;
            int offset[CIRCLE_SIZE];
            for (int i = 0; i < CIRCLE_SIZE; ++i)
            {
                offset[i] = pixelOffset[lvl*CIRCLE_SIZE + i];
            }
            std::vector<std::promise<bool>> promises(nCells);
            int curCell = 0;

            std::vector<std::vector<knuff::KeyPoint>> cellkptvecs;
#endif

            for (int py = 0; py < npatchesInY; ++py)
            {
                float startY = minimumY + py * patchHeight;
                float endY = startY + patchHeight + 6;

                if (startY >= maximumY-3)
                    continue;

                if (endY > maximumY)
                    endY = maximumY;

                for (int px = 0; px < npatchesInX; ++px)
                {
                    float startX = minimumX + px * patchWidth;
                    float endX = startX + patchWidth + 6;

                    if (startX >= maximumX-6)
                        continue;

                    if (endX > maximumX)
                        endX = maximumX;

                    //std::chrono::high_resolution_clock::time_point FASTEntry =
                    //        std::chrono::high_resolution_clock::now();

#if MYFAST
#if THREADEDPATCHES
                    fast.workerPool.PushImg(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                            cellkptvecs[curCell], offset, iniThFAST, lvl, &promises[curCell]);
                    ++curCell;

#else
                    std::vector<knuff::KeyPoint> patchKpts;
                    fast.FAST(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                              patchKpts, iniThFAST, lvl);
                    if (patchKpts.empty())
                    {
                        fast.FAST(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                                  patchKpts, minThFAST, lvl);
                    }
#endif
#elif TESTFAST
                    std::vector<knuff::KeyPoint> patchKpts;
                    blorp::FAST_t<16>(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                                      patchKpts, iniThFAST, true);
                    if (patchKpts.empty())
                        blorp::FAST_t<16>(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                                          patchKpts, minThFAST, true);

#else
                    std::vector<knuff::KeyPoint> patchKpts;
                    cv::FAST(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                            patchKpts, iniThFAST, true, cv::FastFeatureDetector::TYPE_9_16);
                    if (patchKpts.empty())
                    {
                        cv::FAST(imagePyramid[lvl].rowRange(startY, endY).colRange(startX, endX),
                            patchKpts, minThFAST, true, cv::FastFeatureDetector::TYPE_9_16);
                    }
#endif
#if !THREADEDPATCHES
                    if(patchKpts.empty())
                        continue;

                    for (auto &kpt : patchKpts)
                    {
                        kpt.pt.y += py * patchHeight;
                        kpt.pt.x += px * patchWidth;
                        levelKpts.emplace_back(kpt);
                    }
#endif
                }
            }
#if THREADEDPATCHES
            for (int i = 0; i < nCells; ++i)
            {
                promises[i].get_future().wait();
                for (auto &kpt : cellkptvecs[i])
                {
                    kpt.pt.x += ((i%npatchesInX) * patchWidth);
                    kpt.pt.y += ((int)(i/npatchesInX) * patchHeight);
                    levelKpts.emplace_back(kpt);
                }
            }
#endif

            allkpts[lvl].reserve(nfeatures);

            if (distributePerLevel)
            {
                using clk = std::chrono::high_resolution_clock;
                clk::time_point t0 = clk::now();
                Distribution::DistributeKeypoints(levelKpts, minimumX, maximumX, minimumY, maximumY,
                                                  nfeaturesPerLevelVec[lvl], mode, softSSCThreshold);
                clk::time_point t1 = clk::now();
                long duration = std::chrono::duration_cast<std::chrono::microseconds>(t1-t0).count();
                distributionDuration += duration;
            }

            allkpts[lvl] = levelKpts;



            for (auto &kpt : allkpts[lvl])
            {
                kpt.pt.y += minimumY;
                kpt.pt.x += minimumX;
                kpt.octave = lvl;
            }
        }
        timeVector.emplace_back(distributionDuration);
    }
}

void ORBextractor::ComputeScalePyramid(cv::Mat &image)
{
    for (int lvl = 0; lvl < nlevels; ++ lvl)
    {
        int width = (int)myRound(image.cols * invScaleFactorVec[lvl]); // 1.f / getScale(lvl));
        int height = (int)myRound(image.rows * invScaleFactorVec[lvl]); // 1.f / getScale(lvl));

        int doubleEdge = EDGE_THRESHOLD * 2;
        int borderedWidth = width + doubleEdge;
        int borderedHeight = height + doubleEdge;

        //Size sz(width, height);
        //Size borderedSize(borderedWidth, borderedHeight);

        cv::Mat borderedImg(borderedHeight, borderedWidth, image.type());
        cv::Range rowRange(EDGE_THRESHOLD, height + EDGE_THRESHOLD);
        cv::Range colRange(EDGE_THRESHOLD, width + EDGE_THRESHOLD);

        //imagePyramid[lvl] = borderedImg(cv::Rect(EDGE_THRESHOLD, EDGE_THRESHOLD, width, height));
        imagePyramid[lvl] = borderedImg(rowRange, colRange);


        if (lvl)
        {
            cv::resize(imagePyramid[lvl-1], imagePyramid[lvl], cv::Size(width, height), 0, 0, CV_INTER_LINEAR);

            cv::copyMakeBorder(imagePyramid[lvl], borderedImg, EDGE_THRESHOLD, EDGE_THRESHOLD, EDGE_THRESHOLD,
                               EDGE_THRESHOLD, cv::BORDER_REFLECT_101+cv::BORDER_ISOLATED);
        }
        else
        {
            cv::copyMakeBorder(image, borderedImg, EDGE_THRESHOLD, EDGE_THRESHOLD, EDGE_THRESHOLD, EDGE_THRESHOLD,
                               cv::BORDER_REFLECT_101);
        }
    }
}

void ORBextractor::SetSteps()
{
    if (stepsChanged)
    {
        std::vector<int> steps(nlevels);
        for (int lvl = 0; lvl < nlevels; ++lvl)
        {
            steps[lvl] = (int)imagePyramid[lvl].step1();
        }
        fast.SetLevels(nlevels);
        fast.SetStepVector(steps);

        stepsChanged = false;
    }
}

void ORBextractor::SetnLevels(int n)
{
    stepsChanged = true;
    nlevels = std::max(std::min(12, n), 2);
    scaleFactorVec.resize(nlevels);
    invScaleFactorVec.resize(nlevels);
    imagePyramid.resize(nlevels);
    nfeaturesPerLevelVec.resize(nlevels);
    levelSigma2Vec.resize(nlevels);
    invLevelSigma2Vec.resize(nlevels);
    pixelOffset.resize(nlevels * CIRCLE_SIZE);

    SetScaleFactor(scaleFactor);
    SetSteps();

    float fac = 1.f / scaleFactor;
    float nDesiredFeaturesPerScale = nfeatures * (1.f - fac) / (1.f - (float) pow((double) fac, (double) nlevels));
    int sumFeatures = 0;
    for (int i = 0; i < nlevels - 1; ++i)
    {
        nfeaturesPerLevelVec[i] = myRound(nDesiredFeaturesPerScale);
        sumFeatures += nfeaturesPerLevelVec[i];
        nDesiredFeaturesPerScale *= fac;
    }
    nfeaturesPerLevelVec[nlevels-1] = std::max(nfeatures - sumFeatures, 0);
}

void ORBextractor::SetScaleFactor(float s)
{
    stepsChanged = true;
    scaleFactor = std::max(std::min(1.5f, s), 1.001f);
    scaleFactorVec[0] = 1.f;
    invScaleFactorVec[0] = 1.f;

    SetSteps();

    for (int i = 1; i < nlevels; ++i) {
        scaleFactorVec[i] = scaleFactor * scaleFactorVec[i - 1];
        invScaleFactorVec[i] = 1 / scaleFactorVec[i];

        levelSigma2Vec[i] = scaleFactorVec[i] * scaleFactorVec[i];
        invLevelSigma2Vec[i] = 1.f / levelSigma2Vec[i];
    }
}
}