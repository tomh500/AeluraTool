# Screen recorder third-party notices

The recording module uses Windows Media Foundation, WASAPI, DXGI and Direct3D APIs.
No NVIDIA, AMD or CUDA runtime DLL is bundled. Driver components are loaded only
from the Windows system directory. Users install and update their own display drivers.

## NVIDIA NVENC API header

Copyright (c) 2010-2026 NVIDIA Corporation.

Source: https://github.com/FFmpeg/nv-codec-headers/tree/71acc268a541f7d7f33cf0387229e3015cfb4977

Only nvEncodeAPI.h is included. Its MIT notice applies to this header only.

## AMD AMF public headers

Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.
Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.
Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

Source: https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/57ee0cfb0e1a08d9041f77435653794c1b012be0

The upstream LICENSE.txt is included alongside this notice as AMF.LICENSE.txt.
Only the public headers are compiled here; the upstream project's FFmpeg libraries
are not part of this recording module.

AMD notice regarding standards: AMD does not provide a license or sublicense to
any intellectual property rights relating to standards, including MPEG-2, MPEG-4,
AVC/H.264, HEVC/H.265, AAC, VC-1 and MP3. Any applicable third-party technology
royalties are not granted by these headers. The complete original notices remain
in Vendor/amf.

## MIT permission and warranty notice (NVIDIA and AMD headers)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## ScreenShotPlus selection behavior

The region normalization, window filtering and snapping behavior is derived from
the integrated ScreenShotPlus source. Its copyright and MIT license are retained
in Tools/ScreenShotPlus/LICENSE and shipped as Tools/Licenses/ScreenShotPlus.LICENSE.txt.
