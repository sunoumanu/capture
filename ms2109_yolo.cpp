// ms2109_yolo.cpp
// Implementation of YoloDetector and YoloPipeline.

#include "ms2109_yolo.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>

namespace {
// Default DOTA-v1 class list used by Ultralytics *-obb models.
const char* kDotaClasses[] = {
    "plane", "ship", "storage tank", "baseball diamond", "tennis court",
    "basketball court", "ground track field", "harbor", "bridge",
    "large vehicle", "small vehicle", "helicopter", "roundabout",
    "soccer ball field", "swimming pool"
};

// Default COCO-80 class list (kept here so we don't hand-roll it twice).
const char* kCocoClasses[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};

bool pathContainsObbTag(const std::string& path) {
    std::string lower;
    lower.reserve(path.size());
    for (char c : path) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // Match "-obb", "_obb" or ".obb" so we don't trip on unrelated "obb" substrings.
    return lower.find("-obb") != std::string::npos ||
           lower.find("_obb") != std::string::npos ||
           lower.find(".obb") != std::string::npos;
}
} // namespace

void YoloDetector::loadClasses(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return;
    std::string line;
    while (std::getline(ifs, line)) {
        line.erase(line.find_last_not_of(" \r\n\t") + 1);
        if (!line.empty()) classNames.push_back(line);
    }
}

bool YoloDetector::loadModel(const std::string& modelPath,
                             const std::string& classesPath,
                             float confThresh,
                             float nmsThresh) {
    confThreshold = confThresh;
    nmsThreshold  = nmsThresh;

    try {
        if (modelPath.size() > 5 &&
            modelPath.substr(modelPath.size() - 5) == ".onnx") {
            net = cv::dnn::readNetFromONNX(modelPath);
        } else {
            std::cerr << "Only ONNX models are currently supported. Given: "
                      << modelPath << "\n";
            return false;
        }
    } catch (const cv::Exception& e) {
        std::cerr << "OpenCV exception loading model: " << e.what() << "\n";
        return false;
    }

    if (net.empty()) {
        std::cerr << "Failed to load model: " << modelPath << "\n";
        return false;
    }

    // Pick the best backend/target actually compiled into this OpenCV build.
    // setPreferableBackend/Target do NOT throw on unsupported combos — they
    // assert later inside the DNN engine — so we must query availability.
    bool haveCudaFp32  = false;
    bool haveCudaFp16  = false;
    bool haveOpenCL    = false;
    bool haveOpenCLFp16 = false;
    try {
        for (const auto& bt : cv::dnn::getAvailableBackends()) {
            if (bt.first == cv::dnn::DNN_BACKEND_CUDA) {
                if (bt.second == cv::dnn::DNN_TARGET_CUDA)      haveCudaFp32 = true;
                if (bt.second == cv::dnn::DNN_TARGET_CUDA_FP16) haveCudaFp16 = true;
            }
            if (bt.first == cv::dnn::DNN_BACKEND_OPENCV ||
                bt.first == cv::dnn::DNN_BACKEND_DEFAULT) {
                if (bt.second == cv::dnn::DNN_TARGET_OPENCL)      haveOpenCL = true;
                if (bt.second == cv::dnn::DNN_TARGET_OPENCL_FP16) haveOpenCLFp16 = true;
            }
        }
    } catch (...) {}

    if (haveCudaFp16) {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA_FP16);
        std::cerr << "YOLO: using CUDA FP16 backend\n";
    } else if (haveCudaFp32) {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
        std::cerr << "YOLO: using CUDA FP32 backend\n";
    } else if (haveOpenCLFp16) {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL_FP16);
        std::cerr << "YOLO: using OpenCL FP16 backend\n";
    } else if (haveOpenCL) {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);
        std::cerr << "YOLO: using OpenCL backend\n";
    } else {
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        std::cerr << "YOLO: using CPU backend\n";
    }

    outLayerNames = net.getUnconnectedOutLayersNames();
    isObb = pathContainsObbTag(modelPath);
    classNamesFromUser = !classesPath.empty();
    if (classNamesFromUser) loadClasses(classesPath);

    // Default class names if the user didn't supply any. Pick the list that
    // matches the model family, NOT a one-size-fits-all COCO fallback —
    // otherwise an OBB/DOTA model lights up every box with COCO labels
    // (e.g. class index 1 → "bicycle"), which is misleading garbage.
    if (classNames.empty()) {
        if (isObb) {
            classNames.assign(kDotaClasses, kDotaClasses + 15);
            std::cerr << "YOLO: model name suggests OBB; defaulting to DOTA-15 class names. "
                         "Use --yolo-classes to override.\n";
        } else {
            classNames.assign(kCocoClasses, kCocoClasses + 80);
        }
    }

    // OBB checkpoints from Ultralytics are trained on DOTA — top-down aerial
    // photography. Run them on a webcam / HDMI capture and they spew junk
    // detections that argmax to whichever class the model is biased toward
    // ("ship" is the common one). Two safety nets:
    //   1. Bump the confidence floor so most of the noise drops out unless
    //      the user explicitly lowered the threshold.
    //   2. Tell the user what's going on so they don't think it's a bug.
    if (isObb) {
        if (confThreshold < 0.5f) {
            std::cerr << "YOLO: raising confidence threshold to 0.50 for OBB model "
                         "(was " << confThreshold << "). Override with --yolo-conf.\n";
            confThreshold = 0.5f;
        }
        std::cerr << "YOLO: NOTE — OBB models are trained on DOTA aerial imagery. "
                     "On ordinary video they will produce mostly false positives. "
                     "For general video use a COCO model (e.g. yolov8n.onnx).\n";
    }
    return true;
}

std::vector<Detection> YoloDetector::detect(const cv::Mat& frame) {
    if (net.empty() || frame.empty()) return {};

    // Letterbox to 640x640 preserving aspect ratio (grey padding = 114).
    float imgW = static_cast<float>(frame.cols);
    float imgH = static_cast<float>(frame.rows);
    float scale = std::min(modelInputSize.width  / imgW,
                           modelInputSize.height / imgH);
    int newW = static_cast<int>(std::round(imgW * scale));
    int newH = static_cast<int>(std::round(imgH * scale));
    int padX = (modelInputSize.width  - newW) / 2;
    int padY = (modelInputSize.height - newH) / 2;

    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);

    cv::Mat padded(modelInputSize, frame.type(), cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(padX, padY, newW, newH)));

    cv::Mat blob;
    cv::dnn::blobFromImage(padded, blob, 1.0 / 255.0, modelInputSize,
                           cv::Scalar(), true, false);

    std::vector<cv::Mat> outs;
    try {
        net.setInput(blob);
        net.forward(outs, outLayerNames);
    } catch (const cv::Exception& e) {
        // OpenCV DNN can throw on unsupported ops in newer YOLO graphs
        // (the "axis >= -dims && axis < dims" assertion is the classic one).
        // Without this guard the exception unwinds to std::terminate and
        // crashes the whole capture app. Log once, then disable inference.
        if (!inferenceFailed) {
            std::cerr << "YOLO: inference failed and is now disabled for this run.\n"
                      << "      OpenCV said: " << e.what() << "\n";
            inferenceFailed = true;
        }
        return {};
    } catch (const std::exception& e) {
        if (!inferenceFailed) {
            std::cerr << "YOLO: inference failed: " << e.what() << "\n";
            inferenceFailed = true;
        }
        return {};
    }
    try {
        return postProcess(frame, outs, scale, padX, padY);
    } catch (const cv::Exception& e) {
        std::cerr << "YOLO: postProcess failed: " << e.what() << "\n";
        return {};
    }
}

std::vector<Detection> YoloDetector::postProcess(const cv::Mat& frame,
                                                 const std::vector<cv::Mat>& outs,
                                                 float scale, int padX, int padY) {
    std::vector<Detection> result;
    std::vector<cv::Rect>  boxes;
    std::vector<float>     confidences;
    std::vector<int>       classIds;

    int origW = frame.cols;
    int origH = frame.rows;

    for (const auto& out : outs) {
        cv::Mat predictions;
        if (out.dims == 3) {
            if (out.size[1] > out.size[2]) {
                // [1, anchors, features]  e.g. [1, 25200, 85]
                predictions = out.reshape(1, out.size[1]);
            } else {
                // [1, features, anchors]  e.g. [1, 84, 8400]
                float* ptr = const_cast<float*>(out.ptr<float>(0));
                cv::Mat twoD(out.size[1], out.size[2], CV_32F, ptr);
                cv::transpose(twoD, predictions);
            }
        } else if (out.dims == 2) {
            predictions = out;
        } else {
            continue;
        }

        const int numPreds = predictions.rows;
        const int numAttrs = predictions.cols;
        if (numPreds == 0 || numAttrs < 5) continue;

        // Figure out the layout. We support three:
        //   v5/v7 detect:   [4 box, 1 obj, N cls]                  (numAttrs = 5 + N)
        //   v8/v11 detect:  [4 box, N cls]                         (numAttrs = 4 + N)
        //   v8/v11 OBB:     [4 box, N cls, 1 angle]                (numAttrs = 5 + N)
        //
        // For OBB with N classes, total attrs is the same as v5 with N classes,
        // so we disambiguate using the OBB hint from the model filename.
        int  numClasses    = 0;
        bool hasObjectness = false;
        bool obbAngle      = false;
        const int classesHint = static_cast<int>(classNames.size());

        if (isObb && numAttrs >= 6) {
            // Ultralytics OBB: 4 box + N cls + 1 angle, in that order.
            numClasses = numAttrs - 5;
            obbAngle   = true;
        } else if (classesHint > 0 && numAttrs == 5 + classesHint) {
            numClasses    = classesHint;
            hasObjectness = true;
        } else if (classesHint > 0 && numAttrs == 4 + classesHint) {
            numClasses    = classesHint;
        } else {
            // Last resort: assume v8-style (no objectness, no angle).
            numClasses = numAttrs - 4;
        }
        if (numClasses <= 0) continue;

        // If the model output has more classes than we have names for, mint
        // generic ones rather than aliasing into a wrong list (this is what
        // produced the "everything is a bicycle" bug with an OBB model on
        // COCO defaults).
        if (numClasses != classesHint) {
            if (!classNamesFromUser) {
                if (numClasses > classesHint) {
                    for (int k = classesHint; k < numClasses; ++k)
                        classNames.push_back("class_" + std::to_string(k));
                }
            } else {
                static bool warned = false;
                if (!warned) {
                    std::cerr << "YOLO: model has " << numClasses
                              << " classes but " << classesHint
                              << " names were supplied; extra ids will be labeled class_N.\n";
                    warned = true;
                }
                for (int k = classesHint; k < numClasses; ++k)
                    classNames.push_back("class_" + std::to_string(k));
            }
        }

        for (int i = 0; i < numPreds; ++i) {
            const float* data = predictions.ptr<float>(i);
            float cx = data[0];
            float cy = data[1];
            float w  = data[2];
            float h  = data[3];

            float conf = 0.0f;
            int   classId = -1;

            if (hasObjectness) {
                float objConf = data[4];
                float* sptr = const_cast<float*>(data + 5);
                cv::Mat scores(1, numClasses, CV_32F, sptr);
                cv::Point maxLoc;
                double maxVal;
                cv::minMaxLoc(scores, nullptr, &maxVal, nullptr, &maxLoc);
                conf = static_cast<float>(maxVal) * objConf;
                classId = maxLoc.x;
            } else {
                float* sptr = const_cast<float*>(data + 4);
                cv::Mat scores(1, numClasses, CV_32F, sptr);
                cv::Point maxLoc;
                double maxVal;
                cv::minMaxLoc(scores, nullptr, &maxVal, nullptr, &maxLoc);
                conf = static_cast<float>(maxVal);
                classId = maxLoc.x;
            }

            if (conf < confThreshold || classId < 0) continue;

            // Map from 640x640 padded space back to original image.
            // NB: do NOT name these origW/origH — those names are taken
            // by the image dims captured above. Shadowing them was an old
            // bug that made the box-clamping math compare against the
            // box's own size instead of the frame, so boxes could fly
            // off-screen.
            float boxCX = (cx - padX) / scale;
            float boxCY = (cy - padY) / scale;
            float boxW  = w / scale;
            float boxH  = h / scale;

            float x = boxCX - boxW * 0.5f;
            float y = boxCY - boxH * 0.5f;

            int left   = std::max(0, static_cast<int>(x));
            int top    = std::max(0, static_cast<int>(y));
            int width  = static_cast<int>(boxW);
            int height = static_cast<int>(boxH);
            if (left + width  > origW) width  = origW - left;
            if (top  + height > origH) height = origH - top;
            if (width  < 1 || height < 1) continue;

            boxes.emplace_back(left, top, width, height);
            confidences.push_back(conf);
            classIds.push_back(classId);
        }
        (void)obbAngle; // reserved for future rotated-box rendering
    }

    std::vector<int> indices;
    cv::dnn::NMSBoxes(boxes, confidences, confThreshold, nmsThreshold, indices);

    result.reserve(indices.size());
    for (int idx : indices) {
        Detection d;
        d.classId    = classIds[idx];
        d.confidence = confidences[idx];
        d.box        = boxes[idx];
        if (d.classId >= 0 && d.classId < static_cast<int>(classNames.size()))
            d.className = classNames[d.classId];
        result.push_back(d);
    }
    return result;
}

void YoloDetector::drawDetections(cv::Mat& frame,
                                  const std::vector<Detection>& detections) {
    for (const auto& d : detections) {
        cv::Scalar color(
            static_cast<double>((d.classId * 37) % 255),
            static_cast<double>((d.classId * 71) % 255),
            static_cast<double>((d.classId * 113) % 255));

        cv::rectangle(frame, d.box, color, 2);

        std::string label = d.className + " " + cv::format("%.2f", d.confidence);
        int baseline = 0;
        cv::Size ts = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                      0.5, 1, &baseline);
        int top = std::max(d.box.y, ts.height + 4);
        cv::rectangle(frame,
                      cv::Point(d.box.x, top - ts.height - 4),
                      cv::Point(d.box.x + ts.width, top),
                      color, cv::FILLED);
        cv::putText(frame, label,
                    cv::Point(d.box.x, top - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
    }
}

// --------------------- YoloPipeline ------------------------------

void YoloPipeline::start() {
    thread = std::thread([this] {
        cv::Mat frame;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return !q.empty() || done; });
                if (done && q.empty()) break;
                frame = std::move(q.front());
                q.pop_front();
            }
            if (frame.empty()) continue;

            // Catch everything inside the worker. An uncaught cv::Exception
            // here (the OBB model's axis-out-of-range assertion is the one
            // that bit us) would unwind through std::thread and call
            // std::terminate, killing the entire capture program.
            std::vector<Detection> detections;
            try {
                detections = detector.detect(frame);
            } catch (const cv::Exception& e) {
                std::cerr << "YOLO worker: OpenCV exception, frame skipped: "
                          << e.what() << "\n";
                continue;
            } catch (const std::exception& e) {
                std::cerr << "YOLO worker: exception, frame skipped: "
                          << e.what() << "\n";
                continue;
            } catch (...) {
                std::cerr << "YOLO worker: unknown exception, frame skipped\n";
                continue;
            }

            // Publish detections only — *not* an annotated frame. The display
            // thread will overlay these onto whatever the freshest captured
            // frame happens to be, so display FPS stays at capture FPS even
            // when inference is much slower.
            {
                std::lock_guard<std::mutex> lk(outMailbox.m);
                outMailbox.detections = std::move(detections);
                ++outMailbox.serial;
            }
            processedCount.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

void YoloPipeline::stop() {
    {
        std::lock_guard<std::mutex> lk(m);
        done = true;
    }
    cv.notify_all();
    if (thread.joinable()) thread.join();
}

bool YoloPipeline::push(const cv::Mat& f) {
    std::unique_lock<std::mutex> lk(m);
    if (q.size() >= maxSize) return false;
    q.push_back(f.clone());
    lk.unlock();
    cv.notify_one();
    return true;
}

bool YoloPipeline::tryTakeDetections(uint64_t& lastSeen,
                                     std::vector<Detection>& out) {
    std::lock_guard<std::mutex> lk(outMailbox.m);
    if (outMailbox.serial == lastSeen) return false;
    out = outMailbox.detections;   // small vector copy under the lock
    lastSeen = outMailbox.serial;
    return true;
}
