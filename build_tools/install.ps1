# Copyright 2024 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Enable strict mode
$ErrorActionPreference = 'Stop'

$this_dir = Split-Path -Path $MyInvocation.MyCommand.Path -Parent
$repo_root = Resolve-Path -Path "$this_dir/.."
$iree_dir = Resolve-Path -Path "$repo_root/third_party/iree"
$build_dir = "$repo_root/iree-build"
$install_dir = "$repo_root/iree-install"

if (-not (Test-Path "$build_dir"))
{
    New-Item -Path $build_dir -ItemType Directory | Out-Null
}
$build_dir = Resolve-Path -Path "$build_dir"
$llvm_install_dir = "$env:llvm_install_dir"

echo "Installing"
echo "----------"
echo "Install to: $install_dir"
& cmake --build $build_dir --target install
& cmake --build $build_dir --target iree-install-dist

if ($llvm_install_dir -and (Test-Path "$llvm_install_dir"))
{
    Copy-Item -Path "$llvm_install_dir/bin/lld.exe" -Destination "$install_dir/bin" -Force
    Copy-Item -Path "$llvm_install_dir/bin/FileCheck.exe" -Destination "$install_dir/bin" -Force
    Copy-Item -Path "$llvm_install_dir/bin/not.exe" -Destination "$install_dir/bin" -Force
}

Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-matmul-test.exe" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-matmul-test.pdb" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-attention-test.exe" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-attention-test.pdb" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-conv2d-test.exe" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/testing/e2e/iree-e2e-conv2d-test.pdb" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/xrt_coreutil.dll" -Destination "$install_dir/bin" -Force
Copy-Item -Path "$build_dir/tools/xrt_coreutil.pdb" -Destination "$install_dir/bin" -Force
