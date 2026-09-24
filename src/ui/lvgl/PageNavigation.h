#pragma once
#include "Theme.h"
#include <lvgl.h>

namespace handheld {
inline void drawPageNavigation(lv_event_t* event) {
    auto* button=lv_event_get_target(event);
    const auto index=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button));
    lv_area_t area;lv_obj_get_coords(button,&area);
    const lv_coord_t cx=(area.x1+area.x2)/2,cy=(area.y1+area.y2)/2;
    const bool doubleArrow=index==0 || index==3;
    lv_draw_line_dsc_t stroke;lv_draw_line_dsc_init(&stroke);
    stroke.width=2;stroke.round_start=1;stroke.round_end=1;
    stroke.color=lv_color_hex(lv_obj_has_state(button,LV_STATE_DISABLED)?Theme::TEXT_MUTED:Theme::TEXT_PRIMARY);
    // Geometric icons keep the visible strokes centered independently of font
    // bearings. The complete button, including the icon, remains the hit area.
    for (int i=0;i<(doubleArrow?2:1);++i) {
        const lv_coord_t x=cx-(doubleArrow?7:3)+i*8;
        const lv_coord_t outer=index<2?x+6:x,tip=index<2?x:x+6;
        const lv_point_t points[]={{outer,static_cast<lv_coord_t>(cy-6)},
                                  {tip,cy},{outer,static_cast<lv_coord_t>(cy+6)}};
        lv_draw_line(lv_event_get_draw_ctx(event),&stroke,&points[0],&points[1]);
        lv_draw_line(lv_event_get_draw_ctx(event),&stroke,&points[1],&points[2]);
    }
}
}
