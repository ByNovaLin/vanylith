# Vanylith Third-Party Notices

The Vanylith Community Source License 1.0 applies only to portions of Vanylith
for which the Vanylith Licensor has authority to grant rights. The components
below remain subject to their own licenses and terms.

## Distribution inventory for v0.1.0

| Component | Source and version | Included in source/build | Bundled in official Windows binary/package | Governing terms | Attribution or notice | Source redistribution requirement | Binary redistribution requirement |
|---|---|---|---|---|---|---|---|
| bitcoin-core/libsecp256k1 | v0.8.0, signed annotated tag object `18f07c42218765cd46148d74d9fe575795f56dce`, peeled commit `6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d`; <https://github.com/bitcoin-core/secp256k1> | Fetched by CMake and statically linked | Yes, as linked object code | MIT License | Preserve the copyright and full MIT text below | No source-publication requirement; any source copies or substantial portions must retain the notice | Include the copyright and permission notice with copies or substantial portions |
| Microsoft Edge WebView2 SDK | 1.0.4191.47; hash-pinned NuGet package; <https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4191.47> | Headers and `WebView2LoaderStatic.lib` are used | Yes, loader as linked object code | BSD-3-Clause | Preserve Microsoft's copyright, conditions, and disclaimer below | No source-publication requirement; redistributed source must retain the notice, conditions, and disclaimer | Reproduce the copyright, conditions, and disclaimer in documentation or other distribution materials |
| Microsoft Edge WebView2 Evergreen Runtime | Installed and serviced independently by Microsoft | Runtime interface only | No | Microsoft WebView2 terms | No notice for a runtime copy is required from this package because no runtime copy is distributed | Not applicable | Not applicable |
| NVIDIA CUDA Runtime | CUDA Toolkit 13.3; `cudart_static.lib`; <https://docs.nvidia.com/cuda/eula/index.html> | Statically linked by CUDA builds | Yes, as object code incorporated into `Vanylith.exe`; no CUDA DLL is bundled | NVIDIA Software License Agreement and CUDA Toolkit Supplement, last updated January 26, 2026 | Preserve NVIDIA rights and comply with the EULA; this notice does not replace the EULA | No CUDA source is distributed by Vanylith | Attachment A lists Windows `cudart_static.lib` as redistributable with qualifying applications, subject to all EULA distribution requirements |
| NVIDIA Display Driver / NVML | User-installed NVIDIA driver | NVML is loaded dynamically if available | No | NVIDIA driver terms | No driver notice is redistributed by this package | Not applicable | Not applicable |
| Microsoft Visual C++ runtime | Visual Studio 2022 build tools; statically linked (`/MT`); <https://learn.microsoft.com/visualstudio/releases/2022/redistribution> | Build toolchain component | No standalone runtime file; runtime object code is incorporated into the executable | Applicable Microsoft Visual Studio / Build Tools license terms | No separate project attribution file was identified | Not applicable | Subject to the applicable Microsoft terms; release builds must be produced using a properly licensed toolchain. Microsoft's redistribution guidance states that qualifying linked output may be distributed subject to those terms |
| Windows SDK and system libraries | Windows CNG, Winsock, COM, Shell, User/GDI, and related system APIs | Headers/import libraries used for the build | No system DLL is bundled | Applicable Microsoft Windows/SDK terms | No separate project attribution file was identified | Not applicable | The executable imports only system-provided DLLs |

The current executable dependency audit reports only these system DLL imports:
`ADVAPI32.dll`, `bcrypt.dll`, `GDI32.dll`, `KERNEL32.dll`, `ole32.dll`,
`SHELL32.dll`, `USER32.dll`, and `WS2_32.dll`. The project does not package the
NVIDIA Driver, `nvml.dll`, the WebView2 Evergreen Runtime, or a CUDA Runtime
DLL.

## bitcoin-core/libsecp256k1 — MIT License

Copyright (c) 2013 Pieter Wuille

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Microsoft Edge WebView2 SDK — BSD-3-Clause License

Copyright (C) Microsoft Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* The name of Microsoft Corporation, or the names of its contributors may not
  be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

An exact standalone copy of the WebView2 SDK license is also retained at
`docs/third-party/WebView2-SDK-LICENSE.txt`.

## NVIDIA CUDA Toolkit distribution note

CUDA builds incorporate the CUDA Runtime static library into the application.
The NVIDIA CUDA Toolkit EULA's Attachment A identifies `cudart_static.lib` as a
Windows CUDA Runtime file that may be distributed with applications developed
by the licensee, subject to the full agreement. In particular, the application
must provide material additional functionality, the distributable SDK portion
must be accessed only by the application, and the SDK may not be distributed as
a standalone product. Vanylith's license does not purport to relicense NVIDIA
code, and Corresponding Source under the Vanylith Community Source License 1.0
does not include NVIDIA SDK source.

The authoritative NVIDIA terms are available at:
<https://docs.nvidia.com/cuda/eula/index.html>.
