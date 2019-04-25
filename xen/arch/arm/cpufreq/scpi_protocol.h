/*
 * SCPI Message Protocol driver header
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Based on Linux include/linux/scpi_protocol.h
 * => commit 45ca7df7c345465dbd2426a33012c9c33d27de62
 *
 * Xen modification:
 * Oleksandr Tyshchenko <Oleksandr_Tyshchenko@epam.com>
 * Copyright (C) 2017 EPAM Systems Inc.
 */

#ifndef __ARCH_ARM_CPUFREQ_SCPI_PROTOCOL_H__
#define __ARCH_ARM_CPUFREQ_SCPI_PROTOCOL_H__

#if 0
#include <linux/types.h>
#endif

#include <asm/device.h>

#define IS_REACHABLE(CONFIG_ARM_SCPI_PROTOCOL) 1

#define CMD_ID_SHIFT		0
#define CMD_ID_MASK		0x7f
#define CMD_TOKEN_ID_SHIFT	8
#define CMD_TOKEN_ID_MASK	0xff
#define CMD_DATA_SIZE_SHIFT	16
#define CMD_DATA_SIZE_MASK	0x1ff
#define CMD_LEGACY_DATA_SIZE_SHIFT	20
#define CMD_LEGACY_DATA_SIZE_MASK	0x1ff
#define PACK_SCPI_CMD(cmd_id, tx_sz)			\
	((((cmd_id) & CMD_ID_MASK) << CMD_ID_SHIFT) |	\
	(((tx_sz) & CMD_DATA_SIZE_MASK) << CMD_DATA_SIZE_SHIFT))
#define ADD_SCPI_TOKEN(cmd, token)			\
	((cmd) |= (((token) & CMD_TOKEN_ID_MASK) << CMD_TOKEN_ID_SHIFT))
#define PACK_LEGACY_SCPI_CMD(cmd_id, tx_sz)				\
	((((cmd_id) & CMD_ID_MASK) << CMD_ID_SHIFT) |			       \
	(((tx_sz) & CMD_LEGACY_DATA_SIZE_MASK) << CMD_LEGACY_DATA_SIZE_SHIFT))

#define CMD_SIZE(cmd)	(((cmd) >> CMD_DATA_SIZE_SHIFT) & CMD_DATA_SIZE_MASK)
#define CMD_LEGACY_SIZE(cmd)	(((cmd) >> CMD_LEGACY_DATA_SIZE_SHIFT) & \
					CMD_LEGACY_DATA_SIZE_MASK)
#define CMD_UNIQ_MASK	(CMD_TOKEN_ID_MASK << CMD_TOKEN_ID_SHIFT | CMD_ID_MASK)
#define CMD_XTRACT_UNIQ(cmd)	((cmd) & CMD_UNIQ_MASK)
#define CMD_ID(cmd) ((cmd) & CMD_ID_MASK)

#define SCPI_SLOT		0

#define MAX_DVFS_DOMAINS	8
#define MAX_DVFS_OPPS		16
#define DVFS_LATENCY(hdr)	(le32_to_cpu(hdr) >> 16)
#define DVFS_OPP_COUNT(hdr)	((le32_to_cpu(hdr) >> 8) & 0xff)
#define DVFS_HEADER(pd, oppcnt, latency) (((pd) & 0xFF) | ((oppcnt) << 8) | \
										  ((latency) << 16))

#define PROTOCOL_REV_MINOR_BITS	16
#define PROTOCOL_REV_MINOR_MASK	((1U << PROTOCOL_REV_MINOR_BITS) - 1)
#define PROTOCOL_REV_MAJOR(x)	((x) >> PROTOCOL_REV_MINOR_BITS)
#define PROTOCOL_REV_MINOR(x)	((x) & PROTOCOL_REV_MINOR_MASK)

#define FW_REV_MAJOR_BITS	24
#define FW_REV_MINOR_BITS	16
#define FW_REV_PATCH_MASK	((1U << FW_REV_MINOR_BITS) - 1)
#define FW_REV_MINOR_MASK	((1U << FW_REV_MAJOR_BITS) - 1)
#define FW_REV_MAJOR(x)		((x) >> FW_REV_MAJOR_BITS)
#define FW_REV_MINOR(x)		(((x) & FW_REV_MINOR_MASK) >> FW_REV_MINOR_BITS)
#define FW_REV_PATCH(x)		((x) & FW_REV_PATCH_MASK)

enum scpi_error_codes {
	SCPI_SUCCESS = 0, /* Success */
	SCPI_ERR_PARAM = 1, /* Invalid parameter(s) */
	SCPI_ERR_ALIGN = 2, /* Invalid alignment */
	SCPI_ERR_SIZE = 3, /* Invalid size */
	SCPI_ERR_HANDLER = 4, /* Invalid handler/callback */
	SCPI_ERR_ACCESS = 5, /* Invalid access/permission denied */
	SCPI_ERR_RANGE = 6, /* Value out of range */
	SCPI_ERR_TIMEOUT = 7, /* Timeout has occurred */
	SCPI_ERR_NOMEM = 8, /* Invalid memory area or pointer */
	SCPI_ERR_PWRSTATE = 9, /* Invalid power state */
	SCPI_ERR_SUPPORT = 10, /* Not supported or disabled */
	SCPI_ERR_DEVICE = 11, /* Device error */
	SCPI_ERR_BUSY = 12, /* Device busy */
	SCPI_ERR_MAX
};

/* SCPI Standard commands */
enum scpi_std_cmd {
	SCPI_CMD_INVALID		= 0x00,
	SCPI_CMD_SCPI_READY		= 0x01,
	SCPI_CMD_SCPI_CAPABILITIES	= 0x02,
	SCPI_CMD_SET_CSS_PWR_STATE	= 0x03,
	SCPI_CMD_GET_CSS_PWR_STATE	= 0x04,
	SCPI_CMD_SET_SYS_PWR_STATE	= 0x05,
	SCPI_CMD_SET_CPU_TIMER		= 0x06,
	SCPI_CMD_CANCEL_CPU_TIMER	= 0x07,
	SCPI_CMD_DVFS_CAPABILITIES	= 0x08,
	SCPI_CMD_GET_DVFS_INFO		= 0x09,
	SCPI_CMD_SET_DVFS		= 0x0a,
	SCPI_CMD_GET_DVFS		= 0x0b,
	SCPI_CMD_GET_DVFS_STAT		= 0x0c,
	SCPI_CMD_CLOCK_CAPABILITIES	= 0x0d,
	SCPI_CMD_GET_CLOCK_INFO		= 0x0e,
	SCPI_CMD_SET_CLOCK_VALUE	= 0x0f,
	SCPI_CMD_GET_CLOCK_VALUE	= 0x10,
	SCPI_CMD_PSU_CAPABILITIES	= 0x11,
	SCPI_CMD_GET_PSU_INFO		= 0x12,
	SCPI_CMD_SET_PSU		= 0x13,
	SCPI_CMD_GET_PSU		= 0x14,
	SCPI_CMD_SENSOR_CAPABILITIES	= 0x15,
	SCPI_CMD_SENSOR_INFO		= 0x16,
	SCPI_CMD_SENSOR_VALUE		= 0x17,
	SCPI_CMD_SENSOR_CFG_PERIODIC	= 0x18,
	SCPI_CMD_SENSOR_CFG_BOUNDS	= 0x19,
	SCPI_CMD_SENSOR_ASYNC_VALUE	= 0x1a,
	SCPI_CMD_SET_DEVICE_PWR_STATE	= 0x1b,
	SCPI_CMD_GET_DEVICE_PWR_STATE	= 0x1c,
	SCPI_CMD_COUNT
};

/* SCPI Legacy Commands */
enum legacy_scpi_std_cmd {
	LEGACY_SCPI_CMD_INVALID			= 0x00,
	LEGACY_SCPI_CMD_SCPI_READY		= 0x01,
	LEGACY_SCPI_CMD_SCPI_CAPABILITIES	= 0x02,
	LEGACY_SCPI_CMD_EVENT			= 0x03,
	LEGACY_SCPI_CMD_SET_CSS_PWR_STATE	= 0x04,
	LEGACY_SCPI_CMD_GET_CSS_PWR_STATE	= 0x05,
	LEGACY_SCPI_CMD_CFG_PWR_STATE_STAT	= 0x06,
	LEGACY_SCPI_CMD_GET_PWR_STATE_STAT	= 0x07,
	LEGACY_SCPI_CMD_SYS_PWR_STATE		= 0x08,
	LEGACY_SCPI_CMD_L2_READY		= 0x09,
	LEGACY_SCPI_CMD_SET_AP_TIMER		= 0x0a,
	LEGACY_SCPI_CMD_CANCEL_AP_TIME		= 0x0b,
	LEGACY_SCPI_CMD_DVFS_CAPABILITIES	= 0x0c,
	LEGACY_SCPI_CMD_GET_DVFS_INFO		= 0x0d,
	LEGACY_SCPI_CMD_SET_DVFS		= 0x0e,
	LEGACY_SCPI_CMD_GET_DVFS		= 0x0f,
	LEGACY_SCPI_CMD_GET_DVFS_STAT		= 0x10,
	LEGACY_SCPI_CMD_SET_RTC			= 0x11,
	LEGACY_SCPI_CMD_GET_RTC			= 0x12,
	LEGACY_SCPI_CMD_CLOCK_CAPABILITIES	= 0x13,
	LEGACY_SCPI_CMD_SET_CLOCK_INDEX		= 0x14,
	LEGACY_SCPI_CMD_SET_CLOCK_VALUE		= 0x15,
	LEGACY_SCPI_CMD_GET_CLOCK_VALUE		= 0x16,
	LEGACY_SCPI_CMD_PSU_CAPABILITIES	= 0x17,
	LEGACY_SCPI_CMD_SET_PSU			= 0x18,
	LEGACY_SCPI_CMD_GET_PSU			= 0x19,
	LEGACY_SCPI_CMD_SENSOR_CAPABILITIES	= 0x1a,
	LEGACY_SCPI_CMD_SENSOR_INFO		= 0x1b,
	LEGACY_SCPI_CMD_SENSOR_VALUE		= 0x1c,
	LEGACY_SCPI_CMD_SENSOR_CFG_PERIODIC	= 0x1d,
	LEGACY_SCPI_CMD_SENSOR_CFG_BOUNDS	= 0x1e,
	LEGACY_SCPI_CMD_SENSOR_ASYNC_VALUE	= 0x1f,
	LEGACY_SCPI_CMD_COUNT
};

/* List all commands used by this driver, used as indexes */
enum scpi_drv_cmds {
	CMD_SCPI_CAPABILITIES = 0,
	CMD_GET_CLOCK_INFO,
	CMD_GET_CLOCK_VALUE,
	CMD_SET_CLOCK_VALUE,
	CMD_GET_DVFS,
	CMD_SET_DVFS,
	CMD_GET_DVFS_INFO,
	CMD_SENSOR_CAPABILITIES,
	CMD_SENSOR_INFO,
	CMD_SENSOR_VALUE,
	CMD_SET_DEVICE_PWR_STATE,
	CMD_GET_DEVICE_PWR_STATE,
	CMD_MAX_COUNT,
};

struct scpi_opp {
	u32 freq;
	u32 m_volt;
} __packed;

struct scpi_dvfs_info {
	unsigned int count;
	unsigned int latency; /* in nanoseconds */
	struct scpi_opp *opps;
};

enum scpi_sensor_class {
	TEMPERATURE,
	VOLTAGE,
	CURRENT,
	POWER,
	ENERGY,
};

struct scpi_sensor_info {
	u16 sensor_id;
	u8 class;
	u8 trigger_type;
	char name[20];
} __packed;

/*
 * The SCP firmware only executes in little-endian mode, so any buffers
 * shared through SCPI should have their contents converted to little-endian
 */
struct scpi_shared_mem {
	__le32 command;
	__le32 status;
	u8 payload[0];
} __packed;

struct legacy_scpi_shared_mem {
	__le32 status;
	u8 payload[0];
} __packed;

struct scp_capabilities {
	__le32 protocol_version;
	__le32 event_version;
	__le32 platform_version;
	__le32 commands[4];
} __packed;

struct clk_get_info {
	__le16 id;
	__le16 flags;
	__le32 min_rate;
	__le32 max_rate;
	u8 name[20];
} __packed;

struct clk_get_value {
	__le32 rate;
} __packed;

struct clk_set_value {
	__le16 id;
	__le16 reserved;
	__le32 rate;
} __packed;

struct legacy_clk_set_value {
	__le32 rate;
	__le16 id;
	__le16 reserved;
} __packed;

struct dvfs_capabilities {
	char nr_power_domains;
} __packed;

struct dvfs_info_req {
	char domain;
} __packed;

struct dvfs_info {
	__le32 header;
	struct {
		__le32 freq;
		__le32 m_volt;
	} opps[MAX_DVFS_OPPS];
} __packed;

struct dvfs_set {
	u8 domain;
	u8 index;
} __packed;

struct sensor_capabilities {
	__le16 sensors;
} __packed;

struct _scpi_sensor_info {
	__le16 sensor_id;
	u8 class;
	u8 trigger_type;
	char name[20];
};

struct sensor_value {
	__le32 lo_val;
	__le32 hi_val;
} __packed;

struct dev_pstate_set {
	__le16 dev_id;
	u8 pstate;
} __packed;

/**
 * struct scpi_ops - represents the various operations provided
 *	by SCP through SCPI message protocol
 * @get_version: returns the major and minor revision on the SCPI
 *	message protocol
 * @clk_get_range: gets clock range limit(min - max in Hz)
 * @clk_get_val: gets clock value(in Hz)
 * @clk_set_val: sets the clock value, setting to 0 will disable the
 *	clock (if supported)
 * @dvfs_get_idx: gets the Operating Point of the given power domain.
 *	OPP is an index to the list return by @dvfs_get_info
 * @dvfs_set_idx: sets the Operating Point of the given power domain.
 *	OPP is an index to the list return by @dvfs_get_info
 * @dvfs_get_info: returns the DVFS capabilities of the given power
 *	domain. It includes the OPP list and the latency information
 */
struct scpi_ops {
	u32 (*get_version)(void);
	int (*clk_get_range)(u16, unsigned long *, unsigned long *);
	unsigned long (*clk_get_val)(u16);
	int (*clk_set_val)(u16, unsigned long);
	int (*dvfs_get_idx)(u8);
	int (*dvfs_set_idx)(u8, u8);
	struct scpi_dvfs_info *(*dvfs_get_info)(u8);
	int (*device_domain_id)(struct device *);
	int (*get_transition_latency)(struct device *);
	int (*add_opps_to_device)(struct device *);
	int (*sensor_get_capability)(u16 *sensors);
	int (*sensor_get_info)(u16 sensor_id, struct scpi_sensor_info *);
	int (*sensor_get_value)(u16, u64 *);
	int (*device_get_power_state)(u16);
	int (*device_set_power_state)(u16, u8);
};

#if IS_REACHABLE(CONFIG_ARM_SCPI_PROTOCOL)
int scpi_init(void);
struct device *get_scpi_dev(void);
struct scpi_ops *get_scpi_ops(void);
#else
static inline int scpi_init(void) { return -1; }
static inline struct device *get_scpi_dev(void) { return NULL; }
static inline struct scpi_ops *get_scpi_ops(void) { return NULL; }
#endif

#endif /* __ARCH_ARM_CPUFREQ_SCPI_PROTOCOL_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
