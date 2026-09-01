/*
 * Qualcomm Generic Interface (GENI) Serial Engine UART Emulation
 *
 * Implements minimal GENI UART for earlycon and ttyMSM console.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qemu/log.h"
#include "qom/object.h"

#define TYPE_QCOM_GENI_SERIAL "qcom-geni-serial"
OBJECT_DECLARE_SIMPLE_TYPE(QcomGeniSerialState, QCOM_GENI_SERIAL)

#define GENI_STATUS             0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_SER_S_CLK_CFG      0x04c
#define GENI_FW_REVISION_RO     0x068
#define GENI_S_FW_REVISION_RO   0x06c
#define GENI_M_CMD0             0x600
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_TX_FIFOn           0x700
#define GENI_TX_FIFO_STATUS     0x800
#define GENI_RX_FIFO_STATUS     0x804
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28

#define M_CMD_DONE_EN           (1U << 0)
#define M_TX_FIFO_WATERMARK_EN  (1U << 30)

struct QcomGeniSerialState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    CharBackend chr;

    uint32_t geni_status;
    uint32_t m_cmd0;
    uint32_t m_irq_status;
    uint32_t m_irq_en;
    uint32_t tx_word_count;
    uint32_t tx_bytes_remaining;
};

static void geni_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    if (!f) {
        f = fopen("C:\\qemu_work\\geni.log", "w");
    }
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

static uint64_t qcom_geni_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomGeniSerialState *s = opaque;
    uint64_t ret = 0;

    switch (offset) {
    case 0x00: /* QUP_HW_VER */
        ret = 0x02000000; /* QUP major 2, minor 0 */
        break;
    case GENI_STATUS:
        ret = 0; /* Not busy / idle */
        break;
    case GENI_FW_REVISION_RO:
    case GENI_S_FW_REVISION_RO:
        ret = 0x00010100; /* Protocol UART (0x1), Version 1.0 */
        break;
    case GENI_M_CMD0:
        ret = s->m_cmd0;
        break;
    case GENI_M_IRQ_STATUS:
        /* Always report CMD_DONE and FIFO space ready */
        ret = M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN;
        break;
    case GENI_M_IRQ_EN:
        ret = s->m_irq_en;
        break;
    case GENI_TX_FIFO_STATUS:
        ret = 0x40; /* 64 words available in TX FIFO */
        break;
    case GENI_RX_FIFO_STATUS:
        ret = 0; /* RX FIFO empty */
        break;
    case SE_HW_PARAM_0:
        ret = 0x00400040; /* TX FIFO depth = 64, RX FIFO depth = 64 */
        break;
    case SE_HW_PARAM_1:
        ret = 0x00400040;
        break;
    case GENI_SER_M_CLK_CFG:
    case GENI_SER_S_CLK_CFG:
        ret = 0x11;
        break;
    default:
        ret = 0;
        break;
    }
    geni_log("[GENI_RD] off=0x%" PRIx64 ", size=%u -> val=0x%" PRIx64 "\n", offset, size, ret);
    return ret;
}

static void qcom_geni_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomGeniSerialState *s = opaque;
    geni_log("[GENI_WR] off=0x%" PRIx64 ", size=%u, val=0x%" PRIx64 "\n", offset, size, val);

    switch (offset) {
    case GENI_M_CMD0:
        s->m_cmd0 = val;
        /* Start TX command opcode is 1; val format: (opcode << 27) | (bytes & 0xFFFFFF) */
        if ((val >> 27) == 1) {
            s->tx_bytes_remaining = val & 0xFFFFFF;
            if (s->tx_bytes_remaining == 0) {
                s->tx_bytes_remaining = 4;
            }
        }
        break;
    case GENI_M_IRQ_CLEAR:
        s->m_irq_status &= ~val;
        break;
    case GENI_M_IRQ_EN:
        s->m_irq_en = val;
        break;
    case GENI_TX_FIFOn ... (GENI_TX_FIFOn + 0x7c):
    {
        uint32_t word = (uint32_t)val;
        uint8_t bytes[4];
        bytes[0] = word & 0xFF;
        bytes[1] = (word >> 8) & 0xFF;
        bytes[2] = (word >> 16) & 0xFF;
        bytes[3] = (word >> 24) & 0xFF;

        int to_write = 4;
        if (s->tx_bytes_remaining > 0 && s->tx_bytes_remaining < 4) {
            to_write = s->tx_bytes_remaining;
            s->tx_bytes_remaining = 0;
        } else if (s->tx_bytes_remaining >= 4) {
            s->tx_bytes_remaining -= 4;
        }

        for (int i = 0; i < to_write; i++) {
            if (bytes[i] != 0) {
                if (qemu_chr_fe_backend_connected(&s->chr)) {
                    qemu_chr_fe_write(&s->chr, &bytes[i], 1);
                } else {
                    putchar(bytes[i]);
                    fflush(stdout);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps qcom_geni_ops = {
    .read = qcom_geni_read,
    .write = qcom_geni_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void qcom_geni_serial_init(Object *obj)
{
    QcomGeniSerialState *s = QCOM_GENI_SERIAL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &qcom_geni_ops, s,
                          "qcom-geni-serial", 0x4000);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static Property qcom_geni_serial_properties[] = {
    DEFINE_PROP_CHR("chardev", QcomGeniSerialState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void qcom_geni_serial_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, qcom_geni_serial_properties);
}

static const TypeInfo qcom_geni_serial_info = {
    .name = TYPE_QCOM_GENI_SERIAL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomGeniSerialState),
    .instance_init = qcom_geni_serial_init,
    .class_init = qcom_geni_serial_class_init,
};

static void qcom_geni_serial_register_types(void)
{
    type_register_static(&qcom_geni_serial_info);
}

type_init(qcom_geni_serial_register_types)
