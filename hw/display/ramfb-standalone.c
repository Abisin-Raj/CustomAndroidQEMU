#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/display/ramfb.h"
#include "ui/console.h"
#include "ui/win32-android-display.h"
#include "qom/object.h"

typedef struct RAMFBStandaloneState RAMFBStandaloneState;
DECLARE_INSTANCE_CHECKER(RAMFBStandaloneState, RAMFB,
                         TYPE_RAMFB_DEVICE)

struct RAMFBStandaloneState {
    SysBusDevice parent_obj;
    QemuConsole *con;
    RAMFBState *state;
    bool migrate;
    bool diag_mode;
    uint64_t diag_addr;
    uint32_t diag_width;
    uint32_t diag_height;
    bool export_fb;
    uint64_t export_addr;
    uint32_t export_buf_width;
    uint32_t export_buf_height;
    uint32_t export_width;
    uint32_t export_height;
    bool export_ubwc;
    uint64_t export_count;
    bool win32_display; /* x-win32-display: open independent Win32 window */
    bool export_inject_test; /* x-export-inject-test: POC test pattern injection */
};

static void display_update_wrapper(void *dev)
{
    RAMFBStandaloneState *ramfb = RAMFB(dev);

    if (ramfb->export_fb) {
        ramfb_export_update(ramfb->con, ramfb->state, ramfb->export_addr,
                            ramfb->export_buf_width, ramfb->export_buf_height,
                            ramfb->export_width, ramfb->export_height,
                            ramfb->export_ubwc, ramfb->export_inject_test);
        ramfb->export_count++;
    } else if (ramfb->diag_mode) {
        ramfb_diag_update(ramfb->con, ramfb->state, ramfb->diag_addr,
                          ramfb->diag_width, ramfb->diag_height);
    } else {
        ramfb_display_update(ramfb->con, ramfb->state);
    }
}

static const GraphicHwOps wrapper_ops = {
    .gfx_update = display_update_wrapper,
};

static void ramfb_realizefn(DeviceState *dev, Error **errp)
{
    RAMFBStandaloneState *ramfb = RAMFB(dev);

    ramfb->con = graphic_console_init(dev, 0, &wrapper_ops, dev);
    ramfb->state = ramfb_setup(errp);
    ramfb->export_count = 0;
    object_property_add_uint64_ptr(OBJECT(dev), "export-count", &ramfb->export_count, OBJ_PROP_FLAG_READ);

    if (ramfb->win32_display) {
        if (!win32_android_display_init(ramfb->con)) {
            warn_report("ramfb: x-win32-display=on requested but Win32 display init failed");
        }
    }
}

static bool migrate_needed(void *opaque)
{
    RAMFBStandaloneState *ramfb = RAMFB(opaque);

    return ramfb->migrate;
}

static const VMStateDescription ramfb_dev_vmstate = {
    .name = "ramfb-dev",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = migrate_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(state, RAMFBStandaloneState, ramfb_vmstate, RAMFBState),
        VMSTATE_END_OF_LIST()
    }
};

static Property ramfb_properties[] = {
    DEFINE_PROP_BOOL("x-migrate", RAMFBStandaloneState, migrate,  true),
    DEFINE_PROP_BOOL("x-diag-mode", RAMFBStandaloneState, diag_mode, false),
    DEFINE_PROP_UINT64("x-diag-addr", RAMFBStandaloneState, diag_addr, 0x7f000000ULL),
    DEFINE_PROP_UINT32("x-diag-width", RAMFBStandaloneState, diag_width, 640),
    DEFINE_PROP_UINT32("x-diag-height", RAMFBStandaloneState, diag_height, 480),
    DEFINE_PROP_BOOL("x-export-fb", RAMFBStandaloneState, export_fb, false),
    DEFINE_PROP_UINT64("x-export-addr", RAMFBStandaloneState, export_addr, 0xe7e00000ULL),
    DEFINE_PROP_UINT32("x-export-buf-width", RAMFBStandaloneState, export_buf_width, 1088),
    DEFINE_PROP_UINT32("x-export-buf-height", RAMFBStandaloneState, export_buf_height, 2352),
    DEFINE_PROP_UINT32("x-export-width", RAMFBStandaloneState, export_width, 1080),
    DEFINE_PROP_UINT32("x-export-height", RAMFBStandaloneState, export_height, 2340),
    DEFINE_PROP_BOOL("x-export-ubwc", RAMFBStandaloneState, export_ubwc, true),
    DEFINE_PROP_BOOL("x-win32-display", RAMFBStandaloneState, win32_display, false),
    DEFINE_PROP_BOOL("x-export-inject-test", RAMFBStandaloneState, export_inject_test, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void ramfb_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->vmsd = &ramfb_dev_vmstate;
    dc->realize = ramfb_realizefn;
    dc->desc = "ram framebuffer standalone device";
    dc->user_creatable = true;
    device_class_set_props(dc, ramfb_properties);
}

static const TypeInfo ramfb_info = {
    .name          = TYPE_RAMFB_DEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RAMFBStandaloneState),
    .class_init    = ramfb_class_initfn,
};

static void ramfb_register_types(void)
{
    type_register_static(&ramfb_info);
}

type_init(ramfb_register_types)
