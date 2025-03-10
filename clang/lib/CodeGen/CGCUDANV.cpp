//===----- CGCUDANV.cpp - Interface to NVIDIA CUDA Runtime ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This provides a class for CUDA code generation targeting the NVIDIA CUDA
// runtime library.
//
//===----------------------------------------------------------------------===//

#include "CGCUDARuntime.h"
#include "CGCXXABI.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/Cuda.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ConstantInitBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/Support/Format.h"

using namespace clang;
using namespace CodeGen;

namespace {
constexpr unsigned CudaFatMagic = 0x466243b1;
constexpr unsigned HIPFatMagic = 0x48495046; // "HIPF"

class CGNVCUDARuntime : public CGCUDARuntime {

private:
  llvm::IntegerType *IntTy, *SizeTy;
  llvm::Type *VoidTy;
  llvm::PointerType *CharPtrTy, *VoidPtrTy, *VoidPtrPtrTy;

  /// Convenience reference to LLVM Context
  llvm::LLVMContext &Context;
  /// Convenience reference to the current module
  llvm::Module &TheModule;
  /// Keeps track of kernel launch stubs and handles emitted in this module
  struct KernelInfo {
    llvm::Function *Kernel; // stub function to help launch kernel
    const Decl *D;
  };
  llvm::SmallVector<KernelInfo, 16> EmittedKernels;
  // Map a device stub function to a symbol for identifying kernel in host code.
  // For CUDA, the symbol for identifying the kernel is the same as the device
  // stub function. For HIP, they are different.
  llvm::DenseMap<llvm::Function *, llvm::GlobalValue *> KernelHandles;
  // Map a kernel handle to the kernel stub.
  llvm::DenseMap<llvm::GlobalValue *, llvm::Function *> KernelStubs;
  struct VarInfo {
    llvm::GlobalVariable *Var;
    const VarDecl *D;
    DeviceVarFlags Flags;
  };
  llvm::SmallVector<VarInfo, 16> DeviceVars;
  /// Keeps track of variable containing handle of GPU binary. Populated by
  /// ModuleCtorFunction() and used to create corresponding cleanup calls in
  /// ModuleDtorFunction()
  llvm::GlobalVariable *GpuBinaryHandle = nullptr;
  /// Whether we generate relocatable device code.
  bool RelocatableDeviceCode;
  /// Mangle context for device.
  std::unique_ptr<MangleContext> DeviceMC;

  llvm::FunctionCallee getSetupArgumentFn() const;
  llvm::FunctionCallee getLaunchFn() const;

  llvm::FunctionType *getRegisterGlobalsFnTy() const;
  llvm::FunctionType *getCallbackFnTy() const;
  llvm::FunctionType *getRegisterLinkedBinaryFnTy() const;
  std::string addPrefixToName(StringRef FuncName) const;
  std::string addUnderscoredPrefixToName(StringRef FuncName) const;

  /// Creates a function to register all kernel stubs generated in this module.
  llvm::Function *makeRegisterGlobalsFn();

  /// Helper function that generates a constant string and returns a pointer to
  /// the start of the string.  The result of this function can be used anywhere
  /// where the C code specifies const char*.
  llvm::Constant *makeConstantString(const std::string &Str,
                                     const std::string &Name = "",
                                     const std::string &SectionName = "",
                                     unsigned Alignment = 0) {
    llvm::Constant *Zeros[] = {llvm::ConstantInt::get(SizeTy, 0),
                               llvm::ConstantInt::get(SizeTy, 0)};
    auto ConstStr = CGM.GetAddrOfConstantCString(Str, Name.c_str());
    llvm::GlobalVariable *GV =
        cast<llvm::GlobalVariable>(ConstStr.getPointer());
    if (!SectionName.empty()) {
      GV->setSection(SectionName);
      // Mark the address as used which make sure that this section isn't
      // merged and we will really have it in the object file.
      GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    }
    if (Alignment)
      GV->setAlignment(llvm::Align(Alignment));

    return llvm::ConstantExpr::getGetElementPtr(ConstStr.getElementType(),
                                                ConstStr.getPointer(), Zeros);
  }

  /// Helper function that generates an empty dummy function returning void.
  llvm::Function *makeDummyFunction(llvm::FunctionType *FnTy) {
    assert(FnTy->getReturnType()->isVoidTy() &&
           "Can only generate dummy functions returning void!");
    llvm::Function *DummyFunc = llvm::Function::Create(
        FnTy, llvm::GlobalValue::InternalLinkage, "dummy", &TheModule);

    llvm::BasicBlock *DummyBlock =
        llvm::BasicBlock::Create(Context, "", DummyFunc);
    CGBuilderTy FuncBuilder(CGM, Context);
    FuncBuilder.SetInsertPoint(DummyBlock);
    FuncBuilder.CreateRetVoid();

    return DummyFunc;
  }

  void emitDeviceStubBodyLegacy(CodeGenFunction &CGF, FunctionArgList &Args);
  void emitDeviceStubBodyNew(CodeGenFunction &CGF, FunctionArgList &Args);
  std::string getDeviceSideName(const NamedDecl *ND) override;

  void registerDeviceVar(const VarDecl *VD, llvm::GlobalVariable &Var,
                         bool Extern, bool Constant) {
    DeviceVars.push_back({&Var,
                          VD,
                          {DeviceVarFlags::Variable, Extern, Constant,
                           VD->hasAttr<HIPManagedAttr>(),
                           /*Normalized*/ false, 0}});
  }
  void registerDeviceSurf(const VarDecl *VD, llvm::GlobalVariable &Var,
                          bool Extern, int Type) {
    DeviceVars.push_back({&Var,
                          VD,
                          {DeviceVarFlags::Surface, Extern, /*Constant*/ false,
                           /*Managed*/ false,
                           /*Normalized*/ false, Type}});
  }
  void registerDeviceTex(const VarDecl *VD, llvm::GlobalVariable &Var,
                         bool Extern, int Type, bool Normalized) {
    DeviceVars.push_back({&Var,
                          VD,
                          {DeviceVarFlags::Texture, Extern, /*Constant*/ false,
                           /*Managed*/ false, Normalized, Type}});
  }

  /// Creates module constructor function
  llvm::Function *makeModuleCtorFunction();
  /// Creates module destructor function
  llvm::Function *makeModuleDtorFunction();
  /// Transform managed variables for device compilation.
  void transformManagedVars();

public:
  CGNVCUDARuntime(CodeGenModule &CGM);

  llvm::GlobalValue *getKernelHandle(llvm::Function *F, GlobalDecl GD) override;
  llvm::Function *getKernelStub(llvm::GlobalValue *Handle) override {
    auto Loc = KernelStubs.find(Handle);
    assert(Loc != KernelStubs.end());
    return Loc->second;
  }
  void emitDeviceStub(CodeGenFunction &CGF, FunctionArgList &Args) override;
  void handleVarRegistration(const VarDecl *VD,
                             llvm::GlobalVariable &Var) override;
  void
  internalizeDeviceSideVar(const VarDecl *D,
                           llvm::GlobalValue::LinkageTypes &Linkage) override;

  llvm::Function *finalizeModule() override;
};

}

std::string CGNVCUDARuntime::addPrefixToName(StringRef FuncName) const {
  if (CGM.getLangOpts().HIP)
    return ((Twine("hip") + Twine(FuncName)).str());
  return ((Twine("cuda") + Twine(FuncName)).str());
}
std::string
CGNVCUDARuntime::addUnderscoredPrefixToName(StringRef FuncName) const {
  if (CGM.getLangOpts().HIP)
    return ((Twine("__hip") + Twine(FuncName)).str());
  return ((Twine("__cuda") + Twine(FuncName)).str());
}

CGNVCUDARuntime::CGNVCUDARuntime(CodeGenModule &CGM)
    : CGCUDARuntime(CGM), Context(CGM.getLLVMContext()),
      TheModule(CGM.getModule()),
      RelocatableDeviceCode(CGM.getLangOpts().GPURelocatableDeviceCode),
      DeviceMC(CGM.getContext().createMangleContext(
          CGM.getContext().getAuxTargetInfo())) {
  CodeGen::CodeGenTypes &Types = CGM.getTypes();
  ASTContext &Ctx = CGM.getContext();

  IntTy = CGM.IntTy;
  SizeTy = CGM.SizeTy;
  VoidTy = CGM.VoidTy;

  CharPtrTy = llvm::PointerType::getUnqual(Types.ConvertType(Ctx.CharTy));
  VoidPtrTy = cast<llvm::PointerType>(Types.ConvertType(Ctx.VoidPtrTy));
  VoidPtrPtrTy = VoidPtrTy->getPointerTo();
  if (CGM.getContext().getAuxTargetInfo()) {
    // If the host and device have different C++ ABIs, mark it as the device
    // mangle context so that the mangling needs to retrieve the additonal
    // device lambda mangling number instead of the regular host one.
    DeviceMC->setDeviceMangleContext(
        CGM.getContext().getTargetInfo().getCXXABI().isMicrosoft() &&
        CGM.getContext().getAuxTargetInfo()->getCXXABI().isItaniumFamily());
  }
}

llvm::FunctionCallee CGNVCUDARuntime::getSetupArgumentFn() const {
  // cudaError_t cudaSetupArgument(void *, size_t, size_t)
  llvm::Type *Params[] = {VoidPtrTy, SizeTy, SizeTy};
  return CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(IntTy, Params, false),
      addPrefixToName("SetupArgument"));
}

llvm::FunctionCallee CGNVCUDARuntime::getLaunchFn() const {
  if (CGM.getLangOpts().HIP) {
    // hipError_t hipLaunchByPtr(char *);
    return CGM.CreateRuntimeFunction(
        llvm::FunctionType::get(IntTy, CharPtrTy, false), "hipLaunchByPtr");
  } else {
    // cudaError_t cudaLaunch(char *);
    return CGM.CreateRuntimeFunction(
        llvm::FunctionType::get(IntTy, CharPtrTy, false), "cudaLaunch");
  }
}

llvm::FunctionType *CGNVCUDARuntime::getRegisterGlobalsFnTy() const {
  return llvm::FunctionType::get(VoidTy, VoidPtrPtrTy, false);
}

llvm::FunctionType *CGNVCUDARuntime::getCallbackFnTy() const {
  return llvm::FunctionType::get(VoidTy, VoidPtrTy, false);
}

llvm::FunctionType *CGNVCUDARuntime::getRegisterLinkedBinaryFnTy() const {
  auto CallbackFnTy = getCallbackFnTy();
  auto RegisterGlobalsFnTy = getRegisterGlobalsFnTy();
  llvm::Type *Params[] = {RegisterGlobalsFnTy->getPointerTo(), VoidPtrTy,
                          VoidPtrTy, CallbackFnTy->getPointerTo()};
  return llvm::FunctionType::get(VoidTy, Params, false);
}

std::string CGNVCUDARuntime::getDeviceSideName(const NamedDecl *ND) {
  GlobalDecl GD;
  // D could be either a kernel or a variable.
  if (auto *FD = dyn_cast<FunctionDecl>(ND))
    GD = GlobalDecl(FD, KernelReferenceKind::Kernel);
  else
    GD = GlobalDecl(ND);
  std::string DeviceSideName;
  MangleContext *MC;
  if (CGM.getLangOpts().CUDAIsDevice)
    MC = &CGM.getCXXABI().getMangleContext();
  else
    MC = DeviceMC.get();
  if (MC->shouldMangleDeclName(ND)) {
    SmallString<256> Buffer;
    llvm::raw_svector_ostream Out(Buffer);
    MC->mangleName(GD, Out);
    DeviceSideName = std::string(Out.str());
  } else
    DeviceSideName = std::string(ND->getIdentifier()->getName());

  // Make unique name for device side static file-scope variable for HIP.
  if (CGM.getContext().shouldExternalizeStaticVar(ND) &&
      CGM.getLangOpts().GPURelocatableDeviceCode &&
      !CGM.getLangOpts().CUID.empty()) {
    SmallString<256> Buffer;
    llvm::raw_svector_ostream Out(Buffer);
    Out << DeviceSideName;
    CGM.printPostfixForExternalizedStaticVar(Out);
    DeviceSideName = std::string(Out.str());
  }
  return DeviceSideName;
}

void CGNVCUDARuntime::emitDeviceStub(CodeGenFunction &CGF,
                                     FunctionArgList &Args) {
  EmittedKernels.push_back({CGF.CurFn, CGF.CurFuncDecl});
  if (auto *GV = dyn_cast<llvm::GlobalVariable>(KernelHandles[CGF.CurFn])) {
    GV->setLinkage(CGF.CurFn->getLinkage());
    GV->setInitializer(CGF.CurFn);
  }
  if (CudaFeatureEnabled(CGM.getTarget().getSDKVersion(),
                         CudaFeature::CUDA_USES_NEW_LAUNCH) ||
      (CGF.getLangOpts().HIP && CGF.getLangOpts().HIPUseNewLaunchAPI))
    emitDeviceStubBodyNew(CGF, Args);
  else
    emitDeviceStubBodyLegacy(CGF, Args);
}

// CUDA 9.0+ uses new way to launch kernels. Parameters are packed in a local
// array and kernels are launched using cudaLaunchKernel().
void CGNVCUDARuntime::emitDeviceStubBodyNew(CodeGenFunction &CGF,
                                            FunctionArgList &Args) {
  // Build the shadow stack entry at the very start of the function.

  // Calculate amount of space we will need for all arguments.  If we have no
  // args, allocate a single pointer so we still have a valid pointer to the
  // argument array that we can pass to runtime, even if it will be unused.
  Address KernelArgs = CGF.CreateTempAlloca(
      VoidPtrTy, CharUnits::fromQuantity(16), "kernel_args",
      llvm::ConstantInt::get(SizeTy, std::max<size_t>(1, Args.size())));
  // Store pointers to the arguments in a locally allocated launch_args.
  for (unsigned i = 0; i < Args.size(); ++i) {
    llvm::Value* VarPtr = CGF.GetAddrOfLocalVar(Args[i]).getPointer();
    llvm::Value *VoidVarPtr = CGF.Builder.CreatePointerCast(VarPtr, VoidPtrTy);
    CGF.Builder.CreateDefaultAlignedStore(
        VoidVarPtr, CGF.Builder.CreateConstGEP1_32(KernelArgs.getPointer(), i));
  }

  llvm::BasicBlock *EndBlock = CGF.createBasicBlock("setup.end");

  // Lookup cudaLaunchKernel/hipLaunchKernel function.
  // cudaError_t cudaLaunchKernel(const void *func, dim3 gridDim, dim3 blockDim,
  //                              void **args, size_t sharedMem,
  //                              cudaStream_t stream);
  // hipError_t hipLaunchKernel(const void *func, dim3 gridDim, dim3 blockDim,
  //                            void **args, size_t sharedMem,
  //                            hipStream_t stream);
  TranslationUnitDecl *TUDecl = CGM.getContext().getTranslationUnitDecl();
  DeclContext *DC = TranslationUnitDecl::castToDeclContext(TUDecl);
  auto LaunchKernelName = addPrefixToName("LaunchKernel");
  IdentifierInfo &cudaLaunchKernelII =
      CGM.getContext().Idents.get(LaunchKernelName);
  FunctionDecl *cudaLaunchKernelFD = nullptr;
  for (const auto &Result : DC->lookup(&cudaLaunchKernelII)) {
    if (FunctionDecl *FD = dyn_cast<FunctionDecl>(Result))
      cudaLaunchKernelFD = FD;
  }

  if (cudaLaunchKernelFD == nullptr) {
    CGM.Error(CGF.CurFuncDecl->getLocation(),
              "Can't find declaration for " + LaunchKernelName);
    return;
  }
  // Create temporary dim3 grid_dim, block_dim.
  ParmVarDecl *GridDimParam = cudaLaunchKernelFD->getParamDecl(1);
  QualType Dim3Ty = GridDimParam->getType();
  Address GridDim =
      CGF.CreateMemTemp(Dim3Ty, CharUnits::fromQuantity(8), "grid_dim");
  Address BlockDim =
      CGF.CreateMemTemp(Dim3Ty, CharUnits::fromQuantity(8), "block_dim");
  Address ShmemSize =
      CGF.CreateTempAlloca(SizeTy, CGM.getSizeAlign(), "shmem_size");
  Address Stream =
      CGF.CreateTempAlloca(VoidPtrTy, CGM.getPointerAlign(), "stream");
  llvm::FunctionCallee cudaPopConfigFn = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(IntTy,
                              {/*gridDim=*/GridDim.getType(),
                               /*blockDim=*/BlockDim.getType(),
                               /*ShmemSize=*/ShmemSize.getType(),
                               /*Stream=*/Stream.getType()},
                              /*isVarArg=*/false),
      addUnderscoredPrefixToName("PopCallConfiguration"));

  CGF.EmitRuntimeCallOrInvoke(cudaPopConfigFn,
                              {GridDim.getPointer(), BlockDim.getPointer(),
                               ShmemSize.getPointer(), Stream.getPointer()});

  // Emit the call to cudaLaunch
  llvm::Value *Kernel =
      CGF.Builder.CreatePointerCast(KernelHandles[CGF.CurFn], VoidPtrTy);
  CallArgList LaunchKernelArgs;
  LaunchKernelArgs.add(RValue::get(Kernel),
                       cudaLaunchKernelFD->getParamDecl(0)->getType());
  LaunchKernelArgs.add(RValue::getAggregate(GridDim), Dim3Ty);
  LaunchKernelArgs.add(RValue::getAggregate(BlockDim), Dim3Ty);
  LaunchKernelArgs.add(RValue::get(KernelArgs.getPointer()),
                       cudaLaunchKernelFD->getParamDecl(3)->getType());
  LaunchKernelArgs.add(RValue::get(CGF.Builder.CreateLoad(ShmemSize)),
                       cudaLaunchKernelFD->getParamDecl(4)->getType());
  LaunchKernelArgs.add(RValue::get(CGF.Builder.CreateLoad(Stream)),
                       cudaLaunchKernelFD->getParamDecl(5)->getType());

  QualType QT = cudaLaunchKernelFD->getType();
  QualType CQT = QT.getCanonicalType();
  llvm::Type *Ty = CGM.getTypes().ConvertType(CQT);
  llvm::FunctionType *FTy = dyn_cast<llvm::FunctionType>(Ty);

  const CGFunctionInfo &FI =
      CGM.getTypes().arrangeFunctionDeclaration(cudaLaunchKernelFD);
  llvm::FunctionCallee cudaLaunchKernelFn =
      CGM.CreateRuntimeFunction(FTy, LaunchKernelName);
  CGF.EmitCall(FI, CGCallee::forDirect(cudaLaunchKernelFn), ReturnValueSlot(),
               LaunchKernelArgs);
  CGF.EmitBranch(EndBlock);

  CGF.EmitBlock(EndBlock);
}

void CGNVCUDARuntime::emitDeviceStubBodyLegacy(CodeGenFunction &CGF,
                                               FunctionArgList &Args) {
  // Emit a call to cudaSetupArgument for each arg in Args.
  llvm::FunctionCallee cudaSetupArgFn = getSetupArgumentFn();
  llvm::BasicBlock *EndBlock = CGF.createBasicBlock("setup.end");
  CharUnits Offset = CharUnits::Zero();
  for (const VarDecl *A : Args) {
    auto TInfo = CGM.getContext().getTypeInfoInChars(A->getType());
    Offset = Offset.alignTo(TInfo.Align);
    llvm::Value *Args[] = {
        CGF.Builder.CreatePointerCast(CGF.GetAddrOfLocalVar(A).getPointer(),
                                      VoidPtrTy),
        llvm::ConstantInt::get(SizeTy, TInfo.Width.getQuantity()),
        llvm::ConstantInt::get(SizeTy, Offset.getQuantity()),
    };
    llvm::CallBase *CB = CGF.EmitRuntimeCallOrInvoke(cudaSetupArgFn, Args);
    llvm::Constant *Zero = llvm::ConstantInt::get(IntTy, 0);
    llvm::Value *CBZero = CGF.Builder.CreateICmpEQ(CB, Zero);
    llvm::BasicBlock *NextBlock = CGF.createBasicBlock("setup.next");
    CGF.Builder.CreateCondBr(CBZero, NextBlock, EndBlock);
    CGF.EmitBlock(NextBlock);
    Offset += TInfo.Width;
  }

  // Emit the call to cudaLaunch
  llvm::FunctionCallee cudaLaunchFn = getLaunchFn();
  llvm::Value *Arg =
      CGF.Builder.CreatePointerCast(KernelHandles[CGF.CurFn], CharPtrTy);
  CGF.EmitRuntimeCallOrInvoke(cudaLaunchFn, Arg);
  CGF.EmitBranch(EndBlock);

  CGF.EmitBlock(EndBlock);
}

// Replace the original variable Var with the address loaded from variable
// ManagedVar populated by HIP runtime.
static void replaceManagedVar(llvm::GlobalVariable *Var,
                              llvm::GlobalVariable *ManagedVar) {
  SmallVector<SmallVector<llvm::User *, 8>, 8> WorkList;
  for (auto &&VarUse : Var->uses()) {
    WorkList.push_back({VarUse.getUser()});
  }
  while (!WorkList.empty()) {
    auto &&WorkItem = WorkList.pop_back_val();
    auto *U = WorkItem.back();
    if (isa<llvm::ConstantExpr>(U)) {
      for (auto &&UU : U->uses()) {
        WorkItem.push_back(UU.getUser());
        WorkList.push_back(WorkItem);
        WorkItem.pop_back();
      }
      continue;
    }
    if (auto *I = dyn_cast<llvm::Instruction>(U)) {
      llvm::Value *OldV = Var;
      llvm::Instruction *NewV =
          new llvm::LoadInst(Var->getType(), ManagedVar, "ld.managed", false,
                             llvm::Align(Var->getAlignment()), I);
      WorkItem.pop_back();
      // Replace constant expressions directly or indirectly using the managed
      // variable with instructions.
      for (auto &&Op : WorkItem) {
        auto *CE = cast<llvm::ConstantExpr>(Op);
        auto *NewInst = llvm::createReplacementInstr(CE, I);
        NewInst->replaceUsesOfWith(OldV, NewV);
        OldV = CE;
        NewV = NewInst;
      }
      I->replaceUsesOfWith(OldV, NewV);
    } else {
      llvm_unreachable("Invalid use of managed variable");
    }
  }
}

/// Creates a function that sets up state on the host side for CUDA objects that
/// have a presence on both the host and device sides. Specifically, registers
/// the host side of kernel functions and device global variables with the CUDA
/// runtime.
/// \code
/// void __cuda_register_globals(void** GpuBinaryHandle) {
///    __cudaRegisterFunction(GpuBinaryHandle,Kernel0,...);
///    ...
///    __cudaRegisterFunction(GpuBinaryHandle,KernelM,...);
///    __cudaRegisterVar(GpuBinaryHandle, GlobalVar0, ...);
///    ...
///    __cudaRegisterVar(GpuBinaryHandle, GlobalVarN, ...);
/// }
/// \endcode
llvm::Function *CGNVCUDARuntime::makeRegisterGlobalsFn() {
  // No need to register anything
  if (EmittedKernels.empty() && DeviceVars.empty())
    return nullptr;

  llvm::Function *RegisterKernelsFunc = llvm::Function::Create(
      getRegisterGlobalsFnTy(), llvm::GlobalValue::InternalLinkage,
      addUnderscoredPrefixToName("_register_globals"), &TheModule);
  llvm::BasicBlock *EntryBB =
      llvm::BasicBlock::Create(Context, "entry", RegisterKernelsFunc);
  CGBuilderTy Builder(CGM, Context);
  Builder.SetInsertPoint(EntryBB);

  // void __cudaRegisterFunction(void **, const char *, char *, const char *,
  //                             int, uint3*, uint3*, dim3*, dim3*, int*)
  llvm::Type *RegisterFuncParams[] = {
      VoidPtrPtrTy, CharPtrTy, CharPtrTy, CharPtrTy, IntTy,
      VoidPtrTy,    VoidPtrTy, VoidPtrTy, VoidPtrTy, IntTy->getPointerTo()};
  llvm::FunctionCallee RegisterFunc = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(IntTy, RegisterFuncParams, false),
      addUnderscoredPrefixToName("RegisterFunction"));

  // Extract GpuBinaryHandle passed as the first argument passed to
  // __cuda_register_globals() and generate __cudaRegisterFunction() call for
  // each emitted kernel.
  llvm::Argument &GpuBinaryHandlePtr = *RegisterKernelsFunc->arg_begin();
  for (auto &&I : EmittedKernels) {
    llvm::Constant *KernelName =
        makeConstantString(getDeviceSideName(cast<NamedDecl>(I.D)));
    llvm::Constant *NullPtr = llvm::ConstantPointerNull::get(VoidPtrTy);
    llvm::Value *Args[] = {
        &GpuBinaryHandlePtr,
        Builder.CreateBitCast(KernelHandles[I.Kernel], VoidPtrTy),
        KernelName,
        KernelName,
        llvm::ConstantInt::get(IntTy, -1),
        NullPtr,
        NullPtr,
        NullPtr,
        NullPtr,
        llvm::ConstantPointerNull::get(IntTy->getPointerTo())};
    Builder.CreateCall(RegisterFunc, Args);
  }

  llvm::Type *VarSizeTy = IntTy;
  // For HIP or CUDA 9.0+, device variable size is type of `size_t`.
  if (CGM.getLangOpts().HIP ||
      ToCudaVersion(CGM.getTarget().getSDKVersion()) >= CudaVersion::CUDA_90)
    VarSizeTy = SizeTy;

  // void __cudaRegisterVar(void **, char *, char *, const char *,
  //                        int, int, int, int)
  llvm::Type *RegisterVarParams[] = {VoidPtrPtrTy, CharPtrTy, CharPtrTy,
                                     CharPtrTy,    IntTy,     VarSizeTy,
                                     IntTy,        IntTy};
  llvm::FunctionCallee RegisterVar = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(VoidTy, RegisterVarParams, false),
      addUnderscoredPrefixToName("RegisterVar"));
  // void __hipRegisterManagedVar(void **, char *, char *, const char *,
  //                              size_t, unsigned)
  llvm::Type *RegisterManagedVarParams[] = {VoidPtrPtrTy, CharPtrTy, CharPtrTy,
                                            CharPtrTy,    VarSizeTy, IntTy};
  llvm::FunctionCallee RegisterManagedVar = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(VoidTy, RegisterManagedVarParams, false),
      addUnderscoredPrefixToName("RegisterManagedVar"));
  // void __cudaRegisterSurface(void **, const struct surfaceReference *,
  //                            const void **, const char *, int, int);
  llvm::FunctionCallee RegisterSurf = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(
          VoidTy, {VoidPtrPtrTy, VoidPtrTy, CharPtrTy, CharPtrTy, IntTy, IntTy},
          false),
      addUnderscoredPrefixToName("RegisterSurface"));
  // void __cudaRegisterTexture(void **, const struct textureReference *,
  //                            const void **, const char *, int, int, int)
  llvm::FunctionCallee RegisterTex = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(
          VoidTy,
          {VoidPtrPtrTy, VoidPtrTy, CharPtrTy, CharPtrTy, IntTy, IntTy, IntTy},
          false),
      addUnderscoredPrefixToName("RegisterTexture"));
  for (auto &&Info : DeviceVars) {
    llvm::GlobalVariable *Var = Info.Var;
    assert((!Var->isDeclaration() || Info.Flags.isManaged()) &&
           "External variables should not show up here, except HIP managed "
           "variables");
    llvm::Constant *VarName = makeConstantString(getDeviceSideName(Info.D));
    switch (Info.Flags.getKind()) {
    case DeviceVarFlags::Variable: {
      uint64_t VarSize =
          CGM.getDataLayout().getTypeAllocSize(Var->getValueType());
      if (Info.Flags.isManaged()) {
        auto ManagedVar = new llvm::GlobalVariable(
            CGM.getModule(), Var->getType(),
            /*isConstant=*/false, Var->getLinkage(),
            /*Init=*/Var->isDeclaration()
                ? nullptr
                : llvm::ConstantPointerNull::get(Var->getType()),
            /*Name=*/"", /*InsertBefore=*/nullptr,
            llvm::GlobalVariable::NotThreadLocal);
        ManagedVar->setDSOLocal(Var->isDSOLocal());
        ManagedVar->setVisibility(Var->getVisibility());
        ManagedVar->setExternallyInitialized(true);
        ManagedVar->takeName(Var);
        Var->setName(Twine(ManagedVar->getName() + ".managed"));
        replaceManagedVar(Var, ManagedVar);
        llvm::Value *Args[] = {
            &GpuBinaryHandlePtr,
            Builder.CreateBitCast(ManagedVar, VoidPtrTy),
            Builder.CreateBitCast(Var, VoidPtrTy),
            VarName,
            llvm::ConstantInt::get(VarSizeTy, VarSize),
            llvm::ConstantInt::get(IntTy, Var->getAlignment())};
        if (!Var->isDeclaration())
          Builder.CreateCall(RegisterManagedVar, Args);
      } else {
        llvm::Value *Args[] = {
            &GpuBinaryHandlePtr,
            Builder.CreateBitCast(Var, VoidPtrTy),
            VarName,
            VarName,
            llvm::ConstantInt::get(IntTy, Info.Flags.isExtern()),
            llvm::ConstantInt::get(VarSizeTy, VarSize),
            llvm::ConstantInt::get(IntTy, Info.Flags.isConstant()),
            llvm::ConstantInt::get(IntTy, 0)};
        Builder.CreateCall(RegisterVar, Args);
      }
      break;
    }
    case DeviceVarFlags::Surface:
      Builder.CreateCall(
          RegisterSurf,
          {&GpuBinaryHandlePtr, Builder.CreateBitCast(Var, VoidPtrTy), VarName,
           VarName, llvm::ConstantInt::get(IntTy, Info.Flags.getSurfTexType()),
           llvm::ConstantInt::get(IntTy, Info.Flags.isExtern())});
      break;
    case DeviceVarFlags::Texture:
      Builder.CreateCall(
          RegisterTex,
          {&GpuBinaryHandlePtr, Builder.CreateBitCast(Var, VoidPtrTy), VarName,
           VarName, llvm::ConstantInt::get(IntTy, Info.Flags.getSurfTexType()),
           llvm::ConstantInt::get(IntTy, Info.Flags.isNormalized()),
           llvm::ConstantInt::get(IntTy, Info.Flags.isExtern())});
      break;
    }
  }

  Builder.CreateRetVoid();
  return RegisterKernelsFunc;
}

/// Creates a global constructor function for the module:
///
/// For CUDA:
/// \code
/// void __cuda_module_ctor(void*) {
///     Handle = __cudaRegisterFatBinary(GpuBinaryBlob);
///     __cuda_register_globals(Handle);
/// }
/// \endcode
///
/// For HIP:
/// \code
/// void __hip_module_ctor(void*) {
///     if (__hip_gpubin_handle == 0) {
///         __hip_gpubin_handle  = __hipRegisterFatBinary(GpuBinaryBlob);
///         __hip_register_globals(__hip_gpubin_handle);
///     }
/// }
/// \endcode
llvm::Function *CGNVCUDARuntime::makeModuleCtorFunction() {
  bool IsHIP = CGM.getLangOpts().HIP;
  bool IsCUDA = CGM.getLangOpts().CUDA;
  // No need to generate ctors/dtors if there is no GPU binary.
  StringRef CudaGpuBinaryFileName = CGM.getCodeGenOpts().CudaGpuBinaryFileName;
  if (CudaGpuBinaryFileName.empty() && !IsHIP)
    return nullptr;
  if ((IsHIP || (IsCUDA && !RelocatableDeviceCode)) && EmittedKernels.empty() &&
      DeviceVars.empty())
    return nullptr;

  // void __{cuda|hip}_register_globals(void* handle);
  llvm::Function *RegisterGlobalsFunc = makeRegisterGlobalsFn();
  // We always need a function to pass in as callback. Create a dummy
  // implementation if we don't need to register anything.
  if (RelocatableDeviceCode && !RegisterGlobalsFunc)
    RegisterGlobalsFunc = makeDummyFunction(getRegisterGlobalsFnTy());

  // void ** __{cuda|hip}RegisterFatBinary(void *);
  llvm::FunctionCallee RegisterFatbinFunc = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(VoidPtrPtrTy, VoidPtrTy, false),
      addUnderscoredPrefixToName("RegisterFatBinary"));
  // struct { int magic, int version, void * gpu_binary, void * dont_care };
  llvm::StructType *FatbinWrapperTy =
      llvm::StructType::get(IntTy, IntTy, VoidPtrTy, VoidPtrTy);

  // Register GPU binary with the CUDA runtime, store returned handle in a
  // global variable and save a reference in GpuBinaryHandle to be cleaned up
  // in destructor on exit. Then associate all known kernels with the GPU binary
  // handle so CUDA runtime can figure out what to call on the GPU side.
  std::unique_ptr<llvm::MemoryBuffer> CudaGpuBinary = nullptr;
  if (!CudaGpuBinaryFileName.empty()) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> CudaGpuBinaryOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(CudaGpuBinaryFileName);
    if (std::error_code EC = CudaGpuBinaryOrErr.getError()) {
      CGM.getDiags().Report(diag::err_cannot_open_file)
          << CudaGpuBinaryFileName << EC.message();
      return nullptr;
    }
    CudaGpuBinary = std::move(CudaGpuBinaryOrErr.get());
  }

  llvm::Function *ModuleCtorFunc = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, VoidPtrTy, false),
      llvm::GlobalValue::InternalLinkage,
      addUnderscoredPrefixToName("_module_ctor"), &TheModule);
  llvm::BasicBlock *CtorEntryBB =
      llvm::BasicBlock::Create(Context, "entry", ModuleCtorFunc);
  CGBuilderTy CtorBuilder(CGM, Context);

  CtorBuilder.SetInsertPoint(CtorEntryBB);

  const char *FatbinConstantName;
  const char *FatbinSectionName;
  const char *ModuleIDSectionName;
  StringRef ModuleIDPrefix;
  llvm::Constant *FatBinStr;
  unsigned FatMagic;
  if (IsHIP) {
    FatbinConstantName = ".hip_fatbin";
    FatbinSectionName = ".hipFatBinSegment";

    ModuleIDSectionName = "__hip_module_id";
    ModuleIDPrefix = "__hip_";

    if (CudaGpuBinary) {
      // If fatbin is available from early finalization, create a string
      // literal containing the fat binary loaded from the given file.
      const unsigned HIPCodeObjectAlign = 4096;
      FatBinStr =
          makeConstantString(std::string(CudaGpuBinary->getBuffer()), "",
                             FatbinConstantName, HIPCodeObjectAlign);
    } else {
      // If fatbin is not available, create an external symbol
      // __hip_fatbin in section .hip_fatbin. The external symbol is supposed
      // to contain the fat binary but will be populated somewhere else,
      // e.g. by lld through link script.
      FatBinStr = new llvm::GlobalVariable(
        CGM.getModule(), CGM.Int8Ty,
        /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage, nullptr,
        "__hip_fatbin", nullptr,
        llvm::GlobalVariable::NotThreadLocal);
      cast<llvm::GlobalVariable>(FatBinStr)->setSection(FatbinConstantName);
    }

    FatMagic = HIPFatMagic;
  } else {
    if (RelocatableDeviceCode)
      FatbinConstantName = CGM.getTriple().isMacOSX()
                               ? "__NV_CUDA,__nv_relfatbin"
                               : "__nv_relfatbin";
    else
      FatbinConstantName =
          CGM.getTriple().isMacOSX() ? "__NV_CUDA,__nv_fatbin" : ".nv_fatbin";
    // NVIDIA's cuobjdump looks for fatbins in this section.
    FatbinSectionName =
        CGM.getTriple().isMacOSX() ? "__NV_CUDA,__fatbin" : ".nvFatBinSegment";

    ModuleIDSectionName = CGM.getTriple().isMacOSX()
                              ? "__NV_CUDA,__nv_module_id"
                              : "__nv_module_id";
    ModuleIDPrefix = "__nv_";

    // For CUDA, create a string literal containing the fat binary loaded from
    // the given file.
    FatBinStr = makeConstantString(std::string(CudaGpuBinary->getBuffer()), "",
                                   FatbinConstantName, 8);
    FatMagic = CudaFatMagic;
  }

  // Create initialized wrapper structure that points to the loaded GPU binary
  ConstantInitBuilder Builder(CGM);
  auto Values = Builder.beginStruct(FatbinWrapperTy);
  // Fatbin wrapper magic.
  Values.addInt(IntTy, FatMagic);
  // Fatbin version.
  Values.addInt(IntTy, 1);
  // Data.
  Values.add(FatBinStr);
  // Unused in fatbin v1.
  Values.add(llvm::ConstantPointerNull::get(VoidPtrTy));
  llvm::GlobalVariable *FatbinWrapper = Values.finishAndCreateGlobal(
      addUnderscoredPrefixToName("_fatbin_wrapper"), CGM.getPointerAlign(),
      /*constant*/ true);
  FatbinWrapper->setSection(FatbinSectionName);

  // There is only one HIP fat binary per linked module, however there are
  // multiple constructor functions. Make sure the fat binary is registered
  // only once. The constructor functions are executed by the dynamic loader
  // before the program gains control. The dynamic loader cannot execute the
  // constructor functions concurrently since doing that would not guarantee
  // thread safety of the loaded program. Therefore we can assume sequential
  // execution of constructor functions here.
  if (IsHIP) {
    auto Linkage = CudaGpuBinary ? llvm::GlobalValue::InternalLinkage :
        llvm::GlobalValue::LinkOnceAnyLinkage;
    llvm::BasicBlock *IfBlock =
        llvm::BasicBlock::Create(Context, "if", ModuleCtorFunc);
    llvm::BasicBlock *ExitBlock =
        llvm::BasicBlock::Create(Context, "exit", ModuleCtorFunc);
    // The name, size, and initialization pattern of this variable is part
    // of HIP ABI.
    GpuBinaryHandle = new llvm::GlobalVariable(
        TheModule, VoidPtrPtrTy, /*isConstant=*/false,
        Linkage,
        /*Initializer=*/llvm::ConstantPointerNull::get(VoidPtrPtrTy),
        "__hip_gpubin_handle");
    GpuBinaryHandle->setAlignment(CGM.getPointerAlign().getAsAlign());
    // Prevent the weak symbol in different shared libraries being merged.
    if (Linkage != llvm::GlobalValue::InternalLinkage)
      GpuBinaryHandle->setVisibility(llvm::GlobalValue::HiddenVisibility);
    Address GpuBinaryAddr(
        GpuBinaryHandle,
        CharUnits::fromQuantity(GpuBinaryHandle->getAlignment()));
    {
      auto HandleValue = CtorBuilder.CreateLoad(GpuBinaryAddr);
      llvm::Constant *Zero =
          llvm::Constant::getNullValue(HandleValue->getType());
      llvm::Value *EQZero = CtorBuilder.CreateICmpEQ(HandleValue, Zero);
      CtorBuilder.CreateCondBr(EQZero, IfBlock, ExitBlock);
    }
    {
      CtorBuilder.SetInsertPoint(IfBlock);
      // GpuBinaryHandle = __hipRegisterFatBinary(&FatbinWrapper);
      llvm::CallInst *RegisterFatbinCall = CtorBuilder.CreateCall(
          RegisterFatbinFunc,
          CtorBuilder.CreateBitCast(FatbinWrapper, VoidPtrTy));
      CtorBuilder.CreateStore(RegisterFatbinCall, GpuBinaryAddr);
      CtorBuilder.CreateBr(ExitBlock);
    }
    {
      CtorBuilder.SetInsertPoint(ExitBlock);
      // Call __hip_register_globals(GpuBinaryHandle);
      if (RegisterGlobalsFunc) {
        auto HandleValue = CtorBuilder.CreateLoad(GpuBinaryAddr);
        CtorBuilder.CreateCall(RegisterGlobalsFunc, HandleValue);
      }
    }
  } else if (!RelocatableDeviceCode) {
    // Register binary with CUDA runtime. This is substantially different in
    // default mode vs. separate compilation!
    // GpuBinaryHandle = __cudaRegisterFatBinary(&FatbinWrapper);
    llvm::CallInst *RegisterFatbinCall = CtorBuilder.CreateCall(
        RegisterFatbinFunc,
        CtorBuilder.CreateBitCast(FatbinWrapper, VoidPtrTy));
    GpuBinaryHandle = new llvm::GlobalVariable(
        TheModule, VoidPtrPtrTy, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantPointerNull::get(VoidPtrPtrTy), "__cuda_gpubin_handle");
    GpuBinaryHandle->setAlignment(CGM.getPointerAlign().getAsAlign());
    CtorBuilder.CreateAlignedStore(RegisterFatbinCall, GpuBinaryHandle,
                                   CGM.getPointerAlign());

    // Call __cuda_register_globals(GpuBinaryHandle);
    if (RegisterGlobalsFunc)
      CtorBuilder.CreateCall(RegisterGlobalsFunc, RegisterFatbinCall);

    // Call __cudaRegisterFatBinaryEnd(Handle) if this CUDA version needs it.
    if (CudaFeatureEnabled(CGM.getTarget().getSDKVersion(),
                           CudaFeature::CUDA_USES_FATBIN_REGISTER_END)) {
      // void __cudaRegisterFatBinaryEnd(void **);
      llvm::FunctionCallee RegisterFatbinEndFunc = CGM.CreateRuntimeFunction(
          llvm::FunctionType::get(VoidTy, VoidPtrPtrTy, false),
          "__cudaRegisterFatBinaryEnd");
      CtorBuilder.CreateCall(RegisterFatbinEndFunc, RegisterFatbinCall);
    }
  } else {
    // Generate a unique module ID.
    SmallString<64> ModuleID;
    llvm::raw_svector_ostream OS(ModuleID);
    OS << ModuleIDPrefix << llvm::format("%" PRIx64, FatbinWrapper->getGUID());
    llvm::Constant *ModuleIDConstant = makeConstantString(
        std::string(ModuleID.str()), "", ModuleIDSectionName, 32);

    // Create an alias for the FatbinWrapper that nvcc will look for.
    llvm::GlobalAlias::create(llvm::GlobalValue::ExternalLinkage,
                              Twine("__fatbinwrap") + ModuleID, FatbinWrapper);

    // void __cudaRegisterLinkedBinary%ModuleID%(void (*)(void *), void *,
    // void *, void (*)(void **))
    SmallString<128> RegisterLinkedBinaryName("__cudaRegisterLinkedBinary");
    RegisterLinkedBinaryName += ModuleID;
    llvm::FunctionCallee RegisterLinkedBinaryFunc = CGM.CreateRuntimeFunction(
        getRegisterLinkedBinaryFnTy(), RegisterLinkedBinaryName);

    assert(RegisterGlobalsFunc && "Expecting at least dummy function!");
    llvm::Value *Args[] = {RegisterGlobalsFunc,
                           CtorBuilder.CreateBitCast(FatbinWrapper, VoidPtrTy),
                           ModuleIDConstant,
                           makeDummyFunction(getCallbackFnTy())};
    CtorBuilder.CreateCall(RegisterLinkedBinaryFunc, Args);
  }

  // Create destructor and register it with atexit() the way NVCC does it. Doing
  // it during regular destructor phase worked in CUDA before 9.2 but results in
  // double-free in 9.2.
  if (llvm::Function *CleanupFn = makeModuleDtorFunction()) {
    // extern "C" int atexit(void (*f)(void));
    llvm::FunctionType *AtExitTy =
        llvm::FunctionType::get(IntTy, CleanupFn->getType(), false);
    llvm::FunctionCallee AtExitFunc =
        CGM.CreateRuntimeFunction(AtExitTy, "atexit", llvm::AttributeList(),
                                  /*Local=*/true);
    CtorBuilder.CreateCall(AtExitFunc, CleanupFn);
  }

  CtorBuilder.CreateRetVoid();
  return ModuleCtorFunc;
}

/// Creates a global destructor function that unregisters the GPU code blob
/// registered by constructor.
///
/// For CUDA:
/// \code
/// void __cuda_module_dtor(void*) {
///     __cudaUnregisterFatBinary(Handle);
/// }
/// \endcode
///
/// For HIP:
/// \code
/// void __hip_module_dtor(void*) {
///     if (__hip_gpubin_handle) {
///         __hipUnregisterFatBinary(__hip_gpubin_handle);
///         __hip_gpubin_handle = 0;
///     }
/// }
/// \endcode
llvm::Function *CGNVCUDARuntime::makeModuleDtorFunction() {
  // No need for destructor if we don't have a handle to unregister.
  if (!GpuBinaryHandle)
    return nullptr;

  // void __cudaUnregisterFatBinary(void ** handle);
  llvm::FunctionCallee UnregisterFatbinFunc = CGM.CreateRuntimeFunction(
      llvm::FunctionType::get(VoidTy, VoidPtrPtrTy, false),
      addUnderscoredPrefixToName("UnregisterFatBinary"));

  llvm::Function *ModuleDtorFunc = llvm::Function::Create(
      llvm::FunctionType::get(VoidTy, VoidPtrTy, false),
      llvm::GlobalValue::InternalLinkage,
      addUnderscoredPrefixToName("_module_dtor"), &TheModule);

  llvm::BasicBlock *DtorEntryBB =
      llvm::BasicBlock::Create(Context, "entry", ModuleDtorFunc);
  CGBuilderTy DtorBuilder(CGM, Context);
  DtorBuilder.SetInsertPoint(DtorEntryBB);

  Address GpuBinaryAddr(GpuBinaryHandle, CharUnits::fromQuantity(
                                             GpuBinaryHandle->getAlignment()));
  auto HandleValue = DtorBuilder.CreateLoad(GpuBinaryAddr);
  // There is only one HIP fat binary per linked module, however there are
  // multiple destructor functions. Make sure the fat binary is unregistered
  // only once.
  if (CGM.getLangOpts().HIP) {
    llvm::BasicBlock *IfBlock =
        llvm::BasicBlock::Create(Context, "if", ModuleDtorFunc);
    llvm::BasicBlock *ExitBlock =
        llvm::BasicBlock::Create(Context, "exit", ModuleDtorFunc);
    llvm::Constant *Zero = llvm::Constant::getNullValue(HandleValue->getType());
    llvm::Value *NEZero = DtorBuilder.CreateICmpNE(HandleValue, Zero);
    DtorBuilder.CreateCondBr(NEZero, IfBlock, ExitBlock);

    DtorBuilder.SetInsertPoint(IfBlock);
    DtorBuilder.CreateCall(UnregisterFatbinFunc, HandleValue);
    DtorBuilder.CreateStore(Zero, GpuBinaryAddr);
    DtorBuilder.CreateBr(ExitBlock);

    DtorBuilder.SetInsertPoint(ExitBlock);
  } else {
    DtorBuilder.CreateCall(UnregisterFatbinFunc, HandleValue);
  }
  DtorBuilder.CreateRetVoid();
  return ModuleDtorFunc;
}

CGCUDARuntime *CodeGen::CreateNVCUDARuntime(CodeGenModule &CGM) {
  return new CGNVCUDARuntime(CGM);
}

void CGNVCUDARuntime::internalizeDeviceSideVar(
    const VarDecl *D, llvm::GlobalValue::LinkageTypes &Linkage) {
  // For -fno-gpu-rdc, host-side shadows of external declarations of device-side
  // global variables become internal definitions. These have to be internal in
  // order to prevent name conflicts with global host variables with the same
  // name in a different TUs.
  //
  // For -fgpu-rdc, the shadow variables should not be internalized because
  // they may be accessed by different TU.
  if (CGM.getLangOpts().GPURelocatableDeviceCode)
    return;

  // __shared__ variables are odd. Shadows do get created, but
  // they are not registered with the CUDA runtime, so they
  // can't really be used to access their device-side
  // counterparts. It's not clear yet whether it's nvcc's bug or
  // a feature, but we've got to do the same for compatibility.
  if (D->hasAttr<CUDADeviceAttr>() || D->hasAttr<CUDAConstantAttr>() ||
      D->hasAttr<CUDASharedAttr>() ||
      D->getType()->isCUDADeviceBuiltinSurfaceType() ||
      D->getType()->isCUDADeviceBuiltinTextureType()) {
    Linkage = llvm::GlobalValue::InternalLinkage;
  }
}

void CGNVCUDARuntime::handleVarRegistration(const VarDecl *D,
                                            llvm::GlobalVariable &GV) {
  if (D->hasAttr<CUDADeviceAttr>() || D->hasAttr<CUDAConstantAttr>()) {
    // Shadow variables and their properties must be registered with CUDA
    // runtime. Skip Extern global variables, which will be registered in
    // the TU where they are defined.
    //
    // Don't register a C++17 inline variable. The local symbol can be
    // discarded and referencing a discarded local symbol from outside the
    // comdat (__cuda_register_globals) is disallowed by the ELF spec.
    // TODO: Reject __device__ constexpr and __device__ inline in Sema.
    // HIP managed variables need to be always recorded in device and host
    // compilations for transformation.
    if ((!D->hasExternalStorage() && !D->isInline()) ||
        D->hasAttr<HIPManagedAttr>()) {
      registerDeviceVar(D, GV, !D->hasDefinition(),
                        D->hasAttr<CUDAConstantAttr>());
    }
  } else if (D->getType()->isCUDADeviceBuiltinSurfaceType() ||
             D->getType()->isCUDADeviceBuiltinTextureType()) {
    // Builtin surfaces and textures and their template arguments are
    // also registered with CUDA runtime.
    const auto *TD = cast<ClassTemplateSpecializationDecl>(
        D->getType()->castAs<RecordType>()->getDecl());
    const TemplateArgumentList &Args = TD->getTemplateArgs();
    if (TD->hasAttr<CUDADeviceBuiltinSurfaceTypeAttr>()) {
      assert(Args.size() == 2 &&
             "Unexpected number of template arguments of CUDA device "
             "builtin surface type.");
      auto SurfType = Args[1].getAsIntegral();
      if (!D->hasExternalStorage())
        registerDeviceSurf(D, GV, !D->hasDefinition(), SurfType.getSExtValue());
    } else {
      assert(Args.size() == 3 &&
             "Unexpected number of template arguments of CUDA device "
             "builtin texture type.");
      auto TexType = Args[1].getAsIntegral();
      auto Normalized = Args[2].getAsIntegral();
      if (!D->hasExternalStorage())
        registerDeviceTex(D, GV, !D->hasDefinition(), TexType.getSExtValue(),
                          Normalized.getZExtValue());
    }
  }
}

// Transform managed variables to pointers to managed variables in device code.
// Each use of the original managed variable is replaced by a load from the
// transformed managed variable. The transformed managed variable contains
// the address of managed memory which will be allocated by the runtime.
void CGNVCUDARuntime::transformManagedVars() {
  for (auto &&Info : DeviceVars) {
    llvm::GlobalVariable *Var = Info.Var;
    if (Info.Flags.getKind() == DeviceVarFlags::Variable &&
        Info.Flags.isManaged()) {
      auto ManagedVar = new llvm::GlobalVariable(
          CGM.getModule(), Var->getType(),
          /*isConstant=*/false, Var->getLinkage(),
          /*Init=*/Var->isDeclaration()
              ? nullptr
              : llvm::ConstantPointerNull::get(Var->getType()),
          /*Name=*/"", /*InsertBefore=*/nullptr,
          llvm::GlobalVariable::NotThreadLocal,
          CGM.getContext().getTargetAddressSpace(LangAS::cuda_device));
      ManagedVar->setDSOLocal(Var->isDSOLocal());
      ManagedVar->setVisibility(Var->getVisibility());
      ManagedVar->setExternallyInitialized(true);
      replaceManagedVar(Var, ManagedVar);
      ManagedVar->takeName(Var);
      Var->setName(Twine(ManagedVar->getName()) + ".managed");
      // Keep managed variables even if they are not used in device code since
      // they need to be allocated by the runtime.
      if (!Var->isDeclaration()) {
        assert(!ManagedVar->isDeclaration());
        CGM.addCompilerUsedGlobal(Var);
        CGM.addCompilerUsedGlobal(ManagedVar);
      }
    }
  }
}

// Returns module constructor to be added.
llvm::Function *CGNVCUDARuntime::finalizeModule() {
  if (CGM.getLangOpts().CUDAIsDevice) {
    transformManagedVars();

    // Mark ODR-used device variables as compiler used to prevent it from being
    // eliminated by optimization. This is necessary for device variables
    // ODR-used by host functions. Sema correctly marks them as ODR-used no
    // matter whether they are ODR-used by device or host functions.
    //
    // We do not need to do this if the variable has used attribute since it
    // has already been added.
    //
    // Static device variables have been externalized at this point, therefore
    // variables with LLVM private or internal linkage need not be added.
    for (auto &&Info : DeviceVars) {
      auto Kind = Info.Flags.getKind();
      if (!Info.Var->isDeclaration() &&
          !llvm::GlobalValue::isLocalLinkage(Info.Var->getLinkage()) &&
          (Kind == DeviceVarFlags::Variable ||
           Kind == DeviceVarFlags::Surface ||
           Kind == DeviceVarFlags::Texture) &&
          Info.D->isUsed() && !Info.D->hasAttr<UsedAttr>()) {
        CGM.addCompilerUsedGlobal(Info.Var);
      }
    }
    return nullptr;
  }
  return makeModuleCtorFunction();
}

llvm::GlobalValue *CGNVCUDARuntime::getKernelHandle(llvm::Function *F,
                                                    GlobalDecl GD) {
  auto Loc = KernelHandles.find(F);
  if (Loc != KernelHandles.end())
    return Loc->second;

  if (!CGM.getLangOpts().HIP) {
    KernelHandles[F] = F;
    KernelStubs[F] = F;
    return F;
  }

  auto *Var = new llvm::GlobalVariable(
      TheModule, F->getType(), /*isConstant=*/true, F->getLinkage(),
      /*Initializer=*/nullptr,
      CGM.getMangledName(
          GD.getWithKernelReferenceKind(KernelReferenceKind::Kernel)));
  Var->setAlignment(CGM.getPointerAlign().getAsAlign());
  Var->setDSOLocal(F->isDSOLocal());
  Var->setVisibility(F->getVisibility());
  KernelHandles[F] = Var;
  KernelStubs[Var] = F;
  return Var;
}
