// MUSA-only diagnostic: mirrors the formal T3 resource setup.
#include <cstdio>
#include <cstdlib>
#include "check.h"
#include "device_ir.h"
#include "flagcx.h"
#include "flagcx_kernel.h"
#include "tools.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

constexpr int kUnifiedIrSignalComboCount = 12;
constexpr int kUnifiedIrS23ComboCount = 18;

extern "C" flagcxResult_t flagcxMusaIrRunDiagnostic(
    int mode, flagcxDevComm_t handle, const void *opaque, int *out,
    flagcxStream_t stream);

#define DIAG_CHECK(call) do { \
  flagcxResult_t code = (call); \
  if (code != flagcxSuccess) { \
    std::fprintf(stderr, "IR DIAG setup/run failed: %s -> %d at line %d\n", \
                 #call, int(code), __LINE__); \
    std::fflush(stderr); \
    int initialized = 0; MPI_Initialized(&initialized); \
    if (initialized) MPI_Abort(MPI_COMM_WORLD, int(code)); \
    return int(code); \
  } \
} while (0)

int main(int argc, char *argv[]) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char *modeText = std::getenv("FLAGCX_MUSA_IR_CASE");
  const int mode = modeText ? std::atoi(modeText) : 0;
  if (mode < 1 || mode > 6) {
    std::fprintf(stderr, "Set FLAGCX_MUSA_IR_CASE=1..6; diagnostic only.\n");
    return 1;
  }

  flagcxDeviceHandle_t devHandle;
  DIAG_CHECK(flagcxDeviceHandleInit(&devHandle));
  flagcxComm_t comm;
  flagcxUniqueId uniqueId;

  int worldSize = 1, worldRank = 0;
  int totalProcs = 1, proc = 0;
#define RPRINTF(...)                                                           \
  do {                                                                         \
    printf("[rank %d] ", proc);                                                \
    printf(__VA_ARGS__);                                                       \
    fflush(stdout);                                                            \
  } while (0)
  MPI_Comm splitComm;
  uint64_t splitMask = 0;
  int color = 0;
  initMpiEnv(argc, argv, worldRank, worldSize, proc, totalProcs, color,
             splitComm, splitMask);

  parser args(argc, argv);
  size_t minBytes = args.getMinBytes();
  size_t maxBytes = args.getMaxBytes();
  int stepFactor = args.getStepFactor();

  if (stepFactor <= 1) {
    if (proc == 0)
      printf("Error: stepFactor must be > 1, got %d\n", stepFactor);
    MPI_Finalize();
    return 1;
  }

  int nGpu;
  DIAG_CHECK(devHandle->getDeviceCount(&nGpu));
  if (totalProcs != 8 || nGpu < 8) {
    std::fprintf(stderr, "IR diagnostics require 8 ranks and at least 8 visible GPUs\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
    return 1;
  }
  DIAG_CHECK(devHandle->setDevice(worldRank % nGpu));

  if (proc == 0)
    DIAG_CHECK(flagcxGetUniqueId(&uniqueId));
  MPI_Bcast((void *)&uniqueId, sizeof(flagcxUniqueId), MPI_BYTE, 0, splitComm);
  MPI_Barrier(MPI_COMM_WORLD);

  DIAG_CHECK(flagcxCommInitRank(&comm, totalProcs, &uniqueId, proc));

  flagcxStream_t stream;
  DIAG_CHECK(devHandle->streamCreate(&stream));

  // Create DevComm with signal/counter/barrier slots
  // Signal variants use 12 slots; S23 uses 18 signal/counter slots.
  flagcxDevCommRequirements reqs = FLAGCX_DEV_COMM_REQUIREMENTS_INITIALIZER;
  reqs.barrierCount = FLAGCX_DEVICE_CTA_COUNT;
  reqs.intraBarrierCount = FLAGCX_DEVICE_CTA_COUNT;
  reqs.interBarrierCount = FLAGCX_DEVICE_CTA_COUNT;
  reqs.interSignalCount = kUnifiedIrS23ComboCount;
  reqs.interCounterCount = kUnifiedIrS23ComboCount;

  flagcxDevComm_t devComm = nullptr;
  DIAG_CHECK(flagcxDevCommCreate(comm, &reqs, &devComm));

  // Allocate send/recv buffers for S23, the largest combination matrix.
  size_t bufSize = maxBytes * kUnifiedIrS23ComboCount;
  void *sendBuff = nullptr, *recvBuff = nullptr;
#ifdef FLAGCX_TEST_ALLOCATOR_SHMEM
  flagcxMemAllocator_t memAllocator = flagcxMemSHMEM;
#else
  flagcxMemAllocator_t memAllocator = flagcxMemCCL;
#endif
  DIAG_CHECK(flagcxMemAlloc(&recvBuff, bufSize, memAllocator));
  DIAG_CHECK(flagcxMemAlloc(&sendBuff, bufSize, memAllocator));

  // Register symmetric windows
  flagcxWindow_t sendWin = nullptr, recvWin = nullptr;
  DIAG_CHECK(flagcxCommWindowRegister(comm, sendBuff, bufSize, &sendWin,
                                       FLAGCX_WIN_COLL_SYMMETRIC,
                                       memAllocator));
  DIAG_CHECK(flagcxCommWindowRegister(comm, recvBuff, bufSize, &recvWin,
                                       FLAGCX_WIN_COLL_SYMMETRIC,
                                       memAllocator));

  // Create DevMem handles
  flagcxDevMem_t sendMem = nullptr, recvMem = nullptr;
  DIAG_CHECK(flagcxDevMemCreate(comm, sendBuff, bufSize, sendWin, &sendMem));
  DIAG_CHECK(flagcxDevMemCreate(comm, recvBuff, bufSize, recvWin, &recvMem));

  // Get device pointers for IR functions
  void *devCommPtr = nullptr;
  DIAG_CHECK(flagcxDevCommGetDevicePtr(devComm, &devCommPtr));
  void *sendMemPtr = nullptr, *recvMemPtr = nullptr;
  DIAG_CHECK(flagcxDevMemGetDevicePtr(sendMem, &sendMemPtr));
  DIAG_CHECK(flagcxDevMemGetDevicePtr(recvMem, &recvMemPtr));

  // Allocate results buffer
  int *devResults = nullptr;
  DIAG_CHECK(devHandle->deviceMalloc((void **)&devResults, 256 * sizeof(int),
                                      flagcxMemDevice, NULL));


  MPI_Barrier(MPI_COMM_WORLD);
  DIAG_CHECK(devHandle->deviceMemset(devResults, 0, 256 * sizeof(int),
                                    flagcxMemDevice, stream));
  DIAG_CHECK(devHandle->streamSynchronize(stream));
  DIAG_CHECK(flagcxMusaIrRunDiagnostic(mode, devComm, devCommPtr,
                                       devResults, stream));
  int results[64] = {};
  DIAG_CHECK(devHandle->deviceMemcpy(results, devResults, sizeof(results),
                                    flagcxMemcpyDeviceToHost, nullptr));
  bool passed = true;
  for (int block = 0; block < 4; ++block) {
    int *row = results + block * 16;
    passed &= row[0] == 1;
    if (mode == 1) {
      passed &= row[1] == proc && row[2] == totalProcs &&
                row[3] == proc && row[4] == totalProcs && row[5] > 0 && row[7] == 1;
      std::printf("IR DIAG rank=%d block=%d comm: rank=%d size=%d intraRank=%d "
                  "intraSize=%d contexts=%d deviceCommBytes=%d netPresent=%d\n",
                  proc, block, row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
    }
    if (mode == 2) {
      passed &= row[5] > 0 && row[5] <= 4;
      for (int i = 0; i < row[5] && i < 4; ++i) {
        passed &= row[8 + i * 2] == proc && row[9 + i * 2] == totalProcs;
        std::printf("IR DIAG rank=%d block=%d context=%d dcRank=%d dcSize=%d deviceNetBytes=%d\n",
                    proc, block, i, row[8 + i * 2], row[9 + i * 2], row[6]);
      }
    }
  }
  int local = passed ? 1 : 0, global = 0;
  MPI_Allreduce(&local, &global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  DIAG_CHECK(devHandle->deviceFree(devResults, flagcxMemDevice, nullptr));
  DIAG_CHECK(flagcxDevMemDestroy(comm, sendMem));
  DIAG_CHECK(flagcxDevMemDestroy(comm, recvMem));
  DIAG_CHECK(flagcxCommWindowDeregister(comm, sendWin, memAllocator));
  DIAG_CHECK(flagcxCommWindowDeregister(comm, recvWin, memAllocator));
  DIAG_CHECK(flagcxMemFree(sendBuff, memAllocator));
  DIAG_CHECK(flagcxMemFree(recvBuff, memAllocator));
  DIAG_CHECK(flagcxDevCommDestroy(comm, devComm));
  DIAG_CHECK(devHandle->streamDestroy(stream));
  DIAG_CHECK(flagcxCommDestroy(comm));
  DIAG_CHECK(flagcxDeviceHandleFree(devHandle));
  if (proc == 0)
    std::printf("IR DIAGNOSTIC CASE %d: %s; not T3 acceptance\n", mode, global ? "PASS" : "FAIL");
  MPI_Finalize();
  return global ? 0 : 1;
}
