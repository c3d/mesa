/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_private.h"

static bool
lookup_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_shader_bin *bin =
      anv_pipeline_cache_search(&device->blorp_shader_cache, key, key_size);
   if (!bin)
      return false;

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out = bin->prog_data;

   return true;
}

static void
upload_blorp_shader(struct blorp_context *blorp,
                    const void *key, uint32_t key_size,
                    const void *kernel, uint32_t kernel_size,
                    const struct brw_stage_prog_data *prog_data,
                    uint32_t prog_data_size,
                    uint32_t *kernel_out, void *prog_data_out)
{
   struct anv_device *device = blorp->driver_ctx;

   /* The blorp cache must be a real cache */
   assert(device->blorp_shader_cache.cache);

   struct anv_pipeline_bind_map bind_map = {
      .surface_count = 0,
      .sampler_count = 0,
   };

   struct anv_shader_bin *bin =
      anv_pipeline_cache_upload_kernel(&device->blorp_shader_cache,
                                       key, key_size, kernel, kernel_size,
                                       prog_data, prog_data_size, &bind_map);

   /* The cache already has a reference and it's not going anywhere so there
    * is no need to hold a second reference.
    */
   anv_shader_bin_unref(device, bin);

   *kernel_out = bin->kernel.offset;
   *(const struct brw_stage_prog_data **)prog_data_out = bin->prog_data;
}

void
anv_device_init_blorp(struct anv_device *device)
{
   anv_pipeline_cache_init(&device->blorp_shader_cache, device, true);
   blorp_init(&device->blorp, device, &device->isl_dev);
   device->blorp.compiler = device->instance->physicalDevice.compiler;
   device->blorp.mocs.tex = device->default_mocs;
   device->blorp.mocs.rb = device->default_mocs;
   device->blorp.mocs.vb = device->default_mocs;
   device->blorp.lookup_shader = lookup_blorp_shader;
   device->blorp.upload_shader = upload_blorp_shader;
   switch (device->info.gen) {
   case 7:
      if (device->info.is_haswell) {
         device->blorp.exec = gen75_blorp_exec;
      } else {
         device->blorp.exec = gen7_blorp_exec;
      }
      break;
   case 8:
      device->blorp.exec = gen8_blorp_exec;
      break;
   case 9:
      device->blorp.exec = gen9_blorp_exec;
      break;
   default:
      unreachable("Unknown hardware generation");
   }
}

void
anv_device_finish_blorp(struct anv_device *device)
{
   blorp_finish(&device->blorp);
   anv_pipeline_cache_finish(&device->blorp_shader_cache);
}

static void
get_blorp_surf_for_anv_buffer(struct anv_device *device,
                              struct anv_buffer *buffer, uint64_t offset,
                              uint32_t width, uint32_t height,
                              uint32_t row_pitch, enum isl_format format,
                              struct blorp_surf *blorp_surf,
                              struct isl_surf *isl_surf)
{
   const struct isl_format_layout *fmtl =
      isl_format_get_layout(format);

   /* ASTC is the only format which doesn't support linear layouts.
    * Create an equivalently sized surface with ISL to get around this.
    */
   if (fmtl->txc == ISL_TXC_ASTC) {
      /* Use an equivalently sized format */
      format = ISL_FORMAT_R32G32B32A32_UINT;
      assert(fmtl->bpb == isl_format_get_layout(format)->bpb);

      /* Shrink the dimensions for the new format */
      width = DIV_ROUND_UP(width, fmtl->bw);
      height = DIV_ROUND_UP(height, fmtl->bh);
   }

   *blorp_surf = (struct blorp_surf) {
      .surf = isl_surf,
      .addr = {
         .buffer = buffer->bo,
         .offset = buffer->offset + offset,
      },
   };

   isl_surf_init(&device->isl_dev, isl_surf,
                 .dim = ISL_SURF_DIM_2D,
                 .format = format,
                 .width = width,
                 .height = height,
                 .depth = 1,
                 .levels = 1,
                 .array_len = 1,
                 .samples = 1,
                 .min_pitch = row_pitch,
                 .usage = ISL_SURF_USAGE_TEXTURE_BIT |
                          ISL_SURF_USAGE_RENDER_TARGET_BIT,
                 .tiling_flags = ISL_TILING_LINEAR_BIT);
   assert(isl_surf->row_pitch == row_pitch);
}

static void
get_blorp_surf_for_anv_image(const struct anv_image *image,
                             VkImageAspectFlags aspect,
                             struct blorp_surf *blorp_surf)
{
   const struct anv_surface *surface =
      anv_image_get_surface_for_aspect_mask(image, aspect);

   *blorp_surf = (struct blorp_surf) {
      .surf = &surface->isl,
      .addr = {
         .buffer = image->bo,
         .offset = image->offset + surface->offset,
      },
   };
}

void anv_CmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      VkOffset3D srcOffset =
         anv_sanitize_image_offset(src_image->type, pRegions[r].srcOffset);
      VkOffset3D dstOffset =
         anv_sanitize_image_offset(dst_image->type, pRegions[r].dstOffset);
      VkExtent3D extent =
         anv_sanitize_image_extent(src_image->type, pRegions[r].extent);

      unsigned dst_base_layer, layer_count;
      if (dst_image->type == VK_IMAGE_TYPE_3D) {
         dst_base_layer = pRegions[r].dstOffset.z;
         layer_count = pRegions[r].extent.depth;
      } else {
         dst_base_layer = pRegions[r].dstSubresource.baseArrayLayer;
         layer_count = pRegions[r].dstSubresource.layerCount;
      }

      unsigned src_base_layer;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         src_base_layer = pRegions[r].srcOffset.z;
      } else {
         src_base_layer = pRegions[r].srcSubresource.baseArrayLayer;
         assert(pRegions[r].srcSubresource.layerCount == layer_count);
      }

      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);

      uint32_t a;
      for_each_bit(a, pRegions[r].dstSubresource.aspectMask) {
         VkImageAspectFlagBits aspect = (1 << a);

         struct blorp_surf src_surf, dst_surf;
         get_blorp_surf_for_anv_image(src_image, aspect, &src_surf);
         get_blorp_surf_for_anv_image(dst_image, aspect, &dst_surf);

         for (unsigned i = 0; i < layer_count; i++) {
            blorp_copy(&batch, &src_surf, pRegions[r].srcSubresource.mipLevel,
                       src_base_layer + i,
                       &dst_surf, pRegions[r].dstSubresource.mipLevel,
                       dst_base_layer + i,
                       srcOffset.x, srcOffset.y,
                       dstOffset.x, dstOffset.y,
                       extent.width, extent.height);
         }
      }
   }

   blorp_batch_finish(&batch);
}

static void
copy_buffer_to_image(struct anv_cmd_buffer *cmd_buffer,
                     struct anv_buffer *anv_buffer,
                     struct anv_image *anv_image,
                     uint32_t regionCount,
                     const VkBufferImageCopy* pRegions,
                     bool buffer_to_image)
{
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct {
      struct blorp_surf surf;
      uint32_t level;
      VkOffset3D offset;
   } image, buffer, *src, *dst;

   buffer.level = 0;
   buffer.offset = (VkOffset3D) { 0, 0, 0 };

   if (buffer_to_image) {
      src = &buffer;
      dst = &image;
   } else {
      src = &image;
      dst = &buffer;
   }

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;

      get_blorp_surf_for_anv_image(anv_image, aspect, &image.surf);
      image.offset =
         anv_sanitize_image_offset(anv_image->type, pRegions[r].imageOffset);
      image.level = pRegions[r].imageSubresource.mipLevel;

      VkExtent3D extent =
         anv_sanitize_image_extent(anv_image->type, pRegions[r].imageExtent);
      if (anv_image->type != VK_IMAGE_TYPE_3D) {
         image.offset.z = pRegions[r].imageSubresource.baseArrayLayer;
         extent.depth = pRegions[r].imageSubresource.layerCount;
      }

      const enum isl_format buffer_format =
         anv_get_isl_format(&cmd_buffer->device->info, anv_image->vk_format,
                            aspect, VK_IMAGE_TILING_LINEAR);

      const VkExtent3D bufferImageExtent = {
         .width  = pRegions[r].bufferRowLength ?
                   pRegions[r].bufferRowLength : extent.width,
         .height = pRegions[r].bufferImageHeight ?
                   pRegions[r].bufferImageHeight : extent.height,
      };

      const struct isl_format_layout *buffer_fmtl =
         isl_format_get_layout(buffer_format);

      const uint32_t buffer_row_pitch =
         DIV_ROUND_UP(bufferImageExtent.width, buffer_fmtl->bw) *
         (buffer_fmtl->bpb / 8);

      const uint32_t buffer_layer_stride =
         DIV_ROUND_UP(bufferImageExtent.height, buffer_fmtl->bh) *
         buffer_row_pitch;

      struct isl_surf buffer_isl_surf;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    anv_buffer, pRegions[r].bufferOffset,
                                    extent.width, extent.height,
                                    buffer_row_pitch, buffer_format,
                                    &buffer.surf, &buffer_isl_surf);

      for (unsigned z = 0; z < extent.depth; z++) {
         blorp_copy(&batch, &src->surf, src->level, src->offset.z,
                    &dst->surf, dst->level, dst->offset.z,
                    src->offset.x, src->offset.y, dst->offset.x, dst->offset.y,
                    extent.width, extent.height);

         image.offset.z++;
         buffer.surf.addr.offset += buffer_layer_stride;
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   copy_buffer_to_image(cmd_buffer, src_buffer, dst_image,
                        regionCount, pRegions, true);
}

void anv_CmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   copy_buffer_to_image(cmd_buffer, dst_buffer, src_image,
                        regionCount, pRegions, false);
}

static bool
flip_coords(unsigned *src0, unsigned *src1, unsigned *dst0, unsigned *dst1)
{
   bool flip = false;
   if (*src0 > *src1) {
      unsigned tmp = *src0;
      *src0 = *src1;
      *src1 = tmp;
      flip = !flip;
   }

   if (*dst0 > *dst1) {
      unsigned tmp = *dst0;
      *dst0 = *dst1;
      *dst1 = tmp;
      flip = !flip;
   }

   return flip;
}

void anv_CmdBlitImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkFilter                                    filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_surf src, dst;

   uint32_t gl_filter;
   switch (filter) {
   case VK_FILTER_NEAREST:
      gl_filter = 0x2600; /* GL_NEAREST */
      break;
   case VK_FILTER_LINEAR:
      gl_filter = 0x2601; /* GL_LINEAR */
      break;
   default:
      unreachable("Invalid filter");
   }

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      const VkImageSubresourceLayers *src_res = &pRegions[r].srcSubresource;
      const VkImageSubresourceLayers *dst_res = &pRegions[r].dstSubresource;

      get_blorp_surf_for_anv_image(src_image, src_res->aspectMask, &src);
      get_blorp_surf_for_anv_image(dst_image, dst_res->aspectMask, &dst);

      struct anv_format src_format =
         anv_get_format(&cmd_buffer->device->info, src_image->vk_format,
                        src_res->aspectMask, src_image->tiling);
      struct anv_format dst_format =
         anv_get_format(&cmd_buffer->device->info, dst_image->vk_format,
                        dst_res->aspectMask, dst_image->tiling);

      unsigned dst_start, dst_end;
      if (dst_image->type == VK_IMAGE_TYPE_3D) {
         assert(dst_res->baseArrayLayer == 0);
         dst_start = pRegions[r].dstOffsets[0].z;
         dst_end = pRegions[r].dstOffsets[1].z;
      } else {
         dst_start = dst_res->baseArrayLayer;
         dst_end = dst_start + dst_res->layerCount;
      }

      unsigned src_start, src_end;
      if (src_image->type == VK_IMAGE_TYPE_3D) {
         assert(src_res->baseArrayLayer == 0);
         src_start = pRegions[r].srcOffsets[0].z;
         src_end = pRegions[r].srcOffsets[1].z;
      } else {
         src_start = src_res->baseArrayLayer;
         src_end = src_start + src_res->layerCount;
      }

      bool flip_z = flip_coords(&src_start, &src_end, &dst_start, &dst_end);
      float src_z_step = (float)(src_end + 1 - src_start) /
                         (float)(dst_end + 1 - dst_start);

      if (flip_z) {
         src_start = src_end;
         src_z_step *= -1;
      }

      unsigned src_x0 = pRegions[r].srcOffsets[0].x;
      unsigned src_x1 = pRegions[r].srcOffsets[1].x;
      unsigned dst_x0 = pRegions[r].dstOffsets[0].x;
      unsigned dst_x1 = pRegions[r].dstOffsets[1].x;
      bool flip_x = flip_coords(&src_x0, &src_x1, &dst_x0, &dst_x1);

      unsigned src_y0 = pRegions[r].srcOffsets[0].y;
      unsigned src_y1 = pRegions[r].srcOffsets[1].y;
      unsigned dst_y0 = pRegions[r].dstOffsets[0].y;
      unsigned dst_y1 = pRegions[r].dstOffsets[1].y;
      bool flip_y = flip_coords(&src_y0, &src_y1, &dst_y0, &dst_y1);

      const unsigned num_layers = dst_end - dst_start;
      for (unsigned i = 0; i < num_layers; i++) {
         unsigned dst_z = dst_start + i;
         unsigned src_z = src_start + i * src_z_step;

         blorp_blit(&batch, &src, src_res->mipLevel, src_z,
                    src_format.isl_format, src_format.swizzle,
                    &dst, dst_res->mipLevel, dst_z,
                    dst_format.isl_format, dst_format.swizzle,
                    src_x0, src_y0, src_x1, src_y1,
                    dst_x0, dst_y0, dst_x1, dst_y1,
                    gl_filter, flip_x, flip_y);
      }

   }

   blorp_batch_finish(&batch);
}

static enum isl_format
isl_format_for_size(unsigned size_B)
{
   switch (size_B) {
   case 1:  return ISL_FORMAT_R8_UINT;
   case 2:  return ISL_FORMAT_R8G8_UINT;
   case 4:  return ISL_FORMAT_R8G8B8A8_UINT;
   case 8:  return ISL_FORMAT_R16G16B16A16_UINT;
   case 16: return ISL_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Not a power-of-two format size");
   }
}

static void
do_buffer_copy(struct blorp_batch *batch,
               struct anv_bo *src, uint64_t src_offset,
               struct anv_bo *dst, uint64_t dst_offset,
               int width, int height, int block_size)
{
   struct anv_device *device = batch->blorp->driver_ctx;

   /* The actual format we pick doesn't matter as blorp will throw it away.
    * The only thing that actually matters is the size.
    */
   enum isl_format format = isl_format_for_size(block_size);

   struct isl_surf surf;
   isl_surf_init(&device->isl_dev, &surf,
                 .dim = ISL_SURF_DIM_2D,
                 .format = format,
                 .width = width,
                 .height = height,
                 .depth = 1,
                 .levels = 1,
                 .array_len = 1,
                 .samples = 1,
                 .usage = ISL_SURF_USAGE_TEXTURE_BIT |
                          ISL_SURF_USAGE_RENDER_TARGET_BIT,
                 .tiling_flags = ISL_TILING_LINEAR_BIT);
   assert(surf.row_pitch == width * block_size);

   struct blorp_surf src_blorp_surf = {
      .surf = &surf,
      .addr = {
         .buffer = src,
         .offset = src_offset,
      },
   };

   struct blorp_surf dst_blorp_surf = {
      .surf = &surf,
      .addr = {
         .buffer = dst,
         .offset = dst_offset,
      },
   };

   blorp_copy(batch, &src_blorp_surf, 0, 0, &dst_blorp_surf, 0, 0,
              0, 0, 0, 0, width, height);
}

/**
 * Returns the greatest common divisor of a and b that is a power of two.
 */
static inline uint64_t
gcd_pow2_u64(uint64_t a, uint64_t b)
{
   assert(a > 0 || b > 0);

   unsigned a_log2 = ffsll(a) - 1;
   unsigned b_log2 = ffsll(b) - 1;

   /* If either a or b is 0, then a_log2 or b_log2 till be UINT_MAX in which
    * case, the MIN2() will take the other one.  If both are 0 then we will
    * hit the assert above.
    */
   return 1 << MIN2(a_log2, b_log2);
}

/* This is maximum possible width/height our HW can handle */
#define MAX_SURFACE_DIM (1ull << 14)

void anv_CmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dst_offset = dst_buffer->offset + pRegions[r].dstOffset;
      uint64_t copy_size = pRegions[r].size;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int bs = 16;
      bs = gcd_pow2_u64(bs, src_offset);
      bs = gcd_pow2_u64(bs, dst_offset);
      bs = gcd_pow2_u64(bs, pRegions[r].size);

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = MAX_SURFACE_DIM * MAX_SURFACE_DIM * bs;
      while (copy_size >= max_copy_size) {
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        MAX_SURFACE_DIM, MAX_SURFACE_DIM, bs);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dst_offset += max_copy_size;
      }

      /* Now make a max-width copy */
      uint64_t height = copy_size / (MAX_SURFACE_DIM * bs);
      assert(height < MAX_SURFACE_DIM);
      if (height != 0) {
         uint64_t rect_copy_size = height * MAX_SURFACE_DIM * bs;
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        MAX_SURFACE_DIM, height, bs);
         copy_size -= rect_copy_size;
         src_offset += rect_copy_size;
         dst_offset += rect_copy_size;
      }

      /* Finally, make a small copy to finish it off */
      if (copy_size != 0) {
         do_buffer_copy(&batch, src_buffer->bo, src_offset,
                        dst_buffer->bo, dst_offset,
                        copy_size / bs, 1, bs);
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdUpdateBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                dataSize,
    const void*                                 pData)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   /* We can't quite grab a full block because the state stream needs a
    * little data at the top to build its linked list.
    */
   const uint32_t max_update_size =
      cmd_buffer->device->dynamic_state_block_pool.block_size - 64;

   assert(max_update_size < MAX_SURFACE_DIM * 4);

   while (dataSize) {
      const uint32_t copy_size = MIN2(dataSize, max_update_size);

      struct anv_state tmp_data =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, copy_size, 64);

      memcpy(tmp_data.map, pData, copy_size);

      int bs = 16;
      bs = gcd_pow2_u64(bs, dstOffset);
      bs = gcd_pow2_u64(bs, copy_size);

      do_buffer_copy(&batch,
                     &cmd_buffer->device->dynamic_state_block_pool.bo,
                     tmp_data.offset,
                     dst_buffer->bo, dst_buffer->offset + dstOffset,
                     copy_size / bs, 1, bs);

      dataSize -= copy_size;
      dstOffset += copy_size;
      pData = (void *)pData + copy_size;
   }

   blorp_batch_finish(&batch);
}

void anv_CmdFillBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, dstBuffer);
   struct blorp_surf surf;
   struct isl_surf isl_surf;

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   if (fillSize == VK_WHOLE_SIZE) {
      fillSize = dst_buffer->size - dstOffset;
      /* Make sure fillSize is a multiple of 4 */
      fillSize &= ~3ull;
   }

   /* First, we compute the biggest format that can be used with the
    * given offsets and size.
    */
   int bs = 16;
   bs = gcd_pow2_u64(bs, dstOffset);
   bs = gcd_pow2_u64(bs, fillSize);
   enum isl_format isl_format = isl_format_for_size(bs);

   union isl_color_value color = {
      .u32 = { data, data, data, data },
   };

   const uint64_t max_fill_size = MAX_SURFACE_DIM * MAX_SURFACE_DIM * bs;
   while (fillSize >= max_fill_size) {
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    MAX_SURFACE_DIM, MAX_SURFACE_DIM,
                                    MAX_SURFACE_DIM * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, MAX_SURFACE_DIM, MAX_SURFACE_DIM,
                  color, NULL);
      fillSize -= max_fill_size;
      dstOffset += max_fill_size;
   }

   uint64_t height = fillSize / (MAX_SURFACE_DIM * bs);
   assert(height < MAX_SURFACE_DIM);
   if (height != 0) {
      const uint64_t rect_fill_size = height * MAX_SURFACE_DIM * bs;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    MAX_SURFACE_DIM, height,
                                    MAX_SURFACE_DIM * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, MAX_SURFACE_DIM, height,
                  color, NULL);
      fillSize -= rect_fill_size;
      dstOffset += rect_fill_size;
   }

   if (fillSize != 0) {
      const uint32_t width = fillSize / bs;
      get_blorp_surf_for_anv_buffer(cmd_buffer->device,
                                    dst_buffer, dstOffset,
                                    width, 1,
                                    width * bs, isl_format,
                                    &surf, &isl_surf);

      blorp_clear(&batch, &surf, isl_format, ISL_SWIZZLE_IDENTITY,
                  0, 0, 1, 0, 0, width, 1,
                  color, NULL);
   }

   blorp_batch_finish(&batch);
}

void anv_CmdClearColorImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     _image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, image, _image);

   static const bool color_write_disable[4] = { false, false, false, false };

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   union isl_color_value clear_color;
   memcpy(clear_color.u32, pColor->uint32, sizeof(pColor->uint32));

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_COLOR_BIT, &surf);

   for (unsigned r = 0; r < rangeCount; r++) {
      if (pRanges[r].aspectMask == 0)
         continue;

      assert(pRanges[r].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);

      struct anv_format src_format =
         anv_get_format(&cmd_buffer->device->info, image->vk_format,
                        VK_IMAGE_ASPECT_COLOR_BIT, image->tiling);

      unsigned base_layer = pRanges[r].baseArrayLayer;
      unsigned layer_count = pRanges[r].layerCount;

      for (unsigned i = 0; i < pRanges[r].levelCount; i++) {
         const unsigned level = pRanges[r].baseMipLevel + i;
         const unsigned level_width = anv_minify(image->extent.width, level);
         const unsigned level_height = anv_minify(image->extent.height, level);

         if (image->type == VK_IMAGE_TYPE_3D) {
            base_layer = 0;
            layer_count = anv_minify(image->extent.depth, level);
         }

         blorp_clear(&batch, &surf,
                     src_format.isl_format, src_format.swizzle,
                     level, base_layer, layer_count,
                     0, 0, level_width, level_height,
                     clear_color, color_write_disable);
      }
   }

   blorp_batch_finish(&batch);
}

void anv_CmdClearDepthStencilImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     image_h,
    VkImageLayout                               imageLayout,
    const VkClearDepthStencilValue*             pDepthStencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, image, image_h);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   struct blorp_surf depth, stencil;
   if (image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   &depth);
   } else {
      memset(&depth, 0, sizeof(depth));
   }

   if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                   &stencil);
   } else {
      memset(&stencil, 0, sizeof(stencil));
   }

   for (unsigned r = 0; r < rangeCount; r++) {
      if (pRanges[r].aspectMask == 0)
         continue;

      bool clear_depth = pRanges[r].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT;
      bool clear_stencil = pRanges[r].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;

      unsigned base_layer = pRanges[r].baseArrayLayer;
      unsigned layer_count = pRanges[r].layerCount;

      for (unsigned i = 0; i < pRanges[r].levelCount; i++) {
         const unsigned level = pRanges[r].baseMipLevel + i;
         const unsigned level_width = anv_minify(image->extent.width, level);
         const unsigned level_height = anv_minify(image->extent.height, level);

         if (image->type == VK_IMAGE_TYPE_3D)
            layer_count = anv_minify(image->extent.depth, level);

         blorp_clear_depth_stencil(&batch, &depth, &stencil,
                                   level, base_layer, layer_count,
                                   0, 0, level_width, level_height,
                                   clear_depth, pDepthStencil->depth,
                                   clear_stencil ? 0xff : 0,
                                   pDepthStencil->stencil);
      }
   }

   blorp_batch_finish(&batch);
}

static void
clear_color_attachment(struct anv_cmd_buffer *cmd_buffer,
                       struct blorp_batch *batch,
                       const VkClearAttachment *attachment,
                       uint32_t rectCount, const VkClearRect *pRects)
{
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_subpass *subpass = cmd_buffer->state.subpass;
   const uint32_t att = attachment->colorAttachment;
   const struct anv_image_view *iview =
      fb->attachments[subpass->color_attachments[att]];
   const struct anv_image *image = iview->image;

   struct blorp_surf surf;
   get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_COLOR_BIT, &surf);

   union isl_color_value clear_color;
   memcpy(clear_color.u32, attachment->clearValue.color.uint32,
          sizeof(clear_color.u32));

   static const bool color_write_disable[4] = { false, false, false, false };

   for (uint32_t r = 0; r < rectCount; ++r) {
      const VkOffset2D offset = pRects[r].rect.offset;
      const VkExtent2D extent = pRects[r].rect.extent;
      blorp_clear(batch, &surf, iview->isl.format, iview->isl.swizzle,
                  iview->isl.base_level,
                  iview->isl.base_array_layer + pRects[r].baseArrayLayer,
                  pRects[r].layerCount,
                  offset.x, offset.y,
                  offset.x + extent.width, offset.y + extent.height,
                  clear_color, color_write_disable);
   }
}

static void
clear_depth_stencil_attachment(struct anv_cmd_buffer *cmd_buffer,
                               struct blorp_batch *batch,
                               const VkClearAttachment *attachment,
                               uint32_t rectCount, const VkClearRect *pRects)
{
   const struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   const struct anv_subpass *subpass = cmd_buffer->state.subpass;
   const struct anv_image_view *iview =
      fb->attachments[subpass->depth_stencil_attachment];
   const struct anv_image *image = iview->image;

   bool clear_depth = attachment->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT;
   bool clear_stencil = attachment->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;

   struct blorp_surf depth, stencil;
   if (clear_depth) {
      get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                   &depth);
   } else {
      memset(&depth, 0, sizeof(depth));
   }

   if (clear_stencil) {
      get_blorp_surf_for_anv_image(image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                   &stencil);
   } else {
      memset(&stencil, 0, sizeof(stencil));
   }

   for (uint32_t r = 0; r < rectCount; ++r) {
      const VkOffset2D offset = pRects[r].rect.offset;
      const VkExtent2D extent = pRects[r].rect.extent;
      VkClearDepthStencilValue value = attachment->clearValue.depthStencil;
      blorp_clear_depth_stencil(batch, &depth, &stencil,
                                iview->isl.base_level,
                                iview->isl.base_array_layer +
                                   pRects[r].baseArrayLayer,
                                pRects[r].layerCount,
                                offset.x, offset.y,
                                offset.x + extent.width,
                                offset.y + extent.height,
                                clear_depth, value.depth,
                                clear_stencil ? 0xff : 0, value.stencil);
   }
}

void anv_CmdClearAttachments(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    attachmentCount,
    const VkClearAttachment*                    pAttachments,
    uint32_t                                    rectCount,
    const VkClearRect*                          pRects)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   /* Because this gets called within a render pass, we tell blorp not to
    * trash our depth and stencil buffers.
    */
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer,
                    BLORP_BATCH_NO_EMIT_DEPTH_STENCIL);

   for (uint32_t a = 0; a < attachmentCount; ++a) {
      if (pAttachments[a].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
         clear_color_attachment(cmd_buffer, &batch,
                                &pAttachments[a],
                                rectCount, pRects);
      } else {
         clear_depth_stencil_attachment(cmd_buffer, &batch,
                                        &pAttachments[a],
                                        rectCount, pRects);
      }
   }

   blorp_batch_finish(&batch);
}

static bool
subpass_needs_clear(const struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   uint32_t ds = cmd_state->subpass->depth_stencil_attachment;

   for (uint32_t i = 0; i < cmd_state->subpass->color_count; ++i) {
      uint32_t a = cmd_state->subpass->color_attachments[i];
      if (cmd_state->attachments[a].pending_clear_aspects) {
         return true;
      }
   }

   if (ds != VK_ATTACHMENT_UNUSED &&
       cmd_state->attachments[ds].pending_clear_aspects) {
      return true;
   }

   return false;
}

void
anv_cmd_buffer_clear_subpass(struct anv_cmd_buffer *cmd_buffer)
{
   const struct anv_cmd_state *cmd_state = &cmd_buffer->state;

   if (!subpass_needs_clear(cmd_buffer))
      return;

   /* Because this gets called within a render pass, we tell blorp not to
    * trash our depth and stencil buffers.
    */
   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer,
                    BLORP_BATCH_NO_EMIT_DEPTH_STENCIL);

   VkClearRect clear_rect = {
      .rect = cmd_buffer->state.render_area,
      .baseArrayLayer = 0,
      .layerCount = cmd_buffer->state.framebuffer->layers,
   };

   for (uint32_t i = 0; i < cmd_state->subpass->color_count; ++i) {
      const uint32_t a = cmd_state->subpass->color_attachments[i];

      if (!cmd_state->attachments[a].pending_clear_aspects)
         continue;

      assert(cmd_state->attachments[a].pending_clear_aspects ==
             VK_IMAGE_ASPECT_COLOR_BIT);

      VkClearAttachment clear_att = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = i, /* Use attachment index relative to subpass */
         .clearValue = cmd_state->attachments[a].clear_value,
      };

      clear_color_attachment(cmd_buffer, &batch, &clear_att, 1, &clear_rect);

      cmd_state->attachments[a].pending_clear_aspects = 0;
   }

   const uint32_t ds = cmd_state->subpass->depth_stencil_attachment;

   if (ds != VK_ATTACHMENT_UNUSED &&
       cmd_state->attachments[ds].pending_clear_aspects) {

      VkClearAttachment clear_att = {
         .aspectMask = cmd_state->attachments[ds].pending_clear_aspects,
         .clearValue = cmd_state->attachments[ds].clear_value,
      };

      clear_depth_stencil_attachment(cmd_buffer, &batch,
                                     &clear_att, 1, &clear_rect);

      cmd_state->attachments[ds].pending_clear_aspects = 0;
   }

   blorp_batch_finish(&batch);
}

static void
resolve_image(struct blorp_batch *batch,
              const struct anv_image *src_image,
              uint32_t src_level, uint32_t src_layer,
              const struct anv_image *dst_image,
              uint32_t dst_level, uint32_t dst_layer,
              VkImageAspectFlags aspect_mask,
              uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y,
              uint32_t width, uint32_t height)
{
   assert(src_image->type == VK_IMAGE_TYPE_2D);
   assert(src_image->samples > 1);
   assert(dst_image->type == VK_IMAGE_TYPE_2D);
   assert(dst_image->samples == 1);

   uint32_t a;
   for_each_bit(a, aspect_mask) {
      VkImageAspectFlagBits aspect = 1 << a;

      struct blorp_surf src_surf, dst_surf;
      get_blorp_surf_for_anv_image(src_image, aspect, &src_surf);
      get_blorp_surf_for_anv_image(dst_image, aspect, &dst_surf);

      blorp_blit(batch,
                 &src_surf, src_level, src_layer,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 &dst_surf, dst_level, dst_layer,
                 ISL_FORMAT_UNSUPPORTED, ISL_SWIZZLE_IDENTITY,
                 src_x, src_y, src_x + width, src_y + height,
                 dst_x, dst_y, dst_x + width, dst_y + height,
                 0x2600 /* GL_NEAREST */, false, false);
   }
}

void anv_CmdResolveImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dst_image, dstImage);

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (uint32_t r = 0; r < regionCount; r++) {
      assert(pRegions[r].srcSubresource.aspectMask ==
             pRegions[r].dstSubresource.aspectMask);
      assert(pRegions[r].srcSubresource.layerCount ==
             pRegions[r].dstSubresource.layerCount);

      const uint32_t layer_count = pRegions[r].dstSubresource.layerCount;

      for (uint32_t layer = 0; layer < layer_count; layer++) {
         resolve_image(&batch,
                       src_image, pRegions[r].srcSubresource.mipLevel,
                       pRegions[r].srcSubresource.baseArrayLayer + layer,
                       dst_image, pRegions[r].dstSubresource.mipLevel,
                       pRegions[r].dstSubresource.baseArrayLayer + layer,
                       pRegions[r].dstSubresource.aspectMask,
                       pRegions[r].srcOffset.x, pRegions[r].srcOffset.y,
                       pRegions[r].dstOffset.x, pRegions[r].dstOffset.y,
                       pRegions[r].extent.width, pRegions[r].extent.height);
      }
   }

   blorp_batch_finish(&batch);
}

void
anv_cmd_buffer_resolve_subpass(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   struct anv_subpass *subpass = cmd_buffer->state.subpass;

   /* FINISHME(perf): Skip clears for resolve attachments.
    *
    * From the Vulkan 1.0 spec:
    *
    *    If the first use of an attachment in a render pass is as a resolve
    *    attachment, then the loadOp is effectively ignored as the resolve is
    *    guaranteed to overwrite all pixels in the render area.
    */

   if (!subpass->has_resolve)
      return;

   struct blorp_batch batch;
   blorp_batch_init(&cmd_buffer->device->blorp, &batch, cmd_buffer, 0);

   for (uint32_t i = 0; i < subpass->color_count; ++i) {
      uint32_t src_att = subpass->color_attachments[i];
      uint32_t dst_att = subpass->resolve_attachments[i];

      if (dst_att == VK_ATTACHMENT_UNUSED)
         continue;

      struct anv_image_view *src_iview = fb->attachments[src_att];
      struct anv_image_view *dst_iview = fb->attachments[dst_att];

      const VkRect2D render_area = cmd_buffer->state.render_area;

      assert(src_iview->aspect_mask == dst_iview->aspect_mask);
      resolve_image(&batch, src_iview->image,
                    src_iview->isl.base_level, src_iview->isl.base_array_layer,
                    dst_iview->image,
                    dst_iview->isl.base_level, dst_iview->isl.base_array_layer,
                    src_iview->aspect_mask,
                    render_area.offset.x, render_area.offset.y,
                    render_area.offset.x, render_area.offset.y,
                    render_area.extent.width, render_area.extent.height);
   }

   blorp_batch_finish(&batch);
}