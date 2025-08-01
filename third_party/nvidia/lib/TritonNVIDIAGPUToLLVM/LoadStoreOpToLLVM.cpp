#include "Dialect/NVGPU/IR/Dialect.h"
#include "TargetInfo.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/TypeUtilities.h"

#include "TritonNVIDIAGPUToLLVM/PTXAsmFormat.h"

#include "PatternTritonGPUOpToLLVM.h"
#include "Utility.h"
#include "triton/Analysis/AxisInfo.h"
#include "triton/Conversion/TritonGPUToLLVM/Utility.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Attributes.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/LinearLayoutConversions.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/TMAUtilities.h"
#include "triton/Tools/LayoutUtils.h"

#include <cassert>

using namespace mlir;
using namespace mlir::triton;
namespace tt = mlir::triton;
namespace ttg = mlir::triton::gpu;
namespace ttng = mlir::triton::nvidia_gpu;

using ::mlir::LLVM::delinearize;
using ::mlir::LLVM::getSharedMemoryObjectFromStruct;
using ::mlir::LLVM::linearize;
using ::mlir::triton::gpu::getCTALayout;
using ::mlir::triton::gpu::getTotalElemsPerThread;
using ::mlir::triton::gpu::NVMMASharedEncodingAttr;

// Toggle this to work around Cooperative Grid Launch ld.acquire optimized path
static constexpr bool disableLDAcquireLowering = false;

namespace {

Value maybeAnd(RewriterBase &rewriter, Location loc, Value a, Value b) {
  auto tb = TritonLLVMOpBuilder(loc, rewriter);
  if (a && b) {
    return tb.and_(a, b);
  }
  return a ? a : b;
}

// Return a predicate that is true only if the current thread holds unique data,
// according to freeVarsMask. The predicate may be null to indicate no
// predication is required.
Value emitRedundantThreadPredicate(
    const llvm::MapVector<StringAttr, int32_t> &freeVarMasks,
    ConversionPatternRewriter &rewriter, Location loc,
    const NVIDIA::TargetInfo &targetInfo) {
  auto b = TritonLLVMOpBuilder(loc, rewriter);
  auto ctx = rewriter.getContext();
  auto kLane = str_attr("lane");
  auto kWarp = str_attr("warp");
  auto kBlock = str_attr("block");

  Value zero = b.i32_val(0);
  auto [laneId, warpId] = getLaneAndWarpId(rewriter, loc);
  Value blockId = freeVarMasks.lookup(kBlock) == 0
                      ? zero
                      : targetInfo.getClusterCTAId(rewriter, loc);

  Value pred;
  auto dimNames = {kLane, kWarp, kBlock};
  auto dimIds = {laneId, warpId, blockId};
  for (auto [dimName, dimId] : llvm::zip(dimNames, dimIds)) {
    int32_t mask = freeVarMasks.lookup(dimName);
    if (mask != 0) {
      auto dimPred = b.icmp_eq(b.and_(dimId, b.i32_val(mask)), zero);
      pred = maybeAnd(rewriter, loc, pred, dimPred);
    }
  }
  return pred;
}

unsigned getCanonicalIndex(unsigned index, unsigned freeVarMask) {
  return index & ~freeVarMask;
}

std::string getRegisterSizeCode(int size, bool is_float) {
  switch (size) {
  case 1:
    return "b";
  case 16:
    return "h";
  case 32:
    return is_float ? "f" : "r";
  case 64:
    return is_float ? "d" : "l";
  case 128:
    return "q";
  default:
    llvm_unreachable("Unsupported register size");
  }
}

Value createCachePolicy(triton::EvictionPolicy opEvict,
                        ConversionPatternRewriter &rewriter, Location loc,
                        int computeCapability) {
  // Emit createpolicy.fractional.L2::policy.b64 xx 1.0
  PTXBuilder ptxBuilder;
  const bool hasL2EvictPolicy =
      opEvict == triton::EvictionPolicy::EVICT_FIRST ||
      opEvict == triton::EvictionPolicy::EVICT_LAST;
  Value policyRet;

  const bool hardwareSupport = computeCapability >= 80;

  if (hasL2EvictPolicy && hardwareSupport) {
    auto &policy =
        ptxBuilder.create<>("createpolicy.fractional")
            ->o("L2::evict_first",
                opEvict == triton::EvictionPolicy::EVICT_FIRST)
            .o("L2::evict_last", opEvict == triton::EvictionPolicy::EVICT_LAST)
            .b(64);

    const std::string writeConstraint = "=l";
    // prepare asm operands
    auto *dstOpr = ptxBuilder.newOperand(writeConstraint, /*init=*/true);
    std::string fractionStr = "1.0";
    auto *fractionOpr = ptxBuilder.newConstantOperand(fractionStr);
    policy(dstOpr, fractionOpr);

    Type policyRetTy = rewriter.getI64Type();
    policyRet = ptxBuilder.launch(rewriter, loc, policyRetTy);
  }

  return policyRet;
}

// Contains some helper functions for both Load and Store conversions.
struct LoadStoreConversionBase {
  explicit LoadStoreConversionBase(const NVIDIA::TargetInfo &targetInfo,
                                   ModuleAxisInfoAnalysis &axisAnalysisPass)
      : targetInfo(targetInfo), axisAnalysisPass(axisAnalysisPass) {}

  unsigned getContiguity(Value ptr) const {
    auto tensorTy = dyn_cast<RankedTensorType>(ptr.getType());
    if (!tensorTy)
      return 1;
    return axisAnalysisPass.getContiguity(ptr);
  }

  unsigned getVectorSize(Value ptr) const {
    auto tensorTy = dyn_cast<RankedTensorType>(ptr.getType());
    if (!tensorTy)
      return 1;
    auto contiguity = getContiguity(ptr);
    auto pointeeBitWidth = triton::getPointeeBitWidth(tensorTy);
    LDBG("getVectorSize contiguity = " << contiguity << " pointeeBitWidth = "
                                       << pointeeBitWidth);
    // The maximum vector size is 128 bits on NVIDIA GPUs.
    return std::min<unsigned>(128 / pointeeBitWidth, contiguity);
  }

  unsigned getMaskAlignment(Value mask) const {
    return axisAnalysisPass.getMaskAlignment(mask);
  }

protected:
  const NVIDIA::TargetInfo &targetInfo;
  ModuleAxisInfoAnalysis &axisAnalysisPass;
};

struct LoadOpConversion : public ConvertOpToLLVMPattern<triton::LoadOp>,
                          public LoadStoreConversionBase {
  LoadOpConversion(LLVMTypeConverter &converter,
                   const NVIDIA::TargetInfo &targetInfo, int computeCapability,
                   ModuleAxisInfoAnalysis &axisAnalysisPass,
                   PatternBenefit benefit)
      : ConvertOpToLLVMPattern<triton::LoadOp>(converter, benefit),
        computeCapability(computeCapability),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::LoadOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = getContext();
    auto loc = op->getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    auto typeConverter = getTypeConverter();

    // original values
    Value ptr = op.getPtr();
    Value mask = op.getMask();
    Value other = op.getOther();
    LDBG("Lower LoadOp for " << ptr);

    // adaptor values
    assert(!isTensorPointerType(ptr.getType()) &&
           "Cannot convert load with a tensor pointer into LLVM; "
           "this case should be transformed to normal load before lowering");
    Value llPtr = adaptor.getPtr();
    Value llMask = adaptor.getMask();
    Value llOther = adaptor.getOther();

    // Determine the vectorization size
    Type valueElemTy =
        typeConverter->convertType(getElementTypeOrSelf(op.getType()));
    unsigned vec = getVectorSize(ptr);
    unsigned numElems = getTotalElemsPerThread(ptr.getType());
    unsigned vecOrig = vec;
    if (llMask) {
      LLVM_DEBUG(DBGS() << "vec = " << vec
                        << " mask_alignment = " << getMaskAlignment(mask));
      vec = std::min<size_t>(vec, getMaskAlignment(mask));
      LLVM_DEBUG(llvm::dbgs() << " vec = " << vec << '\n');
    }

    if (vec == 1 && numElems > 1) {
      int maskValue = !llMask ? -1 : getMaskAlignment(mask);
      op->emitRemark() << "Warning: vectorization fails vec = " << vec
                       << " origin vec = " << vecOrig
                       << " numElems = " << numElems << " mask is " << maskValue
                       << "\n";
    }
    // Get the LLVM values for pointers
    auto ptrElems = unpackLLElements(loc, llPtr, rewriter);
    assert(ptrElems.size() == numElems);

    // Get the LLVM values for mask
    SmallVector<Value> maskElems;
    if (llMask) {
      maskElems = unpackLLElements(loc, llMask, rewriter);
      assert(maskElems.size() == numElems);
    }

    // Get the LLVM values for `other`
    // TODO: (goostavz) handle when other is const but not splat, which
    //       should be rarely seen
    bool otherIsSplatConstInt = false;
    DenseElementsAttr constAttr;
    int64_t splatVal = 0;
    if (other && isa<IntegerType>(valueElemTy) &&
        matchPattern(other, m_Constant(&constAttr)) && constAttr.isSplat() &&
        isa<IntegerType>(constAttr.getElementType())) {
      otherIsSplatConstInt = true;
      splatVal = constAttr.getSplatValue<APInt>().getSExtValue();
    }
    SmallVector<Value> otherElems;
    if (other) {
      otherElems = unpackLLElements(loc, llOther, rewriter);
    }

    // vectorized iteration through all the pointer/mask/other elements
    const int valueElemNBits =
        std::max(8u, valueElemTy.getIntOrFloatBitWidth());
    const int numVecs = numElems / vec;

    // Load redundantly in all dims except reg
    auto freeVarMasks = getFreeVariableMasks(ptr.getType());
    uint32_t regMask = freeVarMasks[str_attr("reg")];

    LDBG("LoadOp numElems = " << numElems << " vec = " << vec
                              << " valueElemNBits = " << valueElemNBits << " "
                              << op.getType());
    SmallVector<Value> loadedVals;
    for (size_t vecStart = 0; vecStart < numElems; vecStart += vec) {
      if (auto canonicalVecStart = getCanonicalIndex(vecStart, regMask);
          vecStart != canonicalVecStart) {
        // For redundant registers, refer back to the canonical load
        for (auto iVec = 0; iVec < vec; ++iVec) {
          loadedVals.push_back(loadedVals[canonicalVecStart + iVec]);
        }
        continue;
      }

      // TODO: optimization when ptr is GEP with constant offset
      size_t in_off = 0;

      const size_t maxWordWidth = std::max<size_t>(32, valueElemNBits);
      const size_t totalWidth = valueElemNBits * vec;
      const size_t width = std::min(totalWidth, maxWordWidth);
      const size_t nWords = std::max<size_t>(1, totalWidth / width);
      const size_t wordNElems = width / valueElemNBits;
      const size_t movWidth = width < 16 ? 16 : width;
      assert(wordNElems * nWords * numVecs == numElems);

      PTXBuilder ptxBuilder;

      Value pred = mask ? maskElems[vecStart] : Value{};

      const std::string readConstraint =
          (width == 64) ? "l" : ((width == 32) ? "r" : "c");
      const std::string writeConstraint =
          (width == 64) ? "=l" : ((width == 32) ? "=r" : "=c");

      // prepare asm operands
      auto *dstsOpr = ptxBuilder.newListOperand();
      // If there is a `other` value, use it to init.
      bool init = other == nullptr;
      for (size_t wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        auto *opr = ptxBuilder.newOperand(writeConstraint,
                                          init); // =r operations
        dstsOpr->listAppend(opr);
      }

      if (other) {
        for (size_t ii = 0; ii < nWords; ++ii) {
          // PTX doesn't support mov.u8, so we need to use mov.u16
          PTXInstr &mov =
              ptxBuilder.create<>("mov")->o("u" + std::to_string(movWidth));

          size_t size = width / valueElemNBits;

          auto vecTy = LLVM::getVectorType(valueElemTy, size);
          Value v = b.undef(vecTy);
          for (size_t s = 0; s < size; ++s) {
            Value falseVal = otherElems[vecStart + ii * size + s];
            Value sVal = createIndexAttrConstant(
                rewriter, loc, typeConverter->getIndexType(), s);
            v = b.insert_element(vecTy, v, falseVal, sVal);
          }
          v = b.bitcast(v, IntegerType::get(getContext(), width));

          PTXInstr::Operand *opr{};

          if (otherIsSplatConstInt) {
            int64_t replicatedSplatVal = 0;
            for (size_t s = 0; s < movWidth; s += valueElemNBits) {
              replicatedSplatVal |= splatVal << s;
            }
            opr = ptxBuilder.newConstantOperand(replicatedSplatVal);
          } else
            opr = ptxBuilder.newOperand(v, readConstraint);

          mov(dstsOpr->listGet(ii), opr);
        }
      }

      auto *addrOpr =
          ptxBuilder.newAddrOperand(ptrElems[vecStart], "l", in_off);

      // Create L2 cache policy register if needed
      Value l2PolicyReg =
          createCachePolicy(op.getEvict(), rewriter, loc, computeCapability);

      // Define the instruction opcode
      auto &ld = ptxBuilder.create<>("ld")
                     ->o("volatile", op.getIsVolatile())
                     .global()
                     .o("ca", op.getCache() == triton::CacheModifier::CA)
                     .o("cg", op.getCache() == triton::CacheModifier::CG)
                     .o("L1::evict_first",
                        op.getEvict() == triton::EvictionPolicy::EVICT_FIRST)
                     .o("L1::evict_last",
                        op.getEvict() == triton::EvictionPolicy::EVICT_LAST)
                     .o("L2::cache_hint", l2PolicyReg != Value())
                     .v(nWords)
                     .b(width);

      PTXBuilder::Operand *evictOpr = nullptr;
      if (l2PolicyReg)
        evictOpr = ptxBuilder.newOperand(l2PolicyReg, "l");

      if (!evictOpr)
        ld(dstsOpr, addrOpr).maybePredicate(pred, "b");
      else
        ld(dstsOpr, addrOpr, evictOpr).maybePredicate(pred, "b");

      // Create inline ASM signature
      SmallVector<Type> retTys(nWords, IntegerType::get(getContext(), width));
      Type retTy = retTys.size() > 1
                       ? LLVM::LLVMStructType::getLiteral(getContext(), retTys)
                       : retTys[0];

      Value ret = ptxBuilder.launch(rewriter, loc, retTy);

      // Extract and store return values
      SmallVector<Value> rets;
      for (unsigned int ii = 0; ii < nWords; ++ii) {
        Value curr;
        if (isa<LLVM::LLVMStructType>(retTy)) {
          curr = b.extract_val(IntegerType::get(getContext(), width), ret, ii);
        } else {
          curr = ret;
        }
        curr = b.bitcast(
            curr, LLVM::getVectorType(valueElemTy, width / valueElemNBits));
        rets.push_back(curr);
      }
      int tmp = width / valueElemNBits;
      for (size_t ii = 0; ii < vec; ++ii) {
        Value vecIdx = createIndexAttrConstant(
            rewriter, loc, typeConverter->getIndexType(), ii % tmp);
        Value loaded = b.extract_element(valueElemTy, rets[ii / tmp], vecIdx);
        loadedVals.push_back(loaded);
      }
    } // end vec

    Type llvmResultStructTy = typeConverter->convertType(op.getType());
    Value resultStruct = packLLElements(loc, typeConverter, loadedVals,
                                        rewriter, llvmResultStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }

  int computeCapability;
};

struct StoreOpConversion : public ConvertOpToLLVMPattern<triton::StoreOp>,
                           public LoadStoreConversionBase {
  StoreOpConversion(LLVMTypeConverter &converter,
                    const NVIDIA::TargetInfo &targetInfo, int computeCapability,
                    ModuleAxisInfoAnalysis &axisAnalysisPass,
                    PatternBenefit benefit)
      : ConvertOpToLLVMPattern<triton::StoreOp>(converter, benefit),
        computeCapability(computeCapability),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::StoreOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value ptr = op.getPtr();
    Value value = op.getValue();

    Value llPtr = adaptor.getPtr();
    Value llMask = adaptor.getMask();
    Value llValue = adaptor.getValue();

    auto loc = op->getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    MLIRContext *ctx = rewriter.getContext();

    auto valueTy = value.getType();
    Type valueElemTy =
        typeConverter->convertType(getElementTypeOrSelf(valueTy));

    unsigned vec = getVectorSize(ptr);
    unsigned elemsPerThread = getTotalElemsPerThread(ptr.getType());

    auto ptrElems = unpackLLElements(loc, llPtr, rewriter);
    auto valueElems = unpackLLElements(loc, llValue, rewriter);
    assert(ptrElems.size() == valueElems.size());

    // Determine the vectorization size
    unsigned vecOrig = vec;
    SmallVector<Value> maskElems;
    if (llMask) {
      Value mask = op.getMask();
      maskElems = unpackLLElements(loc, llMask, rewriter);
      assert(valueElems.size() == maskElems.size());

      unsigned maskAlign = getMaskAlignment(mask);
      vec = std::min(vec, maskAlign);
    }

    if (vec == 1 && elemsPerThread > 1) {
      int mask = !llMask ? -1 : getMaskAlignment(op.getMask());
      op->emitRemark() << "Warning: vectorization fails vec = " << vec
                       << " origin vec = " << vecOrig
                       << " elemsPerThread = " << elemsPerThread << " mask is "
                       << mask << "\n";
    }

    const size_t dtsize =
        std::max<int>(1, valueElemTy.getIntOrFloatBitWidth() / 8);
    const size_t valueElemNBits = dtsize * 8;

    auto freeVarMasks = getFreeVariableMasks(ptr.getType());
    Value threadPred =
        emitRedundantThreadPredicate(freeVarMasks, rewriter, loc, targetInfo);
    uint32_t regMask = freeVarMasks[str_attr("reg")];

    const int numVecs = elemsPerThread / vec;
    for (size_t vecStart = 0; vecStart < elemsPerThread; vecStart += vec) {
      if (!isCanonicalIndex(vecStart, regMask)) {
        // Don't emit store ops for redundant elements within a thread
        continue;
      }
      // TODO: optimization when ptr is AddPtr with constant offset
      size_t in_off = 0;

      const size_t maxWordWidth = std::max<size_t>(32, valueElemNBits);
      const size_t totalWidth = valueElemNBits * vec;
      const size_t width = std::min(totalWidth, maxWordWidth);
      const size_t nWords = std::max<size_t>(1, totalWidth / width);
      const size_t wordNElems = width / valueElemNBits;
      assert(wordNElems * nWords * numVecs == elemsPerThread);

      // TODO(Superjomn) Add cache policy fields to StoreOp.
      // TODO(Superjomn) Deal with cache policy here.

      Type valArgTy = IntegerType::get(ctx, width);
      auto wordTy = vec_ty(valueElemTy, wordNElems);

      SmallVector<std::pair<Value, std::string>> asmArgs;
      for (size_t wordIdx = 0; wordIdx < nWords; ++wordIdx) {
        // llWord is a width-len composition
        Value llWord = b.undef(wordTy);
        // Insert each value element to the composition
        for (size_t elemIdx = 0; elemIdx < wordNElems; ++elemIdx) {
          const size_t elemOffset = vecStart + wordIdx * wordNElems + elemIdx;
          assert(elemOffset < valueElems.size());
          Value elem = valueElems[elemOffset];
          if (elem.getType().isInteger(1))
            elem = b.sext(i8_ty, elem);
          elem = b.bitcast(elem, valueElemTy);

          llWord = b.insert_element(wordTy, llWord, elem, b.i32_val(elemIdx));
        }
        llWord = b.bitcast(llWord, valArgTy);
        std::string constraint =
            (width == 64) ? "l" : ((width == 32) ? "r" : "c");
        asmArgs.emplace_back(llWord, constraint);
      }

      // Prepare the PTX inline asm.
      PTXBuilder ptxBuilder;
      auto *asmArgList = ptxBuilder.newListOperand(asmArgs);

      Value pred = threadPred;
      if (llMask) {
        auto mask = maskElems[vecStart];
        pred = maybeAnd(rewriter, loc, pred, mask);
      }

      auto *asmAddr =
          ptxBuilder.newAddrOperand(ptrElems[vecStart], "l", in_off);

      // Create L2 cache policy register if needed
      Value l2PolicyReg =
          createCachePolicy(op.getEvict(), rewriter, loc, computeCapability);

      auto &ptxStoreInstr =
          ptxBuilder.create<>("st")
              ->global()
              .o("wb", op.getCache() == triton::CacheModifier::WB)
              .o("cg", op.getCache() == triton::CacheModifier::CG)
              .o("cs", op.getCache() == triton::CacheModifier::CS)
              .o("wt", op.getCache() == triton::CacheModifier::WT)
              .o("L1::evict_first",
                 op.getEvict() == triton::EvictionPolicy::EVICT_FIRST)
              .o("L1::evict_last",
                 op.getEvict() == triton::EvictionPolicy::EVICT_LAST)
              .o("L2::cache_hint", l2PolicyReg != Value())
              .v(nWords)
              .b(width);

      PTXBuilder::Operand *evictOpr = nullptr;
      if (l2PolicyReg)
        evictOpr = ptxBuilder.newOperand(l2PolicyReg, "l");

      if (!evictOpr)
        ptxStoreInstr(asmAddr, asmArgList).maybePredicate(pred, "b");
      else
        ptxStoreInstr(asmAddr, asmArgList, evictOpr).maybePredicate(pred, "b");

      auto asmReturnTy = void_ty(ctx);
      ptxBuilder.launch(rewriter, loc, asmReturnTy);
    }
    rewriter.eraseOp(op);
    return success();
  }

  int computeCapability;
};

void createBarrier(ConversionPatternRewriter &rewriter, Location loc,
                   int numCTAs) {
  auto b = TritonLLVMOpBuilder(loc, rewriter);
  if (numCTAs == 1) {
    b.barrier();
  } else {
    rewriter.create<triton::nvidia_gpu::ClusterArriveOp>(loc, false);
    rewriter.create<triton::nvidia_gpu::ClusterWaitOp>(loc);
  }
}

struct AtomicCASOpConversion
    : public ConvertOpToLLVMPattern<triton::AtomicCASOp>,
      public LoadStoreConversionBase {
  AtomicCASOpConversion(LLVMTypeConverter &converter,
                        const NVIDIA::TargetInfo &targetInfo,
                        ModuleAxisInfoAnalysis &axisAnalysisPass,
                        PatternBenefit benefit)
      : ConvertOpToLLVMPattern<triton::AtomicCASOp>(converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::AtomicCASOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    MLIRContext *ctx = rewriter.getContext();

    auto moduleOp = op->getParentOfType<ModuleOp>();
    assert(moduleOp && "Parent ModuleOp not found for AtomicCASOp");
    int numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(moduleOp);

    Value llPtr = adaptor.getPtr();
    Value llCmp = adaptor.getCmp();
    Value llVal = adaptor.getVal();

    auto ptrElements = unpackLLElements(loc, llPtr, rewriter);
    auto cmpElements = unpackLLElements(loc, llCmp, rewriter);
    auto valElements = unpackLLElements(loc, llVal, rewriter);

    auto valueTy = op.getType();
    auto tensorTy = dyn_cast<RankedTensorType>(valueTy);
    Type valueElemTy =
        tensorTy ? getTypeConverter()->convertType(tensorTy.getElementType())
                 : valueTy;
    auto valueElemNBits = valueElemTy.getIntOrFloatBitWidth();
    auto elemsPerThread = getTotalElemsPerThread(op.getVal().getType());
    // vec = 1 for scalar
    auto vec = getVectorSize(op.getPtr());
    auto vecOrig = vec;
    // tensor
    if (tensorTy) {
      auto valTy = cast<RankedTensorType>(op.getVal().getType());
      vec = std::min<unsigned>(vec, valTy.getElementType().isF16() ? 2 : 1);
    }

    if (vec == 1 && elemsPerThread > 1)
      op->emitRemark() << "Warning: vectorization fails vec = " << vec
                       << " origin vec = " << vecOrig
                       << " elemsPerThread = " << elemsPerThread << "\n";

    auto freeVarMasks = getFreeVariableMasks(op.getPtr().getType());
    Value threadPred =
        emitRedundantThreadPredicate(freeVarMasks, rewriter, loc, targetInfo);
    uint32_t regMask = freeVarMasks[str_attr("reg")];

    auto vecTy = vec_ty(valueElemTy, vec);
    SmallVector<Value> resultVals(elemsPerThread);

    for (size_t i = 0; i < elemsPerThread; i += vec) {
      if (auto canonicalVecStart = getCanonicalIndex(i, regMask);
          canonicalVecStart != i) {
        // For redundant registers, refer back to the canonical result
        for (auto iVec = 0; iVec < vec; ++iVec) {
          resultVals[i + iVec] = resultVals[canonicalVecStart + iVec];
        }
        continue;
      }

      Value casVal = b.undef(vecTy);
      for (int ii = 0; ii < vec; ++ii) {
        Value iiVal = createIndexAttrConstant(
            rewriter, loc, getTypeConverter()->getIndexType(), ii);
        casVal = b.insert_element(vecTy, casVal, valElements[i + ii], iiVal);
      }

      Value casPtr = ptrElements[i];
      Value casCmp = cmpElements[i];
      casVal = valElements[i];
      PTXBuilder ptxBuilderAtomicCAS;
      std::string tyId = valueElemNBits * vec == 64
                             ? "l"
                             : (valueElemNBits * vec == 32 ? "r" : "h");
      auto *dstOpr = ptxBuilderAtomicCAS.newOperand("=" + tyId, /*init=*/true);
      auto *ptrOpr = ptxBuilderAtomicCAS.newAddrOperand(casPtr, "l");
      auto *cmpOpr = ptxBuilderAtomicCAS.newOperand(casCmp, tyId);
      auto *valOpr = ptxBuilderAtomicCAS.newOperand(casVal, tyId);
      auto &atom = *ptxBuilderAtomicCAS.create<PTXInstr>("atom");
      auto sTy = "b" + std::to_string(valueElemNBits);
      std::string semStr;
      llvm::raw_string_ostream os(semStr);
      os << op.getSem();
      auto scope = stringifyMemSyncScope(op.getScope()).str();
      atom.global().o(semStr).o(scope).o("cas").o(sTy);
      atom(dstOpr, ptrOpr, cmpOpr, valOpr).maybePredicate(threadPred);

      if (tensorTy) {
        auto retType = vec == 1 ? valueElemTy : vecTy;
        auto ret = ptxBuilderAtomicCAS.launch(rewriter, loc, retType);
        for (int ii = 0; ii < vec; ++ii) {
          resultVals[i + ii] =
              vec == 1 ? ret
                       : b.extract_element(valueElemTy, ret, b.i32_val(ii));
        }
      } else {
        auto old = ptxBuilderAtomicCAS.launch(rewriter, loc, valueElemTy);
        if (!atomicNeedsSharedMemory(op.getResult())) {
          rewriter.eraseOp(op);
          return success();
        }
        Value atomPtr = LLVM::getSharedMemoryBase(loc, rewriter, targetInfo,
                                                  op.getOperation());
        atomPtr = b.bitcast(atomPtr, ptr_ty(ctx, 3));
        // Only threads with mask = True store the result
        PTXBuilder ptxBuilderStore;
        auto *dstOprStore = ptxBuilderStore.newAddrOperand(atomPtr, "r");
        auto *valOprStore = ptxBuilderStore.newOperand(old, "r");
        auto &st = *ptxBuilderStore.create<PTXInstr>("st");
        st.shared().o(sTy);
        st(dstOprStore, valOprStore).maybePredicate(threadPred);
        auto ASMReturnTy = void_ty(ctx);
        ptxBuilderStore.launch(rewriter, loc, ASMReturnTy);
        createBarrier(rewriter, loc, numCTAs);
        Value ret = b.load(valueElemTy, atomPtr);
        rewriter.replaceOp(op, {ret});
      }
    }

    if (tensorTy) {
      Type structTy = getTypeConverter()->convertType(tensorTy);
      Value resultStruct = packLLElements(loc, getTypeConverter(), resultVals,
                                          rewriter, structTy);
      rewriter.replaceOp(op, {resultStruct});
    }
    return success();
  }
};

struct AtomicRMWOpConversion
    : public ConvertOpToLLVMPattern<triton::AtomicRMWOp>,
      public LoadStoreConversionBase {
  AtomicRMWOpConversion(LLVMTypeConverter &converter,
                        const NVIDIA::TargetInfo &targetInfo,
                        ModuleAxisInfoAnalysis &axisAnalysisPass,
                        PatternBenefit benefit)
      : ConvertOpToLLVMPattern<triton::AtomicRMWOp>(converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  bool supportsVectorized(RMWOp opType, Type elementType) const {
    // vectorized atomics are only supported on hopper,
    // and only for specific atomic ops (add, min, max).
    // Note that "packed types" like f16x2 are supported sm60+.
    if (!targetInfo.supportVectorizedAtomics()) {
      return false;
    }

    return opType == RMWOp::FADD &&
           (elementType.isF16() || elementType.isBF16() || elementType.isF32());
  }

  bool isPromotableToNVPTXLD(triton::AtomicRMWOp op) const {
    if (disableLDAcquireLowering)
      return false;

    Type valueTy =
        getTypeConverter()->convertType(getElementTypeOrSelf(op.getType()));

    if (!valueTy.isIntOrFloat())
      return false;
    if (op.getSem() != triton::MemSemantic::ACQUIRE &&
        op.getSem() != triton::MemSemantic::RELAXED)
      return false;
    if (op.getScope() != triton::MemSyncScope::CTA &&
        op.getScope() != triton::MemSyncScope::GPU &&
        op.getScope() != triton::MemSyncScope::SYSTEM)
      return false;

    if (op.getAtomicRmwOp() != RMWOp::ADD && op.getAtomicRmwOp() != RMWOp::FADD)
      return false;
    if (isa<RankedTensorType>(op.getType()))
      return false;
    if (!op.getVal().getDefiningOp())
      return false;
    if (!isa<arith::ConstantOp>(op.getVal().getDefiningOp()))
      return false;

    auto constOp = cast<arith::ConstantOp>(op.getVal().getDefiningOp());
    if (!isa<FloatAttr>(constOp.getValueAttr()) &&
        !isa<IntegerAttr>(constOp.getValueAttr()))
      return false;

    if (auto attr = dyn_cast_or_null<FloatAttr>(constOp.getValueAttr()))
      if (!attr.getValue().isZero())
        return false;

    if (auto attr = dyn_cast_or_null<IntegerAttr>(constOp.getValueAttr()))
      if (!attr.getValue().isZero())
        return false;

    return true;
  }

  LogicalResult
  matchAndRewrite(triton::AtomicRMWOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    MLIRContext *ctx = rewriter.getContext();

    auto moduleOp = op->getParentOfType<ModuleOp>();
    assert(moduleOp && "Parent ModuleOp not found for AtomicRMWOp");
    int numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(moduleOp);

    auto atomicRmwAttr = op.getAtomicRmwOp();

    Value val = op.getVal();
    Value ptr = op.getPtr();

    Value llPtr = adaptor.getPtr();
    Value llVal = adaptor.getVal();
    Value llMask = adaptor.getMask();

    auto valElements = unpackLLElements(loc, llVal, rewriter);
    auto ptrElements = unpackLLElements(loc, llPtr, rewriter);
    SmallVector<Value> maskElements;
    if (llMask)
      maskElements = unpackLLElements(loc, llMask, rewriter);

    auto valueTy = op.getType();
    auto tensorTy = dyn_cast<RankedTensorType>(valueTy);
    Type valueElemTy =
        tensorTy ? getTypeConverter()->convertType(tensorTy.getElementType())
                 : valueTy;
    const size_t valueElemNBits = valueElemTy.getIntOrFloatBitWidth();
    auto elemsPerThread = getTotalElemsPerThread(val.getType());
    // packed: e.g. packed=2 for f16x2
    // vec: e.g. .v2, .v4, .v8 version of atom instruction.
    unsigned vec, vecOrig;
    int numElems, packed;
    if (tensorTy) {
      vec = getVectorSize(ptr);
      if (llMask) {
        vec = std::min<unsigned>(vec, getMaskAlignment(op.getMask()));
      }
      vecOrig = vec;
      packed = 1;
      auto valTy = cast<RankedTensorType>(val.getType());
      if (!supportsVectorized(atomicRmwAttr, valTy.getElementType())) {
        packed =
            std::min<unsigned>(vecOrig, valTy.getElementType().isF16() ? 2 : 1);
        vec = 1;
      }
      numElems = tensorTy.getNumElements();
    } else {
      // scalar
      vec = 1;
      vecOrig = 1;
      numElems = 1;
      packed = 1;
    }
    assert((packed == 1 || vec == 1) && "packed or vec must be 1");

    if (vec * packed == 1 && numElems > 1)
      op->emitRemark() << "Warning: vectorization fails vec = " << vec
                       << " packed = " << packed << " origin vec = " << vecOrig
                       << " numElems = " << numElems;

    auto freeVarMasks = getFreeVariableMasks(ptr.getType());
    Value threadPred =
        emitRedundantThreadPredicate(freeVarMasks, rewriter, loc, targetInfo);
    uint32_t regMask = freeVarMasks[str_attr("reg")];

    auto packedTy = vec_ty(valueElemTy, packed);
    SmallVector<Value> resultVals(elemsPerThread);

    // Lower AtomicRMWOp to a ld.acquire if possible
    std::unordered_map<triton::MemSyncScope, triton::nvgpu::MemSyncScope>
        ScopeMap = {
            {triton::MemSyncScope::CTA, triton::nvgpu::MemSyncScope::CTA},
            {triton::MemSyncScope::GPU, triton::nvgpu::MemSyncScope::GPU},
            {triton::MemSyncScope::SYSTEM,
             triton::nvgpu::MemSyncScope::SYSTEM}};
    const bool doPTXLDPromotion = isPromotableToNVPTXLD(op) && vec == 1 &&
                                  packed == 1 && ScopeMap.count(op.getScope());

    for (size_t i = 0; i < elemsPerThread; i += vec * packed) {
      if (auto canonicalStart = getCanonicalIndex(i, regMask);
          canonicalStart != i) {
        // For redundant registers, refer back to the canonical result
        for (auto iVecPack = 0; iVecPack < vec * packed; ++iVecPack) {
          resultVals[i + iVecPack] = resultVals[canonicalStart + iVecPack];
        }
        continue;
      }

      Value rmwPtr = ptrElements[i];
      Value pred = llMask ? maybeAnd(rewriter, loc, threadPred, maskElements[i])
                          : threadPred;

      if (doPTXLDPromotion) {
        Type convertedValueTy =
            getTypeConverter()->convertType(getElementTypeOrSelf(op.getType()));
        auto loadAcquireOp = rewriter.create<triton::nvgpu::LoadAcquireOp>(
            op.getLoc(), convertedValueTy, rmwPtr, pred,
            op.getSem() == triton::MemSemantic::ACQUIRE
                ? triton::nvgpu::MemSemantic::ACQUIRE
                : triton::nvgpu::MemSemantic::RELAXED,
            ScopeMap[op.getScope()]);

        auto ASMReturnTy = void_ty(ctx);
        if (!atomicNeedsSharedMemory(op.getResult())) {
          rewriter.eraseOp(op);
          return success();
        }
        Value atomPtr = LLVM::getSharedMemoryBase(loc, rewriter, targetInfo,
                                                  op.getOperation());
        atomPtr = b.bitcast(atomPtr, ptr_ty(ctx, 3));
        // Only threads with rmwMask = True store the result
        targetInfo.storeShared(rewriter, loc, atomPtr, loadAcquireOp, pred);
        createBarrier(rewriter, loc, numCTAs);
        Value ret = b.load(valueElemTy, atomPtr);
        rewriter.replaceOp(op, {ret});
        continue;
      }

      // Let LLVM handle compare+swap loop; branch-based pred should be fine
      if (valueElemTy.isBF16() && getNVIDIAComputeCapability(moduleOp) < 90) {
        // Lower atomic bin-op and sem to LLVM
        auto llvmAtomicBinOp = matchAtomicOp(atomicRmwAttr);
        auto llvmAtomicMemOrdering = getMemoryOrdering(op.getSem());

        // Generate dominating undef
        Value undefVal = b.undef(valueElemTy);

        // Create basic block and branch to handle mask
        auto *curBlock = rewriter.getInsertionBlock();
        auto *endBlock = curBlock->splitBlock(rewriter.getInsertionPoint());
        auto *atomicBlock = rewriter.createBlock(
            curBlock->getParent(), std::next(Region::iterator(curBlock)));

        // Setup the BlockArgument to return the result
        endBlock->addArgument({valueElemTy}, {loc});

        // Enter into predicate block
        rewriter.setInsertionPointToEnd(curBlock);
        bool doesAtomicNeedMEM = atomicNeedsSharedMemory(op.getResult());

        // Setup for SMEM Sync case
        Value atomPtr = tensorTy || !doesAtomicNeedMEM
                            ? nullptr
                            : LLVM::getSharedMemoryBase(
                                  loc, rewriter, targetInfo, op.getOperation());
        rewriter.create<LLVM::CondBrOp>(loc, pred, atomicBlock, endBlock,
                                        undefVal);

        // Codegen the atomic-rmw instruction(s)
        rewriter.setInsertionPointToEnd(atomicBlock);
        Value atom = rewriter
                         .create<LLVM::AtomicRMWOp>(
                             loc, *llvmAtomicBinOp, rmwPtr, valElements[i],
                             *llvmAtomicMemOrdering, StringRef("agent"))
                         .getResult();
        // Handle the 2 bf16 case
        if (packed == 2 && valueElemNBits == 16) {
          Value atom2 = rewriter
                            .create<LLVM::AtomicRMWOp>(
                                loc, *llvmAtomicBinOp, ptrElements[i + 1],
                                valElements[i + 1], *llvmAtomicMemOrdering,
                                StringRef("agent"))
                            .getResult();
          auto vecTy = vec_ty(valueElemTy, vec);
          auto tmp =
              b.insert_element(vecTy, b.undef(vecTy), atom, b.i32_val(0));
          atom = b.insert_element(vecTy, tmp, atom2, b.i32_val(1)).getResult();
        }

        if (tensorTy) {
          // Return from predicated block
          rewriter.create<LLVM::BrOp>(loc, atom, endBlock);

          // Recover values from predicated block
          rewriter.setInsertionPointToStart(endBlock);
          Value ret = endBlock->getArgument(0);
          if (vec > 1) {
            for (unsigned ii = 0; ii < vec; ++ii) {
              resultVals[i + ii] = b.extract_val(valueElemTy, ret, ii);
            }
          } else if (packed > 1) {
            for (unsigned ii = 0; ii < packed; ++ii) {
              resultVals[i + ii] =
                  b.extract_element(valueElemTy, ret, b.i32_val(ii));
            }
          } else {
            resultVals[i] = ret;
          }
        } else {
          if (!doesAtomicNeedMEM) {
            rewriter.create<LLVM::BrOp>(loc, atom, endBlock);
            rewriter.eraseOp(op);
            // if type isn't a tensor and there is no need to write to SMEM then
            // we are done here
            return success();
          }

          // Commit values from predicated block to SMEM and return from
          // predicate block
          // Note: there is no need to use the BlockArgument here because
          //       the value is recovered from SMEM in the !tensorTy case
          b.store(atom, atomPtr);
          rewriter.create<LLVM::BrOp>(loc, atom, endBlock);

          // Recover values from predicated block (from SMEM)
          rewriter.setInsertionPointToStart(endBlock);
          b.barrier();
          Value ret = b.load(valueElemTy, atomPtr);
          rewriter.replaceOp(op, {ret});
        }
        continue;
      }

      std::string sTy;
      PTXBuilder ptxBuilderAtomicRMW;
      // 16-bit -> "h", 32-bit -> "r", 64-bit -> "l"
      std::string tyId =
          getRegisterSizeCode(valueElemNBits * packed, /*is_float=*/false);

      PTXBuilder::Operand *dstOpr;
      if (vec > 1) {
        dstOpr = ptxBuilderAtomicRMW.newListOperand();
        for (unsigned ii = 0; ii < vec; ++ii) {
          dstOpr->listAppend(
              ptxBuilderAtomicRMW.newOperand("=" + tyId, /*init=*/true));
        }
      } else {
        dstOpr = ptxBuilderAtomicRMW.newOperand("=" + tyId, /*init=*/true);
      }

      auto *ptrOpr = ptxBuilderAtomicRMW.newAddrOperand(rmwPtr, "l");

      PTXBuilder::Operand *valOpr;
      if (vec > 1) {
        valOpr = ptxBuilderAtomicRMW.newListOperand();
        for (unsigned ii = 0; ii < vec; ++ii) {
          valOpr->listAppend(
              ptxBuilderAtomicRMW.newOperand(valElements[i + ii], tyId));
        }
      } else if (packed > 1) {
        Value rmwVal = b.undef(packedTy);
        for (int ii = 0; ii < packed; ++ii) {
          rmwVal = b.insert_element(packedTy, rmwVal, valElements[i + ii],
                                    b.i32_val(ii));
        }
        valOpr = ptxBuilderAtomicRMW.newOperand(rmwVal, tyId);
      } else {
        valOpr = ptxBuilderAtomicRMW.newOperand(valElements[i], tyId);
      }

      auto scope = stringifyMemSyncScope(op.getScope()).str();
      auto &atom = ptxBuilderAtomicRMW.create<>("atom")->global().o(scope);
      auto rmwOp = stringifyRMWOp(atomicRmwAttr).str();
      auto sBits = std::to_string(valueElemNBits);
      switch (atomicRmwAttr) {
      case RMWOp::AND:
        sTy = "b" + sBits;
        break;
      case RMWOp::OR:
        sTy = "b" + sBits;
        break;
      case RMWOp::XOR:
        sTy = "b" + sBits;
        break;
      case RMWOp::ADD:
        sTy = "u" + sBits;
        break;
      case RMWOp::FADD:
        rmwOp = "add";
        rmwOp += (valueElemNBits == 16 ? ".noftz" : "");
        sTy = (valueElemTy.isBF16() ? "bf" : "f") + sBits;
        sTy += (packed == 2 && valueElemNBits == 16) ? "x2" : "";
        break;
      case RMWOp::MAX:
        sTy = "s" + sBits;
        break;
      case RMWOp::MIN:
        sTy = "s" + sBits;
        break;
      case RMWOp::UMAX:
        rmwOp = "max";
        sTy = "u" + sBits;
        break;
      case RMWOp::UMIN:
        rmwOp = "min";
        sTy = "u" + sBits;
        break;
      case RMWOp::XCHG:
        sTy = "b" + sBits;
        break;
      default:
        return failure();
      }
      std::string semStr;
      llvm::raw_string_ostream os(semStr);
      os << op.getSem();
      atom.o(semStr).o(rmwOp).v(vec).o(sTy);
      if (tensorTy) {
        atom(dstOpr, ptrOpr, valOpr).maybePredicate(pred);
        Type retType;
        if (vec > 1) {
          SmallVector<Type> retTys(vec, valueElemTy);
          retType = struct_ty(retTys);
        } else if (packed > 1) {
          retType = packedTy;
        } else {
          retType = valueElemTy;
        }

        auto ret = ptxBuilderAtomicRMW.launch(rewriter, loc, retType);

        if (vec > 1) {
          for (unsigned ii = 0; ii < vec; ++ii) {
            resultVals[i + ii] = b.extract_val(valueElemTy, ret, ii);
          }
        } else if (packed > 1) {
          for (unsigned ii = 0; ii < packed; ++ii) {
            resultVals[i + ii] =
                b.extract_element(valueElemTy, ret, b.i32_val(ii));
          }
        } else {
          resultVals[i] = ret;
        }

      } else {
        auto ASMReturnTy = void_ty(ctx);
        atom(dstOpr, ptrOpr, valOpr).maybePredicate(pred);
        auto old = ptxBuilderAtomicRMW.launch(rewriter, loc, valueElemTy);
        if (!atomicNeedsSharedMemory(op.getResult())) {
          rewriter.eraseOp(op);
          return success();
        }
        Value atomPtr = LLVM::getSharedMemoryBase(loc, rewriter, targetInfo,
                                                  op.getOperation());
        atomPtr = b.bitcast(atomPtr, ptr_ty(ctx, 3));
        // Only threads with rmwMask = True store the result
        targetInfo.storeShared(rewriter, loc, atomPtr, old, pred);
        createBarrier(rewriter, loc, numCTAs);
        Value ret = b.load(valueElemTy, atomPtr);
        rewriter.replaceOp(op, {ret});
      }
    }
    if (tensorTy) {
      Type structTy = getTypeConverter()->convertType(tensorTy);
      Value resultStruct = packLLElements(loc, getTypeConverter(), resultVals,
                                          rewriter, structTy);
      rewriter.replaceOp(op, {resultStruct});
    }
    return success();
  }
};

struct AsyncCopyGlobalToLocalOpConversion
    : public ConvertOpToLLVMPattern<triton::gpu::AsyncCopyGlobalToLocalOp>,
      public LoadStoreConversionBase {
  AsyncCopyGlobalToLocalOpConversion(LLVMTypeConverter &converter,
                                     const NVIDIA::TargetInfo &targetInfo,
                                     ModuleAxisInfoAnalysis &axisAnalysisPass,
                                     PatternBenefit benefit)
      : ConvertOpToLLVMPattern(converter, benefit),
        LoadStoreConversionBase(targetInfo, axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::gpu::AsyncCopyGlobalToLocalOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = getContext();
    auto loc = op.getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    Value res = op.getResult();
    Value mask = op.getMask();
    Value other = op.getOther();
    auto funcOp = op->getParentOfType<FunctionOpInterface>();

    auto srcTy = op.getSrc().getType();
    auto dstTy = op.getResult().getType();
    auto resElemTy = getTypeConverter()->convertType(dstTy.getElementType());

    Value llDst = adaptor.getResult();
    Value llSrc = adaptor.getSrc();
    Value llMask = adaptor.getMask();
    Value llOther = adaptor.getOther();

    // %src
    auto srcElems = unpackLLElements(loc, llSrc, rewriter);

    // %mask
    SmallVector<Value> maskElems;
    if (llMask) {
      maskElems = unpackLLElements(loc, llMask, rewriter);
      assert(srcElems.size() == maskElems.size());
    }

    // We assume other = 0, see XXX(Keren) below
    // %other
    // SmallVector<Value> otherElems;
    // if (llOther) {
    //   otherElems = unpackLLElements(loc, llOther, rewriter);
    //   assert(srcElems.size() == otherElems.size());
    // }

    // zip(src, mask)
    SmallVector<Value> vals;
    auto ptrTy = srcElems[0].getType();
    auto structTy =
        LLVM::LLVMStructType::getLiteral(ctx, ArrayRef<Type>{ptrTy, i1_ty});
    for (int i = 0; i < srcElems.size(); i++) {
      Value packedArr = rewriter.create<LLVM::UndefOp>(loc, structTy);
      packedArr = b.insert_val(packedArr, srcElems[i], 0);
      auto maskElem = llMask ? maskElems[i] : b.false_val();
      packedArr = b.insert_val(packedArr, maskElem, 1);
      vals.push_back(packedArr);
    }

    // Remove broadcasted registers
    auto srcLayout = ttg::toLinearLayout(srcTy);
    auto removeBroadcastSrc = actionRemoveBroadcastedRegs(srcLayout);
    srcLayout = removeBroadcastSrc.apply(srcLayout);
    vals = removeBroadcastSrc.apply(vals);

    // We can load N elements at a time if:
    //  1. Every group of N source pointers are contiguous.  For example, if
    //     N=2, then the pointers should be [x, x+1, y, y+1, ...].
    //  2. The mask (if present) has "alignment" N, meaning that each group of N
    //     mask bits are the same.  For example if N=2, the mask must be
    //     [x, x, y, y, ...].
    unsigned maxVec = getContiguity(op.getSrc());
    if (mask) {
      maxVec = std::min(maxVec, getMaskAlignment(mask));
    }
    // The maximum vector size is 128 bits on NVIDIA GPUs.
    maxVec = std::min(maxVec, 128 / resElemTy.getIntOrFloatBitWidth());

    int vecBytes = maxVec * resElemTy.getIntOrFloatBitWidth() / 8;
    if (vecBytes < 4) {
      return emitError(loc, "cp.async does not support transfers smaller than "
                            "4 bytes; calculated this as ")
             << vecBytes << " bytes";
    }
    assert(vecBytes == 16 || vecBytes == 8 || vecBytes == 4);

    auto freeVarMasks = getFreeVariableMasks(srcTy);
    // NOTE(@peterbell10): We load redundant data on different CTAs, so the data
    // is available in each CTAs respective shared memory. Otherwise, we would
    // need an additional broadcast step to copy the data between CTAs.
    freeVarMasks[str_attr("block")] = 0;
    Value threadPred =
        emitRedundantThreadPredicate(freeVarMasks, rewriter, loc, targetInfo);

    auto emitCpAsync = [&b, threadPred, ptrTy, hasMask = bool(llMask)](
                           ConversionPatternRewriter &rewriter, Location loc,
                           ArrayRef<Value> vals, Value shmemAddr, int startIdx,
                           VectorType vecTy) -> SmallVector<Value> {
      assert(isa<VectorType>(vecTy));
      auto *ctx = rewriter.getContext();
      auto elemTy = vecTy.getElementType();
      auto nBytes = vecTy.getNumElements() * elemTy.getIntOrFloatBitWidth() / 8;
      assert(nBytes == 16 || nBytes == 8 || nBytes == 4);
      // Tune CG and CA.
      CacheModifier srcCacheModifier =
          nBytes == 16 ? CacheModifier::CG : CacheModifier::CA;

      auto structElem = vals[startIdx];
      auto srcElem = b.extract_val(ptrTy, structElem, 0);
      auto maskElem = b.extract_val(i1_ty, structElem, 1);

      PTXBuilder ptxBuilder;
      auto &copyAsyncOp =
          *ptxBuilder.create<PTXCpAsyncLoadInstr>(srcCacheModifier);
      auto *dstOperand = ptxBuilder.newAddrOperand(shmemAddr, "r");
      auto *srcOperand = ptxBuilder.newAddrOperand(srcElem, "l");
      auto *copySize = ptxBuilder.newConstantOperand(nBytes);
      auto *srcSize = copySize;
      if (hasMask) {
        // We don't use predicate in this case, setting src-size to 0
        // if there's any mask. cp.async will automatically fill the
        // remaining slots with 0 if cp-size > src-size.
        // XXX(Keren): Always assume other = 0 for now.
        // When 'other != 0' is supported, we will need to fold the
        // op.getMask() and redundantDataMask() into the same predicate, the
        // way it is done for LoadOp.
        auto selectOp = b.select(maskElem, b.i32_val(nBytes), b.i32_val(0));
        srcSize = ptxBuilder.newOperand(selectOp, "r");
      }
      copyAsyncOp(dstOperand, srcOperand, copySize, srcSize)
          .maybePredicate(threadPred);
      ptxBuilder.launch(rewriter, loc, void_ty(ctx));
      return {};
    };

    // %dst
    auto smemObj =
        getSharedMemoryObjectFromStruct(loc, llDst, resElemTy, rewriter);
    auto smemLayout = ttg::toLinearLayout(dstTy);
    auto cvt = srcLayout.invertAndCompose(smemLayout);
    if (!cvt.isTrivialOver({str_attr("block")})) {
      return emitError(loc,
                       "cp.async does not support non-trivial block dimension");
    }
    cvt = cvt.sublayout(
        {str_attr("register"), str_attr("lane"), str_attr("warp")},
        {str_attr("offset")});
    auto affineOffset = smemObj.getShmemOffset(loc, rewriter, dstTy);
    auto maskSpanAffineOffset = SharedMemoryObject::getMaskSpanOffsets(dstTy);
    lowerLdSt(
        loc, ctx, cvt, vals, resElemTy, smemObj.getBase(),
        [](Value v) { return v; }, affineOffset, maskSpanAffineOffset, rewriter,
        targetInfo, maxVec, emitCpAsync);

    // Drop the result token.
    Value zero = rewriter.create<LLVM::ConstantOp>(
        op.getLoc(), IntegerType::get(op.getContext(), 32),
        rewriter.getI32IntegerAttr(0));
    rewriter.replaceOp(op, zero);
    return success();
  }
};

static LinearLayout getMsgToPackedOffsetLayout(ttg::MemDescType ty) {
  auto ctx = ty.getContext();
  auto kMsg = str_attr("msg");
  auto kBlock = str_attr("block");
  auto shapePerCTA = ttg::getShapePerCTA(ty);
  int rank = shapePerCTA.size();
  auto blockShape = ttng::getTMABlockShape(ty, /*packedSize=*/true);
  auto outDimNames = standardOutDimNames(ctx, rank);
  LinearLayout msgToOffset;
  for (int dim = 0; dim < rank; ++dim) {
    msgToOffset *=
        LinearLayout::strided1D(shapePerCTA[dim] / blockShape[dim],
                                blockShape[dim], kMsg, outDimNames[dim]);
  }
  auto ctaLayout = getCTALayout(ty.getEncoding());
  for (int i = 0; i < rank; ++i) {
    auto dim = ctaLayout.getCTAOrder()[i];
    msgToOffset *= LinearLayout::identity1D(ctaLayout.getCTASplitNum()[dim],
                                            kBlock, outDimNames[dim]);
  }
  return msgToOffset;
}

static LinearLayout
getMsgToUnpackedOffsetLayout(const LinearLayout &packedLayout,
                             ttg::MemDescType ty) {
  auto isFp4Padded =
      cast<NVMMASharedEncodingAttr>(ty.getEncoding()).getFp4Padded();
  if (!isFp4Padded) {
    return packedLayout;
  }
  auto ctx = ty.getContext();
  auto rank = ty.getRank();
  auto kMsg = str_attr("msg");
  auto kLastDim = str_attr("dim" + Twine(rank - 1));
  // Multiply to offset by 2 in the last dimension
  auto unpackLayout = LinearLayout::zeros1D(1, kMsg, kLastDim, 2);
  return unpackLayout * packedLayout;
}

struct AsyncTMACopyGlobalToLocalOpConversion
    : public ConvertOpToLLVMPattern<
          triton::nvidia_gpu::AsyncTMACopyGlobalToLocalOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::AsyncTMACopyGlobalToLocalOp op,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    if (op.getCache() != triton::CacheModifier::NONE)
      return op.emitError("cache modifiers not supported yet");
    if (op.getEvict() != triton::EvictionPolicy::NORMAL)
      return op.emitError("eviction policy not supported yet");
    if (op.getIsVolatile())
      return op.emitError("volatile not supported yet");

    auto loc = op.getLoc();
    auto b = TritonLLVMOpBuilder(loc, rewriter);
    Type llvmElemTy =
        typeConverter->convertType(op.getResult().getType().getElementType());
    auto barrierMemObj = LLVM::getSharedMemoryObjectFromStruct(
        loc, adaptor.getBarrier(),
        typeConverter->convertType(op.getBarrier().getType().getElementType()),
        rewriter);
    auto dstMemObj = LLVM::getSharedMemoryObjectFromStruct(
        loc, adaptor.getResult(), llvmElemTy, rewriter);
    auto voidTy = void_ty(op->getContext());
    auto id = getThreadId(rewriter, loc);

    auto mod = op->getParentOfType<ModuleOp>();
    int numWarps = ttg::lookupNumWarps(op);
    int warpSize = ttg::TritonGPUDialect::getThreadsPerWarp(mod);
    Value warpID = rewriter.create<nvgpu::WarpIdOp>(loc);
    Value pred = adaptor.getPred();
    // Select just one thread for the TMA copy. This also helps the compiler to
    // figure out that the op is uniform.
    pred = b.and_(pred, LLVM::NVIDIA::createElectPredicate(loc, rewriter));

    auto smemTy = op.getResult().getType();
    Attribute encoding = smemTy.getEncoding();
    auto mmaEncoding = dyn_cast_or_null<NVMMASharedEncodingAttr>(encoding);
    int elementSizeInBytes =
        op.getResult().getType().getElementType().getIntOrFloatBitWidth() / 8;
    int packingFactor = (mmaEncoding && mmaEncoding.getFp4Padded()) ? 2 : 1;

    auto shapePerCTA = ttg::getShapePerCTA(smemTy);
    int rank = op.getCoord().size();

    auto msgToPackedOffset = getMsgToPackedOffsetLayout(smemTy);
    auto smemLayout = ttg::toLinearLayout(smemTy);
    auto msgToShared = msgToPackedOffset.invertAndCompose(smemLayout);
    auto msgToOffset = getMsgToUnpackedOffsetLayout(msgToPackedOffset, smemTy);

    auto ctx = op.getContext();
    auto kMsg = str_attr("msg");
    auto kBlock = str_attr("block");
    const auto numCopies = msgToOffset.getInDimSize(kMsg);
    auto zero = b.i32_val(0);
    auto ctaId = rewriter.create<nvgpu::ClusterCTAIdOp>(loc);

    // The bounding box inner dimension must be less than or equal to the
    // swizzle size.
    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__TENSOR__MEMORY.html#group__CUDA__TENSOR__MEMORY_1ga7c7d2aaac9e49294304e755e6f341d7
    // We clamp the block size and the codegen will emit multiple copy
    // operations.
    for (int copyIdx = 0; copyIdx < numCopies; copyIdx += numWarps) {
      int numWarpsToCopy = std::min(numCopies - copyIdx, numWarps);
      if (numWarpsToCopy == 1)
        warpID = b.i32_val(0);
      Value boxPred =
          b.and_(pred, b.icmp_ult(id, b.i32_val(numWarpsToCopy * warpSize)));
      ::mlir::triton::PTXBuilder ptxBuilderTMA;
      Type elemPtrTy = ptr_ty(rewriter.getContext(), 3);
      Value copyIdxVal = b.add(warpID, b.i32_val(copyIdx));
      Value shMemOffset =
          applyLinearLayout(loc, rewriter, msgToShared,
                            {{kMsg, copyIdxVal}, {kBlock, zero}})[0]
              .second;
      Value shMemPtr =
          b.gep(elemPtrTy, llvmElemTy, dstMemObj.getBase(), shMemOffset);
      SmallVector<PTXBuilder::Operand *> operands = {
          ptxBuilderTMA.newOperand(boxPred, "b"),
          ptxBuilderTMA.newOperand(shMemPtr, "r"),
          ptxBuilderTMA.newOperand(adaptor.getDesc(), "l")};
      std::string tmaInst =
          "@$0 cp.async.bulk.tensor." + std::to_string(rank) +
          "d.shared::cluster.global.mbarrier::complete_tx::bytes [$1], [$2, {";

      auto offsets = applyLinearLayout(loc, rewriter, msgToOffset,
                                       {{kMsg, copyIdxVal}, {kBlock, ctaId}});
      int operandIdx = 3;
      for (int i = 0; i < rank; i++) {
        Value coord = adaptor.getCoord()[rank - i - 1];
        if (i < offsets.size())
          coord = b.add(coord, offsets[offsets.size() - i - 1].second);
        operands.push_back(ptxBuilderTMA.newOperand(coord, "r"));
        tmaInst += "$" + std::to_string(operandIdx++);
        if (i != rank - 1)
          tmaInst += ", ";
      }
      operands.push_back(
          ptxBuilderTMA.newOperand(barrierMemObj.getBase(), "r"));
      tmaInst += "}], [$" + std::to_string(operandIdx++) + "];";

      auto &tma = *ptxBuilderTMA.create<>(tmaInst);
      tma(operands, /*onlyAttachMLIRArgs=*/true);
      ptxBuilderTMA.launch(rewriter, loc, voidTy);
    }
    rewriter.eraseOp(op);
    return success();
  }
};

LogicalResult convertTMAStoreLikeOp(Operation *op,
                                    const TypeConverter *typeConverter,
                                    ConversionPatternRewriter &rewriter,
                                    Value tmaPtr, ttg::MemDescType srcTy,
                                    Value src, ValueRange coords,
                                    const std::string &tmaInst) {
  auto loc = op->getLoc();
  auto b = TritonLLVMOpBuilder(loc, rewriter);
  Type llvmElemTy = typeConverter->convertType(srcTy.getElementType());
  auto dstMemObj =
      LLVM::getSharedMemoryObjectFromStruct(loc, src, llvmElemTy, rewriter);
  auto voidTy = void_ty(op->getContext());
  auto id = getThreadId(rewriter, loc);
  // Select just one thread for the TMA copy. This also helps the compiler to
  // figure out that the op is uniform.
  Value pred = LLVM::NVIDIA::createElectPredicate(loc, rewriter);
  int elementSizeInBytes = srcTy.getElementType().getIntOrFloatBitWidth() / 8;

  auto mod = op->getParentOfType<ModuleOp>();
  int numWarps = ttg::lookupNumWarps(op);
  int warpSize = ttg::TritonGPUDialect::getThreadsPerWarp(mod);
  Value warpID = rewriter.create<nvgpu::WarpIdOp>(loc);
  auto shapePerCTA = ttg::getShapePerCTA(srcTy);
  int elementsPerCTA = product(shapePerCTA);

  auto rank = coords.size();
  auto encoding = srcTy.getEncoding();

  auto msgToPackedOffset = getMsgToPackedOffsetLayout(srcTy);
  auto smemLayout = ttg::toLinearLayout(srcTy);
  auto msgToShared = msgToPackedOffset.invertAndCompose(smemLayout);
  auto msgToOffset = getMsgToUnpackedOffsetLayout(msgToPackedOffset, srcTy);

  auto ctx = op->getContext();
  auto kMsg = str_attr("msg");
  auto kBlock = str_attr("block");
  auto numCopies = msgToOffset.getInDimSize(kMsg);
  auto zero = b.i32_val(0);
  auto ctaId = rewriter.create<nvgpu::ClusterCTAIdOp>(loc);

  for (int copyIdx = 0; copyIdx < numCopies; copyIdx += numWarps) {
    int numWarpsToCopy = std::min(numCopies - copyIdx, numWarps);
    if (numWarpsToCopy == 1)
      warpID = b.i32_val(0);
    Value boxPred =
        b.and_(pred, b.icmp_ult(id, b.i32_val(numWarpsToCopy * warpSize)));
    ::mlir::triton::PTXBuilder ptxBuilderTMA;
    Type elemPtrTy = ptr_ty(rewriter.getContext(), 3);
    Value copyIdxVal = b.add(warpID, b.i32_val(copyIdx));
    Value shMemOffset =
        applyLinearLayout(loc, rewriter, msgToShared,
                          {{kMsg, copyIdxVal}, {kBlock, zero}})[0]
            .second;
    Value shMemPtr =
        b.gep(elemPtrTy, llvmElemTy, dstMemObj.getBase(), shMemOffset);
    SmallVector<PTXBuilder::Operand *> operands = {
        ptxBuilderTMA.newOperand(boxPred, "b"),
        ptxBuilderTMA.newOperand(tmaPtr, "l")};

    auto offsets = applyLinearLayout(loc, rewriter, msgToOffset,
                                     {{kMsg, copyIdxVal}, {kBlock, ctaId}});
    int operandIdx = 2;
    for (int i = 0; i < rank; i++) {
      Value coord = coords[rank - i - 1];
      if (i < offsets.size())
        coord = b.add(coord, offsets[offsets.size() - i - 1].second);
      operands.push_back(ptxBuilderTMA.newOperand(coord, "r"));
    }
    operands.push_back(ptxBuilderTMA.newOperand(shMemPtr, "r"));
    auto &tma = *ptxBuilderTMA.create<>(tmaInst);
    tma(operands, /*onlyAttachMLIRArgs=*/true);
    ptxBuilderTMA.launch(rewriter, loc, voidTy);
  }

  // TODO: Separate the syncronizations operations into separate TTGIR ops to
  // be able to schedule them at the high level.
  rewriter.create<NVVM::CpAsyncBulkCommitGroupOp>(loc);

  rewriter.eraseOp(op);
  return success();
};

struct AsyncTMACopyLocalToGlobalOpConversion
    : public ConvertOpToLLVMPattern<
          triton::nvidia_gpu::AsyncTMACopyLocalToGlobalOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::AsyncTMACopyLocalToGlobalOp op,
                  OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    std::ostringstream tmaInst;
    auto rank = op.getCoord().size();
    tmaInst << "@$0 cp.async.bulk.tensor." << rank;
    tmaInst << "d.global.shared::cta.bulk_group [$1, {";
    int operandIdx = 2;
    for (int i = 0; i < rank; i++) {
      tmaInst << "$" << operandIdx++;
      if (i != rank - 1)
        tmaInst << ", ";
    }
    tmaInst << "}], [$" << (operandIdx++) << "];";
    return convertTMAStoreLikeOp(op, typeConverter, rewriter, adaptor.getDesc(),
                                 op.getSrc().getType(), adaptor.getSrc(),
                                 adaptor.getCoord(), tmaInst.str());
  }
};

struct AsyncTMAReduceOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::AsyncTMAReduceOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::AsyncTMAReduceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    std::ostringstream tmaInst;
    auto rank = op.getCoord().size();
    auto kind = stringifyDescriptorReduceKind(op.getKind());
    tmaInst << "@$0 cp.reduce.async.bulk.tensor." << rank;
    tmaInst << "d.global.shared::cta." << kind.str() << ".bulk_group [$1, {";
    int operandIdx = 2;
    for (int i = 0; i < rank; i++) {
      tmaInst << "$" << operandIdx++;
      if (i != rank - 1)
        tmaInst << ", ";
    }
    tmaInst << "}], [$" << (operandIdx++) << "];";
    return convertTMAStoreLikeOp(op, typeConverter, rewriter, adaptor.getDesc(),
                                 op.getSrc().getType(), adaptor.getSrc(),
                                 adaptor.getCoord(), tmaInst.str());
  }
};

static LinearLayout getUnswizzledLayout(triton::gpu::MemDescType type) {
  auto mmaEncoding = dyn_cast<NVMMASharedEncodingAttr>(type.getEncoding());
  if (!mmaEncoding) {
    assert(isa<ttg::SwizzledSharedEncodingAttr>(type.getEncoding()));
    return ttg::toLinearLayout(type);
  }
  return ttg::nvmmaSharedToLinearLayout(
      type.getShape(), cast<NVMMASharedEncodingAttr>(type.getEncoding()),
      /*disableSwizzle=*/true);
}

// This function is shared between the TMA gather and scatter lowerings. It
// handles the logic for iterating over the x offset values in groups of 4
// consecutive indices and mapping them to the appropriate shared memory offset.
//
// This invokes a callback with the predicate, shared memory offset, y offset,
// and x offsets.
static LogicalResult iterateGatherScatterIndices(
    Operation *op, ConversionPatternRewriter &rewriter,
    const TypeConverter &typeConverter,
    mlir::TypedValue<RankedTensorType> xCoords,
    mlir::TypedValue<ttg::MemDescType> smem, Value smemObjValue,
    Value xOffsetsValue, Value yOffsetValue, Value pred,
    function_ref<void(Value, Value, Value, ArrayRef<Value>)> callback) {
  MLIRContext *ctx = op->getContext();
  Location loc = op->getLoc();
  auto b = TritonLLVMOpBuilder(loc, rewriter);

  StringAttr kDim0 = str_attr("dim0");
  StringAttr kDim1 = str_attr("dim1");
  StringAttr kMsg = str_attr("msg");
  StringAttr kRegister = str_attr("register");
  StringAttr kLane = str_attr("lane");
  StringAttr kWarp = str_attr("warp");
  StringAttr kBlock = str_attr("block");

  // Each warp can issue a distinct `gather4` instruction that loads 4 rows into
  // consecutive shared memory. Thus, the layout of the x offsets must be such
  // that 4 consecutive elements are broadcasted to a warp.
  LinearLayout xCoordsLayout = triton::gpu::toLinearLayout(xCoords.getType());
  if (xCoordsLayout.getInDimSize(kRegister) < 4)
    return op->emitError("must have at least 4 x offsets per warp");
  // Check that the first two bases are [1] and [2].
  for (unsigned i : {0, 1}) {
    if (xCoordsLayout.getBasis(kRegister, i).front() != (1 << i))
      return op->emitError(
          "x offsets are not grouped by 4 contiguous elements");
  }

  // TMA expects the memdesc shape to match the alloc shape.
  triton::gpu::MemDescType smemType = smem.getType();
  ArrayRef<int64_t> allocShape = smemType.getAllocShape();
  if (allocShape.size() < 2 || smemType.getShape() != allocShape.take_back(2))
    return op->emitError("memdesc shape must match alloc shape");
  // `NVMMASharedEncodingAttr` means the core matrix tiles are placed next to
  // each other in shared memory, which lines up with how `gather4` loads data.
  if (!isa<NVMMASharedEncodingAttr>(smemType.getEncoding()))
    return op->emitError("requires dst encoding NVMMASharedEncodingAttr");
  Type llvmElemTy = typeConverter.convertType(smemType.getElementType());
  Type elemPtrTy = ptr_ty(ctx, /*addrspace=*/3);
  auto smemObj = LLVM::getSharedMemoryObjectFromStruct(loc, smemObjValue,
                                                       llvmElemTy, rewriter);

  unsigned threadsPerWarp = xCoordsLayout.getInDimSize(kLane);
  unsigned numWarps = xCoordsLayout.getInDimSize(kWarp);

  // Each gather4 instructions reads contigDimSize columns, 4 rows at a time.
  auto shapePerCTA = ttg::getShapePerCTA(smemType);
  auto tmaBlockShape = ttng::getTMABlockShape(smemType, /*packedSize=*/true);
  unsigned innerBlockSize = shapePerCTA.back();
  unsigned contigDimSize = tmaBlockShape.back();
  unsigned numMessagesPerRow = ceil<unsigned>(innerBlockSize, contigDimSize);

  // `xCoordsLayout` maps the register ID into dim0. Tile dim1 by adding a new
  // dimension representing the TMA message ID.
  assert(innerBlockSize % numMessagesPerRow == 0);
  assert(llvm::isPowerOf2_32(numMessagesPerRow));
  unsigned msgSize = innerBlockSize / numMessagesPerRow;
  LinearLayout msgToCol =
      LinearLayout::strided1D(numMessagesPerRow, msgSize, kMsg, kDim1);
  LinearLayout msgLayout = xCoordsLayout * msgToCol;

  // `gather4` will put the segments of the 4 rows consecutively in
  // shared memory. However, if the 4 rows are smaller than the shared memory
  // swizzle tile size, e.g. [4, 32] vs. [8, 32], then, for example, the address
  // of the 0th element of row 4 will not be at the start of the segment.
  LinearLayout sharedLayout = getUnswizzledLayout(smemType);
  LinearLayout msgToShared = msgLayout.invertAndCompose(sharedLayout);

  // If there are too few rows, warps will have redundant data. An individual
  // thread might also have redundant indices if there is register broadcasting.
  auto freeVars = xCoordsLayout.getFreeVariableMasks();
  unsigned regMask = freeVars[kRegister];
  unsigned warpMask = freeVars[kWarp];
  if (freeVars[kLane] != (threadsPerWarp - 1))
    return op->emitError("x offsets must be broadcasted across each warp");

  Value warpId = rewriter.create<nvgpu::WarpIdOp>(loc);
  Value blockId = rewriter.create<nvgpu::ClusterCTAIdOp>(loc);

  // Mask out warps with redundant x offsets.
  pred = b.and_(pred,
                b.icmp_eq(b.i32_val(0), b.and_(warpId, b.i32_val(warpMask))));
  // Select one thread in each warp to issue the gather4 messages.
  pred = b.and_(pred, LLVM::NVIDIA::createElectPredicate(loc, rewriter));

  SmallVector<Value> xOffsets = unpackLLElements(loc, xOffsetsValue, rewriter);
  // Lane ID doesn't matter.
  Value laneId = b.i32_val(0);
  for (auto regId : seq<unsigned>(0, xOffsets.size(), 4)) {
    // Skip redundant x offsets within a thread.
    if ((regMask & regId) != 0)
      continue;
    Value regIdVal = b.i32_val(regId);

    for (auto msgId : llvm::seq(numMessagesPerRow)) {
      Value msgIdVal = b.i32_val(msgId);

      auto result = applyLinearLayout(loc, rewriter, msgToShared,
                                      {{kRegister, regIdVal},
                                       {kLane, laneId},
                                       {kWarp, warpId},
                                       {kBlock, blockId},
                                       {kMsg, msgIdVal}});
      assert(result.size() == 2 && result.front().first == "offset" &&
             result.back().first == "block");
      Value shMemOffset = result.front().second;
      // Because we checked that the memdesc's allocshape and shape match, we
      // can ignore the strides and directly index into the shmem object.
      Value shMemPtr =
          b.gep(elemPtrTy, llvmElemTy, smemObj.getBase(), shMemOffset);
      Value yOffset = b.add(yOffsetValue, b.i32_val(msgId * msgSize));

      callback(pred, shMemPtr, yOffset, ArrayRef(xOffsets).slice(regId, 4));
    };
  }

  return success();
}

struct AsyncTMAGatherOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::AsyncTMAGatherOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::AsyncTMAGatherOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override;
};

LogicalResult AsyncTMAGatherOpConversion::matchAndRewrite(
    triton::nvidia_gpu::AsyncTMAGatherOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Location loc = op.getLoc();
  MLIRContext *ctx = getContext();

  LLVM::LLVMVoidType voidTy = void_ty(op->getContext());
  auto barrierMemObj = LLVM::getSharedMemoryObjectFromStruct(
      loc, adaptor.getBarrier(),
      typeConverter->convertType(op.getBarrier().getType().getElementType()),
      rewriter);

  // Callback to generate the gather4 instruction.
  auto callback = [&](Value pred, Value shMemPtr, Value yOffset,
                      ArrayRef<Value> xOffsets) {
    std::string tmaInst = "@$0 cp.async.bulk.tensor.2d.tile::gather4.shared"
                          "::cluster.global.mbarrier::complete_tx::bytes "
                          "[$1], [$2, {$3, $4, $5, $6, $7}], [$8];";

    PTXBuilder ptxBuilder;
    SmallVector<PTXBuilder::Operand *, 9> operands{
        // clang-format off
        ptxBuilder.newOperand(pred, "b"),
        ptxBuilder.newOperand(shMemPtr, "r"),
        ptxBuilder.newOperand(adaptor.getDesc(), "l"),
        ptxBuilder.newOperand(yOffset, "r")
        // clang-format on
    };
    for (Value xOffset : xOffsets)
      operands.push_back(ptxBuilder.newOperand(xOffset, "r"));
    operands.push_back(ptxBuilder.newOperand(barrierMemObj.getBase(), "r"));

    auto &tma = *ptxBuilder.create<>(tmaInst);
    tma(operands, /*attachOnlyMLIRArgs=*/true);
    ptxBuilder.launch(rewriter, loc, voidTy);
  };

  if (failed(iterateGatherScatterIndices(
          op, rewriter, *getTypeConverter(), op.getXOffsets(), op.getResult(),
          adaptor.getResult(), adaptor.getXOffsets(), adaptor.getYOffset(),
          adaptor.getPred(), callback)))
    return failure();

  rewriter.eraseOp(op);
  return success();
}

struct AsyncTMAScatterOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::AsyncTMAScatterOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::AsyncTMAScatterOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override;
};

LogicalResult AsyncTMAScatterOpConversion::matchAndRewrite(
    triton::nvidia_gpu::AsyncTMAScatterOp op, OpAdaptor adaptor,
    ConversionPatternRewriter &rewriter) const {
  Location loc = op.getLoc();
  auto b = TritonLLVMOpBuilder(loc, rewriter);
  MLIRContext *ctx = getContext();
  LLVM::LLVMVoidType voidTy = void_ty(op->getContext());

  // Callback to generate the scatter4 instruction.
  auto callback = [&](Value pred, Value shMemPtr, Value yOffset,
                      ArrayRef<Value> xOffsets) {
    std::string tmaInst = "@$0 cp.async.bulk.tensor.2d.tile::scatter4.global"
                          ".shared::cta.bulk_group "
                          "[$1, {$2, $3, $4, $5, $6}], [$7];";

    PTXBuilder ptxBuilder;
    SmallVector<PTXBuilder::Operand *, 8> operands{
        // clang-format off
        ptxBuilder.newOperand(pred, "b"),
        ptxBuilder.newOperand(adaptor.getDesc(), "l"),
        ptxBuilder.newOperand(yOffset, "r")
        // clang-format on
    };
    for (Value xOffset : xOffsets)
      operands.push_back(ptxBuilder.newOperand(xOffset, "r"));
    operands.push_back(ptxBuilder.newOperand(shMemPtr, "r"));

    auto &tma = *ptxBuilder.create<>(tmaInst);
    tma(operands, /*attachOnlyMLIRArgs=*/true);
    ptxBuilder.launch(rewriter, loc, voidTy);
  };

  if (failed(iterateGatherScatterIndices(
          op, rewriter, *getTypeConverter(), op.getXOffsets(), op.getSrc(),
          adaptor.getSrc(), adaptor.getXOffsets(), adaptor.getYOffset(),
          /*pred=*/b.true_val(), callback)))
    return failure();

  // TODO: Separate the syncronizations operations into separate TTGIR ops to
  // be able to schedule them at the high level.
  rewriter.create<NVVM::CpAsyncBulkCommitGroupOp>(loc);

  rewriter.eraseOp(op);
  return success();
}

struct AsyncCopyMbarrierArriveOpConversion
    : public ConvertOpToLLVMPattern<ttng::AsyncCopyMbarrierArriveOp> {
  using ConvertOpToLLVMPattern<
      ttng::AsyncCopyMbarrierArriveOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(ttng::AsyncCopyMbarrierArriveOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto noinc = op.getNoIncrement();
    auto barrierMemObj = LLVM::getSharedMemoryObjectFromStruct(
        loc, adaptor.getBarrier(),
        typeConverter->convertType(op.getBarrier().getType().getElementType()),
        rewriter);
    TritonLLVMOpBuilder b(loc, rewriter);
    rewriter.create<NVVM::CpAsyncMBarrierArriveSharedOp>(
        loc, barrierMemObj.getBase(), noinc);
    op->erase();
    return success();
  }
};

struct AsyncWaitOpConversion
    : public ConvertOpToLLVMPattern<triton::gpu::AsyncWaitOp> {
  using ConvertOpToLLVMPattern<
      triton::gpu::AsyncWaitOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::gpu::AsyncWaitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto num = op->getAttrOfType<IntegerAttr>("num");
    rewriter.create<NVVM::CpAsyncWaitGroupOp>(loc, num);

    // Drop the result token.
    TritonLLVMOpBuilder b(loc, rewriter);
    rewriter.replaceOp(op, b.i32_val(0));
    return success();
  }
};

struct AsyncCommitGroupOpConversion
    : public ConvertOpToLLVMPattern<triton::gpu::AsyncCommitGroupOp> {
  using ConvertOpToLLVMPattern<
      triton::gpu::AsyncCommitGroupOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::gpu::AsyncCommitGroupOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    rewriter.create<NVVM::CpAsyncCommitGroupOp>(loc);

    // Drop the result token.
    TritonLLVMOpBuilder b(loc, rewriter);
    rewriter.replaceOp(op, b.i32_val(0));
    return success();
  }
};

struct TMAStoreWaitOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::TMAStoreWaitOp> {
  using ConvertOpToLLVMPattern::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::TMAStoreWaitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto ctx = op.getContext();
    auto isRead = UnitAttr::get(ctx);
    rewriter.replaceOpWithNewOp<NVVM::CpAsyncBulkWaitGroupOp>(
        op, op.getPendingsAttr(), isRead);
    return success();
  }
};

} // namespace

void mlir::triton::NVIDIA::populateLoadStoreOpToLLVMPatterns(
    LLVMTypeConverter &typeConverter, const TargetInfo &targetInfo,
    int computeCapability, RewritePatternSet &patterns,
    ModuleAxisInfoAnalysis &axisInfoAnalysis, PatternBenefit benefit) {
  patterns.add<AsyncCopyGlobalToLocalOpConversion, AtomicCASOpConversion,
               AtomicRMWOpConversion>(typeConverter, targetInfo,
                                      axisInfoAnalysis, benefit);
  patterns.add<LoadOpConversion, StoreOpConversion>(
      typeConverter, targetInfo, computeCapability, axisInfoAnalysis, benefit);
  patterns.add<AsyncCommitGroupOpConversion, AsyncWaitOpConversion,
               AsyncCopyMbarrierArriveOpConversion>(typeConverter, benefit);
  patterns.add<AsyncTMACopyGlobalToLocalOpConversion,
               AsyncTMACopyLocalToGlobalOpConversion,
               AsyncTMAReduceOpConversion, AsyncTMAGatherOpConversion,
               AsyncTMAScatterOpConversion, TMAStoreWaitOpConversion>(
      typeConverter, benefit);
}
