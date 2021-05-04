#include <ATen/native/TensorTransformations.h>
#include <ATen/WrapDimUtilsMulti.h>

#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <c10/util/Exception.h>

#include <algorithm>
#include <vector>

namespace at {
namespace native {

DEFINE_DISPATCH(flip_stub);

constexpr size_t dim_bitset_size = 64;

template <typename scalar_t>
void inline flip_old_cpu_kernel(
  const int64_t total_dims,
  const std::vector<int64_t>& stride_contiguous_v,
  const std::bitset<dim_bitset_size>& flip_dims_b,
  const Tensor& in_tensor,
  Tensor& out_tensor
){
  const int64_t numel = in_tensor.numel();
  const scalar_t* in_tensor_d = in_tensor.data_ptr<scalar_t>();
  scalar_t* out_tensor_d = out_tensor.data_ptr<scalar_t>();
  auto sizes_v = in_tensor.sizes().vec();
  auto strides_v = in_tensor.strides().vec();

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  at::parallel_for(0, numel, 1000, [&](int64_t start, int64_t end) {
    for (auto i = start; i < end; i++) {
      int64_t cur_indices = i;
      int64_t rem = 0;
      int64_t dst_offset = 0;

      for (int64_t d = 0; d < total_dims; d++) {
        int64_t temp = cur_indices;
        cur_indices = cur_indices / stride_contiguous_v[d];
        rem = temp - cur_indices * stride_contiguous_v[d];
        dst_offset += flip_dims_b[d] ? (sizes_v[d] - 1 - cur_indices) * strides_v[d] : cur_indices * strides_v[d];
        cur_indices = rem;
      }
      out_tensor_d[i] = in_tensor_d[dst_offset];
    }
  });
}


Tensor flip_old_cpu(const Tensor& self, IntArrayRef dims) {
  auto in_tensor = self;
  const int64_t total_dims = in_tensor.dim();
  auto flip_dims_b = at::dim_list_to_bitset(dims, total_dims);
  Tensor out_tensor = at::empty_like(in_tensor, LEGACY_CONTIGUOUS_MEMORY_FORMAT);

  // create contiguous strides for input tensor
  auto stride_contiguous_v = std::vector<int64_t>(total_dims);
  for (int64_t i = total_dims - 1; i >= 0; i--) {
    if (i == total_dims - 1) {
      stride_contiguous_v[i] = 1;
    } else {
      stride_contiguous_v[i] = std::max<int64_t>(in_tensor.size(i + 1), 1) * stride_contiguous_v[i + 1];
    }
  }

  if (in_tensor.is_quantized()) {
    // NOLINTNEXTLINE(clang-diagnostic-unused-variable)
    AT_DISPATCH_QINT_AND_SUB_BYTE_TYPES(in_tensor.scalar_type(),
                                        "flip_quantized_cpu", [&] {
      flip_old_cpu_kernel<scalar_t>(
        total_dims,
        stride_contiguous_v,
        flip_dims_b,
        in_tensor,
        out_tensor
      );
    });
  } else {
    AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(kBool, kHalf, kBFloat16,
                                          in_tensor.scalar_type(),
                                          "flip_cpu", [&] {
      flip_old_cpu_kernel<scalar_t>(
        total_dims,
        stride_contiguous_v,
        flip_dims_b,
        in_tensor,
        out_tensor
      );
    });
  }

  return out_tensor;
}

Tensor build_index(int64_t num_dims, int64_t flip_dim, int64_t dim_size) {
  auto new_shape = std::vector<int64_t>(num_dims, 1);
  new_shape[flip_dim] = dim_size;

  TensorOptions tensor_options =
    TensorOptions(c10::kLong).
    device(c10::kCPU);

  return at::empty(new_shape, tensor_options);
}


std::vector<Tensor> build_indices_loop(Tensor input, IntArrayRef flip_dims) {
  std::vector<Tensor> indices;
  int64_t element_size_bytes = input.element_size();
  for(auto dim: flip_dims) {
    auto dim_size = input.size(dim);
    auto index = build_index(input.ndimension(), dim, dim_size);
    auto stride = input.stride(dim);
    auto input_index_ptr = index.data_ptr<int64_t>();

    for(int64_t i = 0; i < dim_size; i++) {
      input_index_ptr[i] = static_cast<int64_t>(dim_size - i - 1) * stride * element_size_bytes;
    }
    indices.push_back(index);
  }
  return indices;
}

static TensorIterator make_index_iterator(const Tensor input, const std::vector<Tensor> indices) {
  TensorIteratorConfig config;
  config.set_check_mem_overlap(false)
        .check_all_same_dtype(false)
        .declare_static_dtype_and_device(input.scalar_type(), input.device())
        .add_output(Tensor())
        .add_input(input);
  for (auto& index : indices) {
    config.add_input(index);
  }
  return config.build();
}

struct Indexer {
  Indexer(int64_t num_indexers, char** indexers, const int64_t* indexer_strides)
      : num_indexers(num_indexers),
        indexers(indexers),
        indexer_strides(indexer_strides) {}

  int64_t num_indexers;
  char** indexers;
  const int64_t* indexer_strides;

  int64_t get(int64_t idx) {
    int64_t offset = *(int64_t*)&indexers[0][idx * indexer_strides[0]];
    for (int j = 1; j < num_indexers; j++) {
      offset += *(int64_t*)&indexers[j][idx * indexer_strides[j]];
    }
    return offset;
  }
};

template <typename scalar_t>
void flip_cpu_kernel(TensorIterator& iter) {
  int ntensor = iter.ntensors();
  // When launch the index parallel version, set a relative samll grain size
  // less than the INTERNAL::GRAIN_SIZE to make the whole available thread
  // numbers get more balanced work load and a better cache location. The grain
  // size here is chosen by the op benchmark to overcome the thread launch
  // overhead This value was taken from the AdvancedIndexing kernel.
  const int index_parallel_grain_size = 3000;
  auto loop = [&](char** data, const int64_t* strides, int64_t n) {
    auto indexer = Indexer(ntensor - 2, &data[2], &strides[2]);
    char* dst = data[0];
    char* src = data[1];

    for (int64_t i = 0; i < n; i++) {
      int64_t offset = indexer.get(i);
      *(scalar_t*)(dst + strides[0] * i) =
          *(scalar_t*)(src + strides[1] * i + offset);
    }
  };

  iter.for_each(loop, index_parallel_grain_size);
}

Tensor flip_cpu(const Tensor& self, IntArrayRef dims) {
  auto input = self;
  const int64_t total_dims = input.dim();
  auto flip_dims_b = at::dim_list_to_bitset(dims, total_dims);

  std::vector<int64_t> flip_dims;
  for(int64_t i = 0; i < total_dims; i++) {
      if(flip_dims_b[i]) {
        flip_dims.push_back(i);
      }
  }

  auto shape = input.sizes().vec();
  auto strides = input.strides().vec();

  // Set stride to zero on the dimensions that are going to be flipped
  for(auto dim: flip_dims) {
    strides[dim] = 0;
  }

  // Restride the input to index only on the dimensions to flip
  auto restrided_input = input.as_strided(shape, strides);
  auto indices = build_indices_loop(input, flip_dims);
  auto iter = make_index_iterator(restrided_input, indices);

  flip_stub(iter.device_type(), iter, input);

  auto result = iter.output();
  return result;
}

Tensor flip_cpu_internal(const Tensor& self, IntArrayRef dims) {
  auto input = self;
  const int64_t total_dims = input.dim();
  auto flip_dims_b = at::dim_list_to_bitset(dims, total_dims);

  std::vector<int64_t> flip_dims;
  for(int64_t i = 0; i < total_dims; i++) {
      if(flip_dims_b[i]) {
        flip_dims.push_back(i);
      }
  }

  auto shape = input.sizes().vec();
  auto strides = input.strides().vec();

  // Set stride to zero on the dimensions that are going to be flipped
  for(auto dim: flip_dims) {
    strides[dim] = 0;
  }

  // Restride the input to index only on the dimensions to flip
  auto restrided_input = input.as_strided(shape, strides);
  auto indices = build_indices_loop(input, flip_dims);
  auto iter = make_index_iterator(restrided_input, indices);

  if (input.is_quantized()) {
    AT_DISPATCH_QINT_AND_SUB_BYTE_TYPES(
        input.scalar_type(), "flip_quantized_cpu", [&] {
          flip_cpu_kernel<scalar_t>(iter);
        });
  } else {
    AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
        kBool, kHalf, kBFloat16, input.scalar_type(), "flip_cpu", [&] {
          flip_cpu_kernel<scalar_t>(iter);
        });
  }

  auto result = iter.output();
  return result;
}

Tensor roll_cpu(const Tensor& self, IntArrayRef shifts, IntArrayRef dims) {
  if (dims.size() != 1 || shifts.size() != 1) {
    return roll_common(self, shifts, dims);
  }
  // avoid a div zero error below.
  if (self.numel() == 0) {
    return self.clone(at::MemoryFormat::Preserve);
  }
  int64_t dim = dims[0];
  int64_t size = self.size(dim);
  int64_t start = (size - shifts[0]) % size;
  // Behavior of % is different in C++ vs Python for negative numbers. This
  // corrects the difference.
  if (start < 0) {
    start = start + size;
  }
  auto t0 = self.narrow(dim, start, size-start);
  auto t1 = self.narrow(dim, 0, start);
  return at::cat({t0, t1}, dim);
}

Tensor rot90(const Tensor& self, int64_t k, IntArrayRef dims) {
  const int64_t total_dims = self.dim(), total_rot_dims = dims.size();

  TORCH_CHECK(total_rot_dims == 2,
    "expected total rotation dims == 2, but got dims = ", total_rot_dims);

  TORCH_CHECK(total_dims >= 2,
    "expected total dims >= 2, but got total dims = ", total_dims);

  TORCH_CHECK(dims[0] != dims[1] && std::abs(dims[0] - dims[1]) != total_dims,
    "expected rotation dims to be different, but got dim0 = ", dims[0],
    " and dim1 = ", dims[1]);

  // check range of dims
  TORCH_CHECK(dims[0] < total_dims && dims[0] >= -total_dims,
    "Rotation dim0 out of range, dim0 = ", dims[0]);

  TORCH_CHECK(dims[1] < total_dims && dims[1] >= -total_dims,
    "Rotation dim1 out of range, dim1 = ", dims[1]);

  // handle modulo with negative k
  k = (4 + (k % 4)) % 4;

  switch(k) {
    case 1:
      return self.flip({dims[1]}).transpose_(dims[0], dims[1]);
    case 2:
      return self.flip(dims);
    case 3:
      return self.flip({dims[0]}).transpose_(dims[0], dims[1]);
    default:
      return self.clone(at::MemoryFormat::Contiguous);
  }
}

Tensor fliplr(const Tensor& self) {
  TORCH_CHECK(self.dim() >= 2, "Input must be >= 2-d.");

  return self.flip({1});
}

Tensor flipud(const Tensor& self) {
  TORCH_CHECK(self.dim() >= 1, "Input must be >= 1-d.");

  return self.flip({0});
}

Tensor atleast_1d(const Tensor& self) {
  switch (self.dim()) {
    case 0:
      return self.reshape({1});
    default:
      return self;
  }
}

std::vector<Tensor> atleast_1d(TensorList tensors) {
  std::vector<Tensor> result(tensors.size());
  auto transform_lambda = [](const Tensor& input) -> Tensor {
    return at::native::atleast_1d(input);
  };
  std::transform(tensors.cbegin(), tensors.cend(), result.begin(), transform_lambda);
  return result;
}

Tensor atleast_2d(const Tensor& self) {
  switch (self.dim()) {
    case 0:
      return self.reshape({1, 1});
    case 1: {
      return self.unsqueeze(0);
    }
    default:
      return self;
  }
}

std::vector<Tensor> atleast_2d(TensorList tensors) {
  std::vector<Tensor> result(tensors.size());
  auto transform_lambda = [](const Tensor& input) -> Tensor {
    return at::native::atleast_2d(input);
  };
  std::transform(tensors.cbegin(), tensors.cend(), result.begin(), transform_lambda);
  return result;
}

Tensor atleast_3d(const Tensor& self) {
  switch (self.dim()) {
    case 0:
      return self.reshape({1, 1, 1});
    case 1: {
      return self.unsqueeze(0).unsqueeze(-1);
    }
    case 2: {
      return self.unsqueeze(-1);
    }
    default:
      return self;
  }
}

std::vector<Tensor> atleast_3d(TensorList tensors) {
  std::vector<Tensor> result(tensors.size());
  auto transform_lambda = [](const Tensor& input) -> Tensor {
    return at::native::atleast_3d(input);
  };
  std::transform(tensors.cbegin(), tensors.cend(), result.begin(), transform_lambda);
  return result;
}

}} // namespace at::native
