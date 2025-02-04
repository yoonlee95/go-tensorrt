#ifdef __linux__

#include "predict.hpp"
#include "json.hpp"
#include "timer.h"
#include "timer.impl.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

#include <cuda_runtime_api.h>

#include "NvCaffeParser.h"
#include "NvInfer.h"

using namespace nvinfer1;
using namespace nvcaffeparser1;

using json = nlohmann::json;

class Logger : public ILogger {
  void log(Severity severity, const char *msg) override {
    // suppress info-level messages
    if (severity != Severity::kINFO) {
      std::cout << msg << std::endl;
    }
  }
} gLogger;

#define CHECK(status)                                                          \
  {                                                                            \
    if (status != 0) {                                                         \
      std::cerr << "Cuda failure on line " << __LINE__                         \
                << " status =  " << status << "\n";                            \
      return nullptr;                                                          \
    }                                                                          \
  }

class Profiler : public IProfiler {
public:
  Profiler(profile *prof) : prof_(prof) { current_time_ = prof->get_start(); }

  /** \brief layer time reporting callback
   *
   * \param layerName the name of the layer, set when constructing the network
   * definition
   * \param ms the time in milliseconds to execute the layer
   */
  virtual void reportLayerTime(const char *layer_name, float ms) {

    if (prof_ == nullptr) {
      return;
    }

    auto duration = std::chrono::nanoseconds((timestamp_t::rep)(1000000 * ms));
    auto e =
        new profile_entry(layer_name, current_time_, current_time_ + duration);
    prof_->add(e);

    current_time_ += duration;
  }

  virtual ~Profiler() {}

private:
  profile *prof_{nullptr};
  timestamp_t current_time_{};
};

class Predictor {
public:
  Predictor(ICudaEngine *engine) : engine_(engine){};
  ~Predictor() {

    if (engine_) {
      engine_->destroy();
    }
    if (prof_) {
      prof_->reset();
      delete prof_;
      prof_ = nullptr;
    }
  }

  ICudaEngine *engine_;
  profile *prof_{nullptr};
  bool prof_registered_{false};
};

PredictorContext NewTensorRT(char *deploy_file, char *weights_file, int batch,
                             char *outputLayer) {
  try {
    IBuilder *builder = createInferBuilder(gLogger);
    INetworkDefinition *network = builder->createNetwork();
    ICaffeParser *parser = createCaffeParser();

    const IBlobNameToTensor *blobNameToTensor =
        parser->parse(deploy_file, weights_file, *network, DataType::kFLOAT);

    auto loc = blobNameToTensor->find(outputLayer);
    if (loc == nullptr) {
      std::cerr << "cannot find " << outputLayer << " in blobNameToTensor\n";
      return nullptr;
    }
    network->markOutput(*loc);

    builder->setMaxBatchSize(batch);
    builder->setMaxWorkspaceSize(1 << 20);
    ICudaEngine *engine = builder->buildCudaEngine(*network);
    Predictor *pred = new Predictor(engine);
    return (PredictorContext)pred;
  } catch (const std::invalid_argument &ex) {
    return nullptr;
  }
}

void DeleteTensorRT(PredictorContext pred) {
  auto predictor = (Predictor *)pred;
  if (predictor == nullptr) {
    return;
  }
  delete predictor;
}

const char *PredictTensorRT(PredictorContext pred, float *input,
                            const char *input_layer_name,
                            const char *output_layer_name,
                            const int batchSize) {

  auto predictor = (Predictor *)pred;

  if (predictor == nullptr) {
    std::cerr << "tensorrt prediction error on " << __LINE__ << "\n";
    return nullptr;
  }
  auto engine = predictor->engine_;
  if (engine->getNbBindings() != 2) {
    std::cerr << "tensorrt prediction error on " << __LINE__ << "\n";
    return nullptr;
  }

  // In order to bind the buffers, we need to know the names of the input and
  // output tensors.
  // note that indices are guaranteed to be less than IEngine::getNbBindings()
  const int input_index = engine->getBindingIndex(input_layer_name);
  const int output_index = engine->getBindingIndex(output_layer_name);

  // std::cerr << "using input layer = " << input_layer_name << "\n";
  // std::cerr << "using output layer = " << output_layer_name << "\n";

  const auto input_dim_ =
      static_cast<DimsCHW &&>(engine->getBindingDimensions(input_index));
  const auto input_byte_size =
      input_dim_.c() * input_dim_.h() * input_dim_.w() * sizeof(float);

  const auto output_dim_ =
      static_cast<DimsCHW &&>(engine->getBindingDimensions(output_index));
  const auto output_size = output_dim_.c() * output_dim_.h() * output_dim_.w();
  const auto output_byte_size = output_size * sizeof(float);

  float *input_layer, *output_layer;

  CHECK(cudaMalloc((void **)&input_layer, batchSize * input_byte_size));
  CHECK(cudaMalloc((void **)&output_layer, batchSize * output_byte_size));

  IExecutionContext *context = engine->createExecutionContext();

  // std::cerr << "size of input = " << batchSize * input_byte_size << "\n";
  // std::cerr << "size of output = " << batchSize * output_byte_size << "\n";

  // DMA the input to the GPU,  execute the batch asynchronously, and DMA it
  // back:
  CHECK(cudaMemcpy(input_layer, input, batchSize * input_byte_size,
                   cudaMemcpyHostToDevice));

  void *buffers[2] = {input_layer, output_layer};

  Profiler profiler(predictor->prof_);

  // Set the custom profiler.
  context->setProfiler(&profiler);

  context->execute(batchSize, buffers);

  std::vector<float> output(batchSize * output_size);
  std::fill(output.begin(), output.end(), 0);

  CHECK(cudaMemcpy(output.data(), output_layer, batchSize * output_byte_size,
                   cudaMemcpyDeviceToHost));

  // release the stream and the buffers
  CHECK(cudaFree(input_layer));
  CHECK(cudaFree(output_layer));

  context->destroy();

  // classify image
  json preds = json::array();

  for (int cnt = 0; cnt < batchSize; cnt++) {
    for (int idx = 0; idx < output_size; idx++) {
      preds.push_back(
          {{"index", idx}, {"probability", output[cnt * output_size + idx]}});
    }
  }

  fflush(stderr);

  auto res = strdup(preds.dump().c_str());
  return res;
}

void TensorRTInit() {}

void TensorRTStartProfiling(PredictorContext pred, const char *name,
                            const char *metadata) {
  auto predictor = (Predictor *)pred;
  if (predictor == nullptr) {
    return;
  }
  if (name == nullptr) {
    name = "";
  }
  if (metadata == nullptr) {
    metadata = "";
  }
  if (predictor->prof_ == nullptr) {
    predictor->prof_ = new profile(name, metadata);
  } else {
    predictor->prof_->reset();
  }
}

void TensorRTEndProfiling(PredictorContext pred) {
  auto predictor = (Predictor *)pred;
  if (predictor == nullptr) {
    return;
  }
  if (predictor->prof_) {
    predictor->prof_->end();
  }
}

void TensorRTDisableProfiling(PredictorContext pred) {
  auto predictor = (Predictor *)pred;
  if (predictor == nullptr) {
    return;
  }
  if (predictor->prof_) {
    predictor->prof_->reset();
  }
}

char *TensorRTReadProfile(PredictorContext pred) {
  auto predictor = (Predictor *)pred;
  if (predictor == nullptr) {
    return strdup("");
  }
  if (predictor->prof_ == nullptr) {
    return strdup("");
  }
  const auto s = predictor->prof_->read();
  const auto cstr = s.c_str();
  return strdup(cstr);
}

#endif // __linux__