#include "border.h"

extern uint32_t g_active_window_color;
extern uint32_t g_inactive_window_color;
extern float g_border_width;

static void border_init(struct border* border) {
  memset(border, 0, sizeof(struct border));
}

static void border_destroy(struct border* border) {
  SLSReleaseWindow(SLSMainConnectionID(), border->wid);
  if (border->context) CGContextRelease(border->context);
  border_init(border);
}

static void border_move(struct border* border) {
  int cid = SLSMainConnectionID();
  CFTypeRef transaction = SLSTransactionCreate(cid);

  CGRect window_frame;
  SLSGetWindowBounds(cid, border->target_wid, &window_frame);
  CGRect frame = CGRectInset(window_frame, -g_border_width, -g_border_width);
  SLSTransactionMoveWindowWithGroup(transaction, border->wid, frame.origin);

  SLSTransactionCommit(transaction, true);
  border->origin = window_frame.origin;

  CFRelease(transaction);
}

static void border_draw(struct border* border) {
  int cid = SLSMainConnectionID();

  bool shown = false;
  SLSWindowIsOrderedIn(cid, border->target_wid, &shown);
  if (!shown && border->wid) {
    SLSReleaseWindow(cid, border->wid);
    border->wid = 0;
    CGContextRelease(border->context);
    border->context = NULL;
  }
  if (!shown) return;

  CGRect window_frame;
  SLSGetWindowBounds(cid, border->target_wid, &window_frame);

  float border_width = g_border_width;
  float border_radius = 9.f;
  CGRect frame = CGRectInset(window_frame, -border_width, -border_width);

  CFArrayRef target_ref = cfarray_of_cfnumbers(&border->target_wid,
                                               sizeof(uint32_t),
                                               1,
                                               kCFNumberSInt32Type );

  CFTypeRef query = SLSWindowQueryWindows(cid, target_ref, 1);
  CFTypeRef iterator = SLSWindowQueryResultCopyWindows(query);
  int level = SLSWindowIteratorGetLevel(iterator, 0);
  CFRelease(iterator);
  CFRelease(query);

  int sub_level = 0;
  SLSGetWindowSubLevel(cid, border->target_wid, &sub_level);

  CGPoint origin = frame.origin;
  frame.origin = CGPointZero;

  SLSDisableUpdate(cid);
  if (!border->wid) {
    CFTypeRef frame_region;
    CGSNewRegionWithRect(&frame, &frame_region);

    uint64_t set_tags = 1ULL << 1;
    uint64_t clear_tags = 0;

    uint64_t id;
    SLSNewWindow(cid,
                 kCGBackingStoreBuffered,
                 -9999,
                 -9999,
                 frame_region,
                 &id                     );

    border->wid = (uint32_t)id;

    SLSSetWindowResolution(cid, border->wid, 1.0f);
    SLSSetWindowTags(cid, border->wid, &set_tags, 64);
    SLSClearWindowTags(cid, border->wid, &clear_tags, 64);
    SLSSetWindowOpacity(cid, border->wid, 0);

    border->context = SLWindowContextCreate(cid, border->wid, NULL);
    CGContextSetInterpolationQuality(border->context, kCGInterpolationNone);
    border->bounds = frame;
    border->origin = origin;
    border->needs_redraw = true;
    CFRelease(frame_region);

    if (!border->sid) {
      CFArrayRef spaces = SLSCopySpacesForWindows(cid, 0x2, target_ref);
      if (CFArrayGetCount(spaces) > 0) {
        CFNumberRef number = CFArrayGetValueAtIndex(spaces, 0);
        uint64_t sid;
        CFNumberGetValue(number, CFNumberGetType(number), &sid);
        border->sid = sid;
        CFRelease(number);
      }
      CFRelease(spaces);
    }
    CFRelease(target_ref);

    CFArrayRef window_list = cfarray_of_cfnumbers(&border->wid,
                                                  sizeof(uint32_t),
                                                  1,
                                                  kCFNumberSInt32Type);

    SLSMoveWindowsToManagedSpace(cid, window_list, border->sid);
    CFRelease(window_list);
  }

  if (!CGRectEqualToRect(frame, border->bounds)) {
    CFTypeRef frame_region;
    CGSNewRegionWithRect(&frame, &frame_region);

    border->bounds = frame;
    SLSSetWindowShape(cid, border->wid, -9999, -9999, frame_region);
    border->needs_redraw = true;
    CFRelease(frame_region);
  }

  SLSSetWindowLevel(cid, border->wid, level);
  SLSSetWindowSubLevel(cid, border->wid, sub_level);

  SLSOrderWindow(cid, border->wid, -1, border->target_wid);

  if (border->needs_redraw) {
    border->needs_redraw = false;
    CGRect path_rect = (CGRect) {{ border_width, border_width },
                                 { frame.size.width - 2.f*border_width,
                                   frame.size.height - 2.f*border_width }};

    CGPathRef path = CGPathCreateWithRoundedRect(path_rect,
                                                 border_radius,
                                                 border_radius,
                                                 NULL          );

    if (border->focused) {
      CGContextSetRGBStrokeColor(border->context,
                                 ((g_active_window_color >> 16) & 0xff) / 255.f,
                                 ((g_active_window_color >> 8) & 0xff) / 255.f,
                                 ((g_active_window_color >> 0) & 0xff) / 255.f,
                                 ((g_active_window_color >> 24) & 0xff) / 255.f );
    } else {
      CGContextSetRGBStrokeColor(border->context,
                                 ((g_inactive_window_color >> 16) & 0xff) / 255.f,
                                 ((g_inactive_window_color >> 8) & 0xff) / 255.f,
                                 ((g_inactive_window_color >> 0) & 0xff) / 255.f,
                                 ((g_inactive_window_color >> 24) & 0xff) / 255.f );
    }
    CGContextSetLineWidth(border->context, border_width);

    CGContextClearRect(border->context, frame);
    CGContextAddPath(border->context, path);
    CGContextStrokePath(border->context);
    CGContextFlush(border->context);
    CFRelease(path);
  }

  border_move(border);
  SLSReenableUpdate(cid);
}

void borders_init(struct borders* borders) {
  memset(borders, 0, sizeof(struct borders));
}

struct border* borders_add_border(struct borders* borders, uint32_t wid, uint64_t sid) {
  struct border* border = NULL;

  for (int i = 0; i < borders->num_borders; i++) {
    if (borders->borders[i].target_wid == wid
        && (borders->borders[i].sid == sid || sid == 0)) {
      border = &borders->borders[i];
    }
  }

  if (!border) {
    borders->borders = realloc(borders->borders,
                               ++borders->num_borders * sizeof(struct border));
    border = &borders->borders[borders->num_borders - 1];
    border_init(border);
  }

  border->target_wid = wid;
  border->sid = sid;
  border->needs_redraw = true;
  border_draw(border);
  return border;
}

void borders_remove_border(struct borders* borders, uint32_t wid, uint64_t sid) {
  for (int i = 0; i < borders->num_borders; i++) {
    if (borders->borders[i].target_wid == wid
        && (borders->borders[i].sid == sid || sid == 0)) {
      border_destroy(&borders->borders[i]);
    }
  }
}

void borders_update_border(struct borders* borders, uint32_t wid) {
  for (int i = 0; i < borders->num_borders; i++) {
    if (borders->borders[i].target_wid == wid) {
      border_draw(&borders->borders[i]);
    }
  }
}

void borders_window_focus(struct borders* borders, uint32_t wid) {
  for (int i = 0; i < borders->num_borders; i++) {
    if (borders->borders[i].focused && borders->borders[i].target_wid != wid) {
      borders->borders[i].focused = false;
      borders->borders[i].needs_redraw = true;
      border_draw(&borders->borders[i]);
    }

    if (borders->borders[i].target_wid == wid) {
      if (!borders->borders[i].focused) {
        borders->borders[i].focused = true;
        borders->borders[i].needs_redraw = true;
        border_draw(&borders->borders[i]);
      }
    }
  }
}

void borders_move_border(struct borders* borders, uint32_t wid) {
  for (int i = 0; i < borders->num_borders; i++) {
    if (borders->borders[i].target_wid == wid) {
      border_move(&borders->borders[i]);
    }
  }
}
