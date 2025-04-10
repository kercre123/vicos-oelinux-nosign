/*
 * Copyright (c) 2016-2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "EGLImageBuffer.h"
#include <cutils/native_handle.h>
#include <gralloc_priv.h>
#include <ui/GraphicBuffer.h>
#include <map>
#include "EGLImageWrapper.h"
#include "glengine.h"
#define EGL_PROTECTED_CONTENT_EXT 0x32C0

//-----------------------------------------------------------------------------
EGLImageKHR create_eglImage(android::sp<android::GraphicBuffer> graphicBuffer)
//-----------------------------------------------------------------------------
{
  bool isProtected = (graphicBuffer->getUsage() & GRALLOC_USAGE_PROTECTED);
  EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                    isProtected ? EGL_PROTECTED_CONTENT_EXT : EGL_NONE,
                    isProtected ? EGL_TRUE : EGL_NONE, EGL_NONE};

  EGLImageKHR eglImage = eglCreateImageKHR(
      eglGetCurrentDisplay(), (EGLContext)EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
      (EGLClientBuffer)(graphicBuffer->getNativeBuffer()), attrs);

  return eglImage;
}

//-----------------------------------------------------------------------------
EGLImageBuffer::EGLImageBuffer(android::sp<android::GraphicBuffer> graphicBuffer)
//-----------------------------------------------------------------------------
{
  // this->graphicBuffer = graphicBuffer;
  this->eglImageID = create_eglImage(graphicBuffer);
  this->width = graphicBuffer->getWidth();
  this->height = graphicBuffer->getHeight();

  textureID = 0;
  renderbufferID = 0;
  framebufferID = 0;
}

//-----------------------------------------------------------------------------
EGLImageBufferBase::~EGLImageBufferBase()
//-----------------------------------------------------------------------------
{
  if (textureID != 0) {
    GL(glDeleteTextures(1, &textureID));
    textureID = 0;
  }

  if (renderbufferID != 0) {
    GL(glDeleteRenderbuffers(1, &renderbufferID));
    renderbufferID = 0;
  }

  if (framebufferID != 0) {
    GL(glDeleteFramebuffers(1, &framebufferID));
    framebufferID = 0;
  }

  // Delete the eglImage
  if (eglImageID != 0)
  {
      eglDestroyImageKHR(eglGetCurrentDisplay(), eglImageID);
      eglImageID = 0;
  }
}
