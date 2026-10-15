/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

// BaseballMR: per-draw-buffer blending (GL 4.0 / GLES 3.2 core), for the VR passthrough coverage
// attachment, which blends differently from the EFB colour attachment.

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLBLENDEQUATIONSEPARATEIPROC)(GLuint buf, GLenum modeRGB,
                                                          GLenum modeAlpha);
typedef void(APIENTRYP PFNDOLBLENDFUNCSEPARATEIPROC)(GLuint buf, GLenum srcRGB, GLenum dstRGB,
                                                      GLenum srcAlpha, GLenum dstAlpha);

extern PFNDOLBLENDEQUATIONSEPARATEIPROC dolBlendEquationSeparatei;
extern PFNDOLBLENDFUNCSEPARATEIPROC dolBlendFuncSeparatei;

#define glBlendEquationSeparatei dolBlendEquationSeparatei
#define glBlendFuncSeparatei dolBlendFuncSeparatei
