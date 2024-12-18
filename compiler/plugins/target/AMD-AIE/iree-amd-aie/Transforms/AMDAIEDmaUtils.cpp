// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "AMDAIEDmaUtils.h"

#include <cstdlib>

#include "iree-amd-aie/Transforms/AMDAIEUtils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"

namespace mlir::iree_compiler::AMDAIE {

/// Return an ancestor of 'op' in 'block', or nullptr if no such ancestor.
Operation *getAncestorInBlock(Operation *op, Block *block) {
  if (!op || !block) return nullptr;
  auto parent = op;
  while (parent && (parent->getBlock() != block))
    parent = parent->getParentOp();
  return parent;
}

bool areAccessPatternsCombinable(const SmallVector<OpFoldResult> &offsetsA,
                                 const SmallVector<OpFoldResult> &sizesA,
                                 const SmallVector<OpFoldResult> &stridesA,
                                 const SmallVector<OpFoldResult> &offsetsB,
                                 const SmallVector<OpFoldResult> &sizesB,
                                 const SmallVector<OpFoldResult> &stridesB,
                                 size_t maxNbDims) {
  assert(offsetsA.size() == sizesA.size() &&
         "expected same number of source offsets and sizes");
  assert(offsetsA.size() == stridesA.size() &&
         "expected same number of source offsets and strides");
  assert(offsetsB.size() == sizesB.size() &&
         "expected same number of source offsets and sizes");
  assert(offsetsB.size() == stridesB.size() &&
         "expected same number of source offsets and strides");
  if (std::abs((ssize_t)offsetsA.size() - (ssize_t)offsetsB.size()) > 1)
    return false;
  // Empty access patterns are always combinable as they effectively mean:
  // 'don't perform any or change the addressing'.
  if (offsetsA.empty() && offsetsB.empty()) return true;
  // In case both access patterns have the same number of dimension, a new
  // dimension will need to be added, so fail if there aren't enough
  // dimensions.
  if (offsetsA.size() == offsetsB.size() && offsetsA.size() + 1 > maxNbDims)
    return false;

  for (auto &&[strideA, strideB] :
       llvm::zip(llvm::reverse(stridesA), llvm::reverse(stridesB))) {
    std::optional<int64_t> maybeStrideA = getConstantIntValue(strideA);
    std::optional<int64_t> maybeStrideB = getConstantIntValue(strideB);
    // Handle static and constant value with same int value.
    if (maybeStrideA && maybeStrideB &&
        maybeStrideA.value() == maybeStrideB.value()) {
      continue;
    }
    if (strideA != strideB) return false;
  }

  // Don't check the outermost dimension of size at this point.
  SmallVector<OpFoldResult> innerSizesA;
  SmallVector<OpFoldResult> innerSizesB;
  std::copy(sizesA.begin() + 1, sizesA.end(), std::back_inserter(innerSizesA));
  std::copy(sizesB.begin() + 1, sizesB.end(), std::back_inserter(innerSizesB));
  for (auto &&[sizeA, sizeB] :
       llvm::zip(llvm::reverse(innerSizesA), llvm::reverse(innerSizesB))) {
    std::optional<int64_t> maybeSizeA = getConstantIntValue(sizeA);
    std::optional<int64_t> maybeSizeB = getConstantIntValue(sizeB);
    // Handle static and constant value with same int value.
    if (maybeSizeA && maybeSizeB && maybeSizeA.value() == maybeSizeB.value()) {
      continue;
    }
    if (sizeA != sizeB) return false;
  }

  // Edge case for sizesA[0] != sizesB[0].
  if (offsetsB.size() == offsetsA.size() && sizesA[0] != sizesB[0]) {
    std::optional<int64_t> constOffsetA = getConstantIntValue(offsetsA[0]);
    std::optional<int64_t> constSizeA = getConstantIntValue(sizesA[0]);
    std::optional<int64_t> constOffsetB = getConstantIntValue(offsetsB[0]);
    std::optional<int64_t> constSizeB = getConstantIntValue(sizesB[0]);
    if (constOffsetA && constOffsetB && constSizeA && constSizeB) {
      int64_t offsetDiff = constOffsetB.value() - constOffsetA.value();
      if (constSizeA.value() != offsetDiff) return false;
    } else {
      return false;
    }
  }

  bool foundDiff{false};
  for (auto iter : llvm::enumerate(
           llvm::zip(llvm::reverse(offsetsA), llvm::reverse(offsetsB)))) {
    const OpFoldResult &offsetA = std::get<0>(iter.value());
    const OpFoldResult &offsetB = std::get<1>(iter.value());
    if (offsetA == offsetB) continue;
    std::optional<int64_t> maybeOffsetA = getConstantIntValue(offsetA);
    std::optional<int64_t> maybeOffsetB = getConstantIntValue(offsetB);
    if (maybeOffsetA && maybeOffsetB &&
        maybeOffsetA.value() == maybeOffsetB.value()) {
      continue;
    }
    // Retrieve the corresponding stride for this dimension.
    std::optional<int64_t> maybeStride =
        getConstantIntValue(stridesA[stridesA.size() - 1 - iter.index()]);
    if (maybeOffsetA && maybeOffsetB && maybeStride) {
      int64_t diff =
          (maybeOffsetB.value() - maybeOffsetA.value()) * maybeStride.value();
      // Handle the three different size cases. Return early in case of an
      // incompatibility.
      if (offsetsA.size() > offsetsB.size()) {
        std::optional<int64_t> constOffset = getConstantIntValue(offsetsA[0]);
        std::optional<int64_t> constStride = getConstantIntValue(stridesA[0]);
        std::optional<int64_t> constSize = getConstantIntValue(sizesA[0]);
        if (constOffset && constStride && constSize &&
            constOffset.value() == 0 &&
            (constStride.value() * constSize.value()) == diff) {
          if (foundDiff) return false;
          foundDiff = true;
        } else {
          return false;
        }
      } else if (offsetsB.size() > offsetsA.size()) {
        std::optional<int64_t> constOffset = getConstantIntValue(offsetsB[0]);
        std::optional<int64_t> constStride = getConstantIntValue(stridesB[0]);
        if (constOffset && constStride && constOffset.value() == 0 &&
            constStride.value() == diff) {
          if (foundDiff) return false;
          foundDiff = true;
        } else {
          return false;
        }
      } else {
        if (foundDiff) return false;
        foundDiff = true;
      }
    } else {
      return false;
    }
  }
  return foundDiff;
}

LogicalResult combineAccessPatterns(RewriterBase &rewriter,
                                    const SmallVector<OpFoldResult> &offsetsA,
                                    const SmallVector<OpFoldResult> &sizesA,
                                    const SmallVector<OpFoldResult> &stridesA,
                                    const SmallVector<OpFoldResult> &offsetsB,
                                    const SmallVector<OpFoldResult> &sizesB,
                                    const SmallVector<OpFoldResult> &stridesB,
                                    SmallVector<OpFoldResult> &newOffsets,
                                    SmallVector<OpFoldResult> &newSizes,
                                    SmallVector<OpFoldResult> &newStrides,
                                    size_t maxNbDims) {
  assert(offsetsA.size() == sizesA.size() &&
         "expected same number of source offsets and sizes");
  assert(offsetsA.size() == stridesA.size() &&
         "expected same number of source offsets and strides");
  assert(offsetsB.size() == sizesB.size() &&
         "expected same number of source offsets and sizes");
  assert(offsetsB.size() == stridesB.size() &&
         "expected same number of source offsets and strides");
  if (!areAccessPatternsCombinable(offsetsA, sizesA, stridesA, offsetsB, sizesB,
                                   stridesB, maxNbDims)) {
    return failure();
  }
  if (offsetsA.empty() && offsetsB.empty()) return success();
  if (offsetsB.size() > offsetsA.size()) {
    newOffsets = offsetsB;
    newSizes = sizesB;
    newStrides = stridesB;
    for (int i = 1; i <= offsetsA.size(); i++) {
      if (offsetsA[offsetsA.size() - i] != offsetsB[offsetsB.size() - i]) {
        newOffsets[newOffsets.size() - i] = offsetsA[offsetsA.size() - i];
        break;
      }
    }
    std::optional<int64_t> size = getConstantIntValue(newSizes[0]);
    if (!size) return failure();
    newSizes[0] = rewriter.getI64IntegerAttr(size.value() + 1);
  } else if (offsetsA.size() > offsetsB.size()) {
    newOffsets = offsetsA;
    newSizes = sizesA;
    newStrides = stridesA;
    std::optional<int64_t> size = getConstantIntValue(newSizes[0]);
    if (!size) return failure();
    newSizes[0] = rewriter.getI64IntegerAttr(size.value() + 1);
  } else {
    // Edge case for sizesA[0] != sizesB[0].
    if (sizesA[0] != sizesB[0]) {
      newOffsets = offsetsA;
      newSizes = sizesA;
      newStrides = stridesA;
      std::optional<int64_t> sizeA = getConstantIntValue(sizesA[0]);
      std::optional<int64_t> sizeB = getConstantIntValue(sizesB[0]);
      if (!sizeA || !sizeB) return failure();
      newSizes[0] = rewriter.getI64IntegerAttr(sizeA.value() + sizeB.value());
    } else {
      // All dims of sizes are the same, so add a new dimension with
      // 'offset == 0', 'size == 2' and 'stride == offsetDiff'.
      newOffsets.push_back(rewriter.getI64IntegerAttr(0));
      int64_t offsetDiff;
      int64_t strideMultiplier;
      for (auto iter : llvm::enumerate(llvm::zip(offsetsA, offsetsB))) {
        const OpFoldResult &offsetA = std::get<0>(iter.value());
        const OpFoldResult &offsetB = std::get<1>(iter.value());
        newOffsets.push_back(offsetA);
        if (offsetA != offsetB) {
          std::optional<int64_t> constOffsetA = getConstantIntValue(offsetA);
          std::optional<int64_t> constOffsetB = getConstantIntValue(offsetB);
          if (!constOffsetA || !constOffsetB) {
            return emitError(rewriter.getUnknownLoc())
                   << "differing offsets should be constants";
          }
          offsetDiff = constOffsetB.value() - constOffsetA.value();
          std::optional<int64_t> maybeStride =
              getConstantIntValue(stridesA[iter.index()]);
          if (!maybeStride) {
            return emitError(rewriter.getUnknownLoc())
                   << "no constant stride found at the same index where the "
                      "offset "
                      "difference occurs";
          }
          strideMultiplier = maybeStride.value();
        }
      }
      newSizes.push_back(rewriter.getI64IntegerAttr(2));
      newSizes.append(sizesA.begin(), sizesA.end());
      newStrides.push_back(
          rewriter.getI64IntegerAttr(offsetDiff * strideMultiplier));
      newStrides.append(stridesA.begin(), stridesA.end());
    }
  }
  assert(newOffsets.size() == newSizes.size() &&
         "expected same number of new offsets and sizes");
  assert(newOffsets.size() == newStrides.size() &&
         "expected same number of new offsets and strides");
  return success();
}

/// Fold subsequent dimensions within a strided access pattern that describe a
/// single linear access.
///
/// Example:
///
/// `offsets: [0, 0], sizes: [2, 8], strides: [8, 1]`
///
/// This describes accessing two times eight elements with a stride of eight in
/// between. For example if the input is: [1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
/// 2, 2, 2, 2] this will access [1, 1, 1, 1, 1, 1, 1, 1] and then [2, 2, 2, 2,
/// 2, 2, 2, 2].
///
/// This is the same access pattern as accessing
/// [1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2] in one go, which is
/// represented by:
///
/// `offsets: [0], sizes: [16], strides: [1]`
LogicalResult foldLinearDims(MLIRContext *ctx,
                             const SmallVector<OpFoldResult> &offsets,
                             const SmallVector<OpFoldResult> &sizes,
                             const SmallVector<OpFoldResult> &strides,
                             SmallVector<OpFoldResult> &newOffsets,
                             SmallVector<OpFoldResult> &newSizes,
                             SmallVector<OpFoldResult> &newStrides,
                             ArrayRef<int64_t> maxSizes) {
  assert(offsets.size() == sizes.size() && offsets.size() == strides.size() &&
         "expected same number of offsets, sizes and strides");
  bool foldableLinearDimsFound = false;
  if (offsets.size() == 0) return success(foldableLinearDimsFound);

  // As the new offsets/sizes/strides are created in reverse order, reverse the
  // maxSizes as well for easy comparison.
  SmallVector<int64_t> maxSizesReversed(maxSizes);
  std::reverse(maxSizesReversed.begin(), maxSizesReversed.end());

  std::optional<SmallVector<int64_t>> staticSizes = getConstantIntValues(sizes);
  std::optional<SmallVector<int64_t>> staticStrides =
      getConstantIntValues(strides);
  if (!staticSizes || !staticStrides) return failure();
  SmallVector<int64_t> staticSizeVals = staticSizes.value();
  SmallVector<int64_t> staticStrideVals = staticStrides.value();

  newOffsets.push_back(offsets[offsets.size() - 1]);
  newStrides.push_back(strides[strides.size() - 1]);
  newSizes.push_back(sizes[sizes.size() - 1]);

  for (int i = offsets.size() - 2; i >= 0; i--) {
    // Conditions for folding a dim.
    // 1. Offsets[i] == 0.This is required because we are dropping the offset
    // of the i dimension and keep newOffets[-1]
    // 2. newSizes[-1] x newStrides[-1] == strides[i]. With this we can have
    // newSizes[-1] = sizes[i] * newSizes[-1] , and then fold away the i
    // dimension
    // 3. sizes[i] * newSizes[-1] <= maxSizes[newSizes.size() - 1], IF max
    // constraints are provided. This allows hardware constraints to be
    // provided.
    size_t vecSize = newOffsets.size();
    int64_t newStride = staticStrideVals[i];
    int64_t newSize = staticSizeVals[i];
    int64_t prevStride = getConstantIndexOrAssert(newStrides[vecSize - 1]);
    int64_t prevSize = getConstantIndexOrAssert(newSizes[vecSize - 1]);
    int64_t dimExtent = prevStride * prevSize;
    // Fail if max constraints are provided, but the newly created
    // offsets/sizes/strides start exceeding the number of provide max
    // constraints as this will result in undefined behaviour.
    if (maxSizesReversed.size() > 0 && vecSize > maxSizesReversed.size())
      return failure();
    bool fitsMaxConstraint =
        maxSizesReversed.size() == 0 ||
        newSize * prevSize <= maxSizesReversed[vecSize - 1];
    if (fitsMaxConstraint && isConstantIntValue(offsets[i], 0) &&
        dimExtent == newStride) {
      foldableLinearDimsFound = true;
      newSizes[vecSize - 1] = getAsIndexOpFoldResult(ctx, newSize * prevSize);
      continue;
    }
    newOffsets.push_back(offsets[i]);
    newStrides.push_back(strides[i]);
    newSizes.push_back(sizes[i]);
  }

  // Reverse as the new offsets/sizes/strides were created in reverse order.
  std::reverse(newOffsets.begin(), newOffsets.end());
  std::reverse(newSizes.begin(), newSizes.end());
  std::reverse(newStrides.begin(), newStrides.end());
  return success(foldableLinearDimsFound);
}

/// Fold single dimension linear accesses and make them implicit. This requires
/// offset 0 and stride 1.
///
/// Example:
///   offsets: [%c0], sizes: [%c1024], strides: [%c1]
/// becomes:
///   offsets: [], sizes: [], strides: []
LogicalResult foldSingleDim(SmallVector<OpFoldResult> &offsets,
                            SmallVector<OpFoldResult> &sizes,
                            SmallVector<OpFoldResult> &strides) {
  assert(offsets.size() == sizes.size() && offsets.size() == strides.size() &&
         "expected same number of source offsets and sizes");

  if (offsets.size() != 1) return failure();
  if (!isConstantIntValue(offsets[0], 0)) return failure();
  if (!isConstantIntValue(strides[0], 1)) return failure();

  offsets.clear();
  sizes.clear();
  strides.clear();
  return success();
}

/// Fold unit dimensions within a strided access pattern.
LogicalResult foldUnitDims(const SmallVector<OpFoldResult> &offsets,
                           const SmallVector<OpFoldResult> &sizes,
                           const SmallVector<OpFoldResult> &strides,
                           SmallVector<OpFoldResult> &newOffsets,
                           SmallVector<OpFoldResult> &newSizes,
                           SmallVector<OpFoldResult> &newStrides) {
  bool foldableUnitDimsFound = false;

  for (int i = 0; i < offsets.size(); i++) {
    // Dim can be folded if offset is 0 and size is 1
    if (isConstantIntValue(offsets[i], 0) && isConstantIntValue(sizes[i], 1)) {
      foldableUnitDimsFound = true;
      continue;
    }
    newOffsets.push_back(offsets[i]);
    newStrides.push_back(strides[i]);
    newSizes.push_back(sizes[i]);
  }
  return success(foldableUnitDimsFound);
}

LogicalResult moveNpuDmaSyncUsersAfterAncestorInSameBlock(
    RewriterBase &rewriter, Operation *parentOp) {
  WalkResult res = parentOp->walk([&](AMDAIE::NpuDmaWaitOp npuDmaWaitOp) {
    SmallPtrSet<Operation *, 4> ancestorsInSameBlock;
    // All async token producers should result in the same ancestor being found.
    for (Value asyncToken : npuDmaWaitOp.getAsyncTokens()) {
      Operation *dmaOp = asyncToken.getDefiningOp();
      ancestorsInSameBlock.insert(
          getAncestorInBlock(npuDmaWaitOp, dmaOp->getBlock()));
    }
    if (ancestorsInSameBlock.size() == 0) {
      npuDmaWaitOp.emitOpError() << "no ancestors found";
      return WalkResult::interrupt();
    }
    if (ancestorsInSameBlock.size() != 1) {
      npuDmaWaitOp.emitOpError()
          << "the async token producers are located in a different scope";
      return WalkResult::interrupt();
    }
    Operation *ancestorInSameBlock = *ancestorsInSameBlock.begin();
    if (!ancestorInSameBlock) {
      npuDmaWaitOp->emitOpError(
          "doesn't have an ancestor in the same scope as the source DMA op");
      return WalkResult::interrupt();
    }
    rewriter.moveOpAfter(npuDmaWaitOp, ancestorInSameBlock);
    return WalkResult::advance();
  });
  if (res.wasInterrupted()) return failure();
  return success();
}

}  // namespace mlir::iree_compiler::AMDAIE
