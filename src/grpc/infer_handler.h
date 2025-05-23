// Copyright 2023-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
#pragma once

#include <grpc++/alarm.h>
#include <grpc++/grpc++.h>
#include <re2/re2.h>

#include <condition_variable>
#include <queue>
#include <regex>
#include <shared_mutex>
#include <thread>

#include "../tracer.h"
#include "grpc_handler.h"
#include "grpc_service.grpc.pb.h"
#include "grpc_utils.h"
#include "triton/common/logging.h"
#include "triton/core/tritonserver.h"

// Unique IDs are only needed when debugging. They only appear in
// verbose logging.
#ifndef NDEBUG
uint64_t NextUniqueId();
#define NEXT_UNIQUE_ID NextUniqueId()
#else
#define NEXT_UNIQUE_ID (0)
#endif  // NDEBUG

namespace triton { namespace server { namespace grpc {

// Options used in InferHandler/StreamInferHandler states that are set from
// request parameters
struct StateParameters {
  // Whether to generate an empty response when a FINAL flag is received with
  // no corresponding response. Only applicable to StreamInferHandlerState.
  bool enable_empty_final_response_ = false;
};

//
// C++11 doesn't have a barrier so we implement our own.
//
class Barrier {
 public:
  explicit Barrier(size_t cnt) : threshold_(cnt), count_(cnt), generation_(0) {}

  void Wait()
  {
    std::unique_lock<std::mutex> lock(mu_);
    auto lgen = generation_;
    if (--count_ == 0) {
      generation_++;
      count_ = threshold_;
      cv_.notify_all();
    } else {
      cv_.wait(lock, [this, lgen] { return lgen != generation_; });
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  const size_t threshold_;
  size_t count_;
  size_t generation_;
};

// Simple structure that carries the userp payload needed for
// request release callback.
struct RequestReleasePayload final {
  explicit RequestReleasePayload(
      const std::shared_ptr<TRITONSERVER_InferenceRequest>& inference_request)
      : inference_request_(inference_request){};

 private:
  std::shared_ptr<TRITONSERVER_InferenceRequest> inference_request_ = nullptr;
};

//
// ResponseQueue
//
// This class implements a queue to manage responses that need to be written.
// It internally uses a reusable pool of persistent message objects to avoid
// allocating memory for each response individually.
//
template <typename ResponseType>
class ResponseQueue {
 public:
  explicit ResponseQueue(const size_t max_response_queue_size)
      : max_response_queue_size_(max_response_queue_size)
  {
    Reset();
  }

  ~ResponseQueue()
  {
    // Delete all responses in the reusable pool
    for (auto response : reusable_pool_) {
      delete response;
    }

    // Delete all responses currently in the queue
    for (auto response : responses_) {
      delete response;
    }
  }

  // Resets the queue to its initial state
  void Reset()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    alloc_count_ = 0;
    ready_count_ = 0;
    pop_count_ = 0;

    while (!responses_.empty()) {
      responses_.front()->Clear();
      reusable_pool_.push_back(responses_.front());
      responses_.pop_front();
    }
  }

  // Gets the response for the non-decoupled models.
  // Note that there will be a single response in
  // non-decoupled cases.
  ResponseType* GetNonDecoupledResponse()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    alloc_count_ = 1;
    if (responses_.size() < 1) {
      if (!reusable_pool_.empty()) {
        responses_.push_back(reusable_pool_.front());
        reusable_pool_.pop_front();
      } else {
        responses_.push_back(new ResponseType());
      }
    }
    return responses_[0];
  }

  // Allocates a response at the end of the queue
  void AllocateResponse()
  {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(
        lock, [this] { return responses_.size() < max_response_queue_size_; });
    alloc_count_++;

    // Use a response from the reusable pool if available
    if (!reusable_pool_.empty()) {
      responses_.push_back(reusable_pool_.front());
      reusable_pool_.pop_front();
    } else {
      responses_.push_back(new ResponseType());
    }
  }

  // Gets the last allocated response
  ResponseType* GetLastAllocatedResponse()
  {
    std::lock_guard<std::mutex> lock(mtx_);

    // Ensure that the requested response has been allocated
    if ((responses_.size() + pop_count_) < alloc_count_) {
      LOG_ERROR
          << "[INTERNAL] Attempting to access the response not yet allocated";
      return nullptr;
    }

    return responses_.back();
  }

  // Marks the next non-ready response complete
  bool MarkNextResponseComplete()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (alloc_count_ <= ready_count_) {
      LOG_ERROR
          << "[INTERNAL] Attempting to mark an unallocated response complete";
      return false;
    }
    ready_count_++;

    return true;
  }

  // Gets the current response from the front of the queue
  ResponseType* GetCurrentResponse()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (pop_count_ >= ready_count_) {
      LOG_ERROR << "[INTERNAL] Attempting to access current response when it "
                   "is not ready";
      return nullptr;
    }
    if (responses_.empty()) {
      LOG_ERROR << "[INTERNAL] No responses are available in the queue.";
      return nullptr;
    }

    return responses_.front();
  }

  // Gets the response at the specified index
  ResponseType* GetResponseAt(const uint32_t index)
  {
    std::lock_guard<std::mutex> lock(mtx_);

    // Check if the index is valid for allocated responses
    if (index >= alloc_count_) {
      LOG_ERROR << "[INTERNAL] Attempting to access response which is not yet "
                   "allocated";
      return nullptr;
    }
    if (index < pop_count_) {
      LOG_ERROR << "[INTERNAL] Attempting to access a response that has "
                   "already been removed from the queue.";
      return nullptr;
    }

    // Adjust index based on number of popped responses to get actual index in
    // 'responses_'
    return responses_[index - pop_count_];
  }

  // Removes the current response from the front of the queue
  void PopResponse()
  {
    std::lock_guard<std::mutex> lock(mtx_);

    // Ensure there are responses in the queue to pop
    if (responses_.empty()) {
      LOG_ERROR << "[INTERNAL] No responses in the queue to pop.";
      return;
    }

    // Clear and move the current response to the reusable pool
    auto response = responses_.front();
    response->Clear();
    reusable_pool_.push_back(response);
    responses_.pop_front();
    pop_count_++;

    cv_.notify_one();
  }

  // Returns whether the queue is empty
  bool IsEmpty()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return (
        (alloc_count_ == ready_count_) && (alloc_count_ == pop_count_) &&
        responses_.empty());
  }

  // Returns whether the queue has responses
  // ready to be written.
  bool HasReadyResponse()
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return (ready_count_ > pop_count_);
  }

 private:
  // Stores responses that need to be written. The front of the queue indicates
  // the current response, while the back indicates the last allocated response.
  std::deque<ResponseType*> responses_;
  // Stores completed responses that can be reused
  std::deque<ResponseType*> reusable_pool_;
  std::condition_variable cv_;
  size_t max_response_queue_size_;
  std::mutex mtx_;

  // Three counters are used to track and manage responses in the queue
  uint32_t alloc_count_;  // Number of allocated responses
  uint32_t ready_count_;  // Number of ready-to-write responses
  uint32_t pop_count_;    // Number of removed responses from the queue
};


//
// ShmInfo
//
// Simple structure that carries the shared memory information
//
struct ShmInfo {
  ShmInfo(
      void* base, size_t byte_size, TRITONSERVER_MemoryType memory_type,
      int64_t memory_type_id, char* cuda_ipc_handle)
      : base_(base), byte_size_(byte_size), memory_type_(memory_type),
        memory_type_id_(memory_type_id), cuda_ipc_handle_(cuda_ipc_handle)
  {
  }
  void* base_;
  size_t byte_size_;
  TRITONSERVER_MemoryType memory_type_;
  int64_t memory_type_id_;
  char* cuda_ipc_handle_;
};


using TensorShmMap = std::unordered_map<std::string, ShmInfo>;

//
// AllocPayload
//
// Simple structure that carries the userp payload needed for
// allocation.
//
template <typename ResponseType>
struct AllocPayload {
  using ClassificationMap = std::unordered_map<std::string, uint32_t>;

  explicit AllocPayload() : response_queue_(nullptr) {}
  ~AllocPayload()
  {
    // Don't delete 'response_'.. it is owned by the InferHandlerState
  }

  std::shared_ptr<ResponseQueue<ResponseType>> response_queue_;
  uint32_t response_alloc_count_;
  TensorShmMap shm_map_;
  ClassificationMap classification_map_;

  // Used to extend the lifetime of the serialized data in case
  // non-raw contents were provided in the request. Serialized data's
  // actual lifetime is that of the request whereas AllocPayload's
  // lifetime is that of a response... but it is convenient to keep it
  // here.
  std::list<std::string> serialized_data_;
};

template <typename ResponseType>
TRITONSERVER_Error*
InferAllocatorPayload(
    const std::shared_ptr<TRITONSERVER_Server>& tritonserver,
    const std::shared_ptr<SharedMemoryManager>& shm_manager,
    const inference::ModelInferRequest& request,
    std::list<std::string>&& serialized_data,
    std::shared_ptr<ResponseQueue<ResponseType>> response_queue,
    AllocPayload<ResponseType>* alloc_payload,
    std::vector<std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo>>*
        shm_regions_info)
{
  alloc_payload->response_queue_ = response_queue;
  alloc_payload->shm_map_.clear();
  alloc_payload->classification_map_.clear();
  alloc_payload->serialized_data_ = std::move(serialized_data);

  // If any of the outputs use shared memory, then we must calculate
  // the memory address for that output and store it in the allocator
  // payload so that it is available when the allocation callback is
  // invoked.
  for (const auto& io : request.outputs()) {
    std::string region_name;
    int64_t offset;
    size_t byte_size;
    bool has_shared_memory;
    RETURN_IF_ERR(ParseSharedMemoryParams<
                  inference::ModelInferRequest::InferRequestedOutputTensor>(
        io, &has_shared_memory, &region_name, &offset, &byte_size));

    bool has_classification;
    uint32_t classification_count;
    RETURN_IF_ERR(ParseClassificationParams(
        io, &has_classification, &classification_count));

    if (has_shared_memory && has_classification) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INVALID_ARG,
          "output can't set both 'shared_memory_region' and "
          "'classification'");
    }

    if (has_shared_memory) {
      void* base;
      TRITONSERVER_MemoryType memory_type;
      int64_t memory_type_id;
      std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo> shm_info =
          nullptr;
      RETURN_IF_ERR(shm_manager->GetMemoryInfo(
          region_name, offset, byte_size, &base, &memory_type, &memory_type_id,
          &shm_info));
      shm_regions_info->emplace_back(shm_info);

      if (memory_type == TRITONSERVER_MEMORY_GPU) {
#ifdef TRITON_ENABLE_GPU
        char* cuda_handle;
        RETURN_IF_ERR(shm_manager->GetCUDAHandle(
            region_name, reinterpret_cast<cudaIpcMemHandle_t**>(&cuda_handle)));
        alloc_payload->shm_map_.emplace(
            io.name(),
            ShmInfo(base, byte_size, memory_type, memory_type_id, cuda_handle));
#endif
      } else {
        alloc_payload->shm_map_.emplace(
            io.name(), ShmInfo(
                           base, byte_size, memory_type, memory_type_id,
                           nullptr /* cuda_ipc_handle */));
      }
    } else if (has_classification) {
      alloc_payload->classification_map_.emplace(
          io.name(), classification_count);
    }
  }

  return nullptr;  // Success
}

TRITONSERVER_Error* InferGRPCToInputHelper(
    const std::string& input_name, const std::string& model_name,
    const TRITONSERVER_DataType tensor_dt, const TRITONSERVER_DataType input_dt,
    const size_t binary_data_byte_size);

TRITONSERVER_Error* InferGRPCToInput(
    const std::shared_ptr<TRITONSERVER_Server>& tritonserver,
    const std::shared_ptr<SharedMemoryManager>& shm_manager,
    const inference::ModelInferRequest& request,
    std::list<std::string>* serialized_data,
    TRITONSERVER_InferenceRequest* inference_request,
    std::vector<std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo>>*
        shm_regions_info);

TRITONSERVER_Error* ResponseAllocatorHelper(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t byte_size, TRITONSERVER_MemoryType preferred_memory_type,
    int64_t preferred_memory_type_id, inference::ModelInferResponse* response,
    const TensorShmMap& shm_map, void** buffer, void** buffer_userp,
    TRITONSERVER_MemoryType* actual_memory_type,
    int64_t* actual_memory_type_id);

TRITONSERVER_Error* OutputBufferAttributesHelper(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    const TensorShmMap& shm_map,
    TRITONSERVER_BufferAttributes* buffer_attributes);

TRITONSERVER_Error* OutputBufferQueryHelper(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t* byte_size, const TensorShmMap& shm_map,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id);

// Make sure to keep InferResponseAlloc and OutputBufferQuery logic in sync
TRITONSERVER_Error* InferResponseAlloc(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t byte_size, TRITONSERVER_MemoryType preferred_memory_type,
    int64_t preferred_memory_type_id, void* userp, void** buffer,
    void** buffer_userp, TRITONSERVER_MemoryType* actual_memory_type,
    int64_t* actual_memory_type_id);

TRITONSERVER_Error* SetInferenceRequestMetadata(
    TRITONSERVER_InferenceRequest* inference_request,
    const inference::ModelInferRequest& request, StateParameters& state_params);

// Helper to set options for StreamInferHandler state when parsing
// request parameters.
TRITONSERVER_Error* SetStateParameterFromTritonParameter(
    StateParameters& state_params,
    const std::pair<std::string, inference::InferParameter>& param);

void InferRequestComplete(
    TRITONSERVER_InferenceRequest* request, const uint32_t flags, void* userp);

// Make sure to keep InferResponseAlloc and OutputBufferQuery logic in sync
TRITONSERVER_Error* OutputBufferQuery(
    TRITONSERVER_ResponseAllocator* allocator, void* userp,
    const char* tensor_name, size_t* byte_size,
    TRITONSERVER_MemoryType* memory_type, int64_t* memory_type_id);

// Make sure to keep InferResponseAlloc, OutputBufferQuery, and
// OutputBufferAttributes logic in sync
TRITONSERVER_Error* OutputBufferAttributes(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    TRITONSERVER_BufferAttributes* buffer_attributes, void* userp,
    void* buffer_userp);

TRITONSERVER_Error* InferResponseFree(
    TRITONSERVER_ResponseAllocator* allocator, void* buffer, void* buffer_userp,
    size_t byte_size, TRITONSERVER_MemoryType memory_type,
    int64_t memory_type_id);

TRITONSERVER_Error* InferResponseStart(
    TRITONSERVER_ResponseAllocator* allocator, void* userp);

template <typename ResponseType>
TRITONSERVER_Error*
InferResponseCompleteCommon(
    TRITONSERVER_Server* server, TRITONSERVER_InferenceResponse* iresponse,
    inference::ModelInferResponse& response,
    const AllocPayload<ResponseType>& alloc_payload)
{
  RETURN_IF_ERR(TRITONSERVER_InferenceResponseError(iresponse));

  const char *model_name, *id;
  int64_t model_version;
  RETURN_IF_ERR(TRITONSERVER_InferenceResponseModel(
      iresponse, &model_name, &model_version));
  RETURN_IF_ERR(TRITONSERVER_InferenceResponseId(iresponse, &id));

  response.set_id(id);
  response.set_model_name(model_name);
  response.set_model_version(std::to_string(model_version));

  // Propagate response parameters.
  uint32_t parameter_count;
  RETURN_IF_ERR(TRITONSERVER_InferenceResponseParameterCount(
      iresponse, &parameter_count));
  for (uint32_t pidx = 0; pidx < parameter_count; ++pidx) {
    const char* name;
    TRITONSERVER_ParameterType type;
    const void* vvalue;
    RETURN_IF_ERR(TRITONSERVER_InferenceResponseParameter(
        iresponse, pidx, &name, &type, &vvalue));
    inference::InferParameter& param = (*response.mutable_parameters())[name];
    switch (type) {
      case TRITONSERVER_PARAMETER_BOOL:
        param.set_bool_param(*(reinterpret_cast<const bool*>(vvalue)));
        break;
      case TRITONSERVER_PARAMETER_INT:
        param.set_int64_param(*(reinterpret_cast<const int64_t*>(vvalue)));
        break;
      case TRITONSERVER_PARAMETER_STRING:
        param.set_string_param(reinterpret_cast<const char*>(vvalue));
        break;
      case TRITONSERVER_PARAMETER_DOUBLE:
        param.set_double_param(*(reinterpret_cast<const double*>(vvalue)));
        break;
      case TRITONSERVER_PARAMETER_BYTES:
        return TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_UNSUPPORTED,
            "Response parameter of type 'TRITONSERVER_PARAMETER_BYTES' is not "
            "currently supported");
        break;
    }
  }

  // Go through each response output and transfer information to the
  // corresponding GRPC response output.
  uint32_t output_count;
  RETURN_IF_ERR(
      TRITONSERVER_InferenceResponseOutputCount(iresponse, &output_count));
  if (output_count != (uint32_t)response.outputs_size()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL, "response output count mismatch");
  }

  for (uint32_t output_idx = 0; output_idx < output_count; ++output_idx) {
    const char* cname;
    TRITONSERVER_DataType datatype;
    const int64_t* shape;
    uint64_t dim_count;
    const void* base;
    size_t byte_size;
    TRITONSERVER_MemoryType memory_type;
    int64_t memory_type_id;
    void* userp;

    RETURN_IF_ERR(TRITONSERVER_InferenceResponseOutput(
        iresponse, output_idx, &cname, &datatype, &shape, &dim_count, &base,
        &byte_size, &memory_type, &memory_type_id, &userp));

    const std::string name(cname);

    // There are usually very few outputs so fastest just to look for
    // the one we want... could create a map for cases where there are
    // a large number of outputs. Or rely on order to be same...
    inference::ModelInferResponse::InferOutputTensor* output = nullptr;
    for (auto& io : *(response.mutable_outputs())) {
      if (io.name() == name) {
        output = &io;
        break;
      }
    }

    if (output == nullptr) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          "unable to find expected response output");
    }

    // If this output was requested as classification then remove the
    // raw output from the response and instead return classification
    // results as a string tensor
    const auto itr = alloc_payload.classification_map_.find(name);
    if (itr == alloc_payload.classification_map_.end()) {
      // Not classification...
      output->set_datatype(TRITONSERVER_DataTypeString(datatype));
      for (size_t idx = 0; idx < dim_count; idx++) {
        output->add_shape(shape[idx]);
      }
    } else {
      // Classification
      const uint32_t classification_count = itr->second;

      // For classification need to determine the batch size, if any,
      // because need to use that to break up the response for each
      // batch entry.
      uint32_t batch_size = 0;

      uint32_t batch_flags;
      RETURN_IF_ERR(TRITONSERVER_ServerModelBatchProperties(
          server, model_name, model_version, &batch_flags,
          nullptr /* voidp */));
      if ((dim_count > 0) &&
          ((batch_flags & TRITONSERVER_BATCH_FIRST_DIM) != 0)) {
        batch_size = shape[0];
      }

      // Determine the batch1 byte size of the tensor... needed when
      // the response tensor batch-size > 1 so that we know how to
      // stride though the tensor data.
      size_t batch1_element_count = 1;
      for (size_t idx = ((batch_size == 0) ? 0 : 1); idx < dim_count; idx++) {
        batch1_element_count *= shape[idx];
      }

      const size_t batch1_byte_size =
          batch1_element_count * TRITONSERVER_DataTypeByteSize(datatype);

      // Create the classification contents
      std::string serialized;

      size_t class_offset = 0;
      for (uint32_t bs = 0; bs < std::max((uint32_t)1, batch_size); ++bs) {
        std::vector<std::string> class_strs;
        RETURN_IF_ERR(TopkClassifications(
            iresponse, output_idx,
            reinterpret_cast<const char*>(base) + class_offset,
            ((class_offset + batch1_byte_size) > byte_size) ? 0
                                                            : batch1_byte_size,
            datatype, classification_count, &class_strs));

        // Serialize for binary representation...
        for (const auto& str : class_strs) {
          uint32_t len = str.size();
          serialized.append(reinterpret_cast<const char*>(&len), sizeof(len));
          if (len > 0) {
            serialized.append(str);
          }
        }

        class_offset += batch1_byte_size;
      }

      // Update the output with new datatype, shape and contents.
      output->set_datatype(
          TRITONSERVER_DataTypeString(TRITONSERVER_TYPE_BYTES));

      if (batch_size > 0) {
        output->add_shape(batch_size);
      }
      output->add_shape(
          std::min(classification_count, (uint32_t)batch1_element_count));

      (*response.mutable_raw_output_contents())[output_idx] =
          std::move(serialized);
    }
  }

  // Make sure response doesn't exceed GRPC limits.
  if (response.ByteSizeLong() > MAX_GRPC_MESSAGE_SIZE) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string(
            "Response has byte size " +
            std::to_string(response.ByteSizeLong()) +
            " which exceeds gRPC's byte size limit " + std::to_string(INT_MAX) +
            ".")
            .c_str());
  }

  return nullptr;  // success
}

//
// InferHandlerState
//
template <
    typename ServerResponderType, typename RequestType, typename ResponseType>
class InferHandlerState {
 public:
  using InferHandlerStateType =
      InferHandlerState<ServerResponderType, RequestType, ResponseType>;

  // State that is shared across all state objects that make up a GRPC
  // transaction (e.g. a stream).
  struct Context {
    explicit Context(
        ::grpc::ServerCompletionQueue* cq, const uint64_t unique_id = 0)
        : cq_(cq), unique_id_(unique_id), ongoing_requests_(0),
          step_(Steps::START), finish_ok_(true), ongoing_write_(false),
          received_notification_(false)
    {
      ctx_.reset(new ::grpc::ServerContext());
      responder_.reset(new ServerResponderType(ctx_.get()));
      gRPCErrorTracker_ = std::make_unique<gRPCErrorTracker>();
    }

    void SetCompressionLevel(grpc_compression_level compression_level)
    {
      ctx_->set_compression_level(compression_level);
    }

    void GrpcContextAsyncNotifyWhenDone(InferHandlerStateType* state)
    {
      notify_state_ = std::unique_ptr<InferHandlerStateType>(
          new InferHandlerStateType(Steps::WAITING_NOTIFICATION, state));
      ctx_->AsyncNotifyWhenDone(notify_state_.get());
    }

    void SetReceivedNotification(bool value) { received_notification_ = value; }

    bool ReceivedNotification() { return received_notification_; }

    bool IsCancelled()
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      return received_notification_
                 ? (ctx_->IsCancelled() ||
                    gRPCErrorTracker_->CheckAndUpdateGRPCError())
                 : false;
    }
    // Increments the ongoing request counter
    void IncrementRequestCounter() { ongoing_requests_++; }

    // Decrements the ongoing request counter
    void DecrementRequestCounter() { ongoing_requests_--; }

    // Adds the state object created on this context
    void InsertState(InferHandlerStateType* state)
    {
      all_states_.insert(state);
    }

    // Erases the state object created on this context
    void EraseState(InferHandlerStateType* state)
    {
      EraseInflightState(state);
      all_states_.erase(state);
    }

    bool HandleCompletion()
    {
      if (step_ != Steps::FINISH) {
        for (auto state : all_states_) {
          std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
          // There is no order guarantee on when the AsyncNotifyWhenDone
          // event is placed on the completion queue vs when the actual
          // state RPC is processed. Need to transition through two steps
          // to preserve the lifetime of the state object.
          if (state->step_ == Steps::PARTIAL_COMPLETION) {
            state->step_ = Steps::COMPLETE;
          } else {
            state->step_ = Steps::FINISH;
          }
          PutTaskBackToQueue(state);
        }
        step_ = Steps::FINISH;
        return true;
      }
      return false;
    }

    // Extracts headers from GRPC request and updates state
    void ExtractStateFromHeaders(InferHandlerStateType* state)
    {
      const auto& metadata = state->context_->ctx_->client_metadata();
      std::string triton_grpc_error_key = "triton_grpc_error";

      auto it = metadata.find(
          {triton_grpc_error_key.data(), triton_grpc_error_key.size()});

      if (it != metadata.end()) {
        if (it->second == "true") {
          LOG_VERBOSE(2)
              << "GRPC: triton_grpc_error mode detected in new grpc stream";
          state->context_->gRPCErrorTracker_->triton_grpc_error_ = true;
        }
      }
    }

    void WriteGRPCErrorResponse(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(state->context_->mu_);
      // Check if Error not responded previously
      // Avoid closing connection twice on multiple errors from core
      if (!state->context_->gRPCErrorTracker_->GRPCErrorEncountered()) {
        state->step_ = Steps::COMPLETE;
        state->context_->responder_->Finish(state->status_, state);
        // Mark error for this stream
        state->context_->gRPCErrorTracker_->MarkGRPCErrorEncountered();
      }
    }

    const std::string DebugString(InferHandlerStateType* state)
    {
      std::string debug_string("");
      debug_string.append(
          "Running state_id " + std::to_string(state->unique_id_) + "\n");
      debug_string.append(
          "\tContext step " + std::to_string(state->context_->step_) + " id " +
          std::to_string(state->context_->unique_id_) + "\n");
      for (auto new_state : all_states_) {
        debug_string.append(
            "\t\t State id " + std::to_string(new_state->unique_id_) +
            ": State step " + std::to_string(new_state->step_) + "\n");
      }

      return debug_string;
    }

    // Inserts the state to a set tracking active requests
    // within the server core. Should only be called when
    // the request was successfully enqueued on Triton.
    void InsertInflightState(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      inflight_states_.insert(state);
    }

    // Erases the state to a set tracking active requests
    // within the server core.
    void EraseInflightState(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      inflight_states_.erase(state);
    }

    // Issues the cancellation for all inflight requests
    // being tracked by this context.
    void IssueRequestCancellation()
    {
      {
        std::lock_guard<std::recursive_mutex> lock(mu_);

        // Issues the request cancellation to the core.
        for (auto state : inflight_states_) {
          std::lock_guard<std::recursive_mutex> lock(state->step_mtx_);
          if (state->step_ != Steps::CANCELLED &&
              state->step_ != Steps::COMPLETE) {
            LOG_VERBOSE(1) << "Issuing cancellation for " << state->unique_id_
                           << " step " << state->step_;
            if (state->inference_request_.get() == nullptr) {
              // The context might be holding some states that have
              // not been issued to Triton core. Need to skip calling
              // issuing cancellation for such requests.
              continue;
            }
            // Note that request may or may not be valid at this point.
            // Assuming if RequestComplete callback is run asynchronously
            // before this point.
            TRITONSERVER_Error* err = nullptr;
            err = TRITONSERVER_InferenceRequestCancel(
                state->inference_request_.get());
            // TODO: Add request id to the message
            if (err != nullptr) {
              LOG_INFO << "Failed to cancel the request: "
                       << TRITONSERVER_ErrorMessage(err);
            }
            state->step_ = Steps::CANCELLATION_ISSUED;
          } else if (state->step_ == Steps::COMPLETE) {
            // The RPC is complete and no callback will be invoked to retrieve
            // the object. Hence, need to explicitly place the state on the
            // completion queue.
            PutTaskBackToQueue(state);
          }
        }
      }
    }


    // Handles the gRPC context cancellation. This function can be called
    // multiple times and is supposed to be re-entrant.
    // Returns whether or not to continue cycling through the gRPC
    // completion queue or not.
    bool HandleCancellation(
        InferHandlerStateType* state, bool rpc_ok, const std::string& name)
    {
      // Check to avoid early exit in case of triton_grpc_error
      if (!IsCancelled()) {
        LOG_ERROR
            << "[INTERNAL] HandleCancellation called even when the context was "
               "not cancelled for "
            << name << ", rpc_ok=" << rpc_ok << ", context "
            << state->context_->unique_id_ << ", " << state->unique_id_
            << " step " << state->step_;
        return true;
      }

      if (state->step_ != Steps::CANCELLATION_ISSUED) {
        // If the context has not been cancelled then
        // issue cancellation request to all the inflight
        // states belonging to the context.
        // It means this is the first time we are hiting this line for this grpc
        // transaction.
        if ((state->step_ != Steps::CANCELLED) &&
            (state->context_->step_ != Steps::CANCELLED)) {
          // Issue the request cancellation as it has not been cancelled yet.
          IssueRequestCancellation();
          // Mark the context as cancelled
          state->context_->step_ = Steps::CANCELLED;
          // The state returns true because the CancelExecution
          // call above would have raised alarm objects on all
          // pending inflight states objects. This state will
          // be taken up along with all the other states in the
          // next iteration from the completion queue which
          // would release the state.
          return true;
        } else {
          // The cancellation request has been handled so the state can be
          // released.
          LOG_VERBOSE(1) << "Completing cancellation for " << name
                         << ", rpc_ok=" << rpc_ok << ", context "
                         << state->context_->unique_id_ << ", "
                         << state->unique_id_ << " step " << state->step_;
          return false;
        }
      } else {  // state->step_ == Steps::CANCELLATION_ISSUED
        // Should wait for the InferResponseComplete callbacks to be invoked.
        LOG_VERBOSE(1)
            << "Waiting for the callback to retrieve cancellation for " << name
            << ", rpc_ok=" << rpc_ok << ", context "
            << state->context_->unique_id_ << ", " << state->unique_id_
            << " step " << state->step_;
        return true;
      }
    }

    // Enqueue 'state' so that its response is delivered in the
    // correct order.
    void EnqueueForResponse(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      states_.push(state);
    }

    // Write the response to the stream directly.
    void DecoupledWriteResponse(InferHandlerStateType* state)
    {
#ifdef TRITON_ENABLE_TRACING
      state->trace_timestamps_.emplace_back(
          std::make_pair("GRPC_SEND_START", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING
      state->step_ = Steps::WRITTEN;
      ResponseType* response = state->response_queue_->GetCurrentResponse();
      responder_->Write(*response, state);

      // Clear the response after writing
      response->mutable_infer_response()->Clear();

      // Pop the response from queue
      state->response_queue_->PopResponse();
    }

    // Adds the state object to the completion queue so
    // that it can be processed later
    void PutTaskBackToQueue(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      // FIXME: Is there a better way to put task on the
      // completion queue rather than using alarm object?
      // The alarm object will add a new task to the back of the
      // completion queue when it expires or when it’s cancelled.
      state->alarm_.Set(
          cq_, gpr_now(gpr_clock_type::GPR_CLOCK_REALTIME), state);
    }

    // Check the state at the front of the queue and write it if
    // ready. The state at the front of the queue is ready if it is in
    // the WRITEREADY state and it equals 'required_state' (or
    // 'required_state' is nullptr). Return nullptr if front of queue
    // was not ready (and so not written), or return the state if it
    // was ready and written.
    InferHandlerStateType* WriteResponseIfReady(
        InferHandlerStateType* required_state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (states_.empty()) {
        return nullptr;
      }

      InferHandlerStateType* state = states_.front();
      if (state->step_ != Steps::WRITEREADY) {
        return nullptr;
      }

      if ((required_state != nullptr) && (state != required_state)) {
        return nullptr;
      }

#ifdef TRITON_ENABLE_TRACING
      state->trace_timestamps_.emplace_back(
          std::make_pair("GRPC_SEND_START", TraceManager::CaptureTimestamp()));
#endif  // TRITON_ENABLE_TRACING

      state->step_ = Steps::WRITTEN;
      state->context_->ongoing_write_ = true;
      // Non decoupled writes use only one response
      responder_->Write(*state->response_queue_->GetResponseAt(0), state);

      return state;
    }

    // If 'state' is at the front of the queue and written, pop it and
    // return true. Other return false.
    bool PopCompletedResponse(InferHandlerStateType* state)
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (states_.empty()) {
        return false;
      }

      InferHandlerStateType* front = states_.front();
      if ((front == state) && (state->step_ == Steps::WRITTEN)) {
        states_.pop();
        return true;
      }

      return false;
    }

    // Return true if this context has completed all reads and writes.
    bool IsRequestsCompleted()
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      return (
          (step_ == Steps::WRITEREADY) && states_.empty() &&
          (ongoing_requests_ == 0));
    }

    // The grpc completion queue associated with the RPC.
    ::grpc::ServerCompletionQueue* cq_;

    // Unique ID for the context. Used only for debugging so will
    // always be 0 in non-debug builds.
    const uint64_t unique_id_;

    // Context for the rpc, allowing to tweak aspects of it such as
    // the use of compression, authentication, as well as to send
    // metadata back to the client.
    std::unique_ptr<::grpc::ServerContext> ctx_;
    std::unique_ptr<ServerResponderType> responder_;

    // The states associated with this context that are currently
    // active. Used by stream handlers to maintain request / response
    // orders. A state enters this queue when it has successfully read
    // a request and exits the queue when it is written.
    std::recursive_mutex mu_;
    std::queue<InferHandlerStateType*> states_;
    std::atomic<uint32_t> ongoing_requests_;

    // Tracks the inflight requests sent to Triton core via this
    // context. We will use this structure to issue cancellations
    // on these requests.
    std::set<InferHandlerStateType*> inflight_states_;

    // Tracks all the states that have been created on this context.
    std::set<InferHandlerStateType*> all_states_;

    // Ready to write queue for decoupled
    std::queue<InferHandlerStateType*> ready_to_write_states_;

    // The step of the entire context.
    Steps step_;

    // True if this context should finish with OK status, false if
    // should finish with CANCELLED status.
    bool finish_ok_;

    // True if there is an ongoing write to the grpc stream
    std::atomic<bool> ongoing_write_;

    // The state object that is sent to grpc async notification
    // for tracking the gRPC stream.
    std::unique_ptr<InferHandlerState> notify_state_;

    // Tracks whether the async notification has been delivered by
    // completion queue.
    bool received_notification_;

    std::unique_ptr<gRPCErrorTracker> gRPCErrorTracker_;
  };

  // This constructor is used to build a wrapper state object
  // pointing to the actual state object. The wrapper state
  // object is used to distinguish a tag from AsyncNotifyWhenDone()
  // signal.
  explicit InferHandlerState(Steps start_step, InferHandlerState* state)
      : step_(start_step), state_ptr_(state), async_notify_state_(false)
  {
    state->MarkAsAsyncNotifyState();
  }

  explicit InferHandlerState(
      TRITONSERVER_Server* tritonserver, const size_t max_response_queue_size,
      const std::shared_ptr<Context>& context, Steps start_step = Steps::START)
      : tritonserver_(tritonserver), async_notify_state_(false)
  {
    // For debugging and testing
    delay_response_ms_ = ParseDebugVariable("TRITONSERVER_DELAY_GRPC_RESPONSE");
    delay_complete_ms_ = ParseDebugVariable("TRITONSERVER_DELAY_GRPC_COMPLETE");
    delay_process_ms_ = ParseDebugVariable("TRITONSERVER_DELAY_GRPC_PROCESS");
    delay_process_entry_ms_ =
        ParseDebugVariable("TRITONSERVER_DELAY_GRPC_PROCESS_ENTRY");
    delay_notification_process_entry_ms_ =
        ParseDebugVariable("TRITONSERVER_DELAY_GRPC_NOTIFICATION");
    delay_response_complete_exec_ms_ =
        ParseDebugVariable("TRITONSERVER_DELAY_RESPONSE_COMPLETE_EXEC");
    delay_enqueue_ms_ = ParseDebugVariable("TRITONSERVER_DELAY_GRPC_ENQUEUE");
    delay_response_completion_ms_ =
        ParseDebugVariable("TRITONSERVER_DELAY_RESPONSE_COMPLETION");

    response_queue_.reset(
        new ResponseQueue<ResponseType>(max_response_queue_size));
    Reset(context, start_step);
  }

  ~InferHandlerState() { ClearTraceTimestamps(); }

  int ParseDebugVariable(const char* env_str)
  {
    const char* str = getenv(env_str);
    int val = 0;
    if (str != nullptr) {
      try {
        val = std::stoi(str);
      }
      catch (const std::invalid_argument& e) {
        LOG_ERROR << "Unable to parse the debug variable " << env_str
                  << ". Value provided: '" << str
                  << "' is not a valid integer. Error: " << e.what();
      }
      catch (const std::out_of_range& e) {
        LOG_ERROR << "Unable to parse the debug variable " << env_str
                  << ". Value provided: '" << str
                  << "' is out of range for an integer. Error: " << e.what();
      }
      catch (const std::exception& e) {
        LOG_ERROR
            << "An unexpected error occurred while parsing the debug variable "
            << env_str << " with value '" << str << "'. Error: " << e.what();
      }
    }
    return val;
  }

  bool IsGrpcContextCancelled() { return context_->IsCancelled(); }

  void Reset(
      const std::shared_ptr<Context>& context, Steps start_step = Steps::START)
  {
    unique_id_ = NEXT_UNIQUE_ID;
    context_ = context;
    step_ = start_step;
    status_ = ::grpc::Status{};
    cb_count_ = 0;
    is_decoupled_ = false;
    complete_ = false;
    parameters_ = {};
    request_.Clear();
    response_queue_->Reset();
    // Clear trace_timestamps_ here so they do not grow indefinitely since
    // states are re-used for performance.
    ClearTraceTimestamps();
    // The pointer should be nullptr for all state objects instead of
    // wrapper state object in WAITING_NOTIFICATION step.
    state_ptr_ = nullptr;
    async_notify_state_ = false;
  }

  void Release()
  {
    context_ = nullptr;
    inference_request_.reset();
    ClearTraceTimestamps();
  }

  void ClearTraceTimestamps()
  {
#ifdef TRITON_ENABLE_TRACING
    if (trace_ != nullptr) {
      for (const auto& timestamp : trace_timestamps_) {
        trace_->CaptureTimestamp(timestamp.first, timestamp.second);
      }
      trace_.reset();
    }
    trace_timestamps_.clear();
#endif  // TRITON_ENABLE_TRACING
  }

  // Returns whether all the responses from the state
  // are delivered and successfully written on the
  // stream.
  bool IsComplete() { return (complete_ && response_queue_->IsEmpty()); }

  void MarkAsAsyncNotifyState() { async_notify_state_ = true; }
  bool IsAsyncNotifyState() { return async_notify_state_; }
  // Needed in the response handle for classification outputs.
  TRITONSERVER_Server* tritonserver_;

  // Unique ID for the state. Used only for debugging so will
  // always be 0 in non-debug builds.
  uint64_t unique_id_;

  std::shared_ptr<Context> context_;
  Steps step_;
  std::recursive_mutex step_mtx_;

  // Shared pointer to the inference request object. The lifetime of
  // inference request object is extended till all the responses from
  // the request are processed and the request is released.
  std::shared_ptr<TRITONSERVER_InferenceRequest> inference_request_;

#ifdef TRITON_ENABLE_TRACING
  std::shared_ptr<TraceManager::Trace> trace_;
  // Additional timestamps that are captured before a trace stream is acquired
  std::deque<std::pair<std::string, uint64_t>> trace_timestamps_;
#endif  // TRITON_ENABLE_TRACING

  bool is_decoupled_ = false;
  StateParameters parameters_;

  ::grpc::Status status_;
  std::atomic<uint32_t> cb_count_;
  bool complete_;

  RequestType request_;
  std::shared_ptr<ResponseQueue<ResponseType>> response_queue_;

  ::grpc::Alarm alarm_;

  // For testing and debugging
  int delay_response_ms_;
  int delay_complete_ms_;
  int delay_process_ms_;
  int delay_process_entry_ms_;
  int delay_notification_process_entry_ms_;
  int delay_response_complete_exec_ms_;
  int delay_enqueue_ms_;
  int delay_response_completion_ms_;

  // For inference requests the allocator payload, unused for other
  // requests.
  AllocPayload<ResponseType> alloc_payload_;

  // The below pointer is only set when using this state object as a
  // wrapper over actual state when being sent to completion queue
  // using AsyncNotifyWhenDone function. Otherwise it is nullptr.
  InferHandlerState* state_ptr_;

  // Tracks whether this state object has been wrapped and send to
  // AsyncNotifyWhenDone() function as a tag.
  bool async_notify_state_;
};


//
// InferHandler
//
template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
class InferHandler : public HandlerBase {
 public:
  InferHandler(
      const std::string& name,
      const std::shared_ptr<TRITONSERVER_Server>& tritonserver,
      ServiceType* service, ::grpc::ServerCompletionQueue* cq,
      size_t max_state_bucket_count, size_t max_response_queue_size,
      std::pair<std::string, std::string> restricted_kv,
      const std::string& header_forward_pattern, std::shared_mutex* conn_mtx,
      std::atomic<uint32_t>* conn_cnt, bool* accepting_new_conn);
  virtual ~InferHandler();

  // Descriptive name of of the handler.
  const std::string& Name() const { return name_; }

  // Start handling requests.
  void Start() override;

  // Stop handling requests.
  void Stop() override;

 protected:
  void IncrementConnectionCount() { conn_cnt_->fetch_add(1); }
  void DecrementConnectionCount() { conn_cnt_->fetch_sub(1); }

  using State =
      InferHandlerState<ServerResponderType, RequestType, ResponseType>;
  using StateContext = typename State::Context;

  State* StateNew(
      TRITONSERVER_Server* tritonserver,
      const std::shared_ptr<StateContext>& context,
      Steps start_step = Steps::START)
  {
    IncrementConnectionCount();

    State* state = nullptr;

    if (max_state_bucket_count_ > 0) {
      std::lock_guard<std::mutex> lock(alloc_mu_);

      if (!state_bucket_.empty()) {
        state = state_bucket_.back();
        state->Reset(context, start_step);
        state_bucket_.pop_back();
      }
    }

    if (state == nullptr) {
      state = new State(
          tritonserver, max_response_queue_size_, context, start_step);
    }

    if (start_step == Steps::START) {
      // Need to be called to receive an asynchronous notification
      // when the transaction is cancelled.
      context->GrpcContextAsyncNotifyWhenDone(state);
    }
    context->InsertState(state);

    LOG_VERBOSE(2) << "StateNew, " << state->unique_id_ << " Step "
                   << state->step_;

    return state;
  }

  void StateRelease(State* state)
  {
    LOG_VERBOSE(2) << "StateRelease, " << state->unique_id_ << " Step "
                   << state->step_;
    if (max_state_bucket_count_ > 0) {
      std::lock_guard<std::mutex> lock(alloc_mu_);

      if (state_bucket_.size() < max_state_bucket_count_) {
        state->Release();
        state_bucket_.push_back(state);
        DecrementConnectionCount();
        return;
      }
    }

    delete state;
    DecrementConnectionCount();
  }

  // Simple structure that carries the payload needed for
  // response release callback.
  struct ResponseReleasePayload final {
    State* state_;
    std::vector<std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo>>
        shm_regions_info_;
    std::shared_ptr<SharedMemoryManager> shm_manager_;

    ResponseReleasePayload(
        State* state,
        std::vector<
            std::shared_ptr<const SharedMemoryManager::SharedMemoryInfo>>&&
            shm_regions_info,
        const std::shared_ptr<SharedMemoryManager>& shm_manager)
        : state_(state), shm_regions_info_(std::move(shm_regions_info)),
          shm_manager_(shm_manager)
    {
    }

    ~ResponseReleasePayload()
    {
      // Unregister shm regions that are waiting for the completion of an
      // inference.
      while (!shm_regions_info_.empty()) {
        auto shm_name = shm_regions_info_.back()->name_;
        auto shm_memory_type = shm_regions_info_.back()->kind_;
        auto awaiting_unregister =
            shm_regions_info_.back()->awaiting_unregister_;

        // Delete shared_ptr to decrement reference count
        shm_regions_info_.pop_back();

        if (awaiting_unregister) {
          if (shm_manager_ != nullptr) {
            auto err = shm_manager_->Unregister(shm_name, shm_memory_type);
            if (err != nullptr) {
              LOG_VERBOSE(1) << TRITONSERVER_ErrorMessage(err);
            }
          } else {
            LOG_VERBOSE(1) << "Shared memory manager is not available";
          }
        }
      }
    }
  };

  virtual void StartNewRequest() = 0;
  virtual bool Process(State* state, bool rpc_ok, bool is_notification) = 0;
  bool ExecutePrecondition(InferHandler::State* state);

  TRITONSERVER_Error* ForwardHeadersAsParameters(
      TRITONSERVER_InferenceRequest* irequest, InferHandler::State* state);

  const std::string name_;
  std::shared_ptr<TRITONSERVER_Server> tritonserver_;

  ServiceType* service_;
  ::grpc::ServerCompletionQueue* cq_;
  std::unique_ptr<std::thread> thread_;

  // Mutex to serialize State allocation
  std::mutex alloc_mu_;

  // Keep some number of state objects for reuse to avoid the overhead
  // of creating a state for every new request.
  const size_t max_state_bucket_count_;
  std::vector<State*> state_bucket_;

  const size_t max_response_queue_size_;
  std::pair<std::string, std::string> restricted_kv_;
  std::string header_forward_pattern_;
  re2::RE2 header_forward_regex_;

  std::shared_mutex* conn_mtx_;
  std::atomic<uint32_t>* conn_cnt_;
  bool* accepting_new_conn_;
};

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
InferHandler<ServiceType, ServerResponderType, RequestType, ResponseType>::
    InferHandler(
        const std::string& name,
        const std::shared_ptr<TRITONSERVER_Server>& tritonserver,
        ServiceType* service, ::grpc::ServerCompletionQueue* cq,
        size_t max_state_bucket_count, size_t max_response_queue_size,
        std::pair<std::string, std::string> restricted_kv,
        const std::string& header_forward_pattern, std::shared_mutex* conn_mtx,
        std::atomic<uint32_t>* conn_cnt, bool* accepting_new_conn)
    : name_(name), tritonserver_(tritonserver), service_(service), cq_(cq),
      max_state_bucket_count_(max_state_bucket_count),
      max_response_queue_size_(max_response_queue_size),
      restricted_kv_(restricted_kv),
      header_forward_pattern_(header_forward_pattern),
      header_forward_regex_(header_forward_pattern_), conn_mtx_(conn_mtx),
      conn_cnt_(conn_cnt), accepting_new_conn_(accepting_new_conn)
{
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
InferHandler<ServiceType, ServerResponderType, RequestType, ResponseType>::
    ~InferHandler()
{
  for (State* state : state_bucket_) {
    delete state;
  }
  state_bucket_.clear();

  LOG_VERBOSE(1) << "Destructed " << Name();
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
void
InferHandler<
    ServiceType, ServerResponderType, RequestType, ResponseType>::Start()
{
  // Use a barrier to make sure we don't return until thread has
  // started.
  auto barrier = std::make_shared<Barrier>(2);

  thread_.reset(new std::thread([this, barrier] {
    StartNewRequest();
    barrier->Wait();

    void* tag;
    bool ok;

    while (cq_->Next(&tag, &ok)) {
      State* state = static_cast<State*>(tag);
      bool is_notification = false;
      if (state->step_ == Steps::WAITING_NOTIFICATION) {
        State* state_wrapper = state;
        state = state_wrapper->state_ptr_;
        is_notification = true;
        LOG_VERBOSE(1) << "Received notification for " << Name() << ", "
                       << state->unique_id_;

        if (state->delay_notification_process_entry_ms_ != 0) {
          // Will delay the entry to Process by the specified time.
          // This can be used to test the flow when
          // 1. cancellation request issued for the request, which invokes
          // InferResponseComplete callback right before Process.
          // 2. cancellation request issued for the request during
          // InferResponseComplete callback right before Process in the
          // notification thread.
          LOG_INFO
              << "Delaying the entry to Process for notification thread by "
              << state->delay_notification_process_entry_ms_ << " ms...";
          std::this_thread::sleep_for(std::chrono::milliseconds(
              state->delay_notification_process_entry_ms_));
        }
      } else {
        if (state->delay_process_entry_ms_ != 0) {
          // Will delay the entry to Process by the specified time.
          LOG_INFO << "Delaying the entry to Process thread by "
                   << state->delay_process_entry_ms_ << " ms...";
          std::this_thread::sleep_for(
              std::chrono::milliseconds(state->delay_process_entry_ms_));
        }
      }

      LOG_VERBOSE(2) << "Grpc::CQ::Next() "
                     << state->context_->DebugString(state);
      if (!Process(state, ok, is_notification)) {
        LOG_VERBOSE(1) << "Done for " << Name() << ", " << state->unique_id_;
        state->context_->EraseState(state);
        StateRelease(state);
      } else {
        // In non-streaming infer mode which has multiple request handlers,
        // there is no guarantee state->context_ is valid beyond this line.
        LOG_VERBOSE(2) << "Returning from " << Name() << ", "
                       << state->unique_id_ << ", " << state->step_;
      }
    }
  }));

  barrier->Wait();
  LOG_VERBOSE(1) << "Thread started for " << Name();
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
void
InferHandler<
    ServiceType, ServerResponderType, RequestType, ResponseType>::Stop()
{
  if (thread_->joinable()) {
    thread_->join();
  }

  LOG_VERBOSE(1) << "Thread exited for " << Name();
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
bool
InferHandler<ServiceType, ServerResponderType, RequestType, ResponseType>::
    ExecutePrecondition(InferHandler::State* state)
{
  if (!restricted_kv_.first.empty()) {
    const auto& metadata = state->context_->ctx_->client_metadata();
    const auto it = metadata.find(restricted_kv_.first);
    return (it != metadata.end()) && (it->second == restricted_kv_.second);
  }
  return true;
}

template <
    typename ServiceType, typename ServerResponderType, typename RequestType,
    typename ResponseType>
TRITONSERVER_Error*
InferHandler<ServiceType, ServerResponderType, RequestType, ResponseType>::
    ForwardHeadersAsParameters(
        TRITONSERVER_InferenceRequest* irequest, InferHandler::State* state)
{
  TRITONSERVER_Error* err = nullptr;
  if (!header_forward_pattern_.empty()) {
    const auto& metadata = state->context_->ctx_->client_metadata();
    for (const auto& pair : metadata) {
      auto& key = pair.first;
      auto& value = pair.second;
      std::string param_key = std::string(key.begin(), key.end());
      if (RE2::PartialMatch(param_key, header_forward_regex_)) {
        std::string param_value = std::string(value.begin(), value.end());
        err = TRITONSERVER_InferenceRequestSetStringParameter(
            irequest, param_key.c_str(), param_value.c_str());
        if (err != nullptr) {
          break;
        }
      }
    }
  }

  return err;
}

//
// ModelInferHandler
//
class ModelInferHandler
    : public InferHandler<
          inference::GRPCInferenceService::AsyncService,
          ::grpc::ServerAsyncResponseWriter<inference::ModelInferResponse>,
          inference::ModelInferRequest, inference::ModelInferResponse> {
 public:
  ModelInferHandler(
      const std::string& name,
      const std::shared_ptr<TRITONSERVER_Server>& tritonserver,
      TraceManager* trace_manager,
      const std::shared_ptr<SharedMemoryManager>& shm_manager,
      inference::GRPCInferenceService::AsyncService* service,
      ::grpc::ServerCompletionQueue* cq, size_t max_state_bucket_count,
      size_t max_response_queue_size, grpc_compression_level compression_level,
      std::pair<std::string, std::string> restricted_kv,
      const std::string& forward_header_pattern, std::shared_mutex* conn_mtx,
      std::atomic<uint32_t>* conn_cnt, bool* accepting_new_conn)
      : InferHandler(
            name, tritonserver, service, cq, max_state_bucket_count,
            max_response_queue_size, restricted_kv, forward_header_pattern,
            conn_mtx, conn_cnt, accepting_new_conn),
        trace_manager_(trace_manager), shm_manager_(shm_manager),
        compression_level_(compression_level)
  {
    // Create the allocator that will be used to allocate buffers for
    // the result tensors.
    FAIL_IF_ERR(
        TRITONSERVER_ResponseAllocatorNew(
            &allocator_, InferResponseAlloc, InferResponseFree,
            InferResponseStart),
        "creating inference response allocator");
    FAIL_IF_ERR(
        TRITONSERVER_ResponseAllocatorSetQueryFunction(
            allocator_, OutputBufferQuery),
        "setting allocator's query function");
    FAIL_IF_ERR(
        TRITONSERVER_ResponseAllocatorSetBufferAttributesFunction(
            allocator_, OutputBufferAttributes),
        "setting allocator's output buffer attributes function");
  }

  ~ModelInferHandler()
  {
    LOG_TRITONSERVER_ERROR(
        TRITONSERVER_ResponseAllocatorDelete(allocator_),
        "deleting response allocator");
  }

 protected:
  void StartNewRequest() override;
  bool Process(State* state, bool rpc_ok, bool is_notification) override;

 private:
  void Execute(State* state);
  static void InferResponseComplete(
      TRITONSERVER_InferenceResponse* response, const uint32_t flags,
      void* userp);

  TraceManager* trace_manager_;
  std::shared_ptr<SharedMemoryManager> shm_manager_;
  TRITONSERVER_ResponseAllocator* allocator_;

  grpc_compression_level compression_level_;
};

#if !defined(_WIN32) && defined(TRITON_ENABLE_TRACING)
class GrpcServerCarrier : public otel_cntxt::propagation::TextMapCarrier {
 public:
  GrpcServerCarrier(::grpc::ServerContext* context) : context_(context) {}
  GrpcServerCarrier() = default;
  virtual opentelemetry::nostd::string_view Get(
      opentelemetry::nostd::string_view key) const noexcept override
  {
    auto it = context_->client_metadata().find({key.data(), key.size()});
    if (it != context_->client_metadata().end()) {
      return it->second.data();
    }
    return "";
  }

  // Not required on server side
  virtual void Set(
      opentelemetry::nostd::string_view key,
      opentelemetry::nostd::string_view value) noexcept override
  {
    return;
  }

  ::grpc::ServerContext* context_;
};
#else
using GrpcServerCarrier = void*;
#endif  // TRITON_ENABLE_TRACING

}}}  // namespace triton::server::grpc
