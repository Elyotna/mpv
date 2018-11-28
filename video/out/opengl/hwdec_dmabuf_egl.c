/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libavutil/common.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#include "config.h"

#include "video/out/gpu/hwdec.h"
#include "video/mp_image_pool.h"
#include "common.h"
#include "ra_gl.h"
#include "libmpv/render_gl.h"

#ifndef GL_OES_EGL_image
typedef void* GLeglImageOES;
#endif
#ifndef EGL_KHR_image
typedef void *EGLImageKHR;
#endif

/*#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#endif*/

struct priv {
    GLuint gl_texture;
    EGLImageKHR image;

    EGLImageKHR (EGLAPIENTRY *CreateImageKHR)(EGLDisplay, EGLContext,
                                              EGLenum, EGLClientBuffer,
                                              const EGLint *);
    EGLBoolean (EGLAPIENTRY *DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void (EGLAPIENTRY *EGLImageTargetTexture2DOES)(GLenum, GLeglImageOES);
};

static void uninit(struct ra_hwdec *hw)
{
}

static int init(struct ra_hwdec *hw)
{
    if (!ra_is_gl(hw->ra) || !eglGetCurrentContext())
        return -1;

    const char *exts = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (!exts) {
        MP_VERBOSE(hw, "eglQueryString failed: 0x%x.\n", eglGetError());
        return -1;
    }

    GL *gl = ra_gl_get(hw->ra);
    if (!strstr(exts, "EXT_image_dma_buf_import") ||
        !strstr(exts, "EGL_KHR_image_base") ||
        !strstr(gl->extensions, "GL_OES_EGL_image") ||
        !(gl->mpgl_caps & MPGL_CAP_TEX_RG)) {
        MP_VERBOSE(hw, "EGL/GL stack don't support enough features\n");
        return -1;
    }
    
    MP_VERBOSE(hw, "Using DMABUF EGL interop\n");

    return 0;
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    // EGL_KHR_image_base
    p->CreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    p->DestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    // GL_OES_EGL_image
    p->EGLImageTargetTexture2DOES =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!p->CreateImageKHR || !p->DestroyImageKHR ||
        !p->EGLImageTargetTexture2DOES)
        return -1;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    struct ra_imgfmt_desc desc = {0};
    struct mp_image layout = {0};

    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc))
        return -1;

    mp_image_set_params(&layout, &mapper->dst_params);

    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_2D, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    struct ra_tex_params params = {
        .dimensions = 2,
        .w = mapper->src_params.w,
        .h = mapper->src_params.h,
        .d = 1,
        .format = desc.planes[0],
        .render_src = true,
        .src_linear = true,
    };
    if (!params.format)
        return -1;

    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gl_texture);
    if (!mapper->tex[0])
        return -1;

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;
}

#define ADD_ATTRIB(name, value)                         \
    do {                                                \
    assert(num_attribs + 3 < MP_ARRAY_SIZE(attribs));   \
    attribs[num_attribs++] = (name);                    \
    attribs[num_attribs++] = (value);                   \
    attribs[num_attribs] = EGL_NONE;                    \
    } while(0)

// So that don't depend on libdrm just for one define
#define DRM_FORMAT_NV12 MKTAG('N', 'V', '1', '2')

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor* layer; 
    int attribs[20] = {EGL_NONE};
    int num_attribs = 0;
    
    desc = (AVDRMFrameDescriptor *)mapper->src->planes[0];
    layer = &desc->layers[0];

    ADD_ATTRIB(EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12);
    ADD_ATTRIB(EGL_WIDTH, mapper->src_params.w);
    ADD_ATTRIB(EGL_HEIGHT, mapper->src_params.h);
    ADD_ATTRIB(EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT);
    ADD_ATTRIB(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);
    ADD_ATTRIB(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
    ADD_ATTRIB(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
    
    for (int i = 0; i < layer->nb_planes; i++) {
        switch (i) {
        case 0:
            ADD_ATTRIB(EGL_DMA_BUF_PLANE0_FD_EXT, desc->objects[layer->planes[i].object_index].fd);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE0_OFFSET_EXT, layer->planes[i].offset);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE0_PITCH_EXT, layer->planes[i].pitch);
            break;
        case 1:
            ADD_ATTRIB(EGL_DMA_BUF_PLANE1_FD_EXT, desc->objects[layer->planes[i].object_index].fd);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE1_OFFSET_EXT, layer->planes[i].offset);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE1_PITCH_EXT, layer->planes[i].pitch);
            break;
        case 2:
            ADD_ATTRIB(EGL_DMA_BUF_PLANE2_FD_EXT, desc->objects[layer->planes[i].object_index].fd);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE2_OFFSET_EXT, layer->planes[i].offset);
            ADD_ATTRIB(EGL_DMA_BUF_PLANE2_PITCH_EXT, layer->planes[i].pitch);
            break;
        default:
            return -1;
        }
    }

    p->image = p->CreateImageKHR(eglGetCurrentDisplay(),
            EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    gl->BindTexture(GL_TEXTURE_2D, p->gl_texture);
    p->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, p->image);
    gl->BindTexture(GL_TEXTURE_2D, 0);
    

    return 0;

err:
    return -1;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct priv *p = mapper->priv;

    p->DestroyImageKHR(eglGetCurrentDisplay(), &p->image);
}

const struct ra_hwdec_driver ra_hwdec_drmprime_egl = {
    .name = "drmprime-egl",
    .priv_size = 0,
    .imgfmts = {IMGFMT_DRMPRIME, 0},
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};