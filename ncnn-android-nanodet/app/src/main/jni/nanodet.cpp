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

#include "nanodet.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "cpu.h"


static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

    //     #pragma omp parallel sections
    {
        //         #pragma omp section
        {
            if (left < j) qsort_descent_inplace(faceobjects, left, j);
        }
        //         #pragma omp section
        {
            if (i < right) qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.width * faceobjects[i].rect.height;
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static void generate_proposals(const ncnn::Mat& cls_pred, const ncnn::Mat& dis_pred, int stride, const ncnn::Mat& in_pad, float prob_threshold, std::vector<Object>& objects)
{

    const int num_grid = cls_pred.h;

    int num_grid_x;
    int num_grid_y;
    if (in_pad.w > in_pad.h)
    {
        num_grid_x = in_pad.w / stride;
        num_grid_y = num_grid / num_grid_x;
    }
    else
    {
        num_grid_y = in_pad.h / stride;
        num_grid_x = num_grid / num_grid_y;
    }

    const int num_class = cls_pred.w;
    const int reg_max_1 = dis_pred.w / 4;
    //__android_log_print(ANDROID_LOG_WARN, "ncnn","cls_pred h %d, w %d",cls_pred.h,cls_pred.w);
    //__android_log_print(ANDROID_LOG_WARN, "ncnn","%d,%d,%d,%d",num_grid_x,num_grid_y,num_class,reg_max_1);
    for (int i = 0; i < num_grid_y; i++)
    {
        for (int j = 0; j < num_grid_x; j++)
        {
            const int idx = i * num_grid_x + j;

            const float* scores = cls_pred.row(idx);

            // find label with max score
            int label = -1;
            float score = -FLT_MAX;
            for (int k = 0; k < num_class; k++)
            {
                if (scores[k] > score)
                {
                    label = k;
                    score = scores[k];
                }
            }

            if (score >= prob_threshold)
            {
                ncnn::Mat bbox_pred(reg_max_1, 4, (void*)dis_pred.row(idx));
                {
                    ncnn::Layer* softmax = ncnn::create_layer("Softmax");

                    ncnn::ParamDict pd;
                    pd.set(0, 1); // axis
                    pd.set(1, 1);
                    softmax->load_param(pd);

                    ncnn::Option opt;
                    opt.num_threads = 1;
                    opt.use_packing_layout = false;

                    softmax->create_pipeline(opt);

                    softmax->forward_inplace(bbox_pred, opt);

                    softmax->destroy_pipeline(opt);

                    delete softmax;
                }

                float pred_ltrb[4];
                for (int k = 0; k < 4; k++)
                {
                    float dis = 0.f;
                    const float* dis_after_sm = bbox_pred.row(k);
                    for (int l = 0; l < reg_max_1; l++)
                    {
                        dis += l * dis_after_sm[l];
                    }

                    pred_ltrb[k] = dis * stride;
                }

                float pb_cx = (j + 0.5f) * stride;
                float pb_cy = (i + 0.5f) * stride;

                float x0 = pb_cx - pred_ltrb[0];
                float y0 = pb_cy - pred_ltrb[1];
                float x1 = pb_cx + pred_ltrb[2];
                float y1 = pb_cy + pred_ltrb[3];

                Object obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = x1 - x0;
                obj.rect.height = y1 - y0;
                obj.label = label;
                obj.prob = score;

                objects.push_back(obj);
            }
        }
    }
}

NanoDet::NanoDet()
{
    blob_pool_allocator.set_size_compare_ratio(0.f);
    workspace_pool_allocator.set_size_compare_ratio(0.f);
}

int NanoDet::load(const char* modeltype, int _target_size, const float* _mean_vals, const float* _norm_vals, bool use_gpu)
{
    nanodet.clear();
    blob_pool_allocator.clear();
    workspace_pool_allocator.clear();

    ncnn::set_cpu_powersave(2);
    ncnn::set_omp_num_threads(ncnn::get_big_cpu_count());

    nanodet.opt = ncnn::Option();

#if NCNN_VULKAN
    nanodet.opt.use_vulkan_compute = use_gpu;
#endif

    nanodet.opt.num_threads = ncnn::get_big_cpu_count();
    nanodet.opt.blob_allocator = &blob_pool_allocator;
    nanodet.opt.workspace_allocator = &workspace_pool_allocator;

    char parampath[256];
    char modelpath[256];
    sprintf(parampath, "nanodet-%s.param", modeltype);
    sprintf(modelpath, "nanodet-%s.bin", modeltype);

    nanodet.load_param(parampath);
    nanodet.load_model(modelpath);

    target_size = _target_size;
    mean_vals[0] = _mean_vals[0];
    mean_vals[1] = _mean_vals[1];
    mean_vals[2] = _mean_vals[2];
    norm_vals[0] = _norm_vals[0];
    norm_vals[1] = _norm_vals[1];
    norm_vals[2] = _norm_vals[2];

    return 0;
}

int NanoDet::load(AAssetManager* mgr, const char* modeltype, int _target_size, const float* _mean_vals, const float* _norm_vals, bool use_gpu)
{
    __android_log_print(ANDROID_LOG_WARN, "ncnn", "load %s", modeltype);
    nanodet.clear();
    blob_pool_allocator.clear();
    workspace_pool_allocator.clear();

    ncnn::set_cpu_powersave(2);
    ncnn::set_omp_num_threads(ncnn::get_big_cpu_count());

    nanodet.opt = ncnn::Option();
    handpt.opt = ncnn::Option();

#if NCNN_VULKAN
    nanodet.opt.use_vulkan_compute = use_gpu;
    handpt.opt.use_vulkan_compute = use_gpu;
#endif

    nanodet.opt.num_threads = ncnn::get_big_cpu_count();
    nanodet.opt.blob_allocator = &blob_pool_allocator;
    nanodet.opt.workspace_allocator = &workspace_pool_allocator;

    handpt.opt.num_threads = ncnn::get_big_cpu_count();
    handpt.opt.blob_allocator = &blob_pool_allocator;
    handpt.opt.workspace_allocator = &workspace_pool_allocator;

    char parampath[256];
    char modelpath[256];
    sprintf(parampath, "nanodet-%s.param", modeltype);
    sprintf(modelpath, "nanodet-%s.bin", modeltype);
    //__android_log_print(ANDROID_LOG_WARN, "ncnn", "load %s,%s", parampath,modelpath);
    nanodet.load_param(mgr, parampath);
    nanodet.load_model(mgr, modelpath);
    handpt.load_param(mgr,"handpose.param");
    handpt.load_model(mgr,"handpose.bin");



    target_size = _target_size;
    mean_vals[0] = _mean_vals[0];
    mean_vals[1] = _mean_vals[1];
    mean_vals[2] = _mean_vals[2];
    norm_vals[0] = _norm_vals[0];
    norm_vals[1] = _norm_vals[1];
    norm_vals[2] = _norm_vals[2];

    return 0;
}

int NanoDet::detect(const cv::Mat& rgb, std::vector<Object>& objects, float prob_threshold, float nms_threshold)
{
    int width = rgb.cols;
    int height = rgb.rows;

    // pad to multiple of 32
    int w = width;
    int h = height;
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

#if defined(_WIN32)
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB, width, height, w, h);
#elif defined(__ANDROID__)
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgb.data, ncnn::Mat::PIXEL_RGB2BGR, width, height, w, h);
#endif
    // pad to target_size rectangle
    int wpad = target_size - w;//(w + 31) / 32 * 32 - w;
    int hpad = target_size - h;//(h + 31) / 32 * 32 - h;
    
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 0.f);

    in_pad.substract_mean_normalize(mean_vals, norm_vals);

    ncnn::Extractor ex = nanodet.create_extractor();
    //__android_log_print(ANDROID_LOG_WARN, "ncnn","input w:%d,h:%d",in_pad.w,in_pad.h);
    ex.input("input.1", in_pad);

    std::vector<Object> proposals;

    // stride 8
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_8", cls_pred);
        ex.extract("dis_pred_stride_8", dis_pred);

        std::vector<Object> objects8;
        generate_proposals(cls_pred, dis_pred, 8, in_pad, prob_threshold, objects8);

        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    // stride 16
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_16", cls_pred);
        ex.extract("dis_pred_stride_16", dis_pred);

        std::vector<Object> objects16;
        generate_proposals(cls_pred, dis_pred, 16, in_pad, prob_threshold, objects16);

        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    // stride 32
    {
        ncnn::Mat cls_pred;
        ncnn::Mat dis_pred;
        ex.extract("cls_pred_stride_32", cls_pred);
        ex.extract("dis_pred_stride_32", dis_pred);

        std::vector<Object> objects32;
        generate_proposals(cls_pred, dis_pred, 32, in_pad, prob_threshold, objects32);

        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }

    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    objects.resize(count);

    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x - (wpad / 2)) / scale;
        float y0 = (objects[i].rect.y - (hpad / 2)) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width - (wpad / 2)) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(width - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(height - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(width - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(height - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;

        {
            int xx = x0;
            int yy = y0;
            int ww = x1-x0;
            int hh = y1-y0;
            //__android_log_print(ANDROID_LOG_WARN, "ncnn","rect :%f,%f,%f,%f",objects[i].rect.x,objects[i].rect.y,objects[i].rect.width,objects[i].rect.height);

            //detect pts
            cv::Mat roi_image = rgb(cv::Rect(xx,yy,ww,hh)).clone();
            ncnn::Mat ncnn_in = ncnn::Mat::from_pixels_resize(roi_image.data,ncnn::Mat::PIXEL_RGB2BGR, roi_image.cols, roi_image.rows,224,224);
            ncnn_in.substract_mean_normalize(meanVals, normVals);
            ncnn::Extractor ex1 = handpt.create_extractor();
            ex1.input("input", ncnn_in);
            ncnn::Mat ncnn_out;
            ex1.extract("output", ncnn_out);
            for (int c = 0; c < ncnn_out.c; c++)
            {
                ncnn::Mat data = ncnn_out.channel(c);
                const float *ptr = data.row(0);
                for (size_t j = 0; j < 21; j++)
                {
                    float pt_x = ptr[j * 2] * roi_image.cols;
                    float pt_y = ptr[j * 2 + 1] * roi_image.rows;
                    objects[i].pts.push_back(cv::Point2f(pt_x + objects[i].rect.x, pt_y + objects[i].rect.y));

                }
            }
        }

    }

    // sort objects by area
    struct
    {
        bool operator()(const Object& a, const Object& b) const
        {
            return a.rect.area() > b.rect.area();
        }
    } objects_area_greater;
    std::sort(objects.begin(), objects.end(), objects_area_greater);

    return 0;
}

int NanoDet::draw(cv::Mat& rgb, const std::vector<Object>& objects)
{
    static const char* class_names[] = {
        "person", "hand", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };

    static const unsigned char colors[19][3] = {
        { 54,  67, 244},
        { 99,  30, 233},
        {176,  39, 156},
        {183,  58, 103},
        {181,  81,  63},
        {243, 150,  33},
        {244, 169,   3},
        {212, 188,   0},
        {136, 150,   0},
        { 80, 175,  76},
        { 74, 195, 139},
        { 57, 220, 205},
        { 59, 235, 255},
        {  7, 193, 255},
        {  0, 152, 255},
        { 34,  87, 255},
        { 72,  85, 121},
        {158, 158, 158},
        {139, 125,  96}
    };

    int color_index = 0;

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

//         fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
//                 obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        const unsigned char* color = colors[color_index % 19];
        color_index++;

        cv::Scalar cc(color[0], color[1], color[2]);

        cv::rectangle(rgb, obj.rect, cc, 2);

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > rgb.cols)
            x = rgb.cols - label_size.width;

        cv::rectangle(rgb, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)), cc, -1);

        cv::Scalar textcc = (color[0] + color[1] + color[2] >= 381) ? cv::Scalar(0, 0, 0) : cv::Scalar(255, 255, 255);

        cv::putText(rgb, text, cv::Point(x, y + label_size.height), cv::FONT_HERSHEY_SIMPLEX, 0.5, textcc, 1);


        {
            cv::Scalar color1(10, 215, 255);
            cv::Scalar color2(255, 115, 55);
            cv::Scalar color3(5, 255, 55);
            cv::Scalar color4(25, 15, 255);
            cv::Scalar color5(225, 15, 55);
            for(size_t j = 0; j < obj.pts.size(); j++)
            {
                cv::circle(rgb, obj.pts[j],4,cv::Scalar(255,0,0),-1);
                if (j < 4)
                {
                    cv::line(rgb, obj.pts[j], obj.pts[j+1], color1, 2, 8);
                }
                if (j < 8 && j > 4)
                {
                    cv::line(rgb, obj.pts[j], obj.pts[j+1], color2, 2, 8);
                }
                if (j < 12 && j > 8)
                {
                    cv::line(rgb, obj.pts[j], obj.pts[j+1], color3, 2, 8);
                }
                if (j < 16 && j > 12)
                {
                    cv::line(rgb, obj.pts[j], obj.pts[j+1], color4, 2, 8);
                }
                if (j < 20 && j > 16)
                {
                    cv::line(rgb, obj.pts[j], obj.pts[j+1], color5, 2, 8);
                }
            }
            cv::line(rgb, obj.pts[0], obj.pts[5], color2, 2, 8);
            cv::line(rgb, obj.pts[0], obj.pts[9], color3, 2, 8);
            cv::line(rgb, obj.pts[0], obj.pts[13], color4, 2, 8);
            cv::line(rgb, obj.pts[0], obj.pts[17], color5, 2, 8);
        }


    }

    return 0;
}
