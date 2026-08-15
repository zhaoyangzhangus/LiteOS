#include "window.h"
#include <kernel/input.h>

static BOOLEAN window_belongs(const LITEOS_WINDOW_MANAGER *manager,
                              const LITEOS_WINDOW *window) {
    if (manager == 0 || window == 0) return 0;
    for (UINT32 i = 0; i < LITEOS_WINDOW_COUNT; ++i) {
        if (&manager->Windows[i] == window) return manager->Windows[i].Used;
    }
    return 0;
}

static UINT32 min_u32(UINT32 left, UINT32 right) {
    return left < right ? left : right;
}

static BOOLEAN display_valid(const LITEOS_DISPLAY *display) {
    return display != 0 && display->Base != 0U && display->Width != 0U &&
           display->Height != 0U && display->PixelsPerScanLine >= display->Width;
}

static BOOLEAN display_same(const LITEOS_DISPLAY *left,
                            const LITEOS_DISPLAY *right) {
    if (left == 0 || right == 0) return 0;
    return left->Base == right->Base && left->Width == right->Width &&
           left->Height == right->Height &&
           left->PixelsPerScanLine == right->PixelsPerScanLine &&
           left->Format == right->Format;
}

static VOID mark_full_damage(LITEOS_WINDOW_MANAGER *manager) {
    if (manager == 0) return;
    manager->Dirty = 1;
    manager->DirtyX = 0U;
    manager->DirtyY = 0U;
    manager->DirtyWidth = manager->Display.Width;
    manager->DirtyHeight = manager->Display.Height;
    for (UINT32 i = 0U; i < LITEOS_WINDOW_OUTPUT_COUNT; ++i) {
        if (manager->Outputs[i].Active) manager->Outputs[i].FullDamage = 1;
    }
}

static VOID mark_dirty(LITEOS_WINDOW_MANAGER *manager,
                       INT64 x, INT64 y, UINT32 width, UINT32 height) {
    INT64 right;
    INT64 bottom;
    UINT32 clipped_x;
    UINT32 clipped_y;
    UINT32 clipped_right;
    UINT32 clipped_bottom;
    if (manager == 0 || width == 0U || height == 0U) return;
    right = x + (INT64)width;
    bottom = y + (INT64)height;
    if (right <= 0 || bottom <= 0 || x >= (INT64)manager->Display.Width ||
        y >= (INT64)manager->Display.Height) return;
    clipped_x = x <= 0 ? 0U : (UINT32)x;
    clipped_y = y <= 0 ? 0U : (UINT32)y;
    clipped_right = right >= (INT64)manager->Display.Width ? manager->Display.Width :
                    (UINT32)right;
    clipped_bottom = bottom >= (INT64)manager->Display.Height ? manager->Display.Height :
                     (UINT32)bottom;
    if (clipped_x >= clipped_right || clipped_y >= clipped_bottom) return;
    if (!manager->Dirty) {
        manager->Dirty = 1;
        manager->DirtyX = clipped_x;
        manager->DirtyY = clipped_y;
        manager->DirtyWidth = clipped_right - clipped_x;
        manager->DirtyHeight = clipped_bottom - clipped_y;
        return;
    }
    UINT64 old_right = (UINT64)manager->DirtyX + manager->DirtyWidth;
    UINT64 old_bottom = (UINT64)manager->DirtyY + manager->DirtyHeight;
    if (clipped_x < manager->DirtyX) manager->DirtyX = clipped_x;
    if (clipped_y < manager->DirtyY) manager->DirtyY = clipped_y;
    if (clipped_right > old_right) old_right = clipped_right;
    if (clipped_bottom > old_bottom) old_bottom = clipped_bottom;
    manager->DirtyWidth = (UINT32)(old_right - manager->DirtyX);
    manager->DirtyHeight = (UINT32)(old_bottom - manager->DirtyY);
}

static UINT32 color_from_background(UINT32 color) {
    return color;
}

static LITEOS_WINDOW *window_by_identifier(LITEOS_WINDOW_MANAGER *manager,
                                           UINT32 identifier) {
    if (manager == 0 || identifier == 0U) return 0;
    for (UINT32 i = 0U; i < LITEOS_WINDOW_COUNT; ++i) {
        if (manager->Windows[i].Used &&
            manager->Windows[i].Identifier == identifier) {
            return &manager->Windows[i];
        }
    }
    return 0;
}

static BOOLEAN window_contains_point(const LITEOS_WINDOW *window,
                                     INT64 x, INT64 y,
                                     UINT32 *local_x, UINT32 *local_y) {
    INT64 offset_x;
    INT64 offset_y;
    if (window == 0 || !window->Used || !window->Visible) return 0;
    offset_x = x - (INT64)window->X;
    offset_y = y - (INT64)window->Y;
    if (offset_x < 0 || offset_y < 0 || (UINT64)offset_x >= window->Width ||
        (UINT64)offset_y >= window->Height) return 0;
    if (local_x != 0) *local_x = (UINT32)offset_x;
    if (local_y != 0) *local_y = (UINT32)offset_y;
    return 1;
}

static BOOLEAN window_titlebar_contains_point(const LITEOS_WINDOW *window,
                                              INT64 x, INT64 y,
                                              UINT32 *local_x, UINT32 *local_y) {
    UINT32 titlebar_height;
    UINT32 hit_x;
    UINT32 hit_y;
    if (!window_contains_point(window, x, y, &hit_x, &hit_y)) return 0;
    titlebar_height = min_u32(window->Height, LITEOS_WINDOW_TITLEBAR_HEIGHT);
    if (hit_y >= titlebar_height) return 0;
    if (local_x != 0) *local_x = hit_x;
    if (local_y != 0) *local_y = hit_y;
    return 1;
}

static INT32 clamp_to_i32(INT64 value) {
    if (value > 2147483647LL) return 2147483647;
    if (value < -2147483647LL - 1LL) return (-2147483647 - 1);
    return (INT32)value;
}

static INT32 pointer_event_coordinate(UINT32 coordinate) {
    return coordinate > 2147483647U ? 2147483647 : (INT32)coordinate;
}

static BOOLEAN raise_window(LITEOS_WINDOW_MANAGER *manager,
                            LITEOS_WINDOW *window) {
    UINT8 index;
    if (manager == 0 || !window_belongs(manager, window)) return 0;
    index = (UINT8)(window - manager->Windows);
    for (UINT32 position = 0U; position < manager->ZOrderCount; ++position) {
        if (manager->ZOrder[position] != index) continue;
        if (position + 1U == manager->ZOrderCount) return 1;
        for (UINT32 next = position + 1U; next < manager->ZOrderCount; ++next) {
            manager->ZOrder[next - 1U] = manager->ZOrder[next];
        }
        manager->ZOrder[manager->ZOrderCount - 1U] = index;
        mark_dirty(manager, window->X, window->Y, window->Width, window->Height);
        return 1;
    }
    return 0;
}

/* 重启合成器后优先恢复当前焦点；原窗口缓冲区仍归窗口对象持有。 */
static void restore_focus(LITEOS_WINDOW_MANAGER *manager) {
    LITEOS_WINDOW *focused;
    if (manager == 0) return;
    focused = window_by_identifier(manager, manager->FocusedIdentifier);
    if (focused != 0 && focused->Visible) return;
    manager->FocusedIdentifier = 0U;
    for (UINT32 i = manager->ZOrderCount; i != 0U; --i) {
        LITEOS_WINDOW *window = &manager->Windows[manager->ZOrder[i - 1U]];
        if (window->Used && window->Visible) {
            manager->FocusedIdentifier = window->Identifier;
            return;
        }
    }
}

static void reset_focus_cycle(LITEOS_WINDOW_MANAGER *manager) {
    UINT32 count = 0U;
    if (manager == 0) return;
    for (UINT32 order = manager->ZOrderCount; order != 0U; --order) {
        LITEOS_WINDOW *window = &manager->Windows[manager->ZOrder[order - 1U]];
        if (!window->Used || !window->Visible) continue;
        manager->FocusCycleIdentifiers[count++] = window->Identifier;
    }
    manager->FocusCycleCount = count;
    manager->FocusCycleIndex = 0U;
    for (UINT32 index = 0U; index < count; ++index) {
        if (manager->FocusCycleIdentifiers[index] == manager->FocusedIdentifier) {
            manager->FocusCycleIndex = index;
            break;
        }
    }
    for (UINT32 index = count; index < LITEOS_WINDOW_COUNT; ++index) {
        manager->FocusCycleIdentifiers[index] = 0U;
    }
}

static BOOLEAN focus_window(LITEOS_WINDOW_MANAGER *manager,
                            LITEOS_WINDOW *window, BOOLEAN reset_cycle) {
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning ||
        !window_belongs(manager, window) || !window->Visible ||
        !raise_window(manager, window)) return 0;
    manager->FocusedIdentifier = window->Identifier;
    if (reset_cycle) reset_focus_cycle(manager);
    return 1;
}

BOOLEAN liteos_window_manager_init(LITEOS_WINDOW_MANAGER *manager,
                                   const LITEOS_BOOT_INFO *boot_info) {
    UINT64 required_framebuffer_size;
    if (manager == 0 || boot_info == 0 || manager->Initialized ||
        boot_info->FrameBufferBase == 0 || boot_info->FrameBufferWidth == 0 ||
        boot_info->FrameBufferHeight == 0 || boot_info->FrameBufferPixelsPerScanLine == 0 ||
        boot_info->FrameBufferFormat > LITEOS_PIXEL_BITMASK ||
        boot_info->FrameBufferPixelsPerScanLine > (UINT32)-1 / boot_info->FrameBufferHeight) {
        return 0;
    }
    required_framebuffer_size = (UINT64)boot_info->FrameBufferPixelsPerScanLine *
                                boot_info->FrameBufferHeight * sizeof(UINT32);
    if (boot_info->FrameBufferSize < required_framebuffer_size) return 0;
    manager->Display.Base = boot_info->FrameBufferBase;
    manager->Display.Width = boot_info->FrameBufferWidth;
    manager->Display.Height = boot_info->FrameBufferHeight;
    manager->Display.PixelsPerScanLine = boot_info->FrameBufferPixelsPerScanLine;
    manager->Display.Format = boot_info->FrameBufferFormat;
    for (UINT32 i = 0; i < 4U; ++i) manager->Display.Mask[i] = boot_info->FrameBufferMask[i];
    for (UINT32 i = 0U; i < LITEOS_WINDOW_OUTPUT_COUNT; ++i) {
        manager->Outputs[i].Active = 0;
        manager->Outputs[i].FullDamage = 0;
        manager->Outputs[i].VBlankSequence = 0U;
        manager->Outputs[i].PresentedVBlank = 0U;
        manager->Outputs[i].MissedVBlankCount = 0U;
        manager->Outputs[i].HotplugGeneration = 0U;
    }
    manager->Outputs[0].Active = 1;
    manager->Outputs[0].FullDamage = 1;
    manager->Outputs[0].Display = manager->Display;
    manager->Outputs[0].HotplugGeneration = 1U;
    manager->OutputCount = 1U;
    for (UINT32 i = 0; i < LITEOS_WINDOW_COUNT; ++i) {
        manager->Windows[i].Used = 0;
        manager->ZOrder[i] = 0;
    }
    manager->WindowCount = 0;
    manager->ZOrderCount = 0;
    manager->NextIdentifier = 1U;
    manager->Dirty = 0;
    manager->DirtyX = 0;
    manager->DirtyY = 0;
    manager->DirtyWidth = 0U;
    manager->DirtyHeight = 0U;
    manager->InputRead = 0;
    manager->InputWrite = 0;
    manager->InputCount = 0;
    manager->FocusedIdentifier = 0U;
    manager->FocusCycleCount = 0U;
    manager->FocusCycleIndex = 0U;
    for (UINT32 i = 0U; i < LITEOS_WINDOW_COUNT; ++i) {
        manager->FocusCycleIdentifiers[i] = 0U;
    }
    manager->PointerX = manager->Display.Width / 2U;
    manager->PointerY = manager->Display.Height / 2U;
    manager->PointerButtons = 0U;
    manager->KeyboardModifiers = 0U;
    manager->DraggedIdentifier = 0U;
    manager->DragOffsetX = 0U;
    manager->DragOffsetY = 0U;
    manager->CompositorGeneration = 1U;
    manager->FrameSequence = 0U;
    manager->CompositorRunning = 1;
    manager->Initialized = 1;
    mark_full_damage(manager);
    return 1;
}

BOOLEAN liteos_window_manager_destroy(LITEOS_WINDOW_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized) return 0;
    for (UINT32 i = 0; i < LITEOS_WINDOW_COUNT; ++i) {
        if (manager->Windows[i].Used && !liteos_window_destroy(manager, &manager->Windows[i])) return 0;
    }
    manager->CompositorRunning = 0;
    manager->Initialized = 0;
    return 1;
}

LITEOS_WINDOW *liteos_window_create(LITEOS_WINDOW_MANAGER *manager,
                                    INT32 x, INT32 y, UINT32 width, UINT32 height,
                                    UINT32 background, BOOLEAN visible) {
    if (manager == 0 || !manager->Initialized || width == 0U || height == 0U ||
        width > (UINT32)-1 / height || manager->WindowCount >= LITEOS_WINDOW_COUNT) return 0;
    UINT64 bytes = (UINT64)width * height * sizeof(UINT32);
    for (UINT32 i = 0; i < LITEOS_WINDOW_COUNT; ++i) {
        LITEOS_WINDOW *window = &manager->Windows[i];
        if (window->Used) continue;
        if (!liteos_buddy_alloc_bytes(bytes, &window->BufferBlock)) return 0;
        window->Pixels = (UINT32 *)(uintptr_t)window->BufferBlock.PhysicalAddress;
        for (UINT64 pixel = 0; pixel < (UINT64)width * height; ++pixel) {
            window->Pixels[pixel] = color_from_background(background);
        }
        window->Used = 1;
        window->Visible = visible ? 1 : 0;
        window->Identifier = manager->NextIdentifier++;
        if (window->Identifier == 0U) window->Identifier = manager->NextIdentifier++;
        window->X = x;
        window->Y = y;
        window->Width = width;
        window->Height = height;
        window->Background = background;
        manager->ZOrder[manager->ZOrderCount++] = (UINT8)i;
        ++manager->WindowCount;
        if (window->Visible) {
            manager->FocusedIdentifier = window->Identifier;
        }
        reset_focus_cycle(manager);
        mark_dirty(manager, x, y, width, height);
        return window;
    }
    return 0;
}

BOOLEAN liteos_window_destroy(LITEOS_WINDOW_MANAGER *manager,
                              LITEOS_WINDOW *window) {
    if (manager == 0 || !manager->Initialized || !window_belongs(manager, window)) return 0;
    mark_dirty(manager, window->X, window->Y, window->Width, window->Height);
    for (UINT32 i = 0; i < manager->ZOrderCount; ++i) {
        if (manager->ZOrder[i] != (UINT8)(window - manager->Windows)) continue;
        for (UINT32 j = i + 1U; j < manager->ZOrderCount; ++j) {
            manager->ZOrder[j - 1U] = manager->ZOrder[j];
        }
        --manager->ZOrderCount;
        break;
    }
    if (!liteos_buddy_free(&window->BufferBlock)) return 0;
    if (manager->FocusedIdentifier == window->Identifier) {
        manager->FocusedIdentifier = 0U;
    }
    if (manager->DraggedIdentifier == window->Identifier) {
        manager->DraggedIdentifier = 0U;
        manager->DragOffsetX = 0U;
        manager->DragOffsetY = 0U;
    }
    window->Used = 0;
    window->Visible = 0;
    window->Identifier = 0;
    window->Pixels = 0;
    if (manager->WindowCount != 0U) --manager->WindowCount;
    restore_focus(manager);
    reset_focus_cycle(manager);
    return 1;
}

BOOLEAN liteos_window_move(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, INT32 x, INT32 y) {
    if (manager == 0 || !manager->Initialized || !window_belongs(manager, window)) return 0;
    mark_dirty(manager, window->X, window->Y, window->Width, window->Height);
    window->X = x;
    window->Y = y;
    mark_dirty(manager, window->X, window->Y, window->Width, window->Height);
    return 1;
}

BOOLEAN liteos_window_show(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, BOOLEAN visible) {
    if (manager == 0 || !manager->Initialized || !window_belongs(manager, window)) return 0;
    if (window->Visible == (visible ? 1 : 0)) return 1;
    mark_dirty(manager, window->X, window->Y, window->Width, window->Height);
    window->Visible = visible ? 1 : 0;
    if (!window->Visible && manager->FocusedIdentifier == window->Identifier) {
        manager->FocusedIdentifier = 0U;
        restore_focus(manager);
    }
    if (!window->Visible && manager->DraggedIdentifier == window->Identifier) {
        manager->DraggedIdentifier = 0U;
        manager->DragOffsetX = 0U;
        manager->DragOffsetY = 0U;
    } else if (window->Visible && manager->FocusedIdentifier == 0U) {
        manager->FocusedIdentifier = window->Identifier;
    }
    reset_focus_cycle(manager);
    return 1;
}

BOOLEAN liteos_window_focus(LITEOS_WINDOW_MANAGER *manager,
                            LITEOS_WINDOW *window) {
    return focus_window(manager, window, 1);
}

BOOLEAN liteos_window_focus_next(LITEOS_WINDOW_MANAGER *manager) {
    LITEOS_WINDOW *window;
    UINT32 next;
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning) return 0;
    if (manager->FocusCycleCount == 0U || manager->FocusCycleIndex >= manager->FocusCycleCount ||
        manager->FocusCycleIdentifiers[manager->FocusCycleIndex] != manager->FocusedIdentifier) {
        reset_focus_cycle(manager);
    }
    if (manager->FocusCycleCount == 0U) return 0;
    for (UINT32 attempt = 1U; attempt <= manager->FocusCycleCount; ++attempt) {
        next = (manager->FocusCycleIndex + attempt) % manager->FocusCycleCount;
        window = window_by_identifier(manager, manager->FocusCycleIdentifiers[next]);
        if (window == 0 || !window->Visible) continue;
        if (!focus_window(manager, window, 0)) return 0;
        manager->FocusCycleIndex = next;
        return 1;
    }
    return 0;
}

UINT32 liteos_window_focused(const LITEOS_WINDOW_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning) {
        return 0U;
    }
    return manager->FocusedIdentifier;
}

LITEOS_WINDOW *liteos_window_hit_test(LITEOS_WINDOW_MANAGER *manager,
                                      INT32 x, INT32 y) {
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning) return 0;
    for (UINT32 order = manager->ZOrderCount; order != 0U; --order) {
        LITEOS_WINDOW *window = &manager->Windows[manager->ZOrder[order - 1U]];
        if (window_contains_point(window, x, y, 0, 0)) return window;
    }
    return 0;
}

BOOLEAN liteos_window_pointer_move_relative(LITEOS_WINDOW_MANAGER *manager,
                                            INT32 delta_x, INT32 delta_y) {
    LITEOS_WINDOW *dragged;
    INT64 next_x;
    INT64 next_y;
    INT64 window_x;
    INT64 window_y;
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning ||
        manager->Display.Width == 0U || manager->Display.Height == 0U) return 0;
    next_x = (INT64)manager->PointerX + delta_x;
    next_y = (INT64)manager->PointerY + delta_y;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if (next_x >= manager->Display.Width) next_x = manager->Display.Width - 1U;
    if (next_y >= manager->Display.Height) next_y = manager->Display.Height - 1U;
    manager->PointerX = (UINT32)next_x;
    manager->PointerY = (UINT32)next_y;
    if ((manager->PointerButtons & LITEOS_WINDOW_POINTER_BUTTON_LEFT) == 0U ||
        manager->DraggedIdentifier == 0U) return 1;
    dragged = window_by_identifier(manager, manager->DraggedIdentifier);
    if (dragged == 0 || !dragged->Visible) {
        manager->DraggedIdentifier = 0U;
        manager->DragOffsetX = 0U;
        manager->DragOffsetY = 0U;
        return 1;
    }
    window_x = (INT64)manager->PointerX - manager->DragOffsetX;
    window_y = (INT64)manager->PointerY - manager->DragOffsetY;
    if (dragged->X != clamp_to_i32(window_x) || dragged->Y != clamp_to_i32(window_y)) {
        return liteos_window_move(manager, dragged, clamp_to_i32(window_x), clamp_to_i32(window_y));
    }
    return 1;
}

BOOLEAN liteos_window_pointer_button(LITEOS_WINDOW_MANAGER *manager,
                                     UINT32 button, BOOLEAN pressed) {
    LITEOS_WINDOW *window;
    UINT32 local_x;
    UINT32 local_y;
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning ||
        button != LITEOS_WINDOW_POINTER_BUTTON_LEFT) return 0;
    if (!pressed) {
        manager->PointerButtons &= ~LITEOS_WINDOW_POINTER_BUTTON_LEFT;
        manager->DraggedIdentifier = 0U;
        manager->DragOffsetX = 0U;
        manager->DragOffsetY = 0U;
        return 1;
    }
    if ((manager->PointerButtons & LITEOS_WINDOW_POINTER_BUTTON_LEFT) != 0U) return 1;
    manager->PointerButtons |= LITEOS_WINDOW_POINTER_BUTTON_LEFT;
    window = liteos_window_hit_test(manager, pointer_event_coordinate(manager->PointerX),
                                    pointer_event_coordinate(manager->PointerY));
    if (window == 0 || !liteos_window_focus(manager, window)) return 1;
    if (!window_titlebar_contains_point(window, manager->PointerX, manager->PointerY,
                                        &local_x, &local_y)) return 1;
    manager->DraggedIdentifier = window->Identifier;
    manager->DragOffsetX = local_x;
    manager->DragOffsetY = local_y;
    return 1;
}

BOOLEAN liteos_window_fill(LITEOS_WINDOW_MANAGER *manager,
                           LITEOS_WINDOW *window, UINT32 x, UINT32 y,
                           UINT32 width, UINT32 height, UINT32 color) {
    if (manager == 0 || !manager->Initialized || !window_belongs(manager, window) ||
        x >= window->Width || y >= window->Height || width == 0U || height == 0U) return 0;
    width = min_u32(width, window->Width - x);
    height = min_u32(height, window->Height - y);
    for (UINT32 row = 0; row < height; ++row) {
        UINT32 *pixels = window->Pixels + (UINT64)(y + row) * window->Width + x;
        for (UINT32 column = 0; column < width; ++column) pixels[column] = color;
    }
    mark_dirty(manager, (INT64)window->X + x, (INT64)window->Y + y, width, height);
    return 1;
}

BOOLEAN liteos_window_output_attach(LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output, const LITEOS_DISPLAY *display) {
    LITEOS_WINDOW_OUTPUT *target;
    if (manager == 0 || display == 0 || !manager->Initialized ||
        !manager->CompositorRunning || output == 0U ||
        output >= LITEOS_WINDOW_OUTPUT_COUNT || !display_valid(display)) return 0;
    target = &manager->Outputs[output];
    for (UINT32 i = 0U; i < LITEOS_WINDOW_OUTPUT_COUNT; ++i) {
        if (i != output && manager->Outputs[i].Active &&
            display_same(&manager->Outputs[i].Display, display)) return 0;
    }
    if (target->Active && display_same(&target->Display, display)) return 1;
    if (!target->Active) ++manager->OutputCount;
    target->Active = 1;
    target->FullDamage = 1;
    target->Display = *display;
    target->VBlankSequence = 0U;
    target->PresentedVBlank = 0U;
    target->MissedVBlankCount = 0U;
    ++target->HotplugGeneration;
    if (target->HotplugGeneration == 0U) target->HotplugGeneration = 1U;
    mark_full_damage(manager);
    return 1;
}

BOOLEAN liteos_window_output_detach(LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output) {
    LITEOS_WINDOW_OUTPUT *target;
    if (manager == 0 || !manager->Initialized || output == 0U ||
        output >= LITEOS_WINDOW_OUTPUT_COUNT ||
        !(target = &manager->Outputs[output])->Active) return 0;
    target->Active = 0;
    target->FullDamage = 0;
    target->Display = (LITEOS_DISPLAY){0};
    target->VBlankSequence = 0U;
    target->PresentedVBlank = 0U;
    target->MissedVBlankCount = 0U;
    ++target->HotplugGeneration;
    if (target->HotplugGeneration == 0U) target->HotplugGeneration = 1U;
    if (manager->OutputCount != 0U) --manager->OutputCount;
    return 1;
}

BOOLEAN liteos_window_vblank(LITEOS_WINDOW_MANAGER *manager, UINT32 output) {
    LITEOS_WINDOW_OUTPUT *target;
    if (manager == 0 || !manager->Initialized || output >= LITEOS_WINDOW_OUTPUT_COUNT ||
        !(target = &manager->Outputs[output])->Active) return 0;
    ++target->VBlankSequence;
    if (manager->Dirty && target->VBlankSequence > target->PresentedVBlank + 1U) {
        ++target->MissedVBlankCount;
    }
    return 1;
}

UINT32 liteos_window_output_count(const LITEOS_WINDOW_MANAGER *manager) {
    return manager != 0 && manager->Initialized ? manager->OutputCount : 0U;
}

UINT64 liteos_window_missed_vblanks(const LITEOS_WINDOW_MANAGER *manager,
                                    UINT32 output) {
    if (manager == 0 || !manager->Initialized || output >= LITEOS_WINDOW_OUTPUT_COUNT ||
        !manager->Outputs[output].Active) return 0U;
    return manager->Outputs[output].MissedVBlankCount;
}

BOOLEAN liteos_window_present(LITEOS_WINDOW_MANAGER *manager) {
    BOOLEAN rendered = 0;
    if (manager == 0 || !manager->Initialized || !manager->CompositorRunning) return 0;
    if (!manager->Dirty) {
        for (UINT32 i = 0U; i < LITEOS_WINDOW_OUTPUT_COUNT; ++i) {
            if (manager->Outputs[i].Active && manager->Outputs[i].FullDamage) {
                rendered = 1;
                break;
            }
        }
        if (!rendered) return 1;
    }
    for (UINT32 output_index = 0U; output_index < LITEOS_WINDOW_OUTPUT_COUNT;
         ++output_index) {
        LITEOS_WINDOW_OUTPUT *output = &manager->Outputs[output_index];
        UINT32 left;
        UINT32 top;
        UINT32 right;
        UINT32 bottom;
        if (!output->Active) continue;
        if (output->FullDamage) {
            left = 0U;
            top = 0U;
            right = output->Display.Width;
            bottom = output->Display.Height;
        } else {
            left = manager->DirtyX;
            top = manager->DirtyY;
            right = min_u32(output->Display.Width, manager->DirtyX + manager->DirtyWidth);
            bottom = min_u32(output->Display.Height, manager->DirtyY + manager->DirtyHeight);
        }
        if (left >= right || top >= bottom) continue;
        volatile UINT32 *display = (volatile UINT32 *)(uintptr_t)output->Display.Base;
        for (UINT32 y = top; y < bottom; ++y) {
            for (UINT32 x = left; x < right; ++x) {
            UINT32 color = 0;
            for (UINT32 order = 0; order < manager->ZOrderCount; ++order) {
                LITEOS_WINDOW *window = &manager->Windows[manager->ZOrder[order]];
                INT32 display_x = (INT32)x;
                INT32 display_y = (INT32)y;
                if (!window->Used || !window->Visible || display_x < window->X ||
                    display_y < window->Y || (UINT32)(display_x - window->X) >= window->Width ||
                    (UINT32)(display_y - window->Y) >= window->Height) continue;
                color = window->Pixels[(UINT64)(display_y - window->Y) * window->Width +
                                       (UINT32)(display_x - window->X)];
            }
            display[(UINTN)y * output->Display.PixelsPerScanLine + x] = color;
            }
        }
        output->FullDamage = 0;
        output->PresentedVBlank = output->VBlankSequence;
        rendered = 1;
    }
    manager->Dirty = 0;
    if (rendered) ++manager->FrameSequence;
    return 1;
}

BOOLEAN liteos_window_compositor_restart(LITEOS_WINDOW_MANAGER *manager) {
    if (manager == 0 || !manager->Initialized) return 0;
    /* 运行代次先失效，调用方不能在重启窗口中提交帧。 */
    manager->CompositorRunning = 0;
    ++manager->CompositorGeneration;
    if (manager->CompositorGeneration == 0U) manager->CompositorGeneration = 1U;
    mark_full_damage(manager);
    restore_focus(manager);
    manager->CompositorRunning = 1;
    return 1;
}

BOOLEAN liteos_window_compositor_running(const LITEOS_WINDOW_MANAGER *manager) {
    return manager != 0 && manager->Initialized && manager->CompositorRunning;
}

UINT32 liteos_window_compositor_generation(const LITEOS_WINDOW_MANAGER *manager) {
    return manager != 0 && manager->Initialized ? manager->CompositorGeneration : 0U;
}

UINT64 liteos_window_frame_sequence(const LITEOS_WINDOW_MANAGER *manager) {
    return manager != 0 && manager->Initialized ? manager->FrameSequence : 0U;
}

BOOLEAN liteos_input_push(LITEOS_WINDOW_MANAGER *manager,
                          const LITEOS_INPUT_EVENT *event) {
    if (manager == 0 || event == 0 || !manager->Initialized ||
        manager->InputCount >= LITEOS_INPUT_EVENT_COUNT) return 0;
    manager->InputEvents[manager->InputWrite] = *event;
    manager->InputWrite = (manager->InputWrite + 1U) % LITEOS_INPUT_EVENT_COUNT;
    ++manager->InputCount;
    return 1;
}

BOOLEAN liteos_input_pop(LITEOS_WINDOW_MANAGER *manager,
                         LITEOS_INPUT_EVENT *event) {
    if (manager == 0 || event == 0 || !manager->Initialized || manager->InputCount == 0U) return 0;
    *event = manager->InputEvents[manager->InputRead];
    manager->InputRead = (manager->InputRead + 1U) % LITEOS_INPUT_EVENT_COUNT;
    --manager->InputCount;
    return 1;
}

BOOLEAN liteos_window_pump_input(LITEOS_WINDOW_MANAGER *manager) {
    input_event_t event;
    if (manager == 0 || !manager->Initialized) return 0;
    while (input_core_pop(&event) == K_OK) {
        LITEOS_INPUT_EVENT window_event = {0};
        if (event.type == INPUT_EVENT_KEY) {
            window_event.Type = LITEOS_INPUT_KEY;
            if (event.code == LITEOS_WINDOW_KEY_LEFT_ALT ||
                event.code == LITEOS_WINDOW_KEY_RIGHT_ALT) {
                if (event.value == INPUT_VALUE_PRESS || event.value == INPUT_VALUE_REPEAT) {
                    manager->KeyboardModifiers |= 1U;
                } else if (event.value == INPUT_VALUE_RELEASE) {
                    manager->KeyboardModifiers &= ~1U;
                }
            } else if (event.code == LITEOS_WINDOW_KEY_TAB &&
                       (event.value == INPUT_VALUE_PRESS ||
                        event.value == INPUT_VALUE_REPEAT) &&
                       (manager->KeyboardModifiers & 1U) != 0U &&
                       !liteos_window_focus_next(manager)) {
                return 0;
            }
        } else if (event.type == INPUT_EVENT_BUTTON) {
            window_event.Type = LITEOS_INPUT_POINTER_BUTTON;
            if (event.code == INPUT_BUTTON_LEFT &&
                !liteos_window_pointer_button(manager,
                                               LITEOS_WINDOW_POINTER_BUTTON_LEFT,
                                               event.value != INPUT_VALUE_RELEASE)) {
                return 0;
            }
        } else if (event.type == INPUT_EVENT_RELATIVE) {
            window_event.Type = LITEOS_INPUT_POINTER_MOVE;
            if (event.code == INPUT_REL_X) {
                if (!liteos_window_pointer_move_relative(manager, event.value, 0)) return 0;
            } else if (event.code == INPUT_REL_Y) {
                if (!liteos_window_pointer_move_relative(manager, 0, event.value)) return 0;
            }
        } else {
            continue;
        }
        window_event.Code = event.code;
        if (window_event.Type != LITEOS_INPUT_KEY) {
            window_event.X = pointer_event_coordinate(manager->PointerX);
            window_event.Y = pointer_event_coordinate(manager->PointerY);
        }
        window_event.Value = event.value;
        window_event.Timestamp = event.timestamp;
        if (!liteos_input_push(manager, &window_event)) return 0;
    }
    return 1;
}
