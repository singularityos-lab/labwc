#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/box.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "singularity-preview-unstable-v1-protocol.h"
#include "labwc.h"
#include "view.h"
#include "output.h"

struct singularity_preview_manager {
    struct wl_global *global;
};

struct singularity_preview_frame {
    struct wl_resource *resource;
    struct view *view;
    struct wl_listener view_destroy;
    uint32_t width, height, stride;
};

static bool preview_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("SINGULARITY_PREVIEW_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static void frame_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void frame_destroy(struct singularity_preview_frame *frame) {
    if (frame->view) {
        wl_list_remove(&frame->view_destroy.link);
    }
    free(frame);
}

static void frame_resource_destroy(struct wl_resource *resource) {
    struct singularity_preview_frame *frame = wl_resource_get_user_data(resource);
    frame_destroy(frame);
}

static void handle_view_destroy(struct wl_listener *listener, void *data) {
    struct singularity_preview_frame *frame = wl_container_of(listener, frame, view_destroy);
    zsingularity_preview_frame_v1_send_failed(frame->resource);
    frame->view = NULL;
    wl_list_remove(&frame->view_destroy.link);
    wl_list_init(&frame->view_destroy.link);
}

struct largest_surface_ctx {
    struct wlr_surface *surface;
    int area;
    int count;
};

static void find_largest_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
    (void)sx;
    (void)sy;
    struct largest_surface_ctx *ctx = data;
    if (!wlr_surface_has_buffer(surface)) {
        return;
    }
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) {
        return;
    }
    ctx->count++;
    int area = (int)texture->width * (int)texture->height;
    if (area >= ctx->area) {
        ctx->area = area;
        ctx->surface = surface;
    }
}

struct preview_render_ctx {
    struct wlr_render_pass *pass;
    double scale_x;
    double scale_y;
    int off_x;
    int off_y;
};

static void preview_render_surface(struct wlr_surface *surf, int sx, int sy, void *data) {
    struct preview_render_ctx *ctx = data;
    if (!wlr_surface_has_buffer(surf)) {
        return;
    }
    struct wlr_texture *texture = wlr_surface_get_texture(surf);
    if (!texture) {
        return;
    }
    int dw = (int)(surf->current.width * ctx->scale_x + 0.5);
    int dh = (int)(surf->current.height * ctx->scale_y + 0.5);
    if (dw <= 0 || dh <= 0) {
        return;
    }
    wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
        .texture = texture,
        .src_box = { .width = texture->width, .height = texture->height },
        .dst_box = {
            .x = (int)((sx - ctx->off_x) * ctx->scale_x + 0.5),
            .y = (int)((sy - ctx->off_y) * ctx->scale_y + 0.5),
            .width = dw,
            .height = dh,
        },
        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
    });
}

static bool composite_capture(struct view *view, struct wlr_buffer *buffer) {
    struct wlr_renderer *renderer = server.renderer;
    struct wlr_surface *surface = view->surface;
    struct output *output = view->output;
    if (!renderer || !surface) {
        return false;
    }
    if (!output || !output->wlr_output || !output->wlr_output->swapchain) {
        return false;
    }
    int base_w = view->current.width;
    int base_h = view->current.height;
    int off_x = 0;
    int off_y = 0;
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
    if (xdg != NULL) {
        struct wlr_box geo = xdg->geometry;
        if (geo.width > 0 && geo.height > 0) {
            base_w = geo.width;
            base_h = geo.height;
            off_x = geo.x;
            off_y = geo.y;
        }
    }
    if (base_w <= 0 || base_h <= 0) {
        base_w = (int)surface->current.width;
        base_h = (int)surface->current.height;
    }
    if (base_w <= 0 || base_h <= 0) {
        return false;
    }

    struct wlr_buffer *gpu = server_create_render_buffer(
        buffer->width, buffer->height, &output->wlr_output->swapchain->format);
    if (!gpu) {
        return false;
    }

    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, gpu, NULL);
    if (!pass) {
        wlr_buffer_drop(gpu);
        return false;
    }

    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .width = buffer->width, .height = buffer->height },
        .color = { .r = 0, .g = 0, .b = 0, .a = 0 }
    });

    struct preview_render_ctx ctx = {
        .pass = pass,
        .scale_x = (double)buffer->width / (double)base_w,
        .scale_y = (double)buffer->height / (double)base_h,
        .off_x = off_x,
        .off_y = off_y,
    };
    wlr_surface_for_each_surface(surface, preview_render_surface, &ctx);

    if (!wlr_render_pass_submit(pass)) {
        wlr_buffer_drop(gpu);
        return false;
    }

    struct wlr_texture *gpu_texture = wlr_texture_from_buffer(renderer, gpu);
    if (!gpu_texture) {
        wlr_buffer_drop(gpu);
        return false;
    }

    void *data;
    uint32_t format;
    size_t stride;
    bool ok = false;
    if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        struct wlr_texture_read_pixels_options opts = {
            .data = data,
            .format = format,
            .stride = (uint32_t)stride,
            .src_box = { .width = buffer->width, .height = buffer->height },
        };
        ok = wlr_texture_read_pixels(gpu_texture, &opts);
        wlr_buffer_end_data_ptr_access(buffer);
    }

    wlr_texture_destroy(gpu_texture);
    wlr_buffer_drop(gpu);
    return ok;
}

static void frame_handle_copy(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer_resource) {
    struct singularity_preview_frame *frame = wl_resource_get_user_data(resource);
    if (!frame || !frame->view) {
        wlr_log(WLR_ERROR, "[PREVIEW] Copy failed: Frame or view is NULL");
        zsingularity_preview_frame_v1_send_failed(resource);
        return;
    }

    struct view *view = frame->view;
    struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
    if (!buffer) {
        wlr_log(WLR_ERROR, "[PREVIEW] Copy failed: Could not get wlr_buffer from resource");
        zsingularity_preview_frame_v1_send_failed(resource);
        return;
    }

    struct wlr_renderer *renderer = server.renderer;
    if (!renderer) {
        wlr_log(WLR_ERROR, "[PREVIEW] Copy failed: Renderer is NULL");
        zsingularity_preview_frame_v1_send_failed(resource);
        return;
    }

    // Explicitly lock the buffer to prevent it from being freed during our access
    wlr_buffer_lock(buffer);

    struct wlr_surface *surface = view->surface;
    struct largest_surface_ctx best = { .surface = NULL, .area = 0, .count = 0 };
    if (surface) {
        wlr_surface_for_each_surface(surface, find_largest_surface, &best);
    }
    if (best.count <= 1 && best.surface) {
        struct wlr_texture *texture = wlr_surface_get_texture(best.surface);
        if (texture) {
            void *data;
            uint32_t format;
            size_t stride;

            int rw = (int)texture->width;
            int rh = (int)texture->height;
            if (buffer->width < rw) rw = buffer->width;
            if (buffer->height < rh) rh = buffer->height;

            if (rw > 0 && rh > 0 &&
                    wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
                struct wlr_texture_read_pixels_options options = {
                    .data = data,
                    .format = format,
                    .stride = (uint32_t)stride,
                    .src_box = { .width = rw, .height = rh },
                };
                if (wlr_texture_read_pixels(texture, &options)) {
                    wlr_buffer_end_data_ptr_access(buffer);
                    wlr_buffer_unlock(buffer);
                    zsingularity_preview_frame_v1_send_ready(resource);
                    return;
                }
                wlr_buffer_end_data_ptr_access(buffer);
            }
        }
    }

    bool composited = composite_capture(view, buffer);
    wlr_buffer_unlock(buffer);
    if (composited) {
        zsingularity_preview_frame_v1_send_ready(resource);
    } else {
        zsingularity_preview_frame_v1_send_failed(resource);
    }
}

static const struct zsingularity_preview_frame_v1_interface frame_impl = {
    .copy = frame_handle_copy,
    .destroy = frame_handle_destroy,
};

static void manager_handle_capture_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *toplevel_resource, int32_t overlay_cursor) {
    if (preview_debug_enabled()) {
        wlr_log(WLR_INFO, "[PREVIEW] Capture request received for toplevel resource %p (id: %u)", toplevel_resource, id);
    }
    struct singularity_preview_manager *manager = wl_resource_get_user_data(resource);
    (void)manager;

    if (!toplevel_resource) {
        wlr_log(WLR_ERROR, "[PREVIEW] Capture failed: toplevel_resource is NULL");
        return;
    }

    struct wlr_foreign_toplevel_handle_v1 *toplevel = wl_resource_get_user_data(toplevel_resource);
    struct view *view = toplevel ? toplevel->data : NULL;
    
    if (!view) {
        wlr_log(WLR_ERROR, "[PREVIEW] Capture failed: view is NULL for toplevel resource %p", toplevel_resource);
        return;
    }

    if (preview_debug_enabled()) {
        wlr_log(WLR_INFO, "[PREVIEW] Window: title='%s', app_id='%s', mapped=%d",
                view->title ? view->title : "NULL",
                view->app_id ? view->app_id : "NULL",
                view->mapped);
    }

    struct singularity_preview_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        wlr_log(WLR_ERROR, "[PREVIEW] Failed to allocate singularity_preview_frame");
        wl_client_post_no_memory(client);
        return;
    }

    frame->resource = wl_resource_create(client, &zsingularity_preview_frame_v1_interface, wl_resource_get_version(resource), id);
    if (!frame->resource) {
        wlr_log(WLR_ERROR, "[PREVIEW] Failed to create wl_resource for frame");
        free(frame);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(frame->resource, &frame_impl, frame, frame_resource_destroy);
    
    if (!view->surface || !view->mapped) {
        if (preview_debug_enabled()) {
            wlr_log(WLR_INFO, "[PREVIEW] View surface is NULL or not mapped, failing frame");
        }
        zsingularity_preview_frame_v1_send_failed(frame->resource);
        return;
    }

    struct wlr_box box = view->current;
    if (box.width <= 0 || box.height <= 0) {
        if (preview_debug_enabled()) {
            wlr_log(WLR_INFO, "[PREVIEW] View has no size yet, failing frame");
        }
        zsingularity_preview_frame_v1_send_failed(frame->resource);
        return;
    }

    frame->view = view;
    frame->view_destroy.notify = handle_view_destroy;
    wl_signal_add(&view->events.destroy, &frame->view_destroy);

    /* Prefer the actual surface texture dimensions over view->current.
     * view->current is the compositor layout box (content area, no CSD shadow),
     * but the surface buffer is larger - it includes client-side shadow/border
     * padding on all sides. Using view->current causes the right and bottom
     * portions of the texture to be silently cut off. */
    struct wlr_texture *tex = wlr_surface_has_buffer(view->surface)
        ? wlr_surface_get_texture(view->surface) : NULL;
    if (tex && (int)tex->width > 0 && (int)tex->height > 0) {
        frame->width = tex->width;
        frame->height = tex->height;
    } else {
        frame->width = box.width;
        frame->height = box.height;
    }
    frame->stride = frame->width * 4;

    if (preview_debug_enabled()) {
        wlr_log(WLR_INFO, "[PREVIEW] Sending buffer event: %dx%d", frame->width, frame->height);
    }
    zsingularity_preview_frame_v1_send_buffer(frame->resource, WL_SHM_FORMAT_ARGB8888, frame->width, frame->height, frame->stride);
}

static const struct zsingularity_preview_manager_v1_interface manager_impl = {
    .capture_toplevel = manager_handle_capture_toplevel,
    .destroy = frame_handle_destroy, 
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct singularity_preview_manager *manager = data;
    struct wl_resource *resource = wl_resource_create(client, &zsingularity_preview_manager_v1_interface, version, id);
    wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

void singularity_preview_init(void) {
    struct singularity_preview_manager *manager = calloc(1, sizeof(*manager));
    manager->global = wl_global_create(server.wl_display, &zsingularity_preview_manager_v1_interface, 1, manager, bind_manager);
}
