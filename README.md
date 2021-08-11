## ncnn_nanodet_hand  
1.hand detect:用nanode-m训练了个hand detect模型,  
2.hand pose:用CMU的数据集训练了个ghostnet作为backbone模仿pfld的handpose模型  
3.推理:handpose.cpp单独检测pose，nanodet_hand.cpp单独检测手.参照该代码可以无痛集成到ncnn-android-nanodet中哦.  
4.在nihui大佬的ncnn-android-nanodet项目基础上,去掉了其他模型只保留了检测手和手势的.  
PS:由于数据集质量不理想，且自己水平有限所以实际效果可能较差，自己做着玩的.  
## TODO：  
__1.scrfd/yolov5/yolox hand detection__  
__2.better performance handpose model__  
hand detection dataset: https://pan.baidu.com/s/1wfCdklwfGk_utM_za4OKKg 提取码: cx4g  
![image](https://github.com/FeiGeChuanShu/ncnn_nanodet_hand/blob/main/result.gif)  
Android上的效果可以看一下视频，总之ncnn和nanodet棒极了！    
https://www.bilibili.com/video/BV1Uh411Q7Rq  
Reference：  
https://github.com/nihui/ncnn-android-nanodet (视频中的Android demo是基于该代码的)   
https://github.com/Tencent/ncnn  
https://github.com/RangiLyu/nanodet  
