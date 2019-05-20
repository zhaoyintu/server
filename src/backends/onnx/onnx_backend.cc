// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/backends/onnx/onnx_backend.h"

#include <NvInfer.h>
#include <core/providers/cuda/cuda_provider_factory.h>
#include <stdint.h>
#include <mutex>
#include "cuda/include/cuda_runtime_api.h"
#include "src/backends/onnx/onnx_utils.h"
#include "src/core/constants.h"
#include "src/core/logging.h"
#include "src/core/model_config_cuda.h"
#include "src/core/model_config_utils.h"
#include "src/core/provider.h"
#include "src/core/server_status.h"

namespace nvidia { namespace inferenceserver {

OnnxBackend::Context::Context(
    const std::string& name, const int gpu_device, const int max_batch_size)
    : name_(name), gpu_device_(gpu_device), max_batch_size_(max_batch_size),
      session_(nullptr), allocator_info_(nullptr)
{
}

OnnxBackend::Context::~Context()
{
  LOG_VERBOSE(1) << "~OnnxBackend::Context ";

  ReleaseOrtRunResources();
  if (session_ != nullptr) {
    OrtReleaseSession(session_);
  }
  if (allocator_info_ != nullptr) {
    OrtReleaseAllocatorInfo(allocator_info_);
  }
}

Status
OnnxBackend::Init(const std::string& path, const ModelConfig& config)
{
  RETURN_IF_ERROR(ValidateModelConfig(config, kOnnxRuntimeOnnxPlatform));
  RETURN_IF_ERROR(SetModelConfig(path, config));

  return Status::Success;
}

Status
OnnxBackend::CreateExecutionContexts(
    OrtEnv* env, const std::unordered_map<std::string, std::string>& paths)
{
  // [TODO] configurable like optimization policy in Tensorflow models
  // Create a "prototype" session option, which will be cloned and set
  // context-specific option on context creation.
  OrtSessionOptions* session_options = OrtCreateSessionOptions();
  OrtSetSessionThreadPoolSize(session_options, 1);
  // disable graph optimization
  OrtSetSessionGraphOptimizationLevel(session_options, 0);

  Status status = CreateExecutionContextsHelper(env, session_options, paths);

  OrtReleaseSessionOptions(session_options);
  RETURN_IF_ERROR(status);

  LOG_VERBOSE(1) << "onnx backend for " << Name() << std::endl << *this;

  return Status::Success;
}

Status
OnnxBackend::CreateExecutionContextsHelper(
    OrtEnv* env, OrtSessionOptions* session_options,
    const std::unordered_map<std::string, std::string>& paths)
{
  uint32_t total_context_cnt = 0;

  // Create a session for each instance.
  for (const auto& group : Config().instance_group()) {
    for (int c = 0; c < group.count(); c++) {
      if (group.kind() == ModelInstanceGroup::KIND_CPU) {
        const std::string instance_name =
            group.name() + "_" + std::to_string(c) + "_cpu";
        RETURN_IF_ERROR(CreateExecutionContext(
            instance_name, Context::NO_GPU_DEVICE, env, session_options,
            paths));
        total_context_cnt++;
      } else {
        for (int gpu_device : group.gpus()) {
          const std::string instance_name = group.name() + "_" +
                                            std::to_string(c) + "_gpu" +
                                            std::to_string(gpu_device);
          RETURN_IF_ERROR(CreateExecutionContext(
              instance_name, gpu_device, env, session_options, paths));
          total_context_cnt++;
        }
      }
    }
  }

  // Create a scheduler with one thread for each context available for
  // this model. Each runner is exclusively tied to the context.
  RETURN_IF_ERROR(SetConfiguredScheduler(
      total_context_cnt,
      [](uint32_t runner_idx) -> Status { return Status::Success; },
      [this](
          uint32_t runner_idx, std::vector<Scheduler::Payload>* payloads,
          std::function<void(Status)> func) {
        Run(runner_idx, payloads, func);
      }));

  return Status::Success;
}

Status
OnnxBackend::CreateExecutionContext(
    const std::string& instance_name, const int gpu_device, OrtEnv* env,
    OrtSessionOptions* base_session_options,
    const std::unordered_map<std::string, std::string>& paths)
{
  // For a GPU context, determine the model file to use for device
  // compute capability. CPU always uses the default model file.
  std::string cc;
  std::string cc_model_filename;
  if (gpu_device == Context::NO_GPU_DEVICE) {
    cc_model_filename = Config().default_model_filename();
  } else {
    cudaDeviceProp cuprops;
    cudaError_t cuerr = cudaGetDeviceProperties(&cuprops, gpu_device);
    if (cuerr != cudaSuccess) {
      return Status(
          RequestStatusCode::INTERNAL,
          "unable to get CUDA device properties for " + Name() + ": " +
              cudaGetErrorString(cuerr));
    }

    cc = std::to_string(cuprops.major) + "." + std::to_string(cuprops.minor);
    const auto& cc_itr = Config().cc_model_filenames().find(cc);
    cc_model_filename = (cc_itr == Config().cc_model_filenames().end())
                            ? Config().default_model_filename()
                            : cc_itr->second;
  }

  const auto& op_itr = paths.find(cc_model_filename);
  if (op_itr == paths.end()) {
    return Status(
        RequestStatusCode::INTERNAL,
        "unable to find model '" + cc_model_filename + "' for " + Name());
  }

  if (gpu_device == Context::NO_GPU_DEVICE) {
    LOG_INFO << "Creating instance " << instance_name << " on CPU using "
             << cc_model_filename;
  } else {
    LOG_INFO << "Creating instance " << instance_name << " on GPU "
             << gpu_device << " (" << cc << ") using " << cc_model_filename;
  }

  // Max batch size. A value of 0 in the config becomes NO_BATCHING.
  const int mbs = (Config().max_batch_size() <= 0) ? Context::NO_BATCHING
                                                   : Config().max_batch_size();

  contexts_.emplace_back(new Context(instance_name, gpu_device, mbs));
  Context* context = contexts_.back().get();

  // [TODO] special handling for statefull model?

  // Create Onnx session
  OrtStatus* onnx_status = nullptr;
  OrtSessionOptions* options = OrtCloneSessionOptions(base_session_options);
  if (gpu_device != Context::NO_GPU_DEVICE) {
    onnx_status =
        OrtSessionOptionsAppendExecutionProvider_CUDA(options, gpu_device);
  }
  if (onnx_status == nullptr) {
    onnx_status = OrtCreateSession(
        env, op_itr->second.c_str(), options, &context->session_);
  }
  OrtReleaseSessionOptions(options);

  RETURN_IF_ORT_ERROR(onnx_status);

  RETURN_IF_ERROR(context->ValidateInputs(Config().input()));
  RETURN_IF_ERROR(context->ValidateOutputs(Config().output()));

  RETURN_IF_ORT_ERROR(OrtCreateCpuAllocatorInfo(
      OrtArenaAllocator, OrtMemTypeDefault, &context->allocator_info_));

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateInputs(
    const ::google::protobuf::RepeatedPtrField<ModelInput>& ios)
{
  std::set<std::string> input_node_names;
  RETURN_IF_ERROR(InputNames(session_, input_node_names));

  for (const auto& io : ios) {
    RETURN_IF_ERROR(CheckAllowedModelInput(io, input_node_names));
    if (ConvertToOnnxDataType(io.data_type()) ==
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
      return Status(
          RequestStatusCode::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for input '" + io.name() + "' for model '" + name_ + "'");
    }
  }

  return Status::Success;
}

Status
OnnxBackend::Context::ValidateOutputs(
    const ::google::protobuf::RepeatedPtrField<ModelOutput>& ios)
{
  std::set<std::string> output_node_names;
  RETURN_IF_ERROR(OutputNames(session_, output_node_names));

  for (const auto& io : ios) {
    RETURN_IF_ERROR(CheckAllowedModelOutput(io, output_node_names));
    if (ConvertToOnnxDataType(io.data_type()) ==
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
      return Status(
          RequestStatusCode::INTERNAL,
          "unsupported datatype " + DataType_Name(io.data_type()) +
              " for output '" + io.name() + "' for model '" + name_ + "'");
    }
  }

  return Status::Success;
}

void
OnnxBackend::Run(
    uint32_t runner_idx, std::vector<Scheduler::Payload>* payloads,
    std::function<void(Status)> OnCompleteQueuedPayloads)
{
  // Each runner executes using the corresponding context...
  if (runner_idx >= contexts_.size()) {
    OnCompleteQueuedPayloads(Status(
        RequestStatusCode::INTERNAL,
        "unexpected runner index" + std::to_string(runner_idx) +
            ", max allowed " + std::to_string(contexts_.size())));
    return;
  }

  std::vector<ModelInferStats::ScopedTimer> compute_timers;
  for (auto& payload : *payloads) {
    // Stop queue timer when the payload is scheduled to run
    if (payload.queue_timer_ != nullptr) {
      payload.queue_timer_.reset();
    }

    if (payload.stats_ != nullptr) {
      compute_timers.emplace_back();
      payload.stats_->StartComputeTimer(&compute_timers.back());
      payload.stats_->SetGPUDevice(contexts_[runner_idx]->gpu_device_);
    }
  }

  Status status = contexts_[runner_idx]->Run(this, payloads);
  // Release all run related resources regardless of the run status
  contexts_[runner_idx]->ReleaseOrtRunResources();
  OnCompleteQueuedPayloads(status);
}

Status
OnnxBackend::Context::Run(
    const OnnxBackend* base, std::vector<Scheduler::Payload>* payloads)
{
  LOG_VERBOSE(1) << "Running " << name_ << " with " << payloads->size()
                 << " request payloads";

  std::shared_ptr<InferRequestProvider> input_request_provider;

  // For each request in 'payloads' collect the total batch size for
  // this inference execution. The batch-size, number of inputs, and
  // size of each input has already been checked by each payloads
  // request provider so don't need to do that here.
  size_t total_batch_size = 0;
  for (auto& payload : *payloads) {
    if (!payload.status_.IsOk()) {
      return Status(
          RequestStatusCode::INTERNAL,
          "unexpected payload with non-OK status given to runner for '" +
              name_ + "'");
    }

    total_batch_size += payload.request_provider_->RequestHeader().batch_size();

    // All payloads must have equally-sized input tensors so use any
    // payload as the representative for the input tensors.
    input_request_provider = payload.request_provider_;
  }

  // If there are no valid payloads then no need to run the
  // inference. The payloads will have their error status set so can
  // just return.
  if (total_batch_size == 0) {
    return Status::Success;
  }

  // total_batch_size can be 1 for models that don't support batching
  // (i.e. max_batch_size_ == 0).
  if ((total_batch_size != 1) && (total_batch_size > (size_t)max_batch_size_)) {
    return Status(
        RequestStatusCode::INTERNAL,
        "dynamic batch size " + std::to_string(total_batch_size) + " for '" +
            name_ + "', max allowed is " + std::to_string(max_batch_size_));
  }

  // Hold reference to each buffer of input data so that it stays
  // until the inference has completed.
  std::vector<std::unique_ptr<char[]>> input_buffers;

  std::vector<const char*> input_names;

  for (const auto& input : input_request_provider->RequestHeader().input()) {
    const std::string& name = input.name();

    const ModelInput* input_config;
    RETURN_IF_ERROR(base->GetInput(name, &input_config));

    // Create a tensor for each input sized correctly for the total
    // payload batch size. Concatenate input values from each payload
    // into the corresponding tensor.
    RETURN_IF_ERROR(SetInputTensor(
        name, input_config->data_type(), input.dims(), total_batch_size,
        payloads, &input_buffers, &input_names));
  }

  // Additional inputs added to the provider...
  const std::shared_ptr<InferRequestProvider::InputOverrideMap>&
      input_override_map = input_request_provider->GetInputOverride();
  if (input_override_map != nullptr) {
    for (const auto& pr : *input_override_map) {
      const std::string& name = pr.first;
      const std::shared_ptr<InferRequestProvider::InputOverride>& override =
          pr.second;

      RETURN_IF_ERROR(SetInputTensor(
          name, override->datatype_, override->dims_, total_batch_size,
          payloads, &input_buffers, &input_names));
    }
  }

  // Request to retrieve all output specified in model config
  // and reserve placeholder for output tensors
  std::vector<const char*> output_names;
  for (const auto& output : base->Config().output()) {
    output_names.emplace_back(output.name().c_str());
    output_tensors_.emplace_back(nullptr);
  }

  // Run...
  RETURN_IF_ORT_ERROR(OrtRun(
      session_, NULL /* run options */, input_names.data(),
      (const OrtValue* const*)input_tensors_.data(), input_tensors_.size(),
      output_names.data(), output_names.size(), output_tensors_.data()));

  // Make sure each output is of the expected size and copy it into
  // the payload responses.
  return ReadOutputTensors(base, total_batch_size, output_names, payloads);
}

Status
OnnxBackend::Context::SetInputTensor(
    const std::string& name, const DataType datatype, const DimsList& dims,
    size_t total_batch_size, std::vector<Scheduler::Payload>* payloads,
    std::vector<std::unique_ptr<char[]>>* input_buffers,
    std::vector<const char*>* input_names)
{
  input_names->emplace_back(name.c_str());
  input_tensors_.emplace_back(nullptr);
  input_buffers->emplace_back();

  size_t batch1_element_cnt = 1;
  std::vector<int64_t> input_dims;

  // Only add batch dimension if the model support batching
  if (max_batch_size_ != NO_BATCHING) {
    input_dims.push_back(total_batch_size);
  }
  for (const auto dim : dims) {
    input_dims.push_back(dim);
    batch1_element_cnt *= dim;
  }

  const size_t batch1_byte_size =
      batch1_element_cnt * GetDataTypeByteSize(datatype);
  const size_t total_byte_size = total_batch_size * batch1_byte_size;

  input_buffers->back().reset(new char[total_byte_size]);
  char* buffer = input_buffers->back().get();

  // Visit the payloads in order and copy the input tensors to
  // 'buffer'.
  size_t buffer_copy_offset = 0;
  for (auto& payload : *payloads) {
    const InferRequestHeader& request_header =
        payload.request_provider_->RequestHeader();
    const size_t expected_byte_size =
        request_header.batch_size() * batch1_byte_size;

    size_t copied_byte_size = 0;
    while (payload.status_.IsOk()) {
      const void* content;
      size_t content_byte_size = expected_byte_size - copied_byte_size;
      payload.status_ = payload.request_provider_->GetNextInputContent(
          name, &content, &content_byte_size, false);
      if (!payload.status_.IsOk()) {
        break;
      }

      // No more input content available then done with copying...
      if (content == nullptr) {
        break;
      }

      if ((buffer_copy_offset + copied_byte_size + content_byte_size) >
          total_byte_size) {
        payload.status_ = Status(
            RequestStatusCode::INVALID_ARG,
            "unexpected size " +
                std::to_string(
                    buffer_copy_offset + copied_byte_size + content_byte_size) +
                " for inference input '" + name + "', expecting " +
                std::to_string(total_byte_size));
        break;
      }

      memcpy(
          static_cast<char*>(buffer) + buffer_copy_offset + copied_byte_size,
          content, content_byte_size);
      copied_byte_size += content_byte_size;
    }

    if (payload.status_.IsOk() && (copied_byte_size != expected_byte_size)) {
      payload.status_ = Status(
          RequestStatusCode::INTERNAL,
          "expected " + std::to_string(expected_byte_size) +
              " bytes of data for inference input '" + name + "', got " +
              std::to_string(copied_byte_size));
    }

    buffer_copy_offset += expected_byte_size;
  }

  // [TODO] not handling STRING data type right now, need to use other
  // Ort function to handle it.
  RETURN_IF_ORT_ERROR(OrtCreateTensorWithDataAsOrtValue(
      allocator_info_, (void*)buffer, total_byte_size, input_dims.data(),
      input_dims.size(), ConvertToOnnxDataType(datatype),
      &input_tensors_.back()));

  return Status::Success;
}

Status
OnnxBackend::Context::ReadOutputTensors(
    const OnnxBackend* base, size_t total_batch_size,
    const std::vector<const char*>& output_names,
    std::vector<Scheduler::Payload>* payloads)
{
  for (size_t idx = 0; idx < output_names.size(); idx++) {
    std::string name = std::string(output_names[idx]);

    const ModelOutput* output_config;
    RETURN_IF_ERROR(base->GetOutput(name, &output_config));

    OrtValue* output_tensor = output_tensors_[idx];
    if (output_tensor == nullptr) {
      return Status(
          RequestStatusCode::INTERNAL,
          "output tensor '" + name + "' does not found");
    }

    std::vector<int64_t> content_shape;
    char* content = nullptr;

    RETURN_IF_ORT_ERROR(
        OrtGetTensorMutableData(output_tensor, (void**)&content));

    // Get output type and shape
    OrtTypeInfo* typeinfo;
    RETURN_IF_ORT_ERROR(OrtGetTypeInfo(output_tensor, &typeinfo));
    const OrtTensorTypeAndShapeInfo* type_and_shape =
        OrtCastTypeInfoToTensorInfo(typeinfo);

    content_shape.resize(OrtGetNumOfDimensions(type_and_shape));
    OrtGetDimensions(
        type_and_shape, content_shape.data(), content_shape.size());
    ONNXTensorElementDataType type = OrtGetTensorElementType(type_and_shape);

    OrtReleaseTypeInfo(typeinfo);

    const size_t element_count = GetElementCount(content_shape);
    const size_t total_byte_size =
        element_count * GetDataTypeByteSize(ConvertFromOnnxDataType(type));
    const size_t actual_byte_size =
        element_count * GetDataTypeByteSize(output_config->data_type());
    const size_t batch1_byte_size = total_byte_size / total_batch_size;

    if (actual_byte_size != total_byte_size) {
      return Status(
          RequestStatusCode::INTERNAL,
          "unexpected size for output '" + name + "', byte-size " +
              std::to_string(actual_byte_size) + " does not equal " +
              std::to_string(total_batch_size) + " * " +
              std::to_string(batch1_byte_size));
    }

    size_t content_offset = 0;

    for (auto& payload : *payloads) {
      const InferRequestHeader& request_header =
          payload.request_provider_->RequestHeader();
      const size_t expected_byte_size =
          request_header.batch_size() * batch1_byte_size;

      // If 'payload' requested this output then copy it from
      // 'content'. If it did not request this output then just
      // skip it in the 'content'.
      if ((payload.response_provider_ != nullptr) &&
          payload.response_provider_->RequiresOutput(name)) {
        void* buffer;
        Status status = payload.response_provider_->AllocateOutputBuffer(
            name, &buffer, expected_byte_size, content_shape);
        if (status.IsOk()) {
          memcpy(buffer, content + content_offset, expected_byte_size);
        }

        if (!status.IsOk()) {
          payload.status_ = status;
        }
      }

      content_offset += expected_byte_size;
    }
  }
  return Status::Success;
}

void
OnnxBackend::Context::ReleaseOrtRunResources()
{
  // Release input tensor if set
  for (auto& tensor : input_tensors_) {
    if (tensor != nullptr) {
      OrtReleaseValue(tensor);
    }
  }
  input_tensors_.clear();

  // Release output tensor if set
  for (auto& tensor : output_tensors_) {
    if (tensor != nullptr) {
      OrtReleaseValue(tensor);
    }
  }
  output_tensors_.clear();
}

std::ostream&
operator<<(std::ostream& out, const OnnxBackend& pb)
{
  out << "name=" << pb.Name() << std::endl;
  out << "contexts:" << std::endl;
  for (const auto& context : pb.contexts_) {
    out << "  name=" << context->name_ << ", gpu="
        << ((context->gpu_device_ == OnnxBackend::Context::NO_GPU_DEVICE)
                ? "<none>"
                : std::to_string(context->gpu_device_))
        << ", max_batch_size="
        << ((context->max_batch_size_ == OnnxBackend::Context::NO_BATCHING)
                ? "<none>"
                : std::to_string(context->max_batch_size_))
        << std::endl;
  }

  return out;
}

}}  // namespace nvidia::inferenceserver
