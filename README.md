ncnn_nanodet_hand  
1.hand detect:用nanode-m训练了个hand detect模型,
2.hand pose:用CMU的数据集训练了个ghostnet作为backbone模仿pfld的handpose模型
3.推理:用ncnn部署推理,参照该代码可以集成到ncnn-android中哦.  
PS:由于数据集质量不理想，且自己水平有限所以实际效果可能较差，自己做着玩的.  
![image](https://github.com/FeiGeChuanShu/ncnn_nanodet_hand/blob/main/result.jpg)
Android上的效果可以看一下视频，总之ncnn和nanodet棒极了！  
https://www.bilibili.com/video/BV1Uh411Q7Rq
