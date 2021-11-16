// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
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

#include "landmark.h"

#include <string.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "cpu.h"



int LandmarkDetect::load(AAssetManager* mgr, const char* modeltype, bool use_gpu)
{
    landmark.clear();

    ncnn::set_cpu_powersave(2);
    ncnn::set_omp_num_threads(ncnn::get_big_cpu_count());

    landmark.opt = ncnn::Option();

#if NCNN_VULKAN
    landmark.opt.use_vulkan_compute = use_gpu;
#endif

    landmark.opt.num_threads = ncnn::get_big_cpu_count();

    char parampath[256];
    char modelpath[256];
    sprintf(parampath, "%s.param", modeltype);
    sprintf(modelpath, "%s.bin", modeltype);

    landmark.load_param(mgr, parampath);
    landmark.load_model(mgr, modelpath);


    return 0;
}

float LandmarkDetect::detect(const cv::Mat& rgb,const cv::Rect& box, std::vector<cv::Point2f> &landmarks)
{
    cv::Mat input = rgb(box).clone();
    int target_size = 224;
    int w = input.cols;
    int h = input.rows;
    float scale = 1.f;
    if (w > h)
    {
        scale = (float)target_size / w;
        w = target_size;
        h = h * scale;
    }
    else
    {
        scale = (float)target_size / h;
        h = target_size;
        w = w * scale;
    }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(input.data, ncnn::Mat::PIXEL_RGB, input.cols, input.rows, w, h);
    int wpad = target_size - w;
    int hpad = target_size - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 0.f);

    const float norm_vals[3] = { 1 / 255.f, 1 / 255.f, 1 / 255.f };
    in_pad.substract_mean_normalize(NULL, norm_vals);
    ncnn::Mat points,score;
    {
        ncnn::Extractor ex = landmark.create_extractor();
        ex.input("input", in_pad);
        ex.extract("points", points);
        ex.extract("score",score);
    }

    float* points_data = (float*)points.data;
    float* score_data = (float*)score.data;
    for (int i = 0; i < 21; i++)
    {
        cv::Point2f pt;
        pt.x = (points_data[i * 3] - (wpad / 2)) / scale+(float)box.x;
        pt.y = (points_data[i * 3 + 1]- (hpad / 2)) / scale+(float)box.y;

        landmarks.push_back(pt);
    }
    return score_data[0];
}

