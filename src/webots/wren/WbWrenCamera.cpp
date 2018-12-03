// Copyright 1996-2018 Cyberbotics Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "WbWrenCamera.hpp"

#include "WbLog.hpp"
#include "WbPreferences.hpp"
#include "WbRandom.hpp"
#include "WbSimulationState.hpp"
#include "WbVector2.hpp"
#include "WbVector4.hpp"
#include "WbWrenOpenGlContext.hpp"
#include "WbWrenPostProcessingEffects.hpp"
#include "WbWrenRenderingContext.hpp"
#include "WbWrenShaders.hpp"

#include <wren/camera.h>
#include <wren/frame_buffer.h>
#include <wren/node.h>
#include <wren/post_processing_effect.h>
#include <wren/scene.h>
#include <wren/shader_program.h>
#include <wren/texture_2d.h>
#include <wren/texture_rtt.h>
#include <wren/transform.h>
#include <wren/viewport.h>

#include <QtCore/QTime>
#include <QtGui/QImageReader>

static const float cDofFarBlurCutoff = 1.5f;
static const float cDofBlurTextureSize[2] = {320.0f, 320.0f};

WbWrenCamera::WbWrenCamera(WrTransform *node, int width, int height, float nearValue, float minRange, float maxRange, float fov,
                           char type, bool hasAntiAliasing, bool isSpherical) :
  mNode(node),
  mWidth(width),
  mHeight(height),
  mNear(nearValue),
  mExposure(1.0f),
  mMinRange(minRange),
  mMaxRange(maxRange),
  mFieldOfView(fov),
  mType(type),
  mAntiAliasing(hasAntiAliasing),
  mIsSpherical(isSpherical),
  mIsCopyingEnabled(false),
  mNotifyOnTextureUpdate(false),
  mPostProcessingEffects(),
  mSphericalPostProcessingEffect(NULL),
  mNumActivePostProcessingEffects(0),
  mColorNoiseIntensity(0.0f),
  mRangeNoiseIntensity(0.0f),
  mDepthResolution(-1.0f),
  mFocusDistance(0.0f),
  mFocusLength(0.0f),
  mIsLensDistortionEnabled(false),
  mLensDistortionCenter(0.5, 0.5),
  mLensDistortionRadialCoeffs(0.0, 0.0),
  mLensDistortionTangentialCoeffs(0.0, 0.0),
  mMotionBlurIntensity(0.0f),
  mNoiseMaskTexture(NULL) {
  if (mType == 'c' || mIsSpherical)
    ++mNumActivePostProcessingEffects;

  init();
}

WbWrenCamera::~WbWrenCamera() {
  cleanup();
}

WrTexture *WbWrenCamera::getWrenTexture() const {
  return WR_TEXTURE(wr_frame_buffer_get_output_texture(mResultFrameBuffer, 0));
}

int WbWrenCamera::textureGLId() const {
  return wr_texture_get_gl_name(getWrenTexture());
}

void WbWrenCamera::setSize(int width, int height) {
  if (width == mWidth && height == mHeight)
    return;

  mWidth = width;
  mHeight = height;

  cleanup();
  init();
}

void WbWrenCamera::setNear(float nearValue) {
  mNear = nearValue;
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i)
    if (mIsCameraActive[i])
      wr_camera_set_near(mCamera[i], mNear);
}

void WbWrenCamera::setFar(float farValue) {
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i)
    if (mIsCameraActive[i])
      wr_camera_set_far(mCamera[i], farValue);
}

void WbWrenCamera::setExposure(float exposure) {
  mExposure = exposure;
}

void WbWrenCamera::setMinRange(float minRange) {
  mMinRange = minRange;
}

void WbWrenCamera::setMaxRange(float maxRange) {
  mMaxRange = maxRange;

  bool isRangeFinderOrLidar = mType == 'r' || mType == 'l';
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i] && isRangeFinderOrLidar)
      wr_camera_set_far(mCamera[i], mMaxRange);
  }

  if (isRangeFinderOrLidar)
    setBackgroundColor(WbRgb(mMaxRange, mMaxRange, mMaxRange));
}

void WbWrenCamera::setFieldOfView(float fov) {
  double aspectRatio = (double)mWidth / mHeight;
  double fieldOfViewY = 1.0;

  if (mIsSpherical) {
    if (fov != mFieldOfView) {
      mFieldOfView = fov;
      cleanup();
      init();
    }

    if (fov > M_PI_2) {  // maximum X field of view of the sub-camera is pi / 2
      aspectRatio = aspectRatio * (M_PI_2 / fov);
      fov = M_PI_2;
    }

    fieldOfViewY = computeFieldOfViewY(fov, aspectRatio);
    if (fieldOfViewY > M_PI_2) {  // maximum Y field of view of the sub-camera is pi / 2
      fieldOfViewY = M_PI_2;
      aspectRatio = 1.0;
    }
  } else {
    mFieldOfView = fov;
    fieldOfViewY = computeFieldOfViewY(fov, aspectRatio);  // fovX -> fovY
    fieldOfViewY = qBound(0.001, fieldOfViewY, M_PI - 0.001);
  }

  setFovy(fieldOfViewY);
  setAspectRatio(aspectRatio);
}

void WbWrenCamera::setMotionBlur(float blur) {
  if (blur == mMotionBlurIntensity)
    return;

  const bool hasStatusChanged = mMotionBlurIntensity == 0.0f || blur == 0.0f;

  mMotionBlurIntensity = blur;
  if (hasStatusChanged) {
    cleanup();

    if (mMotionBlurIntensity > 0.0f)
      ++mNumActivePostProcessingEffects;
    else
      --mNumActivePostProcessingEffects;

    init();
  }
}

void WbWrenCamera::setFocus(float distance, float length) {
  if (mIsSpherical || (distance == mFocusDistance && length == mFocusLength))
    return;

  const bool hasStatusChanged = ((mFocusDistance == 0.0f || mFocusLength == 0.0f) && (distance > 0.0f && length > 0.0f)) ||
                                ((mFocusDistance > 0.0f && mFocusLength > 0.0f) && (distance == 0.0f || length == 0.0f));

  mFocusDistance = distance;
  mFocusLength = length;

  if (hasStatusChanged) {
    cleanup();

    if (mFocusDistance > 0.0f && mFocusLength > 0.0f)
      ++mNumActivePostProcessingEffects;
    else
      --mNumActivePostProcessingEffects;

    init();
  }
}

void WbWrenCamera::enableLensDistortion() {
  if (!mIsLensDistortionEnabled) {
    mIsLensDistortionEnabled = true;
    cleanup();
    ++mNumActivePostProcessingEffects;
    init();
  }
}

void WbWrenCamera::disableLensDistortion() {
  if (mIsLensDistortionEnabled) {
    mIsLensDistortionEnabled = false;
    cleanup();
    --mNumActivePostProcessingEffects;
    init();
  }
}

void WbWrenCamera::setLensDistortionCenter(const WbVector2 &center) {
  mLensDistortionCenter = center;
}

void WbWrenCamera::setRadialLensDistortionCoefficients(const WbVector2 &coefficients) {
  mLensDistortionRadialCoeffs = coefficients;
}

void WbWrenCamera::setTangentialLensDistortionCoefficients(const WbVector2 &coefficients) {
  mLensDistortionTangentialCoeffs = coefficients;
}

void WbWrenCamera::setColorNoise(float colorNoise) {
  if (mType != 'c' || colorNoise == mColorNoiseIntensity)
    return;

  const bool hasStatusChanged = mColorNoiseIntensity == 0.0f || colorNoise == 0.0f;

  mColorNoiseIntensity = colorNoise;
  if (hasStatusChanged) {
    cleanup();

    if (mColorNoiseIntensity > 0.0f)
      ++mNumActivePostProcessingEffects;
    else
      --mNumActivePostProcessingEffects;

    init();
  }
}

void WbWrenCamera::setRangeNoise(float rangeNoise) {
  if ((mType != 'r' && mType != 'l') || rangeNoise == mRangeNoiseIntensity)
    return;

  const bool hasStatusChanged = mRangeNoiseIntensity == 0.0f || rangeNoise == 0.0f;

  mRangeNoiseIntensity = rangeNoise;
  if (hasStatusChanged) {
    cleanup();

    if (mRangeNoiseIntensity > 0.0f)
      ++mNumActivePostProcessingEffects;
    else
      --mNumActivePostProcessingEffects;

    init();
  }
}

void WbWrenCamera::setRangeResolution(float resolution) {
  if ((mType != 'r' && mType != 'l') || resolution == mDepthResolution)
    return;

  // A value of -1 signifies disabled, see WbRangeFinder::applyResolutionToWren()
  const bool hasStatusChanged = mDepthResolution == -1.0f || resolution == -1.0f;

  mDepthResolution = resolution;
  if (hasStatusChanged) {
    cleanup();

    if (mDepthResolution != -1.0f)
      ++mNumActivePostProcessingEffects;
    else
      --mNumActivePostProcessingEffects;

    init();
  }
}

QString WbWrenCamera::setNoiseMask(const char *noiseMaskTexturePath) {
  if ((mType == 'r' || mType == 'l') || mIsSpherical)
    return tr("Noise mask can only be applied to RGB non-spherical cameras");

  cleanup();

  mNoiseMaskTexture = wr_texture_2d_copy_from_cache(noiseMaskTexturePath);
  if (!mNoiseMaskTexture) {
    QImage *image = new QImage();
    QImageReader imageReader(noiseMaskTexturePath);
    if (!imageReader.read(image)) {
      delete image;
      return tr("Cannot load %1: %2").arg(noiseMaskTexturePath).arg(imageReader.errorString());
    }

    const bool isTranslucent = image->pixelFormat().alphaUsage() == QPixelFormat::UsesAlpha;
    if (image->format() != QImage::Format_ARGB32) {
      QImage tmp = image->convertToFormat(QImage::Format_ARGB32);
      image->swap(tmp);
    }

    WbWrenOpenGlContext::makeWrenCurrent();

    mNoiseMaskTexture = wr_texture_2d_new();
    wr_texture_set_size(WR_TEXTURE(mNoiseMaskTexture), image->width(), image->height());
    wr_texture_2d_set_data(mNoiseMaskTexture, reinterpret_cast<const char *>(image->bits()));
    wr_texture_2d_set_file_path(mNoiseMaskTexture, noiseMaskTexturePath);
    wr_texture_set_translucent(WR_TEXTURE(mNoiseMaskTexture), isTranslucent);
    wr_texture_setup(WR_TEXTURE(mNoiseMaskTexture));

    WbWrenOpenGlContext::doneWren();

    delete image;
  }

  WbVector2 factor(1.0, 1.0);
  const double textureWidth = wr_texture_get_width(WR_TEXTURE(mNoiseMaskTexture));
  const double textureHeight = wr_texture_get_height(WR_TEXTURE(mNoiseMaskTexture));
  const double diffW = textureWidth - mWidth;
  const double diffH = textureHeight - mHeight;
  const double ratio = (double)(mWidth) / mHeight;
  if (diffW < 0 || diffH < 0) {
    if (diffW > diffH)
      factor.setX(ratio);
    else
      factor.setY(1.0 / ratio);
  } else {
    factor.setXy(mWidth / textureWidth, mHeight / textureHeight);
  }
  mNoiseMaskTextureFactor = factor;

  if (mNoiseMaskTexture)
    ++mNumActivePostProcessingEffects;
  else
    --mNumActivePostProcessingEffects;

  init();

  return "";
}

void WbWrenCamera::setBackgroundColor(const WbRgb &color) {
  if (mType == 'c')
    mBackgroundColor = color;
  else
    mBackgroundColor = WbRgb(mMaxRange, mMaxRange, mMaxRange);

  const float backgroundColor[] = {static_cast<float>(mBackgroundColor.red()), static_cast<float>(mBackgroundColor.green()),
                                   static_cast<float>(mBackgroundColor.blue())};

  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_viewport_set_clear_color_rgb(mCameraViewport[i], backgroundColor);
  }
}

void WbWrenCamera::render() {
  int numActiveViewports = 0;
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      mViewportsToRender[numActiveViewports++] = mCameraViewport[i];
  }

  if (!numActiveViewports)
    return;

  if (mType != 'c') {
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::encodeDepthShader(), "minRange",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mMinRange));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::encodeDepthShader(), "maxRange",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mMaxRange));
  }

  WbWrenOpenGlContext::makeWrenCurrent();
  // Depth information needs to be conserved for post-processing shaders
  wr_scene_enable_depth_reset(wr_scene_get_instance(), false);
  wr_scene_render_to_viewports(wr_scene_get_instance(), numActiveViewports, mViewportsToRender,
                               (mType != 'c') ? "encodeDepth" : NULL);

  if (mIsSpherical) {
    applySphericalPostProcessingEffect();

    if (mNumActivePostProcessingEffects) {
      foreach (WrPostProcessingEffect *const effect, mPostProcessingEffects)
        wr_post_processing_effect_set_result_frame_buffer(effect, mResultFrameBuffer);

      applyPostProcessingEffectStack(CAMERA_ORIENTATION_COUNT);
    }
  } else if (!mPostProcessingEffects.empty()) {
    for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
      if (mIsCameraActive[i]) {
        foreach (WrPostProcessingEffect *const effect, mPostProcessingEffects)
          wr_post_processing_effect_set_result_frame_buffer(effect, mResultFrameBuffer);

        applyPostProcessingEffectStack(i);
      }
    }
  }

  mFirstRenderingCall = false;

  wr_scene_enable_depth_reset(wr_scene_get_instance(), true);
  WbWrenOpenGlContext::doneWren();

  if (mNotifyOnTextureUpdate)
    emit textureUpdated();
}

void WbWrenCamera::enableCopying(bool enable) {
  if (enable && !mIsCopyingEnabled) {
    mIsCopyingEnabled = true;
    WbWrenOpenGlContext::makeWrenCurrent();
    wr_frame_buffer_enable_copying(mResultFrameBuffer, 1, true);
    WbWrenOpenGlContext::doneWren();
  } else if (!enable && mIsCopyingEnabled) {
    mIsCopyingEnabled = false;
    WbWrenOpenGlContext::makeWrenCurrent();
    wr_frame_buffer_enable_copying(mResultFrameBuffer, 1, false);
    WbWrenOpenGlContext::doneWren();
  }
}

WbRgb WbWrenCamera::copyPixelColourValue(int x, int y) {
  if (mWidth < 1 || mHeight < 1 || !mIsCameraActive[CAMERA_ORIENTATION_FRONT])
    return WbRgb();

  // This method is only called when the user hovers the mouse pointer over the camera overlay
  // in paused mode, so even though it isn't optimal, copying is enabled and then disabled again.
  uint8_t pixelData[4];

  WbWrenOpenGlContext::makeWrenCurrent();
  bool wasCopyingEnabled = mIsCopyingEnabled;
  enableCopying(true);
  wr_frame_buffer_copy_pixel(mResultFrameBuffer, 1, x, y, reinterpret_cast<void *>(pixelData), false);
  enableCopying(wasCopyingEnabled);
  WbWrenOpenGlContext::doneWren();

  WbRgb result;
  if (mType == 'c') {
    // convert BGR to RGB
    result = WbRgb(pixelData[2], pixelData[1], pixelData[0]);
  } else {
    float value;
    memcpy(&value, &pixelData[0], 4);
    result = WbRgb(value, value, value);
  }

  return result;
}

void WbWrenCamera::copyContentsToMemory(void *data) {
  if (!mIsCopyingEnabled || !data || mWidth < 1 || mHeight < 1)
    return;

  if (!mIsCameraActive[CAMERA_ORIENTATION_FRONT]) {
    memset(data, 0, mWidth * mHeight * 4);
    return;
  }

  WbWrenOpenGlContext::makeWrenCurrent();
  wr_frame_buffer_copy_contents(mResultFrameBuffer, 1, data);
  WbWrenOpenGlContext::doneWren();
}

void WbWrenCamera::rotatePitch(float angle) {
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_camera_apply_pitch(mCamera[i], angle);
  }
}

void WbWrenCamera::rotateYaw(float angle) {
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_camera_apply_yaw(mCamera[i], angle);
  }
}

float WbWrenCamera::computeFieldOfViewY(double fovX, double aspectRatio) {
  return 2 * atan(tan(fovX * 0.5) / aspectRatio);
}

void WbWrenCamera::init() {
  assert(mNumActivePostProcessingEffects >= 0);
  mFirstRenderingCall = true;
  mIsCopyingEnabled = false;

  if (mType == 'c')
    mTextureFormat = WR_TEXTURE_INTERNAL_FORMAT_RGBA16F;
  else
    mTextureFormat = WR_TEXTURE_INTERNAL_FORMAT_R32F;

  WbWrenOpenGlContext::makeWrenCurrent();

  WrTextureRtt *renderingTexture = wr_texture_rtt_new();
  wr_texture_rtt_enable_initialize_data(renderingTexture, true);
  wr_texture_set_internal_format(WR_TEXTURE(renderingTexture), mTextureFormat);

  WrTextureRtt *outputTexture = wr_texture_rtt_new();
  wr_texture_rtt_enable_initialize_data(outputTexture, true);
  if (mType == 'c')
    wr_texture_set_internal_format(WR_TEXTURE(outputTexture), WR_TEXTURE_INTERNAL_FORMAT_RGBA8);
  else
    wr_texture_set_internal_format(WR_TEXTURE(outputTexture), mTextureFormat);

  mResultFrameBuffer = wr_frame_buffer_new();
  wr_frame_buffer_set_size(mResultFrameBuffer, mWidth, mHeight);
  wr_frame_buffer_append_output_texture(mResultFrameBuffer, renderingTexture);
  wr_frame_buffer_append_output_texture(mResultFrameBuffer, outputTexture);

  mIsCameraActive[CAMERA_ORIENTATION_FRONT] = true;
  for (int i = CAMERA_ORIENTATION_FRONT + 1; i < CAMERA_ORIENTATION_COUNT; ++i)
    mIsCameraActive[i] = false;

  if (mIsSpherical) {
    setupSphericalSubCameras();

    for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
      if (mIsCameraActive[i])
        setupCamera(i, mSubCamerasResolutionX, mSubCamerasResolutionY);
    }

    setupSphericalPostProcessingEffect();
  } else
    setupCamera(CAMERA_ORIENTATION_FRONT, mWidth, mHeight);

  wr_frame_buffer_setup(mResultFrameBuffer);

  if (mNumActivePostProcessingEffects)
    setupPostProcessingEffects();

  setCamerasOrientations();
  setNear(mNear);
  setMinRange(mMinRange);
  setMaxRange(mMaxRange);
  setFieldOfView(mFieldOfView);
  setBackgroundColor(mBackgroundColor);

  emit cameraInitialized();

  WbWrenOpenGlContext::doneWren();
}

void WbWrenCamera::cleanup() {
  if (!mCamera[CAMERA_ORIENTATION_FRONT] || (mIsSpherical && !mSphericalPostProcessingEffect))
    return;

  WbWrenOpenGlContext::makeWrenCurrent();
  foreach (WrPostProcessingEffect *const effect, mPostProcessingEffects)
    wr_post_processing_effect_delete(effect);
  mPostProcessingEffects.clear();

  wr_post_processing_effect_delete(mSphericalPostProcessingEffect);
  mSphericalPostProcessingEffect = NULL;

  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i]) {
      wr_node_delete(WR_NODE(mCamera[i]));
      wr_viewport_delete(mCameraViewport[i]);

      if (mIsSpherical || mNumActivePostProcessingEffects) {
        wr_texture_delete(WR_TEXTURE(wr_frame_buffer_get_output_texture(mCameraFrameBuffer[i], 0)));
        wr_texture_delete(WR_TEXTURE(wr_frame_buffer_get_depth_texture(mCameraFrameBuffer[i])));
        wr_frame_buffer_delete(mCameraFrameBuffer[i]);
      }
    }
  }

  WrTextureRtt *renderingTexture = wr_frame_buffer_get_output_texture(mResultFrameBuffer, 0);
  WrTextureRtt *outputTexture = wr_frame_buffer_get_output_texture(mResultFrameBuffer, 1);
  wr_frame_buffer_delete(mResultFrameBuffer);
  wr_texture_delete(WR_TEXTURE(renderingTexture));
  wr_texture_delete(WR_TEXTURE(outputTexture));

  wr_texture_delete(WR_TEXTURE(mNoiseMaskTexture));
  mNoiseMaskTexture = NULL;

  WbWrenOpenGlContext::doneWren();
}

void WbWrenCamera::setupCamera(int index, int width, int height) {
  const bool isRangeFinderOrLidar = mType != 'c';

  mCamera[index] = wr_camera_new();
  wr_camera_set_flip_y(mCamera[index], true);
  wr_transform_attach_child(mNode, WR_NODE(mCamera[index]));

  if (isRangeFinderOrLidar)
    wr_camera_set_far(mCamera[index], mMaxRange);
  else
    wr_camera_set_far(mCamera[index], 10000.0f);

  mCameraViewport[index] = wr_viewport_new();
  wr_viewport_sync_aspect_ratio_with_camera(mCameraViewport[index], false);
  wr_viewport_set_camera(mCameraViewport[index], mCamera[index]);

  if (isRangeFinderOrLidar) {
    wr_viewport_set_visibility_mask(mCameraViewport[index], WbWrenRenderingContext::VM_WEBOTS_RANGE_CAMERA);
    wr_viewport_enable_skybox(mCameraViewport[index], false);
  } else
    wr_viewport_set_visibility_mask(mCameraViewport[index], WbWrenRenderingContext::VM_WEBOTS_CAMERA);

  if (mIsSpherical || mNumActivePostProcessingEffects) {
    mCameraFrameBuffer[index] = wr_frame_buffer_new();
    wr_frame_buffer_set_size(mCameraFrameBuffer[index], width, height);
    wr_frame_buffer_enable_depth_buffer(mCameraFrameBuffer[index], true);

    // depth must be rendered to texture for depth of field effect
    if (mFocusDistance > 0.0f && mFocusLength > 0.0f) {
      WrTextureRtt *depthRenderTexture = wr_texture_rtt_new();
      wr_texture_set_internal_format(WR_TEXTURE(depthRenderTexture), WR_TEXTURE_INTERNAL_FORMAT_DEPTH24_STENCIL8);
      wr_frame_buffer_set_depth_texture(mCameraFrameBuffer[index], depthRenderTexture);
    }

    WrTextureRtt *texture = wr_texture_rtt_new();
    wr_texture_set_internal_format(WR_TEXTURE(texture), mTextureFormat);
    wr_frame_buffer_append_output_texture(mCameraFrameBuffer[index], texture);
    wr_frame_buffer_setup(mCameraFrameBuffer[index]);

    wr_viewport_set_frame_buffer(mCameraViewport[index], mCameraFrameBuffer[index]);
  } else {
    wr_frame_buffer_enable_depth_buffer(mResultFrameBuffer, true);
    wr_viewport_set_frame_buffer(mCameraViewport[index], mResultFrameBuffer);
  }
}

void WbWrenCamera::setupSphericalSubCameras() {
  mSphericalFieldOfViewX = mFieldOfView;
  mSphericalFieldOfViewY = mSphericalFieldOfViewX * mHeight / mWidth;  // fovX -> fovY

  // only activate the needed cameras
  int lateralCameraNumber = 1;
  int verticalCameraNumber = 1;
  if (mSphericalFieldOfViewX > M_PI_2) {
    mIsCameraActive[CAMERA_ORIENTATION_RIGHT] = true;
    mIsCameraActive[CAMERA_ORIENTATION_LEFT] = true;
    lateralCameraNumber += 2;
  }
  if (mSphericalFieldOfViewY > 1.230959417) {  // 2.0*asin(1/sqrt(3)) // phi angle of the (Sqrt(3), Sqrt(3), Sqrt(3)) coordinate
    mIsCameraActive[CAMERA_ORIENTATION_UP] = true;
    mIsCameraActive[CAMERA_ORIENTATION_DOWN] = true;
    verticalCameraNumber += 2;
  }
  if (mSphericalFieldOfViewX > 3.0 * M_PI_2 || mSphericalFieldOfViewY > 3.0 * M_PI_2) {
    mIsCameraActive[CAMERA_ORIENTATION_BACK] = true;
    if (mSphericalFieldOfViewX > 3.0 * M_PI_2)
      lateralCameraNumber += 1;
    if (mSphericalFieldOfViewY > 3.0 * M_PI_2)
      verticalCameraNumber += 1;
  }

  if (verticalCameraNumber == 1) {
    // this coefficient is set to work even in the worse case (just before enabling top and bottom cameras)
    mSphericalFovYCorrectionCoefficient = 1.27;
    mSphericalFieldOfViewY *= mSphericalFovYCorrectionCoefficient;
  } else
    mSphericalFovYCorrectionCoefficient = 1.0;

  // compute the ideal resolution of the cameras
  // and bound it in order to not to explode if fov is too low
  if (mHeight > mWidth) {
    mSubCamerasResolutionY = (int)ceil(2.0 / tan(mSphericalFieldOfViewY / mHeight));
    mSubCamerasResolutionX = mSubCamerasResolutionY * mWidth / mHeight;
  } else {
    mSubCamerasResolutionX = (int)ceil(2.0 / tan(mSphericalFieldOfViewX / mWidth));
    mSubCamerasResolutionY = mSphericalFovYCorrectionCoefficient * mSubCamerasResolutionX * mHeight / mWidth;
  }

  if (lateralCameraNumber > verticalCameraNumber)
    mSubCamerasResolutionY = mSubCamerasResolutionY * lateralCameraNumber / verticalCameraNumber;
  else if (lateralCameraNumber < verticalCameraNumber)
    mSubCamerasResolutionX = mSubCamerasResolutionX * verticalCameraNumber / lateralCameraNumber;

  mSubCamerasResolutionX = qBound(1, mSubCamerasResolutionX, 2048);
  mSubCamerasResolutionY = qBound(1, mSubCamerasResolutionY, 2048);
}

void WbWrenCamera::setupPostProcessingEffects() {
  if (!mNumActivePostProcessingEffects)
    return;

  // lens distortion
  if (mIsLensDistortionEnabled)
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::lensDistortion(mWidth, mHeight, mTextureFormat));
  // depth of field
  if (mFocusDistance > 0.0f && mFocusLength > 0.0f)
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::depthOfField(
      mWidth, mHeight, cDofBlurTextureSize[0], cDofBlurTextureSize[1], mTextureFormat,
      WR_TEXTURE(wr_frame_buffer_get_output_texture(mCameraFrameBuffer[CAMERA_ORIENTATION_FRONT], 0)),
      WR_TEXTURE(wr_frame_buffer_get_depth_texture(mCameraFrameBuffer[CAMERA_ORIENTATION_FRONT]))));
  // motion blur
  if (mMotionBlurIntensity > 0.0f)
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::motionBlur(mWidth, mHeight, mTextureFormat));

  // hdr resolve
  if (mType == 'c')
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::hdrResolve(mWidth, mHeight));

  // anti-aliasing
  if (mAntiAliasing && mType == 'c')
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::smaa(mWidth, mHeight, mTextureFormat));
  // color noise
  if (mColorNoiseIntensity > 0.0f && mType == 'c')
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::colorNoise(mWidth, mHeight, mTextureFormat));
  // range noise
  if (mRangeNoiseIntensity > 0.0f && mType != 'c')
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::rangeNoise(mWidth, mHeight, mTextureFormat));
  // depth resolution
  if (mDepthResolution > 0.0f && mType != 'c')
    mPostProcessingEffects.append(WbWrenPostProcessingEffects::depthResolution(mWidth, mHeight, mTextureFormat));
  // noise masks
  if (mNoiseMaskTexture && mType == 'c')
    mPostProcessingEffects.append(
      WbWrenPostProcessingEffects::noiseMask(mWidth, mHeight, mTextureFormat, WR_TEXTURE(mNoiseMaskTexture)));

  for (int i = 0; i < mPostProcessingEffects.size(); ++i) {
    if (i == 0)
      wr_post_processing_effect_set_input_frame_buffer(mPostProcessingEffects[i], mCameraFrameBuffer[CAMERA_ORIENTATION_FRONT]);
    else
      wr_post_processing_effect_set_input_frame_buffer(mPostProcessingEffects[i], mResultFrameBuffer);
    wr_post_processing_effect_set_result_program(mPostProcessingEffects[i], WbWrenShaders::passThroughShader());
    wr_post_processing_effect_setup(mPostProcessingEffects[i]);
  }
}

void WbWrenCamera::setupSphericalPostProcessingEffect() {
  if (!mIsSpherical)
    return;
  mSphericalPostProcessingEffect =
    WbWrenPostProcessingEffects::sphericalCameraMerge(mWidth, mHeight, CAMERA_ORIENTATION_COUNT, mTextureFormat);
  wr_post_processing_effect_set_result_frame_buffer(mSphericalPostProcessingEffect, mResultFrameBuffer);
  wr_post_processing_effect_setup(mSphericalPostProcessingEffect);
}

void WbWrenCamera::setCamerasOrientations() {
  if (mIsCameraActive[CAMERA_ORIENTATION_RIGHT])
    wr_camera_apply_yaw(mCamera[CAMERA_ORIENTATION_RIGHT], -M_PI_2);
  if (mIsCameraActive[CAMERA_ORIENTATION_BACK])
    wr_camera_apply_yaw(mCamera[CAMERA_ORIENTATION_BACK], M_PI);
  if (mIsCameraActive[CAMERA_ORIENTATION_LEFT])
    wr_camera_apply_yaw(mCamera[CAMERA_ORIENTATION_LEFT], M_PI_2);
  if (mIsCameraActive[CAMERA_ORIENTATION_UP])
    wr_camera_apply_pitch(mCamera[CAMERA_ORIENTATION_UP], M_PI_2);
  if (mIsCameraActive[CAMERA_ORIENTATION_DOWN])
    wr_camera_apply_pitch(mCamera[CAMERA_ORIENTATION_DOWN], -M_PI_2);
}

void WbWrenCamera::setFovy(float fov) {
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_camera_set_fovy(mCamera[i], fov);
  }
}

void WbWrenCamera::setAspectRatio(float aspectRatio) {
  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_camera_set_aspect_ratio(mCamera[i], aspectRatio);
  }
}

void WbWrenCamera::applyPostProcessingEffectStack(int index) {
  assert(index >= 0 && index <= CAMERA_ORIENTATION_COUNT);

  // if this camera is spherical image we need is in result framebuffer if
  for (int i = 0; i < mPostProcessingEffects.size(); ++i) {
    WrPostProcessingEffectPass *firstPass = wr_post_processing_effect_get_first_pass(mPostProcessingEffects[i]);
    if (index == CAMERA_ORIENTATION_COUNT || i != 0)
      wr_post_processing_effect_pass_set_input_texture(firstPass, 0,
                                                       WR_TEXTURE(wr_frame_buffer_get_output_texture(mResultFrameBuffer, 0)));
    else
      wr_post_processing_effect_pass_set_input_texture(
        firstPass, 0, WR_TEXTURE(wr_frame_buffer_get_output_texture(mCameraFrameBuffer[index], 0)));
    WrPostProcessingEffectPass *hdrPass = wr_post_processing_effect_get_pass(mPostProcessingEffects[i], "hdrResolve");
    if (hdrPass)
      wr_post_processing_effect_pass_set_program_parameter(hdrPass, "exposure", reinterpret_cast<const char *>(&mExposure));
  }

  if (mIsLensDistortionEnabled) {
    float center[2] = {static_cast<float>(mLensDistortionCenter.x()), static_cast<float>(mLensDistortionCenter.y())};
    float radialDistortionCoeffs[2] = {static_cast<float>(mLensDistortionRadialCoeffs.x()),
                                       static_cast<float>(mLensDistortionRadialCoeffs.y())};
    float tangentialDistortionCoeffs[2] = {static_cast<float>(mLensDistortionTangentialCoeffs.x()),
                                           static_cast<float>(mLensDistortionTangentialCoeffs.y())};

    wr_shader_program_set_custom_uniform_value(WbWrenShaders::lensDistortionShader(), "center",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F, reinterpret_cast<const char *>(&center));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::lensDistortionShader(), "radialDistortionCoeffs",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F,
                                               reinterpret_cast<const char *>(&radialDistortionCoeffs));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::lensDistortionShader(), "tangentialDistortionCoeffs",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F,
                                               reinterpret_cast<const char *>(&tangentialDistortionCoeffs));
  }

  if (mFocusDistance > 0.0f && mFocusLength > 0.0f) {
    float cameraParams[2] = {wr_camera_get_near(mCamera[CAMERA_ORIENTATION_FRONT]),
                             wr_camera_get_far(mCamera[CAMERA_ORIENTATION_FRONT])};
    float dofParams[4] = {mFocusDistance - mFocusLength, mFocusDistance, mFocusDistance + mFocusLength, cDofFarBlurCutoff};

    wr_shader_program_set_custom_uniform_value(WbWrenShaders::depthOfFieldShader(), "cameraParams",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC4F,
                                               reinterpret_cast<const char *>(&cameraParams));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::depthOfFieldShader(), "dofParams",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC4F,
                                               reinterpret_cast<const char *>(&dofParams));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::depthOfFieldShader(), "blurTextureSize",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F,
                                               reinterpret_cast<const char *>(&cDofBlurTextureSize));
  }

  if (mMotionBlurIntensity > 0.0f) {
    float firstRender = mFirstRenderingCall ? 1.0f : 0.0f;

    wr_shader_program_set_custom_uniform_value(WbWrenShaders::motionBlurShader(), "firstRender",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&firstRender));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::motionBlurShader(), "intensity",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mMotionBlurIntensity));
  }

  if (mColorNoiseIntensity > 0.0f) {
    float time = WbSimulationState::instance()->time();

    wr_shader_program_set_custom_uniform_value(WbWrenShaders::colorNoiseShader(), "time", WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&time));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::colorNoiseShader(), "intensity",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mColorNoiseIntensity));
  }

  if (mRangeNoiseIntensity > 0.0f) {
    float time = WbSimulationState::instance()->time();

    wr_shader_program_set_custom_uniform_value(WbWrenShaders::rangeNoiseShader(), "time", WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&time));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::rangeNoiseShader(), "intensity",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mRangeNoiseIntensity));
  }

  if (mDepthResolution > 0.0f) {
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::depthResolutionShader(), "resolution",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                               reinterpret_cast<const char *>(&mDepthResolution));
  }

  if (mNoiseMaskTexture) {
    const float offset[2] = {static_cast<float>(WbRandom::nextUniform()), static_cast<float>(WbRandom::nextUniform())};
    const float factor[2] = {static_cast<float>(mNoiseMaskTextureFactor.x()), static_cast<float>(mNoiseMaskTextureFactor.y())};
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::noiseMaskShader(), "textureOffset",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F, reinterpret_cast<const char *>(offset));
    wr_shader_program_set_custom_uniform_value(WbWrenShaders::noiseMaskShader(), "textureFactor",
                                               WR_SHADER_PROGRAM_UNIFORM_TYPE_VEC2F, reinterpret_cast<const char *>(factor));
  }

  for (int i = 0; i < mPostProcessingEffects.size(); ++i)
    wr_post_processing_effect_apply(mPostProcessingEffects.at(i));
}

void WbWrenCamera::applySphericalPostProcessingEffect() {
  assert(mIsSpherical);

  const int isRangeFinderOrLidar = (mType != 'c');

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "rangeCamera",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_INT,
                                             reinterpret_cast<const char *>(&isRangeFinderOrLidar));

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "minRange",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT, reinterpret_cast<const char *>(&mMinRange));

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "maxRange",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT, reinterpret_cast<const char *>(&mMaxRange));

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "fovX",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                             reinterpret_cast<const char *>(&mSphericalFieldOfViewX));

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "fovY",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                             reinterpret_cast<const char *>(&mSphericalFieldOfViewY));

  wr_shader_program_set_custom_uniform_value(WbWrenShaders::mergeSphericalShader(), "fovYCorrectionCoefficient",
                                             WR_SHADER_PROGRAM_UNIFORM_TYPE_FLOAT,
                                             reinterpret_cast<const char *>(&mSphericalFovYCorrectionCoefficient));

  WrPostProcessingEffectPass *mergeSphericalPass =
    wr_post_processing_effect_get_pass(mSphericalPostProcessingEffect, "MergeSpherical");

  for (int i = CAMERA_ORIENTATION_FRONT; i < CAMERA_ORIENTATION_COUNT; ++i) {
    if (mIsCameraActive[i])
      wr_post_processing_effect_pass_set_input_texture(
        mergeSphericalPass, i, WR_TEXTURE(wr_frame_buffer_get_output_texture(mCameraFrameBuffer[i], 0)));
    else
      wr_post_processing_effect_pass_set_input_texture(mergeSphericalPass, i, NULL);
  }

  wr_post_processing_effect_apply(mSphericalPostProcessingEffect);
}