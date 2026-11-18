cmake_minimum_required(VERSION 3.5.0)
project(yoloentry)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
set(LIBYUV_DIR ${NATIVERENDER_ROOT_PATH}/third_party/libyuv)
set(FREETYPE_DIR ${NATIVERENDER_ROOT_PATH}/freetype)
set(NCNN_DIR ${NATIVERENDER_ROOT_PATH}/third_party/ncnn)
set(OPENCV_DIR ${NATIVERENDER_ROOT_PATH}/third_party/opencv)
set(ONNXRT_DIR ${NATIVERENDER_ROOT_PATH}/third_party/onnxruntime)
set(OCRLITE_DIR ${NATIVERENDER_ROOT_PATH}/ocrlite)

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${LIBYUV_DIR}/include
    ${LIBYUV_DIR}/include/libyuv
    ${LIBYUV_DIR}
    ${FREETYPE_DIR}/include
    ${FREETYPE_DIR}/include/freetype
    ${NCNN_DIR}/include
    ${NCNN_DIR}/include/ncnn
    ${OCRLITE_DIR}/include
    ${OPENCV_DIR}/include/opencv4
    ${ONNXRT_DIR}/include
)

# ─────────────── libyuv ───────────────
file(GLOB LIBYUV_RAW ${LIBYUV_DIR}/source/*.cc)
set(LIBYUV_SOURCES "")
foreach(src ${LIBYUV_RAW})
    get_filename_component(fname ${src} NAME)
    if(fname MATCHES "mips|msa|sve|lsx|lasx|rvv|x86|_win|_gcc|_jpeg|mjpeg")
    else()
        list(APPEND LIBYUV_SOURCES ${src})
    endif()
endforeach()

add_library(yuv STATIC ${LIBYUV_SOURCES})
target_include_directories(yuv PRIVATE ${LIBYUV_DIR} ${LIBYUV_DIR}/include ${LIBYUV_DIR}/include/libyuv)
# ★ -O3：libyuv 像素循环为主，O3 相比 O2 提升 5~10%（每帧多趟缩放/转换的绝对热点）
target_compile_options(yuv PRIVATE -O3 -std=c++17 -DLIBYUV_NEON)
target_link_libraries(yuv PRIVATE libhilog_ndk.z.so)

# ─────────────── FreeType (静态) ───────────────
set(FREETYPE_SOURCES
    ${FREETYPE_DIR}/src/base/ftbase.c
    ${FREETYPE_DIR}/src/base/ftsystem.c
    ${FREETYPE_DIR}/src/base/ftinit.c
    ${FREETYPE_DIR}/src/base/ftdebug.c
    ${FREETYPE_DIR}/src/base/ftbbox.c
    ${FREETYPE_DIR}/src/base/ftglyph.c
    ${FREETYPE_DIR}/src/base/ftbitmap.c
    ${FREETYPE_DIR}/src/base/ftmm.c
    ${FREETYPE_DIR}/src/base/ftstroke.c
    ${FREETYPE_DIR}/src/base/ftsynth.c
    ${FREETYPE_DIR}/src/truetype/truetype.c
    ${FREETYPE_DIR}/src/cff/cff.c
    ${FREETYPE_DIR}/src/sfnt/sfnt.c
    ${FREETYPE_DIR}/src/type1/type1.c
    ${FREETYPE_DIR}/src/cid/type1cid.c
    ${FREETYPE_DIR}/src/pfr/pfr.c
    ${FREETYPE_DIR}/src/type42/type42.c
    ${FREETYPE_DIR}/src/winfonts/winfnt.c
    ${FREETYPE_DIR}/src/pcf/pcf.c
    ${FREETYPE_DIR}/src/bdf/bdf.c
    ${FREETYPE_DIR}/src/smooth/smooth.c
    ${FREETYPE_DIR}/src/raster/raster.c
    ${FREETYPE_DIR}/src/sdf/sdf.c
    ${FREETYPE_DIR}/src/svg/svg.c
    ${FREETYPE_DIR}/src/autofit/autofit.c
    ${FREETYPE_DIR}/src/psnames/psnames.c
    ${FREETYPE_DIR}/src/pshinter/pshinter.c
    ${FREETYPE_DIR}/src/psaux/psaux.c
    ${FREETYPE_DIR}/src/gzip/ftgzip.c
    ${FREETYPE_DIR}/src/lzw/ftlzw.c
)
add_library(freetype STATIC ${FREETYPE_SOURCES})
target_include_directories(freetype PUBLIC ${FREETYPE_DIR}/include ${FREETYPE_DIR}/include/freetype ${FREETYPE_DIR}/include/freetype/config)
target_compile_definitions(freetype PRIVATE FT2_BUILD_LIBRARY=1)
target_compile_options(freetype PRIVATE -O2 -fPIC -std=gnu11 -Wno-unused-parameter -Wno-unused-function)

# ─────────────── libusb (saki fork → OHOS fd 注入移植) ───────────────
# -D__ANDROID__ 让全部 fd 注入分支与 Android 行为一致：
# _get_usbfs_fd 直返 dpriv->fd、op_close 不关 fd、scan/monitor no-op。
set(LIBUSB_DIR ${NATIVERENDER_ROOT_PATH}/third_party/libusb)
set(PORT_DIR ${NATIVERENDER_ROOT_PATH}/third_party/port)
add_library(usb100 STATIC
    ${LIBUSB_DIR}/core.c
    ${LIBUSB_DIR}/descriptor.c
    ${LIBUSB_DIR}/hotplug.c
    ${LIBUSB_DIR}/io.c
    ${LIBUSB_DIR}/sync.c
    ${LIBUSB_DIR}/strerror.c
    ${LIBUSB_DIR}/os/android_usbfs.c
    ${LIBUSB_DIR}/os/android_netlink.c
    ${LIBUSB_DIR}/os/poll_posix.c
    ${LIBUSB_DIR}/os/threads_posix.c
)
target_include_directories(usb100 PUBLIC ${LIBUSB_DIR} ${LIBUSB_DIR}/os ${PORT_DIR})
target_compile_options(usb100 PRIVATE -O2 -fPIC -D__ANDROID__ -DACCESS_RAW_DESCRIPTORS -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-format)
target_link_libraries(usb100 PUBLIC libhilog_ndk.z.so)

# ─────────────── libuvc (saki fork → OHOS) ───────────────
set(LIBUVC_DIR ${NATIVERENDER_ROOT_PATH}/third_party/libuvc)
set(LIBJPEG_DIR ${NATIVERENDER_ROOT_PATH}/third_party/libjpeg)
add_library(uvc STATIC
    ${LIBUVC_DIR}/src/ctrl.c
    ${LIBUVC_DIR}/src/device.c
    ${LIBUVC_DIR}/src/diag.c
    ${LIBUVC_DIR}/src/frame.c
    ${LIBUVC_DIR}/src/frame-mjpeg.c
    ${LIBUVC_DIR}/src/init.c
    ${LIBUVC_DIR}/src/stream.c
)
target_include_directories(uvc PUBLIC
    ${LIBUVC_DIR}/include
    ${LIBUSB_DIR}
    ${NATIVERENDER_ROOT_PATH}/third_party
    ${PORT_DIR}
    ${LIBJPEG_DIR}/include
)
target_compile_options(uvc PRIVATE -O2 -fPIC -D__ANDROID__ -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-format)
target_link_libraries(uvc PUBLIC
    usb100
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibjpeg-turbo.a
)

# ─────────────── ocrlite (RapidOCR) ───────────────
add_library(ocrlite STATIC
    ${OCRLITE_DIR}/src/OcrLite.cpp
    ${OCRLITE_DIR}/src/OcrLiteImpl.cpp
    ${OCRLITE_DIR}/src/DbNet.cpp
    ${OCRLITE_DIR}/src/AngleNet.cpp
    ${OCRLITE_DIR}/src/CrnnNet.cpp
    ${OCRLITE_DIR}/src/OcrResultUtils.cpp
    ${OCRLITE_DIR}/src/OcrUtils.cpp
    ${OCRLITE_DIR}/src/clipper.cpp
)
target_include_directories(ocrlite PUBLIC
    ${OCRLITE_DIR}/include
    ${OPENCV_DIR}/include/opencv4
    ${ONNXRT_DIR}/include
)
target_compile_options(ocrlite PRIVATE -O2 -std=c++17 -Wno-unused-parameter -Wno-deprecated-declarations)

# ─────────────── libocr.so（OCR 引擎独立动态库）───────────────
# 运行时由 libentry.so 内 OcrBridge dlopen，避免 entry 模块加载期
# 依赖 libonnxruntime.so 的重定位问题。
add_library(ocr SHARED
    RapidOCR.cpp
    TableNet.cpp
    SrNet.cpp
    FaceNet.cpp
)
target_include_directories(ocr PRIVATE
    ${NATIVERENDER_ROOT_PATH}
    ${OCRLITE_DIR}/include
    ${OPENCV_DIR}/include/opencv4
    ${ONNXRT_DIR}/include
)
target_link_libraries(ocr PRIVATE
    ocrlite
    ${OPENCV_DIR}/lib/libopencv_imgcodecs.a
    ${OPENCV_DIR}/lib/libopencv_imgproc.a
    ${OPENCV_DIR}/lib/libopencv_core.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibjpeg-turbo.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibopenjp2.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibpng.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibtiff.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/liblibwebp.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/libittnotify.a
    ${OPENCV_DIR}/lib/opencv4/3rdparty/libtegra_hal.a
    ${NATIVERENDER_ROOT_PATH}/../../../libs/arm64-v8a/libonnxruntime.so
    libhilog_ndk.z.so
    libz.so
    pthread
)
target_compile_options(ocr PRIVATE -O2 -std=c++17)

# ─────────────── 主库 ───────────────
add_library(entry SHARED
    napi_init.cpp
    OcrBridge.cpp
    yolo_detector.cpp
    nms.cpp
    yuv_pipeline.cpp
    uvc_camera.cpp
)

target_link_libraries(entry PUBLIC
    yuv
    freetype
    uvc
    usb100
    ${NCNN_DIR}/lib/libncnn.a
    libace_napi.z.so
    libhilog_ndk.z.so
    libohimage.so
    libimage_receiver.so
    libnative_image.so
    libnative_buffer.so
    libnative_window.so
    librawfile.z.so
    libz.so
    pthread
)

target_compile_options(entry PRIVATE -O3 -std=c++17)