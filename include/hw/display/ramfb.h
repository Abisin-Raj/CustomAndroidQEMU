#ifndef RAMFB_H
#define RAMFB_H

#include "migration/vmstate.h"

/* ramfb.c */
typedef struct RAMFBState RAMFBState;
void ramfb_display_update(QemuConsole *con, RAMFBState *s);
void ramfb_diag_update(QemuConsole *con, RAMFBState *s, hwaddr addr,
                       uint32_t width, uint32_t height);
void ramfb_export_update(QemuConsole *con, RAMFBState *s,
                         hwaddr addr, uint32_t buf_width, uint32_t buf_height,
                         uint32_t vis_width, uint32_t vis_height, bool is_ubwc);
RAMFBState *ramfb_setup(Error **errp);

extern const VMStateDescription ramfb_vmstate;

/* ramfb-standalone.c */
#define TYPE_RAMFB_DEVICE "ramfb"

#endif /* RAMFB_H */
