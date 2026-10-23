# GPU tests on the laptop's RTX 3080 Ti (native Windows, not WSL)

Which GPU back ends exist in the repo (from CMakeLists.txt and core/src/dsp):

| Platform | LDPC decoder | Notes |
|---|---|---|
| macOS | Metal (`gpu_ldpc.mm`, `gpu_viterbi.mm`) | the default on the Mac |
| Windows | Direct3D 11 compute (`gpu_ldpc_d3d11.cpp`, `conceal_d3d11.cpp`) | works on any DX11 GPU: NVIDIA, Intel Iris Xe, AMD |
| Linux | none (`gpu_ldpc_stub.cpp`) | CPU only |
| Vulkan | **not implemented** in the core (only whisper's `GGML_VULKAN` option, which the project forces off) | so Vulkan drivers in WSL are not needed for OnAir; they are installed only to be ready |

So the real-GPU test is a **native Windows** run (D3D11). WSL2 cannot run it: the Linux build has no GPU decoder.
The laptop has two GPUs: the RTX 3080 Ti and the Intel Iris Xe. The D3D11 code takes the default adapter, so make
Windows use the RTX for the test (Settings > System > Display > Graphics > Add the test .exe > High performance) and
read the "GPU: ..." line the test prints.

## Switches
- `DECT2_NOGPU=1` (environment variable): the GPU LDPC and the D3D11 concealment are not used (CPU path). Use it to compare.
- `DECT2_VULKAN`: does not exist in the source today. If a task mentions it, there is no such path yet.

## Requirements
- Current NVIDIA Game Ready / Studio driver (DX11 is part of the driver; no CUDA toolkit, no Vulkan SDK needed).
- Nothing else for the D3D11 test. The Vulkan runtime (`vulkan-1.dll`) comes with the NVIDIA driver and is only relevant if a Vulkan back end is added later.

## How to run (needs a native Windows build, see the NATIVE section in docs/REMOTE-BUILD.md)
The test `test_gpu_ldpc` is built only on Apple and Windows (`if(APPLE OR WIN32)`). It checks the GPU decoder against the CPU decoder on
noisy codewords for all normal-frame rates, and times both. Prints "no GPU" and returns 0 when no adapter works.

    cd <windows build folder>
    set DECT2_NOGPU=
    .\test_gpu_ldpc.exe            # expect: GPU: NVIDIA GeForce RTX 3080 Ti Laptop GPU, no mismatches
    set DECT2_NOGPU=1
    .\test_gpu_ldpc.exe            # expect: no GPU (CPU only)

Until the native build exists, the cross-built `test_gpu_ldpc.exe` from `tools/package/make_windows_cross.sh` (it builds the tests too, ONAIR_WIN_BUILD)
can be copied to the laptop with its DLLs and run there the same way.

## CUDA (issue #39, DTMB decoder on CUDA)
Not implemented and not to be implemented now. The laptop makes it testable later: install the CUDA toolkit natively, add a `gpu_ldpc_cuda`
back end behind a CMake option, compare it with the D3D11 and CPU decoders using the same `test_gpu_ldpc` pattern. Until then DTMB LDPC runs on
Metal (Mac) or D3D11 (Windows).
