#ifndef RAMFB_H
#define RAMFB_H

#include "migration/vmstate.h"

/* ramfb.c */
typedef struct RAMFBState RAMFBState;
void ramfb_display_update(QemuConsole *con, RAMFBState *s);
void ramfb_diag_update(QemuConsole *con, RAMFBState *s, hwaddr addr,
                       uint32_t width, uint32_t height);
RAMFBState *ramfb_setup(Error **errp);

extern const VMStateDescription ramfb_vmstate;

/* ramfb-standalone.c */
#define TYPE_RAMFB_DEVICE "ramfb"

#endif /* RAMFB_H */
