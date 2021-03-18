// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "net.h"

#include <algorithm>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <stdio.h>
#include <vector>


static int detect(const cv::Mat& bgr, std::vector<cv::Point2f>& keypoints)
{
    ncnn::Net posenet;

    posenet.opt.use_vulkan_compute = true;

    // the ncnn model https://github.com/nihui/ncnn-assets/tree/master/models
    posenet.load_param("handpose.param");
    posenet.load_model("handpose.bin");

    int w = bgr.cols;
    int h = bgr.rows;

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, ncnn::Mat::PIXEL_BGR2RGB, w, h, 224, 224);

    const float meanVals[3] = { 128.0f, 128.0f,  128.0f };
    const float normVals[3] = { 0.00390625f, 0.00390625f, 0.00390625f };
    in.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex = posenet.create_extractor();

    ex.input("input", in);

    ncnn::Mat out;
    ex.extract("output", out);

    // resolve point from heatmap
    keypoints.clear();
    for (int c = 0; c < out.c; c++)
    {
        ncnn::Mat data = out.channel(c);
        const float *ptr = data.row(0);
        for (size_t j = 0; j < 21; j++)
        {
            float pt_x = ptr[j * 2] * bgr.cols;
            float pt_y = ptr[j * 2 + 1] * bgr.rows;
            cv::Point2f keypt;
            keypt = cv::Point2f(pt_x , pt_y);
            keypoints.push_back(keypt);
        }
    }

    return 0;
}

static void draw_pose(const cv::Mat& bgr, const std::vector<cv::Point2f>& keypoints)
{
    cv::Mat image = bgr.clone();

    cv::Scalar color1(10, 215, 255);
    cv::Scalar color2(255, 115, 55);
    cv::Scalar color3(5, 255, 55);
    cv::Scalar color4(25, 15, 255);
    cv::Scalar color5(225, 15, 55);
    for(size_t j = 0; j < keypoints.size(); j++)
    {
        cv::circle(image, keypoints[j],4,cv::Scalar(255,0,0),-1);
        if (j < 4)
        {
            cv::line(image, keypoints[j], keypoints[j+1], color1, 2, 8);
        }
        if (j < 8 && j > 4)
        {
            cv::line(image, keypoints[j], keypoints[j+1], color2, 2, 8);
        }
        if (j < 12 && j > 8)
        {
            cv::line(image, keypoints[j], keypoints[j+1], color3, 2, 8);
        }
        if (j < 16 && j > 12)
        {
            cv::line(image, keypoints[j], keypoints[j+1], color4, 2, 8);
        }
        if (j < 20 && j > 16)
        {
            cv::line(image, keypoints[j], keypoints[j+1], color5, 2, 8);
        }
    }
    cv::line(image, keypoints[0], keypoints[5], color2, 2, 8);
    cv::line(image, keypoints[0], keypoints[9], color3, 2, 8);
    cv::line(image, keypoints[0], keypoints[13], color4, 2, 8);
    cv::line(image, keypoints[0], keypoints[17], color5, 2, 8);

    cv::imshow("image", image);
    cv::waitKey(0);
}

int main(int argc, char** argv)
{
    //if (argc != 2)
    //{
    //    fprintf(stderr, "Usage: %s [imagepath]\n", argv[0]);
    //    return -1;
    //}

	const char* imagepath = "122.jpg";//argv[1];

    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty())
    {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

    std::vector<cv::Point2f> keypoints;
    detect(m, keypoints);

    draw_pose(m, keypoints);

    return 0;
}
