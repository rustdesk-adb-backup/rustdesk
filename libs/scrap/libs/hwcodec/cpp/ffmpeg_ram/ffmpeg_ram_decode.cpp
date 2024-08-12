// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c
// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <memory>
#include <stdbool.h>
#include <cstring>

#define LOG_MODULE "FFMPEG_RAM_DEC"
#include <log.h>

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#ifdef ANDROID
extern "C" {
#include <jni.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <libavcodec/jni.h>
#include <libavcodec/mediacodec.h>
}
#endif

#include "common.h"
#include "system.h"

// #define CFG_PKG_TRACE

namespace {
typedef void (*RamDecodeCallback)(const void *obj, int width, int height,
                                  enum AVPixelFormat pixfmt,
                                  int linesize[AV_NUM_DATA_POINTERS],
                                  uint8_t *data[AV_NUM_DATA_POINTERS], int key);

class FFmpegRamDecoder {
public:
  const AVCodec *cid_ = NULL;
  AVCodecContext *c_ = NULL;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *sw_frame_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  bool hwaccel_ = true;

  std::string name_;
  AVHWDeviceType device_type_ = AV_HWDEVICE_TYPE_NONE;
  int thread_count_ = 1;
  RamDecodeCallback callback_ = NULL;
  DataFormat data_format_;
#ifdef ANDROID
  GLuint texture_name_ = 0;

  int align_ = 0;

  JNIEnv *env_ = NULL;

  jobject surface_ = NULL;
  jobject surface_texture_ = NULL;

  jmethodID surface_release_ = NULL;

  jmethodID surfaceTexture_release_ = NULL;
  jmethodID surfaceTexture_updateTexImage_ = NULL;
#endif

#ifdef CFG_PKG_TRACE
  int in_ = 0;
  int out_ = 0;
#endif

  FFmpegRamDecoder(const char *name, int device_type, int thread_count,
                   RamDecodeCallback callback) {
    this->name_ = name;
    this->device_type_ = (AVHWDeviceType)device_type;
    this->thread_count_ = thread_count;
    this->callback_ = callback;
  }

  ~FFmpegRamDecoder() {}

  void free_decoder() {
    if (frame_)
      av_frame_free(&frame_);
    if (pkt_)
      av_packet_free(&pkt_);
    if (sw_frame_)
      av_frame_free(&sw_frame_);
    if (c_)
      avcodec_free_context(&c_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);
#ifdef ANDROID
    if (surface_) {
      env_->CallVoidMethod(surface_, surface_release_);
      env_->ExceptionClear();
      env_->DeleteGlobalRef(surface_);
    }
    if (surface_texture_) {
      env_->CallVoidMethod(surface_texture_, surfaceTexture_release_);
      env_->ExceptionClear();
      env_->DeleteGlobalRef(surface_texture_);
    }
#endif

    frame_ = NULL;
    pkt_ = NULL;
    sw_frame_ = NULL;
    c_ = NULL;
    cid_ = NULL;
    hw_device_ctx_ = NULL;

#ifdef ANDROID
    texture_name_ = 0;
    surface_ = NULL;
    surface_texture_ = NULL;
#endif
  }

  int reset() {
    if (name_.find("vp8") != std::string::npos) {
      data_format_ = DataFormat::VP8;
    } else if (name_.find("vp9") != std::string::npos) {
      data_format_ = DataFormat::VP9;
    } else if (name_.find("av1") != std::string::npos) {
      data_format_ = DataFormat::AV1;
    } else if (name_.find("h264") != std::string::npos) {
      data_format_ = DataFormat::H264;
    } else if (name_.find("hevc") != std::string::npos) {
      data_format_ = DataFormat::H265;
    } else {
      LOG_ERROR("unsupported data format:" + name_);
      return -1;
    }

    free_decoder();
    hwaccel_ = device_type_ != AV_HWDEVICE_TYPE_NONE;
    int ret;

    if (!(cid_ = avcodec_find_decoder_by_name(name_.c_str()))) {
      LOG_ERROR("avcodec_find_decoder_by_name " + name_ + " failed");
      return -1;
    }

    if (!(c_ = avcodec_alloc_context3(cid_))) {
      LOG_ERROR("Could not allocate video codec context");
      return -1;
    }

    c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c_->thread_count =
        device_type_ != AV_HWDEVICE_TYPE_NONE ? 1 : thread_count_;
    c_->thread_type = FF_THREAD_SLICE;

    if (name_.find("qsv") != std::string::npos) {
      if ((ret = av_opt_set(c_->priv_data, "async_depth", "1", 0)) < 0) {
        LOG_ERROR("qsv set opt async_depth 1 failed");
        return -1;
      }
      // https://github.com/FFmpeg/FFmpeg/blob/c6364b711bad1fe2fbd90e5b2798f87080ddf5ea/libavcodec/qsvdec.c#L932
      // for disable warning
      c_->pkt_timebase = av_make_q(1, 30);
    }

    if (hwaccel_) {
      ret =
          av_hwdevice_ctx_create(&hw_device_ctx_, device_type_, NULL, NULL, 0);
      if (ret < 0) {
        LOG_ERROR("av_hwdevice_ctx_create failed, ret = " + av_err2str(ret));
        return -1;
      }
      c_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

#ifdef ANDROID
      c_->pix_fmt = AV_PIX_FMT_MEDIACODEC;
      // Set initial width and height to avoid crashes of some OMX decoders
      c_->width = 1920;
      c_->height = 1080;


EGLConfig eglConf;
EGLSurface eglSurface;
EGLContext eglCtx;
EGLDisplay eglDisp;

const EGLint confAttr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,    // very important!
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,          // we will create a pixelbuffer surface
            EGL_RED_SIZE,   8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE,  8,
            EGL_ALPHA_SIZE, 8,     // if you need the alpha channel
            EGL_DEPTH_SIZE, 16,    // if you need the depth buffer
            EGL_NONE
    };

    // EGL context attributes
    const EGLint ctxAttr[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,              // very important!
            EGL_NONE
    };

    // surface attributes
    // the surface size is set to the input frame size
    const EGLint surfaceAttr[] = {
             EGL_WIDTH, 1920,
             EGL_HEIGHT, 1080,
             EGL_NONE
    };

    EGLint eglMajVers, eglMinVers;
    EGLint numConfigs;

    eglDisp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisp, &eglMajVers, &eglMinVers);

    // choose the first config, i.e. best config
    eglChooseConfig(eglDisp, confAttr, &eglConf, 1, &numConfigs);

    eglCtx = eglCreateContext(eglDisp, eglConf, EGL_NO_CONTEXT, ctxAttr);

    // create a pixelbuffer surface
    eglSurface = eglCreatePbufferSurface(eglDisp, eglConf, surfaceAttr);

    eglMakeCurrent(eglDisp, eglSurface, eglSurface, eglCtx);

      // Get Java VM

      JavaVM *jvm = (JavaVM*)av_jni_get_java_vm(NULL);
      if (!jvm) {
        LOG_ERROR("Can not get JVM pointer");
        return -1;
      }

      // Get JNIEnv

      jint jret = jvm->AttachCurrentThread(&env_, NULL);
      if (jret != JNI_OK) {
        LOG_ERROR("Failed to obtain JNI environment");
        return -1;
      }

      // Generate named GLES texture and save it

      glGenTextures(1, &texture_name_);
//      glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_name_);

      // Now create SurfaceTexture and Surface instances

      jclass surfaceTextureClass = env_->FindClass("android/graphics/SurfaceTexture");
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/graphics/SurfaceTexture");
        env_->ExceptionClear();
        return -1;
      }

      jclass surfaceClass = env_->FindClass("android/view/Surface");
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/view/Surface");
        env_->ExceptionClear();
        return -1;
      }

      jmethodID surfaceTextureConstructor = env_->GetMethodID(
             surfaceTextureClass,
             "<init>",
             "(I)V"
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/graphics/SurfaceTexture constructor");
        env_->ExceptionClear();
        return -1;
      }

      surfaceTexture_release_ = env_->GetMethodID(
             surfaceTextureClass,
             "release",
             "()V"
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/graphics/SurfaceTexture.release() method");
        env_->ExceptionClear();
        return -1;
      }

      surfaceTexture_updateTexImage_ = env_->GetMethodID(
             surfaceTextureClass,
             "updateTexImage",
             "()V"
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/graphics/SurfaceTexture.updateTexImage() method");
        env_->ExceptionClear();
        return -1;
      }

      jmethodID surfaceConstructor = env_->GetMethodID(
             surfaceClass,
             "<init>",
             "(Landroid/graphics/SurfaceTexture;)V"
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/view/Surface constructor");
        env_->ExceptionClear();
        return -1;
      }

      surface_release_ = env_->GetMethodID(
             surfaceClass,
             "release",
             "()V"
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not find android/view/Surface.release() method");
        env_->ExceptionClear();
        return -1;
      }

      surface_texture_ = env_->NewGlobalRef(
        env_->NewObject(surfaceTextureClass, surfaceTextureConstructor, texture_name_)
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not create android/graphics/SurfaceTexture");
        env_->ExceptionClear();
        return -1;
      }

      surface_ = env_->NewGlobalRef(
        env_->NewObject(surfaceClass, surfaceConstructor, surface_texture_)
      );
      if (env_->ExceptionOccurred()) {
        LOG_ERROR("Can not create android/view/Surface");
        env_->ExceptionClear();
        return -1;
      }

      // Register the surface with FFmpeg

      AVMediaCodecContext *mc_ctx = av_mediacodec_alloc_context();

      ret = av_mediacodec_default_init(c_, mc_ctx, surface_);
      if (ret < 0) {
        LOG_ERROR("Failed to register surfqce; ret = " + av_err2str(ret));
        return -1;
      }

      LOG_INFO("Created surface!");
#else
      if (!check_support()) {
        LOG_ERROR("check_support failed");
        return -1;
      }
#endif

      if (!(sw_frame_ = av_frame_alloc())) {
        LOG_ERROR("av_frame_alloc failed");
        return -1;
      }
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR("av_packet_alloc failed");
      return -1;
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR("av_frame_alloc failed");
      return -1;
    }

#ifdef CFG_PKG_TRACE
    in_ = 0;
    out_ = 0;
#endif

    return 0;
  }

  int decode(const uint8_t *data, int length, const void *obj) {
    int ret = -1;
#ifdef CFG_PKG_TRACE
    in_++;
    LOG_DEBUG("delay DI: in:" + in_ + " out:" + out_);
#endif

    if (!data || !length) {
      LOG_ERROR("illegal decode parameter");
      return -1;
    }
    pkt_->data = (uint8_t *)data;
    pkt_->size = length;
    ret = do_decode(obj);
    return ret;
  }

private:
  int do_decode(const void *obj) {
    int ret;
    AVFrame *tmp_frame = NULL;
    bool decoded = false;

    if(!avcodec_is_open(c_)) {
      bool need_extradata = false;
      std::string sw_name = "";

      if (name_ == "h264_mediacodec") {
        need_extradata = true;
        sw_name = "h264";
      } else if (name_ == "hevc_mediacodec") {
        sw_name = "hevc";
        need_extradata = true;
      }

      AVCodecContext *sw_c = NULL;

      if (need_extradata) {
        const AVCodec *codec = NULL;

        if (!(codec = avcodec_find_decoder_by_name(sw_name.c_str()))) {
          LOG_ERROR("avcodec_find_decoder_by_name " + sw_name + " failed");
          return -1;
        }

        if (!(sw_c = avcodec_alloc_context3(codec))) {
          LOG_ERROR("Could not allocate video codec context");
          return -1;
        }

        if ((ret = avcodec_open2(sw_c, codec, NULL)) != 0) {
          LOG_ERROR("avcodec_open2 failed, ret = " + av_err2str(ret));
          return -1;
        }

        extract_extradata(sw_c);
      }

      if ((ret = avcodec_open2(c_, cid_, NULL)) != 0) {
        LOG_ERROR("avcodec_open2 failed, ret = " + av_err2str(ret));
        return -1;
      }
    }

    ret = avcodec_send_packet(c_, pkt_);
    if (ret < 0) {
      LOG_ERROR("avcodec_send_packet failed, ret = " + av_err2str(ret));
      return ret;
    }

    while (ret >= 0) {
      if ((ret = avcodec_receive_frame(c_, frame_)) != 0) {
        if (ret != AVERROR(EAGAIN)) {
          LOG_ERROR("avcodec_receive_frame failed, ret = " + av_err2str(ret));
        }
        goto _exit;
      }

      if (hwaccel_) {
#ifdef ANDROID
        if (frame_->format == AV_PIX_FMT_MEDIACODEC) {
          // Release decoded packet to surface

          int ret2 = av_mediacodec_release_buffer((AVMediaCodecBuffer *)frame_->data[3], 1);
          if (ret2 < 0) {
            LOG_ERROR("Failed to release MediaCodec buffer: ret = " + av_err2str(ret2));
            continue;
          }

          // Update image texture

          env_->CallVoidMethod(surface_texture_, surfaceTexture_updateTexImage_);
          if (env_->ExceptionOccurred()) {
            LOG_ERROR("Failed to update texture image");
            env_->ExceptionClear();
            continue;
          }

          // Store frame height and width and recreate temp buffer if changed

          if (sw_frame_->width != frame_->width || sw_frame_->height != frame_->height) {
            LOG_INFO("Frame dimensions changed");

            sw_frame_->format = AV_PIX_FMT_RGBA;
            sw_frame_->width = frame_->width;
            sw_frame_->height = frame_->height;

            // Set fields

            if ((ret2 = av_frame_get_buffer(sw_frame_, align_)) < 0) {
              LOG_ERROR("av_frame_get_buffer, ret = " + av_err2str(ret2));
              continue;
            }

            // Ensure sw frame is writable

            if ((ret2 = av_frame_make_writable(sw_frame_)) != 0) {
              LOG_ERROR("av_frame_make_writable failed, ret = " + av_err2str(ret2));
              continue;
            }
          }

          continue;

          // TODO: Render the texture into FBO / PBO

          // Read pixels from texture (slow!)

          glReadPixels(0, 0, frame_->width, frame_->height, GL_RGBA, GL_UNSIGNED_BYTE, sw_frame_->buf[0]->data);
        } else
#endif
        {
          if (!frame_->hw_frames_ctx) {
            LOG_ERROR("hw_frames_ctx is NULL");
            goto _exit;
          }
          if ((ret = av_hwframe_transfer_data(sw_frame_, frame_, 0)) < 0) {
            LOG_ERROR("av_hwframe_transfer_data failed, ret = " +
                      av_err2str(ret));
            goto _exit;
          }

          tmp_frame = sw_frame_;
        }
      } else {
        tmp_frame = frame_;
      }
      decoded = true;
#ifdef CFG_PKG_TRACE
      out_++;
      LOG_DEBUG("delay DO: in:" + in_ + " out:" + out_);
#endif
#if FF_API_FRAME_KEY
      int key_frame = frame_->flags & AV_FRAME_FLAG_KEY;
#else
      int key_frame = frame_->key_frame;
#endif

      callback_(obj, tmp_frame->width, tmp_frame->height,
                (AVPixelFormat)tmp_frame->format, tmp_frame->linesize,
                tmp_frame->data, key_frame);
    }
  _exit:
    av_packet_unref(pkt_);

    if (decoded) return 0;
    if (ret == AVERROR(EAGAIN)) return 0;
    return -1;
  }

  bool check_support() {
#ifdef _WIN32
    if (device_type_ == AV_HWDEVICE_TYPE_D3D11VA) {
      if (!c_->hw_device_ctx) {
        LOG_ERROR("hw_device_ctx is NULL");
        return false;
      }
      AVHWDeviceContext *deviceContext =
          (AVHWDeviceContext *)hw_device_ctx_->data;
      if (!deviceContext) {
        LOG_ERROR("deviceContext is NULL");
        return false;
      }
      AVD3D11VADeviceContext *d3d11vaDeviceContext =
          (AVD3D11VADeviceContext *)deviceContext->hwctx;
      if (!d3d11vaDeviceContext) {
        LOG_ERROR("d3d11vaDeviceContext is NULL");
        return false;
      }
      ID3D11Device *device = d3d11vaDeviceContext->device;
      if (!device) {
        LOG_ERROR("device is NULL");
        return false;
      }
      std::unique_ptr<NativeDevice> native_ = std::make_unique<NativeDevice>();
      if (!native_) {
        LOG_ERROR("Failed to create native device");
        return false;
      }
      if (!native_->Init(0, (ID3D11Device *)device, 0)) {
        LOG_ERROR("Failed to init native device");
        return false;
      }
      if (!native_->support_decode(data_format_)) {
        LOG_ERROR("Failed to check support " + name_);
        return false;
      }
      return true;
    } else {
      return true;
    }
#else
    return true;
#endif
  }

  int extract_extradata(const AVCodecContext *codec_ctx)
  {
    const AVBitStreamFilter *bsf = NULL;
    int ret;

    if ((bsf = av_bsf_get_by_name("extract_extradata")) == NULL)
    {
      LOG_ERROR("Failed to get extract_extradata bsf");
      return 0;
    }

    AVBSFContext *bsf_context;
    if ((ret=av_bsf_alloc(bsf, &bsf_context) ) < 0)
    {
      LOG_ERROR("Failed to allocate bsf context");
      return 0;
    }

    if ((ret=avcodec_parameters_from_context(bsf_context->par_in, codec_ctx)) < 0)
    {
      LOG_ERROR("Failed to copy parameters from context");
      av_bsf_free(&bsf_context);
      return 0;
    }

    if ((ret = av_bsf_init(bsf_context)) < 0)
    {
      LOG_ERROR("Failed to init bsf context");
      av_bsf_free(&bsf_context);
      return 0;
    }

    AVPacket *packet_ref = av_packet_alloc();
    if (av_packet_ref(packet_ref, pkt_) < 0)
    {
      LOG_ERROR("Failed to ref packet");
      av_bsf_free(&bsf_context);
      return 0;
    }

    if ((ret = av_bsf_send_packet(bsf_context, packet_ref)) < 0)
    {
      LOG_ERROR("Failed to send packet to bsf");
      av_packet_unref(packet_ref);
      av_bsf_free(&bsf_context);
      return 0;
    }

    int done=0;

    while (ret >= 0 && !done)
    {
      size_t extradata_size;
      uint8_t *extradata;

      ret = av_bsf_receive_packet(bsf_context, packet_ref);
      if (ret < 0)
      {
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
          LOG_ERROR("bsf error, not EAGAIN or EOF");
          av_packet_unref(packet_ref);
          av_bsf_free(&bsf_context);
          return 0;
        }

        continue;
      }

      extradata = av_packet_get_side_data(packet_ref, AV_PKT_DATA_NEW_EXTRADATA, &extradata_size);

      if (extradata)
      {
        LOG_INFO("Got extradata!");
        done = 1;
        c_->extradata = (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(c_->extradata, extradata, extradata_size);
        c_->extradata_size = extradata_size;
      }
    }

    av_packet_unref(packet_ref);
    av_bsf_free(&bsf_context);
    return 1;
  }
};

} // namespace

extern "C" void ffmpeg_ram_free_decoder(FFmpegRamDecoder *decoder) {
  try {
    if (!decoder)
      return;
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR("ffmpeg_ram_free_decoder exception:" + e.what());
  }
}

extern "C" FFmpegRamDecoder *
ffmpeg_ram_new_decoder(const char *name, int device_type, int thread_count,
                       RamDecodeCallback callback) {
  FFmpegRamDecoder *decoder = NULL;
  try {
    decoder = new FFmpegRamDecoder(name, device_type, thread_count, callback);
    if (decoder) {
      if (decoder->reset() == 0) {
        return decoder;
      }
    }
  } catch (std::exception &e) {
    LOG_ERROR("new decoder exception:" + e.what());
  }
  if (decoder) {
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_decode(FFmpegRamDecoder *decoder, const uint8_t *data,
                                 int length, const void *obj) {
  try {
    return decoder->decode(data, length, obj);
  } catch (const std::exception &e) {
    LOG_ERROR("ffmpeg_ram_decode exception:" + e.what());
  }
  return -1;
}
