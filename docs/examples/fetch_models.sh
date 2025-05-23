#!/bin/bash
# Copyright (c) 2018-2025, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set -ex

# Convert Tensorflow inception V3 module to ONNX
# Pre-requisite: Python3, venv, and Pip3 are installed on the system
mkdir -p model_repository/inception_onnx/1
wget -O /tmp/inception_v3_2016_08_28_frozen.pb.tar.gz \
     https://storage.googleapis.com/download.tensorflow.org/models/inception_v3_2016_08_28_frozen.pb.tar.gz
(cd /tmp && tar xzf inception_v3_2016_08_28_frozen.pb.tar.gz)
python3 -m venv tf2onnx
source ./tf2onnx/bin/activate
pip3 install "numpy<2" tensorflow tf2onnx
python3 -m tf2onnx.convert --graphdef /tmp/inception_v3_2016_08_28_frozen.pb --output inception_v3_onnx.model.onnx --inputs input:0 --outputs InceptionV3/Predictions/Softmax:0
deactivate
mv inception_v3_onnx.model.onnx model_repository/inception_onnx/1/model.onnx


# ONNX densenet
mkdir -p model_repository/densenet_onnx/1
wget -O model_repository/densenet_onnx/1/model.onnx \
     https://github.com/onnx/models/raw/main/validated/vision/classification/densenet-121/model/densenet-7.onnx
