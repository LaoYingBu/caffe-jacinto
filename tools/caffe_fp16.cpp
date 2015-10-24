#ifdef WITH_PYTHON_LAYER
#include "boost/python.hpp"
namespace bp = boost::python;
#endif

#include <glog/logging.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "boost/algorithm/string.hpp"
#include "caffe/caffe.hpp"
#include "caffe/util/gpu_memory.hpp"
#include "caffe/util/signal_handler.h"
#include "caffe/util/get.hpp"

using caffe::Blob;
using caffe::Caffe;
using caffe::Net;
using caffe::Layer;
using caffe::Solver;
using caffe::shared_ptr;
using caffe::string;
using caffe::Timer;
using caffe::vector;
using std::ostringstream;
using caffe::Get;
using caffe::float16;

DEFINE_string(gpu, "",
    "Optional; run in GPU mode on given device IDs separated by ','."
    "Use '-gpu all' to run on all available GPUs. The effective training "
    "batch size is multiplied by the number of devices.");
DEFINE_string(solver, "",
    "The solver definition protocol buffer text file.");
DEFINE_string(model, "",
    "The model definition protocol buffer text file..");
DEFINE_string(snapshot, "",
    "Optional; the snapshot solver state to resume training.");
DEFINE_string(weights, "",
    "Optional; the pretrained weights to initialize finetuning, "
    "separated by ','. Cannot be set simultaneously with snapshot.");
DEFINE_int32(iterations, 50,
    "The number of iterations to run.");
DEFINE_string(sigint_effect, "stop",
             "Optional; action to take when a SIGINT signal is received: "
              "snapshot, stop or none.");
DEFINE_string(sighup_effect, "snapshot",
             "Optional; action to take when a SIGHUP signal is received: "
             "snapshot, stop or none.");

// A simple registry for caffe commands.
typedef int (*BrewFunction)();
typedef std::map<caffe::string, BrewFunction> BrewMap;
BrewMap g_brew_map;

#define RegisterBrewFunction(func) \
namespace { \
class __Registerer_##func { \
 public: /* NOLINT */ \
  __Registerer_##func() { \
    g_brew_map[#func] = &func; \
  } \
}; \
__Registerer_##func g_registerer_##func; \
}

static BrewFunction GetBrewFunction(const caffe::string& name) {
  if (g_brew_map.count(name)) {
    return g_brew_map[name];
  } else {
    LOG(ERROR) << "Available caffe actions:";
    for (BrewMap::iterator it = g_brew_map.begin();
         it != g_brew_map.end(); ++it) {
      LOG(ERROR) << "\t" << it->first;
    }
    LOG(FATAL) << "Unknown action: " << name;
    return NULL;  // not reachable, just to suppress old compiler warnings.
  }
}

// Parse GPU ids or use all available devices
static void get_gpus(vector<int>* gpus) {
  if (FLAGS_gpu == "all") {
    int count = 0;
#ifndef CPU_ONLY
    CUDA_CHECK(cudaGetDeviceCount(&count));
#else
    NO_GPU;
#endif
    for (int i = 0; i < count; ++i) {
      gpus->push_back(i);
    }
  } else if (FLAGS_gpu.size()) {
    vector<string> strings;
    boost::split(strings, FLAGS_gpu, boost::is_any_of(","));
    for (int i = 0; i < strings.size(); ++i) {
      gpus->push_back(boost::lexical_cast<int>(strings[i]));
    }
  } else {
    CHECK_EQ(gpus->size(), 0);
  }
}

// caffe commands to call by
//     caffe <command> <args>
//
// To add a command, define a function "int command()" and register it with
// RegisterBrewFunction(action);

// Device Query: show diagnostic information for a GPU device.
int device_query() {
  LOG(INFO) << "Querying GPUs " << FLAGS_gpu;
  vector<int> gpus;
  get_gpus(&gpus);
  for (int i = 0; i < gpus.size(); ++i) {
    caffe::Caffe::SetDevice(gpus[i]);
    caffe::Caffe::DeviceQuery();
  }
  return 0;
}
RegisterBrewFunction(device_query);

// Load the weights from the specified caffemodel(s) into the train and
// test nets.
#if! defined (CPU_ONLY)
void CopyLayers(caffe::Solver<float16,CAFFE_FP16_MTYPE>* solver, const std::string& model_list) {
  std::vector<std::string> model_names;
  boost::split(model_names, model_list, boost::is_any_of(",") );
  for (int i = 0; i < model_names.size(); ++i) {
    LOG(INFO) << "Finetuning from " << model_names[i];
    solver->net()->CopyTrainedLayersFrom(model_names[i]);
    for (int j = 0; j < solver->test_nets().size(); ++j) {
      solver->test_nets()[j]->CopyTrainedLayersFrom(model_names[i]);
    }
  }
}

// Translate the signal effect the user specified on the command-line to the
// corresponding enumeration.
caffe::SolverAction::Enum GetRequestedAction(
    const std::string& flag_value) {
  if (flag_value == "stop") {
    return caffe::SolverAction::STOP;
  }
  if (flag_value == "snapshot") {
    return caffe::SolverAction::SNAPSHOT;
  }
  if (flag_value == "none") {
    return caffe::SolverAction::NONE;
  }
  LOG(FATAL) << "Invalid signal effect \""<< flag_value << "\" was specified";
}


// // Test: score a model.
// int test() {
//   CHECK_GT(FLAGS_model.size(), 0) << "Need a model definition to score.";
//   CHECK_GT(FLAGS_weights.size(), 0) << "Need model weights to score.";

//   // Set device id and mode
//   vector<int> gpus;
//   get_gpus(&gpus);
//   if (gpus.size() != 0) {
//     LOG(INFO) << "Use GPU with device ID " << gpus[0];
//     Caffe::SetDevice(gpus[0]);
//     Caffe::set_mode(Caffe::GPU);
//   } else {
//     LOG(INFO) << "Use CPU.";
//     Caffe::set_mode(Caffe::CPU);
//   }
//   // Instantiate the caffe net.
//   Net<float16,float16> caffe_net(FLAGS_model, caffe::TEST);
//   caffe_net.CopyTrainedLayersFrom(FLAGS_weights);
//   LOG(INFO) << "Running for " << FLAGS_iterations << " iterations.";

//   vector<Blob<float16,float16>* > bottom_vec;
//   vector<int> test_score_output_id;
//   vector<float16> test_score;
//   float loss = 0;
//   for (int i = 0; i < FLAGS_iterations; ++i) {
//     float iter_loss;
//     const vector<Blob<float16,float16>*>& result =
//         caffe_net.Forward(bottom_vec, &iter_loss);
//     loss += iter_loss;
//     int idx = 0;
//     for (int j = 0; j < result.size(); ++j) {
//       const float16* result_vec = result[j]->cpu_data();
//       for (int k = 0; k < result[j]->count(); ++k, ++idx) {
//         const CAFFE_FP16_MTYPE score = Get<CAFFE_FP16_MTYPE>(result_vec[k]);
//         if (i == 0) {
//           test_score.push_back(score);
//           test_score_output_id.push_back(j);
//         } else {
//           test_score[idx] += score;
//         }
//         const std::string& output_name = caffe_net.blob_names()[
//             caffe_net.output_blob_indices()[j]];
//         LOG(INFO) << "Batch " << i << ", " << output_name << " = " << score;
//       }
//     }
//   }
//   loss /= FLAGS_iterations;
//   LOG(INFO) << "Loss: " << loss;
//   for (int i = 0; i < test_score.size(); ++i) {
//     const std::string& output_name = caffe_net.blob_names()[
//         caffe_net.output_blob_indices()[test_score_output_id[i]]];
//     const float loss_weight = caffe_net.blob_loss_weights()[
//         caffe_net.output_blob_indices()[test_score_output_id[i]]];
//     std::ostringstream loss_msg_stream;
//     const float mean_score = test_score[i] / FLAGS_iterations;
//     if (loss_weight) {
//       loss_msg_stream << " (* " << loss_weight
//                       << " = " << loss_weight * mean_score << " loss)";
//     }
//     LOG(INFO) << output_name << " = " << mean_score << loss_msg_stream.str();
//   }

//   return 0;
// }
// RegisterBrewFunction(test);

// Time: benchmark the execution time of a model.
int time() {
  CHECK_GT(FLAGS_model.size(), 0) << "Need a model definition to time.";

  // Set device id and mode
  vector<int> gpus;
  get_gpus(&gpus);
  if (gpus.size() != 0) {
    LOG(INFO) << "Use GPU with device ID " << gpus[0];
    Caffe::SetDevice(gpus[0]);
    Caffe::set_mode(Caffe::GPU);
  } else {
    LOG(INFO) << "Use CPU.";
    Caffe::set_mode(Caffe::CPU);
  }
  // Instantiate the caffe net.
  Net<float16,CAFFE_FP16_MTYPE> caffe_net(FLAGS_model, caffe::TRAIN);

  // Do a clean forward and backward pass, so that memory allocation are done
  // and future iterations will be more stable.
  LOG(INFO) << "Performing Forward";
  // Note that for the speed benchmark, we will assume that the network does
  // not take any input blobs.
  CAFFE_FP16_MTYPE initial_loss;
  caffe_net.Forward(vector<Blob<float16,CAFFE_FP16_MTYPE>*>(), &initial_loss);
  LOG(INFO) << "Initial loss: " << initial_loss;

  const vector<shared_ptr<Layer<float16,CAFFE_FP16_MTYPE> > >& layers = caffe_net.layers();
  const vector<vector<Blob<float16,CAFFE_FP16_MTYPE>*> >& bottom_vecs = caffe_net.bottom_vecs();
  const vector<vector<Blob<float16,CAFFE_FP16_MTYPE>*> >& top_vecs = caffe_net.top_vecs();
  LOG(INFO) << "*** Benchmark begins ***";
  LOG(INFO) << "Testing for " << FLAGS_iterations << " iterations.";
  Timer total_timer;
  total_timer.Start();
  Timer forward_timer;
  Timer timer;
  std::vector<double> forward_time_per_layer(layers.size(), 0.0);
  double forward_time = 0.0;
  // Per layer timing
  for (int j = 0; j < FLAGS_iterations; ++j) {
    Timer iter_timer;
    iter_timer.Start();
    // Time forward layers
    for (int i = 0; i < layers.size(); ++i) {
      timer.Start();
      layers[i]->Forward(bottom_vecs[i], top_vecs[i]);
      forward_time_per_layer[i] += timer.MicroSeconds();
    }
    LOG(INFO) << "Iteration: " << j + 1 << " forward time (layer by layer): "
	      << iter_timer.MilliSeconds() << " ms.";
  }
  LOG(INFO) << "Average time per layer: ";
  for (int i = 0; i < layers.size(); ++i) {
    const caffe::string& layername = layers[i]->layer_param().name();
    LOG(INFO) << std::setfill(' ') << std::setw(10) << layername <<
      "\tforward: " << forward_time_per_layer[i] / 1000 /
      FLAGS_iterations << " ms.";
  }
  // Total timing - remove overheads introduced in timing individual layers
  total_timer.Start();
  for (int j = 0; j < FLAGS_iterations; ++j) {
    Timer iter_timer;
    iter_timer.Start();
    // Time total forward
    forward_timer.Start();
    for (int i = 0; i < layers.size(); ++i) {
      layers[i]->Forward(bottom_vecs[i], top_vecs[i]);
    }
    forward_time += forward_timer.MicroSeconds();
    
    LOG(INFO) << "Iteration: " << j + 1 << " forward-backward time: "
	      << iter_timer.MilliSeconds() << " ms.";
  }
  total_timer.Stop();

  LOG(INFO) << "Average Forward pass: " << forward_time / 1000 /
    FLAGS_iterations << " ms.";
  LOG(INFO) << "Total Time: " << total_timer.MilliSeconds() << " ms.";
  LOG(INFO) << "*** Benchmark ends ***";
  return 0;
}
RegisterBrewFunction(time);
#endif // ifndef CPU_ONLY

int main(int argc, char** argv) {
  // Print output to stderr (while still logging).
  FLAGS_alsologtostderr = 1;
  // Usage message.
  gflags::SetUsageMessage("command line brew\n"
      "usage: caffe <command> <args>\n\n"
      "commands:\n"
      "  test            score a model\n"
      "  device_query    show GPU diagnostic information\n"
      "  time            benchmark model execution time");
  // Run tool or show usage.
  caffe::GlobalInit(&argc, &argv);

  vector<int> gpus;
  get_gpus(&gpus);
  caffe::gpu_memory::arena arena(gpus);

  if (argc == 2) {
#ifdef WITH_PYTHON_LAYER
    try {
#endif
      return GetBrewFunction(caffe::string(argv[1]))();
#ifdef WITH_PYTHON_LAYER
    } catch (bp::error_already_set) {
      PyErr_Print();
      return 1;
    }
#endif
  } else {
    gflags::ShowUsageWithFlagsRestrict(argv[0], "tools/caffe");
  }
}