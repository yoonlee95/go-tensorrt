// +build linux

package tensorrt

// #cgo LDFLAGS: -lnvinfer -lnvcaffe_parser -lcudart -L${SRCDIR} -lstdc++
// #cgo CXXFLAGS: -std=c++11  -O3 -Wall -g
// #cgo CXXFLAGS: -Wno-sign-compare -Wno-unused-function
// #cgo linux, amd64 CXXFLAGS: -I/usr/local/cuda/include -I/usr/include/x86_64-linux-gnu -I/opt/frameworks/tensorrt/include
// #cgo arm64 CXXFLAGS: -I/usr/local/cuda/include -I/usr/include/aarch64-linux-gnu -I/opt/frameworks/tensorrt/include
// #cgo CXXFLAGS: -I${SRCDIR}/cbits -I${SRCDIR}/cbits/util -I${SRCDIR}/cuda
// #cgo LDFLAGS: -L/usr/local/cuda/lib64 -L/opt/frameworks/tensorrt/lib -lcudart
import "C"
