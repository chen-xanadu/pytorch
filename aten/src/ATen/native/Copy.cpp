#include <ATen/native/Copy.h>

#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/quantized/Copy.h>

namespace {

using namespace at;

bool copy_transpose_valid(const Tensor& self, const Tensor& src) {
  const int MIN_SZ = 60 * 60;
  return self.is_contiguous() && src.numel() != 0 && src.dim() == 2 &&
      src.stride(0) == 1 && src.stride(1) == src.size(0) &&
      self.numel() >= MIN_SZ;
}

// special case copy where tensor is contiguous and src is a transposed matrix
// This can be generalized to most copies, but it's trickier
void copy_same_type_transpose_(Tensor& self, const Tensor& src) {
  int64_t BLOCK_SZ;
  if (self.scalar_type() == kByte) {
    BLOCK_SZ = 120;
  } else {
    BLOCK_SZ = 60;
  }
  Tensor buf = empty({BLOCK_SZ, BLOCK_SZ}, self.options());

  AT_DISPATCH_ALL_TYPES_AND2(kHalf, kBool, self.scalar_type(), "copy_", [&] {
    scalar_t* sp = src.data<scalar_t>();
    scalar_t* rp = self.data<scalar_t>();
    scalar_t* bp = buf.data<scalar_t>();

    int64_t NR = src.size(0);
    int64_t NC = src.size(1);
    for (int64_t R = 0; R < NR; R += BLOCK_SZ) {
      for (int64_t C = 0; C < NC; C += BLOCK_SZ) {
        scalar_t* spo = sp + R + C * NR;
        scalar_t* rpo = rp + C + R * NC;

        int nr = std::min(NR - R, BLOCK_SZ);
        int nc = std::min(NC - C, BLOCK_SZ);

        // 1. copy columns from src to buf
        for (int c = 0; c < nc; c++) {
          memcpy(bp + c * BLOCK_SZ, spo + c * NR, nr * sizeof(scalar_t));
        }

        // 2. transpose buf in place
        int rc_max = std::max(nr, nc);
        int rc_min = std::min(nr, nc);
        for (int r = 0; r < rc_max; r++) {
          int end = std::min(r, rc_min);
          for (int c = 0; c < end; c++) {
            scalar_t tmp = bp[r + BLOCK_SZ * c];
            bp[r + BLOCK_SZ * c] = bp[r * BLOCK_SZ + c];
            bp[r * BLOCK_SZ + c] = tmp;
          }
        }

        // 3. copy rows from buf to dst
        for (int r = 0; r < nr; r++) {
          memcpy(rpo + r * NC, bp + r * BLOCK_SZ, nc * sizeof(scalar_t));
        }
      }
    }
  });
}

} // namespace

namespace at {
namespace native {

Tensor & copy_(Tensor & self, const Tensor & src, bool non_blocking) {
  // TODO: this should be handled during dispatch, but that's missing...
  TORCH_CHECK(self.defined(), "self is undefined");
  TORCH_CHECK(self.defined(), "src is undefined");

  Tensor b_src;
  if (self.is_sparse() && src.is_sparse()) {
    return at::copy_sparse_to_sparse_(self, src, non_blocking);
  } else if (self.is_sparse() || src.is_sparse()) {
    AT_ERROR("copy_() between dense and sparse Tensors is not implemented! Found self type = ",
             self.type(), " and src type = ", src.type());
  }

  if (self.is_same(src)) {
    return self;
  }

  if (self.scalar_type() == kQUInt8) {
    return quantized_copy_(self, src);
  }

  auto builder = TensorIterator::Builder();
  builder.add_output(self);
  builder.add_input(src);
  builder.dont_resize_outputs();
  builder.dont_compute_common_dtype();
  auto iter = builder.build();

  if (iter->numel() == 0) {
    return self;
  }

  DeviceType device_type = iter->device_type(0);
  if (iter->device_type(1) == kCUDA) {
    device_type = kCUDA;
  }

  if (device_type == kCPU && copy_transpose_valid(self, src)) {
    copy_same_type_transpose_(self, src);
    return self;
  }

  copy_stub(device_type, *iter, non_blocking);
  return self;
}

DEFINE_DISPATCH(copy_stub);

} // namespace native
} // namespace at
