#include "ccv_nnc_mfa.hpp"
#include "ccv_nnc_mfa_hash.hpp"
using namespace ccv::nnc;

#include <string>

// MARK: - C

void ccv_nnc_mfa_async_prepare_gemm(mfa::context* context, ccv_nnc_mfa_gemm_params_t params)
{
  mfa::gemm::hash hash(params);
  if (context->gemm_cache.find(hash) == context->gemm_cache.end()) {
    auto* pipeline = new mfa::gemm::pipeline(context, hash);
    context->gemm_cache[hash] = pipeline;
  }
}

void ccv_nnc_mfa_encode_gemm(mfa::context* context, ccv_nnc_mfa_gemm_params_t params, MTL::CommandBatch* command_batch, MTL::Buffer** tensors, size_t* tensor_offsets)
{
  mfa::gemm::hash hash(params);
  auto iterator = context->gemm_cache.find(hash);
  if (iterator == context->gemm_cache.end()) {
    mfa::precondition_failure("GEMM hash not cached.", __LINE__, __FILE__, __FUNCTION__);
  }
  
  auto* pipeline = context->gemm_cache.extract(iterator).mapped();
  pipeline->wait();
  
  auto* encoder = command_batch->command_encoder;
  encoder->setComputePipelineState(pipeline->get_pso());
  encoder->setThreadgroupMemoryLength(pipeline->get_threadgroup_memory_length(), 0);
  
  int num_tensors = 0;
  while (tensors[num_tensors] != nullptr) {
    num_tensors += 1;
  }
  CCV_NNC_MFA_PRECONDITION(num_tensors == 3)
  for (int i = 0; i < num_tensors; ++i) {
    encoder->setBuffer(tensors[i], tensor_offsets[i], i);
  }
  
  uint32_t batch_size = 1;
  auto grid_size = pipeline->get_grid_size();
  grid_size.depth = batch_size;
  encoder->dispatchThreadgroups(grid_size, pipeline->get_group_size());
}

// MARK: - C++

mfa::gemm::hash::hash(ccv_nnc_mfa_gemm_params_t params) {
  data_type = params.data_type;
  M = params.M;
  N = params.N;
  K = params.K;
  A_trans = params.A_trans;
  B_trans = params.B_trans;
  alpha = params.alpha;
  beta = params.beta;
  batched = params.batched;
  fused_activation = params.fused_activation;
}

bool mfa::gemm::hash::operator==(const mfa::gemm::hash& hash) const {
  return (memcmp(this, &hash, sizeof(hash)) == 0);
}

std::size_t std::hash<mfa::gemm::hash>::operator()(const mfa::gemm::hash& hash) const noexcept {
  std::size_t seed = 0;
  mfa::hash::combine_64(seed, hash.data_type);
  mfa::hash::combine_32(seed, hash.M);
  mfa::hash::combine_32(seed, hash.N);
  mfa::hash::combine_32(seed, hash.K);
  mfa::hash::combine_32(seed, uint32_t(hash.A_trans));
  mfa::hash::combine_32(seed, uint32_t(hash.B_trans));
  mfa::hash::combine_32(seed, *reinterpret_cast<const uint32_t*>(&hash.alpha));
  mfa::hash::combine_32(seed, *reinterpret_cast<const uint32_t*>(&hash.beta));
  mfa::hash::combine_32(seed, uint32_t(hash.batched));
  mfa::hash::combine_32(seed, uint32_t(hash.fused_activation));
  return seed;
}

mfa::gemm::pipeline::pipeline(mfa::context* context, mfa::gemm::hash hash) : semaphore(0) {
  CCV_NNC_MFA_PRECONDITION((hash.data_type == MTL::DataTypeFloat) || (hash.data_type == MTL::DataTypeHalf))
  CCV_NNC_MFA_PRECONDITION(hash.A_trans == false)
  CCV_NNC_MFA_PRECONDITION(hash.B_trans == false)
  CCV_NNC_MFA_PRECONDITION(hash.alpha == 1.0)
  CCV_NNC_MFA_PRECONDITION(hash.beta == 0.0)
  CCV_NNC_MFA_PRECONDITION(hash.batched == false)
  CCV_NNC_MFA_PRECONDITION(hash.fused_activation == false)
  
  auto* pool = NS::AutoreleasePool::alloc()->init();
  
  auto constants = NS::TransferPtr(MTL::FunctionConstantValues::alloc()->init());
  constants->setConstantValue(&hash.M, MTL::DataTypeUInt, NS::UInteger(0));
  constants->setConstantValue(&hash.N, MTL::DataTypeUInt, 1);
  constants->setConstantValue(&hash.K, MTL::DataTypeUInt, 2);
  constants->setConstantValue(&hash.A_trans, MTL::DataTypeBool, 10);
  constants->setConstantValue(&hash.B_trans, MTL::DataTypeBool, 11);
  constants->setConstantValue(&hash.alpha, MTL::DataTypeFloat, 20);
  constants->setConstantValue(&hash.beta, MTL::DataTypeFloat, 21);
  constants->setConstantValue(&hash.batched, MTL::DataTypeBool, 100);
  constants->setConstantValue(&hash.fused_activation, MTL::DataTypeBool, 101);
  
  // 32x32x32 block size for now.
  uint16_t M_group = 32;
  uint16_t N_group = 32;
  uint16_t K_group = 32;
  uint16_t M_splits = 2;
  uint16_t N_splits = 2;
  uint16_t K_splits = 1;
  uint16_t M_simd = M_group / M_splits;
  uint16_t N_simd = N_group / N_splits;
  uint16_t K_simd = K_group / K_splits;
  
  constants->setConstantValue(&M_simd, MTL::DataTypeUShort, 200);
  constants->setConstantValue(&N_simd, MTL::DataTypeUShort, 201);
  constants->setConstantValue(&K_simd, MTL::DataTypeUShort, 202);
  constants->setConstantValue(&M_splits, MTL::DataTypeUShort, 210);
  constants->setConstantValue(&N_splits, MTL::DataTypeUShort, 211);
  constants->setConstantValue(&K_splits, MTL::DataTypeUShort, 212);
  
  std::string cpp_name;
  uint16_t data_type_size = UINT16_MAX;
  switch (hash.data_type) {
    case MTL::DataTypeHalf: {
      cpp_name = "hgemm";
      data_type_size = 2;
      break;
    }
    case MTL::DataTypeFloat: {
      cpp_name = "sgemm";
      data_type_size = 4;
      break;
    }
    default: {
      CCV_NNC_MFA_PRECONDITION(false)
      break;
    }
  }
  auto* swift_name = NS::String::string(cpp_name.c_str(), NS::UTF8StringEncoding);
  
  uint16_t A_block_bytes = M_group * K_group * data_type_size;
  uint16_t B_block_bytes = K_group * N_group * data_type_size;
  threadgroup_memory_length = A_block_bytes + B_block_bytes;
  
  std::function<size_t(size_t, uint16_t)> ceil_divide = [](size_t original, uint16_t granularity) {
    return (original + size_t(granularity) - 1) / size_t(granularity);
  };
  grid_size = MTL::Size(ceil_divide(hash.N, N_group), ceil_divide(hash.M, M_group), 1);
  group_size = MTL::Size(128 * K_splits, 1, 1);
  
  context->library->newFunction(swift_name, constants.get(), [context, this](MTL::Function* pFunction, NS::Error* error) {
    CCV_NNC_MFA_CHECK_ERROR(error)
    auto function = NS::TransferPtr(pFunction);
   
    pso = NS::TransferPtr(context->device->newComputePipelineState(function.get(), &error));
    CCV_NNC_MFA_CHECK_ERROR(error)
    
    semaphore.signal();
  });
  
  pool->drain();
}

void mfa::gemm::pipeline::wait() {
  if (!finished) {
    semaphore.wait();
    finished = true;
  }
}

MTL::ComputePipelineState* mfa::gemm::pipeline::get_pso() const {
  if (finished) {
    return pso.get();
  } else {
    return nullptr;
  }
}

uint16_t mfa::gemm::pipeline::get_threadgroup_memory_length() const {
  if (finished) {
    return threadgroup_memory_length;
  } else {
    return UINT16_MAX;
  }
}

MTL::Size mfa::gemm::pipeline::get_grid_size() const {
  if (finished) {
    return grid_size;
  } else {
    return MTL::Size(0, UINT64_MAX, UINT64_MAX);
  }
}

MTL::Size mfa::gemm::pipeline::get_group_size() const {
  if (finished) {
    return group_size;
  } else {
    return MTL::Size(0, UINT64_MAX, UINT64_MAX);
  }
}